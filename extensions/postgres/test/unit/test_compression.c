/*-------------------------------------------------------------------------
 *
 * test_compression.c
 *    Unit tests for Orochi DB compression algorithms
 *
 * Tests cover:
 *   - Delta encoding (sequential, random, negative deltas, overflow)
 *   - Run-Length Encoding (repeated values, no repeats, single element)
 *   - Dictionary encoding (string encoding, lookup, capacity)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Delta Encoding Implementation (standalone for testing)
 * ============================================================ */

#define MAX_DELTA_VALUES 1024

typedef struct DeltaEncoded {
    int64_t base_value;
    int32_t *deltas;
    int count;
} DeltaEncoded;

static DeltaEncoded *
delta_encode(const int64_t *values, int count)
{
    DeltaEncoded *result;

    if (count <= 0)
        return NULL;

    result = (DeltaEncoded *)malloc(sizeof(DeltaEncoded));
    if (result == NULL)
        return NULL;

    result->base_value = values[0];
    result->count = count;
    result->deltas = (int32_t *)malloc(sizeof(int32_t) * count);

    if (result->deltas == NULL) {
        free(result);
        return NULL;
    }

    result->deltas[0] = 0; /* first delta is always 0 */
    for (int i = 1; i < count; i++) {
        int64_t diff = values[i] - values[i - 1];
        /* Clamp to int32 range for safety */
        if (diff > INT32_MAX)
            result->deltas[i] = INT32_MAX;
        else if (diff < INT32_MIN)
            result->deltas[i] = INT32_MIN;
        else
            result->deltas[i] = (int32_t)diff;
    }

    return result;
}

static int64_t *
delta_decode(const DeltaEncoded *encoded, int *out_count)
{
    int64_t *values;

    if (encoded == NULL || encoded->count <= 0)
        return NULL;

    values = (int64_t *)malloc(sizeof(int64_t) * encoded->count);
    if (values == NULL)
        return NULL;

    values[0] = encoded->base_value;
    for (int i = 1; i < encoded->count; i++) {
        values[i] = values[i - 1] + encoded->deltas[i];
    }

    *out_count = encoded->count;
    return values;
}

static void
delta_free(DeltaEncoded *encoded)
{
    if (encoded) {
        free(encoded->deltas);
        free(encoded);
    }
}

/* ============================================================
 * Run-Length Encoding Implementation (standalone for testing)
 * ============================================================ */

#define MAX_RLE_RUNS 512

typedef struct RLERun {
    int64_t value;
    int32_t run_length;
} RLERun;

typedef struct RLEEncoded {
    RLERun *runs;
    int num_runs;
    int total_values;
} RLEEncoded;

static RLEEncoded *
rle_encode(const int64_t *values, int count)
{
    RLEEncoded *result;

    if (count <= 0)
        return NULL;

    result = (RLEEncoded *)malloc(sizeof(RLEEncoded));
    if (result == NULL)
        return NULL;

    result->runs = (RLERun *)malloc(sizeof(RLERun) * count); /* worst case */
    if (result->runs == NULL) {
        free(result);
        return NULL;
    }

    result->total_values = count;
    result->num_runs = 0;

    int run_start = 0;
    while (run_start < count) {
        int64_t current = values[run_start];
        int run_len = 1;

        while (run_start + run_len < count &&
               values[run_start + run_len] == current) {
            run_len++;
        }

        result->runs[result->num_runs].value = current;
        result->runs[result->num_runs].run_length = run_len;
        result->num_runs++;

        run_start += run_len;
    }

    return result;
}

static int64_t *
rle_decode(const RLEEncoded *encoded, int *out_count)
{
    int64_t *values;
    int pos = 0;

    if (encoded == NULL || encoded->num_runs <= 0)
        return NULL;

    values = (int64_t *)malloc(sizeof(int64_t) * encoded->total_values);
    if (values == NULL)
        return NULL;

    for (int i = 0; i < encoded->num_runs; i++) {
        for (int j = 0; j < encoded->runs[i].run_length; j++) {
            values[pos++] = encoded->runs[i].value;
        }
    }

    *out_count = pos;
    return values;
}

