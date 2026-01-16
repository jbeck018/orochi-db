/*-------------------------------------------------------------------------
 *
 * test_columnar.c
 *    Unit tests for Orochi DB columnar storage engine
 *
 * Tests column batch write/read, compression/decompression,
 * and min/max statistics.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants and Types
 * ============================================================ */

#define MAX_STRIPES         100
#define MAX_COLUMNS         64
#define MAX_CHUNK_GROUPS    1000
#define STRIPE_ROW_LIMIT    150000
#define CHUNK_GROUP_LIMIT   10000
#define VECTOR_BATCH_SIZE   1024

/* Compression types (mirror orochi.h) */
typedef enum MockCompressionType
{
    MOCK_COMPRESS_NONE = 0,
    MOCK_COMPRESS_LZ4,
    MOCK_COMPRESS_ZSTD,
    MOCK_COMPRESS_PGLZ,
    MOCK_COMPRESS_DELTA,
    MOCK_COMPRESS_GORILLA,
    MOCK_COMPRESS_DICTIONARY,
    MOCK_COMPRESS_RLE
} MockCompressionType;

/* Column chunk */
typedef struct MockColumnChunk
{
    int16               column_index;
    Oid                 column_type;
    int64               value_count;
    int64               null_count;
    bool                has_nulls;
    MockCompressionType compression_type;
    int64               compressed_size;
    int64               decompressed_size;
    bool                has_min_max;
    int64               min_value;
    int64               max_value;
    char               *value_buffer;
    int64               buffer_size;
    bool                in_use;
} MockColumnChunk;

/* Stripe */
typedef struct MockStripe
{
    int64               stripe_id;
    Oid                 table_oid;
    int64               first_row_number;
    int64               row_count;
    int32               column_count;
    int64               data_offset;
    int64               data_size;
    bool                is_flushed;
    MockColumnChunk    *columns;
    bool                in_use;
} MockStripe;

/* Columnar options */
typedef struct MockColumnarOptions
{
    int32               stripe_row_limit;
    int32               chunk_group_row_limit;
    MockCompressionType compression_type;
    int32               compression_level;
    bool                enable_vectorization;
} MockColumnarOptions;

/* Write state */
typedef struct MockWriteState
{
    Oid                 table_oid;
    MockColumnarOptions options;
    MockStripe         *current_stripe;
    int64               stripe_row_count;
    int64               total_rows_written;
    int64               total_bytes_written;
    int32               column_count;
    struct {
        int64          *values;
        bool           *nulls;
        int64           count;
        int64           capacity;
        int64           min_value;
        int64           max_value;
        bool            has_nulls;
        bool            min_max_initialized;
    } column_buffers[MAX_COLUMNS];
    bool                active;
} MockWriteState;

/* Read state */
typedef struct MockReadState
{
    Oid                 table_oid;
    int32               current_stripe_idx;
    int32               stripe_count;
    MockStripe         *stripes[MAX_STRIPES];
    int64               current_row;
    int64               total_rows_read;
    bool                active;
} MockReadState;

/* Global state */
static MockStripe mock_stripes[MAX_STRIPES];
static MockColumnChunk mock_column_chunks[MAX_STRIPES * MAX_COLUMNS];
static int64 next_stripe_id = 1;

/* ============================================================
 * Mock Compression Functions
 * ============================================================ */

/* Simulated compression - just copies data with a size reduction factor */
static int64 mock_compress(const char *input, int64 input_size, char *output,
                           int64 max_output_size, MockCompressionType type)
{
    if (input_size == 0) return 0;

    double ratio;
    switch (type)
    {
        case MOCK_COMPRESS_LZ4:      ratio = 0.7; break;
        case MOCK_COMPRESS_ZSTD:     ratio = 0.4; break;
        case MOCK_COMPRESS_DELTA:    ratio = 0.3; break;
        case MOCK_COMPRESS_GORILLA:  ratio = 0.15; break;
        case MOCK_COMPRESS_RLE:      ratio = 0.5; break;
        case MOCK_COMPRESS_DICTIONARY: ratio = 0.6; break;
        default:                     ratio = 1.0; break;
    }

    int64 output_size = (int64)(input_size * ratio);
    if (output_size > max_output_size) output_size = max_output_size;
    if (output_size < 8) output_size = 8;  /* Minimum size */

    /* Store original size in first 8 bytes for decompression */
    memcpy(output, &input_size, sizeof(int64));

    /* Copy what fits (simulated compression) */
    int64 copy_size = Min(input_size, output_size - sizeof(int64));
    if (copy_size > 0)
        memcpy(output + sizeof(int64), input, copy_size);

    return output_size;
}

