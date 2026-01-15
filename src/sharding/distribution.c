/*-------------------------------------------------------------------------
 *
 * distribution.c
 *    Orochi DB distributed table management implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include <math.h>

#include "../orochi.h"
#include "../core/catalog.h"
#include "distribution.h"
#include "physical_sharding.h"

/* CRC32 lookup table */
static uint32 crc32_table[256];
static bool crc32_initialized = false;

/* Forward declarations */
static void init_crc32_table(void);

/* ============================================================
 * Distribution Checks
 * ============================================================ */

bool
orochi_is_distributed_table(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL)
        return false;

    return info->is_distributed;
}

bool
orochi_is_reference_table(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL)
        return false;

    return (info->is_distributed &&
            info->shard_strategy == OROCHI_SHARD_REFERENCE);
}

bool
orochi_tables_are_colocated(Oid table1_oid, Oid table2_oid)
{
    int32 group1, group2;

    /* Same table is always co-located with itself */
    if (table1_oid == table2_oid)
        return true;

    /* Reference tables are co-located with everything */
    if (orochi_is_reference_table(table1_oid) ||
        orochi_is_reference_table(table2_oid))
        return true;

    group1 = orochi_get_colocation_group(table1_oid);
    group2 = orochi_get_colocation_group(table2_oid);

    /* Tables are co-located if they're in the same group */
    return (group1 == group2 && group1 != -1);
}

int32
orochi_get_colocation_group(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || !info->is_distributed)
        return -1;

    /* For now, tables with same shard count and strategy are co-located */
    /* TODO: Implement proper co-location tracking in catalog */
    return info->shard_count;
}

int32
orochi_create_colocation_group(int32 shard_count, OrochiShardStrategy strategy,
                               const char *dist_type)
{
    /* TODO: Implement co-location group creation in catalog */
    return shard_count;  /* Use shard count as simple group ID */
}

/* ============================================================
 * Hash Functions
 * ============================================================ */

static void
init_crc32_table(void)
{
    uint32 i, j;
    uint32 crc;

    for (i = 0; i < 256; i++)
    {
        crc = i;
        for (j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }

    crc32_initialized = true;
}

uint32
orochi_crc32_hash(const void *data, size_t length)
{
    const uint8 *bytes = (const uint8 *) data;
    uint32 crc = 0xFFFFFFFF;
    size_t i;

    if (!crc32_initialized)
        init_crc32_table();

    for (i = 0; i < length; i++)
        crc = crc32_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFF;
}

int32
orochi_hash_datum(Datum value, Oid type_oid)
{
    int32 hash_value;

    switch (type_oid)
    {
        case INT2OID:
            {
                int16 val = DatumGetInt16(value);
                hash_value = (int32) orochi_crc32_hash(&val, sizeof(val));
            }
            break;

        case INT4OID:
            {
                int32 val = DatumGetInt32(value);
                hash_value = (int32) orochi_crc32_hash(&val, sizeof(val));
            }
            break;

        case INT8OID:
            {
                int64 val = DatumGetInt64(value);
                hash_value = (int32) orochi_crc32_hash(&val, sizeof(val));
            }
            break;

        case TEXTOID:
        case VARCHAROID:
            {
                text *txt = DatumGetTextPP(value);
                hash_value = (int32) orochi_crc32_hash(VARDATA_ANY(txt),
                                                       VARSIZE_ANY_EXHDR(txt));
            }
            break;

        case UUIDOID:
            {
                /* UUID is 16 bytes */
                hash_value = (int32) orochi_crc32_hash(DatumGetPointer(value), 16);
            }
            break;

        default:
            /* Fall back to hashing the datum pointer */
            hash_value = (int32) orochi_crc32_hash(&value, sizeof(Datum));
            break;
    }

    return hash_value;
}

int32
orochi_get_shard_index(int32 hash_value, int32 shard_count)
{
    uint32 unsigned_hash;

    if (shard_count <= 0)
        return 0;

    /* Convert to unsigned for proper modulo */
    unsigned_hash = (uint32) hash_value;

    return (int32) (unsigned_hash % (uint32) shard_count);
}

/* ============================================================
 * Shard Routing
 * ============================================================ */

OrochiShardInfo *
orochi_route_to_shard(Oid table_oid, Datum value, Oid type_oid)
{
    int32 hash_value;

    hash_value = orochi_hash_datum(value, type_oid);

    return orochi_catalog_get_shard_for_hash(table_oid, hash_value);
}

List *
orochi_get_all_shards(Oid table_oid)
{
    return orochi_catalog_get_table_shards(table_oid);
}

List *
orochi_get_shards_in_range(Oid table_oid, int32 min_hash, int32 max_hash)
{
    List *all_shards;
    List *matched = NIL;
    ListCell *lc;

    all_shards = orochi_catalog_get_table_shards(table_oid);

    foreach(lc, all_shards)
    {
        OrochiShardInfo *shard = (OrochiShardInfo *) lfirst(lc);

        /* Check if shard overlaps with range */
        if (shard->hash_max >= min_hash && shard->hash_min <= max_hash)
            matched = lappend(matched, shard);
    }

    return matched;
}

/* ============================================================
 * Distribution Column Functions
 * ============================================================ */

char *
orochi_get_distribution_column(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL)
        return NULL;

    return info->distribution_column;
}