static void
rle_free(RLEEncoded *encoded)
{
    if (encoded) {
        free(encoded->runs);
        free(encoded);
    }
}

/* ============================================================
 * Dictionary Encoding Implementation (standalone for testing)
 * ============================================================ */

#define DICT_MAX_ENTRIES 256
#define DICT_MAX_STRING_LEN 128

typedef struct DictEntry {
    char value[DICT_MAX_STRING_LEN];
    int32_t code;
} DictEntry;

typedef struct DictEncoded {
    DictEntry *dictionary;
    int dict_size;
    int32_t *codes;
    int num_values;
} DictEncoded;

static int
dict_lookup_or_add(DictEncoded *dict, const char *value)
{
    /* Search existing entries */
    for (int i = 0; i < dict->dict_size; i++) {
        if (strcmp(dict->dictionary[i].value, value) == 0)
            return dict->dictionary[i].code;
    }

    /* Add new entry */
    if (dict->dict_size >= DICT_MAX_ENTRIES)
        return -1; /* dictionary full */

    int code = dict->dict_size;
    strncpy(dict->dictionary[code].value, value, DICT_MAX_STRING_LEN - 1);
    dict->dictionary[code].value[DICT_MAX_STRING_LEN - 1] = '\0';
    dict->dictionary[code].code = code;
    dict->dict_size++;

    return code;
}

static DictEncoded *
dict_encode(const char **values, int count)
{
    DictEncoded *result;

    if (count <= 0)
        return NULL;

    result = (DictEncoded *)calloc(1, sizeof(DictEncoded));
    if (result == NULL)
        return NULL;

    result->dictionary = (DictEntry *)calloc(DICT_MAX_ENTRIES, sizeof(DictEntry));
    result->codes = (int32_t *)malloc(sizeof(int32_t) * count);
    result->num_values = count;
    result->dict_size = 0;

    if (result->dictionary == NULL || result->codes == NULL) {
        free(result->dictionary);
        free(result->codes);
        free(result);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        int code = dict_lookup_or_add(result, values[i]);
        result->codes[i] = code;
    }

    return result;
}

static const char *
dict_decode_value(const DictEncoded *encoded, int32_t code)
{
    if (encoded == NULL || code < 0 || code >= encoded->dict_size)
        return NULL;

    return encoded->dictionary[code].value;
}

static void
dict_free(DictEncoded *encoded)
{
    if (encoded) {
        free(encoded->dictionary);
        free(encoded->codes);
        free(encoded);
    }
}

/* ============================================================
 * Delta Encoding Tests
 * ============================================================ */

static void test_delta_sequential(void)
{
    TEST_BEGIN("delta_sequential_values")
        /* Sequential values should produce all deltas of 1 */
        int64_t values[] = {100, 101, 102, 103, 104, 105, 106, 107};
        int count = 8;

        DeltaEncoded *encoded = delta_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_EQ(100, encoded->base_value);
        TEST_ASSERT_EQ(count, encoded->count);

        /* All deltas except first should be 1 */
        TEST_ASSERT_EQ(0, encoded->deltas[0]);
        for (int i = 1; i < count; i++) {
            TEST_ASSERT_EQ(1, encoded->deltas[i]);
        }

        /* Decode and verify roundtrip */
        int decoded_count = 0;
        int64_t *decoded = delta_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);
        TEST_ASSERT_EQ(count, decoded_count);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], decoded[i]);
        }

        free(decoded);
        delta_free(encoded);
    TEST_END();
}

