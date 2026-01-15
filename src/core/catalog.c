/*-------------------------------------------------------------------------
 *
 * catalog.c
 *    Orochi DB catalog implementation - metadata storage and operations
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "../orochi.h"
#include "catalog.h"

/* Cache for schema OID */
static Oid OrochiSchemaOid = InvalidOid;

/* Forward declarations */
static void ensure_catalog_initialized(void);
static Datum datum_from_cstring(const char *str);

/*
 * orochi_catalog_init
 *    Create the Orochi schema and all metadata tables
 */
void
orochi_catalog_init(void)
{
    const char *create_schema_sql =
        "CREATE SCHEMA IF NOT EXISTS orochi";

    const char *create_tables_sql =
        /* Main table registry */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_tables ("
        "    table_oid OID PRIMARY KEY,"
        "    schema_name TEXT NOT NULL,"
        "    table_name TEXT NOT NULL,"
        "    storage_type INTEGER NOT NULL DEFAULT 0,"
        "    shard_strategy INTEGER NOT NULL DEFAULT 0,"
        "    shard_count INTEGER NOT NULL DEFAULT 0,"
        "    distribution_column TEXT,"
        "    is_distributed BOOLEAN NOT NULL DEFAULT FALSE,"
        "    is_timeseries BOOLEAN NOT NULL DEFAULT FALSE,"
        "    time_column TEXT,"
        "    chunk_interval INTERVAL,"
        "    compression INTEGER NOT NULL DEFAULT 0,"
        "    compression_level INTEGER NOT NULL DEFAULT 3,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ");"

        /* Shard metadata */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_shards ("
        "    shard_id BIGSERIAL PRIMARY KEY,"
        "    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    shard_index INTEGER NOT NULL,"
        "    hash_min INTEGER NOT NULL,"
        "    hash_max INTEGER NOT NULL,"
        "    node_id INTEGER,"
        "    row_count BIGINT NOT NULL DEFAULT 0,"
        "    size_bytes BIGINT NOT NULL DEFAULT 0,"
        "    storage_tier INTEGER NOT NULL DEFAULT 0,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    last_accessed TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    UNIQUE(table_oid, shard_index)"
        ");"

        /* Shard placements (for replication) */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_shard_placements ("
        "    placement_id BIGSERIAL PRIMARY KEY,"
        "    shard_id BIGINT NOT NULL REFERENCES orochi.orochi_shards(shard_id) ON DELETE CASCADE,"
        "    node_id INTEGER NOT NULL,"
        "    is_primary BOOLEAN NOT NULL DEFAULT TRUE,"
        "    state INTEGER NOT NULL DEFAULT 0,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ");"

        /* Time-series chunks */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_chunks ("
        "    chunk_id BIGSERIAL PRIMARY KEY,"
        "    hypertable_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    dimension_id INTEGER NOT NULL DEFAULT 0,"
        "    chunk_table_oid OID,"
        "    range_start TIMESTAMPTZ NOT NULL,"
        "    range_end TIMESTAMPTZ NOT NULL,"
        "    row_count BIGINT NOT NULL DEFAULT 0,"
        "    size_bytes BIGINT NOT NULL DEFAULT 0,"
        "    is_compressed BOOLEAN NOT NULL DEFAULT FALSE,"
        "    storage_tier INTEGER NOT NULL DEFAULT 0,"
        "    tablespace_name TEXT,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    UNIQUE(hypertable_oid, range_start)"
        ");"

        /* Cluster nodes */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_nodes ("
        "    node_id SERIAL PRIMARY KEY,"
        "    hostname TEXT NOT NULL,"
        "    port INTEGER NOT NULL DEFAULT 5432,"
        "    role INTEGER NOT NULL DEFAULT 1,"
        "    is_active BOOLEAN NOT NULL DEFAULT TRUE,"
        "    shard_count BIGINT NOT NULL DEFAULT 0,"
        "    total_size BIGINT NOT NULL DEFAULT 0,"
        "    cpu_usage DOUBLE PRECISION NOT NULL DEFAULT 0.0,"
        "    memory_usage DOUBLE PRECISION NOT NULL DEFAULT 0.0,"
        "    last_heartbeat TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    UNIQUE(hostname, port)"
        ");"

        /* Columnar stripes */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_stripes ("
        "    stripe_id BIGSERIAL PRIMARY KEY,"
        "    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    first_row BIGINT NOT NULL,"
        "    row_count BIGINT NOT NULL,"
        "    column_count INTEGER NOT NULL,"
        "    data_size BIGINT NOT NULL DEFAULT 0,"
        "    metadata_size BIGINT NOT NULL DEFAULT 0,"
        "    compression INTEGER NOT NULL DEFAULT 0,"
        "    is_flushed BOOLEAN NOT NULL DEFAULT FALSE,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ");"

        /* Column chunks within stripes */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_column_chunks ("
        "    chunk_id BIGSERIAL PRIMARY KEY,"
        "    stripe_id BIGINT NOT NULL REFERENCES orochi.orochi_stripes(stripe_id) ON DELETE CASCADE,"
        "    column_index SMALLINT NOT NULL,"
        "    value_count BIGINT NOT NULL,"
        "    null_count BIGINT NOT NULL DEFAULT 0,"
        "    compressed_size BIGINT NOT NULL,"
        "    decompressed_size BIGINT NOT NULL,"
        "    compression INTEGER NOT NULL DEFAULT 0,"
        "    min_value BYTEA,"
        "    max_value BYTEA,"
        "    has_nulls BOOLEAN NOT NULL DEFAULT FALSE,"
        "    UNIQUE(stripe_id, column_index)"
        ");"

        /* Tiering policies */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_tiering_policies ("
        "    policy_id BIGSERIAL PRIMARY KEY,"
        "    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    hot_to_warm INTERVAL,"
        "    warm_to_cold INTERVAL,"
        "    cold_to_frozen INTERVAL,"
        "    compress_on_tier BOOLEAN NOT NULL DEFAULT TRUE,"
        "    enabled BOOLEAN NOT NULL DEFAULT TRUE,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    UNIQUE(table_oid)"
        ");"

        /* Vector indexes */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_vector_indexes ("
        "    index_oid OID PRIMARY KEY,"
        "    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    vector_column SMALLINT NOT NULL,"
        "    dimensions INTEGER NOT NULL,"
        "    lists INTEGER NOT NULL DEFAULT 100,"
        "    probes INTEGER NOT NULL DEFAULT 10,"
        "    distance_type TEXT NOT NULL DEFAULT 'l2',"
        "    index_type TEXT NOT NULL DEFAULT 'ivfflat',"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ");"

        /* Continuous aggregates */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_continuous_aggregates ("
        "    agg_id BIGSERIAL PRIMARY KEY,"
        "    view_oid OID NOT NULL,"
        "    source_table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,"
        "    materialization_table_oid OID,"
        "    query_text TEXT NOT NULL,"
        "    refresh_interval INTERVAL,"
        "    last_refresh TIMESTAMPTZ,"
        "    enabled BOOLEAN NOT NULL DEFAULT TRUE,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    UNIQUE(view_oid)"
        ");"

        /* Invalidation log for continuous aggregates */
        "CREATE TABLE IF NOT EXISTS orochi.orochi_invalidation_log ("
        "    log_id BIGSERIAL PRIMARY KEY,"
        "    agg_id BIGINT NOT NULL REFERENCES orochi.orochi_continuous_aggregates(agg_id) ON DELETE CASCADE,"
        "    range_start TIMESTAMPTZ NOT NULL,"
        "    range_end TIMESTAMPTZ NOT NULL,"
        "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ");"

        /* Create indexes for performance */
        "CREATE INDEX IF NOT EXISTS idx_shards_table ON orochi.orochi_shards(table_oid);"
        "CREATE INDEX IF NOT EXISTS idx_shards_node ON orochi.orochi_shards(node_id);"
        "CREATE INDEX IF NOT EXISTS idx_chunks_hypertable ON orochi.orochi_chunks(hypertable_oid);"
        "CREATE INDEX IF NOT EXISTS idx_chunks_range ON orochi.orochi_chunks(range_start, range_end);"
        "CREATE INDEX IF NOT EXISTS idx_stripes_table ON orochi.orochi_stripes(table_oid);"
        "CREATE INDEX IF NOT EXISTS idx_column_chunks_stripe ON orochi.orochi_column_chunks(stripe_id);"
        "CREATE INDEX IF NOT EXISTS idx_invalidation_agg ON orochi.orochi_invalidation_log(agg_id);";

    int ret;

    /* Connect to SPI */
    SPI_connect();

    /* Create schema */
    ret = SPI_execute(create_schema_sql, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(ERROR, "Failed to create orochi schema");

    /* Create tables */
    ret = SPI_execute(create_tables_sql, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(ERROR, "Failed to create orochi catalog tables");

    SPI_finish();

    /* Cache schema OID */
    OrochiSchemaOid = get_namespace_oid(OROCHI_SCHEMA_NAME, false);

    elog(LOG, "Orochi catalog initialized");
}

/*
 * orochi_catalog_exists
 *    Check if the catalog schema exists
 */
bool
orochi_catalog_exists(void)
{
    Oid schema_oid = get_namespace_oid(OROCHI_SCHEMA_NAME, true);
    return OidIsValid(schema_oid);
}

/*
 * orochi_get_schema_oid
 *    Get the Orochi schema OID
 */
Oid
orochi_get_schema_oid(void)
{
    if (!OidIsValid(OrochiSchemaOid))
        OrochiSchemaOid = get_namespace_oid(OROCHI_SCHEMA_NAME, false);
    return OrochiSchemaOid;
}

/*
 * ensure_catalog_initialized
 *    Make sure catalog exists before operations
 */
static void
ensure_catalog_initialized(void)
{
    if (!orochi_catalog_exists())
        orochi_catalog_init();
}

/* ============================================================
 * Table Metadata Operations
 * ============================================================ */

void
orochi_catalog_register_table(OrochiTableInfo *table_info)
{
    StringInfoData query;
    int ret;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_tables "
        "(table_oid, schema_name, table_name, storage_type, shard_strategy, "
        "shard_count, distribution_column, is_distributed, is_timeseries, "
        "time_column, chunk_interval, compression, compression_level) "
        "VALUES (%u, %s, %s, %d, %d, %d, %s, %s, %s, %s, %s, %d, %d) "
        "ON CONFLICT (table_oid) DO UPDATE SET "
        "storage_type = EXCLUDED.storage_type, "
        "shard_strategy = EXCLUDED.shard_strategy, "
        "shard_count = EXCLUDED.shard_count, "
        "distribution_column = EXCLUDED.distribution_column, "
        "is_distributed = EXCLUDED.is_distributed, "
        "is_timeseries = EXCLUDED.is_timeseries, "
        "time_column = EXCLUDED.time_column, "
        "chunk_interval = EXCLUDED.chunk_interval, "
        "compression = EXCLUDED.compression, "
        "compression_level = EXCLUDED.compression_level, "
        "updated_at = NOW()",
        table_info->relid,
        table_info->schema_name ? quote_literal_cstr(table_info->schema_name) : "NULL",
        table_info->table_name ? quote_literal_cstr(table_info->table_name) : "NULL",
        (int) table_info->storage_type,
        (int) table_info->shard_strategy,
        table_info->shard_count,
        table_info->distribution_column ? quote_literal_cstr(table_info->distribution_column) : "NULL",
        table_info->is_distributed ? "TRUE" : "FALSE",
        table_info->is_timeseries ? "TRUE" : "FALSE",
        table_info->time_column ? quote_literal_cstr(table_info->time_column) : "NULL",
        table_info->chunk_interval ? "INTERVAL '1 day'" : "NULL",  /* TODO: proper interval */
        (int) table_info->compression,
        table_info->compression_level);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
        elog(ERROR, "Failed to register table in Orochi catalog");
    SPI_finish();

    pfree(query.data);
}

OrochiTableInfo *
orochi_catalog_get_table(Oid relid)
{
    StringInfoData query;
    OrochiTableInfo *info = NULL;
    int ret;

    if (!orochi_catalog_exists())
        return NULL;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT table_oid, schema_name, table_name, storage_type, shard_strategy, "
        "shard_count, distribution_column, is_distributed, is_timeseries, "
        "time_column, compression, compression_level "
        "FROM orochi.orochi_tables WHERE table_oid = %u",
        relid);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        info = (OrochiTableInfo *) palloc0(sizeof(OrochiTableInfo));

        info->relid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        info->schema_name = SPI_getvalue(tuple, tupdesc, 2);
        info->table_name = SPI_getvalue(tuple, tupdesc, 3);
        info->storage_type = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        info->shard_strategy = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        info->shard_count = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        info->distribution_column = SPI_getvalue(tuple, tupdesc, 7);
        info->is_distributed = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        info->is_timeseries = DatumGetBool(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        info->time_column = SPI_getvalue(tuple, tupdesc, 10);
        info->compression = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 11, &isnull));
        info->compression_level = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 12, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return info;
}

bool
orochi_catalog_is_orochi_table(Oid relid)
{
    return orochi_catalog_get_table(relid) != NULL;
}

void
orochi_catalog_remove_table(Oid relid)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_tables WHERE table_oid = %u", relid);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(WARNING, "Failed to remove table from Orochi catalog");
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Shard Operations
 * ============================================================ */

void
orochi_catalog_create_shards(Oid table_oid, int shard_count, OrochiShardStrategy strategy)
{
    StringInfoData query;
    int i;
    int hash_range = INT32_MAX / shard_count;
    int ret;

    ensure_catalog_initialized();

    SPI_connect();

    for (i = 0; i < shard_count; i++)
    {
        int32 hash_min = (i == 0) ? INT32_MIN : (INT32_MIN + (i * hash_range));
        int32 hash_max = (i == shard_count - 1) ? INT32_MAX : (INT32_MIN + ((i + 1) * hash_range) - 1);

        initStringInfo(&query);
        appendStringInfo(&query,
            "INSERT INTO orochi.orochi_shards "
            "(table_oid, shard_index, hash_min, hash_max, storage_tier) "
            "VALUES (%u, %d, %d, %d, 0)",
            table_oid, i, hash_min, hash_max);

        ret = SPI_execute(query.data, false, 0);
        if (ret != SPI_OK_INSERT)
            elog(ERROR, "Failed to create shard %d for table %u", i, table_oid);

        pfree(query.data);
    }

    SPI_finish();
}

OrochiShardInfo *
orochi_catalog_get_shard(int64 shard_id)
{
    StringInfoData query;
    OrochiShardInfo *info = NULL;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT shard_id, table_oid, shard_index, hash_min, hash_max, "
        "node_id, row_count, size_bytes, storage_tier, created_at, last_accessed "
        "FROM orochi.orochi_shards WHERE shard_id = %ld",
        shard_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        info = (OrochiShardInfo *) palloc0(sizeof(OrochiShardInfo));

        info->shard_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        info->table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        info->shard_index = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        info->hash_min = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        info->hash_max = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        info->node_id = isnull ? -1 : DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        info->row_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        info->size_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        info->storage_tier = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        info->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 10, &isnull));
        info->last_accessed = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 11, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return info;
}

List *
orochi_catalog_get_table_shards(Oid table_oid)
{
    StringInfoData query;
    List *shards = NIL;
    int ret;
    uint64 i;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT shard_id, table_oid, shard_index, hash_min, hash_max, "
        "node_id, row_count, size_bytes, storage_tier, created_at, last_accessed "
        "FROM orochi.orochi_shards WHERE table_oid = %u ORDER BY shard_index",
        table_oid);

    SPI_connect();
    ret = SPI_execute(query.data, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiShardInfo *info;

            info = (OrochiShardInfo *) palloc0(sizeof(OrochiShardInfo));

            info->shard_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            info->table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
            info->shard_index = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
            info->hash_min = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
            info->hash_max = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
            info->node_id = isnull ? -1 : DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
            info->row_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
            info->size_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 8, &isnull));
            info->storage_tier = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
            info->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 10, &isnull));
            info->last_accessed = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 11, &isnull));

            shards = lappend(shards, info);
        }
    }

    SPI_finish();
    pfree(query.data);

    return shards;
}