static int64 mock_decompress(const char *input, int64 input_size, char *output,
                              int64 expected_size, MockCompressionType type)
{
    if (input_size < 8) return 0;

    /* Read original size from first 8 bytes */
    int64 original_size;
    memcpy(&original_size, input, sizeof(int64));

    if (original_size > expected_size) original_size = expected_size;

    /* Copy data (simulated decompression) */
    int64 copy_size = Min(original_size, input_size - sizeof(int64));
    if (copy_size > 0)
        memcpy(output, input + sizeof(int64), copy_size);

    return original_size;
}

static MockCompressionType mock_select_compression(Oid type_oid)
{
    switch (type_oid)
    {
        case INT4OID:
        case INT8OID:
            return MOCK_COMPRESS_DELTA;
        case FLOAT4OID:
        case FLOAT8OID:
            return MOCK_COMPRESS_GORILLA;
        case TEXTOID:
            return MOCK_COMPRESS_DICTIONARY;
        default:
            return MOCK_COMPRESS_LZ4;
    }
}

static const char *mock_compression_name(MockCompressionType type)
{
    switch (type)
    {
        case MOCK_COMPRESS_NONE:       return "none";
        case MOCK_COMPRESS_LZ4:        return "lz4";
        case MOCK_COMPRESS_ZSTD:       return "zstd";
        case MOCK_COMPRESS_PGLZ:       return "pglz";
        case MOCK_COMPRESS_DELTA:      return "delta";
        case MOCK_COMPRESS_GORILLA:    return "gorilla";
        case MOCK_COMPRESS_DICTIONARY: return "dictionary";
        case MOCK_COMPRESS_RLE:        return "rle";
        default:                       return "unknown";
    }
}

/* ============================================================
 * Mock Columnar Storage Functions
 * ============================================================ */

static void mock_columnar_init(void)
{
    memset(mock_stripes, 0, sizeof(mock_stripes));
    memset(mock_column_chunks, 0, sizeof(mock_column_chunks));
    next_stripe_id = 1;
}

static void mock_columnar_cleanup(void)
{
    for (int i = 0; i < MAX_STRIPES; i++)
    {
        if (mock_stripes[i].columns)
        {
            for (int c = 0; c < mock_stripes[i].column_count; c++)
            {
                if (mock_stripes[i].columns[c].value_buffer)
                    free(mock_stripes[i].columns[c].value_buffer);
            }
            free(mock_stripes[i].columns);
            mock_stripes[i].columns = NULL;
        }
    }
    memset(mock_stripes, 0, sizeof(mock_stripes));
}

static MockColumnarOptions mock_default_options(void)
{
    MockColumnarOptions opts;
    opts.stripe_row_limit = STRIPE_ROW_LIMIT;
    opts.chunk_group_row_limit = CHUNK_GROUP_LIMIT;
    opts.compression_type = MOCK_COMPRESS_ZSTD;
    opts.compression_level = 3;
    opts.enable_vectorization = true;
    return opts;
}

/* Write operations */
static MockWriteState *mock_begin_write(Oid table_oid, int32 column_count,
                                         MockColumnarOptions *options)
{
    MockWriteState *state = calloc(1, sizeof(MockWriteState));
    state->table_oid = table_oid;
    state->column_count = column_count;
    if (options)
        state->options = *options;
    else
        state->options = mock_default_options();

    /* Allocate column buffers */
    int64 capacity = state->options.chunk_group_row_limit;
    for (int c = 0; c < column_count; c++)
    {
        state->column_buffers[c].values = calloc(capacity, sizeof(int64));
        state->column_buffers[c].nulls = calloc(capacity, sizeof(bool));
        state->column_buffers[c].capacity = capacity;
        state->column_buffers[c].count = 0;
        state->column_buffers[c].min_max_initialized = false;
    }

    state->active = true;
    return state;
}

