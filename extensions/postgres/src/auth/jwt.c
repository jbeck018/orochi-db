/*-------------------------------------------------------------------------
 *
 * jwt.c
 *    JWT (JSON Web Token) validation implementation for Orochi DB
 *
 * This module implements JWT parsing, validation, and signature verification
 * using OpenSSL for cryptographic operations. It integrates with PostgreSQL
 * session variables to enable Row Level Security (RLS) policies.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "jwt.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <string.h>
#include <time.h>

/* PostgreSQL module magic */
PG_MODULE_MAGIC;

/* SQL function declarations */
PG_FUNCTION_INFO_V1(orochi_jwt_verify);
PG_FUNCTION_INFO_V1(orochi_jwt_decode_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_get_claim);
PG_FUNCTION_INFO_V1(orochi_jwt_set_auth_context);
PG_FUNCTION_INFO_V1(orochi_jwt_clear_auth_context);
PG_FUNCTION_INFO_V1(orochi_jwt_encode);

/* ============================================================
 * Constants
 * ============================================================ */

/* Unix epoch offset from PostgreSQL epoch */
#define UNIX_EPOCH_JDATE     2440588 /* Julian date of Unix epoch */
#define POSTGRES_EPOCH_JDATE 2451545 /* Julian date of Postgres epoch */
#define SECS_PER_DAY         86400
#define USECS_PER_SEC        1000000LL

/* Maximum token size (256KB) */
#define JWT_MAX_TOKEN_SIZE (256 * 1024)

/* Session variable prefix */
#define JWT_SESSION_PREFIX "request.jwt."
#define JWT_CLAIM_PREFIX   "request.jwt.claim."

/* ============================================================
 * Base64URL Encoding/Decoding
 * ============================================================ */

/*
 * Base64URL character set (RFC 4648 Section 5)
 */
static const char base64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/*
 * Decode table for Base64URL
 */
static const int8 base64url_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
    7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/*
 * orochi_base64url_decode
 *    Decode a Base64URL encoded string
 *
 * JWT uses Base64URL encoding which differs from standard Base64:
 *   - '+' is replaced with '-'
 *   - '/' is replaced with '_'
 *   - Padding '=' is optional
 */
char *orochi_base64url_decode(const char *input, Size *output_len)
{
    Size input_len;
    Size padding;
    Size decoded_len;
    char *output;
    Size i, j;
    uint32 buffer;
    int bits;

    if (input == NULL || output_len == NULL)
        return NULL;

    input_len = strlen(input);
    if (input_len == 0) {
        *output_len = 0;
        return pstrdup("");
    }

    /* Calculate padding needed */
    padding = (4 - (input_len % 4)) % 4;

    /* Calculate output size */
    decoded_len = ((input_len + padding) * 3) / 4;
    output = palloc(decoded_len + 1);

    buffer = 0;
    bits = 0;
    j = 0;

    for (i = 0; i < input_len; i++) {
        int8 val = base64url_decode_table[(unsigned char)input[i]];

        if (val < 0) {
            /* Invalid character - skip or error */
            if (input[i] == '=' || input[i] == '\n' || input[i] == '\r')
                continue;

            pfree(output);
            *output_len = 0;
            return NULL;
        }

        buffer = (buffer << 6) | val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            output[j++] = (char)((buffer >> bits) & 0xFF);
        }
    }

    output[j] = '\0';
    *output_len = j;

    return output;
}

/*
 * orochi_base64url_encode
 *    Encode bytes to Base64URL string
 */
char *orochi_base64url_encode(const unsigned char *input, Size input_len)
{
    Size output_len;
    char *output;
    Size i, j;
    uint32 buffer;
    int bits;

    if (input == NULL || input_len == 0)
        return pstrdup("");

    /* Calculate output size (no padding) */
    output_len = ((input_len * 4) + 2) / 3;
    output = palloc(output_len + 1);

    buffer = 0;
    bits = 0;
    j = 0;

    for (i = 0; i < input_len; i++) {
        buffer = (buffer << 8) | input[i];
        bits += 8;

        while (bits >= 6) {
            bits -= 6;
            output[j++] = base64url_chars[(buffer >> bits) & 0x3F];
        }
    }

    /* Handle remaining bits */
    if (bits > 0) {
        buffer <<= (6 - bits);
        output[j++] = base64url_chars[buffer & 0x3F];
    }

    output[j] = '\0';

    return output;
}

/* ============================================================
 * Algorithm Conversion Functions
 * ============================================================ */

/*
 * orochi_jwt_algorithm_from_string
 *    Convert algorithm string to enum
 */
OrochiJwtAlgorithm orochi_jwt_algorithm_from_string(const char *alg_str)
{
    if (alg_str == NULL)
        return OROCHI_JWT_ALG_NONE;

    if (strcmp(alg_str, "HS256") == 0)
        return OROCHI_JWT_HS256;
    if (strcmp(alg_str, "HS384") == 0)
        return OROCHI_JWT_HS384;
    if (strcmp(alg_str, "HS512") == 0)
        return OROCHI_JWT_HS512;
    if (strcmp(alg_str, "RS256") == 0)
        return OROCHI_JWT_RS256;
    if (strcmp(alg_str, "RS384") == 0)
        return OROCHI_JWT_RS384;
    if (strcmp(alg_str, "RS512") == 0)
        return OROCHI_JWT_RS512;
    if (strcmp(alg_str, "ES256") == 0)
        return OROCHI_JWT_ES256;
    if (strcmp(alg_str, "ES384") == 0)
        return OROCHI_JWT_ES384;
    if (strcmp(alg_str, "ES512") == 0)
        return OROCHI_JWT_ES512;
    if (strcmp(alg_str, "none") == 0)
        return OROCHI_JWT_ALG_NONE;

    return OROCHI_JWT_ALG_NONE;
}

/*
 * orochi_jwt_algorithm_to_string
 *    Convert algorithm enum to string
 */
const char *orochi_jwt_algorithm_to_string(OrochiJwtAlgorithm alg)
{
    switch (alg) {
    case OROCHI_JWT_HS256:
        return "HS256";
    case OROCHI_JWT_HS384:
        return "HS384";
    case OROCHI_JWT_HS512:
        return "HS512";
    case OROCHI_JWT_RS256:
        return "RS256";
    case OROCHI_JWT_RS384:
        return "RS384";
    case OROCHI_JWT_RS512:
        return "RS512";
    case OROCHI_JWT_ES256:
        return "ES256";
    case OROCHI_JWT_ES384:
        return "ES384";
    case OROCHI_JWT_ES512:
        return "ES512";
    case OROCHI_JWT_ALG_NONE:
    default:
        return "none";
    }
}

