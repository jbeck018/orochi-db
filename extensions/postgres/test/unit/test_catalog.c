/*-------------------------------------------------------------------------
 *
 * test_catalog.c
 *    Unit tests for Orochi DB catalog operations
 *
 * Tests table metadata CRUD, shard mapping lookups, and policy management.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Mock Catalog State (In-Memory Simulation)
 * ============================================================ */

#define MAX_TABLES      100
#define MAX_SHARDS      1000
#define MAX_NODES       10
#define MAX_POLICIES    50

/* Storage types (mirror orochi.h) */
typedef enum MockStorageType
{
    MOCK_STORAGE_ROW = 0,
    MOCK_STORAGE_COLUMNAR,
    MOCK_STORAGE_HYBRID,
    MOCK_STORAGE_VECTOR
} MockStorageType;

/* Shard strategies */
typedef enum MockShardStrategy
{
    MOCK_SHARD_HASH = 0,
    MOCK_SHARD_RANGE,
    MOCK_SHARD_LIST,
    MOCK_SHARD_COMPOSITE,
    MOCK_SHARD_REFERENCE
} MockShardStrategy;

/* Storage tiers */
typedef enum MockStorageTier
{
    MOCK_TIER_HOT = 0,
    MOCK_TIER_WARM,
    MOCK_TIER_COLD,
    MOCK_TIER_FROZEN
} MockStorageTier;

/* Node roles */
typedef enum MockNodeRole
{
    MOCK_NODE_COORDINATOR = 0,
    MOCK_NODE_WORKER,
    MOCK_NODE_REPLICA
} MockNodeRole;

/* Table metadata */
typedef struct MockTableInfo
{
    Oid                 relid;
    char                schema_name[64];
    char                table_name[64];
    MockStorageType     storage_type;
    MockShardStrategy   shard_strategy;
    int                 shard_count;
    char                distribution_column[64];
    bool                is_distributed;
    bool                is_timeseries;
    char                time_column[64];
    bool                in_use;
} MockTableInfo;

/* Shard metadata */
typedef struct MockShardInfo
{
    int64               shard_id;
    Oid                 table_oid;
    int32               shard_index;
    int32               hash_min;
    int32               hash_max;
    int32               node_id;
    int64               row_count;
    int64               size_bytes;
    MockStorageTier     storage_tier;
    TimestampTz         last_accessed;
    bool                in_use;
} MockShardInfo;

/* Node metadata */
typedef struct MockNodeInfo
{
    int32               node_id;
    char                hostname[256];
    int                 port;
    MockNodeRole        role;
    bool                is_active;
    int64               shard_count;
    int64               total_size;
    bool                in_use;
} MockNodeInfo;

/* Tiering policy */
typedef struct MockTieringPolicy
{
    int64               policy_id;
    Oid                 table_oid;
    int64               hot_to_warm_hours;
    int64               warm_to_cold_hours;
    bool                compress_on_tier;
    bool                enabled;
    bool                in_use;
} MockTieringPolicy;

/* Global catalog state */
static MockTableInfo mock_tables[MAX_TABLES];
static MockShardInfo mock_shards[MAX_SHARDS];
static MockNodeInfo mock_nodes[MAX_NODES];
static MockTieringPolicy mock_policies[MAX_POLICIES];
static int64 next_shard_id = 1;
static int32 next_node_id = 1;
static int64 next_policy_id = 1;
static bool catalog_initialized = false;

/* ============================================================
 * Mock Catalog Functions
 * ============================================================ */

static void mock_catalog_init(void)
{
    memset(mock_tables, 0, sizeof(mock_tables));
    memset(mock_shards, 0, sizeof(mock_shards));
    memset(mock_nodes, 0, sizeof(mock_nodes));
    memset(mock_policies, 0, sizeof(mock_policies));
    next_shard_id = 1;
    next_node_id = 1;
    next_policy_id = 1;
    catalog_initialized = true;
}

static void mock_catalog_cleanup(void)
{
    catalog_initialized = false;
}

static bool mock_catalog_exists(void)
{
    return catalog_initialized;
}