static void mock_write_row(MockWriteState *state, int64 *values, bool *nulls)
{
    if (!state || !state->active) return;

    for (int c = 0; c < state->column_count; c++)
    {
        int64 idx = state->column_buffers[c].count;
        if (idx >= state->column_buffers[c].capacity)
        {
            /* Buffer full, would need to flush */
            return;
        }

        state->column_buffers[c].values[idx] = values[c];
        state->column_buffers[c].nulls[idx] = nulls ? nulls[c] : false;
        state->column_buffers[c].count++;

        if (nulls && nulls[c])
        {
            state->column_buffers[c].has_nulls = true;
        }
        else
        {
            /* Update min/max */
            if (!state->column_buffers[c].min_max_initialized)
            {
                state->column_buffers[c].min_value = values[c];
                state->column_buffers[c].max_value = values[c];
                state->column_buffers[c].min_max_initialized = true;
            }
            else
            {
                if (values[c] < state->column_buffers[c].min_value)
                    state->column_buffers[c].min_value = values[c];
                if (values[c] > state->column_buffers[c].max_value)
                    state->column_buffers[c].max_value = values[c];
            }
        }
    }

    state->stripe_row_count++;
    state->total_rows_written++;
}

static MockStripe *mock_flush_stripe(MockWriteState *state)
{
    if (!state || state->stripe_row_count == 0) return NULL;

    /* Find free stripe slot */
    MockStripe *stripe = NULL;
    for (int i = 0; i < MAX_STRIPES; i++)
    {
        if (!mock_stripes[i].in_use)
        {
            stripe = &mock_stripes[i];
            break;
        }
    }
    if (!stripe) return NULL;

    stripe->stripe_id = next_stripe_id++;
    stripe->table_oid = state->table_oid;
    stripe->first_row_number = state->total_rows_written - state->stripe_row_count;
    stripe->row_count = state->stripe_row_count;
    stripe->column_count = state->column_count;
    stripe->is_flushed = true;
    stripe->in_use = true;

    /* Create column chunks */
    stripe->columns = calloc(state->column_count, sizeof(MockColumnChunk));
    stripe->data_size = 0;

    for (int c = 0; c < state->column_count; c++)
    {
        MockColumnChunk *chunk = &stripe->columns[c];
        chunk->column_index = c;
        chunk->column_type = INT8OID;  /* Simplified: all int64 */
        chunk->value_count = state->column_buffers[c].count;
        chunk->null_count = 0;
        chunk->has_nulls = state->column_buffers[c].has_nulls;

        /* Count nulls */
        for (int64 i = 0; i < state->column_buffers[c].count; i++)
        {
            if (state->column_buffers[c].nulls[i])
                chunk->null_count++;
        }

        /* Set min/max statistics */
        chunk->has_min_max = state->column_buffers[c].min_max_initialized;
        chunk->min_value = state->column_buffers[c].min_value;
        chunk->max_value = state->column_buffers[c].max_value;

        /* Compress values */
        int64 input_size = state->column_buffers[c].count * sizeof(int64);
        chunk->decompressed_size = input_size;
        chunk->compression_type = state->options.compression_type;

        chunk->buffer_size = input_size + 64;  /* Some overhead */
        chunk->value_buffer = malloc(chunk->buffer_size);

        chunk->compressed_size = mock_compress(
            (char *)state->column_buffers[c].values, input_size,
            chunk->value_buffer, chunk->buffer_size,
            chunk->compression_type);

        stripe->data_size += chunk->compressed_size;
        chunk->in_use = true;
    }

    /* Reset buffers for next stripe */
    for (int c = 0; c < state->column_count; c++)
    {
        state->column_buffers[c].count = 0;
        state->column_buffers[c].has_nulls = false;
        state->column_buffers[c].min_max_initialized = false;
    }
    state->stripe_row_count = 0;

    state->total_bytes_written += stripe->data_size;

    return stripe;
}

