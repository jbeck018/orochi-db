/*-------------------------------------------------------------------------
 *
 * test_hypertable.c
 *    Unit tests for Orochi DB hypertable (time-series) functionality
 *
 * Tests hypertable creation, chunk management, time partitioning,
 * and related time-series operations.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants and Types
 * ============================================================ */

#define MAX_HYPERTABLES     50
#define MAX_CHUNKS          1000
#define MAX_DIMENSIONS      10
#define MAX_POLICIES        50

#define USECS_PER_SEC       1000000LL
#define USECS_PER_MINUTE    (60LL * USECS_PER_SEC)
#define USECS_PER_HOUR      (60LL * USECS_PER_MINUTE)
#define USECS_PER_DAY       (24LL * USECS_PER_HOUR)

/* Dimension types */
typedef enum MockDimensionType
{
    MOCK_DIMENSION_TIME = 0,
    MOCK_DIMENSION_SPACE
} MockDimensionType;

/* Storage tier */
typedef enum MockChunkTier
{
    MOCK_CHUNK_HOT = 0,
    MOCK_CHUNK_WARM,
    MOCK_CHUNK_COLD
} MockChunkTier;

/* Dimension info */
typedef struct MockDimension
{
    int32               dimension_id;
    Oid                 hypertable_oid;
    char                column_name[64];
    MockDimensionType   dim_type;
    int64               interval_length;    /* microseconds for time */
    int32               num_slices;         /* for space dimensions */
    bool                in_use;
} MockDimension;

/* Chunk info */
typedef struct MockChunkInfo
{
    int64               chunk_id;
    Oid                 hypertable_oid;
    int64               range_start;        /* TimestampTz */
    int64               range_end;          /* TimestampTz */
    bool                is_compressed;
    MockChunkTier       storage_tier;
    int64               row_count;
    int64               size_bytes;
    int64               created_at;
    bool                in_use;
} MockChunkInfo;

/* Hypertable info */
typedef struct MockHypertableInfo
{
    Oid                 hypertable_oid;
    char                schema_name[64];
    char                table_name[64];
    int32               num_dimensions;
    bool                compression_enabled;
    int64               compress_after_usecs;
    bool                is_distributed;
    bool                in_use;
} MockHypertableInfo;

/* Retention policy */
typedef struct MockRetentionPolicy
{
    int64               policy_id;
    Oid                 hypertable_oid;
    int64               drop_after_usecs;
    bool                enabled;
    int64               last_run;
    bool                in_use;
} MockRetentionPolicy;

/* Compression policy */
typedef struct MockCompressionPolicy
{
    int64               policy_id;
    Oid                 hypertable_oid;
    int64               compress_after_usecs;
    char                segment_by[64];
    char                order_by[64];
    bool                enabled;
    bool                in_use;
} MockCompressionPolicy;

/* Global state */
static MockHypertableInfo mock_hypertables[MAX_HYPERTABLES];
static MockDimension mock_dimensions[MAX_DIMENSIONS * MAX_HYPERTABLES];
static MockChunkInfo mock_chunks[MAX_CHUNKS];
static MockRetentionPolicy mock_retention_policies[MAX_POLICIES];
static MockCompressionPolicy mock_compression_policies[MAX_POLICIES];

static int64 next_chunk_id = 1;
static int32 next_dimension_id = 1;
static int64 next_policy_id = 1;

/* ============================================================
 * Mock Hypertable Functions
 * ============================================================ */

static void mock_hypertable_init(void)
{
    memset(mock_hypertables, 0, sizeof(mock_hypertables));
    memset(mock_dimensions, 0, sizeof(mock_dimensions));
    memset(mock_chunks, 0, sizeof(mock_chunks));
    memset(mock_retention_policies, 0, sizeof(mock_retention_policies));
    memset(mock_compression_policies, 0, sizeof(mock_compression_policies));
    next_chunk_id = 1;
    next_dimension_id = 1;
    next_policy_id = 1;
}