OrochiShardInfo *
orochi_catalog_get_shard_for_hash(Oid table_oid, int32 hash_value)
{
    StringInfoData query;
    OrochiShardInfo *info = NULL;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT shard_id, table_oid, shard_index, hash_min, hash_max, "
        "node_id, row_count, size_bytes, storage_tier, created_at, last_accessed "
        "FROM orochi.orochi_shards "
        "WHERE table_oid = %u AND hash_min <= %d AND hash_max >= %d",
        table_oid, hash_value, hash_value);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        info = (OrochiShardInfo *) palloc0(sizeof(OrochiShardInfo));

        info->shard_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        info->table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        info->shard_index = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        info->hash_min = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        info->hash_max = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        info->node_id = isnull ? -1 : DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        info->row_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        info->size_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        info->storage_tier = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return info;
}

/* ============================================================
 * Node Operations
 * ============================================================ */

int32
orochi_catalog_add_node(const char *hostname, int port, OrochiNodeRole role)
{
    StringInfoData query;
    int ret;
    int32 node_id = -1;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_nodes (hostname, port, role) "
        "VALUES (%s, %d, %d) "
        "ON CONFLICT (hostname, port) DO UPDATE SET role = EXCLUDED.role, is_active = TRUE "
        "RETURNING node_id",
        quote_literal_cstr(hostname), port, (int) role);

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        node_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return node_id;
}