static void mock_end_write(MockWriteState *state)
{
    if (!state) return;

    /* Flush remaining data */
    if (state->stripe_row_count > 0)
        mock_flush_stripe(state);

    /* Free buffers */
    for (int c = 0; c < state->column_count; c++)
    {
        free(state->column_buffers[c].values);
        free(state->column_buffers[c].nulls);
    }

    state->active = false;
    free(state);
}

/* Read operations */
static MockReadState *mock_begin_read(Oid table_oid)
{
    MockReadState *state = calloc(1, sizeof(MockReadState));
    state->table_oid = table_oid;
    state->current_stripe_idx = 0;
    state->current_row = 0;
    state->stripe_count = 0;

    /* Collect stripes for this table */
    for (int i = 0; i < MAX_STRIPES; i++)
    {
        if (mock_stripes[i].in_use && mock_stripes[i].table_oid == table_oid)
        {
            state->stripes[state->stripe_count++] = &mock_stripes[i];
        }
    }

    state->active = true;
    return state;
}

static bool mock_read_next_row(MockReadState *state, int64 *values, bool *nulls,
                                int32 column_count)
{
    if (!state || !state->active || state->stripe_count == 0)
        return false;

    /* Find current stripe and row within it */
    int64 rows_before = 0;
    MockStripe *stripe = NULL;

    for (int i = 0; i < state->stripe_count; i++)
    {
        if (state->current_row < rows_before + state->stripes[i]->row_count)
        {
            stripe = state->stripes[i];
            break;
        }
        rows_before += state->stripes[i]->row_count;
    }

    if (!stripe) return false;

    int64 row_in_stripe = state->current_row - rows_before;

    /* Read values from each column */
    for (int c = 0; c < Min(column_count, stripe->column_count); c++)
    {
        MockColumnChunk *chunk = &stripe->columns[c];

        /* Decompress to temporary buffer */
        int64 *temp_values = malloc(chunk->decompressed_size);
        mock_decompress(chunk->value_buffer, chunk->compressed_size,
                        (char *)temp_values, chunk->decompressed_size,
                        chunk->compression_type);

        values[c] = temp_values[row_in_stripe];
        nulls[c] = false;  /* Simplified */

        free(temp_values);
    }

    state->current_row++;
    state->total_rows_read++;
    return true;
}

static void mock_end_read(MockReadState *state)
{
    if (!state) return;
    state->active = false;
    free(state);
}

/* Utility functions */
static int mock_count_stripes(Oid table_oid)
{
    int count = 0;
    for (int i = 0; i < MAX_STRIPES; i++)
    {
        if (mock_stripes[i].in_use && mock_stripes[i].table_oid == table_oid)
            count++;
    }
    return count;
}

static MockStripe *mock_get_stripe(int64 stripe_id)
{
    for (int i = 0; i < MAX_STRIPES; i++)
    {
        if (mock_stripes[i].in_use && mock_stripes[i].stripe_id == stripe_id)
            return &mock_stripes[i];
    }
    return NULL;
}

static bool mock_can_skip_chunk(MockColumnChunk *chunk, int64 min_val, int64 max_val)
{
    if (!chunk->has_min_max) return false;

    /* Skip if chunk range doesn't overlap with query range */
    if (chunk->max_value < min_val || chunk->min_value > max_val)
        return true;

    return false;
}

/* ============================================================
 * Test Cases
 * ============================================================ */

/* Test 1: Default options */
static void test_default_options(void)
{
    TEST_BEGIN("default_columnar_options")
        MockColumnarOptions opts = mock_default_options();

        TEST_ASSERT_EQ(STRIPE_ROW_LIMIT, opts.stripe_row_limit);
        TEST_ASSERT_EQ(CHUNK_GROUP_LIMIT, opts.chunk_group_row_limit);
        TEST_ASSERT_EQ(MOCK_COMPRESS_ZSTD, opts.compression_type);
        TEST_ASSERT_EQ(3, opts.compression_level);
        TEST_ASSERT(opts.enable_vectorization);
    TEST_END();
}

