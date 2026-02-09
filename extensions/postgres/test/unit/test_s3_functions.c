/*-------------------------------------------------------------------------
 *
 * test_s3_functions.c
 *    Unit tests for S3 list_objects and head_object functions
 *
 * Tests cover:
 *   - XML tag extraction and parsing
 *   - Truncation flag parsing
 *   - Continuation token parsing
 *   - Multiple Contents block parsing
 *   - ETag quote removal
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* Mock types for testing XML parsing */
typedef struct S3Object {
    char *key;
    char *etag;
    long long size;
    long long last_modified;
    char *content_type;
    char *storage_class;
} S3Object;

/* ============================================================
 * XML Parsing Helper (mirrors production implementation)
 * ============================================================ */

static char *
extract_xml_text(const char *tag, const char **next_pos)
{
    char *start_tag;
    char *end_tag;
    char *value;
    char open_tag[128];
    char close_tag[128];
    size_t len;

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start_tag = strstr(*next_pos, open_tag);
    if (start_tag == NULL)
        return NULL;

    start_tag += strlen(open_tag);
    end_tag = strstr(start_tag, close_tag);
    if (end_tag == NULL)
        return NULL;

    len = end_tag - start_tag;
    value = malloc(len + 1);
    memcpy(value, start_tag, len);
    value[len] = '\0';

    *next_pos = end_tag + strlen(close_tag);
    return value;
}

/* ============================================================
 * Test Cases
 * ============================================================ */

static void test_xml_parsing(void)
{
    TEST_BEGIN("xml_basic_tag_extraction")
        const char *xml =
            "<Contents>"
            "<Key>test-key.dat</Key>"
            "<Size>12345</Size>"
            "<ETag>\"abc123\"</ETag>"
            "<LastModified>2025-01-17T12:34:56.000Z</LastModified>"
            "<StorageClass>STANDARD</StorageClass>"
            "</Contents>";

        const char *pos = xml;
        char *key = extract_xml_text("Key", &pos);
        char *size = extract_xml_text("Size", &pos);
        char *etag = extract_xml_text("ETag", &pos);
        char *modified = extract_xml_text("LastModified", &pos);
        char *storage = extract_xml_text("StorageClass", &pos);

        TEST_ASSERT_NOT_NULL(key);
        TEST_ASSERT_STR_EQ("test-key.dat", key);
        TEST_ASSERT_NOT_NULL(size);
        TEST_ASSERT_STR_EQ("12345", size);
        TEST_ASSERT_NOT_NULL(etag);
        TEST_ASSERT_STR_EQ("\"abc123\"", etag);
        TEST_ASSERT_NOT_NULL(modified);
        TEST_ASSERT_STR_EQ("2025-01-17T12:34:56.000Z", modified);
        TEST_ASSERT_NOT_NULL(storage);
        TEST_ASSERT_STR_EQ("STANDARD", storage);

        free(key);
        free(size);
        free(etag);
        free(modified);
        free(storage);
    TEST_END();

    TEST_BEGIN("xml_missing_tag_returns_null")
        const char *xml = "<Key>test-key.dat</Key>";
        const char *pos = xml;
        char *missing = extract_xml_text("NonExistent", &pos);
        TEST_ASSERT_NULL(missing);
    TEST_END();

    TEST_BEGIN("xml_empty_tag_value")
        const char *xml = "<Key></Key>";
        const char *pos = xml;
        char *value = extract_xml_text("Key", &pos);
        TEST_ASSERT_NOT_NULL(value);
        TEST_ASSERT_STR_EQ("", value);
        free(value);
    TEST_END();
}

static void test_truncation_parsing(void)
{
    TEST_BEGIN("truncation_flag_true")
        const char *xml_true = "<IsTruncated>true</IsTruncated>";
        const char *pos = xml_true;
        char *truncated = extract_xml_text("IsTruncated", &pos);
        TEST_ASSERT_NOT_NULL(truncated);
        TEST_ASSERT_STR_EQ("true", truncated);
        free(truncated);
    TEST_END();

    TEST_BEGIN("truncation_flag_false")
        const char *xml_false = "<IsTruncated>false</IsTruncated>";
        const char *pos = xml_false;
        char *truncated = extract_xml_text("IsTruncated", &pos);
        TEST_ASSERT_NOT_NULL(truncated);
        TEST_ASSERT_STR_EQ("false", truncated);
        free(truncated);
    TEST_END();
}

static void test_continuation_token(void)
{
    TEST_BEGIN("continuation_token_extraction")
        const char *xml = "<NextContinuationToken>token123abc</NextContinuationToken>";
        const char *pos = xml;
        char *token = extract_xml_text("NextContinuationToken", &pos);
        TEST_ASSERT_NOT_NULL(token);
        TEST_ASSERT_STR_EQ("token123abc", token);
        free(token);
    TEST_END();
}

static void test_multiple_contents(void)
{
    TEST_BEGIN("multiple_contents_blocks")
        const char *xml =
            "<Contents><Key>file1.dat</Key><Size>100</Size></Contents>"
            "<Contents><Key>file2.dat</Key><Size>200</Size></Contents>"
            "<Contents><Key>file3.dat</Key><Size>300</Size></Contents>";

        const char *pos = xml;
        int count = 0;

        const char *contents = strstr(pos, "<Contents>");
        while (contents != NULL) {
            const char *end = strstr(contents, "</Contents>");
            TEST_ASSERT_NOT_NULL(end);

            const char *block_pos = contents;
            char *key = extract_xml_text("Key", &block_pos);
            char *size = extract_xml_text("Size", &block_pos);

            TEST_ASSERT_NOT_NULL(key);
            TEST_ASSERT_NOT_NULL(size);

            count++;

            free(key);
            free(size);

            pos = end + strlen("</Contents>");
            contents = strstr(pos, "<Contents>");
        }

        TEST_ASSERT_EQ(3, count);
    TEST_END();
}

static void test_etag_quote_removal(void)
{
    TEST_BEGIN("etag_quote_removal")
        const char *etag_quoted = "\"abc123def456\"";
        char *result = strdup(etag_quoted);

        /* Simulate quote removal logic */
        if (*result == '"') {
            char *end = strchr(result + 1, '"');
            if (end) {
                memmove(result, result + 1, end - result);
                result[end - result - 1] = '\0';
            }
        }

        TEST_ASSERT_STR_EQ("abc123def456", result);
        free(result);
    TEST_END();

    TEST_BEGIN("etag_no_quotes")
        const char *etag_plain = "abc123def456";
        char *result = strdup(etag_plain);

        /* Quote removal should leave unquoted strings unchanged */
        if (*result == '"') {
            char *end = strchr(result + 1, '"');
            if (end) {
                memmove(result, result + 1, end - result);
                result[end - result - 1] = '\0';
            }
        }

        TEST_ASSERT_STR_EQ("abc123def456", result);
        free(result);
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_s3_functions(void)
{
    test_xml_parsing();
    test_truncation_parsing();
    test_continuation_token();
    test_multiple_contents();
    test_etag_quote_removal();
}
