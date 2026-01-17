/*-------------------------------------------------------------------------
 *
 * test_vectorized.c
 *    Standalone unit tests for Orochi DB vectorized execution engine
 *
 * This test file provides comprehensive testing of the vectorized
 * execution implementation WITHOUT requiring PostgreSQL dependencies.
 *
 * Tests cover:
 *   - VectorBatch creation and destruction
 *   - Vectorized filter operations (int64, float64)
 *   - Vectorized aggregations (SUM, COUNT, MIN, MAX)
 *   - Selection vector logic
 *
 * Build: gcc -o test_vectorized test_vectorized.c -lm -mavx2 (if AVX2 available)
 * Run:   ./test_vectorized
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <float.h>
#include <assert.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* ============================================================
 * Test Framework
 * ============================================================ */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg) do { \
    tests_run++; \
    if (!(condition)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
        return false; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) != (actual)) { \
        printf("  FAIL: %s - expected %ld, got %ld (line %d)\n", \
               msg, (long)(expected), (long)(actual), __LINE__); \
        tests_failed++; \
        return false; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_FLOAT_EQ(expected, actual, epsilon, msg) do { \
    tests_run++; \
    if (fabs((expected) - (actual)) > (epsilon)) { \
        printf("  FAIL: %s - expected %f, got %f (line %d)\n", \
               msg, (double)(expected), (double)(actual), __LINE__); \
        tests_failed++; \
        return false; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    printf("Running %s...\n", #test_func); \
    if (test_func()) { \
        printf("  PASSED\n"); \
    } else { \
        printf("  FAILED\n"); \
    } \
} while(0)

/* ============================================================
 * Mock PostgreSQL Types (Standalone Version)
 * ============================================================ */

/* Basic types */
typedef unsigned int Oid;
typedef uintptr_t Datum;
typedef size_t Size;

/* Memory context mock */
typedef struct MemoryContextData *MemoryContext;
struct MemoryContextData {
    char *name;
    void **allocations;
    int alloc_count;
    int alloc_capacity;
};

static MemoryContext current_context = NULL;

/* Simple memory tracking for tests */
static void *tracked_allocs[1024];
static int tracked_alloc_count = 0;

/* ============================================================
 * Mock PostgreSQL Functions
 * ============================================================ */

static void *
palloc(Size size)
{
    void *ptr = malloc(size);
    if (ptr && tracked_alloc_count < 1024) {
        tracked_allocs[tracked_alloc_count++] = ptr;
    }
    return ptr;
}

static void *
palloc0(Size size)
{
    void *ptr = calloc(1, size);
    if (ptr && tracked_alloc_count < 1024) {
        tracked_allocs[tracked_alloc_count++] = ptr;
    }
    return ptr;
}

static void *
repalloc(void *ptr, Size size)
{
    void *new_ptr = realloc(ptr, size);
    /* Update tracking */
    for (int i = 0; i < tracked_alloc_count; i++) {
        if (tracked_allocs[i] == ptr) {
            tracked_allocs[i] = new_ptr;
            break;
        }
    }
    return new_ptr;
}

static void
pfree(void *ptr)
{
    free(ptr);
    /* Remove from tracking */
    for (int i = 0; i < tracked_alloc_count; i++) {
        if (tracked_allocs[i] == ptr) {
            tracked_allocs[i] = tracked_allocs[--tracked_alloc_count];
            break;
        }
    }
}

static MemoryContext
AllocSetContextCreate(MemoryContext parent, const char *name, int flags)
{
    (void)parent;
    (void)flags;
    MemoryContext ctx = malloc(sizeof(struct MemoryContextData));
    ctx->name = strdup(name);
    ctx->allocations = malloc(sizeof(void*) * 256);
    ctx->alloc_count = 0;
    ctx->alloc_capacity = 256;
    return ctx;
}

static MemoryContext
MemoryContextSwitchTo(MemoryContext ctx)
{
    MemoryContext old = current_context;
    current_context = ctx;
    return old;
}

static void
MemoryContextDelete(MemoryContext ctx)
{
    if (ctx) {
        free(ctx->name);
        free(ctx->allocations);
        free(ctx);
    }
}

#define ALLOCSET_DEFAULT_SIZES 0
#define CurrentMemoryContext current_context

/* Error handling mock */
#define ereport(level, rest) do { printf("ERROR: "); rest; exit(1); } while(0)
#define errcode(c) printf("code=%d ", c)
#define errmsg(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define ERRCODE_INVALID_PARAMETER_VALUE 1
#define ERRCODE_FEATURE_NOT_SUPPORTED 2
#define ERROR 0

/* Type OIDs */
#define INT2OID 21
#define INT4OID 23
#define INT8OID 20
#define FLOAT4OID 700
#define FLOAT8OID 701
#define BOOLOID 16
#define TEXTOID 25
#define VARCHAROID 1043
#define BPCHAROID 1042
#define TIMESTAMPOID 1114
#define TIMESTAMPTZOID 1184
#define DATEOID 1082

/* Datum accessors */
#define DatumGetInt32(d) ((int32_t)(d))
#define DatumGetInt64(d) ((int64_t)(d))
#define DatumGetFloat4(d) (*((float*)&(d)))
#define DatumGetFloat8(d) (*((double*)&(d)))
#define DatumGetBool(d) ((bool)(d))

#define Int32GetDatum(i) ((Datum)(i))
#define Int64GetDatum(i) ((Datum)(i))
#define Float4GetDatum(f) (*(Datum*)&(f))
#define Float8GetDatum(d) (*(Datum*)&(d))
#define BoolGetDatum(b) ((Datum)(b))

/* ============================================================
 * Vectorized Types (Copied from vectorized.h for standalone testing)
 * ============================================================ */

#define VECTORIZED_ALIGNMENT 32
#define VECTORIZED_BATCH_SIZE 1024
#define VECTORIZED_MAX_COLUMNS 256
#define VECTORIZED_INT64_LANES 4
#define VECTORIZED_INT32_LANES 8
#define VECTORIZED_FLOAT64_LANES 4
#define VECTORIZED_FLOAT32_LANES 8

typedef enum VectorizedCompareOp {
    VECTORIZED_CMP_EQ,
    VECTORIZED_CMP_NE,
    VECTORIZED_CMP_LT,
    VECTORIZED_CMP_LE,
    VECTORIZED_CMP_GT,
    VECTORIZED_CMP_GE,
    VECTORIZED_CMP_IS_NULL,
    VECTORIZED_CMP_IS_NOT_NULL
} VectorizedCompareOp;

typedef enum VectorizedAggType {
    VECTORIZED_AGG_COUNT,
    VECTORIZED_AGG_SUM,
    VECTORIZED_AGG_AVG,
    VECTORIZED_AGG_MIN,
    VECTORIZED_AGG_MAX
} VectorizedAggType;

typedef enum VectorizedDataType {
    VECTORIZED_TYPE_INT32,
    VECTORIZED_TYPE_INT64,
    VECTORIZED_TYPE_FLOAT32,
    VECTORIZED_TYPE_FLOAT64,
    VECTORIZED_TYPE_BOOL,
    VECTORIZED_TYPE_STRING,
    VECTORIZED_TYPE_TIMESTAMP,
    VECTORIZED_TYPE_DATE,
    VECTORIZED_TYPE_UNKNOWN
} VectorizedDataType;

typedef struct VectorColumn {
    VectorizedDataType data_type;
    int32_t type_len;
    bool type_byval;
    Oid type_oid;
    void *data;
    uint64_t *nulls;
    bool has_nulls;
    int32_t *offsets;
    int64_t data_capacity;
} VectorColumn;

typedef struct VectorBatch {
    int32_t capacity;
    int32_t count;
    int32_t column_count;
    VectorColumn **columns;
    uint32_t *selection;
    int32_t selected_count;
    bool has_selection;
    MemoryContext batch_context;
    int64_t rows_processed;
    int64_t rows_filtered;
} VectorBatch;

/* ============================================================
 * Aligned Memory Allocation
 * ============================================================ */

static void *
vectorized_palloc_aligned(Size size, Size alignment)
{
    void *ptr;
    void *aligned_ptr;
    Size total_size;

    total_size = size + alignment + sizeof(void *);
    ptr = palloc(total_size);

    aligned_ptr = (void *)(((uintptr_t)ptr + sizeof(void *) + alignment - 1) &
                           ~(alignment - 1));
    ((void **)aligned_ptr)[-1] = ptr;

    return aligned_ptr;
}

static void
vectorized_pfree_aligned(void *aligned_ptr)
{
    if (aligned_ptr != NULL) {
        void *original_ptr = ((void **)aligned_ptr)[-1];
        pfree(original_ptr);
    }
}

/* ============================================================
 * Batch Creation and Management
 * ============================================================ */

static VectorBatch *
vector_batch_create(int32_t capacity, int32_t column_count)
{
    VectorBatch *batch;
    MemoryContext batch_context;
    MemoryContext old_context;

    if (capacity < 1)
        capacity = VECTORIZED_BATCH_SIZE;

    if (column_count < 1 || column_count > VECTORIZED_MAX_COLUMNS) {
        fprintf(stderr, "Invalid column count: %d\n", column_count);
        return NULL;
    }

    batch_context = AllocSetContextCreate(CurrentMemoryContext,
                                          "VectorBatchContext",
                                          ALLOCSET_DEFAULT_SIZES);

    old_context = MemoryContextSwitchTo(batch_context);

    batch = palloc0(sizeof(VectorBatch));
    batch->capacity = capacity;
    batch->count = 0;
    batch->column_count = column_count;
    batch->batch_context = batch_context;

    batch->columns = palloc0(column_count * sizeof(VectorColumn *));

    batch->selection = vectorized_palloc_aligned(capacity * sizeof(uint32_t),
                                                  VECTORIZED_ALIGNMENT);
    batch->selected_count = 0;
    batch->has_selection = false;

    batch->rows_processed = 0;
    batch->rows_filtered = 0;

    MemoryContextSwitchTo(old_context);

    return batch;
}

static void
vector_batch_free(VectorBatch *batch)
{
    if (batch == NULL)
        return;

    /* Free columns */
    for (int i = 0; i < batch->column_count; i++) {
        if (batch->columns[i] != NULL) {
            if (batch->columns[i]->data)
                vectorized_pfree_aligned(batch->columns[i]->data);
            if (batch->columns[i]->nulls)
                pfree(batch->columns[i]->nulls);
            if (batch->columns[i]->offsets)
                pfree(batch->columns[i]->offsets);
            pfree(batch->columns[i]);
        }
    }
    pfree(batch->columns);
    vectorized_pfree_aligned(batch->selection);
    MemoryContextDelete(batch->batch_context);
    pfree(batch);
}

static void
vector_batch_reset(VectorBatch *batch)
{
    if (batch == NULL)
        return;

    batch->count = 0;
    batch->selected_count = 0;
    batch->has_selection = false;

    for (int i = 0; i < batch->column_count; i++) {
        if (batch->columns[i] != NULL) {
            batch->columns[i]->has_nulls = false;
            if (batch->columns[i]->nulls) {
                memset(batch->columns[i]->nulls, 0,
                       ((batch->capacity + 63) / 64) * sizeof(uint64_t));
            }
        }
    }
}

static VectorColumn *
vector_batch_add_column(VectorBatch *batch, int32_t column_index,
                        VectorizedDataType data_type, Oid type_oid)
{
    VectorColumn *column;
    Size data_size;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count) {
        fprintf(stderr, "Invalid column index: %d\n", column_index);
        return NULL;
    }

    column = palloc0(sizeof(VectorColumn));
    column->data_type = data_type;
    column->type_oid = type_oid;

    switch (data_type) {
        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            column->type_len = sizeof(int32_t);
            column->type_byval = true;
            data_size = batch->capacity * sizeof(int32_t);
            break;

        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            column->type_len = sizeof(int64_t);
            column->type_byval = true;
            data_size = batch->capacity * sizeof(int64_t);
            break;

        case VECTORIZED_TYPE_FLOAT32:
            column->type_len = sizeof(float);
            column->type_byval = true;
            data_size = batch->capacity * sizeof(float);
            break;

        case VECTORIZED_TYPE_FLOAT64:
            column->type_len = sizeof(double);
            column->type_byval = true;
            data_size = batch->capacity * sizeof(double);
            break;

        case VECTORIZED_TYPE_BOOL:
            column->type_len = sizeof(bool);
            column->type_byval = true;
            data_size = batch->capacity * sizeof(bool);
            break;

        default:
            column->type_len = sizeof(Datum);
            column->type_byval = false;
            data_size = batch->capacity * sizeof(Datum);
            break;
    }

    column->data = vectorized_palloc_aligned(data_size, VECTORIZED_ALIGNMENT);
    memset(column->data, 0, data_size);

    column->nulls = palloc0(((batch->capacity + 63) / 64) * sizeof(uint64_t));
    column->has_nulls = false;

    batch->columns[column_index] = column;

    return column;
}

/* ============================================================
 * Selection Vector Management
 * ============================================================ */

static void
init_full_selection(VectorBatch *batch)
{
    for (int32_t i = 0; i < batch->count; i++)
        batch->selection[i] = i;

    batch->selected_count = batch->count;
    batch->has_selection = true;
}

static void
compact_selection(VectorBatch *batch, bool *mask)
{
    int32_t write_pos = 0;

    for (int32_t i = 0; i < batch->selected_count; i++) {
        if (mask[batch->selection[i]]) {
            batch->selection[write_pos++] = batch->selection[i];
        }
    }

    batch->rows_filtered += (batch->selected_count - write_pos);
    batch->selected_count = write_pos;
}

/* ============================================================
 * Filter Implementations
 * ============================================================ */

static void
vectorized_filter_int64(VectorBatch *batch, int32_t column_index,
                        VectorizedCompareOp op, int64_t value)
{
    VectorColumn *column;
    int64_t *data;
    bool *mask;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return;

    column = batch->columns[column_index];
    if (column == NULL)
        return;

    data = (int64_t *)column->data;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));
    memset(mask, 0, batch->capacity * sizeof(bool));

#ifdef __AVX2__
    {
        __m256i val_vec = _mm256_set1_epi64x(value);

        for (int32_t i = 0; i + VECTORIZED_INT64_LANES <= batch->count; i += VECTORIZED_INT64_LANES) {
            __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
            __m256i cmp_result;

            switch (op) {
                case VECTORIZED_CMP_EQ:
                    cmp_result = _mm256_cmpeq_epi64(data_vec, val_vec);
                    break;
                case VECTORIZED_CMP_GT:
                    cmp_result = _mm256_cmpgt_epi64(data_vec, val_vec);
                    break;
                case VECTORIZED_CMP_LT:
                    cmp_result = _mm256_cmpgt_epi64(val_vec, data_vec);
                    break;
                case VECTORIZED_CMP_GE:
                    cmp_result = _mm256_cmpgt_epi64(val_vec, data_vec);
                    cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
                    break;
                case VECTORIZED_CMP_LE:
                    cmp_result = _mm256_cmpgt_epi64(data_vec, val_vec);
                    cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
                    break;
                case VECTORIZED_CMP_NE:
                    cmp_result = _mm256_cmpeq_epi64(data_vec, val_vec);
                    cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
                    break;
                default:
                    cmp_result = _mm256_setzero_si256();
                    break;
            }

            int64_t results[4];
            _mm256_storeu_si256((__m256i *)results, cmp_result);
            mask[i] = (results[0] != 0);
            mask[i + 1] = (results[1] != 0);
            mask[i + 2] = (results[2] != 0);
            mask[i + 3] = (results[3] != 0);
        }
    }

    /* Handle remaining elements after SIMD */
    int32_t simd_processed = (batch->count / VECTORIZED_INT64_LANES) * VECTORIZED_INT64_LANES;
    for (int32_t i = simd_processed; i < batch->count; i++) {
#else
    for (int32_t i = 0; i < batch->count; i++) {
#endif
        if (column->has_nulls &&
            (column->nulls[i / 64] & (1ULL << (i % 64)))) {
            mask[i] = false;
            continue;
        }

        switch (op) {
            case VECTORIZED_CMP_EQ:
                mask[i] = (data[i] == value);
                break;
            case VECTORIZED_CMP_NE:
                mask[i] = (data[i] != value);
                break;
            case VECTORIZED_CMP_LT:
                mask[i] = (data[i] < value);
                break;
            case VECTORIZED_CMP_LE:
                mask[i] = (data[i] <= value);
                break;
            case VECTORIZED_CMP_GT:
                mask[i] = (data[i] > value);
                break;
            case VECTORIZED_CMP_GE:
                mask[i] = (data[i] >= value);
                break;
            default:
                mask[i] = false;
                break;
        }
    }

    /* Handle null values in SIMD region */
    if (column->has_nulls) {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

static void
vectorized_filter_float64(VectorBatch *batch, int32_t column_index,
                          VectorizedCompareOp op, double value)
{
    VectorColumn *column;
    double *data;
    bool *mask;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return;

    column = batch->columns[column_index];
    if (column == NULL)
        return;

    data = (double *)column->data;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));
    memset(mask, 0, batch->capacity * sizeof(bool));

#ifdef __AVX2__
    {
        __m256d val_vec = _mm256_set1_pd(value);

        for (int32_t i = 0; i + VECTORIZED_FLOAT64_LANES <= batch->count; i += VECTORIZED_FLOAT64_LANES) {
            __m256d data_vec = _mm256_loadu_pd(data + i);
            __m256d cmp_result;

            switch (op) {
                case VECTORIZED_CMP_EQ:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_EQ_OQ);
                    break;
                case VECTORIZED_CMP_NE:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_NEQ_OQ);
                    break;
                case VECTORIZED_CMP_LT:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_LT_OQ);
                    break;
                case VECTORIZED_CMP_LE:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_LE_OQ);
                    break;
                case VECTORIZED_CMP_GT:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_GT_OQ);
                    break;
                case VECTORIZED_CMP_GE:
                    cmp_result = _mm256_cmp_pd(data_vec, val_vec, _CMP_GE_OQ);
                    break;
                default:
                    cmp_result = _mm256_setzero_pd();
                    break;
            }

            int64_t results[4];
            _mm256_storeu_si256((__m256i *)results, _mm256_castpd_si256(cmp_result));
            mask[i] = (results[0] != 0);
            mask[i + 1] = (results[1] != 0);
            mask[i + 2] = (results[2] != 0);
            mask[i + 3] = (results[3] != 0);
        }
    }

    int32_t simd_processed = (batch->count / VECTORIZED_FLOAT64_LANES) * VECTORIZED_FLOAT64_LANES;
    for (int32_t i = simd_processed; i < batch->count; i++) {
#else
    for (int32_t i = 0; i < batch->count; i++) {
#endif
        if (column->has_nulls &&
            (column->nulls[i / 64] & (1ULL << (i % 64)))) {
            mask[i] = false;
            continue;
        }

        switch (op) {
            case VECTORIZED_CMP_EQ:
                mask[i] = (data[i] == value);
                break;
            case VECTORIZED_CMP_NE:
                mask[i] = (data[i] != value);
                break;
            case VECTORIZED_CMP_LT:
                mask[i] = (data[i] < value);
                break;
            case VECTORIZED_CMP_LE:
                mask[i] = (data[i] <= value);
                break;
            case VECTORIZED_CMP_GT:
                mask[i] = (data[i] > value);
                break;
            case VECTORIZED_CMP_GE:
                mask[i] = (data[i] >= value);
                break;
            default:
                mask[i] = false;
                break;
        }
    }

    if (column->has_nulls) {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

/* ============================================================
 * Aggregation Implementations
 * ============================================================ */

static int64_t
vectorized_sum_int64(VectorBatch *batch, int32_t column_index)
{
    VectorColumn *column;
    int64_t *data;
    int64_t sum = 0;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            sum += data[idx];
        }
    } else {
#ifdef __AVX2__
        __m256i sum_vec = _mm256_setzero_si256();

        for (int32_t i = 0; i + VECTORIZED_INT64_LANES <= batch->count; i += VECTORIZED_INT64_LANES) {
            __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
            sum_vec = _mm256_add_epi64(sum_vec, data_vec);
        }

        int64_t partial[4];
        _mm256_storeu_si256((__m256i *)partial, sum_vec);
        sum = partial[0] + partial[1] + partial[2] + partial[3];

        /* Subtract nulls that were included */
        if (column->has_nulls) {
            int32_t simd_end = (batch->count / VECTORIZED_INT64_LANES) * VECTORIZED_INT64_LANES;
            for (int32_t i = 0; i < simd_end; i++) {
                if (column->nulls[i / 64] & (1ULL << (i % 64)))
                    sum -= data[i];
            }
        }

        /* Handle remaining */
        for (int32_t i = (batch->count / VECTORIZED_INT64_LANES) * VECTORIZED_INT64_LANES;
             i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
#else
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
#endif
    }

    return sum;
}

static double
vectorized_sum_float64(VectorBatch *batch, int32_t column_index)
{
    VectorColumn *column;
    double *data;
    double sum = 0.0;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0.0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            sum += data[idx];
        }
    } else {
#ifdef __AVX2__
        __m256d sum_vec = _mm256_setzero_pd();

        for (int32_t i = 0; i + VECTORIZED_FLOAT64_LANES <= batch->count; i += VECTORIZED_FLOAT64_LANES) {
            __m256d data_vec = _mm256_loadu_pd(data + i);
            sum_vec = _mm256_add_pd(sum_vec, data_vec);
        }

        __m128d hi = _mm256_extractf128_pd(sum_vec, 1);
        __m128d lo = _mm256_castpd256_pd128(sum_vec);
        __m128d sum128 = _mm_add_pd(hi, lo);
        sum128 = _mm_hadd_pd(sum128, sum128);
        sum = _mm_cvtsd_f64(sum128);

        /* Subtract nulls */
        if (column->has_nulls) {
            int32_t simd_end = (batch->count / VECTORIZED_FLOAT64_LANES) * VECTORIZED_FLOAT64_LANES;
            for (int32_t i = 0; i < simd_end; i++) {
                if (column->nulls[i / 64] & (1ULL << (i % 64)))
                    sum -= data[i];
            }
        }

        /* Handle remaining */
        for (int32_t i = (batch->count / VECTORIZED_FLOAT64_LANES) * VECTORIZED_FLOAT64_LANES;
             i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
#else
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
#endif
    }

    return sum;
}