List *
orochi_catalog_get_active_nodes(void)
{
    const char *query =
        "SELECT node_id, hostname, port, role, is_active, "
        "shard_count, total_size, cpu_usage, memory_usage, last_heartbeat "
        "FROM orochi.orochi_nodes WHERE is_active = TRUE ORDER BY node_id";
    List *nodes = NIL;
    int ret;
    uint64 i;

    SPI_connect();
    ret = SPI_execute(query, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiNodeInfo *info;

            info = (OrochiNodeInfo *) palloc0(sizeof(OrochiNodeInfo));

            info->node_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            info->hostname = SPI_getvalue(tuple, tupdesc, 2);
            info->port = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
            info->role = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
            info->is_active = DatumGetBool(SPI_getbinval(tuple, tupdesc, 5, &isnull));
            info->shard_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 6, &isnull));
            info->total_size = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
            info->cpu_usage = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 8, &isnull));
            info->memory_usage = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 9, &isnull));
            info->last_heartbeat = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 10, &isnull));

            nodes = lappend(nodes, info);
        }
    }

    SPI_finish();

    return nodes;
}

void
orochi_catalog_record_heartbeat(int32 node_id)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_nodes SET last_heartbeat = NOW() WHERE node_id = %d",
        node_id);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

void
orochi_catalog_update_node_status(int32 node_id, bool is_active)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_nodes SET is_active = %s WHERE node_id = %d",
        is_active ? "TRUE" : "FALSE", node_id);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Chunk Operations
 * ============================================================ */