/*
 * orochi_jwt_status_message
 *    Get human-readable error message
 */
const char *orochi_jwt_status_message(OrochiJwtStatus status)
{
    switch (status) {
    case OROCHI_JWT_STATUS_VALID:
        return "Token is valid";
    case OROCHI_JWT_STATUS_INVALID_FORMAT:
        return "Invalid token format";
    case OROCHI_JWT_STATUS_INVALID_HEADER:
        return "Invalid token header";
    case OROCHI_JWT_STATUS_INVALID_PAYLOAD:
        return "Invalid token payload";
    case OROCHI_JWT_STATUS_INVALID_SIGNATURE:
        return "Invalid signature";
    case OROCHI_JWT_STATUS_EXPIRED:
        return "Token has expired";
    case OROCHI_JWT_STATUS_NOT_YET_VALID:
        return "Token is not yet valid";
    case OROCHI_JWT_STATUS_UNSUPPORTED_ALGORITHM:
        return "Unsupported algorithm";
    case OROCHI_JWT_STATUS_DECODE_ERROR:
        return "Failed to decode token";
    case OROCHI_JWT_STATUS_VERIFICATION_ERROR:
        return "Signature verification failed";
    default:
        return "Unknown error";
    }
}

/* ============================================================
 * Timestamp Conversion Functions
 * ============================================================ */

/*
 * orochi_unix_to_timestamptz
 *    Convert Unix timestamp to PostgreSQL TimestampTz
 */
TimestampTz orochi_unix_to_timestamptz(int64 unix_timestamp)
{
    /* Convert Unix epoch to Postgres epoch */
    int64 pg_epoch_offset = (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;

    return (TimestampTz)((unix_timestamp - pg_epoch_offset) * USECS_PER_SEC);
}

/*
 * orochi_timestamptz_to_unix
 *    Convert PostgreSQL TimestampTz to Unix timestamp
 */
int64 orochi_timestamptz_to_unix(TimestampTz ts)
{
    int64 pg_epoch_offset = (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;

    return (ts / USECS_PER_SEC) + pg_epoch_offset;
}

/*
 * Get current time as Unix timestamp
 */
static int64 get_current_unix_timestamp(void)
{
    return orochi_timestamptz_to_unix(GetCurrentTimestamp());
}

/* ============================================================
 * JWT Header Parsing
 * ============================================================ */

/*
 * orochi_jwt_parse_header
 *    Parse JWT header from JSON string
 */
OrochiJwtHeader *orochi_jwt_parse_header(const char *json_str)
{
    OrochiJwtHeader *header;
    Jsonb *jsonb;
    JsonbValue *alg_val, *typ_val, *kid_val;

    if (json_str == NULL)
        return NULL;

    header = palloc0(sizeof(OrochiJwtHeader));
    header->raw_json = pstrdup(json_str);

    /* Parse JSON to JSONB */
    PG_TRY();
    {
        jsonb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json_str)));
    }
    PG_CATCH();
    {
        pfree(header->raw_json);
        pfree(header);
        return NULL;
    }
    PG_END_TRY();

    /* Extract algorithm */
    alg_val = getKeyJsonValueFromContainer(&jsonb->root, "alg", 3, NULL);
    if (alg_val != NULL && alg_val->type == jbvString) {
        char *alg_str = pnstrdup(alg_val->val.string.val, alg_val->val.string.len);
        header->alg = orochi_jwt_algorithm_from_string(alg_str);
        pfree(alg_str);
    } else {
        header->alg = OROCHI_JWT_ALG_NONE;
    }

    /* Extract type */
    typ_val = getKeyJsonValueFromContainer(&jsonb->root, "typ", 3, NULL);
    if (typ_val != NULL && typ_val->type == jbvString) {
        header->typ = pnstrdup(typ_val->val.string.val, typ_val->val.string.len);
    }

    /* Extract key ID (optional) */
    kid_val = getKeyJsonValueFromContainer(&jsonb->root, "kid", 3, NULL);
    if (kid_val != NULL && kid_val->type == jbvString) {
        header->kid = pnstrdup(kid_val->val.string.val, kid_val->val.string.len);
    }

    return header;
}

/* ============================================================
 * JWT Claims Parsing
 * ============================================================ */

/*
 * Extract string value from JSONB by key
 */
static char *extract_jsonb_string(Jsonb *jsonb, const char *key)
{
    JsonbValue *val;

    val = getKeyJsonValueFromContainer(&jsonb->root, key, strlen(key), NULL);
    if (val != NULL && val->type == jbvString)
        return pnstrdup(val->val.string.val, val->val.string.len);

    return NULL;
}

/*
 * Extract numeric value from JSONB by key
 */
static int64 extract_jsonb_int64(Jsonb *jsonb, const char *key, bool *found)
{
    JsonbValue *val;

    *found = false;
    val = getKeyJsonValueFromContainer(&jsonb->root, key, strlen(key), NULL);

    if (val != NULL && val->type == jbvNumeric) {
        *found = true;
        return DatumGetInt64(DirectFunctionCall1(numeric_int8, NumericGetDatum(val->val.numeric)));
    }

    return 0;
}

/*
 * Extract JSONB value by key
 */
static Jsonb *extract_jsonb_object(Jsonb *jsonb, const char *key)
{
    JsonbValue *val;
    JsonbValue jbv;

    val = getKeyJsonValueFromContainer(&jsonb->root, key, strlen(key), NULL);

    if (val != NULL &&
        (val->type == jbvObject || val->type == jbvArray || val->type == jbvBinary)) {
        /* Convert JsonbValue to Jsonb */
        if (val->type == jbvBinary) {
            return JsonbValueToJsonb(val);
        } else {
            jbv = *val;
            return JsonbValueToJsonb(&jbv);
        }
    }

    return NULL;
}

/*
 * orochi_jwt_parse_claims
 *    Parse JWT claims from JSON string
 */