static void test_delta_random(void)
{
    TEST_BEGIN("delta_random_values")
        /* Non-sequential values with varying deltas */
        int64_t values[] = {10, 50, 30, 100, 95, 200};
        int count = 6;

        DeltaEncoded *encoded = delta_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_EQ(10, encoded->base_value);

        /* Verify specific deltas */
        TEST_ASSERT_EQ(0, encoded->deltas[0]);   /* base */
        TEST_ASSERT_EQ(40, encoded->deltas[1]);   /* 50 - 10 */
        TEST_ASSERT_EQ(-20, encoded->deltas[2]);  /* 30 - 50 */
        TEST_ASSERT_EQ(70, encoded->deltas[3]);   /* 100 - 30 */
        TEST_ASSERT_EQ(-5, encoded->deltas[4]);   /* 95 - 100 */
        TEST_ASSERT_EQ(105, encoded->deltas[5]);  /* 200 - 95 */

        /* Roundtrip decode */
        int decoded_count = 0;
        int64_t *decoded = delta_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], decoded[i]);
        }

        free(decoded);
        delta_free(encoded);
    TEST_END();
}

static void test_delta_negative(void)
{
    TEST_BEGIN("delta_negative_deltas")
        /* Monotonically decreasing values */
        int64_t values[] = {1000, 900, 800, 700, 600};
        int count = 5;

        DeltaEncoded *encoded = delta_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_EQ(1000, encoded->base_value);

        /* All deltas should be -100 */
        for (int i = 1; i < count; i++) {
            TEST_ASSERT_EQ(-100, encoded->deltas[i]);
        }

        /* Roundtrip decode */
        int decoded_count = 0;
        int64_t *decoded = delta_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], decoded[i]);
        }

        free(decoded);
        delta_free(encoded);
    TEST_END();
}

static void test_delta_overflow(void)
{
    TEST_BEGIN("delta_large_differences_clamped")
        /* Values with differences exceeding int32 range */
        int64_t values[] = {0, (int64_t)INT32_MAX + 100LL};
        int count = 2;

        DeltaEncoded *encoded = delta_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Delta should be clamped to INT32_MAX */
        TEST_ASSERT_EQ(INT32_MAX, encoded->deltas[1]);

        delta_free(encoded);
    TEST_END();

    TEST_BEGIN("delta_single_element")
        int64_t values[] = {42};
        int count = 1;

        DeltaEncoded *encoded = delta_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_EQ(42, encoded->base_value);
        TEST_ASSERT_EQ(1, encoded->count);
        TEST_ASSERT_EQ(0, encoded->deltas[0]);

        int decoded_count = 0;
        int64_t *decoded = delta_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);
        TEST_ASSERT_EQ(1, decoded_count);
        TEST_ASSERT_EQ(42, decoded[0]);

        free(decoded);
        delta_free(encoded);
    TEST_END();
}

/* ============================================================
 * RLE Tests
 * ============================================================ */

static void test_rle_repeated(void)
{
    TEST_BEGIN("rle_repeated_values")
        /* Many repeated values should compress well */
        int64_t values[] = {5, 5, 5, 5, 5, 10, 10, 10, 20, 20};
        int count = 10;

        RLEEncoded *encoded = rle_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Should produce 3 runs: (5,5), (10,3), (20,2) */
        TEST_ASSERT_EQ(3, encoded->num_runs);
        TEST_ASSERT_EQ(10, encoded->total_values);

        TEST_ASSERT_EQ(5, encoded->runs[0].value);
        TEST_ASSERT_EQ(5, encoded->runs[0].run_length);

        TEST_ASSERT_EQ(10, encoded->runs[1].value);
        TEST_ASSERT_EQ(3, encoded->runs[1].run_length);

        TEST_ASSERT_EQ(20, encoded->runs[2].value);
        TEST_ASSERT_EQ(2, encoded->runs[2].run_length);

        /* Roundtrip decode */
        int decoded_count = 0;
        int64_t *decoded = rle_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);
        TEST_ASSERT_EQ(count, decoded_count);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], decoded[i]);
        }

        free(decoded);
        rle_free(encoded);
    TEST_END();
}

