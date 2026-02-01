/*-------------------------------------------------------------------------
 *
 * vectorized.c
 *    Orochi DB vectorized query execution engine implementation
 *
 * This module provides SIMD-accelerated vectorized operations for:
 *   - Filtering (predicate evaluation)
 *   - Aggregations (SUM, COUNT, MIN, MAX, AVG)
 *   - Selection vector management
 *
 * Uses AVX2 intrinsics when available, with scalar fallbacks.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include <float.h>
#include <math.h>
#include <stdint.h> /* For INT64_MAX, INT64_MIN */
#include <string.h>

#include "vectorized.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* Forward declarations for scalar fallbacks */
static void filter_int64_scalar(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                int64 value);
static void filter_float64_scalar(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                  double value);
static int64 sum_int64_scalar(VectorBatch *batch, int32 column_index);
static double sum_float64_scalar(VectorBatch *batch, int32 column_index);

/* ============================================================
 * Memory Management
 * ============================================================ */

/*
 * Allocate aligned memory using palloc
 * Returns pointer aligned to specified boundary
 */
void *vectorized_palloc_aligned(Size size, Size alignment)
{
    void *ptr;
    void *aligned_ptr;
    Size total_size;

    /* Allocate extra space for alignment and storing original pointer */
    total_size = size + alignment + sizeof(void *);
    ptr = palloc(total_size);

    /* Calculate aligned address */
    aligned_ptr = (void *)(((uintptr_t)ptr + sizeof(void *) + alignment - 1) & ~(alignment - 1));

    /* Store original pointer just before aligned address */
    ((void **)aligned_ptr)[-1] = ptr;

    return aligned_ptr;
}

/*
 * Free aligned memory allocated by vectorized_palloc_aligned
 */
void vectorized_pfree_aligned(void *aligned_ptr)
{
    if (aligned_ptr != NULL) {
        void *original_ptr = ((void **)aligned_ptr)[-1];
        pfree(original_ptr);
    }
}

/* ============================================================
 * Batch Creation and Management
 * ============================================================ */

VectorBatch *vector_batch_create(int32 capacity, int32 column_count)
{
    VectorBatch *batch;
    MemoryContext batch_context;
    MemoryContext old_context;

    if (capacity < 1)
        capacity = VECTORIZED_BATCH_SIZE;

    if (column_count < 1 || column_count > VECTORIZED_MAX_COLUMNS)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid column count: %d", column_count)));

    /* Create dedicated memory context */
    batch_context =
        AllocSetContextCreate(CurrentMemoryContext, "VectorBatchContext", ALLOCSET_DEFAULT_SIZES);

    old_context = MemoryContextSwitchTo(batch_context);

    batch = palloc0(sizeof(VectorBatch));
    batch->capacity = capacity;
    batch->count = 0;
    batch->column_count = column_count;
    batch->batch_context = batch_context;

    /* Allocate column pointer array */
    batch->columns = palloc0(column_count * sizeof(VectorColumn *));

    /* Allocate selection vector (aligned for SIMD) */
    batch->selection = vectorized_palloc_aligned(capacity * sizeof(uint32), VECTORIZED_ALIGNMENT);
    batch->selected_count = 0;
    batch->has_selection = false;

    /* Initialize statistics */
    batch->rows_processed = 0;
    batch->rows_filtered = 0;

    MemoryContextSwitchTo(old_context);

    return batch;
}

void vector_batch_free(VectorBatch *batch)
{
    if (batch == NULL)
        return;

    /* Freeing the context frees all allocated memory */
    MemoryContextDelete(batch->batch_context);
}

void vector_batch_reset(VectorBatch *batch)
{
    int i;

    if (batch == NULL)
        return;

    batch->count = 0;
    batch->selected_count = 0;
    batch->has_selection = false;

    /* Reset each column */
    for (i = 0; i < batch->column_count; i++) {
        if (batch->columns[i] != NULL) {
            batch->columns[i]->has_nulls = false;
        }
    }
}

VectorColumn *vector_batch_add_column(VectorBatch *batch, int32 column_index,
                                      VectorizedDataType data_type, Oid type_oid)
{
    VectorColumn *column;
    MemoryContext old_context;
    Size data_size;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid column index: %d", column_index)));

    old_context = MemoryContextSwitchTo(batch->batch_context);

    column = palloc0(sizeof(VectorColumn));
    column->data_type = data_type;
    column->type_oid = type_oid;

    /* Determine type properties and allocate data buffer */
    switch (data_type) {
    case VECTORIZED_TYPE_INT32:
    case VECTORIZED_TYPE_DATE:
        column->type_len = sizeof(int32);
        column->type_byval = true;
        data_size = batch->capacity * sizeof(int32);
        break;

    case VECTORIZED_TYPE_INT64:
    case VECTORIZED_TYPE_TIMESTAMP:
        column->type_len = sizeof(int64);
        column->type_byval = true;
        data_size = batch->capacity * sizeof(int64);
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

    case VECTORIZED_TYPE_STRING:
        column->type_len = -1; /* Variable length */
        column->type_byval = false;
        /* For strings, allocate pointer array and offsets */
        data_size = batch->capacity * sizeof(char *);
        column->offsets = palloc0(batch->capacity * sizeof(int32));
        break;

    default:
        column->type_len = sizeof(Datum);
        column->type_byval = false;
        data_size = batch->capacity * sizeof(Datum);
        break;
    }

    /* Allocate aligned data buffer */
    column->data = vectorized_palloc_aligned(data_size, VECTORIZED_ALIGNMENT);
    memset(column->data, 0, data_size);

    /* Allocate null bitmap (1 bit per row, rounded up to 64-bit words) */
    column->nulls = palloc0(((batch->capacity + 63) / 64) * sizeof(uint64));
    column->has_nulls = false;

    batch->columns[column_index] = column;

    MemoryContextSwitchTo(old_context);

    return column;
}