/* Table operations */
static MockTableInfo *mock_catalog_register_table(Oid relid, const char *schema,
                                                   const char *table_name)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (!mock_tables[i].in_use)
        {
            mock_tables[i].relid = relid;
            strncpy(mock_tables[i].schema_name, schema, 63);
            strncpy(mock_tables[i].table_name, table_name, 63);
            mock_tables[i].storage_type = MOCK_STORAGE_ROW;
            mock_tables[i].shard_strategy = MOCK_SHARD_HASH;
            mock_tables[i].shard_count = 0;
            mock_tables[i].is_distributed = false;
            mock_tables[i].is_timeseries = false;
            mock_tables[i].in_use = true;
            return &mock_tables[i];
        }
    }
    return NULL;
}

static MockTableInfo *mock_catalog_get_table(Oid relid)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (mock_tables[i].in_use && mock_tables[i].relid == relid)
            return &mock_tables[i];
    }
    return NULL;
}

static MockTableInfo *mock_catalog_get_table_by_name(const char *schema,
                                                      const char *table_name)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (mock_tables[i].in_use &&
            strcmp(mock_tables[i].schema_name, schema) == 0 &&
            strcmp(mock_tables[i].table_name, table_name) == 0)
            return &mock_tables[i];
    }
    return NULL;
}

static bool mock_catalog_remove_table(Oid relid)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (mock_tables[i].in_use && mock_tables[i].relid == relid)
        {
            mock_tables[i].in_use = false;
            return true;
        }
    }
    return false;
}

static int mock_catalog_count_tables(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (mock_tables[i].in_use)
            count++;
    }
    return count;
}

static bool mock_catalog_is_orochi_table(Oid relid)
{
    return mock_catalog_get_table(relid) != NULL;
}

/* Shard operations */
static int64 mock_catalog_create_shard(Oid table_oid, int32 hash_min,
                                        int32 hash_max, int32 node_id)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (!mock_shards[i].in_use)
        {
            mock_shards[i].shard_id = next_shard_id++;
            mock_shards[i].table_oid = table_oid;
            mock_shards[i].hash_min = hash_min;
            mock_shards[i].hash_max = hash_max;
            mock_shards[i].node_id = node_id;
            mock_shards[i].storage_tier = MOCK_TIER_HOT;
            mock_shards[i].row_count = 0;
            mock_shards[i].size_bytes = 0;
            mock_shards[i].in_use = true;
            return mock_shards[i].shard_id;
        }
    }
    return -1;
}

static MockShardInfo *mock_catalog_get_shard(int64 shard_id)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use && mock_shards[i].shard_id == shard_id)
            return &mock_shards[i];
    }
    return NULL;
}

static MockShardInfo *mock_catalog_get_shard_for_hash(Oid table_oid, int32 hash_value)
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

static int mock_catalog_count_table_shards(Oid table_oid)
{
    int count = 0;
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use && mock_shards[i].table_oid == table_oid)
            count++;
    }
    return count;
}

static bool mock_catalog_delete_shard(int64 shard_id)
{
    for (int i = 0; i < MAX_SHARDS; i++)
    {
        if (mock_shards[i].in_use && mock_shards[i].shard_id == shard_id)
        {
            mock_shards[i].in_use = false;
            return true;
        }
    }
    return false;
}

static void mock_catalog_update_shard_stats(int64 shard_id, int64 row_count,
                                             int64 size_bytes)
{
    MockShardInfo *shard = mock_catalog_get_shard(shard_id);
    if (shard)
    {
        shard->row_count = row_count;
        shard->size_bytes = size_bytes;
    }
}

static void mock_catalog_update_shard_placement(int64 shard_id, int32 node_id)
{
    MockShardInfo *shard = mock_catalog_get_shard(shard_id);
    if (shard)
    {
        shard->node_id = node_id;
    }
}

/* Node operations */
static int32 mock_catalog_add_node(const char *hostname, int port, MockNodeRole role)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (!mock_nodes[i].in_use)
        {
            mock_nodes[i].node_id = next_node_id++;
            strncpy(mock_nodes[i].hostname, hostname, 255);
            mock_nodes[i].port = port;
            mock_nodes[i].role = role;
            mock_nodes[i].is_active = true;
            mock_nodes[i].shard_count = 0;
            mock_nodes[i].total_size = 0;
            mock_nodes[i].in_use = true;
            return mock_nodes[i].node_id;
        }
    }
    return -1;
}

static MockNodeInfo *mock_catalog_get_node(int32 node_id)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].node_id == node_id)
            return &mock_nodes[i];
    }
    return NULL;
}

static int mock_catalog_count_active_nodes(void)
{
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].is_active)
            count++;
    }
    return count;
}

