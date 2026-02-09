/*-------------------------------------------------------------------------
 *
 * test_vectorized.c
 *    Unit tests for Orochi DB vectorized execution engine
 *
 * Tests cover:
 *   - VectorBatch creation and destruction
 *   - Vectorized filter operations (int64, float64)
 *   - Vectorized aggregations (SUM, COUNT, MIN, MAX)
 *   - Selection vector logic
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

#include "test_framework.h"
#include "mocks/postgres_mock.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

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
    int64_t rows_processed;
    int64_t rows_filtered;
} VectorBatch;

/* ============================================================
 * Aligned Memory Allocation
 * ============================================================ */

static void *
vectorized_palloc_aligned(size_t size, size_t alignment)
{
    void *ptr;
    void *aligned_ptr;
    size_t total_size;

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

    if (capacity < 1)
        capacity = VECTORIZED_BATCH_SIZE;

    if (column_count < 1 || column_count > VECTORIZED_MAX_COLUMNS)
        return NULL;

    batch = palloc0(sizeof(VectorBatch));
    batch->capacity = capacity;
    batch->count = 0;
    batch->column_count = column_count;

    batch->columns = palloc0(column_count * sizeof(VectorColumn *));

    batch->selection = vectorized_palloc_aligned(capacity * sizeof(uint32_t),
                                                  VECTORIZED_ALIGNMENT);
    batch->selected_count = 0;
    batch->has_selection = false;

    batch->rows_processed = 0;
    batch->rows_filtered = 0;

    return batch;
}

static void
vector_batch_free(VectorBatch *batch)
{
    if (batch == NULL)
        return;

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
    size_t data_size;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return NULL;

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

    for (int32_t i = 0; i < batch->count; i++) {
        if (column->has_nulls &&
            (column->nulls[i / 64] & (1ULL << (i % 64)))) {
            mask[i] = false;
            continue;
        }

        switch (op) {
            case VECTORIZED_CMP_EQ: mask[i] = (data[i] == value); break;
            case VECTORIZED_CMP_NE: mask[i] = (data[i] != value); break;
            case VECTORIZED_CMP_LT: mask[i] = (data[i] < value);  break;
            case VECTORIZED_CMP_LE: mask[i] = (data[i] <= value); break;
            case VECTORIZED_CMP_GT: mask[i] = (data[i] > value);  break;
            case VECTORIZED_CMP_GE: mask[i] = (data[i] >= value); break;
            default: mask[i] = false; break;
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

    for (int32_t i = 0; i < batch->count; i++) {
        if (column->has_nulls &&
            (column->nulls[i / 64] & (1ULL << (i % 64)))) {
            mask[i] = false;
            continue;
        }

        switch (op) {
            case VECTORIZED_CMP_EQ: mask[i] = (data[i] == value); break;
            case VECTORIZED_CMP_NE: mask[i] = (data[i] != value); break;
            case VECTORIZED_CMP_LT: mask[i] = (data[i] < value);  break;
            case VECTORIZED_CMP_LE: mask[i] = (data[i] <= value); break;
            case VECTORIZED_CMP_GT: mask[i] = (data[i] > value);  break;
            case VECTORIZED_CMP_GE: mask[i] = (data[i] >= value); break;
            default: mask[i] = false; break;
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
    if (column == NULL) return 0;

    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            sum += data[idx];
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
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
    if (column == NULL) return 0.0;

    data = (double *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64))))
                continue;
            sum += data[idx];
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64))))
                continue;
            sum += data[i];
        }
    }

    return sum;
}

static int64_t
vectorized_count(VectorBatch *batch)
{
    if (batch == NULL) return 0;
    return batch->has_selection ? batch->selected_count : batch->count;
}

static int64_t
vectorized_count_column(VectorBatch *batch, int32_t column_index)
{
    VectorColumn *column;
    int64_t count = 0;

    if (batch == NULL || column_index < 0 || column_index >= batch->column_count)
        return 0;

    column = batch->columns[column_index];
    if (column == NULL) return 0;

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
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count) return 0;
    column = batch->columns[column_index];
    if (column == NULL) return 0;
    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64)))) continue;
            if (data[idx] < min_val) { min_val = data[idx]; *found = true; }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) continue;
            if (data[i] < min_val) { min_val = data[i]; *found = true; }
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
    if (batch == NULL || column_index < 0 || column_index >= batch->column_count) return 0;
    column = batch->columns[column_index];
    if (column == NULL) return 0;
    data = (int64_t *)column->data;

    if (batch->has_selection) {
        for (int32_t i = 0; i < batch->selected_count; i++) {
            uint32_t idx = batch->selection[i];
            if (column->has_nulls && (column->nulls[idx / 64] & (1ULL << (idx % 64)))) continue;
            if (data[idx] > max_val) { max_val = data[idx]; *found = true; }
        }
    } else {
        for (int32_t i = 0; i < batch->count; i++) {
            if (column->has_nulls && (column->nulls[i / 64] & (1ULL << (i % 64)))) continue;
            if (data[i] > max_val) { max_val = data[i]; *found = true; }
        }
    }
    return max_val;
}