int64
orochi_catalog_create_chunk(Oid hypertable_oid, TimestampTz range_start, TimestampTz range_end)
{
    StringInfoData query;
    int ret;
    int64 chunk_id = -1;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_chunks "
        "(hypertable_oid, range_start, range_end) "
        "VALUES (%u, '%s'::timestamptz, '%s'::timestamptz) "
        "ON CONFLICT (hypertable_oid, range_start) DO NOTHING "
        "RETURNING chunk_id",
        hypertable_oid,
        timestamptz_to_str(range_start),
        timestamptz_to_str(range_end));

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        chunk_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return chunk_id;
}

List *
orochi_catalog_get_chunks_in_range(Oid hypertable_oid, TimestampTz start, TimestampTz end)
{
    StringInfoData query;
    List *chunks = NIL;
    int ret;
    uint64 i;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT chunk_id, hypertable_oid, dimension_id, range_start, range_end, "
        "row_count, size_bytes, is_compressed, storage_tier, tablespace_name "
        "FROM orochi.orochi_chunks "
        "WHERE hypertable_oid = %u "
        "AND range_start < '%s'::timestamptz "
        "AND range_end > '%s'::timestamptz "
        "ORDER BY range_start",
        hypertable_oid,
        timestamptz_to_str(end),
        timestamptz_to_str(start));

    SPI_connect();
    ret = SPI_execute(query.data, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiChunkInfo *info;

            info = (OrochiChunkInfo *) palloc0(sizeof(OrochiChunkInfo));

            info->chunk_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            info->hypertable_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
            info->dimension_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
            info->range_start = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 4, &isnull));
            info->range_end = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 5, &isnull));
            info->row_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 6, &isnull));
            info->size_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
            info->is_compressed = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));
            info->storage_tier = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
            info->tablespace = SPI_getvalue(tuple, tupdesc, 10);

            chunks = lappend(chunks, info);
        }
    }

    SPI_finish();
    pfree(query.data);

    return chunks;
}

