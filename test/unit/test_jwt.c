/*-------------------------------------------------------------------------
 *
 * test_jwt.c
 *    Unit tests for Orochi DB JWT (JSON Web Token) validation
 *
 * Tests covered:
 *   - JWT parsing and structure validation
 *   - Base64URL encoding/decoding
 *   - HMAC signature verification
 *   - Token expiration checks
 *   - Claim extraction
 *   - Token caching
 *   - Error handling
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#define _GNU_SOURCE  /* for strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants and Types
 * ============================================================ */

#define MAX_TOKEN_LEN       4096
#define MAX_CLAIMS          32
#define MAX_CACHED_TOKENS   100

/* JWT algorithms */
typedef enum MockJwtAlgorithm
{
    MOCK_JWT_HS256 = 0,
    MOCK_JWT_HS384,
    MOCK_JWT_HS512,
    MOCK_JWT_RS256,
    MOCK_JWT_ES256,
    MOCK_JWT_NONE
} MockJwtAlgorithm;

/* JWT validation result */
typedef enum MockJwtValidationResult
{
    MOCK_JWT_VALID = 0,
    MOCK_JWT_INVALID_FORMAT,
    MOCK_JWT_INVALID_SIGNATURE,
    MOCK_JWT_EXPIRED,
    MOCK_JWT_NOT_YET_VALID,
    MOCK_JWT_INVALID_ISSUER,
    MOCK_JWT_INVALID_AUDIENCE,
    MOCK_JWT_MALFORMED_HEADER,
    MOCK_JWT_MALFORMED_PAYLOAD,
    MOCK_JWT_UNSUPPORTED_ALGORITHM
} MockJwtValidationResult;

/* JWT claim */
typedef struct MockJwtClaim
{
    char    name[64];
    char    value[512];
    bool    is_numeric;
    int64   numeric_value;
    bool    in_use;
} MockJwtClaim;

/* Parsed JWT */
typedef struct MockJwt
{
    /* Header */
    MockJwtAlgorithm    algorithm;
    char                type[16];
    char                key_id[64];

    /* Standard claims */
    char                issuer[256];        /* iss */
    char                subject[256];       /* sub */
    char                audience[256];      /* aud */
    int64               expiration;         /* exp */
    int64               not_before;         /* nbf */
    int64               issued_at;          /* iat */
    char                jwt_id[128];        /* jti */

    /* Custom claims */
    MockJwtClaim        claims[MAX_CLAIMS];
    int                 claim_count;

    /* Raw parts */
    char                header_b64[1024];
    char                payload_b64[2048];
    char                signature_b64[512];

    bool                is_valid;
    MockJwtValidationResult validation_result;
} MockJwt;

/* Token cache entry */
typedef struct MockJwtCacheEntry
{
    char                token_hash[65];
    MockJwt             jwt;
    int64               cached_at;
    bool                in_use;
} MockJwtCacheEntry;

/* Global state */
static MockJwtCacheEntry jwt_cache[MAX_CACHED_TOKENS];
static int64 mock_current_time = 1704067200;  /* 2024-01-01 00:00:00 UTC */
static char mock_jwt_secret[256] = "super-secret-key-for-testing";

/* ============================================================
 * Mock Base64URL Functions
 * ============================================================ */

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Standard Base64 encoding */
static int mock_base64_encode(const unsigned char *input, int input_len, char *output)
{
    int i, j;
    unsigned char buf[3];
    int out_len = 0;

    for (i = 0, j = 0; i < input_len; i += 3)
    {
        buf[0] = input[i];
        buf[1] = (i + 1 < input_len) ? input[i + 1] : 0;
        buf[2] = (i + 2 < input_len) ? input[i + 2] : 0;

        output[j++] = b64_chars[buf[0] >> 2];
        output[j++] = b64_chars[((buf[0] & 0x03) << 4) | (buf[1] >> 4)];
        output[j++] = (i + 1 < input_len) ? b64_chars[((buf[1] & 0x0F) << 2) | (buf[2] >> 6)] : '=';
        output[j++] = (i + 2 < input_len) ? b64_chars[buf[2] & 0x3F] : '=';
    }

    output[j] = '\0';
    out_len = j;
    return out_len;
}