OrochiJwtClaims *orochi_jwt_parse_claims(const char *json_str)
{
    OrochiJwtClaims *claims;
    Jsonb *jsonb;
    bool found;

    if (json_str == NULL)
        return NULL;

    claims = palloc0(sizeof(OrochiJwtClaims));

    /* Parse JSON to JSONB */
    PG_TRY();
    {
        jsonb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json_str)));
    }
    PG_CATCH();
    {
        pfree(claims);
        return NULL;
    }
    PG_END_TRY();

    claims->raw_payload = jsonb;

    /* Standard claims */
    claims->standard.iss = extract_jsonb_string(jsonb, "iss");
    claims->standard.sub = extract_jsonb_string(jsonb, "sub");
    claims->standard.aud = extract_jsonb_string(jsonb, "aud");
    claims->standard.jti = extract_jsonb_string(jsonb, "jti");

    claims->standard.exp_unix = extract_jsonb_int64(jsonb, "exp", &found);
    if (found)
        claims->standard.exp = orochi_unix_to_timestamptz(claims->standard.exp_unix);

    claims->standard.nbf_unix = extract_jsonb_int64(jsonb, "nbf", &found);
    if (found)
        claims->standard.nbf = orochi_unix_to_timestamptz(claims->standard.nbf_unix);

    claims->standard.iat_unix = extract_jsonb_int64(jsonb, "iat", &found);
    if (found)
        claims->standard.iat = orochi_unix_to_timestamptz(claims->standard.iat_unix);

    /* Custom/Supabase claims */
    claims->custom.email = extract_jsonb_string(jsonb, "email");
    claims->custom.phone = extract_jsonb_string(jsonb, "phone");
    claims->custom.role = extract_jsonb_string(jsonb, "role");
    claims->custom.session_id = extract_jsonb_string(jsonb, "session_id");
    claims->custom.aal = extract_jsonb_string(jsonb, "aal");

    claims->custom.app_metadata = extract_jsonb_object(jsonb, "app_metadata");
    claims->custom.user_metadata = extract_jsonb_object(jsonb, "user_metadata");
    claims->custom.amr = extract_jsonb_object(jsonb, "amr");

    return claims;
}

/* ============================================================
 * HMAC Signature Verification
 * ============================================================ */

/*
 * Get EVP_MD for algorithm
 */
static const EVP_MD *get_hmac_algorithm(OrochiJwtAlgorithm alg)
{
    switch (alg) {
    case OROCHI_JWT_HS256:
        return EVP_sha256();
    case OROCHI_JWT_HS384:
        return EVP_sha384();
    case OROCHI_JWT_HS512:
        return EVP_sha512();
    default:
        return NULL;
    }
}

/*
 * Verify HMAC signature
 */
static bool verify_hmac_signature(const char *data, Size data_len, const unsigned char *signature,
                                  Size sig_len, const char *secret, Size secret_len,
                                  OrochiJwtAlgorithm alg)
{
    const EVP_MD *md;
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;

    md = get_hmac_algorithm(alg);
    if (md == NULL)
        return false;

    /* Compute HMAC */
    if (HMAC(md, secret, (int)secret_len, (unsigned char *)data, data_len, hmac_result,
             &hmac_len) == NULL) {
        return false;
    }

    /* Constant-time comparison to prevent timing attacks */
    if (sig_len != hmac_len)
        return false;

    return CRYPTO_memcmp(hmac_result, signature, hmac_len) == 0;
}

/*
 * Get EVP_MD for RSA algorithm
 */
static const EVP_MD *get_rsa_digest_algorithm(OrochiJwtAlgorithm alg)
{
    switch (alg) {
    case OROCHI_JWT_RS256:
        return EVP_sha256();
    case OROCHI_JWT_RS384:
        return EVP_sha384();
    case OROCHI_JWT_RS512:
        return EVP_sha512();
    default:
        return NULL;
    }
}

/*
 * Get EVP_MD for ECDSA algorithm
 */
static const EVP_MD *get_ecdsa_digest_algorithm(OrochiJwtAlgorithm alg)
{
    switch (alg) {
    case OROCHI_JWT_ES256:
        return EVP_sha256();
    case OROCHI_JWT_ES384:
        return EVP_sha384();
    case OROCHI_JWT_ES512:
        return EVP_sha512();
    default:
        return NULL;
    }
}

/*
 * Verify RSA signature (RS256, RS384, RS512)
 *
 * The public_key_pem should be a PEM-encoded RSA public key
 * The signature is in PKCS#1 v1.5 format
 */
static bool verify_rsa_signature(const char *data, Size data_len, const unsigned char *signature,
                                 Size sig_len, const char *public_key_pem, OrochiJwtAlgorithm alg)
{
    const EVP_MD *md;
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    BIO *bio = NULL;
    bool result = false;
    int verify_result;

    md = get_rsa_digest_algorithm(alg);
    if (md == NULL) {
        elog(WARNING, "Unsupported RSA algorithm");
        return false;
    }

    /* Parse PEM public key */
    bio = BIO_new_mem_buf(public_key_pem, -1);
    if (bio == NULL) {
        elog(WARNING, "Failed to create BIO for public key");
        return false;
    }

    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);

    if (pkey == NULL) {
        /* Try reading as RSA public key format */
        bio = BIO_new_mem_buf(public_key_pem, -1);
        if (bio != NULL) {
            RSA *rsa = PEM_read_bio_RSAPublicKey(bio, NULL, NULL, NULL);
            BIO_free(bio);
            if (rsa != NULL) {
                pkey = EVP_PKEY_new();
                if (pkey != NULL)
                    EVP_PKEY_assign_RSA(pkey, rsa);
                else
                    RSA_free(rsa);
            }
        }
    }

    if (pkey == NULL) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        elog(WARNING, "Failed to parse RSA public key: %s", err_buf);
        return false;
    }

    /* Verify the key is RSA */
    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA) {
        elog(WARNING, "Key is not an RSA key");
        EVP_PKEY_free(pkey);
        return false;
    }

    /* Create verification context */
    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL) {
        elog(WARNING, "Failed to create EVP_MD_CTX");
        EVP_PKEY_free(pkey);
        return false;
    }

    /* Initialize verification */
    if (EVP_DigestVerifyInit(md_ctx, NULL, md, NULL, pkey) != 1) {
        elog(WARNING, "EVP_DigestVerifyInit failed");
        goto cleanup;
    }

    /* Add data to verification */
    if (EVP_DigestVerifyUpdate(md_ctx, data, data_len) != 1) {
        elog(WARNING, "EVP_DigestVerifyUpdate failed");
        goto cleanup;
    }

    /* Verify signature */
    verify_result = EVP_DigestVerifyFinal(md_ctx, signature, sig_len);
    if (verify_result == 1) {
        result = true;
    } else if (verify_result == 0) {
        /* Signature verification failed (signature doesn't match) */
        elog(DEBUG1, "RSA signature verification failed: signature mismatch");
    } else {
        /* Error during verification */
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        elog(WARNING, "RSA verification error: %s", err_buf);
    }

cleanup:
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return result;
}