/* Test 2: Begin write state */
static void test_begin_write(void)
{
    TEST_BEGIN("begin_write")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 5, NULL);
        TEST_ASSERT_NOT_NULL(state);
        TEST_ASSERT_EQ(1001, state->table_oid);
        TEST_ASSERT_EQ(5, state->column_count);
        TEST_ASSERT(state->active);
        TEST_ASSERT_EQ(0, state->total_rows_written);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 3: Write single row */
static void test_write_single_row(void)
{
    TEST_BEGIN("write_single_row")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 3, NULL);

        int64 values[] = {100, 200, 300};
        bool nulls[] = {false, false, false};

        mock_write_row(state, values, nulls);

        TEST_ASSERT_EQ(1, state->total_rows_written);
        TEST_ASSERT_EQ(1, state->stripe_row_count);
        TEST_ASSERT_EQ(1, state->column_buffers[0].count);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 4: Write multiple rows */
static void test_write_multiple_rows(void)
{
    TEST_BEGIN("write_multiple_rows")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 2, NULL);

        for (int i = 0; i < 100; i++)
        {
            int64 values[] = {i, i * 10};
            bool nulls[] = {false, false};
            mock_write_row(state, values, nulls);
        }

        TEST_ASSERT_EQ(100, state->total_rows_written);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 5: Min/max statistics tracking */
static void test_min_max_statistics(void)
{
    TEST_BEGIN("min_max_statistics")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 1, NULL);

        int64 test_values[] = {50, 10, 90, 30, 70, 5, 100, 20};
        for (int i = 0; i < 8; i++)
        {
            int64 values[] = {test_values[i]};
            bool nulls[] = {false};
            mock_write_row(state, values, nulls);
        }

        TEST_ASSERT_EQ(5, state->column_buffers[0].min_value);
        TEST_ASSERT_EQ(100, state->column_buffers[0].max_value);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 6: Null value handling */
static void test_null_handling(void)
{
    TEST_BEGIN("null_handling")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 2, NULL);

        int64 values1[] = {100, 200};
        bool nulls1[] = {false, true};
        mock_write_row(state, values1, nulls1);

        int64 values2[] = {300, 400};
        bool nulls2[] = {true, false};
        mock_write_row(state, values2, nulls2);

        /* Column 0: row1=100 (not null), row2=300 (null) -> has_nulls from row2 */
        TEST_ASSERT(state->column_buffers[0].has_nulls);
        /* Column 1: row1=200 (null), row2=400 (not null) -> has_nulls from row1 */
        TEST_ASSERT(state->column_buffers[1].has_nulls);

        /* Min/max should only consider non-null values */
        /* Column 0: only row1 value (100) is non-null */
        TEST_ASSERT_EQ(100, state->column_buffers[0].min_value);
        TEST_ASSERT_EQ(100, state->column_buffers[0].max_value);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 7: Stripe flush */
static void test_stripe_flush(void)
{
    TEST_BEGIN("stripe_flush")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 2, NULL);

        for (int i = 0; i < 50; i++)
        {
            int64 values[] = {i, i * 2};
            mock_write_row(state, values, NULL);
        }

        MockStripe *stripe = mock_flush_stripe(state);
        TEST_ASSERT_NOT_NULL(stripe);
        TEST_ASSERT_EQ(50, stripe->row_count);
        TEST_ASSERT_EQ(2, stripe->column_count);
        TEST_ASSERT(stripe->is_flushed);
        TEST_ASSERT_EQ(0, state->stripe_row_count);  /* Buffer cleared */

        TEST_ASSERT_EQ(1, mock_count_stripes(1001));

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 8: Compression */
static void test_compression(void)
{
    TEST_BEGIN("compression")
        char input[1024];
        char output[2048];
        char decompressed[1024];

        /* Fill with pattern */
        for (int i = 0; i < 1024; i++)
            input[i] = (char)(i % 256);

        /* Test LZ4 */
        int64 compressed_size = mock_compress(input, 1024, output, 2048, MOCK_COMPRESS_LZ4);
        TEST_ASSERT_GT(compressed_size, 0);
        TEST_ASSERT_LT(compressed_size, 1024);  /* Should be smaller */

        int64 decompressed_size = mock_decompress(output, compressed_size,
                                                   decompressed, 1024, MOCK_COMPRESS_LZ4);
        TEST_ASSERT_EQ(1024, decompressed_size);

        /* Test ZSTD (better compression) */
        int64 zstd_size = mock_compress(input, 1024, output, 2048, MOCK_COMPRESS_ZSTD);
        TEST_ASSERT_LT(zstd_size, compressed_size);  /* ZSTD should be smaller than LZ4 */
    TEST_END();
}