VectorizedDataType vectorized_type_from_oid(Oid type_oid)
{
    switch (type_oid) {
    case INT2OID:
    case INT4OID:
        return VECTORIZED_TYPE_INT32;

    case INT8OID:
        return VECTORIZED_TYPE_INT64;

    case FLOAT4OID:
        return VECTORIZED_TYPE_FLOAT32;

    case FLOAT8OID:
        return VECTORIZED_TYPE_FLOAT64;

    case BOOLOID:
        return VECTORIZED_TYPE_BOOL;

    case TEXTOID:
    case VARCHAROID:
    case BPCHAROID:
        return VECTORIZED_TYPE_STRING;

    case TIMESTAMPOID:
    case TIMESTAMPTZOID:
        return VECTORIZED_TYPE_TIMESTAMP;

    case DATEOID:
        return VECTORIZED_TYPE_DATE;

    default:
        return VECTORIZED_TYPE_UNKNOWN;
    }
}

/* ============================================================
 * Selection Vector Management
 * ============================================================ */

/*
 * Initialize selection vector with all rows selected
 */
static void init_full_selection(VectorBatch *batch)
{
    int32 i;

    for (i = 0; i < batch->count; i++)
        batch->selection[i] = i;

    batch->selected_count = batch->count;
    batch->has_selection = true;
}

/*
 * Compact selection vector after filtering
 * Removes unselected entries and updates count
 */
static void compact_selection(VectorBatch *batch, bool *mask)
{
    int32 write_pos = 0;
    int32 i;

    for (i = 0; i < batch->selected_count; i++) {
        if (mask[batch->selection[i]]) {
            batch->selection[write_pos++] = batch->selection[i];
        }
    }

    batch->rows_filtered += (batch->selected_count - write_pos);
    batch->selected_count = write_pos;
}

/* ============================================================
 * AVX2 Filter Implementations
 * ============================================================ */

#ifdef __AVX2__

/*
 * AVX2 implementation for int64 filtering
 */
static void filter_int64_avx2(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                              int64 value)
{
    VectorColumn *column = batch->columns[column_index];
    int64 *data = (int64 *)column->data;
    bool *mask;
    __m256i val_vec = _mm256_set1_epi64x(value);
    int32 i;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));
    memset(mask, 0, batch->capacity * sizeof(bool));

    /* Process 4 int64 values at a time */
    for (i = 0; i + VECTORIZED_INT64_LANES <= batch->count; i += VECTORIZED_INT64_LANES) {
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
            /* GE = NOT LT = NOT (val > data) */
            cmp_result = _mm256_cmpgt_epi64(val_vec, data_vec);
            cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
            break;

        case VECTORIZED_CMP_LE:
            /* LE = NOT GT */
            cmp_result = _mm256_cmpgt_epi64(data_vec, val_vec);
            cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
            break;

        case VECTORIZED_CMP_NE:
            cmp_result = _mm256_cmpeq_epi64(data_vec, val_vec);
            cmp_result = _mm256_xor_si256(cmp_result, _mm256_set1_epi64x(-1));
            break;

        default:
            pfree(mask);
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unsupported comparison operator")));
            return;
        }

        /* Extract comparison results to mask */
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, cmp_result);
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }

    /* Handle remaining elements */
    for (; i < batch->count; i++) {
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
            break;
        }
    }

    /* Handle null values - nulls never match */
    if (column->has_nulls) {
        for (i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

/*
 * AVX2 implementation for float64 filtering
 */
static void filter_float64_avx2(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                double value)
{
    VectorColumn *column = batch->columns[column_index];
    double *data = (double *)column->data;
    bool *mask;
    __m256d val_vec = _mm256_set1_pd(value);
    int32 i;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));
    memset(mask, 0, batch->capacity * sizeof(bool));

    /* Process 4 double values at a time */
    for (i = 0; i + VECTORIZED_FLOAT64_LANES <= batch->count; i += VECTORIZED_FLOAT64_LANES) {
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
            pfree(mask);
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unsupported comparison operator")));
            return;
        }

        /* Extract comparison results */
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, _mm256_castpd_si256(cmp_result));
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }

    /* Handle remaining elements */
    for (; i < batch->count; i++) {
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
            break;
        }
    }

    /* Handle null values */
    if (column->has_nulls) {
        for (i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

/*
 * AVX2 implementation for int64 sum
 */
static int64 sum_int64_avx2(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column = batch->columns[column_index];
    int64 *data = (int64 *)column->data;
    __m256i sum_vec = _mm256_setzero_si256();
    int64 sum = 0;
    int32 i;

    if (batch->has_selection) {
        /* Use selection vector */
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];

            /* Skip nulls */
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            sum += data[idx];
        }
    } else {
        /* Process all rows with SIMD */
        for (i = 0; i + VECTORIZED_INT64_LANES <= batch->count; i += VECTORIZED_INT64_LANES) {
            __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
            sum_vec = _mm256_add_epi64(sum_vec, data_vec);
        }

        /* Horizontal sum */
        int64 partial[4];
        _mm256_storeu_si256((__m256i *)partial, sum_vec);
        sum = partial[0] + partial[1] + partial[2] + partial[3];

        /* Handle remaining elements */
        for (; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }

        /* Subtract null values that were included in SIMD sum */
        if (column->has_nulls) {
            for (i = 0; i + VECTORIZED_INT64_LANES <= batch->count; i += VECTORIZED_INT64_LANES) {
                for (int j = 0; j < VECTORIZED_INT64_LANES; j++) {
                    int idx = i + j;
                    if (column->nulls[idx / 64] & (1ULL << (idx % 64)))
                        sum -= data[idx];
                }
            }
        }
    }

    return sum;
}