/* Convert standard base64 to URL-safe base64 */
static void mock_base64_to_base64url(char *str)
{
    char *p = str;
    while (*p)
    {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }
        p++;
    }
}

/* Convert URL-safe base64 to standard base64 */
static int mock_base64url_to_base64(const char *input, char *output)
{
    int len = strlen(input);
    int i;

    for (i = 0; i < len; i++)
    {
        if (input[i] == '-')
            output[i] = '+';
        else if (input[i] == '_')
            output[i] = '/';
        else
            output[i] = input[i];
    }

    /* Add padding */
    int padding = (4 - (len % 4)) % 4;
    for (int j = 0; j < padding; j++)
        output[len + j] = '=';
    output[len + padding] = '\0';

    return len + padding;
}

/* Decode base64 */
static int mock_base64_decode(const char *input, unsigned char *output)
{
    int len = strlen(input);
    int i, j = 0;
    unsigned char buf[4];

    for (i = 0; i < len; i += 4)
    {
        for (int k = 0; k < 4; k++)
        {
            char c = input[i + k];
            if (c >= 'A' && c <= 'Z') buf[k] = c - 'A';
            else if (c >= 'a' && c <= 'z') buf[k] = c - 'a' + 26;
            else if (c >= '0' && c <= '9') buf[k] = c - '0' + 52;
            else if (c == '+') buf[k] = 62;
            else if (c == '/') buf[k] = 63;
            else if (c == '=') buf[k] = 0;
            else buf[k] = 0;
        }

        output[j++] = (buf[0] << 2) | (buf[1] >> 4);
        if (input[i + 2] != '=')
            output[j++] = (buf[1] << 4) | (buf[2] >> 2);
        if (input[i + 3] != '=')
            output[j++] = (buf[2] << 6) | buf[3];
    }

    return j;
}

/* Base64URL encode */
static int mock_base64url_encode(const unsigned char *input, int input_len, char *output)
{
    int len = mock_base64_encode(input, input_len, output);
    mock_base64_to_base64url(output);
    return strlen(output);
}

/* Base64URL decode */
static int mock_base64url_decode(const char *input, unsigned char *output)
{
    char temp[MAX_TOKEN_LEN];
    mock_base64url_to_base64(input, temp);
    return mock_base64_decode(temp, output);
}

/* ============================================================
 * Mock Signature Functions
 * ============================================================ */

/* Simple mock HMAC (not cryptographically secure) */
static void mock_hmac_sha256(const char *key, const char *data, unsigned char *output, int *output_len)
{
    unsigned int h = 0x6a09e667;
    const char *p;

    /* Mix key */
    p = key;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h = ((h << 5) | (h >> 27)) + 0x5be0cd19;
    }

    /* Mix data */
    p = data;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h = ((h << 5) | (h >> 27)) + 0x1f83d9ab;
    }

    /* Generate 32-byte output (mock SHA256) */
    for (int i = 0; i < 32; i++)
    {
        h = h * 1103515245 + 12345;
        output[i] = (h >> 16) & 0xFF;
    }
    *output_len = 32;
}

/* Verify HMAC signature */
static bool mock_verify_hmac(const char *data, const char *signature_b64, const char *secret)
{
    unsigned char computed[32];
    int computed_len;
    unsigned char expected[256];
    int expected_len;

    mock_hmac_sha256(secret, data, computed, &computed_len);
    expected_len = mock_base64url_decode(signature_b64, expected);

    if (expected_len != computed_len)
        return false;

    /* Constant-time comparison */
    int diff = 0;
    for (int i = 0; i < computed_len; i++)
        diff |= computed[i] ^ expected[i];

    return diff == 0;
}

/* ============================================================
 * Mock JWT Functions
 * ============================================================ */

static void mock_jwt_init(void)
{
    memset(jwt_cache, 0, sizeof(jwt_cache));
    mock_current_time = 1704067200;
}

static void mock_jwt_cleanup(void)
{
    memset(jwt_cache, 0, sizeof(jwt_cache));
}

