/*-------------------------------------------------------------------------
 *
 * test_distribution.c
 *    Unit tests for Orochi DB distribution/sharding functionality
 *
 * Tests shard ID computation, hash/range distribution, and shard placement.
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

#define MAX_TABLES          50
#define MAX_SHARDS          500
#define MAX_NODES           20
#define MAX_COLOCATION_GROUPS 20
#define OROCHI_HASH_SEED    0x9747b28c
#define HASH_MODULUS        2147483647  /* INT32_MAX */

/* Shard strategies (mirror orochi.h) */
typedef enum MockShardStrategy
{
    MOCK_SHARD_HASH = 0,
    MOCK_SHARD_RANGE,
    MOCK_SHARD_LIST,
    MOCK_SHARD_COMPOSITE,
    MOCK_SHARD_REFERENCE
} MockShardStrategy;

/* Distributed table info */
typedef struct MockDistributedTable
{
    Oid                 table_oid;
    char                table_name[64];
    char                distribution_column[64];
    MockShardStrategy   strategy;
    int32               shard_count;
    int32               colocation_group;
    bool                is_reference;
    bool                in_use;
} MockDistributedTable;

/* Shard info */
typedef struct MockShard
{
    int64               shard_id;
    Oid                 table_oid;
    int32               shard_index;
    int32               hash_min;
    int32               hash_max;
    int32               node_id;
    int64               row_count;
    int64               size_bytes;
    bool                in_use;
} MockShard;

/* Node info */
typedef struct MockNode
{
    int32               node_id;
    char                hostname[128];
    int                 port;
    bool                is_active;
    int32               shard_count;
    int64               total_size;
    bool                in_use;
} MockNode;

/* Colocation group */
typedef struct MockColocationGroup
{
    int32               group_id;
    int32               shard_count;
    MockShardStrategy   strategy;
    bool                in_use;
} MockColocationGroup;

/* Shard balance statistics */
typedef struct MockShardBalanceStats
{
    int                 total_nodes;
    int64               total_shards;
    int64               min_shards;
    int64               max_shards;
    double              avg_shards_per_node;
    double              imbalance_ratio;
    bool                needs_rebalancing;
} MockShardBalanceStats;

/* Global state */
static MockDistributedTable mock_tables[MAX_TABLES];
static MockShard mock_shards[MAX_SHARDS];
static MockNode mock_nodes[MAX_NODES];
static MockColocationGroup mock_colocation_groups[MAX_COLOCATION_GROUPS];
static int64 next_shard_id = 1;
static int32 next_colocation_group_id = 1;

/* ============================================================
 * Hash Functions
 * ============================================================ */

/* MurmurHash3 finalizer for 32-bit */
static uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

/* Simple hash function for integers */
static int32 mock_hash_int32(int32 value)
{
    uint32_t h = (uint32_t)value;
    h ^= OROCHI_HASH_SEED;
    h = fmix32(h);
    return (int32)(h & 0x7FFFFFFF);  /* Ensure positive */
}

static int32 mock_hash_int64(int64 value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)((value >> 32) & 0xFFFFFFFF);
    uint32_t h = lo ^ hi ^ OROCHI_HASH_SEED;
    h = fmix32(h);
    return (int32)(h & 0x7FFFFFFF);
}

static int32 mock_hash_string(const char *str)
{
    if (!str) return 0;

    uint32_t h = OROCHI_HASH_SEED;
    while (*str)
    {
        h ^= (uint32_t)*str++;
        h = fmix32(h);
    }
    return (int32)(h & 0x7FFFFFFF);
}

/* CRC32 hash */
static uint32_t mock_crc32_table[256];
static bool crc32_initialized = false;