/*
 * Convert JWT ECDSA signature (concatenated r||s) to DER format
 *
 * JWT uses a raw signature format where r and s are concatenated
 * OpenSSL expects DER-encoded ECDSA-Sig-Value
 */
static unsigned char *ecdsa_sig_to_der(const unsigned char *jwt_sig, Size jwt_sig_len,
                                       int coord_size, Size *der_len)
{
    ECDSA_SIG *sig = NULL;
    BIGNUM *r = NULL, *s = NULL;
    unsigned char *der_sig = NULL;
    int der_sig_len;

    /* JWT ECDSA signature is r||s, each coord_size bytes */
    if (jwt_sig_len != (Size)(coord_size * 2)) {
        elog(WARNING, "Invalid ECDSA signature length: %zu (expected %d)", jwt_sig_len,
             coord_size * 2);
        return NULL;
    }

    /* Extract r and s from concatenated format */
    r = BN_bin2bn(jwt_sig, coord_size, NULL);
    s = BN_bin2bn(jwt_sig + coord_size, coord_size, NULL);

    if (r == NULL || s == NULL) {
        elog(WARNING, "Failed to create BIGNUMs from signature");
        BN_free(r);
        BN_free(s);
        return NULL;
    }

    /* Create ECDSA_SIG and set r, s */
    sig = ECDSA_SIG_new();
    if (sig == NULL) {
        BN_free(r);
        BN_free(s);
        return NULL;
    }

    /* ECDSA_SIG_set0 takes ownership of r and s */
    if (ECDSA_SIG_set0(sig, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        return NULL;
    }

    /* Convert to DER format */
    der_sig_len = i2d_ECDSA_SIG(sig, NULL);
    if (der_sig_len <= 0) {
        ECDSA_SIG_free(sig);
        return NULL;
    }

    der_sig = palloc(der_sig_len);
    unsigned char *der_ptr = der_sig;
    der_sig_len = i2d_ECDSA_SIG(sig, &der_ptr);

    ECDSA_SIG_free(sig);

    if (der_sig_len <= 0) {
        pfree(der_sig);
        return NULL;
    }

    *der_len = (Size)der_sig_len;
    return der_sig;
}

/*
 * Verify ECDSA signature (ES256, ES384, ES512)
 *
 * The public_key_pem should be a PEM-encoded EC public key
 * The signature is in JWT format (r||s concatenated)
 */
static bool verify_ecdsa_signature(const char *data, Size data_len, const unsigned char *signature,
                                   Size sig_len, const char *public_key_pem, OrochiJwtAlgorithm alg)
{
    const EVP_MD *md;
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    BIO *bio = NULL;
    bool result = false;
    int verify_result;
    unsigned char *der_sig = NULL;
    Size der_sig_len;
    int coord_size;

    md = get_ecdsa_digest_algorithm(alg);
    if (md == NULL) {
        elog(WARNING, "Unsupported ECDSA algorithm");
        return false;
    }

    /* Determine coordinate size based on algorithm */
    switch (alg) {
    case OROCHI_JWT_ES256:
        coord_size = 32; /* P-256 uses 32-byte coordinates */
        break;
    case OROCHI_JWT_ES384:
        coord_size = 48; /* P-384 uses 48-byte coordinates */
        break;
    case OROCHI_JWT_ES512:
        coord_size = 66; /* P-521 uses 66-byte coordinates */
        break;
    default:
        return false;
    }

    /* Convert JWT signature format to DER */
    der_sig = ecdsa_sig_to_der(signature, sig_len, coord_size, &der_sig_len);
    if (der_sig == NULL) {
        elog(WARNING, "Failed to convert ECDSA signature to DER format");
        return false;
    }

    /* Parse PEM public key */
    bio = BIO_new_mem_buf(public_key_pem, -1);
    if (bio == NULL) {
        elog(WARNING, "Failed to create BIO for public key");
        pfree(der_sig);
        return false;
    }

    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);

    if (pkey == NULL) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        elog(WARNING, "Failed to parse EC public key: %s", err_buf);
        pfree(der_sig);
        return false;
    }

    /* Verify the key is EC */
    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_EC) {
        elog(WARNING, "Key is not an EC key");
        EVP_PKEY_free(pkey);
        pfree(der_sig);
        return false;
    }

    /* Create verification context */
    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL) {
        elog(WARNING, "Failed to create EVP_MD_CTX");
        EVP_PKEY_free(pkey);
        pfree(der_sig);
        return false;
    }

    /* Initialize verification */
    if (EVP_DigestVerifyInit(md_ctx, NULL, md, NULL, pkey) != 1) {
        elog(WARNING, "EVP_DigestVerifyInit failed");
        goto cleanup;
    }

    /* Add data to verification */
    if (EVP_DigestVerifyUpdate(md_ctx, data, data_len) != 1) {
        elog(WARNING, "EVP_DigestVerifyUpdate failed");
        goto cleanup;
    }

    /* Verify signature using DER-encoded signature */
    verify_result = EVP_DigestVerifyFinal(md_ctx, der_sig, der_sig_len);
    if (verify_result == 1) {
        result = true;
    } else if (verify_result == 0) {
        elog(DEBUG1, "ECDSA signature verification failed: signature mismatch");
    } else {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        elog(WARNING, "ECDSA verification error: %s", err_buf);
    }

cleanup:
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    pfree(der_sig);

    return result;
}

/*
 * orochi_jwt_verify_signature
 *    Verify JWT signature
 */
bool orochi_jwt_verify_signature(const char *header_payload, const char *signature_b64,
                                 const char *secret, OrochiJwtAlgorithm alg)
{
    char *signature_decoded;
    Size sig_len;
    bool result;

    if (header_payload == NULL || signature_b64 == NULL || secret == NULL)
        return false;

    /* Decode signature */
    signature_decoded = orochi_base64url_decode(signature_b64, &sig_len);
    if (signature_decoded == NULL)
        return false;

    /* Verify based on algorithm type */
    switch (alg) {
    case OROCHI_JWT_HS256:
    case OROCHI_JWT_HS384:
    case OROCHI_JWT_HS512:
        result = verify_hmac_signature(header_payload, strlen(header_payload),
                                       (unsigned char *)signature_decoded, sig_len, secret,
                                       strlen(secret), alg);
        break;

    case OROCHI_JWT_RS256:
    case OROCHI_JWT_RS384:
    case OROCHI_JWT_RS512:
        result = verify_rsa_signature(header_payload, strlen(header_payload),
                                      (unsigned char *)signature_decoded, sig_len, secret, alg);
        break;

    case OROCHI_JWT_ES256:
    case OROCHI_JWT_ES384:
    case OROCHI_JWT_ES512:
        result = verify_ecdsa_signature(header_payload, strlen(header_payload),
                                        (unsigned char *)signature_decoded, sig_len, secret, alg);
        break;

    case OROCHI_JWT_ALG_NONE:
        /* 'none' algorithm - signature should be empty */
        result = (sig_len == 0);
        break;

    default:
        result = false;
        break;
    }

    pfree(signature_decoded);
    return result;
}