/*
 * AVX2 implementation for float64 sum
 */
static double sum_float64_avx2(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column = batch->columns[column_index];
    double *data = (double *)column->data;
    __m256d sum_vec = _mm256_setzero_pd();
    double sum = 0.0;
    int32 i;

    if (batch->has_selection) {
        /* Use selection vector */
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];

            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            sum += data[idx];
        }
    } else {
        /* Process all rows with SIMD */
        for (i = 0; i + VECTORIZED_FLOAT64_LANES <= batch->count; i += VECTORIZED_FLOAT64_LANES) {
            __m256d data_vec = _mm256_loadu_pd(data + i);
            sum_vec = _mm256_add_pd(sum_vec, data_vec);
        }

        /* Horizontal sum */
        __m128d hi = _mm256_extractf128_pd(sum_vec, 1);
        __m128d lo = _mm256_castpd256_pd128(sum_vec);
        __m128d sum128 = _mm_add_pd(hi, lo);
        sum128 = _mm_hadd_pd(sum128, sum128);
        sum = _mm_cvtsd_f64(sum128);

        /* Handle remaining elements */
        for (; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }

        /* Subtract null values */
        if (column->has_nulls) {
            for (i = 0; i + VECTORIZED_FLOAT64_LANES <= batch->count;
                 i += VECTORIZED_FLOAT64_LANES) {
                for (int j = 0; j < VECTORIZED_FLOAT64_LANES; j++) {
                    int idx = i + j;
                    if (column->nulls[idx / 64] & (1ULL << (idx % 64)))
                        sum -= data[idx];
                }
            }
        }
    }

    return sum;
}

#endif /* __AVX2__ */

/* ============================================================
 * Scalar Fallback Implementations
 * ============================================================ */

static void filter_int64_scalar(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                int64 value)
{
    VectorColumn *column = batch->columns[column_index];
    int64 *data = (int64 *)column->data;
    bool *mask;
    int32 i;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));

    for (i = 0; i < batch->count; i++) {
        /* Check for null */
        if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) {
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

    compact_selection(batch, mask);
    pfree(mask);
}

static void filter_float64_scalar(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                  double value)
{
    VectorColumn *column = batch->columns[column_index];
    double *data = (double *)column->data;
    bool *mask;
    int32 i;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));

    for (i = 0; i < batch->count; i++) {
        if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) {
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

    compact_selection(batch, mask);
    pfree(mask);
}

static int64 sum_int64_scalar(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column = batch->columns[column_index];
    int64 *data = (int64 *)column->data;
    int64 sum = 0;
    int32 i;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];

            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            sum += data[idx];
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            sum += data[i];
        }
    }

    return sum;
}

static double sum_float64_scalar(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column = batch->columns[column_index];
    double *data = (double *)column->data;
    double sum = 0.0;
    int32 i;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];

            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            sum += data[idx];
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            sum += data[i];
        }
    }

    return sum;
}

/* ============================================================
 * Public Filter Functions
 * ============================================================ */

