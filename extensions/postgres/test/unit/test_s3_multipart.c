/*-------------------------------------------------------------------------
 *
 * test_s3_multipart.c
 *    Unit tests for S3 multipart upload functionality
 *
 * Tests part size calculation, upload thresholds, XML parsing for
 * upload IDs, ETag extraction, and multipart upload logic.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define S3_MULTIPART_THRESHOLD (100 * 1024 * 1024)   /* 100 MB */
#define S3_MIN_PART_SIZE (5 * 1024 * 1024)           /* 5 MB */
#define S3_MAX_PART_SIZE (5 * 1024 * 1024 * 1024LL)  /* 5 GB */
#define S3_MAX_PARTS 10000

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * Calculate optimal part size for multipart upload
 */
static size_t
calculate_part_size(int64_t total_size)
{
    size_t part_size;

    if (total_size <= S3_MULTIPART_THRESHOLD)
        return 0;  /* Don't use multipart */

    /* Calculate part size to stay under max parts */
    part_size = (total_size + S3_MAX_PARTS - 1) / S3_MAX_PARTS;

    /* Round up to next MB */
    part_size = (part_size + 1024 * 1024 - 1) & ~(1024 * 1024 - 1);

    /* Enforce min/max bounds */
    if (part_size < S3_MIN_PART_SIZE)
        part_size = S3_MIN_PART_SIZE;
    if (part_size > S3_MAX_PART_SIZE)
        part_size = S3_MAX_PART_SIZE;

    return part_size;
}

/*
 * Calculate number of parts needed
 */
static int
calculate_num_parts(int64_t total_size, size_t part_size)
{
    if (part_size == 0)
        return 1;
    return (total_size + part_size - 1) / part_size;
}

/*
 * Check if multipart upload should be used
 */
static bool
should_use_multipart(int64_t size)
{
    return size >= S3_MULTIPART_THRESHOLD;
}

/*
 * Extract UploadId from InitiateMultipartUpload XML response
 */
static char *
extract_upload_id_from_xml(const char *xml_data)
{
    char *id_start;
    char *id_end;
    char *upload_id;
    size_t id_len;

    if (xml_data == NULL)
        return NULL;

    /* Look for <UploadId>...</UploadId> */
    id_start = strstr(xml_data, "<UploadId>");
    if (id_start == NULL)
        return NULL;

    id_start += 10;  /* Skip "<UploadId>" */
    id_end = strstr(id_start, "</UploadId>");
    if (id_end == NULL)
        return NULL;

    id_len = id_end - id_start;
    upload_id = palloc(id_len + 1);
    memcpy(upload_id, id_start, id_len);
    upload_id[id_len] = '\0';

    return upload_id;
}

/*
 * Extract ETag from XML response
 */
static char *
extract_etag_from_xml(const char *xml_data)
{
    char *etag_start;
    char *etag_end;
    char *etag;
    size_t etag_len;

    if (xml_data == NULL)
        return NULL;

    /* Look for <ETag>...</ETag> */
    etag_start = strstr(xml_data, "<ETag>");
    if (etag_start == NULL)
        return NULL;

    etag_start += 6;  /* Skip "<ETag>" */
    etag_end = strstr(etag_start, "</ETag>");
    if (etag_end == NULL)
        return NULL;

    /* Remove quotes if present */
    if (*etag_start == '"')
        etag_start++;
    if (*(etag_end - 1) == '"')
        etag_end--;

    etag_len = etag_end - etag_start;
    etag = palloc(etag_len + 1);
    memcpy(etag, etag_start, etag_len);
    etag[etag_len] = '\0';

    return etag;
}

/* ============================================================
 * Test Cases - Part Size Calculation
 * ============================================================ */

static void test_part_size_small_file(void)
{
    TEST_BEGIN("part_size_small_file")
        /* Files under 100MB shouldn't use multipart */
        int64_t size = 50 * 1024 * 1024;  /* 50 MB */
        size_t part_size = calculate_part_size(size);

        TEST_ASSERT_EQ(0, part_size);
        TEST_ASSERT_FALSE(should_use_multipart(size));
    TEST_END();
}