/* ============================================================
 * JWT Parsing and Validation
 * ============================================================ */

/*
 * orochi_jwt_parse
 *    Parse a JWT token into components (no validation)
 */
OrochiJwt *orochi_jwt_parse(const char *token)
{
    OrochiJwt *jwt;
    char *token_copy;
    char *first_dot, *second_dot;
    char *header_json, *payload_json;
    Size header_len, payload_len;

    if (token == NULL)
        return NULL;

    /* Check token length */
    if (strlen(token) > JWT_MAX_TOKEN_SIZE) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("JWT token exceeds maximum size")));
    }

    jwt = palloc0(sizeof(OrochiJwt));
    jwt->status = OROCHI_JWT_STATUS_INVALID_FORMAT;
    jwt->is_valid = false;

    /* Make a copy we can modify */
    token_copy = pstrdup(token);

    /* Find the two dots separating header.payload.signature */
    first_dot = strchr(token_copy, '.');
    if (first_dot == NULL) {
        jwt->error_message = "Invalid token format: missing first separator";
        pfree(token_copy);
        return jwt;
    }

    second_dot = strchr(first_dot + 1, '.');
    if (second_dot == NULL) {
        jwt->error_message = "Invalid token format: missing second separator";
        pfree(token_copy);
        return jwt;
    }

    /* Extract the three parts */
    *first_dot = '\0';
    *second_dot = '\0';

    jwt->header_b64 = pstrdup(token_copy);
    jwt->payload_b64 = pstrdup(first_dot + 1);
    jwt->signature_b64 = pstrdup(second_dot + 1);

    pfree(token_copy);

    /* Decode header */
    header_json = orochi_base64url_decode(jwt->header_b64, &header_len);
    if (header_json == NULL) {
        jwt->status = OROCHI_JWT_STATUS_DECODE_ERROR;
        jwt->error_message = "Failed to decode header";
        return jwt;
    }

    jwt->header = orochi_jwt_parse_header(header_json);
    pfree(header_json);

    if (jwt->header == NULL) {
        jwt->status = OROCHI_JWT_STATUS_INVALID_HEADER;
        jwt->error_message = "Failed to parse header JSON";
        return jwt;
    }

    /* Decode payload */
    payload_json = orochi_base64url_decode(jwt->payload_b64, &payload_len);
    if (payload_json == NULL) {
        jwt->status = OROCHI_JWT_STATUS_DECODE_ERROR;
        jwt->error_message = "Failed to decode payload";
        return jwt;
    }

    jwt->claims = orochi_jwt_parse_claims(payload_json);
    pfree(payload_json);

    if (jwt->claims == NULL) {
        jwt->status = OROCHI_JWT_STATUS_INVALID_PAYLOAD;
        jwt->error_message = "Failed to parse payload JSON";
        return jwt;
    }

    /* Decode signature */
    jwt->signature = orochi_base64url_decode(jwt->signature_b64, &jwt->signature_len);

    jwt->status = OROCHI_JWT_STATUS_VALID;
    return jwt;
}

/*
 * orochi_jwt_is_expired
 *    Check if token is expired
 */
bool orochi_jwt_is_expired(OrochiJwt *jwt, int64 clock_skew_seconds)
{
    int64 now;
    int64 exp;

    if (jwt == NULL || jwt->claims == NULL)
        return true;

    if (jwt->claims->standard.exp_unix == 0)
        return false; /* No expiry set */

    now = get_current_unix_timestamp();
    exp = jwt->claims->standard.exp_unix + clock_skew_seconds;

    return now > exp;
}

/*
 * orochi_jwt_is_not_yet_valid
 *    Check if token is not yet valid (nbf)
 */
bool orochi_jwt_is_not_yet_valid(OrochiJwt *jwt, int64 clock_skew_seconds)
{
    int64 now;
    int64 nbf;

    if (jwt == NULL || jwt->claims == NULL)
        return true;

    if (jwt->claims->standard.nbf_unix == 0)
        return false; /* No nbf set */

    now = get_current_unix_timestamp();
    nbf = jwt->claims->standard.nbf_unix - clock_skew_seconds;

    return now < nbf;
}

/*
 * orochi_jwt_decode
 *    Decode and validate a JWT token
 */
OrochiJwt *orochi_jwt_decode(const char *token, const char *secret, OrochiJwtAlgorithm expected_alg)
{
    OrochiJwtValidationOptions options = OROCHI_JWT_DEFAULT_OPTIONS;

    return orochi_jwt_decode_with_options(token, secret, expected_alg, &options);
}

/*
 * orochi_jwt_decode_with_options
 *    Decode and validate a JWT token with custom options
 */