static int64_t
vectorized_count(VectorBatch *batch)
{
    if (batch == NULL)
        return 0;

    if (batch->has_selection)
        return batch->selected_count;
    else
        return batch->count;
}

static int64_t
vectorized_count_column(VectorBatch *batch, int32_t column_index)
{
    VectorColumn *column;
    int64_t count = 0;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    if (!column->has_nulls)
        return batch->has_selection ? batch->selected_count : batch->count;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (!(column->nulls[idx / 64] & (1ULL << (idx % 64))))
                count++;
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (!(column->nulls[i / 64] & (1ULL << (i % 64))))
                count++;
        }
    }

    return count;
}

static int64_t
vectorized_min_int64(VectorBatch *batch, int32_t column_index, bool *found)
{
    VectorColumn *column;
    int64_t *data;
    int64_t min_val = INT64_MAX;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            if (data[idx] < min_val) {
                min_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            if (data[i] < min_val) {
                min_val = data[i];
                *found = true;
            }
        }
    }

    return min_val;
}

static int64_t
vectorized_max_int64(VectorBatch *batch, int32_t column_index, bool *found)
{
    VectorColumn *column;
    int64_t *data;
    int64_t max_val = INT64_MIN;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            if (data[idx] > max_val) {
                max_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            if (data[i] > max_val) {
                max_val = data[i];
                *found = true;
            }
        }
    }

    return max_val;
}