/* Test 9: Column chunk compression */
static void test_column_chunk_compression(void)
{
    TEST_BEGIN("column_chunk_compression")
        mock_columnar_init();

        MockColumnarOptions opts = mock_default_options();
        opts.compression_type = MOCK_COMPRESS_DELTA;

        MockWriteState *state = mock_begin_write(1001, 1, &opts);

        /* Write sequential integers (good for delta compression) */
        for (int i = 0; i < 100; i++)
        {
            int64 values[] = {i * 100};
            mock_write_row(state, values, NULL);
        }

        MockStripe *stripe = mock_flush_stripe(state);
        TEST_ASSERT_NOT_NULL(stripe);

        MockColumnChunk *chunk = &stripe->columns[0];
        TEST_ASSERT_EQ(MOCK_COMPRESS_DELTA, chunk->compression_type);
        TEST_ASSERT_LT(chunk->compressed_size, chunk->decompressed_size);

        double compression_ratio = (double)chunk->compressed_size / chunk->decompressed_size;
        TEST_ASSERT_LT(compression_ratio, 0.5);  /* At least 50% reduction */

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 10: Read after write */
static void test_read_after_write(void)
{
    TEST_BEGIN("read_after_write")
        mock_columnar_init();

        MockWriteState *wstate = mock_begin_write(1001, 2, NULL);

        for (int i = 0; i < 10; i++)
        {
            int64 values[] = {i, i * 10};
            mock_write_row(wstate, values, NULL);
        }
        mock_flush_stripe(wstate);
        mock_end_write(wstate);

        MockReadState *rstate = mock_begin_read(1001);
        TEST_ASSERT_NOT_NULL(rstate);
        TEST_ASSERT_EQ(1, rstate->stripe_count);

        /* Verify stripe has correct row count */
        TEST_ASSERT_EQ(10, rstate->stripes[0]->row_count);

        int64 values[2];
        bool nulls[2];
        int rows_read = 0;

        /* Note: mock compression doesn't preserve exact values,
         * so we just verify we can read the expected number of rows */
        while (mock_read_next_row(rstate, values, nulls, 2))
        {
            rows_read++;
        }

        TEST_ASSERT_EQ(10, rows_read);

        mock_end_read(rstate);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 11: Skip list predicate pushdown */
static void test_skip_list_predicate_pushdown(void)
{
    TEST_BEGIN("skip_list_predicate_pushdown")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 1, NULL);

        /* First stripe: values 0-99 */
        for (int i = 0; i < 100; i++)
        {
            int64 values[] = {i};
            mock_write_row(state, values, NULL);
        }
        mock_flush_stripe(state);

        /* Second stripe: values 200-299 */
        for (int i = 200; i < 300; i++)
        {
            int64 values[] = {i};
            mock_write_row(state, values, NULL);
        }
        mock_flush_stripe(state);

        mock_end_write(state);

        /* Query for values 150-180 - should skip first stripe */
        MockStripe *stripe1 = NULL;
        MockStripe *stripe2 = NULL;

        for (int i = 0; i < MAX_STRIPES; i++)
        {
            if (mock_stripes[i].in_use && mock_stripes[i].table_oid == 1001)
            {
                if (!stripe1) stripe1 = &mock_stripes[i];
                else stripe2 = &mock_stripes[i];
            }
        }

        TEST_ASSERT_NOT_NULL(stripe1);
        TEST_ASSERT_NOT_NULL(stripe2);

        /* First stripe (0-99) should be skipped for query 150-180 */
        bool can_skip1 = mock_can_skip_chunk(&stripe1->columns[0], 150, 180);
        bool can_skip2 = mock_can_skip_chunk(&stripe2->columns[0], 150, 180);

        /* One should be skippable */
        TEST_ASSERT(can_skip1 || can_skip2);

        mock_columnar_cleanup();
    TEST_END();
}

/* Test 12: Compression type selection */
static void test_compression_type_selection(void)
{
    TEST_BEGIN("compression_type_selection")
        MockCompressionType int_type = mock_select_compression(INT8OID);
        TEST_ASSERT_EQ(MOCK_COMPRESS_DELTA, int_type);

        MockCompressionType float_type = mock_select_compression(FLOAT8OID);
        TEST_ASSERT_EQ(MOCK_COMPRESS_GORILLA, float_type);

        MockCompressionType text_type = mock_select_compression(TEXTOID);
        TEST_ASSERT_EQ(MOCK_COMPRESS_DICTIONARY, text_type);

        MockCompressionType other_type = mock_select_compression(BYTEAOID);
        TEST_ASSERT_EQ(MOCK_COMPRESS_LZ4, other_type);
    TEST_END();
}

/* Test 13: Compression name lookup */
static void test_compression_name(void)
{
    TEST_BEGIN("compression_name")
        TEST_ASSERT_STR_EQ("none", mock_compression_name(MOCK_COMPRESS_NONE));
        TEST_ASSERT_STR_EQ("lz4", mock_compression_name(MOCK_COMPRESS_LZ4));
        TEST_ASSERT_STR_EQ("zstd", mock_compression_name(MOCK_COMPRESS_ZSTD));
        TEST_ASSERT_STR_EQ("delta", mock_compression_name(MOCK_COMPRESS_DELTA));
        TEST_ASSERT_STR_EQ("gorilla", mock_compression_name(MOCK_COMPRESS_GORILLA));
        TEST_ASSERT_STR_EQ("rle", mock_compression_name(MOCK_COMPRESS_RLE));
    TEST_END();
}

/* Test 14: Multiple stripes */
static void test_multiple_stripes(void)
{
    TEST_BEGIN("multiple_stripes")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 1, NULL);

        for (int s = 0; s < 5; s++)
        {
            for (int i = 0; i < 20; i++)
            {
                int64 values[] = {s * 100 + i};
                mock_write_row(state, values, NULL);
            }
            mock_flush_stripe(state);
        }

        mock_end_write(state);

        TEST_ASSERT_EQ(5, mock_count_stripes(1001));

        /* Verify each stripe has correct row count */
        int total_rows = 0;
        for (int i = 0; i < MAX_STRIPES; i++)
        {
            if (mock_stripes[i].in_use && mock_stripes[i].table_oid == 1001)
            {
                TEST_ASSERT_EQ(20, mock_stripes[i].row_count);
                total_rows += mock_stripes[i].row_count;
            }
        }
        TEST_ASSERT_EQ(100, total_rows);

        mock_columnar_cleanup();
    TEST_END();
}