OrochiJwt *orochi_jwt_decode_with_options(const char *token, const char *secret,
                                          OrochiJwtAlgorithm expected_alg,
                                          OrochiJwtValidationOptions *options)
{
    OrochiJwt *jwt;
    char *header_payload;

    /* Parse the token first */
    jwt = orochi_jwt_parse(token);
    if (jwt == NULL)
        return NULL;

    if (jwt->status != OROCHI_JWT_STATUS_VALID)
        return jwt;

    /* Check algorithm */
    if (jwt->header->alg != expected_alg) {
        jwt->status = OROCHI_JWT_STATUS_UNSUPPORTED_ALGORITHM;
        jwt->error_message = "Algorithm mismatch";
        jwt->is_valid = false;
        return jwt;
    }

    /* Verify signature */
    if (options->verify_signature) {
        header_payload = psprintf("%s.%s", jwt->header_b64, jwt->payload_b64);

        if (!orochi_jwt_verify_signature(header_payload, jwt->signature_b64, secret,
                                         expected_alg)) {
            jwt->status = OROCHI_JWT_STATUS_INVALID_SIGNATURE;
            jwt->error_message = "Signature verification failed";
            jwt->is_valid = false;
            pfree(header_payload);
            return jwt;
        }

        pfree(header_payload);
    }

    /* Check expiration */
    if (options->verify_exp && orochi_jwt_is_expired(jwt, options->clock_skew_seconds)) {
        jwt->status = OROCHI_JWT_STATUS_EXPIRED;
        jwt->error_message = "Token has expired";
        jwt->is_valid = false;
        return jwt;
    }

    /* Check not-before */
    if (options->verify_nbf && orochi_jwt_is_not_yet_valid(jwt, options->clock_skew_seconds)) {
        jwt->status = OROCHI_JWT_STATUS_NOT_YET_VALID;
        jwt->error_message = "Token is not yet valid";
        jwt->is_valid = false;
        return jwt;
    }

    /* Check issuer */
    if (options->required_iss != NULL) {
        if (jwt->claims->standard.iss == NULL ||
            strcmp(jwt->claims->standard.iss, options->required_iss) != 0) {
            jwt->status = OROCHI_JWT_STATUS_VERIFICATION_ERROR;
            jwt->error_message = "Invalid issuer";
            jwt->is_valid = false;
            return jwt;
        }
    }

    /* Check audience */
    if (options->required_aud != NULL) {
        if (jwt->claims->standard.aud == NULL ||
            strcmp(jwt->claims->standard.aud, options->required_aud) != 0) {
            jwt->status = OROCHI_JWT_STATUS_VERIFICATION_ERROR;
            jwt->error_message = "Invalid audience";
            jwt->is_valid = false;
            return jwt;
        }
    }

    jwt->status = OROCHI_JWT_STATUS_VALID;
    jwt->is_valid = true;
    jwt->error_message = NULL;

    return jwt;
}

/*
 * orochi_jwt_free
 *    Free JWT structure and all allocated memory
 */
void orochi_jwt_free(OrochiJwt *jwt)
{
    if (jwt == NULL)
        return;

    if (jwt->header) {
        if (jwt->header->typ)
            pfree(jwt->header->typ);
        if (jwt->header->kid)
            pfree(jwt->header->kid);
        if (jwt->header->raw_json)
            pfree(jwt->header->raw_json);
        pfree(jwt->header);
    }

    if (jwt->claims) {
        if (jwt->claims->standard.iss)
            pfree(jwt->claims->standard.iss);
        if (jwt->claims->standard.sub)
            pfree(jwt->claims->standard.sub);
        if (jwt->claims->standard.aud)
            pfree(jwt->claims->standard.aud);
        if (jwt->claims->standard.jti)
            pfree(jwt->claims->standard.jti);

        if (jwt->claims->custom.email)
            pfree(jwt->claims->custom.email);
        if (jwt->claims->custom.phone)
            pfree(jwt->claims->custom.phone);
        if (jwt->claims->custom.role)
            pfree(jwt->claims->custom.role);
        if (jwt->claims->custom.session_id)
            pfree(jwt->claims->custom.session_id);
        if (jwt->claims->custom.aal)
            pfree(jwt->claims->custom.aal);

        pfree(jwt->claims);
    }

    if (jwt->signature)
        pfree(jwt->signature);
    if (jwt->header_b64)
        pfree(jwt->header_b64);
    if (jwt->payload_b64)
        pfree(jwt->payload_b64);
    if (jwt->signature_b64)
        pfree(jwt->signature_b64);

    pfree(jwt);
}

/* ============================================================
 * Claim Access Functions
 * ============================================================ */

/*
 * orochi_jwt_get_claim_text
 *    Get a specific claim value as text
 */
char *orochi_jwt_get_claim_text(OrochiJwt *jwt, const char *claim_name)
{
    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL)
        return NULL;

    return extract_jsonb_string(jwt->claims->raw_payload, claim_name);
}

/*
 * orochi_jwt_get_claim_jsonb
 *    Get a specific claim value as JSONB
 */
Jsonb *orochi_jwt_get_claim_jsonb(OrochiJwt *jwt, const char *claim_name)
{
    JsonbValue *val;

    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL)
        return NULL;

    val = getKeyJsonValueFromContainer(&jwt->claims->raw_payload->root, claim_name,
                                       strlen(claim_name), NULL);
    if (val == NULL)
        return NULL;

    return JsonbValueToJsonb(val);
}

/*
 * orochi_jwt_has_claim
 *    Check if a claim exists
 */
bool orochi_jwt_has_claim(OrochiJwt *jwt, const char *claim_name)
{
    JsonbValue *val;

    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL)
        return false;

    val = getKeyJsonValueFromContainer(&jwt->claims->raw_payload->root, claim_name,
                                       strlen(claim_name), NULL);
    return val != NULL;
}

/*
 * orochi_jwt_get_claim_int
 *    Get claim as integer
 */
int64 orochi_jwt_get_claim_int(OrochiJwt *jwt, const char *claim_name, bool *is_null)
{
    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL) {
        *is_null = true;
        return 0;
    }

    return extract_jsonb_int64(jwt->claims->raw_payload, claim_name, is_null);
}

/*
 * orochi_jwt_get_claim_bool
 *    Get claim as boolean
 */
bool orochi_jwt_get_claim_bool(OrochiJwt *jwt, const char *claim_name, bool *is_null)
{
    JsonbValue *val;

    *is_null = true;

    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL)
        return false;

    val = getKeyJsonValueFromContainer(&jwt->claims->raw_payload->root, claim_name,
                                       strlen(claim_name), NULL);
    if (val != NULL && val->type == jbvBool) {
        *is_null = false;
        return val->val.boolean;
    }

    return false;
}

/* ============================================================
 * PostgreSQL Session Integration
 * ============================================================ */

/*
 * Set a session variable
 */
static void set_session_variable(const char *name, const char *value)
{
    if (value != NULL)
        SetConfigOption(name, value, PGC_USERSET, PGC_S_SESSION);
    else
        SetConfigOption(name, "", PGC_USERSET, PGC_S_SESSION);
}

/*
 * orochi_jwt_set_session_context
 *    Set JWT claims in PostgreSQL session variables
 */