static double
vectorized_min_float64(VectorBatch *batch, int32_t column_index, bool *found)
{
    VectorColumn *column;
    double *data;
    double min_val = DBL_MAX;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0.0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            if (data[idx] < min_val) {
                min_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            if (data[i] < min_val) {
                min_val = data[i];
                *found = true;
            }
        }
    }

    return min_val;
}

static double
vectorized_max_float64(VectorBatch *batch, int32_t column_index, bool *found)
{
    VectorColumn *column;
    double *data;
    double max_val = -DBL_MAX;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0.0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls &&
                (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            if (data[idx] > max_val) {
                max_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls &&
                (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            if (data[i] > max_val) {
                max_val = data[i];
                *found = true;
            }
        }
    }

    return max_val;
}

/* ============================================================
 * Unit Tests
 * ============================================================ */

/* Test: VectorBatch creation and destruction */
static bool
test_batch_create_destroy(void)
{
    VectorBatch *batch;

    /* Test basic creation */
    batch = vector_batch_create(100, 3);
    TEST_ASSERT(batch != NULL, "Batch should be created");
    TEST_ASSERT_EQ(100, batch->capacity, "Capacity should be 100");
    TEST_ASSERT_EQ(3, batch->column_count, "Column count should be 3");
    TEST_ASSERT_EQ(0, batch->count, "Initial count should be 0");
    TEST_ASSERT_EQ(false, batch->has_selection, "No selection initially");

    vector_batch_free(batch);

    /* Test default capacity */
    batch = vector_batch_create(0, 2);
    TEST_ASSERT(batch != NULL, "Batch with default capacity should be created");
    TEST_ASSERT_EQ(VECTORIZED_BATCH_SIZE, batch->capacity, "Should use default batch size");

    vector_batch_free(batch);

    return true;
}

/* Test: Column addition */
static bool
test_batch_add_column(void)
{
    VectorBatch *batch;
    VectorColumn *col;

    batch = vector_batch_create(100, 3);
    TEST_ASSERT(batch != NULL, "Batch should be created");

    /* Add int64 column */
    col = vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
    TEST_ASSERT(col != NULL, "Column should be added");
    TEST_ASSERT_EQ(VECTORIZED_TYPE_INT64, col->data_type, "Data type should be INT64");
    TEST_ASSERT_EQ(sizeof(int64_t), col->type_len, "Type len should be 8");
    TEST_ASSERT(col->data != NULL, "Data buffer should be allocated");
    TEST_ASSERT(col->nulls != NULL, "Null bitmap should be allocated");

    /* Add float64 column */
    col = vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);
    TEST_ASSERT(col != NULL, "Float64 column should be added");
    TEST_ASSERT_EQ(VECTORIZED_TYPE_FLOAT64, col->data_type, "Data type should be FLOAT64");

    /* Add int32 column */
    col = vector_batch_add_column(batch, 2, VECTORIZED_TYPE_INT32, INT4OID);
    TEST_ASSERT(col != NULL, "Int32 column should be added");
    TEST_ASSERT_EQ(sizeof(int32_t), col->type_len, "Type len should be 4");

    vector_batch_free(batch);

    return true;
}