static void mock_catalog_update_node_status(int32 node_id, bool is_active)
{
    MockNodeInfo *node = mock_catalog_get_node(node_id);
    if (node)
    {
        node->is_active = is_active;
    }
}

static bool mock_catalog_remove_node(int32 node_id)
{
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (mock_nodes[i].in_use && mock_nodes[i].node_id == node_id)
        {
            mock_nodes[i].in_use = false;
            return true;
        }
    }
    return false;
}

/* Policy operations */
static int64 mock_catalog_create_tiering_policy(Oid table_oid, int64 hot_to_warm,
                                                 int64 warm_to_cold, bool compress)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (!mock_policies[i].in_use)
        {
            mock_policies[i].policy_id = next_policy_id++;
            mock_policies[i].table_oid = table_oid;
            mock_policies[i].hot_to_warm_hours = hot_to_warm;
            mock_policies[i].warm_to_cold_hours = warm_to_cold;
            mock_policies[i].compress_on_tier = compress;
            mock_policies[i].enabled = true;
            mock_policies[i].in_use = true;
            return mock_policies[i].policy_id;
        }
    }
    return -1;
}

static MockTieringPolicy *mock_catalog_get_tiering_policy(Oid table_oid)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (mock_policies[i].in_use && mock_policies[i].table_oid == table_oid)
            return &mock_policies[i];
    }
    return NULL;
}