void vectorized_filter_int64(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                             int64 value)
{
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    if (batch->columns[column_index] == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

#ifdef __AVX2__
    filter_int64_avx2(batch, column_index, op, value);
#else
    filter_int64_scalar(batch, column_index, op, value);
#endif
}

void vectorized_filter_float64(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                               double value)
{
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    if (batch->columns[column_index] == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

#ifdef __AVX2__
    filter_float64_avx2(batch, column_index, op, value);
#else
    filter_float64_scalar(batch, column_index, op, value);
#endif
}

void vectorized_filter_int32(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                             int32 value)
{
    VectorColumn *column;
    int32 *data;
    bool *mask;
    int32 i;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    column = batch->columns[column_index];
    if (column == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

    data = (int32 *)column->data;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));

#ifdef __AVX2__
    {
        __m256i val_vec = _mm256_set1_epi32(value);

        /* Process 8 int32 values at a time */
        for (i = 0; i + VECTORIZED_INT32_LANES <= batch->count; i += VECTORIZED_INT32_LANES) {
            __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
            __m256i cmp_result;

            switch (op) {
            case VECTORIZED_CMP_EQ:
                cmp_result = _mm256_cmpeq_epi32(data_vec, val_vec);
                break;
            case VECTORIZED_CMP_GT:
                cmp_result = _mm256_cmpgt_epi32(data_vec, val_vec);
                break;
            case VECTORIZED_CMP_LT:
                cmp_result = _mm256_cmpgt_epi32(val_vec, data_vec);
                break;
            default:
                /* Fall through to scalar for other ops */
                goto scalar_fallback;
            }

            /* Extract results */
            int32 results[8];
            _mm256_storeu_si256((__m256i *)results, cmp_result);
            for (int j = 0; j < 8; j++)
                mask[i + j] = (results[j] != 0);
        }

        /* Handle remaining */
        goto handle_remaining;

    scalar_fallback:;
    }
#endif

    /* Scalar implementation for remaining or all elements */
    for (i = 0; i < batch->count; i++) {
        if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) {
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

#ifdef __AVX2__
handle_remaining:
    for (; i < batch->count; i++) {
        if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) {
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
#endif

    /* Handle nulls in SIMD region */
    if (column->has_nulls) {
        for (i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

void vectorized_filter_float32(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                               float value)
{
    VectorColumn *column;
    float *data;
    bool *mask;
    int32 i;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    column = batch->columns[column_index];
    if (column == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

    data = (float *)column->data;

    if (!batch->has_selection)
        init_full_selection(batch);

    mask = palloc(batch->capacity * sizeof(bool));

#ifdef __AVX2__
    {
        __m256 val_vec = _mm256_set1_ps(value);

        for (i = 0; i + VECTORIZED_FLOAT32_LANES <= batch->count; i += VECTORIZED_FLOAT32_LANES) {
            __m256 data_vec = _mm256_loadu_ps(data + i);
            __m256 cmp_result;

            switch (op) {
            case VECTORIZED_CMP_EQ:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_EQ_OQ);
                break;
            case VECTORIZED_CMP_NE:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_NEQ_OQ);
                break;
            case VECTORIZED_CMP_LT:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_LT_OQ);
                break;
            case VECTORIZED_CMP_LE:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_LE_OQ);
                break;
            case VECTORIZED_CMP_GT:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_GT_OQ);
                break;
            case VECTORIZED_CMP_GE:
                cmp_result = _mm256_cmp_ps(data_vec, val_vec, _CMP_GE_OQ);
                break;
            default:
                cmp_result = _mm256_setzero_ps();
                break;
            }

            int32 results[8];
            _mm256_storeu_si256((__m256i *)results, _mm256_castps_si256(cmp_result));
            for (int j = 0; j < 8; j++)
                mask[i + j] = (results[j] != 0);
        }
    }
#else
    i = 0;
#endif

    /* Handle remaining elements */
    for (; i < batch->count; i++) {
        if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) {
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

    /* Handle nulls */
    if (column->has_nulls) {
        for (i = 0; i < batch->count; i++) {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }

    compact_selection(batch, mask);
    pfree(mask);
}

void vectorized_filter_generic(VectorBatch *batch, VectorizedFilterState *filter)
{
    switch (filter->data_type) {
    case VECTORIZED_TYPE_INT64:
    case VECTORIZED_TYPE_TIMESTAMP:
        vectorized_filter_int64(batch, filter->column_index, filter->compare_op,
                                DatumGetInt64(filter->const_value));
        break;

    case VECTORIZED_TYPE_INT32:
    case VECTORIZED_TYPE_DATE:
        vectorized_filter_int32(batch, filter->column_index, filter->compare_op,
                                DatumGetInt32(filter->const_value));
        break;

    case VECTORIZED_TYPE_FLOAT64:
        vectorized_filter_float64(batch, filter->column_index, filter->compare_op,
                                  DatumGetFloat8(filter->const_value));
        break;

    case VECTORIZED_TYPE_FLOAT32:
        vectorized_filter_float32(batch, filter->column_index, filter->compare_op,
                                  DatumGetFloat4(filter->const_value));
        break;

    default:
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("vectorized filter not supported for this data type")));
    }
}

void vectorized_selection_and(VectorBatch *batch, uint32 *other_selection, int32 other_count)
{
    uint32 *new_selection;
    int32 new_count = 0;
    int32 i, j;

    if (!batch->has_selection)
        init_full_selection(batch);

    new_selection = palloc(batch->capacity * sizeof(uint32));

    /* Intersection of two sorted selection vectors */
    i = 0;
    j = 0;
    while (i < batch->selected_count && j < other_count) {
        if (batch->selection[i] == other_selection[j]) {
            new_selection[new_count++] = batch->selection[i];
            i++;
            j++;
        } else if (batch->selection[i] < other_selection[j]) {
            i++;
        } else {
            j++;
        }
    }

    memcpy(batch->selection, new_selection, new_count * sizeof(uint32));
    batch->selected_count = new_count;

    pfree(new_selection);
}

void vectorized_selection_or(VectorBatch *batch, uint32 *other_selection, int32 other_count)
{
    uint32 *new_selection;
    int32 new_count = 0;
    int32 i, j;

    if (!batch->has_selection) {
        /* All rows already selected */
        return;
    }

    new_selection = palloc(batch->capacity * sizeof(uint32));

    /* Union of two sorted selection vectors */
    i = 0;
    j = 0;
    while (i < batch->selected_count || j < other_count) {
        if (i >= batch->selected_count) {
            new_selection[new_count++] = other_selection[j++];
        } else if (j >= other_count) {
            new_selection[new_count++] = batch->selection[i++];
        } else if (batch->selection[i] == other_selection[j]) {
            new_selection[new_count++] = batch->selection[i];
            i++;
            j++;
        } else if (batch->selection[i] < other_selection[j]) {
            new_selection[new_count++] = batch->selection[i++];
        } else {
            new_selection[new_count++] = other_selection[j++];
        }
    }

    memcpy(batch->selection, new_selection, new_count * sizeof(uint32));
    batch->selected_count = new_count;

    pfree(new_selection);
}

/* ============================================================
 * Public Aggregation Functions
 * ============================================================ */

int64 vectorized_sum_int64(VectorBatch *batch, int32 column_index)
{
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    if (batch->columns[column_index] == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

#ifdef __AVX2__
    return sum_int64_avx2(batch, column_index);
#else
    return sum_int64_scalar(batch, column_index);
#endif
}

double vectorized_sum_float64(VectorBatch *batch, int32 column_index)
{
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    if (batch->columns[column_index] == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

#ifdef __AVX2__
    return sum_float64_avx2(batch, column_index);
#else
    return sum_float64_scalar(batch, column_index);
#endif
}

int64 vectorized_sum_int32(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column;
    int32 *data;
    int64 sum = 0;
    int32 i;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    column = batch->columns[column_index];
    if (column == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

    data = (int32 *)column->data;

#ifdef __AVX2__
    if (!batch->has_selection) {
        __m256i sum_vec = _mm256_setzero_si256();

        for (i = 0; i + VECTORIZED_INT32_LANES <= batch->count; i += VECTORIZED_INT32_LANES) {
            __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
            /* Widen to 64-bit and accumulate */
            __m256i lo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(data_vec));
            __m256i hi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(data_vec, 1));
            sum_vec = _mm256_add_epi64(sum_vec, lo);
            sum_vec = _mm256_add_epi64(sum_vec, hi);
        }

        int64 partial[4];
        _mm256_storeu_si256((__m256i *)partial, sum_vec);
        sum = partial[0] + partial[1] + partial[2] + partial[3];

        for (; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }

        /* Subtract nulls from SIMD region */
        if (column->has_nulls) {
            for (i = 0; i + VECTORIZED_INT32_LANES <= batch->count; i += VECTORIZED_INT32_LANES) {
                for (int j = 0; j < VECTORIZED_INT32_LANES; j++) {
                    int idx = i + j;
                    if (column->nulls[idx / 64] & (1ULL << (idx % 64)))
                        sum -= data[idx];
                }
            }
        }
    } else
#endif
    {
        if (batch->has_selection) {
            for (i = 0; i < batch->selected_count; i++) {
                uint32 idx = batch->selection[i];
                if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                    continue;
                sum += data[idx];
            }
        } else {
            for (i = 0; i < batch->count; i++) {
                if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                    continue;
                sum += data[i];
            }
        }
    }

    return sum;
}

double vectorized_sum_float32(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column;
    float *data;
    double sum = 0.0;
    int32 i;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid batch or column index")));

    column = batch->columns[column_index];
    if (column == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("column %d not initialized", column_index)));

    data = (float *)column->data;

#ifdef __AVX2__
    if (!batch->has_selection) {
        __m256d sum_vec = _mm256_setzero_pd();

        for (i = 0; i + VECTORIZED_FLOAT32_LANES <= batch->count; i += VECTORIZED_FLOAT32_LANES) {
            __m256 data_vec = _mm256_loadu_ps(data + i);
            /* Convert to double for precision */
            __m256d lo = _mm256_cvtps_pd(_mm256_castps256_ps128(data_vec));
            __m256d hi = _mm256_cvtps_pd(_mm256_extractf128_ps(data_vec, 1));
            sum_vec = _mm256_add_pd(sum_vec, lo);
            sum_vec = _mm256_add_pd(sum_vec, hi);
        }

        __m128d hi128 = _mm256_extractf128_pd(sum_vec, 1);
        __m128d lo128 = _mm256_castpd256_pd128(sum_vec);
        __m128d sum128 = _mm_add_pd(hi128, lo128);
        sum128 = _mm_hadd_pd(sum128, sum128);
        sum = _mm_cvtsd_f64(sum128);

        for (; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }

        if (column->has_nulls) {
            for (i = 0; i + VECTORIZED_FLOAT32_LANES <= batch->count;
                 i += VECTORIZED_FLOAT32_LANES) {
                for (int j = 0; j < VECTORIZED_FLOAT32_LANES; j++) {
                    int idx = i + j;
                    if (column->nulls[idx / 64] & (1ULL << (idx % 64)))
                        sum -= data[idx];
                }
            }
        }
    } else
#endif
    {
        if (batch->has_selection) {
            for (i = 0; i < batch->selected_count; i++) {
                uint32 idx = batch->selection[i];
                if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                    continue;
                sum += data[idx];
            }
        } else {
            for (i = 0; i < batch->count; i++) {
                if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                    continue;
                sum += data[i];
            }
        }
    }

    return sum;
}

int64 vectorized_count(VectorBatch *batch)
{
    if (batch == NULL)
        return 0;

    if (batch->has_selection)
        return batch->selected_count;
    else
        return batch->count;
}

int64 vectorized_count_column(VectorBatch *batch, int32 column_index)
{
    VectorColumn *column;
    int64 count = 0;
    int32 i;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    if (!column->has_nulls) {
        return batch->has_selection ? batch->selected_count : batch->count;
    }

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];
            if (!(column->nulls[idx / 64] & (1ULL << (idx % 64))))
                count++;
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (!(column->nulls[i / 64] & (1ULL << (i % 64))))
                count++;
        }
    }

    return count;
}