/* Test: Batch reset */
static bool
test_batch_reset(void)
{
    VectorBatch *batch;

    batch = vector_batch_create(100, 2);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
    vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    /* Set some values */
    batch->count = 50;
    batch->has_selection = true;
    batch->selected_count = 25;
    batch->columns[0]->has_nulls = true;

    /* Reset */
    vector_batch_reset(batch);

    TEST_ASSERT_EQ(0, batch->count, "Count should be reset to 0");
    TEST_ASSERT_EQ(false, batch->has_selection, "Selection should be reset");
    TEST_ASSERT_EQ(0, batch->selected_count, "Selected count should be 0");
    TEST_ASSERT_EQ(false, batch->columns[0]->has_nulls, "has_nulls should be reset");

    vector_batch_free(batch);

    return true;
}

/* Test: Int64 filter - equality */
static bool
test_filter_int64_eq(void)
{
    VectorBatch *batch;
    int64_t *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    /* Fill with data: 0, 1, 2, ..., 15 */
    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i;
    }
    batch->count = 16;

    /* Filter: value == 5 */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_EQ, 5);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(1, batch->selected_count, "Should select exactly 1 row");
    TEST_ASSERT_EQ(5, batch->selection[0], "Selected row should be index 5");

    vector_batch_free(batch);

    return true;
}

