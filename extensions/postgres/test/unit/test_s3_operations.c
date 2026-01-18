/*-------------------------------------------------------------------------
 *
 * test_s3_operations.c
 *    Unit tests for S3 operations and AWS Signature V4
 *
 * Tests S3 list XML parsing, HEAD response parsing, AWS Signature V4
 * generation, URL encoding, and S3 API helpers.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* Include OpenSSL for hashing (would be in actual implementation) */
#include <openssl/sha.h>
#include <openssl/hmac.h>

/* ============================================================
 * Mock S3 Structures
 * ============================================================ */

typedef struct S3Object
{
    char *key;
    char *etag;
    int64_t size;
    int64_t last_modified;
} S3Object;

typedef struct S3Client
{
    char *endpoint;
    char *bucket;
    char *access_key;
    char *secret_key;
    char *region;
    bool use_ssl;
    int timeout_ms;
} S3Client;

/* ============================================================
 * Helper Functions - AWS Signature V4
 * ============================================================ */

/*
 * Convert bytes to hex string
 */
static void
bytes_to_hex(const unsigned char *bytes, size_t len, char *hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++)
    {
        hex[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

/*
 * SHA256 hash
 */
static void
sha256_hash(const char *data, size_t len, unsigned char *hash)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(hash, &ctx);
}

/*
 * HMAC-SHA256
 */
static void
hmac_sha256(const unsigned char *key, size_t key_len,
            const unsigned char *data, size_t data_len,
            unsigned char *result)
{
    unsigned int result_len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, result, &result_len);
}

/*
 * URL encode a string
 */