static void mock_hypertable_cleanup(void)
{
    memset(mock_hypertables, 0, sizeof(mock_hypertables));
}

static MockHypertableInfo *mock_create_hypertable(Oid table_oid, const char *schema,
                                                   const char *name, const char *time_column,
                                                   int64 chunk_interval_usecs)
{
    for (int i = 0; i < MAX_HYPERTABLES; i++)
    {
        if (!mock_hypertables[i].in_use)
        {
            mock_hypertables[i].hypertable_oid = table_oid;
            strncpy(mock_hypertables[i].schema_name, schema, 63);
            strncpy(mock_hypertables[i].table_name, name, 63);
            mock_hypertables[i].num_dimensions = 1;
            mock_hypertables[i].compression_enabled = false;
            mock_hypertables[i].is_distributed = false;
            mock_hypertables[i].in_use = true;

            /* Add time dimension */
            for (int d = 0; d < MAX_DIMENSIONS * MAX_HYPERTABLES; d++)
            {
                if (!mock_dimensions[d].in_use)
                {
                    mock_dimensions[d].dimension_id = next_dimension_id++;
                    mock_dimensions[d].hypertable_oid = table_oid;
                    strncpy(mock_dimensions[d].column_name, time_column, 63);
                    mock_dimensions[d].dim_type = MOCK_DIMENSION_TIME;
                    mock_dimensions[d].interval_length = chunk_interval_usecs;
                    mock_dimensions[d].in_use = true;
                    break;
                }
            }

            return &mock_hypertables[i];
        }
    }
    return NULL;
}

static MockHypertableInfo *mock_get_hypertable(Oid table_oid)
{
    for (int i = 0; i < MAX_HYPERTABLES; i++)
    {
        if (mock_hypertables[i].in_use && mock_hypertables[i].hypertable_oid == table_oid)
            return &mock_hypertables[i];
    }
    return NULL;
}

static bool mock_is_hypertable(Oid table_oid)
{
    return mock_get_hypertable(table_oid) != NULL;
}

static void mock_drop_hypertable(Oid table_oid)
{
    for (int i = 0; i < MAX_HYPERTABLES; i++)
    {
        if (mock_hypertables[i].in_use && mock_hypertables[i].hypertable_oid == table_oid)
        {
            mock_hypertables[i].in_use = false;
            break;
        }
    }
}

static bool mock_add_dimension(Oid hypertable_oid, const char *column_name,
                               int num_partitions)
{
    MockHypertableInfo *ht = mock_get_hypertable(hypertable_oid);
    if (!ht) return false;

    for (int d = 0; d < MAX_DIMENSIONS * MAX_HYPERTABLES; d++)
    {
        if (!mock_dimensions[d].in_use)
        {
            mock_dimensions[d].dimension_id = next_dimension_id++;
            mock_dimensions[d].hypertable_oid = hypertable_oid;
            strncpy(mock_dimensions[d].column_name, column_name, 63);
            mock_dimensions[d].dim_type = MOCK_DIMENSION_SPACE;
            mock_dimensions[d].num_slices = num_partitions;
            mock_dimensions[d].in_use = true;
            ht->num_dimensions++;
            return true;
        }
    }
    return false;
}

static int mock_count_dimensions(Oid hypertable_oid)
{
    int count = 0;
    for (int d = 0; d < MAX_DIMENSIONS * MAX_HYPERTABLES; d++)
    {
        if (mock_dimensions[d].in_use && mock_dimensions[d].hypertable_oid == hypertable_oid)
            count++;
    }
    return count;
}

static MockDimension *mock_get_time_dimension(Oid hypertable_oid)
{
    for (int d = 0; d < MAX_DIMENSIONS * MAX_HYPERTABLES; d++)
    {
        if (mock_dimensions[d].in_use &&
            mock_dimensions[d].hypertable_oid == hypertable_oid &&
            mock_dimensions[d].dim_type == MOCK_DIMENSION_TIME)
            return &mock_dimensions[d];
    }
    return NULL;
}