/* Test: Int64 filter - greater than */
static bool
test_filter_int64_gt(void)
{
    VectorBatch *batch;
    int64_t *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i;
    }
    batch->count = 16;

    /* Filter: value > 10 (should select 11, 12, 13, 14, 15) */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_GT, 10);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(5, batch->selected_count, "Should select 5 rows (11-15)");

    vector_batch_free(batch);

    return true;
}

/* Test: Int64 filter - less than or equal */
static bool
test_filter_int64_le(void)
{
    VectorBatch *batch;
    int64_t *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i;
    }
    batch->count = 16;

    /* Filter: value <= 3 (should select 0, 1, 2, 3) */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 3);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(4, batch->selected_count, "Should select 4 rows (0-3)");

    vector_batch_free(batch);

    return true;
}

/* Test: Int64 filter with nulls */
static bool
test_filter_int64_with_nulls(void)
{
    VectorBatch *batch;
    int64_t *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i;
    }
    batch->count = 16;

    /* Mark indices 3, 7, 11 as null */
    batch->columns[0]->has_nulls = true;
    batch->columns[0]->nulls[0] |= (1ULL << 3);
    batch->columns[0]->nulls[0] |= (1ULL << 7);
    batch->columns[0]->nulls[0] |= (1ULL << 11);

    /* Filter: value <= 5 (normally 0,1,2,3,4,5 but 3 is null) */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 5);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(5, batch->selected_count, "Should select 5 rows (0,1,2,4,5 - 3 is null)");

    vector_batch_free(batch);

    return true;
}