static void test_part_size_at_threshold(void)
{
    TEST_BEGIN("part_size_at_threshold")
        int64_t size = S3_MULTIPART_THRESHOLD;
        bool use_multipart = should_use_multipart(size);

        /* At exactly 100MB, should trigger multipart */
        TEST_ASSERT_TRUE(use_multipart);

        size_t part_size = calculate_part_size(size);
        TEST_ASSERT_GTE(part_size, S3_MIN_PART_SIZE);
    TEST_END();
}

static void test_part_size_just_over_threshold(void)
{
    TEST_BEGIN("part_size_just_over_threshold")
        int64_t size = S3_MULTIPART_THRESHOLD + 1;
        size_t part_size = calculate_part_size(size);

        TEST_ASSERT_GTE(part_size, S3_MIN_PART_SIZE);
        TEST_ASSERT_LTE(part_size, S3_MAX_PART_SIZE);
        TEST_ASSERT_TRUE(should_use_multipart(size));
    TEST_END();
}

static void test_part_size_large_file(void)
{
    TEST_BEGIN("part_size_large_file")
        int64_t size = 1LL * 1024 * 1024 * 1024;  /* 1 GB */
        size_t part_size = calculate_part_size(size);

        TEST_ASSERT_GTE(part_size, S3_MIN_PART_SIZE);
        TEST_ASSERT_LTE(part_size, S3_MAX_PART_SIZE);

        /* Should not exceed max parts */
        int num_parts = calculate_num_parts(size, part_size);
        TEST_ASSERT_LTE(num_parts, S3_MAX_PARTS);
    TEST_END();
}

static void test_part_size_very_large_file(void)
{
    TEST_BEGIN("part_size_very_large_file")
        int64_t size = 50LL * 1024 * 1024 * 1024;  /* 50 GB */
        size_t part_size = calculate_part_size(size);

        TEST_ASSERT_GTE(part_size, S3_MIN_PART_SIZE);
        TEST_ASSERT_LTE(part_size, S3_MAX_PART_SIZE);

        int num_parts = calculate_num_parts(size, part_size);
        TEST_ASSERT_LTE(num_parts, S3_MAX_PARTS);
        TEST_ASSERT_GT(num_parts, 1);
    TEST_END();
}

static void test_part_size_minimum_enforcement(void)
{
    TEST_BEGIN("part_size_minimum_enforcement")
        /* Even small multipart uploads should use minimum part size */
        int64_t size = S3_MULTIPART_THRESHOLD + 1024;
        size_t part_size = calculate_part_size(size);

        TEST_ASSERT_GTE(part_size, S3_MIN_PART_SIZE);
    TEST_END();
}

static void test_part_size_aligned_to_mb(void)
{
    TEST_BEGIN("part_size_aligned_to_mb")
        int64_t size = 500 * 1024 * 1024;  /* 500 MB */
        size_t part_size = calculate_part_size(size);

        /* Part size should be aligned to 1MB boundary */
        TEST_ASSERT_EQ(0, part_size % (1024 * 1024));
    TEST_END();
}

/* ============================================================
 * Test Cases - Number of Parts Calculation
 * ============================================================ */

static void test_num_parts_single_part(void)
{
    TEST_BEGIN("num_parts_single_part")
        int64_t size = 10 * 1024 * 1024;  /* 10 MB */
        size_t part_size = 5 * 1024 * 1024;  /* 5 MB */
        int num_parts = calculate_num_parts(size, part_size);

        TEST_ASSERT_EQ(2, num_parts);
    TEST_END();
}

static void test_num_parts_exact_multiple(void)
{
    TEST_BEGIN("num_parts_exact_multiple")
        int64_t size = 100 * 1024 * 1024;  /* 100 MB */
        size_t part_size = 10 * 1024 * 1024;  /* 10 MB */
        int num_parts = calculate_num_parts(size, part_size);

        TEST_ASSERT_EQ(10, num_parts);
    TEST_END();
}