int64 vectorized_min_int64(VectorBatch *batch, int32 column_index, bool *found)
{
    VectorColumn *column;
    int64 *data;
    int64 min_val = INT64_MAX;
    int32 i;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    data = (int64 *)column->data;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            if (data[idx] < min_val) {
                min_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            if (data[i] < min_val) {
                min_val = data[i];
                *found = true;
            }
        }
    }

    return min_val;
}

int64 vectorized_max_int64(VectorBatch *batch, int32 column_index, bool *found)
{
    VectorColumn *column;
    int64 *data;
    int64 max_val = INT64_MIN;
    int32 i;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0;

    data = (int64 *)column->data;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            if (data[idx] > max_val) {
                max_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            if (data[i] > max_val) {
                max_val = data[i];
                *found = true;
            }
        }
    }

    return max_val;
}

double vectorized_min_float64(VectorBatch *batch, int32 column_index, bool *found)
{
    VectorColumn *column;
    double *data;
    double min_val = DBL_MAX;
    int32 i;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0.0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            if (data[idx] < min_val) {
                min_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            if (data[i] < min_val) {
                min_val = data[i];
                *found = true;
            }
        }
    }

    return min_val;
}

double vectorized_max_float64(VectorBatch *batch, int32 column_index, bool *found)
{
    VectorColumn *column;
    double *data;
    double max_val = -DBL_MAX;
    int32 i;

    *found = false;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0.0;

    column = batch->columns[column_index];
    if (column == NULL)
        return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (i = 0; i < batch->selected_count; i++) {
            uint32 idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;

            if (data[idx] > max_val) {
                max_val = data[idx];
                *found = true;
            }
        }
    } else {
        for (i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;

            if (data[i] > max_val) {
                max_val = data[i];
                *found = true;
            }
        }
    }

    return max_val;
}