/* Test: Float64 filter - equality */
static bool
test_filter_float64_eq(void)
{
    VectorBatch *batch;
    double *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    data = (double *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i * 1.5;
    }
    batch->count = 16;

    /* Filter: value == 7.5 (index 5) */
    vectorized_filter_float64(batch, 0, VECTORIZED_CMP_EQ, 7.5);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(1, batch->selected_count, "Should select 1 row");
    TEST_ASSERT_EQ(5, batch->selection[0], "Selected row should be index 5");

    vector_batch_free(batch);

    return true;
}

/* Test: Float64 filter - greater than or equal */
static bool
test_filter_float64_ge(void)
{
    VectorBatch *batch;
    double *data;

    batch = vector_batch_create(16, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    data = (double *)batch->columns[0]->data;
    for (int i = 0; i < 16; i++) {
        data[i] = i * 1.0;
    }
    batch->count = 16;

    /* Filter: value >= 12.0 (should select 12, 13, 14, 15) */
    vectorized_filter_float64(batch, 0, VECTORIZED_CMP_GE, 12.0);

    TEST_ASSERT(batch->has_selection, "Selection should be active");
    TEST_ASSERT_EQ(4, batch->selected_count, "Should select 4 rows");

    vector_batch_free(batch);

    return true;
}

/* Test: Sum int64 */
static bool
test_sum_int64(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t sum;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 100; i++) {
        data[i] = i + 1;  /* 1, 2, 3, ..., 100 */
    }
    batch->count = 100;

    sum = vectorized_sum_int64(batch, 0);

    /* Sum of 1..100 = 100*101/2 = 5050 */
    TEST_ASSERT_EQ(5050, sum, "Sum of 1..100 should be 5050");

    vector_batch_free(batch);

    return true;
}

/* Test: Sum int64 with selection */
static bool
test_sum_int64_with_selection(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t sum;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 100; i++) {
        data[i] = i + 1;
    }
    batch->count = 100;

    /* Filter: value <= 10 */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 10);

    sum = vectorized_sum_int64(batch, 0);

    /* Sum of 1..10 = 55 */
    TEST_ASSERT_EQ(55, sum, "Sum of 1..10 should be 55");

    vector_batch_free(batch);

    return true;
}

/* Test: Sum int64 with nulls */
static bool
test_sum_int64_with_nulls(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t sum;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 10; i++) {
        data[i] = i + 1;  /* 1, 2, ..., 10 */
    }
    batch->count = 10;

    /* Mark indices 4 and 9 as null (values 5 and 10) */
    batch->columns[0]->has_nulls = true;
    batch->columns[0]->nulls[0] |= (1ULL << 4);
    batch->columns[0]->nulls[0] |= (1ULL << 9);

    sum = vectorized_sum_int64(batch, 0);

    /* Sum of 1..10 = 55, minus 5 and 10 = 40 */
    TEST_ASSERT_EQ(40, sum, "Sum should be 40 (55 - 5 - 10)");

    vector_batch_free(batch);

    return true;
}

/* Test: Sum float64 */
static bool
test_sum_float64(void)
{
    VectorBatch *batch;
    double *data;
    double sum;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    data = (double *)batch->columns[0]->data;
    for (int i = 0; i < 100; i++) {
        data[i] = (i + 1) * 0.5;  /* 0.5, 1.0, 1.5, ..., 50.0 */
    }
    batch->count = 100;

    sum = vectorized_sum_float64(batch, 0);

    /* Sum of 0.5 * (1..100) = 0.5 * 5050 = 2525.0 */
    TEST_ASSERT_FLOAT_EQ(2525.0, sum, 0.001, "Sum should be 2525.0");

    vector_batch_free(batch);

    return true;
}

/* Test: Count */
static bool
test_count(void)
{
    VectorBatch *batch;
    int64_t count;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
    batch->count = 100;

    count = vectorized_count(batch);
    TEST_ASSERT_EQ(100, count, "Count should be 100");

    /* With selection */
    batch->has_selection = true;
    batch->selected_count = 25;

    count = vectorized_count(batch);
    TEST_ASSERT_EQ(25, count, "Count with selection should be 25");

    vector_batch_free(batch);

    return true;
}

