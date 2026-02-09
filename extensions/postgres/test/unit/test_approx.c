/*-------------------------------------------------------------------------
 *
 * test_approx.c
 *    Unit tests for approximate algorithms in Orochi DB
 *
 * Tests HyperLogLog, T-Digest, and Sampling implementations
 * without requiring PostgreSQL infrastructure.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <float.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Standalone implementations for testing
 * (These mirror the actual implementations without PostgreSQL deps)
 * ============================================================ */

/* ----- HyperLogLog Implementation ----- */

#define HLL_PRECISION_DEFAULT   14
#define HLL_PRECISION_MIN       4
#define HLL_PRECISION_MAX       18
#define HLL_REGISTERS(p)        (1 << (p))
#define MURMUR_SEED             42

typedef struct TestHLL
{
    uint8_t     precision;
    int64_t     total_count;
    uint8_t    *registers;
} TestHLL;

static inline uint64_t rotl64_test(uint64_t x, int8_t r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t fmix64_test(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static uint64_t murmurhash3_64_test(const void *key, size_t len, uint32_t seed)
{
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = len / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    const uint64_t *blocks = (const uint64_t *)(data);

    for (int i = 0; i < nblocks; i++)
    {
        uint64_t k1 = blocks[i * 2];
        uint64_t k2 = blocks[i * 2 + 1];

        k1 *= c1;
        k1 = rotl64_test(k1, 31);
        k1 *= c2;
        h1 ^= k1;

        h1 = rotl64_test(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = rotl64_test(k2, 33);
        k2 *= c1;
        h2 ^= k2;

        h2 = rotl64_test(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    const uint8_t *tail = (const uint8_t *)(data + nblocks * 16);

    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch (len & 15)
    {
        case 15: k2 ^= ((uint64_t)tail[14]) << 48; /* fallthrough */
        case 14: k2 ^= ((uint64_t)tail[13]) << 40; /* fallthrough */
        case 13: k2 ^= ((uint64_t)tail[12]) << 32; /* fallthrough */
        case 12: k2 ^= ((uint64_t)tail[11]) << 24; /* fallthrough */
        case 11: k2 ^= ((uint64_t)tail[10]) << 16; /* fallthrough */
        case 10: k2 ^= ((uint64_t)tail[9]) << 8;   /* fallthrough */
        case 9:  k2 ^= ((uint64_t)tail[8]) << 0;
                 k2 *= c2;
                 k2 = rotl64_test(k2, 33);
                 k2 *= c1;
                 h2 ^= k2;
                 /* fallthrough */

        case 8:  k1 ^= ((uint64_t)tail[7]) << 56; /* fallthrough */
        case 7:  k1 ^= ((uint64_t)tail[6]) << 48; /* fallthrough */
        case 6:  k1 ^= ((uint64_t)tail[5]) << 40; /* fallthrough */
        case 5:  k1 ^= ((uint64_t)tail[4]) << 32; /* fallthrough */
        case 4:  k1 ^= ((uint64_t)tail[3]) << 24; /* fallthrough */
        case 3:  k1 ^= ((uint64_t)tail[2]) << 16; /* fallthrough */
        case 2:  k1 ^= ((uint64_t)tail[1]) << 8;  /* fallthrough */
        case 1:  k1 ^= ((uint64_t)tail[0]) << 0;
                 k1 *= c1;
                 k1 = rotl64_test(k1, 31);
                 k1 *= c2;
                 h1 ^= k1;
    }

    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64_test(h1);
    h2 = fmix64_test(h2);

    h1 += h2;

    return h1;
}

static uint8_t hll_leading_zeros_test(uint64_t value)
{
    if (value == 0)
        return 64;

#ifdef __GNUC__
    return __builtin_clzll(value);
#else
    uint8_t n = 0;
    if ((value & 0xFFFFFFFF00000000ULL) == 0) { n += 32; value <<= 32; }
    if ((value & 0xFFFF000000000000ULL) == 0) { n += 16; value <<= 16; }
    if ((value & 0xFF00000000000000ULL) == 0) { n +=  8; value <<=  8; }
    if ((value & 0xF000000000000000ULL) == 0) { n +=  4; value <<=  4; }
    if ((value & 0xC000000000000000ULL) == 0) { n +=  2; value <<=  2; }
    if ((value & 0x8000000000000000ULL) == 0) { n +=  1; }
    return n;
#endif
}

static float hll_alpha_test(uint8_t precision)
{
    int m = 1 << precision;

    switch (m)
    {
        case 16:  return 0.673f;
        case 32:  return 0.697f;
        case 64:  return 0.709f;
        default:  return 0.7213f / (1.0f + 1.079f / (float)m);
    }
}

static TestHLL *test_hll_create(uint8_t precision)
{
    if (precision < HLL_PRECISION_MIN) precision = HLL_PRECISION_MIN;
    if (precision > HLL_PRECISION_MAX) precision = HLL_PRECISION_MAX;

    TestHLL *hll = malloc(sizeof(TestHLL));
    hll->precision = precision;
    hll->total_count = 0;
    hll->registers = calloc(HLL_REGISTERS(precision), sizeof(uint8_t));

    return hll;
}

static void test_hll_free(TestHLL *hll)
{
    if (hll)
    {
        free(hll->registers);
        free(hll);
    }
}

static void test_hll_add_hash(TestHLL *hll, uint64_t hash)
{
    uint32_t idx;
    uint8_t rank;

    /* Use top p bits as bucket index */
    idx = hash >> (64 - hll->precision);

    /* Count leading zeros in remaining bits + 1 */
    uint64_t remaining = (hash << hll->precision) | ((uint64_t)1 << (hll->precision - 1));
    rank = hll_leading_zeros_test(remaining) + 1;

    if (rank > hll->registers[idx])
        hll->registers[idx] = rank;

    hll->total_count++;
}

static void test_hll_add(TestHLL *hll, const void *data, size_t len)
{
    uint64_t hash = murmurhash3_64_test(data, len, MURMUR_SEED);
    test_hll_add_hash(hll, hash);
}

static int64_t test_hll_count(TestHLL *hll)
{
    int num_registers = HLL_REGISTERS(hll->precision);
    double alpha = hll_alpha_test(hll->precision);
    double sum = 0.0;
    int empty_registers = 0;

    for (int i = 0; i < num_registers; i++)
    {
        sum += 1.0 / (double)(1ULL << hll->registers[i]);
        if (hll->registers[i] == 0)
            empty_registers++;
    }

    double raw_estimate = alpha * (double)num_registers * (double)num_registers / sum;
    double estimate;

    if (raw_estimate <= 2.5 * num_registers)
    {
        /* Small range correction using linear counting */
        if (empty_registers > 0)
            estimate = (double)num_registers * log((double)num_registers / (double)empty_registers);
        else
            estimate = raw_estimate;
    }
    else if (raw_estimate <= (1.0 / 30.0) * (double)(1ULL << 32))
    {
        estimate = raw_estimate;
    }
    else
    {
        /* Large range correction */
        estimate = -(double)(1ULL << 32) * log(1.0 - raw_estimate / (double)(1ULL << 32));
    }

    return (int64_t)round(estimate);
}

static float test_hll_error_rate(uint8_t precision)
{
    int m = 1 << precision;
    return 1.04f / sqrtf((float)m);
}

/* ----- T-Digest Implementation ----- */

#define TDIGEST_COMPRESSION_DEFAULT 100.0
#define TDIGEST_MAX_CENTROIDS(d)    ((int)ceil((d) * 1.5708))
#define TDIGEST_BUFFER_SIZE         500

typedef struct TestCentroid
{
    double mean;
    double weight;
} TestCentroid;

typedef struct TestTDigest
{
    double          compression;
    int32_t         num_centroids;
    int32_t         max_centroids;
    int64_t         total_weight;
    double          min_value;
    double          max_value;
    int32_t         buffer_count;
    bool            is_sorted;
    TestCentroid   *centroids;
    double         *buffer_values;
    double         *buffer_weights;
} TestTDigest;

static int compare_centroid(const void *a, const void *b)
{
    double ma = ((const TestCentroid *)a)->mean;
    double mb = ((const TestCentroid *)b)->mean;
    if (ma < mb) return -1;
    if (ma > mb) return 1;
    return 0;
}

typedef struct { double value; double weight; } VW;

static int compare_vw(const void *a, const void *b)
{
    double va = ((const VW *)a)->value;
    double vb = ((const VW *)b)->value;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static double scale_k1(double q, double delta)
{
    if (q <= 0.0) return 0.0;
    if (q >= 1.0) return delta;
    return delta * (asin(2.0 * q - 1.0) / M_PI + 0.5);
}

static TestTDigest *test_tdigest_create(double compression)
{
    if (compression < 10.0) compression = 10.0;
    if (compression > 1000.0) compression = 1000.0;

    TestTDigest *td = malloc(sizeof(TestTDigest));
    td->compression = compression;
    td->max_centroids = TDIGEST_MAX_CENTROIDS(compression);
    td->num_centroids = 0;
    td->total_weight = 0;
    td->min_value = DBL_MAX;
    td->max_value = -DBL_MAX;
    td->buffer_count = 0;
    td->is_sorted = true;
    td->centroids = calloc(td->max_centroids, sizeof(TestCentroid));
    td->buffer_values = calloc(TDIGEST_BUFFER_SIZE, sizeof(double));
    td->buffer_weights = calloc(TDIGEST_BUFFER_SIZE, sizeof(double));

    return td;
}

static void test_tdigest_free(TestTDigest *td)
{
    if (td)
    {
        free(td->centroids);
        free(td->buffer_values);
        free(td->buffer_weights);
        free(td);
    }
}

static void test_tdigest_compress(TestTDigest *td)
{
    if (td->buffer_count == 0 && td->is_sorted)
        return;

    int total_count = td->num_centroids + td->buffer_count;
    if (total_count == 0)
        return;

    VW *all_values = malloc(total_count * sizeof(VW));
    int idx = 0;

    for (int i = 0; i < td->num_centroids; i++)
    {
        all_values[idx].value = td->centroids[i].mean;
        all_values[idx].weight = td->centroids[i].weight;
        idx++;
    }

    for (int i = 0; i < td->buffer_count; i++)
    {
        all_values[idx].value = td->buffer_values[i];
        all_values[idx].weight = td->buffer_weights[i];
        idx++;
    }

    qsort(all_values, total_count, sizeof(VW), compare_vw);

    double total_weight = 0.0;
    for (int i = 0; i < total_count; i++)
        total_weight += all_values[i].weight;

    td->num_centroids = 0;
    td->buffer_count = 0;
    td->total_weight = 0;

    double cumulative_weight = 0.0;
    double centroid_mean = all_values[0].value;
    double centroid_weight = all_values[0].weight;
    double k_low = scale_k1(0.0, td->compression);

    for (int i = 1; i < total_count; i++)
    {
        double proposed_weight = centroid_weight + all_values[i].weight;
        double q_high = (cumulative_weight + proposed_weight) / total_weight;
        double k_high = scale_k1(q_high, td->compression);

        if (k_high - k_low <= 1.0 || centroid_weight == 0.0)
        {
            double new_mean = centroid_mean + (all_values[i].value - centroid_mean) *
                              all_values[i].weight / proposed_weight;
            centroid_mean = new_mean;
            centroid_weight = proposed_weight;
        }
        else
        {
            if (td->num_centroids < td->max_centroids)
            {
                td->centroids[td->num_centroids].mean = centroid_mean;
                td->centroids[td->num_centroids].weight = centroid_weight;
                td->num_centroids++;
                td->total_weight += (int64_t)centroid_weight;
            }

            cumulative_weight += centroid_weight;
            k_low = scale_k1(cumulative_weight / total_weight, td->compression);

            centroid_mean = all_values[i].value;
            centroid_weight = all_values[i].weight;
        }
    }

    if (centroid_weight > 0.0 && td->num_centroids < td->max_centroids)
    {
        td->centroids[td->num_centroids].mean = centroid_mean;
        td->centroids[td->num_centroids].weight = centroid_weight;
        td->num_centroids++;
        td->total_weight += (int64_t)centroid_weight;
    }

    td->is_sorted = true;
    free(all_values);
}

static void test_tdigest_add(TestTDigest *td, double value)
{
    if (isnan(value) || isinf(value))
        return;

    if (value < td->min_value) td->min_value = value;
    if (value > td->max_value) td->max_value = value;

    td->buffer_values[td->buffer_count] = value;
    td->buffer_weights[td->buffer_count] = 1.0;
    td->buffer_count++;

    if (td->buffer_count >= TDIGEST_BUFFER_SIZE)
        test_tdigest_compress(td);
}

static void test_tdigest_sort(TestTDigest *td)
{
    if (!td->is_sorted && td->num_centroids > 1)
    {
        qsort(td->centroids, td->num_centroids, sizeof(TestCentroid), compare_centroid);
        td->is_sorted = true;
    }
}

static double test_tdigest_quantile(TestTDigest *td, double q)
{
    test_tdigest_compress(td);
    test_tdigest_sort(td);

    if (td->num_centroids == 0)
        return NAN;

    if (q <= 0.0) return td->min_value;
    if (q >= 1.0) return td->max_value;

    double total_weight = (double)td->total_weight;
    if (total_weight == 0.0) return NAN;

    double target_weight = q * total_weight;
    double cumulative_weight = 0.0;

    if (td->num_centroids == 1)
        return td->centroids[0].mean;

    for (int i = 0; i < td->num_centroids; i++)
    {
        double next_weight = cumulative_weight + td->centroids[i].weight;

        if (target_weight < next_weight)
        {
            double weight_in_centroid = target_weight - cumulative_weight;
            double fraction = weight_in_centroid / td->centroids[i].weight;

            double prev_mean = (i == 0) ? td->min_value : td->centroids[i - 1].mean;
            double next_mean = (i == td->num_centroids - 1) ? td->max_value : td->centroids[i + 1].mean;

            double delta_prev = td->centroids[i].mean - prev_mean;
            double delta_next = next_mean - td->centroids[i].mean;

            if (fraction < 0.5)
                return td->centroids[i].mean - delta_prev * (0.5 - fraction);
            else
                return td->centroids[i].mean + delta_next * (fraction - 0.5);
        }

        cumulative_weight = next_weight;
    }

    return td->max_value;
}

/* ----- Sampling Implementation (xoshiro256++) ----- */

typedef struct TestRNG
{
    uint64_t s[4];
    bool initialized;
} TestRNG;

static TestRNG test_rng = {.initialized = false};

static inline uint64_t rotl_rng(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static void test_rng_init(uint64_t seed)
{
    uint64_t z = seed;
    for (int i = 0; i < 4; i++)
    {
        z += 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        test_rng.s[i] = z ^ (z >> 31);
    }
    test_rng.initialized = true;
}

static uint64_t test_rng_next(void)
{
    if (!test_rng.initialized)
        test_rng_init((uint64_t)time(NULL));

    uint64_t result = rotl_rng(test_rng.s[0] + test_rng.s[3], 23) + test_rng.s[0];
    uint64_t t = test_rng.s[1] << 17;

    test_rng.s[2] ^= test_rng.s[0];
    test_rng.s[3] ^= test_rng.s[1];
    test_rng.s[1] ^= test_rng.s[2];
    test_rng.s[0] ^= test_rng.s[3];

    test_rng.s[2] ^= t;
    test_rng.s[3] = rotl_rng(test_rng.s[3], 45);

    return result;
}

static double test_random_double(void)
{
    return (test_rng_next() >> 11) * (1.0 / (1ULL << 53));
}

static int64_t test_random_int(int64_t n)
{
    if (n <= 0) return 0;
    uint64_t threshold = (UINT64_MAX - n + 1) % n;
    uint64_t r;
    do {
        r = test_rng_next();
    } while (r < threshold);
    return r % n;
}

/* ============================================================
 * Test Cases
 * ============================================================ */

static void test_hll_basic(void)
{
    TEST_BEGIN("hll_creation_and_basic_ops")
        TestHLL *hll = test_hll_create(14);
        TEST_ASSERT_NOT_NULL(hll);
        TEST_ASSERT_EQ(14, hll->precision);
        TEST_ASSERT_EQ(0, test_hll_count(hll));

        test_hll_add(hll, "hello", 5);
        TEST_ASSERT_EQ(1, hll->total_count);

        test_hll_add(hll, "world", 5);
        TEST_ASSERT_EQ(2, hll->total_count);

        test_hll_free(hll);
    TEST_END();
}

static void test_hll_accuracy(void)
{
    int test_sizes[] = {100, 1000, 10000, 100000};
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int t = 0; t < num_tests; t++)
    {
        TEST_BEGIN("hll_cardinality_accuracy")
            int actual_count = test_sizes[t];
            TestHLL *hll = test_hll_create(14);

            for (int i = 0; i < actual_count; i++)
            {
                test_hll_add(hll, &i, sizeof(i));
            }

            int64_t estimated = test_hll_count(hll);
            double error = fabs((double)(estimated - actual_count)) / (double)actual_count;
            double expected_error = test_hll_error_rate(14);
            double tolerance = 3.0 * expected_error;

            TEST_ASSERT_MSG(error <= tolerance, "HLL error within tolerance");

            test_hll_free(hll);
        TEST_END();
    }
}

static void test_hll_duplicates(void)
{
    TEST_BEGIN("hll_duplicate_handling")
        TestHLL *hll = test_hll_create(14);

        for (int i = 0; i < 10000; i++)
        {
            int val = 42;
            test_hll_add(hll, &val, sizeof(val));
        }

        int64_t count = test_hll_count(hll);
        TEST_ASSERT_MSG(count <= 5, "HLL handles duplicates correctly");

        test_hll_free(hll);
    TEST_END();
}

static void test_hll_merge(void)
{
    TEST_BEGIN("hll_merge_disjoint_sets")
        TestHLL *hll1 = test_hll_create(14);
        TestHLL *hll2 = test_hll_create(14);

        for (int i = 0; i < 5000; i++)
        {
            test_hll_add(hll1, &i, sizeof(i));
        }

        for (int i = 5000; i < 10000; i++)
        {
            test_hll_add(hll2, &i, sizeof(i));
        }

        TestHLL *merged = test_hll_create(14);
        int num_regs = HLL_REGISTERS(14);
        for (int i = 0; i < num_regs; i++)
        {
            merged->registers[i] = (hll1->registers[i] > hll2->registers[i]) ?
                                   hll1->registers[i] : hll2->registers[i];
        }

        int64_t count = test_hll_count(merged);
        double error = fabs((double)(count - 10000)) / 10000.0;

        TEST_ASSERT_MSG(error <= 0.05, "Merged HLL accuracy within 5%");

        test_hll_free(hll1);
        test_hll_free(hll2);
        test_hll_free(merged);
    TEST_END();
}

static void test_hll_precision_levels(void)
{
    uint8_t precisions[] = {4, 8, 12, 14, 16};
    int num_precisions = sizeof(precisions) / sizeof(precisions[0]);
    int test_count = 10000;

    for (int p = 0; p < num_precisions; p++)
    {
        TEST_BEGIN("hll_precision_level")
            uint8_t precision = precisions[p];
            TestHLL *hll = test_hll_create(precision);

            for (int i = 0; i < test_count; i++)
            {
                test_hll_add(hll, &i, sizeof(i));
            }

            int64_t estimated = test_hll_count(hll);
            double error = fabs((double)(estimated - test_count)) / test_count;
            double expected_error = test_hll_error_rate(precision);

            TEST_ASSERT_MSG(error <= 5.0 * expected_error, "HLL precision error within bounds");

            test_hll_free(hll);
        TEST_END();
    }
}

static void test_tdigest_basic(void)
{
    TEST_BEGIN("tdigest_creation_and_basic_ops")
        TestTDigest *td = test_tdigest_create(100);
        TEST_ASSERT_NOT_NULL(td);
        TEST_ASSERT_NEAR(100.0, td->compression, 0.001);

        test_tdigest_add(td, 1.0);
        test_tdigest_add(td, 2.0);
        test_tdigest_add(td, 3.0);

        test_tdigest_compress(td);
        TEST_ASSERT_EQ(3, td->total_weight);

        TEST_ASSERT_NEAR(1.0, td->min_value, 0.001);
        TEST_ASSERT_NEAR(3.0, td->max_value, 0.001);

        test_tdigest_free(td);
    TEST_END();
}

static void test_tdigest_quantile_accuracy(void)
{
    TEST_BEGIN("tdigest_percentile_accuracy")
        TestTDigest *td = test_tdigest_create(100);

        for (int i = 0; i < 10000; i++)
        {
            test_tdigest_add(td, (double)i / 10.0);
        }

        double test_quantiles[] = {0.01, 0.05, 0.25, 0.50, 0.75, 0.95, 0.99};
        int num_q = sizeof(test_quantiles) / sizeof(test_quantiles[0]);

        bool all_accurate = true;
        for (int i = 0; i < num_q; i++)
        {
            double q = test_quantiles[i];
            double expected = q * 1000.0;
            double actual = test_tdigest_quantile(td, q);

            double tolerance;
            if (q <= 0.05 || q >= 0.95)
                tolerance = 20.0;
            else
                tolerance = 50.0;

            if (fabs(actual - expected) > tolerance)
                all_accurate = false;
        }

        TEST_ASSERT_MSG(all_accurate, "T-Digest quantiles within tolerance");

        test_tdigest_free(td);
    TEST_END();
}

static void test_tdigest_merge(void)
{
    TEST_BEGIN("tdigest_merge_disjoint_ranges")
        TestTDigest *td1 = test_tdigest_create(100);
        TestTDigest *td2 = test_tdigest_create(100);

        for (int i = 0; i < 5000; i++)
        {
            test_tdigest_add(td1, (double)i);
        }

        for (int i = 5000; i < 10000; i++)
        {
            test_tdigest_add(td2, (double)i);
        }

        TestTDigest *merged = test_tdigest_create(100);
        test_tdigest_compress(td1);
        test_tdigest_compress(td2);

        for (int i = 0; i < td1->num_centroids; i++)
        {
            for (int j = 0; j < (int)td1->centroids[i].weight; j++)
                test_tdigest_add(merged, td1->centroids[i].mean);
        }
        for (int i = 0; i < td2->num_centroids; i++)
        {
            for (int j = 0; j < (int)td2->centroids[i].weight; j++)
                test_tdigest_add(merged, td2->centroids[i].mean);
        }

        double median = test_tdigest_quantile(merged, 0.5);
        TEST_ASSERT_NEAR(5000.0, median, 300.0);

        test_tdigest_free(td1);
        test_tdigest_free(td2);
        test_tdigest_free(merged);
    TEST_END();
}

static void test_tdigest_edge_cases(void)
{
    TEST_BEGIN("tdigest_single_value")
        TestTDigest *td = test_tdigest_create(100);
        test_tdigest_add(td, 42.0);
        double q50 = test_tdigest_quantile(td, 0.5);
        TEST_ASSERT_NEAR(42.0, q50, 0.001);
        test_tdigest_free(td);
    TEST_END();

    TEST_BEGIN("tdigest_two_values")
        TestTDigest *td = test_tdigest_create(100);
        test_tdigest_add(td, 42.0);
        test_tdigest_add(td, 100.0);
        double q50 = test_tdigest_quantile(td, 0.5);
        TEST_ASSERT_MSG(q50 >= 42.0 && q50 <= 100.0, "Two values: median between values");
        test_tdigest_free(td);
    TEST_END();

    TEST_BEGIN("tdigest_empty_returns_nan")
        TestTDigest *td = test_tdigest_create(100);
        double empty_q = test_tdigest_quantile(td, 0.5);
        TEST_ASSERT_MSG(isnan(empty_q), "Empty T-Digest quantile returns NaN");
        test_tdigest_free(td);
    TEST_END();
}

static void test_tdigest_compression_levels(void)
{
    double compressions[] = {25.0, 50.0, 100.0, 200.0};
    int num_compressions = sizeof(compressions) / sizeof(compressions[0]);
    int test_count = 10000;

    for (int c = 0; c < num_compressions; c++)
    {
        TEST_BEGIN("tdigest_compression_level")
            TestTDigest *td = test_tdigest_create(compressions[c]);

            for (int i = 0; i < test_count; i++)
            {
                test_tdigest_add(td, (double)i);
            }

            double expected_median = (test_count - 1) / 2.0;
            double actual_median = test_tdigest_quantile(td, 0.5);
            double error = fabs(actual_median - expected_median) / test_count;

            TEST_ASSERT_MSG(error <= 0.05, "T-Digest compression median within 5%");

            test_tdigest_free(td);
        TEST_END();
    }
}

static void test_sampling_uniformity(void)
{
    TEST_BEGIN("sampling_uniformity")
        test_rng_init(12345);

        int num_samples = 100000;
        int buckets[10] = {0};

        for (int i = 0; i < num_samples; i++)
        {
            double r = test_random_double();
            int bucket = (int)(r * 10);
            if (bucket >= 10) bucket = 9;
            buckets[bucket]++;
        }

        double expected = num_samples / 10.0;
        bool uniform = true;

        for (int i = 0; i < 10; i++)
        {
            double ratio = (double)buckets[i] / expected;
            if (ratio < 0.90 || ratio > 1.10)
                uniform = false;
        }

        TEST_ASSERT_MSG(uniform, "Random double distribution is uniform across buckets");
    TEST_END();
}

static void test_sampling_random_int(void)
{
    TEST_BEGIN("sampling_random_int_uniformity")
        test_rng_init(54321);

        int n = 6;
        int counts[6] = {0};
        int num_samples = 60000;

        for (int i = 0; i < num_samples; i++)
        {
            int64_t r = test_random_int(n);
            if (r >= 0 && r < n)
                counts[r]++;
        }

        double expected = num_samples / (double)n;
        bool uniform = true;

        for (int i = 0; i < n; i++)
        {
            double ratio = (double)counts[i] / expected;
            if (ratio < 0.90 || ratio > 1.10)
                uniform = false;
        }

        TEST_ASSERT_MSG(uniform, "Random int distribution is uniform");
    TEST_END();

    TEST_BEGIN("sampling_random_int_edge_cases")
        TEST_ASSERT_EQ(0, test_random_int(0));
        TEST_ASSERT_EQ(0, test_random_int(1));
    TEST_END();
}

static void test_reservoir_sampling(void)
{
    TEST_BEGIN("reservoir_sampling_coverage")
        test_rng_init(98765);

        int reservoir_size = 100;
        int stream_size = 10000;
        int *reservoir = malloc(reservoir_size * sizeof(int));

        for (int i = 0; i < stream_size; i++)
        {
            if (i < reservoir_size)
            {
                reservoir[i] = i;
            }
            else
            {
                int64_t j = test_random_int(i + 1);
                if (j < reservoir_size)
                    reservoir[j] = i;
            }
        }

        int early = 0, middle = 0, late = 0;
        for (int i = 0; i < reservoir_size; i++)
        {
            if (reservoir[i] < stream_size / 3) early++;
            else if (reservoir[i] < 2 * stream_size / 3) middle++;
            else late++;
        }

        bool balanced = (early >= 20 && early <= 50) &&
                        (middle >= 20 && middle <= 50) &&
                        (late >= 20 && late <= 50);

        TEST_ASSERT_MSG(balanced, "Reservoir sampling covers all stream regions");

        free(reservoir);
    TEST_END();
}

static void test_bernoulli_sampling(void)
{
    TEST_BEGIN("bernoulli_sampling_rate")
        test_rng_init(11111);

        double probability = 0.1;
        int num_trials = 100000;
        int selected = 0;

        for (int i = 0; i < num_trials; i++)
        {
            if (test_random_double() < probability)
                selected++;
        }

        double actual_rate = (double)selected / num_trials;
        double error = fabs(actual_rate - probability);

        TEST_ASSERT_MSG(error < 0.01, "Bernoulli sampling rate within 1%");
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_approx(void)
{
    /* HyperLogLog Tests */
    test_hll_basic();
    test_hll_accuracy();
    test_hll_duplicates();
    test_hll_merge();
    test_hll_precision_levels();

    /* T-Digest Tests */
    test_tdigest_basic();
    test_tdigest_quantile_accuracy();
    test_tdigest_merge();
    test_tdigest_edge_cases();
    test_tdigest_compression_levels();

    /* Sampling Tests */
    test_sampling_uniformity();
    test_sampling_random_int();
    test_reservoir_sampling();
    test_bernoulli_sampling();
}