/* ============================================================
 * Unit Tests
 * ============================================================ */

static void test_batch_create_destroy(void)
{
    TEST_BEGIN("batch_create_destroy")
        VectorBatch *batch = vector_batch_create(100, 3);
        TEST_ASSERT_NOT_NULL(batch);
        TEST_ASSERT_EQ(100, batch->capacity);
        TEST_ASSERT_EQ(3, batch->column_count);
        TEST_ASSERT_EQ(0, batch->count);
        TEST_ASSERT_FALSE(batch->has_selection);
        vector_batch_free(batch);
    TEST_END();

    TEST_BEGIN("batch_default_capacity")
        VectorBatch *batch = vector_batch_create(0, 2);
        TEST_ASSERT_NOT_NULL(batch);
        TEST_ASSERT_EQ(VECTORIZED_BATCH_SIZE, batch->capacity);
        vector_batch_free(batch);
    TEST_END();
}

static void test_batch_add_column(void)
{
    TEST_BEGIN("batch_add_column_types")
        VectorBatch *batch = vector_batch_create(100, 3);
        TEST_ASSERT_NOT_NULL(batch);

        VectorColumn *col = vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
        TEST_ASSERT_NOT_NULL(col);
        TEST_ASSERT_EQ(VECTORIZED_TYPE_INT64, col->data_type);
        TEST_ASSERT_EQ((int)sizeof(int64_t), col->type_len);
        TEST_ASSERT_NOT_NULL(col->data);
        TEST_ASSERT_NOT_NULL(col->nulls);

        col = vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);
        TEST_ASSERT_NOT_NULL(col);
        TEST_ASSERT_EQ(VECTORIZED_TYPE_FLOAT64, col->data_type);

        col = vector_batch_add_column(batch, 2, VECTORIZED_TYPE_INT32, INT4OID);
        TEST_ASSERT_NOT_NULL(col);
        TEST_ASSERT_EQ((int)sizeof(int32_t), col->type_len);

        vector_batch_free(batch);
    TEST_END();
}

static void test_batch_reset_op(void)
{
    TEST_BEGIN("batch_reset")
        VectorBatch *batch = vector_batch_create(100, 2);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
        vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

        batch->count = 50;
        batch->has_selection = true;
        batch->selected_count = 25;
        batch->columns[0]->has_nulls = true;

        vector_batch_reset(batch);

        TEST_ASSERT_EQ(0, batch->count);
        TEST_ASSERT_FALSE(batch->has_selection);
        TEST_ASSERT_EQ(0, batch->selected_count);
        TEST_ASSERT_FALSE(batch->columns[0]->has_nulls);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_int64_eq(void)
{
    TEST_BEGIN("filter_int64_eq")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i;
        batch->count = 16;

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_EQ, 5);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(1, batch->selected_count);
        TEST_ASSERT_EQ(5, batch->selection[0]);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_int64_gt(void)
{
    TEST_BEGIN("filter_int64_gt")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i;
        batch->count = 16;

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_GT, 10);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(5, batch->selected_count);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_int64_le(void)
{
    TEST_BEGIN("filter_int64_le")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i;
        batch->count = 16;

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 3);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(4, batch->selected_count);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_int64_with_nulls(void)
{
    TEST_BEGIN("filter_int64_with_nulls")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i;
        batch->count = 16;

        batch->columns[0]->has_nulls = true;
        batch->columns[0]->nulls[0] |= (1ULL << 3);
        batch->columns[0]->nulls[0] |= (1ULL << 7);
        batch->columns[0]->nulls[0] |= (1ULL << 11);

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 5);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(5, batch->selected_count);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_float64_eq(void)
{
    TEST_BEGIN("filter_float64_eq")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

        double *data = (double *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i * 1.5;
        batch->count = 16;

        vectorized_filter_float64(batch, 0, VECTORIZED_CMP_EQ, 7.5);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(1, batch->selected_count);
        TEST_ASSERT_EQ(5, batch->selection[0]);

        vector_batch_free(batch);
    TEST_END();
}

static void test_filter_float64_ge(void)
{
    TEST_BEGIN("filter_float64_ge")
        VectorBatch *batch = vector_batch_create(16, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

        double *data = (double *)batch->columns[0]->data;
        for (int i = 0; i < 16; i++) data[i] = i * 1.0;
        batch->count = 16;

        vectorized_filter_float64(batch, 0, VECTORIZED_CMP_GE, 12.0);

        TEST_ASSERT(batch->has_selection);
        TEST_ASSERT_EQ(4, batch->selected_count);

        vector_batch_free(batch);
    TEST_END();
}

static void test_sum_int64(void)
{
    TEST_BEGIN("sum_int64")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 100; i++) data[i] = i + 1;
        batch->count = 100;

        int64_t sum = vectorized_sum_int64(batch, 0);
        TEST_ASSERT_EQ(5050, sum);

        vector_batch_free(batch);
    TEST_END();
}

static void test_sum_int64_with_selection(void)
{
    TEST_BEGIN("sum_int64_with_selection")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 100; i++) data[i] = i + 1;
        batch->count = 100;

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LE, 10);
        int64_t sum = vectorized_sum_int64(batch, 0);
        TEST_ASSERT_EQ(55, sum);

        vector_batch_free(batch);
    TEST_END();
}