/* Test: Count column (with nulls) */
static bool
test_count_column(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t count;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 10; i++) {
        data[i] = i;
    }
    batch->count = 10;

    /* No nulls */
    count = vectorized_count_column(batch, 0);
    TEST_ASSERT_EQ(10, count, "Count without nulls should be 10");

    /* Mark 3 as null */
    batch->columns[0]->has_nulls = true;
    batch->columns[0]->nulls[0] |= (1ULL << 2);
    batch->columns[0]->nulls[0] |= (1ULL << 5);
    batch->columns[0]->nulls[0] |= (1ULL << 8);

    count = vectorized_count_column(batch, 0);
    TEST_ASSERT_EQ(7, count, "Count with 3 nulls should be 7");

    vector_batch_free(batch);

    return true;
}

/* Test: Min int64 */
static bool
test_min_int64(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t min_val;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    data[0] = 50;
    data[1] = 30;
    data[2] = 70;
    data[3] = 10;  /* Minimum */
    data[4] = 90;
    data[5] = 40;
    data[6] = 60;
    data[7] = 20;
    data[8] = 80;
    data[9] = 100;
    batch->count = 10;

    min_val = vectorized_min_int64(batch, 0, &found);

    TEST_ASSERT(found, "Should find minimum");
    TEST_ASSERT_EQ(10, min_val, "Minimum should be 10");

    vector_batch_free(batch);

    return true;
}

/* Test: Max int64 */
static bool
test_max_int64(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t max_val;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    data[0] = 50;
    data[1] = 30;
    data[2] = 70;
    data[3] = 10;
    data[4] = 90;
    data[5] = 40;
    data[6] = 60;
    data[7] = 20;
    data[8] = 80;
    data[9] = 100;  /* Maximum */
    batch->count = 10;

    max_val = vectorized_max_int64(batch, 0, &found);

    TEST_ASSERT(found, "Should find maximum");
    TEST_ASSERT_EQ(100, max_val, "Maximum should be 100");

    vector_batch_free(batch);

    return true;
}

/* Test: Min float64 */
static bool
test_min_float64(void)
{
    VectorBatch *batch;
    double *data;
    double min_val;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    data = (double *)batch->columns[0]->data;
    data[0] = 5.5;
    data[1] = 3.3;
    data[2] = 7.7;
    data[3] = 1.1;  /* Minimum */
    data[4] = 9.9;
    data[5] = 4.4;
    data[6] = 6.6;
    data[7] = 2.2;
    data[8] = 8.8;
    data[9] = 10.0;
    batch->count = 10;

    min_val = vectorized_min_float64(batch, 0, &found);

    TEST_ASSERT(found, "Should find minimum");
    TEST_ASSERT_FLOAT_EQ(1.1, min_val, 0.001, "Minimum should be 1.1");

    vector_batch_free(batch);

    return true;
}

/* Test: Max float64 */
static bool
test_max_float64(void)
{
    VectorBatch *batch;
    double *data;
    double max_val;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    data = (double *)batch->columns[0]->data;
    data[0] = 5.5;
    data[1] = 3.3;
    data[2] = 7.7;
    data[3] = 1.1;
    data[4] = 9.9;
    data[5] = 4.4;
    data[6] = 6.6;
    data[7] = 2.2;
    data[8] = 8.8;
    data[9] = 10.5;  /* Maximum */
    batch->count = 10;

    max_val = vectorized_max_float64(batch, 0, &found);

    TEST_ASSERT(found, "Should find maximum");
    TEST_ASSERT_FLOAT_EQ(10.5, max_val, 0.001, "Maximum should be 10.5");

    vector_batch_free(batch);

    return true;
}

/* Test: Min/Max with nulls */
static bool
test_min_max_with_nulls(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t min_val, max_val;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    data[0] = 50;
    data[1] = 5;    /* Would be minimum but null */
    data[2] = 70;
    data[3] = 10;   /* Actual minimum after null */
    data[4] = 90;
    data[5] = 40;
    data[6] = 60;
    data[7] = 20;
    data[8] = 80;
    data[9] = 200;  /* Would be maximum but null */
    batch->count = 10;

    /* Mark indices 1 and 9 as null */
    batch->columns[0]->has_nulls = true;
    batch->columns[0]->nulls[0] |= (1ULL << 1);
    batch->columns[0]->nulls[0] |= (1ULL << 9);

    min_val = vectorized_min_int64(batch, 0, &found);
    TEST_ASSERT(found, "Should find minimum");
    TEST_ASSERT_EQ(10, min_val, "Minimum should be 10 (5 is null)");

    max_val = vectorized_max_int64(batch, 0, &found);
    TEST_ASSERT(found, "Should find maximum");
    TEST_ASSERT_EQ(90, max_val, "Maximum should be 90 (200 is null)");

    vector_batch_free(batch);

    return true;
}

/* Test: Selection vector chaining (multiple filters) */
static bool
test_selection_chaining(void)
{
    VectorBatch *batch;
    int64_t *data;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 100; i++) {
        data[i] = i;
    }
    batch->count = 100;

    /* First filter: value >= 20 (80 rows) */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_GE, 20);
    TEST_ASSERT_EQ(80, batch->selected_count, "After first filter: 80 rows");

    /* Second filter: value < 30 (narrows to 20-29, 10 rows) */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LT, 30);
    TEST_ASSERT_EQ(10, batch->selected_count, "After second filter: 10 rows");

    /* Verify selection contains 20-29 */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ(20 + i, batch->selection[i], "Selection should contain correct indices");
    }

    vector_batch_free(batch);

    return true;
}