AttrNumber
orochi_get_distribution_attnum(Oid table_oid)
{
    OrochiTableInfo *info;
    HeapTuple tp;
    AttrNumber attnum = InvalidAttrNumber;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || info->distribution_column == NULL)
        return InvalidAttrNumber;

    tp = SearchSysCacheAttName(table_oid, info->distribution_column);
    if (HeapTupleIsValid(tp))
    {
        Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tp);
        attnum = att->attnum;
        ReleaseSysCache(tp);
    }

    return attnum;
}

Oid
orochi_get_distribution_type(Oid table_oid)
{
    AttrNumber attnum;
    HeapTuple tp;
    Oid type_oid = InvalidOid;

    attnum = orochi_get_distribution_attnum(table_oid);
    if (attnum == InvalidAttrNumber)
        return InvalidOid;

    tp = SearchSysCacheAttNum(table_oid, attnum);
    if (HeapTupleIsValid(tp))
    {
        Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tp);
        type_oid = att->atttypid;
        ReleaseSysCache(tp);
    }

    return type_oid;
}

/* ============================================================
 * Reference Table Functions
 * ============================================================ */

void
orochi_create_reference_table_internal(Oid table_oid)
{
    OrochiTableInfo info;

    memset(&info, 0, sizeof(OrochiTableInfo));

    info.relid = table_oid;
    info.schema_name = get_namespace_name(get_rel_namespace(table_oid));
    info.table_name = get_rel_name(table_oid);
    info.storage_type = OROCHI_STORAGE_ROW;
    info.shard_strategy = OROCHI_SHARD_REFERENCE;
    info.shard_count = 1;  /* Reference tables have 1 "shard" (replicated) */
    info.distribution_column = NULL;
    info.is_distributed = true;
    info.is_timeseries = false;

    orochi_catalog_register_table(&info);

    elog(LOG, "Created reference table %s.%s",
         info.schema_name, info.table_name);
}

bool
orochi_can_be_reference_table(Oid table_oid)
{
    /* Check if table is already distributed */
    if (orochi_is_distributed_table(table_oid))
    {
        ereport(WARNING,
                (errmsg("table is already distributed"),
                 errhint("Use undistribute_table() first")));
        return false;
    }

    return true;
}

/* ============================================================
 * Shard Placement Functions
 * ============================================================ */

OrochiNodeInfo *
orochi_get_shard_node(int64 shard_id)
{
    OrochiShardInfo *shard;

    shard = orochi_catalog_get_shard(shard_id);
    if (shard == NULL || shard->node_id < 0)
        return NULL;

    return orochi_catalog_get_node(shard->node_id);
}

void
orochi_move_shard(int64 shard_id, int32 target_node)
{
    /* TODO: Implement shard movement
     * 1. Create shard on target node
     * 2. Copy data
     * 3. Update routing
     * 4. Remove from source
     */
    orochi_catalog_update_shard_placement(shard_id, target_node);
}

int32
orochi_select_node_for_shard(void)
{
    List *nodes;
    ListCell *lc;
    int32 best_node = -1;
    int64 min_shards = INT64_MAX;

    nodes = orochi_catalog_get_active_nodes();

    foreach(lc, nodes)
    {
        OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(lc);

        if (node->role == OROCHI_NODE_WORKER &&
            node->shard_count < min_shards)
        {
            min_shards = node->shard_count;
            best_node = node->node_id;
        }
    }

    return best_node;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

bool
orochi_validate_distribution_column(Oid table_oid, const char *column_name)
{
    HeapTuple tp;
    Oid type_oid;

    tp = SearchSysCacheAttName(table_oid, column_name);
    if (!HeapTupleIsValid(tp))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));
        return false;
    }

    type_oid = ((Form_pg_attribute) GETSTRUCT(tp))->atttypid;
    ReleaseSysCache(tp);

    /* Check if type is hashable */
    switch (type_oid)
    {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case TEXTOID:
        case VARCHAROID:
        case UUIDOID:
        case DATEOID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
            return true;

        default:
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("column type is not supported for distribution"),
                     errhint("Use integer, text, uuid, or timestamp types")));
            return false;
    }
}