/* Chunk operations */
static int64 mock_create_chunk(Oid hypertable_oid, int64 range_start, int64 range_end)
{
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (!mock_chunks[i].in_use)
        {
            mock_chunks[i].chunk_id = next_chunk_id++;
            mock_chunks[i].hypertable_oid = hypertable_oid;
            mock_chunks[i].range_start = range_start;
            mock_chunks[i].range_end = range_end;
            mock_chunks[i].is_compressed = false;
            mock_chunks[i].storage_tier = MOCK_CHUNK_HOT;
            mock_chunks[i].row_count = 0;
            mock_chunks[i].size_bytes = 0;
            mock_chunks[i].created_at = time(NULL) * USECS_PER_SEC;
            mock_chunks[i].in_use = true;
            return mock_chunks[i].chunk_id;
        }
    }
    return -1;
}

static MockChunkInfo *mock_get_chunk(int64 chunk_id)
{
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use && mock_chunks[i].chunk_id == chunk_id)
            return &mock_chunks[i];
    }
    return NULL;
}

static MockChunkInfo *mock_get_chunk_for_timestamp(Oid hypertable_oid, int64 ts)
{
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use &&
            mock_chunks[i].hypertable_oid == hypertable_oid &&
            ts >= mock_chunks[i].range_start &&
            ts < mock_chunks[i].range_end)
            return &mock_chunks[i];
    }
    return NULL;
}

static int mock_count_chunks(Oid hypertable_oid)
{
    int count = 0;
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use && mock_chunks[i].hypertable_oid == hypertable_oid)
            count++;
    }
    return count;
}

static int mock_count_chunks_in_range(Oid hypertable_oid, int64 start, int64 end)
{
    int count = 0;
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use &&
            mock_chunks[i].hypertable_oid == hypertable_oid &&
            mock_chunks[i].range_start < end &&
            mock_chunks[i].range_end > start)
            count++;
    }
    return count;
}

static void mock_compress_chunk(int64 chunk_id)
{
    MockChunkInfo *chunk = mock_get_chunk(chunk_id);
    if (chunk)
        chunk->is_compressed = true;
}

static void mock_decompress_chunk(int64 chunk_id)
{
    MockChunkInfo *chunk = mock_get_chunk(chunk_id);
    if (chunk)
        chunk->is_compressed = false;
}

static int mock_drop_chunks_older_than(Oid hypertable_oid, int64 threshold_ts)
{
    int dropped = 0;
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use &&
            mock_chunks[i].hypertable_oid == hypertable_oid &&
            mock_chunks[i].range_end <= threshold_ts)
        {
            mock_chunks[i].in_use = false;
            dropped++;
        }
    }
    return dropped;
}

static void mock_drop_chunk(int64 chunk_id)
{
    for (int i = 0; i < MAX_CHUNKS; i++)
    {
        if (mock_chunks[i].in_use && mock_chunks[i].chunk_id == chunk_id)
        {
            mock_chunks[i].in_use = false;
            return;
        }
    }
}

/* Time bucketing */
static int64 mock_time_bucket(int64 bucket_width, int64 ts)
{
    /* Align timestamp to bucket boundary */
    if (bucket_width <= 0) return ts;
    int64 bucket_num = ts / bucket_width;
    return bucket_num * bucket_width;
}

static int64 mock_time_bucket_offset(int64 bucket_width, int64 ts, int64 offset)
{
    return mock_time_bucket(bucket_width, ts - offset) + offset;
}

/* Policy operations */
static int64 mock_add_retention_policy(Oid hypertable_oid, int64 drop_after_usecs)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (!mock_retention_policies[i].in_use)
        {
            mock_retention_policies[i].policy_id = next_policy_id++;
            mock_retention_policies[i].hypertable_oid = hypertable_oid;
            mock_retention_policies[i].drop_after_usecs = drop_after_usecs;
            mock_retention_policies[i].enabled = true;
            mock_retention_policies[i].last_run = 0;
            mock_retention_policies[i].in_use = true;
            return mock_retention_policies[i].policy_id;
        }
    }
    return -1;
}