static bool mock_catalog_delete_tiering_policy(int64 policy_id)
{
    for (int i = 0; i < MAX_POLICIES; i++)
    {
        if (mock_policies[i].in_use && mock_policies[i].policy_id == policy_id)
        {
            mock_policies[i].in_use = false;
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Test Cases
 * ============================================================ */

/* Test 1: Catalog initialization */
static void test_catalog_init(void)
{
    TEST_BEGIN("catalog_initialization")
        mock_catalog_init();
        TEST_ASSERT(mock_catalog_exists());
        TEST_ASSERT_EQ(0, mock_catalog_count_tables());
        mock_catalog_cleanup();
        TEST_ASSERT_FALSE(mock_catalog_exists());
    TEST_END();
}

/* Test 2: Table registration */
static void test_table_registration(void)
{
    TEST_BEGIN("table_registration")
        mock_catalog_init();

        MockTableInfo *table = mock_catalog_register_table(1001, "public", "users");
        TEST_ASSERT_NOT_NULL(table);
        TEST_ASSERT_EQ(1001, table->relid);
        TEST_ASSERT_STR_EQ("public", table->schema_name);
        TEST_ASSERT_STR_EQ("users", table->table_name);
        TEST_ASSERT_FALSE(table->is_distributed);
        TEST_ASSERT_EQ(1, mock_catalog_count_tables());

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 3: Table lookup by OID */
static void test_table_lookup_by_oid(void)
{
    TEST_BEGIN("table_lookup_by_oid")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        mock_catalog_register_table(1002, "public", "orders");
        mock_catalog_register_table(1003, "analytics", "events");

        MockTableInfo *table = mock_catalog_get_table(1002);
        TEST_ASSERT_NOT_NULL(table);
        TEST_ASSERT_STR_EQ("orders", table->table_name);

        MockTableInfo *missing = mock_catalog_get_table(9999);
        TEST_ASSERT_NULL(missing);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 4: Table lookup by name */
static void test_table_lookup_by_name(void)
{
    TEST_BEGIN("table_lookup_by_name")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        mock_catalog_register_table(1002, "analytics", "users");

        MockTableInfo *table1 = mock_catalog_get_table_by_name("public", "users");
        TEST_ASSERT_NOT_NULL(table1);
        TEST_ASSERT_EQ(1001, table1->relid);

        MockTableInfo *table2 = mock_catalog_get_table_by_name("analytics", "users");
        TEST_ASSERT_NOT_NULL(table2);
        TEST_ASSERT_EQ(1002, table2->relid);

        MockTableInfo *missing = mock_catalog_get_table_by_name("public", "nonexistent");
        TEST_ASSERT_NULL(missing);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 5: Table removal */
static void test_table_removal(void)
{
    TEST_BEGIN("table_removal")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        mock_catalog_register_table(1002, "public", "orders");
        TEST_ASSERT_EQ(2, mock_catalog_count_tables());

        bool removed = mock_catalog_remove_table(1001);
        TEST_ASSERT(removed);
        TEST_ASSERT_EQ(1, mock_catalog_count_tables());

        TEST_ASSERT_NULL(mock_catalog_get_table(1001));
        TEST_ASSERT_NOT_NULL(mock_catalog_get_table(1002));

        /* Remove non-existent table */
        removed = mock_catalog_remove_table(9999);
        TEST_ASSERT_FALSE(removed);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 6: Table metadata update */
static void test_table_metadata_update(void)
{
    TEST_BEGIN("table_metadata_update")
        mock_catalog_init();

        MockTableInfo *table = mock_catalog_register_table(1001, "public", "users");
        TEST_ASSERT_NOT_NULL(table);

        /* Update table properties */
        table->storage_type = MOCK_STORAGE_COLUMNAR;
        table->shard_strategy = MOCK_SHARD_HASH;
        table->shard_count = 32;
        table->is_distributed = true;
        strncpy(table->distribution_column, "user_id", 63);

        /* Verify updates persist */
        MockTableInfo *retrieved = mock_catalog_get_table(1001);
        TEST_ASSERT_NOT_NULL(retrieved);
        TEST_ASSERT_EQ(MOCK_STORAGE_COLUMNAR, retrieved->storage_type);
        TEST_ASSERT_EQ(MOCK_SHARD_HASH, retrieved->shard_strategy);
        TEST_ASSERT_EQ(32, retrieved->shard_count);
        TEST_ASSERT(retrieved->is_distributed);
        TEST_ASSERT_STR_EQ("user_id", retrieved->distribution_column);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 7: Shard creation and lookup */
static void test_shard_creation(void)
{
    TEST_BEGIN("shard_creation")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");

        /* Create shards with hash ranges */
        int64 shard1 = mock_catalog_create_shard(1001, 0, 1073741823, 1);
        int64 shard2 = mock_catalog_create_shard(1001, 1073741824, 2147483647, 2);

        TEST_ASSERT_GT(shard1, 0);
        TEST_ASSERT_GT(shard2, 0);
        TEST_ASSERT_NEQ(shard1, shard2);
        TEST_ASSERT_EQ(2, mock_catalog_count_table_shards(1001));

        MockShardInfo *s1 = mock_catalog_get_shard(shard1);
        TEST_ASSERT_NOT_NULL(s1);
        TEST_ASSERT_EQ(0, s1->hash_min);
        TEST_ASSERT_EQ(1073741823, s1->hash_max);
        TEST_ASSERT_EQ(1, s1->node_id);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 8: Shard mapping lookup */
static void test_shard_mapping_lookup(void)
{
    TEST_BEGIN("shard_mapping_lookup")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");

        /* Create 4 shards covering full hash range */
        mock_catalog_create_shard(1001, 0, 536870911, 1);
        mock_catalog_create_shard(1001, 536870912, 1073741823, 2);
        mock_catalog_create_shard(1001, 1073741824, 1610612735, 3);
        mock_catalog_create_shard(1001, 1610612736, 2147483647, 4);

        /* Test hash routing */
        MockShardInfo *s1 = mock_catalog_get_shard_for_hash(1001, 100);
        TEST_ASSERT_NOT_NULL(s1);
        TEST_ASSERT_EQ(1, s1->node_id);

        MockShardInfo *s2 = mock_catalog_get_shard_for_hash(1001, 600000000);
        TEST_ASSERT_NOT_NULL(s2);
        TEST_ASSERT_EQ(2, s2->node_id);

        MockShardInfo *s4 = mock_catalog_get_shard_for_hash(1001, 2000000000);
        TEST_ASSERT_NOT_NULL(s4);
        TEST_ASSERT_EQ(4, s4->node_id);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 9: Shard statistics update */
static void test_shard_stats_update(void)
{
    TEST_BEGIN("shard_stats_update")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        int64 shard_id = mock_catalog_create_shard(1001, 0, 2147483647, 1);

        MockShardInfo *shard = mock_catalog_get_shard(shard_id);
        TEST_ASSERT_EQ(0, shard->row_count);
        TEST_ASSERT_EQ(0, shard->size_bytes);

        mock_catalog_update_shard_stats(shard_id, 100000, 52428800);

        shard = mock_catalog_get_shard(shard_id);
        TEST_ASSERT_EQ(100000, shard->row_count);
        TEST_ASSERT_EQ(52428800, shard->size_bytes);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 10: Shard placement update */
static void test_shard_placement_update(void)
{
    TEST_BEGIN("shard_placement_update")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        int64 shard_id = mock_catalog_create_shard(1001, 0, 2147483647, 1);

        MockShardInfo *shard = mock_catalog_get_shard(shard_id);
        TEST_ASSERT_EQ(1, shard->node_id);

        mock_catalog_update_shard_placement(shard_id, 3);

        shard = mock_catalog_get_shard(shard_id);
        TEST_ASSERT_EQ(3, shard->node_id);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 11: Shard deletion */
static void test_shard_deletion(void)
{
    TEST_BEGIN("shard_deletion")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");
        int64 shard1 = mock_catalog_create_shard(1001, 0, 1073741823, 1);
        int64 shard2 = mock_catalog_create_shard(1001, 1073741824, 2147483647, 2);
        TEST_ASSERT_EQ(2, mock_catalog_count_table_shards(1001));

        bool deleted = mock_catalog_delete_shard(shard1);
        TEST_ASSERT(deleted);
        TEST_ASSERT_EQ(1, mock_catalog_count_table_shards(1001));
        TEST_ASSERT_NULL(mock_catalog_get_shard(shard1));
        TEST_ASSERT_NOT_NULL(mock_catalog_get_shard(shard2));

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 12: Node registration */
static void test_node_registration(void)
{
    TEST_BEGIN("node_registration")
        mock_catalog_init();

        int32 node1 = mock_catalog_add_node("worker1.local", 5432, MOCK_NODE_WORKER);
        int32 node2 = mock_catalog_add_node("worker2.local", 5432, MOCK_NODE_WORKER);
        int32 coord = mock_catalog_add_node("coordinator.local", 5432, MOCK_NODE_COORDINATOR);

        TEST_ASSERT_GT(node1, 0);
        TEST_ASSERT_GT(node2, 0);
        TEST_ASSERT_GT(coord, 0);
        TEST_ASSERT_EQ(3, mock_catalog_count_active_nodes());

        MockNodeInfo *n1 = mock_catalog_get_node(node1);
        TEST_ASSERT_NOT_NULL(n1);
        TEST_ASSERT_STR_EQ("worker1.local", n1->hostname);
        TEST_ASSERT_EQ(5432, n1->port);
        TEST_ASSERT_EQ(MOCK_NODE_WORKER, n1->role);
        TEST_ASSERT(n1->is_active);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 13: Node status update */
static void test_node_status_update(void)
{
    TEST_BEGIN("node_status_update")
        mock_catalog_init();

        int32 node1 = mock_catalog_add_node("worker1.local", 5432, MOCK_NODE_WORKER);
        int32 node2 = mock_catalog_add_node("worker2.local", 5432, MOCK_NODE_WORKER);
        TEST_ASSERT_EQ(2, mock_catalog_count_active_nodes());

        mock_catalog_update_node_status(node1, false);
        TEST_ASSERT_EQ(1, mock_catalog_count_active_nodes());

        MockNodeInfo *n1 = mock_catalog_get_node(node1);
        TEST_ASSERT_FALSE(n1->is_active);

        mock_catalog_update_node_status(node1, true);
        TEST_ASSERT_EQ(2, mock_catalog_count_active_nodes());

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 14: Node removal */
static void test_node_removal(void)
{
    TEST_BEGIN("node_removal")
        mock_catalog_init();

        int32 node1 = mock_catalog_add_node("worker1.local", 5432, MOCK_NODE_WORKER);
        int32 node2 = mock_catalog_add_node("worker2.local", 5432, MOCK_NODE_WORKER);

        bool removed = mock_catalog_remove_node(node1);
        TEST_ASSERT(removed);
        TEST_ASSERT_NULL(mock_catalog_get_node(node1));
        TEST_ASSERT_NOT_NULL(mock_catalog_get_node(node2));

        removed = mock_catalog_remove_node(9999);
        TEST_ASSERT_FALSE(removed);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 15: Tiering policy creation */
static void test_tiering_policy_creation(void)
{
    TEST_BEGIN("tiering_policy_creation")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "events");

        int64 policy_id = mock_catalog_create_tiering_policy(1001, 24, 168, true);
        TEST_ASSERT_GT(policy_id, 0);

        MockTieringPolicy *policy = mock_catalog_get_tiering_policy(1001);
        TEST_ASSERT_NOT_NULL(policy);
        TEST_ASSERT_EQ(1001, policy->table_oid);
        TEST_ASSERT_EQ(24, policy->hot_to_warm_hours);
        TEST_ASSERT_EQ(168, policy->warm_to_cold_hours);
        TEST_ASSERT(policy->compress_on_tier);
        TEST_ASSERT(policy->enabled);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 16: Tiering policy deletion */
static void test_tiering_policy_deletion(void)
{
    TEST_BEGIN("tiering_policy_deletion")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "events");
        int64 policy_id = mock_catalog_create_tiering_policy(1001, 24, 168, true);

        TEST_ASSERT_NOT_NULL(mock_catalog_get_tiering_policy(1001));

        bool deleted = mock_catalog_delete_tiering_policy(policy_id);
        TEST_ASSERT(deleted);
        TEST_ASSERT_NULL(mock_catalog_get_tiering_policy(1001));

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 17: Is Orochi table check */
static void test_is_orochi_table(void)
{
    TEST_BEGIN("is_orochi_table_check")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "users");

        TEST_ASSERT(mock_catalog_is_orochi_table(1001));
        TEST_ASSERT_FALSE(mock_catalog_is_orochi_table(9999));

        mock_catalog_remove_table(1001);
        TEST_ASSERT_FALSE(mock_catalog_is_orochi_table(1001));

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 18: Multiple tables in same schema */
static void test_multiple_tables_same_schema(void)
{
    TEST_BEGIN("multiple_tables_same_schema")
        mock_catalog_init();

        mock_catalog_register_table(1001, "analytics", "events");
        mock_catalog_register_table(1002, "analytics", "sessions");
        mock_catalog_register_table(1003, "analytics", "users");
        mock_catalog_register_table(1004, "public", "config");

        TEST_ASSERT_EQ(4, mock_catalog_count_tables());

        /* Verify each table is distinct */
        MockTableInfo *t1 = mock_catalog_get_table(1001);
        MockTableInfo *t2 = mock_catalog_get_table(1002);
        MockTableInfo *t3 = mock_catalog_get_table(1003);
        MockTableInfo *t4 = mock_catalog_get_table(1004);

        TEST_ASSERT_STR_EQ("events", t1->table_name);
        TEST_ASSERT_STR_EQ("sessions", t2->table_name);
        TEST_ASSERT_STR_EQ("users", t3->table_name);
        TEST_ASSERT_STR_EQ("config", t4->table_name);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 19: Shard tier management */
static void test_shard_tier_management(void)
{
    TEST_BEGIN("shard_tier_management")
        mock_catalog_init();

        mock_catalog_register_table(1001, "public", "events");
        int64 shard_id = mock_catalog_create_shard(1001, 0, 2147483647, 1);

        MockShardInfo *shard = mock_catalog_get_shard(shard_id);
        TEST_ASSERT_EQ(MOCK_TIER_HOT, shard->storage_tier);

        /* Simulate tier transitions */
        shard->storage_tier = MOCK_TIER_WARM;
        TEST_ASSERT_EQ(MOCK_TIER_WARM, mock_catalog_get_shard(shard_id)->storage_tier);

        shard->storage_tier = MOCK_TIER_COLD;
        TEST_ASSERT_EQ(MOCK_TIER_COLD, mock_catalog_get_shard(shard_id)->storage_tier);

        mock_catalog_cleanup();
    TEST_END();
}

/* Test 20: Max capacity handling */
static void test_max_capacity_handling(void)
{
    TEST_BEGIN("max_capacity_handling")
        mock_catalog_init();

        /* Fill up table capacity */
        int count = 0;
        for (int i = 0; i < MAX_TABLES + 10; i++)
        {
            char name[64];
            snprintf(name, sizeof(name), "table_%d", i);
            MockTableInfo *t = mock_catalog_register_table(i + 1000, "public", name);
            if (t != NULL)
                count++;
        }

        TEST_ASSERT_EQ(MAX_TABLES, count);
        TEST_ASSERT_EQ(MAX_TABLES, mock_catalog_count_tables());

        mock_catalog_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_catalog(void)
{
    test_catalog_init();
    test_table_registration();
    test_table_lookup_by_oid();
    test_table_lookup_by_name();
    test_table_removal();
    test_table_metadata_update();
    test_shard_creation();
    test_shard_mapping_lookup();
    test_shard_stats_update();
    test_shard_placement_update();
    test_shard_deletion();
    test_node_registration();
    test_node_status_update();
    test_node_removal();
    test_tiering_policy_creation();
    test_tiering_policy_deletion();
    test_is_orochi_table();
    test_multiple_tables_same_schema();
    test_shard_tier_management();
    test_max_capacity_handling();
}