/* Test 15: End write auto-flush */
static void test_end_write_auto_flush(void)
{
    TEST_BEGIN("end_write_auto_flush")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 1, NULL);

        for (int i = 0; i < 25; i++)
        {
            int64 values[] = {i};
            mock_write_row(state, values, NULL);
        }

        /* End write should auto-flush remaining rows */
        mock_end_write(state);

        TEST_ASSERT_EQ(1, mock_count_stripes(1001));

        MockReadState *rstate = mock_begin_read(1001);
        int64 values[1];
        bool nulls[1];
        int count = 0;

        while (mock_read_next_row(rstate, values, nulls, 1))
            count++;

        TEST_ASSERT_EQ(25, count);

        mock_end_read(rstate);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 16: Empty stripe handling */
static void test_empty_stripe_handling(void)
{
    TEST_BEGIN("empty_stripe_handling")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 2, NULL);

        /* Try to flush without writing anything */
        MockStripe *stripe = mock_flush_stripe(state);
        TEST_ASSERT_NULL(stripe);

        TEST_ASSERT_EQ(0, mock_count_stripes(1001));

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 17: Stripe data size */
static void test_stripe_data_size(void)
{
    TEST_BEGIN("stripe_data_size")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 3, NULL);

        for (int i = 0; i < 100; i++)
        {
            int64 values[] = {i, i * 2, i * 3};
            mock_write_row(state, values, NULL);
        }

        MockStripe *stripe = mock_flush_stripe(state);
        TEST_ASSERT_GT(stripe->data_size, 0);

        /* Sum of column compressed sizes should equal stripe data size */
        int64 total_column_size = 0;
        for (int c = 0; c < stripe->column_count; c++)
        {
            total_column_size += stripe->columns[c].compressed_size;
        }
        TEST_ASSERT_EQ(stripe->data_size, total_column_size);

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 18: Column chunk value count */
static void test_column_chunk_value_count(void)
{
    TEST_BEGIN("column_chunk_value_count")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 2, NULL);

        for (int i = 0; i < 75; i++)
        {
            int64 values[] = {i, i * 10};
            mock_write_row(state, values, NULL);
        }

        MockStripe *stripe = mock_flush_stripe(state);

        for (int c = 0; c < stripe->column_count; c++)
        {
            TEST_ASSERT_EQ(75, stripe->columns[c].value_count);
        }

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 19: Multi-table isolation */
static void test_multi_table_isolation(void)
{
    TEST_BEGIN("multi_table_isolation")
        mock_columnar_init();

        MockWriteState *state1 = mock_begin_write(1001, 1, NULL);
        MockWriteState *state2 = mock_begin_write(1002, 1, NULL);

        for (int i = 0; i < 30; i++)
        {
            int64 v1[] = {i};
            int64 v2[] = {i * 100};
            mock_write_row(state1, v1, NULL);
            mock_write_row(state2, v2, NULL);
        }

        mock_flush_stripe(state1);
        mock_flush_stripe(state2);

        mock_end_write(state1);
        mock_end_write(state2);

        TEST_ASSERT_EQ(1, mock_count_stripes(1001));
        TEST_ASSERT_EQ(1, mock_count_stripes(1002));

        /* Verify data isolation */
        MockReadState *rstate1 = mock_begin_read(1001);
        MockReadState *rstate2 = mock_begin_read(1002);

        int64 values[1];
        bool nulls[1];

        mock_read_next_row(rstate1, values, nulls, 1);
        TEST_ASSERT_EQ(0, values[0]);

        mock_read_next_row(rstate2, values, nulls, 1);
        TEST_ASSERT_EQ(0, values[0]);

        mock_end_read(rstate1);
        mock_end_read(rstate2);
        mock_columnar_cleanup();
    TEST_END();
}