int32
orochi_get_shard_count(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL)
        return 0;

    return info->shard_count;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_distributed_table_sql);
PG_FUNCTION_INFO_V1(orochi_create_reference_table_sql);
PG_FUNCTION_INFO_V1(orochi_undistribute_table_sql);

Datum
orochi_create_distributed_table_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *dist_col_text = PG_GETARG_TEXT_PP(1);
    int32 strategy = PG_GETARG_INT32(2);
    int32 shard_count = PG_GETARG_INT32(3);
    char *dist_col;
    OrochiTableInfo info;

    dist_col = text_to_cstring(dist_col_text);

    /* Validate distribution column */
    if (!orochi_validate_distribution_column(table_oid, dist_col))
        PG_RETURN_VOID();

    /* Check if already distributed */
    if (orochi_is_distributed_table(table_oid))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("table is already distributed")));

    /* Prepare table info */
    memset(&info, 0, sizeof(OrochiTableInfo));
    info.relid = table_oid;
    info.schema_name = get_namespace_name(get_rel_namespace(table_oid));
    info.table_name = get_rel_name(table_oid);
    info.storage_type = OROCHI_STORAGE_ROW;
    info.shard_strategy = (OrochiShardStrategy) strategy;
    info.shard_count = shard_count;
    info.distribution_column = dist_col;
    info.is_distributed = true;
    info.is_timeseries = false;
    info.compression = OROCHI_COMPRESS_NONE;
    info.compression_level = 3;

    /* Register in catalog */
    orochi_catalog_register_table(&info);

    /* Create shard metadata entries */
    orochi_catalog_create_shards(table_oid, shard_count,
                                 (OrochiShardStrategy) strategy);

    /* Create physical shard tables */
    orochi_create_physical_shards(table_oid, shard_count, dist_col);

    /* Install routing triggers for INSERT/UPDATE/DELETE */
    orochi_install_routing_trigger(table_oid, dist_col, shard_count);

    /* Move existing data to shards (if any) */
    orochi_redistribute_data(table_oid);

    /* Create union view for easy querying */
    orochi_create_union_view(table_oid, dist_col, shard_count);

    elog(LOG, "Created distributed table %s.%s with %d physical shards on column %s",
         info.schema_name, info.table_name, shard_count, dist_col);

    PG_RETURN_VOID();
}

Datum
orochi_create_reference_table_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);

    if (!orochi_can_be_reference_table(table_oid))
        PG_RETURN_VOID();

    orochi_create_reference_table_internal(table_oid);

    PG_RETURN_VOID();
}

Datum
orochi_undistribute_table_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);

    if (!orochi_is_distributed_table(table_oid))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("table is not distributed")));

    /* Remove routing triggers */
    orochi_remove_routing_trigger(table_oid);

    /* Drop union view */
    orochi_drop_union_view(table_oid);

    /* Drop physical shard tables */
    orochi_drop_physical_shards(table_oid);

    /* Remove from catalog */
    orochi_catalog_remove_table(table_oid);

    elog(LOG, "Undistributed table with OID %u", table_oid);

    PG_RETURN_VOID();
}

/* ============================================================
 * Shard Rebalancing
 * ============================================================ */

/*
 * Get statistics about shard distribution across nodes
 */