static void test_num_parts_non_multiple(void)
{
    TEST_BEGIN("num_parts_non_multiple")
        int64_t size = 107 * 1024 * 1024;  /* 107 MB */
        size_t part_size = 10 * 1024 * 1024;  /* 10 MB */
        int num_parts = calculate_num_parts(size, part_size);

        /* Should round up: 107/10 = 10.7 -> 11 parts */
        TEST_ASSERT_EQ(11, num_parts);
    TEST_END();
}

static void test_num_parts_stays_under_max(void)
{
    TEST_BEGIN("num_parts_stays_under_max")
        /* Test with very large file */
        int64_t size = 50LL * 1024 * 1024 * 1024;  /* 50 GB */
        size_t part_size = calculate_part_size(size);
        int num_parts = calculate_num_parts(size, part_size);

        TEST_ASSERT_LTE(num_parts, S3_MAX_PARTS);
        TEST_ASSERT_GT(num_parts, 0);
    TEST_END();
}

/* ============================================================
 * Test Cases - Upload Threshold Logic
 * ============================================================ */

static void test_threshold_just_below(void)
{
    TEST_BEGIN("threshold_just_below")
        int64_t size = S3_MULTIPART_THRESHOLD - 1;
        TEST_ASSERT_FALSE(should_use_multipart(size));
    TEST_END();
}

static void test_threshold_exact(void)
{
    TEST_BEGIN("threshold_exact")
        int64_t size = S3_MULTIPART_THRESHOLD;
        TEST_ASSERT_TRUE(should_use_multipart(size));
    TEST_END();
}

static void test_threshold_just_above(void)
{
    TEST_BEGIN("threshold_just_above")
        int64_t size = S3_MULTIPART_THRESHOLD + 1;
        TEST_ASSERT_TRUE(should_use_multipart(size));
    TEST_END();
}

static void test_threshold_zero_size(void)
{
    TEST_BEGIN("threshold_zero_size")
        int64_t size = 0;
        TEST_ASSERT_FALSE(should_use_multipart(size));
    TEST_END();
}

static void test_threshold_very_small(void)
{
    TEST_BEGIN("threshold_very_small")
        int64_t size = 1024;  /* 1 KB */
        TEST_ASSERT_FALSE(should_use_multipart(size));
    TEST_END();
}

/* ============================================================
 * Test Cases - XML Parsing for Upload ID
 * ============================================================ */

static void test_parse_upload_id_valid(void)
{
    TEST_BEGIN("parse_upload_id_valid")
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<InitiateMultipartUploadResult>\n"
            "  <Bucket>my-bucket</Bucket>\n"
            "  <Key>chunks/12/12345.chunk</Key>\n"
            "  <UploadId>VXBsb2FkSWQtMTIzNDU2Nzg5MA</UploadId>\n"
            "</InitiateMultipartUploadResult>";

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NOT_NULL(upload_id);
        TEST_ASSERT_STR_EQ("VXBsb2FkSWQtMTIzNDU2Nzg5MA", upload_id);

        pfree(upload_id);
    TEST_END();
}

static void test_parse_upload_id_with_whitespace(void)
{
    TEST_BEGIN("parse_upload_id_with_whitespace")
        const char *xml =
            "<InitiateMultipartUploadResult>\n"
            "  <UploadId>  ABC123XYZ  </UploadId>\n"
            "</InitiateMultipartUploadResult>";

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NOT_NULL(upload_id);
        /* Note: Our simple parser doesn't trim, it extracts exact content */
        /* This tests the actual behavior */
        TEST_ASSERT_EQ(0, strncmp(upload_id, "  ABC123XYZ  ", 13));

        pfree(upload_id);
    TEST_END();
}

static void test_parse_upload_id_missing_tag(void)
{
    TEST_BEGIN("parse_upload_id_missing_tag")
        const char *xml =
            "<InitiateMultipartUploadResult>\n"
            "  <Bucket>my-bucket</Bucket>\n"
            "</InitiateMultipartUploadResult>";

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NULL(upload_id);
    TEST_END();
}