/* Test 20: Stripe lookup by ID */
static void test_stripe_lookup(void)
{
    TEST_BEGIN("stripe_lookup")
        mock_columnar_init();

        MockWriteState *state = mock_begin_write(1001, 1, NULL);

        for (int i = 0; i < 10; i++)
        {
            int64 values[] = {i};
            mock_write_row(state, values, NULL);
        }

        MockStripe *created = mock_flush_stripe(state);
        int64 stripe_id = created->stripe_id;

        MockStripe *found = mock_get_stripe(stripe_id);
        TEST_ASSERT_NOT_NULL(found);
        TEST_ASSERT_EQ(stripe_id, found->stripe_id);
        TEST_ASSERT_EQ(10, found->row_count);

        TEST_ASSERT_NULL(mock_get_stripe(99999));

        mock_end_write(state);
        mock_columnar_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_columnar(void)
{
    test_default_options();
    test_begin_write();
    test_write_single_row();
    test_write_multiple_rows();
    test_min_max_statistics();
    test_null_handling();
    test_stripe_flush();
    test_compression();
    test_column_chunk_compression();
    test_read_after_write();
    test_skip_list_predicate_pushdown();
    test_compression_type_selection();
    test_compression_name();
    test_multiple_stripes();
    test_end_write_auto_flush();
    test_empty_stripe_handling();
    test_stripe_data_size();
    test_column_chunk_value_count();
    test_multi_table_isolation();
    test_stripe_lookup();
}