static void test_rle_no_repeats(void)
{
    TEST_BEGIN("rle_no_repeated_values")
        /* All unique values - worst case for RLE */
        int64_t values[] = {1, 2, 3, 4, 5};
        int count = 5;

        RLEEncoded *encoded = rle_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Each value is its own run */
        TEST_ASSERT_EQ(5, encoded->num_runs);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], encoded->runs[i].value);
            TEST_ASSERT_EQ(1, encoded->runs[i].run_length);
        }

        /* Roundtrip decode */
        int decoded_count = 0;
        int64_t *decoded = rle_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);
        TEST_ASSERT_EQ(count, decoded_count);

        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQ(values[i], decoded[i]);
        }

        free(decoded);
        rle_free(encoded);
    TEST_END();
}

static void test_rle_single_element(void)
{
    TEST_BEGIN("rle_single_element")
        int64_t values[] = {999};
        int count = 1;

        RLEEncoded *encoded = rle_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);
        TEST_ASSERT_EQ(1, encoded->num_runs);
        TEST_ASSERT_EQ(999, encoded->runs[0].value);
        TEST_ASSERT_EQ(1, encoded->runs[0].run_length);

        int decoded_count = 0;
        int64_t *decoded = rle_decode(encoded, &decoded_count);
        TEST_ASSERT_NOT_NULL(decoded);
        TEST_ASSERT_EQ(1, decoded_count);
        TEST_ASSERT_EQ(999, decoded[0]);

        free(decoded);
        rle_free(encoded);
    TEST_END();
}

/* ============================================================
 * Dictionary Encoding Tests
 * ============================================================ */

static void test_dict_string_encoding(void)
{
    TEST_BEGIN("dict_string_encoding")
        const char *values[] = {"red", "green", "blue", "red", "blue", "red"};
        int count = 6;

        DictEncoded *encoded = dict_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Should have 3 dictionary entries */
        TEST_ASSERT_EQ(3, encoded->dict_size);
        TEST_ASSERT_EQ(6, encoded->num_values);

        /* Verify dictionary entries */
        TEST_ASSERT_STR_EQ("red", encoded->dictionary[0].value);
        TEST_ASSERT_STR_EQ("green", encoded->dictionary[1].value);
        TEST_ASSERT_STR_EQ("blue", encoded->dictionary[2].value);

        /* Verify codes: red=0, green=1, blue=2 */
        TEST_ASSERT_EQ(0, encoded->codes[0]); /* red */
        TEST_ASSERT_EQ(1, encoded->codes[1]); /* green */
        TEST_ASSERT_EQ(2, encoded->codes[2]); /* blue */
        TEST_ASSERT_EQ(0, encoded->codes[3]); /* red */
        TEST_ASSERT_EQ(2, encoded->codes[4]); /* blue */
        TEST_ASSERT_EQ(0, encoded->codes[5]); /* red */

        dict_free(encoded);
    TEST_END();
}

static void test_dict_lookup(void)
{
    TEST_BEGIN("dict_decode_by_code")
        const char *values[] = {"alpha", "beta", "gamma"};
        int count = 3;

        DictEncoded *encoded = dict_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Look up each code */
        const char *val0 = dict_decode_value(encoded, 0);
        TEST_ASSERT_NOT_NULL(val0);
        TEST_ASSERT_STR_EQ("alpha", val0);

        const char *val1 = dict_decode_value(encoded, 1);
        TEST_ASSERT_NOT_NULL(val1);
        TEST_ASSERT_STR_EQ("beta", val1);

        const char *val2 = dict_decode_value(encoded, 2);
        TEST_ASSERT_NOT_NULL(val2);
        TEST_ASSERT_STR_EQ("gamma", val2);

        /* Invalid code returns NULL */
        const char *invalid = dict_decode_value(encoded, 99);
        TEST_ASSERT_NULL(invalid);

        const char *negative = dict_decode_value(encoded, -1);
        TEST_ASSERT_NULL(negative);

        dict_free(encoded);
    TEST_END();
}