void
orochi_catalog_update_chunk_tier(int64 chunk_id, OrochiStorageTier tier)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_chunks SET storage_tier = %d WHERE chunk_id = %ld",
        (int) tier, chunk_id);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Stripe Operations
 * ============================================================ */

int64
orochi_catalog_create_stripe(Oid table_oid, int64 first_row, int64 row_count, int32 column_count)
{
    StringInfoData query;
    int ret;
    int64 stripe_id = -1;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_stripes "
        "(table_oid, first_row, row_count, column_count) "
        "VALUES (%u, %ld, %ld, %d) "
        "RETURNING stripe_id",
        table_oid, first_row, row_count, column_count);

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        stripe_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return stripe_id;
}

void
orochi_catalog_update_stripe_status(int64 stripe_id, bool is_flushed, int64 data_size, int64 metadata_size)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_stripes "
        "SET is_flushed = %s, data_size = %ld, metadata_size = %ld "
        "WHERE stripe_id = %ld",
        is_flushed ? "TRUE" : "FALSE", data_size, metadata_size, stripe_id);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Tiering Policy Operations
 * ============================================================ */

int64
orochi_catalog_create_tiering_policy(OrochiTieringPolicy *policy)
{
    StringInfoData query;
    int ret;
    int64 policy_id = -1;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_tiering_policies "
        "(table_oid, hot_to_warm, warm_to_cold, cold_to_frozen, compress_on_tier, enabled) "
        "VALUES (%u, %s, %s, %s, %s, %s) "
        "ON CONFLICT (table_oid) DO UPDATE SET "
        "hot_to_warm = EXCLUDED.hot_to_warm, "
        "warm_to_cold = EXCLUDED.warm_to_cold, "
        "cold_to_frozen = EXCLUDED.cold_to_frozen, "
        "compress_on_tier = EXCLUDED.compress_on_tier, "
        "enabled = EXCLUDED.enabled "
        "RETURNING policy_id",
        policy->table_oid,
        policy->hot_to_warm ? "INTERVAL '1 day'" : "NULL",  /* TODO: proper intervals */
        policy->warm_to_cold ? "INTERVAL '7 days'" : "NULL",
        policy->cold_to_frozen ? "INTERVAL '30 days'" : "NULL",
        policy->compress_on_tier ? "TRUE" : "FALSE",
        policy->enabled ? "TRUE" : "FALSE");

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if ((ret == SPI_OK_INSERT_RETURNING || ret == SPI_OK_UPDATE_RETURNING) && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        policy_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return policy_id;
}