void orochi_jwt_set_session_context(OrochiJwt *jwt)
{
    StringInfoData claims_json;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;

    if (jwt == NULL || jwt->claims == NULL || jwt->claims->raw_payload == NULL)
        return;

    /* Set full claims as JSON */
    initStringInfo(&claims_json);
    (void)JsonbToCString(&claims_json, &jwt->claims->raw_payload->root,
                         VARSIZE(jwt->claims->raw_payload));
    set_session_variable("request.jwt.claims", claims_json.data);
    pfree(claims_json.data);

    /* Set standard claims */
    set_session_variable(JWT_CLAIM_PREFIX "sub", jwt->claims->standard.sub);
    set_session_variable(JWT_CLAIM_PREFIX "iss", jwt->claims->standard.iss);
    set_session_variable(JWT_CLAIM_PREFIX "aud", jwt->claims->standard.aud);

    if (jwt->claims->standard.exp_unix != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)jwt->claims->standard.exp_unix);
        set_session_variable(JWT_CLAIM_PREFIX "exp", buf);
    }

    if (jwt->claims->standard.iat_unix != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)jwt->claims->standard.iat_unix);
        set_session_variable(JWT_CLAIM_PREFIX "iat", buf);
    }

    /* Set custom claims */
    set_session_variable(JWT_CLAIM_PREFIX "email", jwt->claims->custom.email);
    set_session_variable(JWT_CLAIM_PREFIX "phone", jwt->claims->custom.phone);
    set_session_variable(JWT_CLAIM_PREFIX "role",
                         jwt->claims->custom.role ? jwt->claims->custom.role : "authenticated");
    set_session_variable(JWT_CLAIM_PREFIX "session_id", jwt->claims->custom.session_id);
    set_session_variable(JWT_CLAIM_PREFIX "aal",
                         jwt->claims->custom.aal ? jwt->claims->custom.aal : "aal1");

    /* Set metadata as JSON strings */
    if (jwt->claims->custom.app_metadata) {
        StringInfoData md_json;
        initStringInfo(&md_json);
        (void)JsonbToCString(&md_json, &jwt->claims->custom.app_metadata->root,
                             VARSIZE(jwt->claims->custom.app_metadata));
        set_session_variable(JWT_CLAIM_PREFIX "app_metadata", md_json.data);
        pfree(md_json.data);
    }

    if (jwt->claims->custom.user_metadata) {
        StringInfoData md_json;
        initStringInfo(&md_json);
        (void)JsonbToCString(&md_json, &jwt->claims->custom.user_metadata->root,
                             VARSIZE(jwt->claims->custom.user_metadata));
        set_session_variable(JWT_CLAIM_PREFIX "user_metadata", md_json.data);
        pfree(md_json.data);
    }

    /* Iterate through all claims and set them as session variables */
    it = JsonbIteratorInit(&jwt->claims->raw_payload->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            char *setting_name = psprintf(JWT_CLAIM_PREFIX "%s", key);
            char *value = NULL;

            r = JsonbIteratorNext(&it, &v, false);
            if (r == WJB_VALUE) {
                switch (v.type) {
                case jbvString:
                    value = pnstrdup(v.val.string.val, v.val.string.len);
                    break;
                case jbvNumeric:
                    value = DatumGetCString(
                        DirectFunctionCall1(numeric_out, NumericGetDatum(v.val.numeric)));
                    break;
                case jbvBool:
                    value = v.val.boolean ? "true" : "false";
                    break;
                case jbvNull:
                    value = NULL;
                    break;
                default:
                    /* For complex types, skip individual setting */
                    break;
                }

                if (value != NULL) {
                    set_session_variable(setting_name, value);
                    if (v.type == jbvString || v.type == jbvNumeric)
                        pfree(value);
                }
            }

            pfree(key);
            pfree(setting_name);
        }
    }
}

/*
 * orochi_jwt_clear_session_context
 *    Clear JWT session variables
 */
void orochi_jwt_clear_session_context(void)
{
    set_session_variable("request.jwt.claims", "");
    set_session_variable(JWT_CLAIM_PREFIX "sub", "");
    set_session_variable(JWT_CLAIM_PREFIX "iss", "");
    set_session_variable(JWT_CLAIM_PREFIX "aud", "");
    set_session_variable(JWT_CLAIM_PREFIX "exp", "");
    set_session_variable(JWT_CLAIM_PREFIX "iat", "");
    set_session_variable(JWT_CLAIM_PREFIX "email", "");
    set_session_variable(JWT_CLAIM_PREFIX "phone", "");
    set_session_variable(JWT_CLAIM_PREFIX "role", "");
    set_session_variable(JWT_CLAIM_PREFIX "session_id", "");
    set_session_variable(JWT_CLAIM_PREFIX "aal", "");
    set_session_variable(JWT_CLAIM_PREFIX "app_metadata", "");
    set_session_variable(JWT_CLAIM_PREFIX "user_metadata", "");
}

/*
 * orochi_jwt_get_session_claim
 *    Get current session claim value
 */
char *orochi_jwt_get_session_claim(const char *claim_name)
{
    char *setting_name;
    const char *value;

    setting_name = psprintf(JWT_CLAIM_PREFIX "%s", claim_name);
    value = GetConfigOption(setting_name, true, false);
    pfree(setting_name);

    if (value == NULL || value[0] == '\0')
        return NULL;

    return pstrdup(value);
}

/* ============================================================
 * SQL Functions
 * ============================================================ */

/*
 * orochi_jwt_verify
 *    SQL function: Validate a JWT and return boolean
 *
 * Usage: SELECT orochi_jwt_verify(token, secret)
 */
Datum orochi_jwt_verify(PG_FUNCTION_ARGS)
{
    text *token_text;
    text *secret_text;
    char *token;
    char *secret;
    OrochiJwt *jwt;
    bool result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_BOOL(false);

    token_text = PG_GETARG_TEXT_PP(0);
    secret_text = PG_GETARG_TEXT_PP(1);

    token = text_to_cstring(token_text);
    secret = text_to_cstring(secret_text);

    jwt = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);
    result = (jwt != NULL && jwt->is_valid);

    if (jwt != NULL)
        orochi_jwt_free(jwt);

    pfree(token);
    pfree(secret);

    PG_RETURN_BOOL(result);
}

/*
 * orochi_jwt_decode_claims
 *    SQL function: Decode JWT and return claims as JSONB
 *
 * Usage: SELECT orochi_jwt_decode_claims(token, secret)
 */
Datum orochi_jwt_decode_claims(PG_FUNCTION_ARGS)
{
    text *token_text;
    text *secret_text;
    char *token;
    char *secret;
    OrochiJwt *jwt;
    Jsonb *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    token_text = PG_GETARG_TEXT_PP(0);
    secret_text = PG_GETARG_TEXT_PP(1);

    token = text_to_cstring(token_text);
    secret = text_to_cstring(secret_text);

    jwt = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);

    if (jwt == NULL || !jwt->is_valid) {
        const char *error_msg = jwt ? jwt->error_message : "Failed to parse token";
        if (jwt)
            orochi_jwt_free(jwt);
        pfree(token);
        pfree(secret);

        ereport(ERROR, (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                        errmsg("JWT validation failed: %s", error_msg)));
    }

    result = jwt->claims->raw_payload;

    /* Don't free jwt->claims->raw_payload as we're returning it */
    jwt->claims->raw_payload = NULL;
    orochi_jwt_free(jwt);
    pfree(token);
    pfree(secret);

    PG_RETURN_JSONB_P(result);
}