/* Test: Large batch (stress test for SIMD) */
static bool
test_large_batch(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t sum;

    batch = vector_batch_create(VECTORIZED_BATCH_SIZE, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < VECTORIZED_BATCH_SIZE; i++) {
        data[i] = 1;  /* All 1s for easy sum verification */
    }
    batch->count = VECTORIZED_BATCH_SIZE;

    sum = vectorized_sum_int64(batch, 0);
    TEST_ASSERT_EQ(VECTORIZED_BATCH_SIZE, sum, "Sum of 1s should equal batch size");

    /* Filter out half */
    vectorized_filter_int64(batch, 0, VECTORIZED_CMP_EQ, 1);
    TEST_ASSERT_EQ(VECTORIZED_BATCH_SIZE, batch->selected_count, "All rows should match");

    vector_batch_free(batch);

    return true;
}

/* Test: Memory alignment */
static bool
test_memory_alignment(void)
{
    VectorBatch *batch;

    batch = vector_batch_create(100, 2);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
    vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

    /* Check data buffer alignment */
    TEST_ASSERT(((uintptr_t)batch->columns[0]->data & (VECTORIZED_ALIGNMENT - 1)) == 0,
                "Int64 data buffer should be 32-byte aligned");
    TEST_ASSERT(((uintptr_t)batch->columns[1]->data & (VECTORIZED_ALIGNMENT - 1)) == 0,
                "Float64 data buffer should be 32-byte aligned");
    TEST_ASSERT(((uintptr_t)batch->selection & (VECTORIZED_ALIGNMENT - 1)) == 0,
                "Selection vector should be 32-byte aligned");

    vector_batch_free(batch);

    return true;
}

/* Test: Empty batch */
static bool
test_empty_batch(void)
{
    VectorBatch *batch;
    int64_t count;

    batch = vector_batch_create(100, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
    batch->count = 0;  /* Empty batch */

    count = vectorized_count(batch);
    TEST_ASSERT_EQ(0, count, "Count of empty batch should be 0");

    int64_t sum = vectorized_sum_int64(batch, 0);
    TEST_ASSERT_EQ(0, sum, "Sum of empty batch should be 0");

    vector_batch_free(batch);

    return true;
}

/* Test: All null column */
static bool
test_all_null_column(void)
{
    VectorBatch *batch;
    int64_t *data;
    int64_t count, sum;
    bool found;

    batch = vector_batch_create(10, 1);
    vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

    data = (int64_t *)batch->columns[0]->data;
    for (int i = 0; i < 10; i++) {
        data[i] = i + 1;
    }
    batch->count = 10;

    /* Mark all as null */
    batch->columns[0]->has_nulls = true;
    batch->columns[0]->nulls[0] = 0x3FF;  /* First 10 bits set */

    count = vectorized_count_column(batch, 0);
    TEST_ASSERT_EQ(0, count, "Count of all-null column should be 0");

    sum = vectorized_sum_int64(batch, 0);
    TEST_ASSERT_EQ(0, sum, "Sum of all-null column should be 0");

    int64_t min_val = vectorized_min_int64(batch, 0, &found);
    TEST_ASSERT(!found, "Min should not be found in all-null column");

    vector_batch_free(batch);

    return true;
}

/* ============================================================
 * Main Test Runner
 * ============================================================ */

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("===========================================\n");
    printf("Orochi DB Vectorized Execution Unit Tests\n");
    printf("===========================================\n");
#ifdef __AVX2__
    printf("AVX2: ENABLED\n");
#else
    printf("AVX2: DISABLED (scalar fallback)\n");
#endif
    printf("\n");

    /* Batch creation and management tests */
    printf("--- Batch Management Tests ---\n");
    RUN_TEST(test_batch_create_destroy);
    RUN_TEST(test_batch_add_column);
    RUN_TEST(test_batch_reset);
    printf("\n");

    /* Filter tests */
    printf("--- Filter Tests (Int64) ---\n");
    RUN_TEST(test_filter_int64_eq);
    RUN_TEST(test_filter_int64_gt);
    RUN_TEST(test_filter_int64_le);
    RUN_TEST(test_filter_int64_with_nulls);
    printf("\n");

    printf("--- Filter Tests (Float64) ---\n");
    RUN_TEST(test_filter_float64_eq);
    RUN_TEST(test_filter_float64_ge);
    printf("\n");

    /* Aggregation tests */
    printf("--- Aggregation Tests (SUM) ---\n");
    RUN_TEST(test_sum_int64);
    RUN_TEST(test_sum_int64_with_selection);
    RUN_TEST(test_sum_int64_with_nulls);
    RUN_TEST(test_sum_float64);
    printf("\n");

    printf("--- Aggregation Tests (COUNT) ---\n");
    RUN_TEST(test_count);
    RUN_TEST(test_count_column);
    printf("\n");

    printf("--- Aggregation Tests (MIN/MAX) ---\n");
    RUN_TEST(test_min_int64);
    RUN_TEST(test_max_int64);
    RUN_TEST(test_min_float64);
    RUN_TEST(test_max_float64);
    RUN_TEST(test_min_max_with_nulls);
    printf("\n");

    /* Advanced tests */
    printf("--- Advanced Tests ---\n");
    RUN_TEST(test_selection_chaining);
    RUN_TEST(test_large_batch);
    RUN_TEST(test_memory_alignment);
    RUN_TEST(test_empty_batch);
    RUN_TEST(test_all_null_column);
    printf("\n");

    /* Summary */
    printf("===========================================\n");
    printf("Test Summary:\n");
    printf("  Total assertions: %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("===========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