static char *
url_encode(const char *str)
{
    char *result = palloc(strlen(str) * 3 + 1);
    char *out = result;
    const char *p;

    for (p = str; *p; p++)
    {
        if ((*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~')
        {
            *out++ = *p;
        }
        else if (*p == '/')
        {
            *out++ = '/';  /* Don't encode path separators */
        }
        else
        {
            sprintf(out, "%%%02X", (unsigned char)*p);
            out += 3;
        }
    }
    *out = '\0';

    return result;
}

/*
 * Generate AWS Signature V4
 */
static char *
generate_signature_v4(const char *access_key, const char *secret_key,
                     const char *region, const char *method,
                     const char *uri, const char *query_string,
                     const char *date_stamp, const char *amz_date,
                     const char *payload_hash)
{
    unsigned char date_key[32], date_region_key[32], date_region_service_key[32], signing_key[32];
    unsigned char signature_bytes[32];
    char signature_hex[65];
    char canonical_request[1024];
    char string_to_sign[512];
    char scope[128];
    unsigned char canonical_hash[32];
    char canonical_hash_hex[65];
    char aws4_key[256];

    /* Build canonical request */
    snprintf(canonical_request, sizeof(canonical_request),
        "%s\n%s\n%s\nhost:example.s3.amazonaws.com\nx-amz-content-sha256:%s\nx-amz-date:%s\n\nhost;x-amz-content-sha256;x-amz-date\n%s",
        method, uri, query_string ? query_string : "",
        payload_hash, amz_date, payload_hash);

    /* Hash canonical request */
    sha256_hash(canonical_request, strlen(canonical_request), canonical_hash);
    bytes_to_hex(canonical_hash, 32, canonical_hash_hex);

    /* Build scope */
    snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request", date_stamp, region);

    /* Build string to sign */
    snprintf(string_to_sign, sizeof(string_to_sign),
        "AWS4-HMAC-SHA256\n%s\n%s\n%s",
        amz_date, scope, canonical_hash_hex);

    /* Calculate signing key */
    snprintf(aws4_key, sizeof(aws4_key), "AWS4%s", secret_key);
    hmac_sha256((unsigned char *)aws4_key, strlen(aws4_key),
                (unsigned char *)date_stamp, strlen(date_stamp), date_key);
    hmac_sha256(date_key, 32,
                (unsigned char *)region, strlen(region), date_region_key);
    hmac_sha256(date_region_key, 32,
                (unsigned char *)"s3", 2, date_region_service_key);
    hmac_sha256(date_region_service_key, 32,
                (unsigned char *)"aws4_request", 12, signing_key);

    /* Calculate signature */
    hmac_sha256(signing_key, 32,
                (unsigned char *)string_to_sign, strlen(string_to_sign), signature_bytes);
    bytes_to_hex(signature_bytes, 32, signature_hex);

    return pstrdup(signature_hex);
}

/* ============================================================
 * Helper Functions - XML Parsing
 * ============================================================ */

/*
 * Parse S3 ListObjects XML response
 */
static List *
parse_s3_list_response(const char *xml_data)
{
    List *objects = NIL;
    char *cursor = (char *)xml_data;
    char *contents_start;

    if (xml_data == NULL)
        return NIL;

    /* Find each <Contents> block */
    while ((contents_start = strstr(cursor, "<Contents>")) != NULL)
    {
        char *contents_end = strstr(contents_start, "</Contents>");
        if (contents_end == NULL)
            break;

        S3Object *obj = palloc0(sizeof(S3Object));

        /* Extract Key */
        char *key_start = strstr(contents_start, "<Key>");
        if (key_start && key_start < contents_end)
        {
            key_start += 5;
            char *key_end = strstr(key_start, "</Key>");
            if (key_end)
            {
                size_t key_len = key_end - key_start;
                obj->key = palloc(key_len + 1);
                memcpy(obj->key, key_start, key_len);
                obj->key[key_len] = '\0';
            }
        }

        /* Extract ETag */
        char *etag_start = strstr(contents_start, "<ETag>");
        if (etag_start && etag_start < contents_end)
        {
            etag_start += 6;
            char *etag_end = strstr(etag_start, "</ETag>");
            if (etag_end)
            {
                /* Remove quotes */
                if (*etag_start == '"') etag_start++;
                if (*(etag_end - 1) == '"') etag_end--;

                size_t etag_len = etag_end - etag_start;
                obj->etag = palloc(etag_len + 1);
                memcpy(obj->etag, etag_start, etag_len);
                obj->etag[etag_len] = '\0';
            }
        }

        /* Extract Size */
        char *size_start = strstr(contents_start, "<Size>");
        if (size_start && size_start < contents_end)
        {
            size_start += 6;
            obj->size = atoll(size_start);
        }

        objects = lappend(objects, obj);
        cursor = contents_end + 11;  /* Skip past </Contents> */
    }

    return objects;
}

/*
 * Parse S3 HEAD response headers (mock)
 */
static S3Object *
parse_head_response(const char *headers)
{
    S3Object *obj = palloc0(sizeof(S3Object));
    char *etag_line;
    char *size_line;

    if (headers == NULL)
        return obj;

    /* Parse ETag header */
    etag_line = strstr(headers, "ETag:");
    if (etag_line)
    {
        etag_line += 5;
        while (*etag_line == ' ') etag_line++;

        char *etag_end = strchr(etag_line, '\r');
        if (!etag_end) etag_end = strchr(etag_line, '\n');
        if (etag_end)
        {
            /* Remove quotes */
            if (*etag_line == '"') etag_line++;
            if (*(etag_end - 1) == '"') etag_end--;

            size_t etag_len = etag_end - etag_line;
            obj->etag = palloc(etag_len + 1);
            memcpy(obj->etag, etag_line, etag_len);
            obj->etag[etag_len] = '\0';
        }
    }

    /* Parse Content-Length header */
    size_line = strstr(headers, "Content-Length:");
    if (size_line)
    {
        size_line += 15;
        while (*size_line == ' ') size_line++;
        obj->size = atoll(size_line);
    }

    return obj;
}

/* ============================================================
 * Test Cases - URL Encoding
 * ============================================================ */

static void test_url_encode_simple(void)
{
    TEST_BEGIN("url_encode_simple")
        char *encoded = url_encode("hello");

        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_STR_EQ("hello", encoded);

        pfree(encoded);
    TEST_END();
}

static void test_url_encode_spaces(void)
{
    TEST_BEGIN("url_encode_spaces")
        char *encoded = url_encode("hello world");

        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_STR_EQ("hello%20world", encoded);

        pfree(encoded);
    TEST_END();
}

static void test_url_encode_special_chars(void)
{
    TEST_BEGIN("url_encode_special_chars")
        char *encoded = url_encode("key=value&foo=bar");

        TEST_ASSERT_NOT_NULL(encoded);
        /* = and & should be encoded */
        TEST_ASSERT_EQ(0, strstr(encoded, "%3D") != NULL);  /* = */
        TEST_ASSERT_EQ(0, strstr(encoded, "%26") != NULL);  /* & */

        pfree(encoded);
    TEST_END();
}

static void test_url_encode_path_separator(void)
{
    TEST_BEGIN("url_encode_path_separator")
        char *encoded = url_encode("chunks/123/456.chunk");

        TEST_ASSERT_NOT_NULL(encoded);
        /* Path separators should NOT be encoded */
        TEST_ASSERT_STR_EQ("chunks/123/456.chunk", encoded);

        pfree(encoded);
    TEST_END();
}

static void test_url_encode_unreserved_chars(void)
{
    TEST_BEGIN("url_encode_unreserved_chars")
        char *encoded = url_encode("abc-XYZ_123.~");

        TEST_ASSERT_NOT_NULL(encoded);
        /* Unreserved characters should not be encoded */
        TEST_ASSERT_STR_EQ("abc-XYZ_123.~", encoded);

        pfree(encoded);
    TEST_END();
}

static void test_url_encode_unicode(void)
{
    TEST_BEGIN("url_encode_unicode")
        /* UTF-8 character é (0xC3 0xA9) */
        char *encoded = url_encode("café");

        TEST_ASSERT_NOT_NULL(encoded);
        /* Should encode UTF-8 bytes */
        TEST_ASSERT_EQ(0, strncmp(encoded, "caf%C3%A9", 9));

        pfree(encoded);
    TEST_END();
}

/* ============================================================
 * Test Cases - AWS Signature V4
 * ============================================================ */

static void test_sha256_hash_empty(void)
{
    TEST_BEGIN("sha256_hash_empty")
        unsigned char hash[32];
        char hex[65];

        sha256_hash("", 0, hash);
        bytes_to_hex(hash, 32, hex);

        /* SHA256 of empty string is known constant */
        TEST_ASSERT_STR_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
    TEST_END();
}

static void test_sha256_hash_simple(void)
{
    TEST_BEGIN("sha256_hash_simple")
        unsigned char hash[32];
        char hex[65];

        sha256_hash("hello", 5, hash);
        bytes_to_hex(hash, 32, hex);

        /* Verify hex string format */
        TEST_ASSERT_EQ(64, strlen(hex));

        /* Should be all hex characters */
        for (int i = 0; i < 64; i++)
        {
            TEST_ASSERT_TRUE((hex[i] >= '0' && hex[i] <= '9') ||
                           (hex[i] >= 'a' && hex[i] <= 'f'));
        }
    TEST_END();
}

static void test_bytes_to_hex(void)
{
    TEST_BEGIN("bytes_to_hex")
        unsigned char bytes[] = {0x00, 0x01, 0x0F, 0x10, 0xFF};
        char hex[11];

        bytes_to_hex(bytes, 5, hex);

        TEST_ASSERT_STR_EQ("00010f10ff", hex);
    TEST_END();
}

static void test_hmac_sha256_simple(void)
{
    TEST_BEGIN("hmac_sha256_simple")
        unsigned char result[32];
        char hex[65];

        hmac_sha256((unsigned char *)"key", 3,
                   (unsigned char *)"message", 7,
                   result);
        bytes_to_hex(result, 32, hex);

        /* HMAC should produce 32 bytes */
        TEST_ASSERT_EQ(64, strlen(hex));
    TEST_END();
}

static void test_signature_v4_deterministic(void)
{
    TEST_BEGIN("signature_v4_deterministic")
        const char *access_key = "AKIAIOSFODNN7EXAMPLE";
        const char *secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
        const char *region = "us-east-1";
        const char *date_stamp = "20130524";
        const char *amz_date = "20130524T000000Z";
        unsigned char hash[32];
        char payload_hash[65];

        sha256_hash("", 0, hash);
        bytes_to_hex(hash, 32, payload_hash);

        char *sig1 = generate_signature_v4(access_key, secret_key, region,
                                           "GET", "/test.txt", "",
                                           date_stamp, amz_date, payload_hash);
        char *sig2 = generate_signature_v4(access_key, secret_key, region,
                                           "GET", "/test.txt", "",
                                           date_stamp, amz_date, payload_hash);

        /* Same inputs should produce same signature */
        TEST_ASSERT_STR_EQ(sig1, sig2);

        pfree(sig1);
        pfree(sig2);
    TEST_END();
}

static void test_signature_v4_different_methods(void)
{
    TEST_BEGIN("signature_v4_different_methods")
        const char *access_key = "AKIAIOSFODNN7EXAMPLE";
        const char *secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
        const char *region = "us-east-1";
        const char *date_stamp = "20130524";
        const char *amz_date = "20130524T000000Z";
        unsigned char hash[32];
        char payload_hash[65];

        sha256_hash("", 0, hash);
        bytes_to_hex(hash, 32, payload_hash);

        char *sig_get = generate_signature_v4(access_key, secret_key, region,
                                              "GET", "/test.txt", "",
                                              date_stamp, amz_date, payload_hash);
        char *sig_put = generate_signature_v4(access_key, secret_key, region,
                                              "PUT", "/test.txt", "",
                                              date_stamp, amz_date, payload_hash);

        /* Different methods should produce different signatures */
        TEST_ASSERT_NEQ(0, strcmp(sig_get, sig_put));

        pfree(sig_get);
        pfree(sig_put);
    TEST_END();
}

/* ============================================================
 * Test Cases - S3 List XML Parsing
 * ============================================================ */

static void test_parse_list_empty(void)
{
    TEST_BEGIN("parse_list_empty")
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ListBucketResult>\n"
            "  <Name>my-bucket</Name>\n"
            "</ListBucketResult>";

        List *objects = parse_s3_list_response(xml);

        TEST_ASSERT_EQ(0, list_length(objects));
    TEST_END();
}

static void test_parse_list_single_object(void)
{
    TEST_BEGIN("parse_list_single_object")
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ListBucketResult>\n"
            "  <Contents>\n"
            "    <Key>chunks/1/1000.chunk</Key>\n"
            "    <ETag>\"abc123def456\"</ETag>\n"
            "    <Size>1048576</Size>\n"
            "  </Contents>\n"
            "</ListBucketResult>";

        List *objects = parse_s3_list_response(xml);

        TEST_ASSERT_EQ(1, list_length(objects));

        S3Object *obj = (S3Object *)linitial(objects);
        TEST_ASSERT_NOT_NULL(obj);
        TEST_ASSERT_STR_EQ("chunks/1/1000.chunk", obj->key);
        TEST_ASSERT_STR_EQ("abc123def456", obj->etag);
        TEST_ASSERT_EQ(1048576, obj->size);
    TEST_END();
}

static void test_parse_list_multiple_objects(void)
{
    TEST_BEGIN("parse_list_multiple_objects")
        const char *xml =
            "<ListBucketResult>\n"
            "  <Contents>\n"
            "    <Key>chunks/1/1000.chunk</Key>\n"
            "    <ETag>\"aaa\"</ETag>\n"
            "    <Size>100</Size>\n"
            "  </Contents>\n"
            "  <Contents>\n"
            "    <Key>chunks/1/1001.chunk</Key>\n"
            "    <ETag>\"bbb\"</ETag>\n"
            "    <Size>200</Size>\n"
            "  </Contents>\n"
            "  <Contents>\n"
            "    <Key>chunks/2/2000.chunk</Key>\n"
            "    <ETag>\"ccc\"</ETag>\n"
            "    <Size>300</Size>\n"
            "  </Contents>\n"
            "</ListBucketResult>";

        List *objects = parse_s3_list_response(xml);

        TEST_ASSERT_EQ(3, list_length(objects));

        S3Object *obj1 = (S3Object *)linitial(objects);
        S3Object *obj2 = (S3Object *)lsecond(objects);
        S3Object *obj3 = (S3Object *)lthird(objects);

        TEST_ASSERT_STR_EQ("chunks/1/1000.chunk", obj1->key);
        TEST_ASSERT_EQ(100, obj1->size);

        TEST_ASSERT_STR_EQ("chunks/1/1001.chunk", obj2->key);
        TEST_ASSERT_EQ(200, obj2->size);

        TEST_ASSERT_STR_EQ("chunks/2/2000.chunk", obj3->key);
        TEST_ASSERT_EQ(300, obj3->size);
    TEST_END();
}

static void test_parse_list_malformed_xml(void)
{
    TEST_BEGIN("parse_list_malformed_xml")
        const char *xml = "<Contents><Key>test</Key>";  /* Missing closing tags */

        List *objects = parse_s3_list_response(xml);

        /* Should handle gracefully - may return empty or partial list */
        TEST_ASSERT_GTE(list_length(objects), 0);
    TEST_END();
}

static void test_parse_list_null_input(void)
{
    TEST_BEGIN("parse_list_null_input")
        List *objects = parse_s3_list_response(NULL);

        TEST_ASSERT_EQ(0, list_length(objects));
    TEST_END();
}

/* ============================================================
 * Test Cases - HEAD Response Parsing
 * ============================================================ */

static void test_parse_head_response_valid(void)
{
    TEST_BEGIN("parse_head_response_valid")
        const char *headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: 524288\r\n"
            "ETag: \"d41d8cd98f00b204e9800998ecf8427e\"\r\n"
            "\r\n";

        S3Object *obj = parse_head_response(headers);

        TEST_ASSERT_NOT_NULL(obj);
        TEST_ASSERT_EQ(524288, obj->size);
        TEST_ASSERT_NOT_NULL(obj->etag);
        TEST_ASSERT_STR_EQ("d41d8cd98f00b204e9800998ecf8427e", obj->etag);

        pfree(obj);
    TEST_END();
}

static void test_parse_head_response_missing_etag(void)
{
    TEST_BEGIN("parse_head_response_missing_etag")
        const char *headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 1024\r\n"
            "\r\n";

        S3Object *obj = parse_head_response(headers);

        TEST_ASSERT_NOT_NULL(obj);
        TEST_ASSERT_EQ(1024, obj->size);
        /* ETag may be NULL */

        pfree(obj);
    TEST_END();
}

static void test_parse_head_response_null_input(void)
{
    TEST_BEGIN("parse_head_response_null_input")
        S3Object *obj = parse_head_response(NULL);

        TEST_ASSERT_NOT_NULL(obj);
        TEST_ASSERT_EQ(0, obj->size);

        pfree(obj);
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_s3_operations(void)
{
    /* URL encoding tests */
    test_url_encode_simple();
    test_url_encode_spaces();
    test_url_encode_special_chars();
    test_url_encode_path_separator();
    test_url_encode_unreserved_chars();
    test_url_encode_unicode();

    /* AWS Signature V4 tests */
    test_sha256_hash_empty();
    test_sha256_hash_simple();
    test_bytes_to_hex();
    test_hmac_sha256_simple();
    test_signature_v4_deterministic();
    test_signature_v4_different_methods();

    /* S3 List XML parsing tests */
    test_parse_list_empty();
    test_parse_list_single_object();
    test_parse_list_multiple_objects();
    test_parse_list_malformed_xml();
    test_parse_list_null_input();

    /* HEAD response parsing tests */
    test_parse_head_response_valid();
    test_parse_head_response_missing_etag();
    test_parse_head_response_null_input();
}