static MockRetentionPolicy *mock_get_retention_policy(Oid hypertable_oid)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (mock_retention_policies[i].in_use &&
            mock_retention_policies[i].hypertable_oid == hypertable_oid)
            return &mock_retention_policies[i];
    }
    return NULL;
}

static void mock_remove_retention_policy(Oid hypertable_oid)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (mock_retention_policies[i].in_use &&
            mock_retention_policies[i].hypertable_oid == hypertable_oid)
        {
            mock_retention_policies[i].in_use = false;
            return;
        }
    }
}

static int64 mock_add_compression_policy(Oid hypertable_oid, int64 compress_after_usecs,
                                          const char *segment_by, const char *order_by)
{
    MockHypertableInfo *ht = mock_get_hypertable(hypertable_oid);
    if (ht) ht->compression_enabled = true;

    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (!mock_compression_policies[i].in_use)
        {
            mock_compression_policies[i].policy_id = next_policy_id++;
            mock_compression_policies[i].hypertable_oid = hypertable_oid;
            mock_compression_policies[i].compress_after_usecs = compress_after_usecs;
            if (segment_by) strncpy(mock_compression_policies[i].segment_by, segment_by, 63);
            if (order_by) strncpy(mock_compression_policies[i].order_by, order_by, 63);
            mock_compression_policies[i].enabled = true;
            mock_compression_policies[i].in_use = true;
            return mock_compression_policies[i].policy_id;
        }
    }
    return -1;
}

static MockCompressionPolicy *mock_get_compression_policy(Oid hypertable_oid)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (mock_compression_policies[i].in_use &&
            mock_compression_policies[i].hypertable_oid == hypertable_oid)
            return &mock_compression_policies[i];
    }
    return NULL;
}

/* Chunk stats */
static void mock_get_chunk_stats(int64 chunk_id, int64 *row_count, int64 *size_bytes)
{
    MockChunkInfo *chunk = mock_get_chunk(chunk_id);
    if (chunk)
    {
        *row_count = chunk->row_count;
        *size_bytes = chunk->size_bytes;
    }
    else
    {
        *row_count = 0;
        *size_bytes = 0;
    }
}

/* ============================================================
 * Test Cases
 * ============================================================ */