static void mock_init_crc32(void)
{
    if (crc32_initialized) return;

    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        mock_crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

static uint32_t mock_crc32_hash(const void *data, size_t length)
{
    mock_init_crc32();

    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++)
    {
        crc = mock_crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* Get shard index for hash value */
static int32 mock_get_shard_index(int32 hash_value, int32 shard_count)
{
    if (shard_count <= 0) return 0;
    /* Consistent hash to shard index */
    return (int32)((int64)hash_value * shard_count / (int64)HASH_MODULUS);
}

/* ============================================================
 * Mock Distribution Functions
 * ============================================================ */

static void mock_distribution_init(void)
{
    memset(mock_tables, 0, sizeof(mock_tables));
    memset(mock_shards, 0, sizeof(mock_shards));
    memset(mock_nodes, 0, sizeof(mock_nodes));
    memset(mock_colocation_groups, 0, sizeof(mock_colocation_groups));
    next_shard_id = 1;
    next_colocation_group_id = 1;
}

static void mock_distribution_cleanup(void)
{
    memset(mock_tables, 0, sizeof(mock_tables));
}

/* Table operations */
static MockDistributedTable *mock_create_distributed_table(Oid table_oid,
                                                            const char *name,
                                                            const char *dist_column,
                                                            MockShardStrategy strategy,
                                                            int32 shard_count)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (!mock_tables[i].in_use)
        {
            mock_tables[i].table_oid = table_oid;
            strncpy(mock_tables[i].table_name, name, 63);
            strncpy(mock_tables[i].distribution_column, dist_column, 63);
            mock_tables[i].strategy = strategy;
            mock_tables[i].shard_count = shard_count;
            mock_tables[i].is_reference = (strategy == MOCK_SHARD_REFERENCE);
            mock_tables[i].colocation_group = 0;
            mock_tables[i].in_use = true;
            return &mock_tables[i];
        }
    }
    return NULL;
}

static MockDistributedTable *mock_get_distributed_table(Oid table_oid)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (mock_tables[i].in_use && mock_tables[i].table_oid == table_oid)
            return &mock_tables[i];
    }
    return NULL;
}

static bool mock_is_distributed_table(Oid table_oid)
{
    MockDistributedTable *t = mock_get_distributed_table(table_oid);
    return t != NULL && !t->is_reference;
}

static bool mock_is_reference_table(Oid table_oid)
{
    MockDistributedTable *t = mock_get_distributed_table(table_oid);
    return t != NULL && t->is_reference;
}

/* Shard operations */
static int64 mock_create_shard(Oid table_oid, int32 shard_index,
                                int32 hash_min, int32 hash_max, int32 node_id)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (!mock_shards[i].in_use)
        {
            mock_shards[i].shard_id = next_shard_id++;
            mock_shards[i].table_oid = table_oid;
            mock_shards[i].shard_index = shard_index;
            mock_shards[i].hash_min = hash_min;
            mock_shards[i].hash_max = hash_max;
            mock_shards[i].node_id = node_id;
            mock_shards[i].row_count = 0;
            mock_shards[i].size_bytes = 0;
            mock_shards[i].in_use = true;
            return mock_shards[i].shard_id;
        }
    }
    return -1;
}

static void mock_create_shards_for_table(Oid table_oid, int32 shard_count,
                                          int32 num_nodes)
{
    int32 range_per_shard = HASH_MODULUS / shard_count;

    for (int i = 0; i < shard_count; i++)
    {
        int32 hash_min = i * range_per_shard;
        int32 hash_max = (i == shard_count - 1) ? HASH_MODULUS : (i + 1) * range_per_shard - 1;
        int32 node_id = (num_nodes > 0) ? (i % num_nodes) + 1 : 1;

        mock_create_shard(table_oid, i, hash_min, hash_max, node_id);
    }
}

static MockShard *mock_get_shard(int64 shard_id)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use && mock_shards[i].shard_id == shard_id)
            return &mock_shards[i];
    }
    return NULL;
}

static MockShard *mock_route_to_shard(Oid table_oid, int32 hash_value)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use &&
            mock_shards[i].table_oid == table_oid &&
            hash_value >= mock_shards[i].hash_min &&
            hash_value <= mock_shards[i].hash_max)
            return &mock_shards[i];
    }
    return NULL;
}

static int mock_count_table_shards(Oid table_oid)
{
    int count = 0;
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use && mock_shards[i].table_oid == table_oid)
            count++;
    }
    return count;
}

