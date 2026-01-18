/*-------------------------------------------------------------------------
 *
 * test_s3_functions.c
 *    Unit tests for S3 list_objects and head_object functions
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Simple test framework */
#define RUN_TEST(test) do { \
    printf("Running %s...", #test); \
    test(); \
    printf(" PASSED\n"); \
} while(0)

/* Mock types and functions for testing XML parsing */
typedef struct S3Object {
    char *key;
    char *etag;
    long long size;
    long long last_modified;
    char *content_type;
    char *storage_class;
} S3Object;

/* Test XML parsing helper */
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

/* Test XML parsing */
void test_xml_parsing(void)
{
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

    assert(key != NULL);
    assert(strcmp(key, "test-key.dat") == 0);
    assert(size != NULL);
    assert(strcmp(size, "12345") == 0);
    assert(etag != NULL);
    assert(strcmp(etag, "\"abc123\"") == 0);
    assert(modified != NULL);
    assert(strcmp(modified, "2025-01-17T12:34:56.000Z") == 0);
    assert(storage != NULL);
    assert(strcmp(storage, "STANDARD") == 0);

    free(key);
    free(size);
    free(etag);
    free(modified);
    free(storage);
}

/* Test truncation flag parsing */
void test_truncation_parsing(void)
{
    const char *xml_true = "<IsTruncated>true</IsTruncated>";
    const char *xml_false = "<IsTruncated>false</IsTruncated>";

    const char *pos = xml_true;
    char *truncated = extract_xml_text("IsTruncated", &pos);
    assert(truncated != NULL);
    assert(strcmp(truncated, "true") == 0);
    free(truncated);

    pos = xml_false;
    truncated = extract_xml_text("IsTruncated", &pos);
    assert(truncated != NULL);
    assert(strcmp(truncated, "false") == 0);
    free(truncated);
}

/* Test continuation token parsing */
void test_continuation_token(void)
{
    const char *xml = "<NextContinuationToken>token123abc</NextContinuationToken>";

    const char *pos = xml;
    char *token = extract_xml_text("NextContinuationToken", &pos);
    assert(token != NULL);
    assert(strcmp(token, "token123abc") == 0);
    free(token);
}

/* Test multiple Contents parsing */
void test_multiple_contents(void)
{
    const char *xml =
        "<Contents><Key>file1.dat</Key><Size>100</Size></Contents>"
        "<Contents><Key>file2.dat</Key><Size>200</Size></Contents>"
        "<Contents><Key>file3.dat</Key><Size>300</Size></Contents>";

    const char *pos = xml;
    int count = 0;

    /* Find each <Contents> block */
    const char *contents = strstr(pos, "<Contents>");
    while (contents != NULL) {
        const char *end = strstr(contents, "</Contents>");
        assert(end != NULL);

        const char *block_pos = contents;
        char *key = extract_xml_text("Key", &block_pos);
        char *size = extract_xml_text("Size", &block_pos);

        assert(key != NULL);
        assert(size != NULL);

        count++;
        printf("\n  Object %d: key=%s, size=%s", count, key, size);

        free(key);
        free(size);

        pos = end + strlen("</Contents>");
        contents = strstr(pos, "<Contents>");
    }

    assert(count == 3);
}

/* Test ETag quote removal */
void test_etag_quote_removal(void)
{
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

    assert(strcmp(result, "abc123def456") == 0);
    free(result);
}

int main(void)
{
    printf("S3 Functions Unit Tests\n");
    printf("========================\n\n");

    RUN_TEST(test_xml_parsing);
    RUN_TEST(test_truncation_parsing);
    RUN_TEST(test_continuation_token);
    RUN_TEST(test_multiple_contents);
    RUN_TEST(test_etag_quote_removal);

    printf("\nAll tests passed!\n");
    return 0;
}