void vectorized_agg_update(VectorizedAggState *state, VectorBatch *batch)
{
    bool found;

    switch (state->agg_type) {
    case VECTORIZED_AGG_COUNT:
        if (state->column_index < 0)
            state->count += vectorized_count(batch);
        else
            state->count += vectorized_count_column(batch, state->column_index);
        break;

    case VECTORIZED_AGG_SUM:
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            state->sum_int64 += vectorized_sum_int64(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            state->sum_int64 += vectorized_sum_int32(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_FLOAT64:
            state->sum_float64 += vectorized_sum_float64(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_FLOAT32:
            state->sum_float64 += vectorized_sum_float32(batch, state->column_index);
            state->has_value = true;
            break;
        default:
            break;
        }
        break;

    case VECTORIZED_AGG_MIN:
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP: {
            int64 val = vectorized_min_int64(batch, state->column_index, &found);
            if (found && (!state->has_value || val < state->min_int64)) {
                state->min_int64 = val;
                state->has_value = true;
            }
        } break;
        case VECTORIZED_TYPE_FLOAT64: {
            double val = vectorized_min_float64(batch, state->column_index, &found);
            if (found && (!state->has_value || val < state->min_float64)) {
                state->min_float64 = val;
                state->has_value = true;
            }
        } break;
        default:
            break;
        }
        break;

    case VECTORIZED_AGG_MAX:
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP: {
            int64 val = vectorized_max_int64(batch, state->column_index, &found);
            if (found && (!state->has_value || val > state->max_int64)) {
                state->max_int64 = val;
                state->has_value = true;
            }
        } break;
        case VECTORIZED_TYPE_FLOAT64: {
            double val = vectorized_max_float64(batch, state->column_index, &found);
            if (found && (!state->has_value || val > state->max_float64)) {
                state->max_float64 = val;
                state->has_value = true;
            }
        } break;
        default:
            break;
        }
        break;

    case VECTORIZED_AGG_AVG:
        state->count += vectorized_count_column(batch, state->column_index);
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            state->sum_float64 += (double)vectorized_sum_int64(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            state->sum_float64 += (double)vectorized_sum_int32(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_FLOAT64:
            state->sum_float64 += vectorized_sum_float64(batch, state->column_index);
            state->has_value = true;
            break;
        case VECTORIZED_TYPE_FLOAT32:
            state->sum_float64 += vectorized_sum_float32(batch, state->column_index);
            state->has_value = true;
            break;
        default:
            break;
        }
        break;
    }
}

Datum vectorized_agg_finalize(VectorizedAggState *state, bool *is_null)
{
    *is_null = false;

    switch (state->agg_type) {
    case VECTORIZED_AGG_COUNT:
        return Int64GetDatum(state->count);

    case VECTORIZED_AGG_SUM:
        if (!state->has_value) {
            *is_null = true;
            return (Datum)0;
        }
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_TIMESTAMP:
        case VECTORIZED_TYPE_DATE:
            return Int64GetDatum(state->sum_int64);
        case VECTORIZED_TYPE_FLOAT64:
        case VECTORIZED_TYPE_FLOAT32:
            return Float8GetDatum(state->sum_float64);
        default:
            *is_null = true;
            return (Datum)0;
        }

    case VECTORIZED_AGG_AVG:
        if (!state->has_value || state->count == 0) {
            *is_null = true;
            return (Datum)0;
        }
        return Float8GetDatum(state->sum_float64 / (double)state->count);

    case VECTORIZED_AGG_MIN:
        if (!state->has_value) {
            *is_null = true;
            return (Datum)0;
        }
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            return Int64GetDatum(state->min_int64);
        case VECTORIZED_TYPE_FLOAT64:
            return Float8GetDatum(state->min_float64);
        default:
            *is_null = true;
            return (Datum)0;
        }

    case VECTORIZED_AGG_MAX:
        if (!state->has_value) {
            *is_null = true;
            return (Datum)0;
        }
        switch (state->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            return Int64GetDatum(state->max_int64);
        case VECTORIZED_TYPE_FLOAT64:
            return Float8GetDatum(state->max_float64);
        default:
            *is_null = true;
            return (Datum)0;
        }

    default:
        *is_null = true;
        return (Datum)0;
    }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