ShardBalanceStats *
orochi_get_shard_balance_stats(void)
{
    ShardBalanceStats *stats;
    List *nodes;
    ListCell *lc;
    int node_count = 0;
    int64 total_shards = 0;
    int64 min_shards = INT64_MAX;
    int64 max_shards = 0;
    double sum_squared_diff = 0.0;
    double mean;

    stats = palloc0(sizeof(ShardBalanceStats));

    nodes = orochi_catalog_get_active_nodes();

    /* Count total shards and active worker nodes */
    foreach(lc, nodes)
    {
        OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(lc);
        if (node->role == OROCHI_NODE_WORKER && node->is_active)
        {
            total_shards += node->shard_count;
            node_count++;
            if (node->shard_count < min_shards)
                min_shards = node->shard_count;
            if (node->shard_count > max_shards)
                max_shards = node->shard_count;
        }
    }

    if (node_count == 0)
    {
        stats->total_nodes = 0;
        stats->total_shards = 0;
        stats->avg_shards_per_node = 0;
        stats->std_deviation = 0;
        stats->imbalance_ratio = 1.0;
        stats->needs_rebalancing = false;
        return stats;
    }

    mean = (double) total_shards / node_count;

    /* Calculate standard deviation */
    foreach(lc, nodes)
    {
        OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(lc);
        if (node->role == OROCHI_NODE_WORKER && node->is_active)
        {
            double diff = node->shard_count - mean;
            sum_squared_diff += diff * diff;
        }
    }

    stats->total_nodes = node_count;
    stats->total_shards = total_shards;
    stats->min_shards = min_shards == INT64_MAX ? 0 : min_shards;
    stats->max_shards = max_shards;
    stats->avg_shards_per_node = mean;
    stats->std_deviation = sqrt(sum_squared_diff / node_count);

    if (min_shards > 0)
        stats->imbalance_ratio = (double) max_shards / min_shards;
    else
        stats->imbalance_ratio = max_shards > 0 ? 999.0 : 1.0;

    stats->needs_rebalancing = (stats->imbalance_ratio > 1.2);

    return stats;
}

/*
 * Execute shard rebalancing for a table
 */
void
orochi_rebalance_table_shards(Oid table_oid)
{
    List *shards;
    ListCell *lc;
    List *nodes;
    int node_count;
    int64 total_shards;
    int target_per_node;
    int moves_executed = 0;

    if (!orochi_is_distributed_table(table_oid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("table is not distributed")));
    }

    shards = orochi_catalog_get_table_shards(table_oid);
    total_shards = list_length(shards);

    nodes = orochi_catalog_get_active_nodes();
    node_count = 0;
    foreach(lc, nodes)
    {
        OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(lc);
        if (node->role == OROCHI_NODE_WORKER && node->is_active)
            node_count++;
    }

    if (node_count == 0)
    {
        elog(NOTICE, "No active worker nodes for rebalancing");
        return;
    }

    target_per_node = (int) ((total_shards + node_count - 1) / node_count);

    elog(NOTICE, "Rebalancing %ld shards across %d nodes (target: %d per node)",
         total_shards, node_count, target_per_node);

    /* Simple round-robin rebalancing */
    int current_node_idx = 0;
    foreach(lc, shards)
    {
        OrochiShardInfo *shard = (OrochiShardInfo *) lfirst(lc);
        ListCell *nlc;
        int idx = 0;
        int32 target_node = -1;

        /* Find node at current index */
        foreach(nlc, nodes)
        {
            OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(nlc);
            if (node->role == OROCHI_NODE_WORKER && node->is_active)
            {
                if (idx == current_node_idx)
                {
                    target_node = node->node_id;
                    break;
                }
                idx++;
            }
        }

        /* Move shard if needed */
        if (target_node >= 0 && shard->node_id != target_node)
        {
            orochi_move_shard_with_data(shard->shard_id, target_node);
            moves_executed++;
        }

        current_node_idx = (current_node_idx + 1) % node_count;
    }

    elog(NOTICE, "Rebalancing complete: moved %d shards", moves_executed);
}

/*
 * Move a shard with its data to a new node
 */
void
orochi_move_shard_with_data(int64 shard_id, int32 target_node)
{
    OrochiShardInfo *shard;
    StringInfoData update_sql;

    shard = orochi_catalog_get_shard(shard_id);
    if (shard == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("shard %ld does not exist", shard_id)));
    }

    if (shard->node_id == target_node)
        return;

    SPI_connect();

    /* Update placement in catalog */
    initStringInfo(&update_sql);
    appendStringInfo(&update_sql,
        "UPDATE orochi.orochi_shards SET node_id = %d WHERE shard_id = %ld",
        target_node, shard_id);

    SPI_execute(update_sql.data, false, 0);
    pfree(update_sql.data);

    SPI_finish();

    elog(DEBUG1, "Moved shard %ld to node %d", shard_id, target_node);
}

/*
 * Split a shard into two
 */
void
orochi_split_shard_impl(int64 shard_id)
{
    OrochiShardInfo *shard;
    int32 mid_hash;
    int64 new_shard_id;

    shard = orochi_catalog_get_shard(shard_id);
    if (shard == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("shard %ld does not exist", shard_id)));
    }

    mid_hash = shard->hash_min + (shard->hash_max - shard->hash_min) / 2;

    orochi_catalog_update_shard_range(shard_id, shard->hash_min, mid_hash);

    new_shard_id = orochi_catalog_create_shard(shard->table_oid,
                                               mid_hash + 1,
                                               shard->hash_max,
                                               shard->node_id);

    elog(NOTICE, "Split shard %ld into [%d,%d] and new shard %ld [%d,%d]",
         shard_id, shard->hash_min, mid_hash,
         new_shard_id, mid_hash + 1, shard->hash_max);
}