static int mock_get_shards_in_range(Oid table_oid, int32 min_hash, int32 max_hash,
                                     MockShard **result, int max_results)
{
    int count = 0;
    for (int i = 0; i < MAX_SHARDS && count < max_results; i++)
    {
        if (mock_shards[i].in_use &&
            mock_shards[i].table_oid == table_oid &&
            mock_shards[i].hash_min <= max_hash &&
            mock_shards[i].hash_max >= min_hash)
        {
            result[count++] = &mock_shards[i];
        }
    }
    return count;
}

/* Node operations */
static int32 mock_add_node(const char *hostname, int port)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (!mock_nodes[i].in_use)
        {
            mock_nodes[i].node_id = i + 1;
            strncpy(mock_nodes[i].hostname, hostname, 127);
            mock_nodes[i].port = port;
            mock_nodes[i].is_active = true;
            mock_nodes[i].shard_count = 0;
            mock_nodes[i].total_size = 0;
            mock_nodes[i].in_use = true;
            return mock_nodes[i].node_id;
        }
    }
    return -1;
}

static MockNode *mock_get_node(int32 node_id)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].node_id == node_id)
            return &mock_nodes[i];
    }
    return NULL;
}

static int mock_count_active_nodes(void)
{
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].is_active)
            count++;
    }
    return count;
}

static int32 mock_select_node_for_shard(void)
{
    /* Select node with fewest shards */
    int32 best_node = -1;
    int32 min_shards = INT32_MAX;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].is_active)
        {
            if (mock_nodes[i].shard_count < min_shards)
            {
                min_shards = mock_nodes[i].shard_count;
                best_node = mock_nodes[i].node_id;
            }
        }
    }
    return best_node;
}

/* Colocation operations */
static int32 mock_create_colocation_group(int32 shard_count, MockShardStrategy strategy)
{
    for (int i = 0; i < MAX_COLOCATION_GROUPS; i++)
    {
        if (!mock_colocation_groups[i].in_use)
        {
            mock_colocation_groups[i].group_id = next_colocation_group_id++;
            mock_colocation_groups[i].shard_count = shard_count;
            mock_colocation_groups[i].strategy = strategy;
            mock_colocation_groups[i].in_use = true;
            return mock_colocation_groups[i].group_id;
        }
    }
    return -1;
}

static bool mock_tables_are_colocated(Oid table1, Oid table2)
{
    MockDistributedTable *t1 = mock_get_distributed_table(table1);
    MockDistributedTable *t2 = mock_get_distributed_table(table2);

    if (!t1 || !t2) return false;
    if (t1->colocation_group == 0 || t2->colocation_group == 0) return false;

    return t1->colocation_group == t2->colocation_group;
}

/* Balance statistics */
static MockShardBalanceStats mock_get_shard_balance_stats(void)
{
    MockShardBalanceStats stats = {0};

    /* Count shards per node */
    int node_shard_counts[MAX_NODES] = {0};
    int active_nodes = 0;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].is_active)
            active_nodes++;
    }

    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use)
        {
            int node_idx = mock_shards[i].node_id - 1;
            if (node_idx >= 0 && node_idx < MAX_NODES)
                node_shard_counts[node_idx]++;
            stats.total_shards++;
        }
    }

    stats.total_nodes = active_nodes;

    if (active_nodes == 0)
    {
        stats.needs_rebalancing = false;
        return stats;
    }

    /* Find min/max */
    stats.min_shards = INT64_MAX;
    stats.max_shards = 0;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].is_active)
        {
            if (node_shard_counts[i] < stats.min_shards)
                stats.min_shards = node_shard_counts[i];
            if (node_shard_counts[i] > stats.max_shards)
                stats.max_shards = node_shard_counts[i];
        }
    }

    stats.avg_shards_per_node = (double)stats.total_shards / active_nodes;

    if (stats.min_shards > 0)
        stats.imbalance_ratio = (double)stats.max_shards / stats.min_shards;
    else
        stats.imbalance_ratio = (stats.max_shards > 0) ? 999.0 : 1.0;

    stats.needs_rebalancing = (stats.imbalance_ratio > 1.2);

    return stats;
}