static void test_sum_int64_with_nulls(void)
{
    TEST_BEGIN("sum_int64_with_nulls")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 10; i++) data[i] = i + 1;
        batch->count = 10;

        batch->columns[0]->has_nulls = true;
        batch->columns[0]->nulls[0] |= (1ULL << 4);
        batch->columns[0]->nulls[0] |= (1ULL << 9);

        int64_t sum = vectorized_sum_int64(batch, 0);
        TEST_ASSERT_EQ(40, sum);

        vector_batch_free(batch);
    TEST_END();
}

static void test_sum_float64(void)
{
    TEST_BEGIN("sum_float64")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

        double *data = (double *)batch->columns[0]->data;
        for (int i = 0; i < 100; i++) data[i] = (i + 1) * 0.5;
        batch->count = 100;

        double sum = vectorized_sum_float64(batch, 0);
        TEST_ASSERT_NEAR(2525.0, sum, 0.001);

        vector_batch_free(batch);
    TEST_END();
}

static void test_count_ops(void)
{
    TEST_BEGIN("count_basic")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
        batch->count = 100;

        TEST_ASSERT_EQ(100, vectorized_count(batch));

        batch->has_selection = true;
        batch->selected_count = 25;
        TEST_ASSERT_EQ(25, vectorized_count(batch));

        vector_batch_free(batch);
    TEST_END();
}

static void test_count_column_ops(void)
{
    TEST_BEGIN("count_column_with_nulls")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 10; i++) data[i] = i;
        batch->count = 10;

        TEST_ASSERT_EQ(10, vectorized_count_column(batch, 0));

        batch->columns[0]->has_nulls = true;
        batch->columns[0]->nulls[0] |= (1ULL << 2);
        batch->columns[0]->nulls[0] |= (1ULL << 5);
        batch->columns[0]->nulls[0] |= (1ULL << 8);

        TEST_ASSERT_EQ(7, vectorized_count_column(batch, 0));

        vector_batch_free(batch);
    TEST_END();
}

static void test_min_max_int64(void)
{
    TEST_BEGIN("min_int64")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        int64_t vals[] = {50, 30, 70, 10, 90, 40, 60, 20, 80, 100};
        memcpy(data, vals, sizeof(vals));
        batch->count = 10;

        bool found;
        int64_t min_val = vectorized_min_int64(batch, 0, &found);
        TEST_ASSERT(found);
        TEST_ASSERT_EQ(10, min_val);

        vector_batch_free(batch);
    TEST_END();

    TEST_BEGIN("max_int64")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        int64_t vals[] = {50, 30, 70, 10, 90, 40, 60, 20, 80, 100};
        memcpy(data, vals, sizeof(vals));
        batch->count = 10;

        bool found;
        int64_t max_val = vectorized_max_int64(batch, 0, &found);
        TEST_ASSERT(found);
        TEST_ASSERT_EQ(100, max_val);

        vector_batch_free(batch);
    TEST_END();
}