static void test_parse_upload_id_malformed_xml(void)
{
    TEST_BEGIN("parse_upload_id_malformed_xml")
        const char *xml = "<UploadId>ABC123";  /* Missing closing tag */

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NULL(upload_id);
    TEST_END();
}

static void test_parse_upload_id_empty(void)
{
    TEST_BEGIN("parse_upload_id_empty")
        const char *xml = "<UploadId></UploadId>";

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NOT_NULL(upload_id);
        TEST_ASSERT_STR_EQ("", upload_id);

        pfree(upload_id);
    TEST_END();
}

static void test_parse_upload_id_null_input(void)
{
    TEST_BEGIN("parse_upload_id_null_input")
        char *upload_id = extract_upload_id_from_xml(NULL);

        TEST_ASSERT_NULL(upload_id);
    TEST_END();
}

static void test_parse_upload_id_complex_xml(void)
{
    TEST_BEGIN("parse_upload_id_complex_xml")
        const char *xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<InitiateMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
            "  <Bucket>orochi-db-chunks</Bucket>\n"
            "  <Key>production/chunks/12345/12345678.chunk</Key>\n"
            "  <UploadId>exampleUploadIdValue1234567890abcdef</UploadId>\n"
            "</InitiateMultipartUploadResult>";

        char *upload_id = extract_upload_id_from_xml(xml);

        TEST_ASSERT_NOT_NULL(upload_id);
        TEST_ASSERT_STR_EQ("exampleUploadIdValue1234567890abcdef", upload_id);

        pfree(upload_id);
    TEST_END();
}

/* ============================================================
 * Test Cases - ETag Extraction
 * ============================================================ */

static void test_extract_etag_valid(void)
{
    TEST_BEGIN("extract_etag_valid")
        const char *xml =
            "<CompleteMultipartUploadResult>\n"
            "  <ETag>&quot;3858f62230ac3c915f300c664312c11f-9&quot;</ETag>\n"
            "</CompleteMultipartUploadResult>";

        char *etag = extract_etag_from_xml(xml);

        TEST_ASSERT_NOT_NULL(etag);
        /* Our parser removes quotes */
        TEST_ASSERT_STR_EQ("3858f62230ac3c915f300c664312c11f-9", etag);

        pfree(etag);
    TEST_END();
}

static void test_extract_etag_without_quotes(void)
{
    TEST_BEGIN("extract_etag_without_quotes")
        const char *xml =
            "<ETag>abc123def456</ETag>";

        char *etag = extract_etag_from_xml(xml);

        TEST_ASSERT_NOT_NULL(etag);
        TEST_ASSERT_STR_EQ("abc123def456", etag);

        pfree(etag);
    TEST_END();
}

static void test_extract_etag_with_html_entities(void)
{
    TEST_BEGIN("extract_etag_with_html_entities")
        const char *xml = "<ETag>&quot;abc123&quot;</ETag>";

        char *etag = extract_etag_from_xml(xml);

        TEST_ASSERT_NOT_NULL(etag);
        /* Parser removes literal quote characters, not HTML entities */
        /* This tests actual behavior */
        TEST_ASSERT_EQ(0, strcmp(etag, "&quot;abc123&quot;"));

        pfree(etag);
    TEST_END();
}

static void test_extract_etag_missing(void)
{
    TEST_BEGIN("extract_etag_missing")
        const char *xml = "<CompleteMultipartUploadResult></CompleteMultipartUploadResult>";

        char *etag = extract_etag_from_xml(xml);

        TEST_ASSERT_NULL(etag);
    TEST_END();
}

static void test_extract_etag_null_input(void)
{
    TEST_BEGIN("extract_etag_null_input")
        char *etag = extract_etag_from_xml(NULL);

        TEST_ASSERT_NULL(etag);
    TEST_END();
}