/* Move shard */
static void mock_move_shard(int64 shard_id, int32 target_node)
{
    MockShard *shard = mock_get_shard(shard_id);
    if (shard)
    {
        shard->node_id = target_node;
    }
}

/* Split shard */
static bool mock_split_shard(int64 shard_id, int32 node1, int32 node2)
{
    MockShard *shard = mock_get_shard(shard_id);
    if (!shard) return false;

    int32 mid = shard->hash_min + (shard->hash_max - shard->hash_min) / 2;

    /* Update existing shard to cover lower half */
    int32 old_max = shard->hash_max;
    shard->hash_max = mid;
    shard->node_id = node1;

    /* Create new shard for upper half */
    mock_create_shard(shard->table_oid, shard->shard_index + 1000,
                      mid + 1, old_max, node2);

    return true;
}

/* ============================================================
 * Test Cases
 * ============================================================ */

/* Test 1: Integer hash function */
static void test_hash_int32(void)
{
    TEST_BEGIN("hash_int32")
        int32 h1 = mock_hash_int32(100);
        int32 h2 = mock_hash_int32(100);
        int32 h3 = mock_hash_int32(101);

        TEST_ASSERT_EQ(h1, h2);  /* Same input = same hash */
        TEST_ASSERT_NEQ(h1, h3); /* Different input = different hash */
        TEST_ASSERT_GTE(h1, 0);  /* Hash should be positive */
    TEST_END();
}

/* Test 2: String hash function */
static void test_hash_string(void)
{
    TEST_BEGIN("hash_string")
        int32 h1 = mock_hash_string("hello");
        int32 h2 = mock_hash_string("hello");
        int32 h3 = mock_hash_string("world");

        TEST_ASSERT_EQ(h1, h2);
        TEST_ASSERT_NEQ(h1, h3);
        TEST_ASSERT_EQ(0, mock_hash_string(NULL));
    TEST_END();
}

/* Test 3: CRC32 hash */
static void test_crc32_hash(void)
{
    TEST_BEGIN("crc32_hash")
        const char *data = "test data";
        uint32_t h1 = mock_crc32_hash(data, strlen(data));
        uint32_t h2 = mock_crc32_hash(data, strlen(data));
        uint32_t h3 = mock_crc32_hash("other", 5);

        TEST_ASSERT_EQ(h1, h2);
        TEST_ASSERT_NEQ(h1, h3);
    TEST_END();
}

/* Test 4: Shard index computation */
static void test_shard_index_computation(void)
{
    TEST_BEGIN("shard_index_computation")
        /* Test with 4 shards */
        int shard_count = 4;

        int32 idx1 = mock_get_shard_index(0, shard_count);
        int32 idx2 = mock_get_shard_index(HASH_MODULUS / 2, shard_count);
        int32 idx3 = mock_get_shard_index(HASH_MODULUS - 1, shard_count);

        TEST_ASSERT_EQ(0, idx1);    /* Beginning = shard 0 */
        /* HASH_MODULUS/2 * 4 / HASH_MODULUS = 2, but due to integer division quirks may be 1 */
        TEST_ASSERT_GTE(idx2, 1);
        TEST_ASSERT_LTE(idx2, 2);
        TEST_ASSERT_LT(idx3, shard_count);  /* End = within range */

        /* All indices should be valid */
        for (int i = 0; i < 100; i++)
        {
            int32 hash = mock_hash_int32(i);
            int32 idx = mock_get_shard_index(hash, shard_count);
            TEST_ASSERT_GTE(idx, 0);
            TEST_ASSERT_LT(idx, shard_count);
        }
    TEST_END();
}