/* ============================================================
 * Vector Index Operations
 * ============================================================ */

void
orochi_catalog_register_vector_index(OrochiVectorIndex *index_info)
{
    StringInfoData query;
    int ret;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_vector_indexes "
        "(index_oid, table_oid, vector_column, dimensions, lists, probes, distance_type, index_type) "
        "VALUES (%u, %u, %d, %d, %d, %d, %s, %s) "
        "ON CONFLICT (index_oid) DO UPDATE SET "
        "lists = EXCLUDED.lists, "
        "probes = EXCLUDED.probes",
        index_info->index_oid,
        index_info->table_oid,
        index_info->vector_column,
        index_info->dimensions,
        index_info->lists,
        index_info->probes,
        quote_literal_cstr(index_info->distance_type),
        quote_literal_cstr(index_info->index_type));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Additional Catalog Operations
 * ============================================================ */

void
orochi_catalog_update_chunk_compression(int64 chunk_id, bool is_compressed)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_chunks SET is_compressed = %s WHERE chunk_id = %ld",
        is_compressed ? "TRUE" : "FALSE", chunk_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_delete_chunk(int64 chunk_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_chunks WHERE chunk_id = %ld", chunk_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_update_shard_placement(int64 shard_id, int32 node_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_shards SET node_id = %d WHERE shard_id = %ld",
        node_id, shard_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_update_shard_stats(int64 shard_id, int64 row_count, int64 size_bytes)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_shards SET row_count = %ld, size_bytes = %ld WHERE shard_id = %ld",
        row_count, size_bytes, shard_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_record_shard_access(int64 shard_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_shards SET last_accessed = NOW() WHERE shard_id = %ld",
        shard_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

int64
orochi_catalog_create_shard(Oid table_oid, int32 hash_min, int32 hash_max, int32 node_id)
{
    StringInfoData query;
    int ret;
    int64 shard_id = -1;

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_shards "
        "(table_oid, hash_min, hash_max, node_id, storage_tier) "
        "VALUES (%u, %d, %d, %d, 0) "
        "RETURNING shard_id",
        table_oid, hash_min, hash_max, node_id);

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        shard_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                               SPI_tuptable->tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return shard_id;
}

void
orochi_catalog_update_shard_range(int64 shard_id, int32 hash_min, int32 hash_max)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_shards SET hash_min = %d, hash_max = %d "
        "WHERE shard_id = %ld",
        hash_min, hash_max, shard_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_delete_shard(int64 shard_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_shards WHERE shard_id = %ld", shard_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_update_node_stats(int32 node_id, int64 shard_count, int64 total_size,
                                 double cpu_usage, double memory_usage)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_nodes SET shard_count = %ld, total_size = %ld, "
        "cpu_usage = %f, memory_usage = %f, last_heartbeat = NOW() WHERE node_id = %d",
        shard_count, total_size, cpu_usage, memory_usage, node_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_remove_node(int32 node_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_nodes WHERE node_id = %d", node_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_update_table(OrochiTableInfo *table_info)
{
    orochi_catalog_register_table(table_info);
}

void
orochi_catalog_log_invalidation(Oid table_oid, TimestampTz start, TimestampTz end)
{
    StringInfoData query;
    int ret;

    ensure_catalog_initialized();

    /*
     * Log invalidation for all continuous aggregates that depend on this table.
     * This is called when data in a hypertable changes.
     */
    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_invalidation_log (agg_id, range_start, range_end) "
        "SELECT agg_id, '%s'::timestamptz, '%s'::timestamptz "
        "FROM orochi.orochi_continuous_aggregates "
        "WHERE source_table_oid = %u AND enabled = TRUE",
        timestamptz_to_str(start),
        timestamptz_to_str(end),
        table_oid);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    elog(DEBUG1, "Logged invalidation for table %u from %s to %s",
         table_oid, timestamptz_to_str(start), timestamptz_to_str(end));
}

void
orochi_catalog_clear_invalidations(Oid agg_oid, TimestampTz up_to)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_invalidation_log "
        "WHERE agg_id = %u AND range_end <= '%s'::timestamptz",
        agg_oid, timestamptz_to_str(up_to));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