/* Parse algorithm string */
static MockJwtAlgorithm mock_parse_algorithm(const char *alg_str)
{
    if (strcmp(alg_str, "HS256") == 0) return MOCK_JWT_HS256;
    if (strcmp(alg_str, "HS384") == 0) return MOCK_JWT_HS384;
    if (strcmp(alg_str, "HS512") == 0) return MOCK_JWT_HS512;
    if (strcmp(alg_str, "RS256") == 0) return MOCK_JWT_RS256;
    if (strcmp(alg_str, "ES256") == 0) return MOCK_JWT_ES256;
    if (strcmp(alg_str, "none") == 0) return MOCK_JWT_NONE;
    return MOCK_JWT_HS256;
}

/* Simple JSON value extractor (mock - not full JSON parser) */
static bool mock_json_get_string(const char *json, const char *key, char *value, int max_len)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start)
        return false;

    start += strlen(search_key);
    while (*start == ' ' || *start == '\t') start++;

    if (*start == '"')
    {
        start++;
        const char *end = strchr(start, '"');
        if (!end)
            return false;
        int len = Min((int)(end - start), max_len - 1);
        strncpy(value, start, len);
        value[len] = '\0';
        return true;
    }

    return false;
}

/* Extract numeric value from JSON */
static bool mock_json_get_number(const char *json, const char *key, int64 *value)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    const char *start = strstr(json, search_key);
    if (!start)
        return false;

    start += strlen(search_key);
    while (*start == ' ' || *start == '\t') start++;

    if (*start >= '0' && *start <= '9')
    {
        *value = strtoll(start, NULL, 10);
        return true;
    }

    return false;
}

/* Parse a JWT token */
static MockJwt *mock_jwt_parse(const char *token)
{
    MockJwt *jwt = calloc(1, sizeof(MockJwt));
    if (!jwt)
        return NULL;

    /* Split token into parts */
    char *token_copy = strdup(token);
    char *header_b64 = token_copy;
    char *payload_b64 = strchr(header_b64, '.');
    if (!payload_b64)
    {
        jwt->validation_result = MOCK_JWT_INVALID_FORMAT;
        free(token_copy);
        return jwt;
    }
    *payload_b64++ = '\0';

    char *signature_b64 = strchr(payload_b64, '.');
    if (!signature_b64)
    {
        jwt->validation_result = MOCK_JWT_INVALID_FORMAT;
        free(token_copy);
        return jwt;
    }
    *signature_b64++ = '\0';

    /* Store raw parts */
    strncpy(jwt->header_b64, header_b64, sizeof(jwt->header_b64) - 1);
    strncpy(jwt->payload_b64, payload_b64, sizeof(jwt->payload_b64) - 1);
    strncpy(jwt->signature_b64, signature_b64, sizeof(jwt->signature_b64) - 1);

    /* Decode header */
    unsigned char header_json[1024];
    int header_len = mock_base64url_decode(header_b64, header_json);
    header_json[header_len] = '\0';

    /* Parse header */
    char alg_str[32];
    if (mock_json_get_string((char *)header_json, "alg", alg_str, sizeof(alg_str)))
        jwt->algorithm = mock_parse_algorithm(alg_str);
    else
    {
        jwt->validation_result = MOCK_JWT_MALFORMED_HEADER;
        free(token_copy);
        return jwt;
    }

    mock_json_get_string((char *)header_json, "typ", jwt->type, sizeof(jwt->type));
    mock_json_get_string((char *)header_json, "kid", jwt->key_id, sizeof(jwt->key_id));

    /* Decode payload */
    unsigned char payload_json[2048];
    int payload_len = mock_base64url_decode(payload_b64, payload_json);
    payload_json[payload_len] = '\0';

    /* Parse standard claims */
    mock_json_get_string((char *)payload_json, "iss", jwt->issuer, sizeof(jwt->issuer));
    mock_json_get_string((char *)payload_json, "sub", jwt->subject, sizeof(jwt->subject));
    mock_json_get_string((char *)payload_json, "aud", jwt->audience, sizeof(jwt->audience));
    mock_json_get_string((char *)payload_json, "jti", jwt->jwt_id, sizeof(jwt->jwt_id));

    mock_json_get_number((char *)payload_json, "exp", &jwt->expiration);
    mock_json_get_number((char *)payload_json, "nbf", &jwt->not_before);
    mock_json_get_number((char *)payload_json, "iat", &jwt->issued_at);

    jwt->is_valid = true;
    jwt->validation_result = MOCK_JWT_VALID;

    free(token_copy);
    return jwt;
}