static void test_dict_capacity(void)
{
    TEST_BEGIN("dict_capacity_limit")
        /* Fill dictionary to capacity */
        const char *values[DICT_MAX_ENTRIES + 1];
        char buf[DICT_MAX_ENTRIES + 1][32];

        for (int i = 0; i <= DICT_MAX_ENTRIES; i++) {
            snprintf(buf[i], sizeof(buf[i]), "unique_value_%d", i);
            values[i] = buf[i];
        }

        DictEncoded *encoded = dict_encode(values, DICT_MAX_ENTRIES + 1);
        TEST_ASSERT_NOT_NULL(encoded);

        /* Dictionary should be full at DICT_MAX_ENTRIES */
        TEST_ASSERT_EQ(DICT_MAX_ENTRIES, encoded->dict_size);

        /* The value beyond capacity should have code -1 */
        TEST_ASSERT_EQ(-1, encoded->codes[DICT_MAX_ENTRIES]);

        dict_free(encoded);
    TEST_END();
}

static void test_dict_empty_strings(void)
{
    TEST_BEGIN("dict_empty_and_duplicate_strings")
        const char *values[] = {"", "hello", "", "world", "hello"};
        int count = 5;

        DictEncoded *encoded = dict_encode(values, count);
        TEST_ASSERT_NOT_NULL(encoded);

        /* 3 unique entries: "", "hello", "world" */
        TEST_ASSERT_EQ(3, encoded->dict_size);

        /* Verify codes */
        TEST_ASSERT_EQ(0, encoded->codes[0]); /* "" */
        TEST_ASSERT_EQ(1, encoded->codes[1]); /* "hello" */
        TEST_ASSERT_EQ(0, encoded->codes[2]); /* "" (duplicate) */
        TEST_ASSERT_EQ(2, encoded->codes[3]); /* "world" */
        TEST_ASSERT_EQ(1, encoded->codes[4]); /* "hello" (duplicate) */

        /* Verify lookup */
        const char *empty = dict_decode_value(encoded, 0);
        TEST_ASSERT_NOT_NULL(empty);
        TEST_ASSERT_STR_EQ("", empty);

        dict_free(encoded);
    TEST_END();
}

/* ============================================================
 * Edge Case Tests
 * ============================================================ */

static void test_null_and_empty_inputs(void)
{
    TEST_BEGIN("delta_encode_empty")
        DeltaEncoded *encoded = delta_encode(NULL, 0);
        TEST_ASSERT_NULL(encoded);
    TEST_END();

    TEST_BEGIN("rle_encode_empty")
        RLEEncoded *encoded = rle_encode(NULL, 0);
        TEST_ASSERT_NULL(encoded);
    TEST_END();

    TEST_BEGIN("dict_encode_empty")
        DictEncoded *encoded = dict_encode(NULL, 0);
        TEST_ASSERT_NULL(encoded);
    TEST_END();

    TEST_BEGIN("delta_decode_null")
        int count = 0;
        int64_t *decoded = delta_decode(NULL, &count);
        TEST_ASSERT_NULL(decoded);
    TEST_END();

    TEST_BEGIN("rle_decode_null")
        int count = 0;
        int64_t *decoded = rle_decode(NULL, &count);
        TEST_ASSERT_NULL(decoded);
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_compression(void)
{
    /* Delta encoding tests */
    test_delta_sequential();
    test_delta_random();
    test_delta_negative();
    test_delta_overflow();

    /* RLE tests */
    test_rle_repeated();
    test_rle_no_repeats();
    test_rle_single_element();

    /* Dictionary encoding tests */
    test_dict_string_encoding();
    test_dict_lookup();
    test_dict_capacity();
    test_dict_empty_strings();

    /* Edge cases */
    test_null_and_empty_inputs();
}