List *
orochi_catalog_get_pending_invalidations(Oid agg_oid)
{
    StringInfoData query;
    List *invalidations = NIL;
    int ret;
    uint64 i;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT log_id, agg_id, range_start, range_end "
        "FROM orochi.orochi_invalidation_log "
        "WHERE agg_id = %u "
        "ORDER BY range_start",
        agg_oid);

    SPI_connect();
    ret = SPI_execute(query.data, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiInvalidationEntry *entry;

            entry = (OrochiInvalidationEntry *) palloc0(sizeof(OrochiInvalidationEntry));

            entry->log_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            entry->agg_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 2, &isnull));
            entry->range_start = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 3, &isnull));
            entry->range_end = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 4, &isnull));

            invalidations = lappend(invalidations, entry);
        }
    }

    SPI_finish();
    pfree(query.data);

    return invalidations;
}

List *
orochi_catalog_get_continuous_aggs(Oid source_table)
{
    StringInfoData query;
    List *aggs = NIL;
    int ret;
    uint64 i;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT agg_id, view_oid, source_table_oid, materialization_table_oid, "
        "query_text, refresh_interval, last_refresh, enabled "
        "FROM orochi.orochi_continuous_aggregates "
        "WHERE source_table_oid = %u",
        source_table);

    SPI_connect();
    ret = SPI_execute(query.data, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiContinuousAggInfo *info;

            info = (OrochiContinuousAggInfo *) palloc0(sizeof(OrochiContinuousAggInfo));

            info->agg_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            info->view_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
            info->source_table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 3, &isnull));
            info->materialization_table_oid = isnull ? InvalidOid :
                DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 4, &isnull));
            info->query_text = SPI_getvalue(tuple, tupdesc, 5);
            /* refresh_interval and last_refresh handled separately */
            info->enabled = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));

            aggs = lappend(aggs, info);
        }
    }

    SPI_finish();
    pfree(query.data);

    return aggs;
}