/* Validate JWT signature */
static bool mock_jwt_verify_signature(MockJwt *jwt, const char *secret)
{
    if (!jwt || !secret)
        return false;

    /* Only support HMAC algorithms in mock */
    if (jwt->algorithm != MOCK_JWT_HS256 &&
        jwt->algorithm != MOCK_JWT_HS384 &&
        jwt->algorithm != MOCK_JWT_HS512)
    {
        return false;
    }

    char data[MAX_TOKEN_LEN];
    snprintf(data, sizeof(data), "%s.%s", jwt->header_b64, jwt->payload_b64);

    return mock_verify_hmac(data, jwt->signature_b64, secret);
}

/* Validate JWT (full validation) */
static MockJwtValidationResult mock_jwt_validate(const char *token, const char *secret,
                                                   const char *expected_issuer,
                                                   const char *expected_audience)
{
    MockJwt *jwt = mock_jwt_parse(token);
    if (!jwt)
        return MOCK_JWT_INVALID_FORMAT;

    if (jwt->validation_result != MOCK_JWT_VALID)
    {
        MockJwtValidationResult result = jwt->validation_result;
        free(jwt);
        return result;
    }

    /* Verify signature */
    if (secret && !mock_jwt_verify_signature(jwt, secret))
    {
        free(jwt);
        return MOCK_JWT_INVALID_SIGNATURE;
    }

    /* Check expiration */
    if (jwt->expiration > 0 && mock_current_time > jwt->expiration)
    {
        free(jwt);
        return MOCK_JWT_EXPIRED;
    }

    /* Check not-before */
    if (jwt->not_before > 0 && mock_current_time < jwt->not_before)
    {
        free(jwt);
        return MOCK_JWT_NOT_YET_VALID;
    }

    /* Check issuer */
    if (expected_issuer && strlen(jwt->issuer) > 0 &&
        strcmp(jwt->issuer, expected_issuer) != 0)
    {
        free(jwt);
        return MOCK_JWT_INVALID_ISSUER;
    }

    /* Check audience */
    if (expected_audience && strlen(jwt->audience) > 0 &&
        strcmp(jwt->audience, expected_audience) != 0)
    {
        free(jwt);
        return MOCK_JWT_INVALID_AUDIENCE;
    }

    free(jwt);
    return MOCK_JWT_VALID;
}

/* Create a simple JWT for testing */
static char *mock_jwt_create(const char *subject, const char *role,
                              int64 exp, const char *secret)
{
    /* Header */
    const char *header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char header_b64[256];
    mock_base64url_encode((unsigned char *)header_json, strlen(header_json), header_b64);

    /* Payload */
    char payload_json[1024];
    snprintf(payload_json, sizeof(payload_json),
             "{\"sub\":\"%s\",\"role\":\"%s\",\"exp\":%lld,\"iat\":%lld}",
             subject, role, (long long)exp, (long long)mock_current_time);
    char payload_b64[1024];
    mock_base64url_encode((unsigned char *)payload_json, strlen(payload_json), payload_b64);

    /* Signature */
    char data[MAX_TOKEN_LEN];
    snprintf(data, sizeof(data), "%s.%s", header_b64, payload_b64);

    unsigned char signature[32];
    int sig_len;
    mock_hmac_sha256(secret, data, signature, &sig_len);

    char signature_b64[256];
    mock_base64url_encode(signature, sig_len, signature_b64);

    /* Combine */
    char *token = malloc(MAX_TOKEN_LEN);
    snprintf(token, MAX_TOKEN_LEN, "%s.%s.%s", header_b64, payload_b64, signature_b64);

    return token;
}