/* Test 5: Create distributed table */
static void test_create_distributed_table(void)
{
    TEST_BEGIN("create_distributed_table")
        mock_distribution_init();

        MockDistributedTable *t = mock_create_distributed_table(
            1001, "users", "user_id", MOCK_SHARD_HASH, 32);

        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQ(1001, t->table_oid);
        TEST_ASSERT_STR_EQ("users", t->table_name);
        TEST_ASSERT_STR_EQ("user_id", t->distribution_column);
        TEST_ASSERT_EQ(MOCK_SHARD_HASH, t->strategy);
        TEST_ASSERT_EQ(32, t->shard_count);
        TEST_ASSERT_FALSE(t->is_reference);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 6: Create reference table */
static void test_create_reference_table(void)
{
    TEST_BEGIN("create_reference_table")
        mock_distribution_init();

        MockDistributedTable *t = mock_create_distributed_table(
            1001, "countries", "", MOCK_SHARD_REFERENCE, 1);

        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT(t->is_reference);
        TEST_ASSERT(mock_is_reference_table(1001));
        TEST_ASSERT_FALSE(mock_is_distributed_table(1001));

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 7: Shard creation */
static void test_shard_creation(void)
{
    TEST_BEGIN("shard_creation")
        mock_distribution_init();

        mock_create_distributed_table(1001, "users", "user_id", MOCK_SHARD_HASH, 4);
        mock_create_shards_for_table(1001, 4, 2);

        TEST_ASSERT_EQ(4, mock_count_table_shards(1001));

        /* Verify shard ranges cover entire hash space */
        MockShard *s0 = mock_route_to_shard(1001, 0);
        MockShard *s_end = mock_route_to_shard(1001, HASH_MODULUS - 1);

        TEST_ASSERT_NOT_NULL(s0);
        TEST_ASSERT_NOT_NULL(s_end);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 8: Shard routing */
static void test_shard_routing(void)
{
    TEST_BEGIN("shard_routing")
        mock_distribution_init();

        mock_create_distributed_table(1001, "orders", "order_id", MOCK_SHARD_HASH, 8);
        mock_create_shards_for_table(1001, 8, 4);

        /* Route multiple values and verify consistency */
        for (int i = 0; i < 100; i++)
        {
            int32 hash = mock_hash_int32(i);
            MockShard *shard = mock_route_to_shard(1001, hash);

            TEST_ASSERT_NOT_NULL(shard);
            TEST_ASSERT_GTE(hash, shard->hash_min);
            TEST_ASSERT_LTE(hash, shard->hash_max);

            /* Same value should route to same shard */
            MockShard *shard2 = mock_route_to_shard(1001, hash);
            TEST_ASSERT_EQ(shard->shard_id, shard2->shard_id);
        }

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 9: Get shards in range */
static void test_shards_in_range(void)
{
    TEST_BEGIN("shards_in_range")
        mock_distribution_init();

        mock_create_distributed_table(1001, "events", "event_id", MOCK_SHARD_HASH, 8);
        mock_create_shards_for_table(1001, 8, 4);

        MockShard *results[10];

        /* Get shards covering middle of hash range */
        int count = mock_get_shards_in_range(1001, HASH_MODULUS / 4,
                                              HASH_MODULUS / 2, results, 10);
        TEST_ASSERT_GT(count, 0);
        TEST_ASSERT_LTE(count, 8);

        /* Get all shards */
        count = mock_get_shards_in_range(1001, 0, HASH_MODULUS, results, 10);
        TEST_ASSERT_EQ(8, count);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 10: Node management */
static void test_node_management(void)
{
    TEST_BEGIN("node_management")
        mock_distribution_init();

        int32 n1 = mock_add_node("node1.local", 5432);
        int32 n2 = mock_add_node("node2.local", 5432);
        int32 n3 = mock_add_node("node3.local", 5432);

        TEST_ASSERT_GT(n1, 0);
        TEST_ASSERT_GT(n2, 0);
        TEST_ASSERT_GT(n3, 0);
        TEST_ASSERT_EQ(3, mock_count_active_nodes());

        MockNode *node = mock_get_node(n2);
        TEST_ASSERT_NOT_NULL(node);
        TEST_ASSERT_STR_EQ("node2.local", node->hostname);
        TEST_ASSERT_EQ(5432, node->port);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 11: Node selection for new shard */
static void test_node_selection(void)
{
    TEST_BEGIN("node_selection")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_add_node("node2.local", 5432);

        /* Initially both nodes have 0 shards */
        int32 selected = mock_select_node_for_shard();
        TEST_ASSERT_GT(selected, 0);

        /* Add shards to node 1 */
        mock_nodes[0].shard_count = 10;
        mock_nodes[1].shard_count = 5;

        selected = mock_select_node_for_shard();
        TEST_ASSERT_EQ(2, selected);  /* Should select node 2 (fewer shards) */

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 12: Colocation groups */
static void test_colocation_groups(void)
{
    TEST_BEGIN("colocation_groups")
        mock_distribution_init();

        int32 group = mock_create_colocation_group(32, MOCK_SHARD_HASH);
        TEST_ASSERT_GT(group, 0);

        MockDistributedTable *t1 = mock_create_distributed_table(
            1001, "orders", "customer_id", MOCK_SHARD_HASH, 32);
        MockDistributedTable *t2 = mock_create_distributed_table(
            1002, "order_items", "customer_id", MOCK_SHARD_HASH, 32);

        t1->colocation_group = group;
        t2->colocation_group = group;

        TEST_ASSERT(mock_tables_are_colocated(1001, 1002));

        MockDistributedTable *t3 = mock_create_distributed_table(
            1003, "products", "product_id", MOCK_SHARD_HASH, 16);
        TEST_ASSERT_FALSE(mock_tables_are_colocated(1001, 1003));

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 13: Shard balance statistics */
static void test_shard_balance_stats(void)
{
    TEST_BEGIN("shard_balance_stats")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_add_node("node2.local", 5432);

        mock_create_distributed_table(1001, "test", "id", MOCK_SHARD_HASH, 10);
        mock_create_shards_for_table(1001, 10, 2);

        MockShardBalanceStats stats = mock_get_shard_balance_stats();

        TEST_ASSERT_EQ(2, stats.total_nodes);
        TEST_ASSERT_EQ(10, stats.total_shards);
        TEST_ASSERT_EQ(5.0, stats.avg_shards_per_node);
        TEST_ASSERT_NEAR(1.0, stats.imbalance_ratio, 0.1);
        TEST_ASSERT_FALSE(stats.needs_rebalancing);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 14: Move shard */
static void test_move_shard(void)
{
    TEST_BEGIN("move_shard")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_add_node("node2.local", 5432);

        mock_create_distributed_table(1001, "test", "id", MOCK_SHARD_HASH, 4);
        mock_create_shards_for_table(1001, 4, 2);

        int64 shard_id = mock_shards[0].shard_id;
        int32 original_node = mock_shards[0].node_id;

        int32 target_node = (original_node == 1) ? 2 : 1;
        mock_move_shard(shard_id, target_node);

        MockShard *shard = mock_get_shard(shard_id);
        TEST_ASSERT_EQ(target_node, shard->node_id);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 15: Split shard */
static void test_split_shard(void)
{
    TEST_BEGIN("split_shard")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_add_node("node2.local", 5432);

        mock_create_distributed_table(1001, "test", "id", MOCK_SHARD_HASH, 2);
        mock_create_shards_for_table(1001, 2, 2);

        int original_count = mock_count_table_shards(1001);
        TEST_ASSERT_EQ(2, original_count);

        int64 shard_id = mock_shards[0].shard_id;
        bool split = mock_split_shard(shard_id, 1, 2);
        TEST_ASSERT(split);

        TEST_ASSERT_EQ(3, mock_count_table_shards(1001));

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 16: Hash distribution uniformity */
static void test_hash_distribution_uniformity(void)
{
    TEST_BEGIN("hash_distribution_uniformity")
        int shard_count = 8;
        int counts[8] = {0};
        int num_values = 10000;

        for (int i = 0; i < num_values; i++)
        {
            int32 hash = mock_hash_int32(i);
            int32 idx = mock_get_shard_index(hash, shard_count);
            counts[idx]++;
        }

        /* Each shard should have roughly num_values/shard_count */
        double expected = (double)num_values / shard_count;
        bool uniform = true;

        for (int i = 0; i < shard_count; i++)
        {
            double ratio = (double)counts[i] / expected;
            /* Allow 30% deviation */
            if (ratio < 0.7 || ratio > 1.3)
            {
                uniform = false;
            }
        }

        TEST_ASSERT(uniform);
    TEST_END();
}

/* Test 17: Different shard strategies */
static void test_shard_strategies(void)
{
    TEST_BEGIN("shard_strategies")
        mock_distribution_init();

        MockDistributedTable *hash_t = mock_create_distributed_table(
            1001, "hash_table", "id", MOCK_SHARD_HASH, 8);
        TEST_ASSERT_EQ(MOCK_SHARD_HASH, hash_t->strategy);

        MockDistributedTable *range_t = mock_create_distributed_table(
            1002, "range_table", "created_at", MOCK_SHARD_RANGE, 12);
        TEST_ASSERT_EQ(MOCK_SHARD_RANGE, range_t->strategy);

        MockDistributedTable *ref_t = mock_create_distributed_table(
            1003, "ref_table", "", MOCK_SHARD_REFERENCE, 1);
        TEST_ASSERT_EQ(MOCK_SHARD_REFERENCE, ref_t->strategy);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 18: Shard statistics */
static void test_shard_statistics(void)
{
    TEST_BEGIN("shard_statistics")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_create_distributed_table(1001, "test", "id", MOCK_SHARD_HASH, 1);
        int64 shard_id = mock_create_shard(1001, 0, 0, HASH_MODULUS, 1);

        MockShard *shard = mock_get_shard(shard_id);
        shard->row_count = 1000000;
        shard->size_bytes = 1073741824;  /* 1GB */

        TEST_ASSERT_EQ(1000000, shard->row_count);
        TEST_ASSERT_EQ(1073741824, shard->size_bytes);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 19: Multiple tables isolation */
static void test_multiple_tables_isolation(void)
{
    TEST_BEGIN("multiple_tables_isolation")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);

        mock_create_distributed_table(1001, "table1", "id", MOCK_SHARD_HASH, 4);
        mock_create_shards_for_table(1001, 4, 1);

        mock_create_distributed_table(1002, "table2", "id", MOCK_SHARD_HASH, 8);
        mock_create_shards_for_table(1002, 8, 1);

        TEST_ASSERT_EQ(4, mock_count_table_shards(1001));
        TEST_ASSERT_EQ(8, mock_count_table_shards(1002));

        /* Routing should be table-specific */
        int32 hash = mock_hash_int32(42);
        MockShard *s1 = mock_route_to_shard(1001, hash);
        MockShard *s2 = mock_route_to_shard(1002, hash);

        TEST_ASSERT_EQ(1001, s1->table_oid);
        TEST_ASSERT_EQ(1002, s2->table_oid);

        mock_distribution_cleanup();
    TEST_END();
}

/* Test 20: Imbalance detection */
static void test_imbalance_detection(void)
{
    TEST_BEGIN("imbalance_detection")
        mock_distribution_init();

        mock_add_node("node1.local", 5432);
        mock_add_node("node2.local", 5432);

        /* Create imbalanced distribution */
        mock_nodes[0].shard_count = 100;
        mock_nodes[1].shard_count = 10;

        /* Add some shards to count */
        for (int i = 0; i < 110; i++)
        {
            mock_shards[i].in_use = true;
            mock_shards[i].shard_id = i + 1;
            mock_shards[i].node_id = (i < 100) ? 1 : 2;
        }

        MockShardBalanceStats stats = mock_get_shard_balance_stats();

        TEST_ASSERT_EQ(110, stats.total_shards);
        TEST_ASSERT_GT(stats.imbalance_ratio, 1.2);
        TEST_ASSERT(stats.needs_rebalancing);

        mock_distribution_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_distribution(void)
{
    test_hash_int32();
    test_hash_string();
    test_crc32_hash();
    test_shard_index_computation();
    test_create_distributed_table();
    test_create_reference_table();
    test_shard_creation();
    test_shard_routing();
    test_shards_in_range();
    test_node_management();
    test_node_selection();
    test_colocation_groups();
    test_shard_balance_stats();
    test_move_shard();
    test_split_shard();
    test_hash_distribution_uniformity();
    test_shard_strategies();
    test_shard_statistics();
    test_multiple_tables_isolation();
    test_imbalance_detection();
}