int vectorized_batch_to_slots(VectorBatch *batch, TupleTableSlot **slots, TupleDesc tupdesc,
                              int max_slots)
{
    int32 count;
    int32 i, j;

    if (batch == NULL || slots == NULL || tupdesc == NULL)
        return 0;

    count = batch->has_selection ? batch->selected_count : batch->count;
    if (count > max_slots)
        count = max_slots;

    for (i = 0; i < count; i++) {
        uint32 row_idx = batch->has_selection ? batch->selection[i] : i;
        TupleTableSlot *slot = slots[i];

        ExecClearTuple(slot);

        for (j = 0; j < batch->column_count && j < tupdesc->natts; j++) {
            VectorColumn *column = batch->columns[j];

            if (column == NULL) {
                slot->tts_isnull[j] = true;
                slot->tts_values[j] = (Datum)0;
                continue;
            }

            /* Check for null */
            if (column->has_nulls && (column->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
                slot->tts_isnull[j] = true;
                slot->tts_values[j] = (Datum)0;
                continue;
            }

            slot->tts_isnull[j] = false;

            /* Extract value based on type */
            switch (column->data_type) {
            case VECTORIZED_TYPE_INT32:
            case VECTORIZED_TYPE_DATE:
                slot->tts_values[j] = Int32GetDatum(((int32 *)column->data)[row_idx]);
                break;
            case VECTORIZED_TYPE_INT64:
            case VECTORIZED_TYPE_TIMESTAMP:
                slot->tts_values[j] = Int64GetDatum(((int64 *)column->data)[row_idx]);
                break;
            case VECTORIZED_TYPE_FLOAT32:
                slot->tts_values[j] = Float4GetDatum(((float *)column->data)[row_idx]);
                break;
            case VECTORIZED_TYPE_FLOAT64:
                slot->tts_values[j] = Float8GetDatum(((double *)column->data)[row_idx]);
                break;
            case VECTORIZED_TYPE_BOOL:
                slot->tts_values[j] = BoolGetDatum(((bool *)column->data)[row_idx]);
                break;
            default:
                slot->tts_values[j] = ((Datum *)column->data)[row_idx];
                break;
            }
        }

        ExecStoreVirtualTuple(slot);
    }

    return count;
}

const char *vectorized_type_name(VectorizedDataType type)
{
    switch (type) {
    case VECTORIZED_TYPE_INT32:
        return "int32";
    case VECTORIZED_TYPE_INT64:
        return "int64";
    case VECTORIZED_TYPE_FLOAT32:
        return "float32";
    case VECTORIZED_TYPE_FLOAT64:
        return "float64";
    case VECTORIZED_TYPE_BOOL:
        return "bool";
    case VECTORIZED_TYPE_STRING:
        return "string";
    case VECTORIZED_TYPE_TIMESTAMP:
        return "timestamp";
    case VECTORIZED_TYPE_DATE:
        return "date";
    default:
        return "unknown";
    }
}

const char *vectorized_compare_op_name(VectorizedCompareOp op)
{
    switch (op) {
    case VECTORIZED_CMP_EQ:
        return "=";
    case VECTORIZED_CMP_NE:
        return "<>";
    case VECTORIZED_CMP_LT:
        return "<";
    case VECTORIZED_CMP_LE:
        return "<=";
    case VECTORIZED_CMP_GT:
        return ">";
    case VECTORIZED_CMP_GE:
        return ">=";
    case VECTORIZED_CMP_IS_NULL:
        return "IS NULL";
    case VECTORIZED_CMP_IS_NOT_NULL:
        return "IS NOT NULL";
    default:
        return "?";
    }
}

const char *vectorized_agg_type_name(VectorizedAggType type)
{
    switch (type) {
    case VECTORIZED_AGG_COUNT:
        return "COUNT";
    case VECTORIZED_AGG_SUM:
        return "SUM";
    case VECTORIZED_AGG_AVG:
        return "AVG";
    case VECTORIZED_AGG_MIN:
        return "MIN";
    case VECTORIZED_AGG_MAX:
        return "MAX";
    default:
        return "?";
    }
}

bool vectorized_cpu_supports_avx2(void)
{
#ifdef __AVX2__
    return true;
#else
    return false;
#endif
}

void vectorized_batch_debug_print(VectorBatch *batch)
{
    if (batch == NULL) {
        elog(DEBUG1, "VectorBatch: NULL");
        return;
    }

    elog(DEBUG1, "VectorBatch: capacity=%d count=%d columns=%d", batch->capacity, batch->count,
         batch->column_count);
    elog(DEBUG1, "  selection: has=%s count=%d", batch->has_selection ? "true" : "false",
         batch->selected_count);
    elog(DEBUG1, "  stats: processed=%ld filtered=%ld", batch->rows_processed,
         batch->rows_filtered);
}

/* ============================================================
 * JIT Integration Functions
 *
 * These functions integrate JIT compilation with the vectorized
 * executor, providing optimized execution paths when JIT is
 * enabled and beneficial.
 * ============================================================ */

#include "../jit/jit.h"

/* Global JIT context for vectorized execution */
static JitContext *vectorized_jit_ctx = NULL;

/*
 * Minimum batch size to consider JIT worthwhile
 */
#define VECTORIZED_JIT_MIN_BATCH_SIZE 64

/*
 * Minimum expression cost to consider JIT worthwhile
 */
#define VECTORIZED_JIT_MIN_EXPR_COST 5

/*
 * Get or create the global JIT context
 */
JitContext *vectorized_get_jit_context(void)
{
    if (vectorized_jit_ctx == NULL) {
        vectorized_jit_ctx = jit_get_global_context();
    }
    return vectorized_jit_ctx;
}

/*
 * Check if JIT should be used for this batch and expression
 */
bool vectorized_should_use_jit(VectorBatch *batch, int32 expr_cost)
{
    /* Check if JIT is enabled globally */
    if (!orochi_jit_enabled)
        return false;

    /* Batch must be large enough to amortize JIT overhead */
    if (batch == NULL || batch->count < VECTORIZED_JIT_MIN_BATCH_SIZE)
        return false;

    /* Expression must be complex enough to benefit from JIT */
    if (expr_cost < VECTORIZED_JIT_MIN_EXPR_COST)
        return false;

    return true;
}

/*
 * Execute filter with JIT acceleration
 */
void vectorized_filter_with_jit(VectorBatch *batch, VectorizedFilterState *filter,
                                JitCompiledFilter *jit_filter)
{
    /* If JIT filter is available and JIT is enabled, use it */
    if (jit_filter != NULL && orochi_jit_enabled && batch->count >= VECTORIZED_JIT_MIN_BATCH_SIZE) {
        jit_vectorized_filter(batch, jit_filter);
        return;
    }

    /* Fall back to interpreted execution */
    if (filter != NULL) {
        vectorized_filter_generic(batch, filter);
    }
}

/*
 * Execute aggregation with JIT acceleration
 */
void vectorized_aggregate_with_jit(VectorizedAggState *state, VectorBatch *batch,
                                   JitCompiledAgg *jit_agg)
{
    /* If JIT aggregation is available and JIT is enabled, use it */
    if (jit_agg != NULL && orochi_jit_enabled && batch->count >= VECTORIZED_JIT_MIN_BATCH_SIZE) {
        jit_vectorized_aggregate(state, batch, jit_agg);
        return;
    }

    /* Fall back to interpreted execution */
    vectorized_agg_update(state, batch);
}

/*
 * Create a JIT-compiled filter from a VectorizedFilterState
 * Returns NULL if JIT compilation is not possible or beneficial
 */
JitCompiledFilter *vectorized_compile_filter_jit(VectorizedFilterState *filter)
{
    JitContext *ctx;

    if (filter == NULL || !orochi_jit_enabled)
        return NULL;

    ctx = vectorized_get_jit_context();
    if (ctx == NULL)
        return NULL;

    return jit_compile_simple_filter(ctx, filter->column_index, filter->compare_op,
                                     filter->const_value, filter->data_type);
}

/*
 * Create a JIT-compiled aggregation from a VectorizedAggState
 * Returns NULL if JIT compilation is not possible or beneficial
 */
JitCompiledAgg *vectorized_compile_agg_jit(VectorizedAggState *agg)
{
    JitContext *ctx;

    if (agg == NULL || !orochi_jit_enabled)
        return NULL;

    ctx = vectorized_get_jit_context();
    if (ctx == NULL)
        return NULL;

    return jit_compile_agg(ctx, agg->agg_type, agg->column_index, agg->data_type);
}

/*
 * Free JIT-compiled filter
 */
void vectorized_free_filter_jit(JitCompiledFilter *jit_filter)
{
    if (jit_filter != NULL) {
        JitContext *ctx = vectorized_get_jit_context();
        jit_filter_free(ctx, jit_filter);
    }
}

/*
 * Free JIT-compiled aggregation
 */
void vectorized_free_agg_jit(JitCompiledAgg *jit_agg)
{
    if (jit_agg != NULL) {
        JitContext *ctx = vectorized_get_jit_context();
        jit_agg_free(ctx, jit_agg);
    }
}