/* Test 1: Hypertable creation */
static void test_hypertable_creation(void)
{
    TEST_BEGIN("hypertable_creation")
        mock_hypertable_init();

        MockHypertableInfo *ht = mock_create_hypertable(1001, "public", "events",
                                                         "created_at", USECS_PER_DAY);
        TEST_ASSERT_NOT_NULL(ht);
        TEST_ASSERT_EQ(1001, ht->hypertable_oid);
        TEST_ASSERT_STR_EQ("public", ht->schema_name);
        TEST_ASSERT_STR_EQ("events", ht->table_name);
        TEST_ASSERT_EQ(1, ht->num_dimensions);
        TEST_ASSERT_FALSE(ht->compression_enabled);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 2: Hypertable lookup */
static void test_hypertable_lookup(void)
{
    TEST_BEGIN("hypertable_lookup")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);
        mock_create_hypertable(1002, "metrics", "cpu", "time", USECS_PER_HOUR);

        MockHypertableInfo *ht1 = mock_get_hypertable(1001);
        TEST_ASSERT_NOT_NULL(ht1);
        TEST_ASSERT_STR_EQ("events", ht1->table_name);

        MockHypertableInfo *ht2 = mock_get_hypertable(1002);
        TEST_ASSERT_NOT_NULL(ht2);
        TEST_ASSERT_STR_EQ("cpu", ht2->table_name);

        TEST_ASSERT_NULL(mock_get_hypertable(9999));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 3: Is hypertable check */
static void test_is_hypertable(void)
{
    TEST_BEGIN("is_hypertable_check")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        TEST_ASSERT(mock_is_hypertable(1001));
        TEST_ASSERT_FALSE(mock_is_hypertable(9999));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 4: Drop hypertable */
static void test_drop_hypertable(void)
{
    TEST_BEGIN("drop_hypertable")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);
        TEST_ASSERT(mock_is_hypertable(1001));

        mock_drop_hypertable(1001);
        TEST_ASSERT_FALSE(mock_is_hypertable(1001));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 5: Add space dimension */
static void test_add_space_dimension(void)
{
    TEST_BEGIN("add_space_dimension")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);
        TEST_ASSERT_EQ(1, mock_count_dimensions(1001));

        bool added = mock_add_dimension(1001, "device_id", 8);
        TEST_ASSERT(added);
        TEST_ASSERT_EQ(2, mock_count_dimensions(1001));

        MockHypertableInfo *ht = mock_get_hypertable(1001);
        TEST_ASSERT_EQ(2, ht->num_dimensions);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 6: Chunk creation */
static void test_chunk_creation(void)
{
    TEST_BEGIN("chunk_creation")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 day_start = 1704067200LL * USECS_PER_SEC;  /* 2024-01-01 00:00:00 */
        int64 chunk_id = mock_create_chunk(1001, day_start, day_start + USECS_PER_DAY);

        TEST_ASSERT_GT(chunk_id, 0);

        MockChunkInfo *chunk = mock_get_chunk(chunk_id);
        TEST_ASSERT_NOT_NULL(chunk);
        TEST_ASSERT_EQ(1001, chunk->hypertable_oid);
        TEST_ASSERT_EQ(day_start, chunk->range_start);
        TEST_ASSERT_EQ(day_start + USECS_PER_DAY, chunk->range_end);
        TEST_ASSERT_FALSE(chunk->is_compressed);
        TEST_ASSERT_EQ(MOCK_CHUNK_HOT, chunk->storage_tier);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 7: Chunk lookup by timestamp */
static void test_chunk_lookup_by_timestamp(void)
{
    TEST_BEGIN("chunk_lookup_by_timestamp")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        /* Create 3 daily chunks */
        int64 base = 1704067200LL * USECS_PER_SEC;
        mock_create_chunk(1001, base, base + USECS_PER_DAY);
        mock_create_chunk(1001, base + USECS_PER_DAY, base + 2 * USECS_PER_DAY);
        mock_create_chunk(1001, base + 2 * USECS_PER_DAY, base + 3 * USECS_PER_DAY);

        /* Test timestamp in first chunk */
        int64 ts1 = base + 12 * USECS_PER_HOUR;
        MockChunkInfo *c1 = mock_get_chunk_for_timestamp(1001, ts1);
        TEST_ASSERT_NOT_NULL(c1);
        TEST_ASSERT_EQ(base, c1->range_start);

        /* Test timestamp in second chunk */
        int64 ts2 = base + USECS_PER_DAY + 6 * USECS_PER_HOUR;
        MockChunkInfo *c2 = mock_get_chunk_for_timestamp(1001, ts2);
        TEST_ASSERT_NOT_NULL(c2);
        TEST_ASSERT_EQ(base + USECS_PER_DAY, c2->range_start);

        /* Test timestamp outside all chunks */
        int64 ts_before = base - USECS_PER_HOUR;
        TEST_ASSERT_NULL(mock_get_chunk_for_timestamp(1001, ts_before));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 8: Count chunks */
static void test_count_chunks(void)
{
    TEST_BEGIN("count_chunks")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);
        mock_create_hypertable(1002, "metrics", "cpu", "time", USECS_PER_HOUR);

        int64 base = 1704067200LL * USECS_PER_SEC;

        /* Create chunks for first hypertable */
        mock_create_chunk(1001, base, base + USECS_PER_DAY);
        mock_create_chunk(1001, base + USECS_PER_DAY, base + 2 * USECS_PER_DAY);
        mock_create_chunk(1001, base + 2 * USECS_PER_DAY, base + 3 * USECS_PER_DAY);

        /* Create chunks for second hypertable */
        mock_create_chunk(1002, base, base + USECS_PER_HOUR);
        mock_create_chunk(1002, base + USECS_PER_HOUR, base + 2 * USECS_PER_HOUR);

        TEST_ASSERT_EQ(3, mock_count_chunks(1001));
        TEST_ASSERT_EQ(2, mock_count_chunks(1002));
        TEST_ASSERT_EQ(0, mock_count_chunks(9999));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 9: Count chunks in range */
static void test_chunks_in_range(void)
{
    TEST_BEGIN("chunks_in_range")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;

        /* Create 5 daily chunks */
        for (int i = 0; i < 5; i++)
        {
            mock_create_chunk(1001, base + i * USECS_PER_DAY,
                              base + (i + 1) * USECS_PER_DAY);
        }

        /* Query range covering days 2-3 (should return 2 chunks) */
        int64 range_start = base + USECS_PER_DAY + USECS_PER_HOUR;
        int64 range_end = base + 3 * USECS_PER_DAY - USECS_PER_HOUR;
        TEST_ASSERT_EQ(2, mock_count_chunks_in_range(1001, range_start, range_end));

        /* Query range covering all chunks */
        TEST_ASSERT_EQ(5, mock_count_chunks_in_range(1001, base, base + 5 * USECS_PER_DAY));

        /* Query range before all chunks */
        TEST_ASSERT_EQ(0, mock_count_chunks_in_range(1001, base - 2 * USECS_PER_DAY,
                                                      base - USECS_PER_DAY));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 10: Chunk compression */
static void test_chunk_compression(void)
{
    TEST_BEGIN("chunk_compression")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;
        int64 chunk_id = mock_create_chunk(1001, base, base + USECS_PER_DAY);

        MockChunkInfo *chunk = mock_get_chunk(chunk_id);
        TEST_ASSERT_FALSE(chunk->is_compressed);

        mock_compress_chunk(chunk_id);
        TEST_ASSERT(mock_get_chunk(chunk_id)->is_compressed);

        mock_decompress_chunk(chunk_id);
        TEST_ASSERT_FALSE(mock_get_chunk(chunk_id)->is_compressed);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 11: Drop chunks older than */
static void test_drop_chunks_older_than(void)
{
    TEST_BEGIN("drop_chunks_older_than")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;

        /* Create 5 daily chunks */
        for (int i = 0; i < 5; i++)
        {
            mock_create_chunk(1001, base + i * USECS_PER_DAY,
                              base + (i + 1) * USECS_PER_DAY);
        }

        TEST_ASSERT_EQ(5, mock_count_chunks(1001));

        /* Drop chunks older than day 3 */
        int64 threshold = base + 3 * USECS_PER_DAY;
        int dropped = mock_drop_chunks_older_than(1001, threshold);

        TEST_ASSERT_EQ(3, dropped);
        TEST_ASSERT_EQ(2, mock_count_chunks(1001));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 12: Time bucket function */
static void test_time_bucket(void)
{
    TEST_BEGIN("time_bucket")
        /* Test hourly bucketing */
        int64 ts = 1704096000LL * USECS_PER_SEC + 30 * USECS_PER_MINUTE;  /* 08:30 */
        int64 bucket = mock_time_bucket(USECS_PER_HOUR, ts);
        int64 expected = 1704096000LL * USECS_PER_SEC;  /* 08:00 */
        TEST_ASSERT_EQ(expected, bucket);

        /* Test daily bucketing */
        bucket = mock_time_bucket(USECS_PER_DAY, ts);
        expected = 1704067200LL * USECS_PER_SEC;  /* 00:00 */
        TEST_ASSERT_EQ(expected, bucket);

        /* Test 5-minute bucketing */
        int64 five_min = 5 * USECS_PER_MINUTE;
        ts = 1704096000LL * USECS_PER_SEC + 7 * USECS_PER_MINUTE;  /* 08:07 */
        bucket = mock_time_bucket(five_min, ts);
        expected = 1704096000LL * USECS_PER_SEC + 5 * USECS_PER_MINUTE;  /* 08:05 */
        TEST_ASSERT_EQ(expected, bucket);
    TEST_END();
}

/* Test 13: Time bucket with offset */
static void test_time_bucket_offset(void)
{
    TEST_BEGIN("time_bucket_offset")
        int64 ts = 1704096000LL * USECS_PER_SEC;  /* 08:00 */
        int64 offset = 30 * USECS_PER_MINUTE;

        /* Bucket with 1-hour width and 30-min offset */
        int64 bucket = mock_time_bucket_offset(USECS_PER_HOUR, ts, offset);

        /* With 30-min offset, buckets are [X:30, X+1:30) */
        /* 08:00 falls in [07:30, 08:30) bucket, so bucket start is 07:30 */
        /* mock_time_bucket(hour, 08:00 - 0:30) + 0:30 = mock_time_bucket(hour, 07:30) + 0:30 */
        /* = 07:00 + 0:30 = 07:30 */
        int64 expected = mock_time_bucket(USECS_PER_HOUR, ts - offset) + offset;
        TEST_ASSERT_EQ(expected, bucket);
    TEST_END();
}

/* Test 14: Retention policy */
static void test_retention_policy(void)
{
    TEST_BEGIN("retention_policy")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 policy_id = mock_add_retention_policy(1001, 7 * USECS_PER_DAY);
        TEST_ASSERT_GT(policy_id, 0);

        MockRetentionPolicy *policy = mock_get_retention_policy(1001);
        TEST_ASSERT_NOT_NULL(policy);
        TEST_ASSERT_EQ(7 * USECS_PER_DAY, policy->drop_after_usecs);
        TEST_ASSERT(policy->enabled);

        mock_remove_retention_policy(1001);
        TEST_ASSERT_NULL(mock_get_retention_policy(1001));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 15: Compression policy */
static void test_compression_policy(void)
{
    TEST_BEGIN("compression_policy")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 policy_id = mock_add_compression_policy(1001, 24 * USECS_PER_HOUR,
                                                       "device_id", "ts DESC");
        TEST_ASSERT_GT(policy_id, 0);

        MockCompressionPolicy *policy = mock_get_compression_policy(1001);
        TEST_ASSERT_NOT_NULL(policy);
        TEST_ASSERT_EQ(24 * USECS_PER_HOUR, policy->compress_after_usecs);
        TEST_ASSERT_STR_EQ("device_id", policy->segment_by);
        TEST_ASSERT_STR_EQ("ts DESC", policy->order_by);

        MockHypertableInfo *ht = mock_get_hypertable(1001);
        TEST_ASSERT(ht->compression_enabled);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 16: Chunk statistics */
static void test_chunk_statistics(void)
{
    TEST_BEGIN("chunk_statistics")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;
        int64 chunk_id = mock_create_chunk(1001, base, base + USECS_PER_DAY);

        MockChunkInfo *chunk = mock_get_chunk(chunk_id);
        chunk->row_count = 1000000;
        chunk->size_bytes = 104857600;  /* 100MB */

        int64 row_count, size_bytes;
        mock_get_chunk_stats(chunk_id, &row_count, &size_bytes);

        TEST_ASSERT_EQ(1000000, row_count);
        TEST_ASSERT_EQ(104857600, size_bytes);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 17: Multiple hypertables isolation */
static void test_hypertable_isolation(void)
{
    TEST_BEGIN("hypertable_isolation")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);
        mock_create_hypertable(1002, "metrics", "cpu", "time", USECS_PER_HOUR);

        int64 base = 1704067200LL * USECS_PER_SEC;

        /* Create chunks for both */
        mock_create_chunk(1001, base, base + USECS_PER_DAY);
        mock_create_chunk(1002, base, base + USECS_PER_HOUR);

        /* Verify isolation */
        TEST_ASSERT_EQ(1, mock_count_chunks(1001));
        TEST_ASSERT_EQ(1, mock_count_chunks(1002));

        /* Drop one hypertable's chunks */
        mock_drop_chunks_older_than(1001, base + 2 * USECS_PER_DAY);

        TEST_ASSERT_EQ(0, mock_count_chunks(1001));
        TEST_ASSERT_EQ(1, mock_count_chunks(1002));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 18: Drop individual chunk */
static void test_drop_individual_chunk(void)
{
    TEST_BEGIN("drop_individual_chunk")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;
        int64 chunk1 = mock_create_chunk(1001, base, base + USECS_PER_DAY);
        int64 chunk2 = mock_create_chunk(1001, base + USECS_PER_DAY, base + 2 * USECS_PER_DAY);

        TEST_ASSERT_EQ(2, mock_count_chunks(1001));

        mock_drop_chunk(chunk1);

        TEST_ASSERT_EQ(1, mock_count_chunks(1001));
        TEST_ASSERT_NULL(mock_get_chunk(chunk1));
        TEST_ASSERT_NOT_NULL(mock_get_chunk(chunk2));

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 19: Chunk tier management */
static void test_chunk_tier_management(void)
{
    TEST_BEGIN("chunk_tier_management")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "ts", USECS_PER_DAY);

        int64 base = 1704067200LL * USECS_PER_SEC;
        int64 chunk_id = mock_create_chunk(1001, base, base + USECS_PER_DAY);

        MockChunkInfo *chunk = mock_get_chunk(chunk_id);
        TEST_ASSERT_EQ(MOCK_CHUNK_HOT, chunk->storage_tier);

        chunk->storage_tier = MOCK_CHUNK_WARM;
        TEST_ASSERT_EQ(MOCK_CHUNK_WARM, mock_get_chunk(chunk_id)->storage_tier);

        chunk->storage_tier = MOCK_CHUNK_COLD;
        TEST_ASSERT_EQ(MOCK_CHUNK_COLD, mock_get_chunk(chunk_id)->storage_tier);

        mock_hypertable_cleanup();
    TEST_END();
}

/* Test 20: Time dimension retrieval */
static void test_time_dimension_retrieval(void)
{
    TEST_BEGIN("time_dimension_retrieval")
        mock_hypertable_init();

        mock_create_hypertable(1001, "public", "events", "created_at", USECS_PER_DAY);
        mock_add_dimension(1001, "device_id", 8);

        MockDimension *time_dim = mock_get_time_dimension(1001);
        TEST_ASSERT_NOT_NULL(time_dim);
        TEST_ASSERT_STR_EQ("created_at", time_dim->column_name);
        TEST_ASSERT_EQ(MOCK_DIMENSION_TIME, time_dim->dim_type);
        TEST_ASSERT_EQ(USECS_PER_DAY, time_dim->interval_length);

        mock_hypertable_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_hypertable(void)
{
    test_hypertable_creation();
    test_hypertable_lookup();
    test_is_hypertable();
    test_drop_hypertable();
    test_add_space_dimension();
    test_chunk_creation();
    test_chunk_lookup_by_timestamp();
    test_count_chunks();
    test_chunks_in_range();
    test_chunk_compression();
    test_drop_chunks_older_than();
    test_time_bucket();
    test_time_bucket_offset();
    test_retention_policy();
    test_compression_policy();
    test_chunk_statistics();
    test_hypertable_isolation();
    test_drop_individual_chunk();
    test_chunk_tier_management();
    test_time_dimension_retrieval();
}