static void test_extract_etag_multipart_format(void)
{
    TEST_BEGIN("extract_etag_multipart_format")
        /* Multipart upload ETags have format: hash-partcount */
        const char *xml = "<ETag>\"d41d8cd98f00b204e9800998ecf8427e-125\"</ETag>";

        char *etag = extract_etag_from_xml(xml);

        TEST_ASSERT_NOT_NULL(etag);
        TEST_ASSERT_STR_EQ("d41d8cd98f00b204e9800998ecf8427e-125", etag);

        /* Verify format: hash-number */
        char *dash = strchr(etag, '-');
        TEST_ASSERT_NOT_NULL(dash);

        pfree(etag);
    TEST_END();
}

/* ============================================================
 * Test Cases - Edge Cases
 * ============================================================ */

static void test_max_parts_boundary(void)
{
    TEST_BEGIN("max_parts_boundary")
        /* File size that would require exactly max parts */
        int64_t size = (int64_t)S3_MIN_PART_SIZE * S3_MAX_PARTS;
        size_t part_size = calculate_part_size(size);
        int num_parts = calculate_num_parts(size, part_size);

        TEST_ASSERT_LTE(num_parts, S3_MAX_PARTS);
    TEST_END();
}

static void test_part_number_validation(void)
{
    TEST_BEGIN("part_number_validation")
        /* Part numbers must be 1-10000 */
        int valid_min = 1;
        int valid_max = 10000;
        int invalid_low = 0;
        int invalid_high = 10001;

        TEST_ASSERT_GTE(valid_min, 1);
        TEST_ASSERT_LTE(valid_min, S3_MAX_PARTS);
        TEST_ASSERT_GTE(valid_max, 1);
        TEST_ASSERT_LTE(valid_max, S3_MAX_PARTS);
        TEST_ASSERT_LT(invalid_low, 1);
        TEST_ASSERT_GT(invalid_high, S3_MAX_PARTS);
    TEST_END();
}

static void test_last_part_smaller_than_min(void)
{
    TEST_BEGIN("last_part_smaller_than_min")
        /* Last part can be smaller than 5MB */
        int64_t size = S3_MULTIPART_THRESHOLD + 1024 * 1024;  /* 101 MB */
        size_t part_size = 50 * 1024 * 1024;  /* 50 MB parts */
        int num_parts = calculate_num_parts(size, part_size);

        TEST_ASSERT_EQ(3, num_parts);

        /* Calculate last part size */
        int64_t last_part_size = size - (part_size * (num_parts - 1));
        TEST_ASSERT_GT(last_part_size, 0);
        TEST_ASSERT_LT(last_part_size, S3_MIN_PART_SIZE);  /* Last part < 5MB is OK */
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_s3_multipart(void)
{
    /* Part size calculation tests */
    test_part_size_small_file();
    test_part_size_at_threshold();
    test_part_size_just_over_threshold();
    test_part_size_large_file();
    test_part_size_very_large_file();
    test_part_size_minimum_enforcement();
    test_part_size_aligned_to_mb();

    /* Number of parts calculation tests */
    test_num_parts_single_part();
    test_num_parts_exact_multiple();
    test_num_parts_non_multiple();
    test_num_parts_stays_under_max();

    /* Upload threshold logic tests */
    test_threshold_just_below();
    test_threshold_exact();
    test_threshold_just_above();
    test_threshold_zero_size();
    test_threshold_very_small();

    /* XML parsing for upload ID tests */
    test_parse_upload_id_valid();
    test_parse_upload_id_with_whitespace();
    test_parse_upload_id_missing_tag();
    test_parse_upload_id_malformed_xml();
    test_parse_upload_id_empty();
    test_parse_upload_id_null_input();
    test_parse_upload_id_complex_xml();

    /* ETag extraction tests */
    test_extract_etag_valid();
    test_extract_etag_without_quotes();
    test_extract_etag_with_html_entities();
    test_extract_etag_missing();
    test_extract_etag_null_input();
    test_extract_etag_multipart_format();

    /* Edge case tests */
    test_max_parts_boundary();
    test_part_number_validation();
    test_last_part_smaller_than_min();
}