/*
 * orochi_jwt_get_claim
 *    SQL function: Get a specific claim from a JWT
 *
 * Usage: SELECT orochi_jwt_get_claim(token, claim_name)
 * Note: This doesn't verify the token, just extracts the claim
 */
Datum orochi_jwt_get_claim(PG_FUNCTION_ARGS)
{
    text *token_text;
    text *claim_name_text;
    char *token;
    char *claim_name;
    OrochiJwt *jwt;
    char *claim_value;
    text *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    token_text = PG_GETARG_TEXT_PP(0);
    claim_name_text = PG_GETARG_TEXT_PP(1);

    token = text_to_cstring(token_text);
    claim_name = text_to_cstring(claim_name_text);

    /* Parse without validation to just extract claims */
    jwt = orochi_jwt_parse(token);

    if (jwt == NULL || jwt->claims == NULL) {
        if (jwt)
            orochi_jwt_free(jwt);
        pfree(token);
        pfree(claim_name);
        PG_RETURN_NULL();
    }

    claim_value = orochi_jwt_get_claim_text(jwt, claim_name);

    if (claim_value == NULL) {
        orochi_jwt_free(jwt);
        pfree(token);
        pfree(claim_name);
        PG_RETURN_NULL();
    }

    result = cstring_to_text(claim_value);

    pfree(claim_value);
    orochi_jwt_free(jwt);
    pfree(token);
    pfree(claim_name);

    PG_RETURN_TEXT_P(result);
}

/*
 * orochi_jwt_set_auth_context
 *    SQL function: Set auth context from JWT for RLS
 *
 * Usage: SELECT orochi_jwt_set_auth_context(token, secret)
 */
Datum orochi_jwt_set_auth_context(PG_FUNCTION_ARGS)
{
    text *token_text;
    text *secret_text;
    char *token;
    char *secret;
    OrochiJwt *jwt;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_BOOL(false);

    token_text = PG_GETARG_TEXT_PP(0);
    secret_text = PG_GETARG_TEXT_PP(1);

    token = text_to_cstring(token_text);
    secret = text_to_cstring(secret_text);

    jwt = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);

    if (jwt == NULL || !jwt->is_valid) {
        const char *error_msg = jwt ? jwt->error_message : "Failed to parse token";
        if (jwt)
            orochi_jwt_free(jwt);
        pfree(token);
        pfree(secret);

        ereport(ERROR, (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                        errmsg("JWT validation failed: %s", error_msg)));
    }

    /* Set session variables from JWT claims */
    orochi_jwt_set_session_context(jwt);

    orochi_jwt_free(jwt);
    pfree(token);
    pfree(secret);

    PG_RETURN_BOOL(true);
}

/*
 * orochi_jwt_clear_auth_context
 *    SQL function: Clear auth context
 *
 * Usage: SELECT orochi_jwt_clear_auth_context()
 */
Datum orochi_jwt_clear_auth_context(PG_FUNCTION_ARGS)
{
    orochi_jwt_clear_session_context();
    PG_RETURN_VOID();
}

/*
 * orochi_jwt_encode
 *    SQL function: Encode claims to JWT
 *
 * Usage: SELECT orochi_jwt_encode(claims::jsonb, secret, algorithm)
 */
Datum orochi_jwt_encode(PG_FUNCTION_ARGS)
{
    Jsonb *claims_jsonb;
    text *secret_text;
    text *alg_text;
    char *secret;
    char *alg_str;
    OrochiJwtAlgorithm alg;
    StringInfoData header_json;
    StringInfoData payload_json;
    char *header_b64;
    char *payload_b64;
    char *header_payload;
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;
    const EVP_MD *md;
    char *signature_b64;
    char *jwt_token;
    text *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    claims_jsonb = PG_GETARG_JSONB_P(0);
    secret_text = PG_GETARG_TEXT_PP(1);

    /* Get algorithm (default to HS256) */
    if (PG_ARGISNULL(2))
        alg = OROCHI_JWT_HS256;
    else {
        alg_text = PG_GETARG_TEXT_PP(2);
        alg_str = text_to_cstring(alg_text);
        alg = orochi_jwt_algorithm_from_string(alg_str);
        pfree(alg_str);
    }

    secret = text_to_cstring(secret_text);

    /* Build header JSON */
    initStringInfo(&header_json);
    appendStringInfo(&header_json, "{\"alg\":\"%s\",\"typ\":\"JWT\"}",
                     orochi_jwt_algorithm_to_string(alg));

    /* Build payload JSON */
    initStringInfo(&payload_json);
    (void)JsonbToCString(&payload_json, &claims_jsonb->root, VARSIZE(claims_jsonb));

    /* Base64URL encode header and payload */
    header_b64 = orochi_base64url_encode((unsigned char *)header_json.data, header_json.len);
    payload_b64 = orochi_base64url_encode((unsigned char *)payload_json.data, payload_json.len);

    /* Create header.payload for signing */
    header_payload = psprintf("%s.%s", header_b64, payload_b64);

    /* Compute signature */
    md = get_hmac_algorithm(alg);
    if (md == NULL) {
        pfree(header_json.data);
        pfree(payload_json.data);
        pfree(header_b64);
        pfree(payload_b64);
        pfree(header_payload);
        pfree(secret);

        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("Unsupported JWT algorithm: %s", orochi_jwt_algorithm_to_string(alg))));
    }

    if (HMAC(md, secret, strlen(secret), (unsigned char *)header_payload, strlen(header_payload),
             hmac_result, &hmac_len) == NULL) {
        pfree(header_json.data);
        pfree(payload_json.data);
        pfree(header_b64);
        pfree(payload_b64);
        pfree(header_payload);
        pfree(secret);

        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Failed to compute HMAC signature")));
    }

    /* Base64URL encode signature */
    signature_b64 = orochi_base64url_encode(hmac_result, hmac_len);

    /* Build final JWT */
    jwt_token = psprintf("%s.%s", header_payload, signature_b64);

    result = cstring_to_text(jwt_token);

    pfree(header_json.data);
    pfree(payload_json.data);
    pfree(header_b64);
    pfree(payload_b64);
    pfree(header_payload);
    pfree(signature_b64);
    pfree(jwt_token);
    pfree(secret);

    PG_RETURN_TEXT_P(result);
}