/* Cache a token */
static bool mock_jwt_cache_add(const char *token, MockJwt *jwt)
{
    /* Simple hash of token */
    unsigned int h = 0x811c9dc5;
    const char *p = token;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h *= 0x01000193;
    }

    for (int i = 0; i < MAX_CACHED_TOKENS; i++)
    {
        if (!jwt_cache[i].in_use)
        {
            snprintf(jwt_cache[i].token_hash, sizeof(jwt_cache[i].token_hash), "%08x", h);
            memcpy(&jwt_cache[i].jwt, jwt, sizeof(MockJwt));
            jwt_cache[i].cached_at = mock_current_time;
            jwt_cache[i].in_use = true;
            return true;
        }
    }
    return false;
}

/* Lookup cached token */
static MockJwt *mock_jwt_cache_get(const char *token)
{
    unsigned int h = 0x811c9dc5;
    const char *p = token;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h *= 0x01000193;
    }

    char hash_str[16];
    snprintf(hash_str, sizeof(hash_str), "%08x", h);

    for (int i = 0; i < MAX_CACHED_TOKENS; i++)
    {
        if (jwt_cache[i].in_use && strcmp(jwt_cache[i].token_hash, hash_str) == 0)
            return &jwt_cache[i].jwt;
    }
    return NULL;
}

/* ============================================================
 * Test Cases: Base64URL Encoding
 * ============================================================ */

static void test_base64url_encode(void)
{
    TEST_BEGIN("base64url_encode_basic")
        char output[256];
        mock_base64url_encode((unsigned char *)"hello", 5, output);
        TEST_ASSERT_STR_EQ("aGVsbG8", output);
    TEST_END();
}

static void test_base64url_decode(void)
{
    TEST_BEGIN("base64url_decode_basic")
        unsigned char output[256];
        int len = mock_base64url_decode("aGVsbG8", output);
        output[len] = '\0';
        TEST_ASSERT_EQ(5, len);
        TEST_ASSERT_STR_EQ("hello", (char *)output);
    TEST_END();
}

static void test_base64url_roundtrip(void)
{
    TEST_BEGIN("base64url_roundtrip")
        const char *original = "Test string with special chars: +/=";
        char encoded[256];
        unsigned char decoded[256];

        mock_base64url_encode((unsigned char *)original, strlen(original), encoded);
        int len = mock_base64url_decode(encoded, decoded);
        decoded[len] = '\0';

        TEST_ASSERT_STR_EQ(original, (char *)decoded);
    TEST_END();
}

static void test_base64url_urlsafe(void)
{
    TEST_BEGIN("base64url_no_padding_or_special_chars")
        char output[256];
        mock_base64url_encode((unsigned char *)"abc", 3, output);

        /* Should not contain + / or = */
        TEST_ASSERT_NULL(strchr(output, '+'));
        TEST_ASSERT_NULL(strchr(output, '/'));
        TEST_ASSERT_NULL(strchr(output, '='));
    TEST_END();
}

/* ============================================================
 * Test Cases: JWT Parsing
 * ============================================================ */

static void test_jwt_parse_valid(void)
{
    TEST_BEGIN("jwt_parse_valid_token")
        mock_jwt_init();

        char *token = mock_jwt_create("user123", "authenticated",
                                       mock_current_time + 3600, mock_jwt_secret);

        MockJwt *jwt = mock_jwt_parse(token);
        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_VALID, jwt->validation_result);
        TEST_ASSERT_EQ(MOCK_JWT_HS256, jwt->algorithm);
        TEST_ASSERT_STR_EQ("user123", jwt->subject);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_parse_missing_dots(void)
{
    TEST_BEGIN("jwt_parse_missing_separator")
        mock_jwt_init();

        MockJwt *jwt = mock_jwt_parse("invalidtokenwithoutdots");
        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_INVALID_FORMAT, jwt->validation_result);

        free(jwt);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_parse_single_dot(void)
{
    TEST_BEGIN("jwt_parse_single_dot")
        mock_jwt_init();

        MockJwt *jwt = mock_jwt_parse("header.payloadonly");
        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_INVALID_FORMAT, jwt->validation_result);

        free(jwt);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_parse_algorithm(void)
{
    TEST_BEGIN("jwt_parse_extracts_algorithm")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "admin",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_HS256, jwt->algorithm);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Signature Verification
 * ============================================================ */