static void test_min_max_with_nulls(void)
{
    TEST_BEGIN("min_max_with_nulls")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        int64_t vals[] = {50, 5, 70, 10, 90, 40, 60, 20, 80, 200};
        memcpy(data, vals, sizeof(vals));
        batch->count = 10;

        batch->columns[0]->has_nulls = true;
        batch->columns[0]->nulls[0] |= (1ULL << 1);
        batch->columns[0]->nulls[0] |= (1ULL << 9);

        bool found;
        int64_t min_val = vectorized_min_int64(batch, 0, &found);
        TEST_ASSERT(found);
        TEST_ASSERT_EQ(10, min_val);

        int64_t max_val = vectorized_max_int64(batch, 0, &found);
        TEST_ASSERT(found);
        TEST_ASSERT_EQ(90, max_val);

        vector_batch_free(batch);
    TEST_END();
}

static void test_selection_chaining(void)
{
    TEST_BEGIN("selection_chaining")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 100; i++) data[i] = i;
        batch->count = 100;

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_GE, 20);
        TEST_ASSERT_EQ(80, batch->selected_count);

        vectorized_filter_int64(batch, 0, VECTORIZED_CMP_LT, 30);
        TEST_ASSERT_EQ(10, batch->selected_count);

        for (int i = 0; i < 10; i++) {
            TEST_ASSERT_EQ(20 + i, batch->selection[i]);
        }

        vector_batch_free(batch);
    TEST_END();
}

static void test_large_batch(void)
{
    TEST_BEGIN("large_batch_sum")
        VectorBatch *batch = vector_batch_create(VECTORIZED_BATCH_SIZE, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < VECTORIZED_BATCH_SIZE; i++) data[i] = 1;
        batch->count = VECTORIZED_BATCH_SIZE;

        int64_t sum = vectorized_sum_int64(batch, 0);
        TEST_ASSERT_EQ(VECTORIZED_BATCH_SIZE, sum);

        vector_batch_free(batch);
    TEST_END();
}

static void test_memory_alignment(void)
{
    TEST_BEGIN("memory_alignment")
        VectorBatch *batch = vector_batch_create(100, 2);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
        vector_batch_add_column(batch, 1, VECTORIZED_TYPE_FLOAT64, FLOAT8OID);

        TEST_ASSERT_MSG(((uintptr_t)batch->columns[0]->data & (VECTORIZED_ALIGNMENT - 1)) == 0,
                        "Int64 data buffer 32-byte aligned");
        TEST_ASSERT_MSG(((uintptr_t)batch->columns[1]->data & (VECTORIZED_ALIGNMENT - 1)) == 0,
                        "Float64 data buffer 32-byte aligned");
        TEST_ASSERT_MSG(((uintptr_t)batch->selection & (VECTORIZED_ALIGNMENT - 1)) == 0,
                        "Selection vector 32-byte aligned");

        vector_batch_free(batch);
    TEST_END();
}

static void test_empty_batch(void)
{
    TEST_BEGIN("empty_batch")
        VectorBatch *batch = vector_batch_create(100, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);
        batch->count = 0;

        TEST_ASSERT_EQ(0, vectorized_count(batch));
        TEST_ASSERT_EQ(0, vectorized_sum_int64(batch, 0));

        vector_batch_free(batch);
    TEST_END();
}

static void test_all_null_column(void)
{
    TEST_BEGIN("all_null_column")
        VectorBatch *batch = vector_batch_create(10, 1);
        vector_batch_add_column(batch, 0, VECTORIZED_TYPE_INT64, INT8OID);

        int64_t *data = (int64_t *)batch->columns[0]->data;
        for (int i = 0; i < 10; i++) data[i] = i + 1;
        batch->count = 10;

        batch->columns[0]->has_nulls = true;
        batch->columns[0]->nulls[0] = 0x3FF;

        TEST_ASSERT_EQ(0, vectorized_count_column(batch, 0));
        TEST_ASSERT_EQ(0, vectorized_sum_int64(batch, 0));

        bool found;
        vectorized_min_int64(batch, 0, &found);
        TEST_ASSERT_FALSE(found);

        vector_batch_free(batch);
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_vectorized(void)
{
    /* Batch management tests */
    test_batch_create_destroy();
    test_batch_add_column();
    test_batch_reset_op();

    /* Filter tests */
    test_filter_int64_eq();
    test_filter_int64_gt();
    test_filter_int64_le();
    test_filter_int64_with_nulls();
    test_filter_float64_eq();
    test_filter_float64_ge();

    /* Aggregation tests */
    test_sum_int64();
    test_sum_int64_with_selection();
    test_sum_int64_with_nulls();
    test_sum_float64();
    test_count_ops();
    test_count_column_ops();
    test_min_max_int64();
    test_min_max_with_nulls();

    /* Advanced tests */
    test_selection_chaining();
    test_large_batch();
    test_memory_alignment();
    test_empty_batch();
    test_all_null_column();
}