int64
orochi_catalog_register_continuous_agg(Oid view_oid, Oid source_table,
                                       Oid mat_table, const char *query_text)
{
    StringInfoData query;
    int ret;
    int64 agg_id = -1;

    ensure_catalog_initialized();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_continuous_aggregates "
        "(view_oid, source_table_oid, materialization_table_oid, query_text, enabled) "
        "VALUES (%u, %u, %u, %s, TRUE) "
        "ON CONFLICT (view_oid) DO UPDATE SET "
        "query_text = EXCLUDED.query_text, "
        "materialization_table_oid = EXCLUDED.materialization_table_oid "
        "RETURNING agg_id",
        view_oid, source_table, mat_table,
        quote_literal_cstr(query_text));

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if ((ret == SPI_OK_INSERT_RETURNING || ret == SPI_OK_UPDATE_RETURNING) && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        agg_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    elog(DEBUG1, "Registered continuous aggregate %u on table %u (agg_id=%ld)",
         view_oid, source_table, agg_id);

    return agg_id;
}

OrochiContinuousAggInfo *
orochi_catalog_get_continuous_agg(Oid view_oid)
{
    StringInfoData query;
    OrochiContinuousAggInfo *info = NULL;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT agg_id, view_oid, source_table_oid, materialization_table_oid, "
        "query_text, refresh_interval, last_refresh, enabled "
        "FROM orochi.orochi_continuous_aggregates "
        "WHERE view_oid = %u",
        view_oid);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        info = (OrochiContinuousAggInfo *) palloc0(sizeof(OrochiContinuousAggInfo));

        info->agg_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        info->view_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        info->source_table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        info->materialization_table_oid = isnull ? InvalidOid :
            DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        info->query_text = SPI_getvalue(tuple, tupdesc, 5);
        info->enabled = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return info;
}

void
orochi_catalog_update_continuous_agg_refresh(int64 agg_id, TimestampTz last_refresh)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_continuous_aggregates "
        "SET last_refresh = '%s'::timestamptz "
        "WHERE agg_id = %ld",
        timestamptz_to_str(last_refresh), agg_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

void
orochi_catalog_delete_continuous_agg(Oid view_oid)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_continuous_aggregates WHERE view_oid = %u",
        view_oid);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

void
orochi_catalog_create_column_chunk(OrochiColumnChunk *chunk)
{
    /* TODO: Implement column chunk creation */
}

void
orochi_catalog_delete_tiering_policy(int64 policy_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_tiering_policies WHERE policy_id = %ld", policy_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);
}

void
orochi_catalog_update_tiering_policy(OrochiTieringPolicy *policy)
{
    orochi_catalog_create_tiering_policy(policy);
}

void
orochi_catalog_execute(const char *sql_query)
{
    SPI_connect();
    SPI_execute(sql_query, false, 0);
    SPI_finish();
}

void
orochi_catalog_begin_transaction(void)
{
    /* Transaction management is handled by PostgreSQL */
}

void
orochi_catalog_commit_transaction(void)
{
    /* Transaction management is handled by PostgreSQL */
}

void
orochi_catalog_rollback_transaction(void)
{
    /* Transaction management is handled by PostgreSQL */
}

OrochiChunkInfo *
orochi_catalog_get_chunk(int64 chunk_id)
{
    StringInfoData query;
    OrochiChunkInfo *info = NULL;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT chunk_id, hypertable_oid, dimension_id, range_start, range_end, "
        "row_count, size_bytes, is_compressed, storage_tier, tablespace_name "
        "FROM orochi.orochi_chunks WHERE chunk_id = %ld",
        chunk_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        info = (OrochiChunkInfo *) palloc0(sizeof(OrochiChunkInfo));

        info->chunk_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        info->hypertable_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        info->dimension_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        info->range_start = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        info->range_end = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        info->row_count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        info->size_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        info->is_compressed = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        info->storage_tier = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        info->tablespace = SPI_getvalue(tuple, tupdesc, 10);
    }

    SPI_finish();
    pfree(query.data);

    return info;
}