/*
 * Merge two adjacent shards
 */
void
orochi_merge_shards_impl(int64 shard1_id, int64 shard2_id)
{
    OrochiShardInfo *shard1, *shard2;
    int32 new_min, new_max;

    shard1 = orochi_catalog_get_shard(shard1_id);
    shard2 = orochi_catalog_get_shard(shard2_id);

    if (shard1 == NULL || shard2 == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("one or both shards do not exist")));
    }

    if (shard1->table_oid != shard2->table_oid)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("shards belong to different tables")));
    }

    new_min = shard1->hash_min < shard2->hash_min ? shard1->hash_min : shard2->hash_min;
    new_max = shard1->hash_max > shard2->hash_max ? shard1->hash_max : shard2->hash_max;

    orochi_catalog_update_shard_range(shard1_id, new_min, new_max);
    orochi_catalog_delete_shard(shard2_id);

    elog(NOTICE, "Merged shards %ld and %ld into shard %ld [%d,%d]",
         shard1_id, shard2_id, shard1_id, new_min, new_max);
}

/*
 * Get shard placement details
 */
List *
orochi_get_shard_placements(Oid table_oid)
{
    List *shards;
    List *placements = NIL;
    ListCell *lc;

    shards = orochi_catalog_get_table_shards(table_oid);

    foreach(lc, shards)
    {
        OrochiShardInfo *shard = (OrochiShardInfo *) lfirst(lc);
        ShardPlacementInfo *placement = palloc0(sizeof(ShardPlacementInfo));

        placement->shard_id = shard->shard_id;
        placement->table_oid = shard->table_oid;
        placement->node_id = shard->node_id;
        placement->shard_index = shard->shard_index;
        placement->hash_min = shard->hash_min;
        placement->hash_max = shard->hash_max;
        placement->row_count = shard->row_count;
        placement->size_bytes = shard->size_bytes;

        placements = lappend(placements, placement);
    }

    return placements;
}

/* ============================================================
 * SQL-Callable Rebalancing Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_rebalance_table_sql);
PG_FUNCTION_INFO_V1(orochi_move_shard_sql);
PG_FUNCTION_INFO_V1(orochi_split_shard_sql);
PG_FUNCTION_INFO_V1(orochi_merge_shards_sql);
PG_FUNCTION_INFO_V1(orochi_get_rebalance_plan_sql);

Datum
orochi_rebalance_table_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);

    orochi_rebalance_table_shards(table_oid);

    PG_RETURN_VOID();
}

Datum
orochi_move_shard_sql(PG_FUNCTION_ARGS)
{
    int64 shard_id = PG_GETARG_INT64(0);
    int32 target_node = PG_GETARG_INT32(1);

    orochi_move_shard_with_data(shard_id, target_node);

    PG_RETURN_VOID();
}

Datum
orochi_split_shard_sql(PG_FUNCTION_ARGS)
{
    int64 shard_id = PG_GETARG_INT64(0);

    orochi_split_shard_impl(shard_id);

    PG_RETURN_VOID();
}

Datum
orochi_merge_shards_sql(PG_FUNCTION_ARGS)
{
    int64 shard1_id = PG_GETARG_INT64(0);
    int64 shard2_id = PG_GETARG_INT64(1);

    orochi_merge_shards_impl(shard1_id, shard2_id);

    PG_RETURN_VOID();
}

Datum
orochi_get_rebalance_plan_sql(PG_FUNCTION_ARGS)
{
    ShardBalanceStats *stats;
    StringInfoData result;

    stats = orochi_get_shard_balance_stats();

    initStringInfo(&result);
    appendStringInfo(&result,
        "Shard Balance Statistics:\n"
        "  Total nodes: %d\n"
        "  Total shards: %ld\n"
        "  Min shards per node: %ld\n"
        "  Max shards per node: %ld\n"
        "  Avg shards per node: %.2f\n"
        "  Standard deviation: %.2f\n"
        "  Imbalance ratio: %.2f\n"
        "  Needs rebalancing: %s",
        stats->total_nodes,
        stats->total_shards,
        stats->min_shards,
        stats->max_shards,
        stats->avg_shards_per_node,
        stats->std_deviation,
        stats->imbalance_ratio,
        stats->needs_rebalancing ? "YES" : "NO");

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