static void test_jwt_signature_valid(void)
{
    TEST_BEGIN("jwt_signature_valid")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT(mock_jwt_verify_signature(jwt, mock_jwt_secret));

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_signature_invalid_secret(void)
{
    TEST_BEGIN("jwt_signature_wrong_secret")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        /* Verify with wrong secret */
        TEST_ASSERT_FALSE(mock_jwt_verify_signature(jwt, "wrong-secret"));

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_signature_tampered_payload(void)
{
    TEST_BEGIN("jwt_signature_tampered_payload")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);

        /* Tamper with the token by modifying payload */
        char *dot = strchr(token, '.');
        if (dot && *(dot + 1))
            *(dot + 1) = 'X';  /* Corrupt payload */

        MockJwt *jwt = mock_jwt_parse(token);

        /* Signature should no longer match */
        TEST_ASSERT_FALSE(mock_jwt_verify_signature(jwt, mock_jwt_secret));

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Token Expiration
 * ============================================================ */

static void test_jwt_not_expired(void)
{
    TEST_BEGIN("jwt_not_expired")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);

        MockJwtValidationResult result = mock_jwt_validate(token, mock_jwt_secret, NULL, NULL);
        TEST_ASSERT_EQ(MOCK_JWT_VALID, result);

        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_expired(void)
{
    TEST_BEGIN("jwt_expired")
        mock_jwt_init();

        /* Create token that expired 1 hour ago */
        char *token = mock_jwt_create("user", "role",
                                       mock_current_time - 3600, mock_jwt_secret);

        MockJwtValidationResult result = mock_jwt_validate(token, mock_jwt_secret, NULL, NULL);
        TEST_ASSERT_EQ(MOCK_JWT_EXPIRED, result);

        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_expires_exactly_now(void)
{
    TEST_BEGIN("jwt_expires_exactly_now")
        mock_jwt_init();

        /* Token expires exactly at current time */
        char *token = mock_jwt_create("user", "role",
                                       mock_current_time, mock_jwt_secret);

        MockJwtValidationResult result = mock_jwt_validate(token, mock_jwt_secret, NULL, NULL);
        /* Expired since current_time > exp is false when equal */
        TEST_ASSERT_EQ(MOCK_JWT_VALID, result);

        /* Advance time by 1 second */
        mock_current_time += 1;
        result = mock_jwt_validate(token, mock_jwt_secret, NULL, NULL);
        TEST_ASSERT_EQ(MOCK_JWT_EXPIRED, result);

        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Claim Extraction
 * ============================================================ */

static void test_jwt_extract_subject(void)
{
    TEST_BEGIN("jwt_extract_subject")
        mock_jwt_init();

        char *token = mock_jwt_create("unique-user-id-123", "authenticated",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_STR_EQ("unique-user-id-123", jwt->subject);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_extract_expiration(void)
{
    TEST_BEGIN("jwt_extract_expiration")
        mock_jwt_init();

        int64 exp_time = mock_current_time + 7200;
        char *token = mock_jwt_create("user", "role", exp_time, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(exp_time, jwt->expiration);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Token Cache
 * ============================================================ */

static void test_jwt_cache_add_and_get(void)
{
    TEST_BEGIN("jwt_cache_add_and_get")
        mock_jwt_init();

        char *token = mock_jwt_create("cached-user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT(mock_jwt_cache_add(token, jwt));

        MockJwt *cached = mock_jwt_cache_get(token);
        TEST_ASSERT_NOT_NULL(cached);
        TEST_ASSERT_STR_EQ(jwt->subject, cached->subject);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_cache_miss(void)
{
    TEST_BEGIN("jwt_cache_miss")
        mock_jwt_init();

        MockJwt *cached = mock_jwt_cache_get("nonexistent-token");
        TEST_ASSERT_NULL(cached);

        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_cache_multiple_tokens(void)
{
    TEST_BEGIN("jwt_cache_multiple_tokens")
        mock_jwt_init();

        char *token1 = mock_jwt_create("user1", "role1",
                                        mock_current_time + 3600, mock_jwt_secret);
        char *token2 = mock_jwt_create("user2", "role2",
                                        mock_current_time + 3600, mock_jwt_secret);

        MockJwt *jwt1 = mock_jwt_parse(token1);
        MockJwt *jwt2 = mock_jwt_parse(token2);

        TEST_ASSERT(mock_jwt_cache_add(token1, jwt1));
        TEST_ASSERT(mock_jwt_cache_add(token2, jwt2));

        MockJwt *cached1 = mock_jwt_cache_get(token1);
        MockJwt *cached2 = mock_jwt_cache_get(token2);

        TEST_ASSERT_NOT_NULL(cached1);
        TEST_ASSERT_NOT_NULL(cached2);
        TEST_ASSERT_STR_EQ("user1", cached1->subject);
        TEST_ASSERT_STR_EQ("user2", cached2->subject);

        free(jwt1);
        free(jwt2);
        free(token1);
        free(token2);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Full Validation
 * ============================================================ */

static void test_jwt_full_validation_valid(void)
{
    TEST_BEGIN("jwt_full_validation_valid")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);

        MockJwtValidationResult result = mock_jwt_validate(token, mock_jwt_secret, NULL, NULL);
        TEST_ASSERT_EQ(MOCK_JWT_VALID, result);

        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_validation_invalid_signature(void)
{
    TEST_BEGIN("jwt_validation_invalid_signature")
        mock_jwt_init();

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);

        MockJwtValidationResult result = mock_jwt_validate(token, "wrong-secret", NULL, NULL);
        TEST_ASSERT_EQ(MOCK_JWT_INVALID_SIGNATURE, result);

        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Edge Cases
 * ============================================================ */

static void test_jwt_empty_token(void)
{
    TEST_BEGIN("jwt_empty_token")
        mock_jwt_init();

        MockJwt *jwt = mock_jwt_parse("");
        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_INVALID_FORMAT, jwt->validation_result);

        free(jwt);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_null_parameters(void)
{
    TEST_BEGIN("jwt_null_parameters")
        mock_jwt_init();

        TEST_ASSERT_FALSE(mock_jwt_verify_signature(NULL, mock_jwt_secret));

        char *token = mock_jwt_create("user", "role",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);
        TEST_ASSERT_FALSE(mock_jwt_verify_signature(jwt, NULL));

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

static void test_jwt_long_subject(void)
{
    TEST_BEGIN("jwt_long_subject")
        mock_jwt_init();

        char long_subject[256];
        memset(long_subject, 'x', 200);
        long_subject[200] = '\0';

        char *token = mock_jwt_create(long_subject, "role",
                                       mock_current_time + 3600, mock_jwt_secret);
        MockJwt *jwt = mock_jwt_parse(token);

        TEST_ASSERT_NOT_NULL(jwt);
        TEST_ASSERT_EQ(MOCK_JWT_VALID, jwt->validation_result);
        TEST_ASSERT_GT(strlen(jwt->subject), 100);

        free(jwt);
        free(token);
        mock_jwt_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_jwt(void)
{
    /* Base64URL tests */
    test_base64url_encode();
    test_base64url_decode();
    test_base64url_roundtrip();
    test_base64url_urlsafe();

    /* JWT parsing tests */
    test_jwt_parse_valid();
    test_jwt_parse_missing_dots();
    test_jwt_parse_single_dot();
    test_jwt_parse_algorithm();

    /* Signature verification tests */
    test_jwt_signature_valid();
    test_jwt_signature_invalid_secret();
    test_jwt_signature_tampered_payload();

    /* Expiration tests */
    test_jwt_not_expired();
    test_jwt_expired();
    test_jwt_expires_exactly_now();

    /* Claim extraction tests */
    test_jwt_extract_subject();
    test_jwt_extract_expiration();

    /* Token cache tests */
    test_jwt_cache_add_and_get();
    test_jwt_cache_miss();
    test_jwt_cache_multiple_tokens();

    /* Full validation tests */
    test_jwt_full_validation_valid();
    test_jwt_validation_invalid_signature();

    /* Edge cases */
    test_jwt_empty_token();
    test_jwt_null_parameters();
    test_jwt_long_subject();
}
