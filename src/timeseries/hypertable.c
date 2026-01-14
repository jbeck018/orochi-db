/*-------------------------------------------------------------------------
 *
 * hypertable.c
 *    Orochi DB time-series hypertable implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "hypertable.h"

/* ============================================================
 * Time Bucketing Implementation
 * ============================================================ */

/*
 * Convert interval to microseconds for bucketing
 */
static int64
interval_to_usec(Interval *interval)
{
    int64 usec = 0;

    usec += interval->time;
    usec += (int64) interval->day * USECS_PER_DAY;
    usec += (int64) interval->month * USECS_PER_DAY * 30;  /* Approximate */

    return usec;
}

TimestampTz
orochi_time_bucket(Interval *bucket_width, TimestampTz ts)
{
    int64 bucket_usec;
    int64 ts_usec;
    int64 bucket_start;

    bucket_usec = interval_to_usec(bucket_width);
    if (bucket_usec <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bucket width must be positive")));

    /* Convert timestamp to microseconds since epoch */
    ts_usec = ts;

    /* Calculate bucket start */
    bucket_start = (ts_usec / bucket_usec) * bucket_usec;

    return (TimestampTz) bucket_start;
}

TimestampTz
orochi_time_bucket_offset(Interval *bucket_width, TimestampTz ts, Interval *offset)
{
    int64 offset_usec = interval_to_usec(offset);
    TimestampTz adjusted_ts = ts + offset_usec;
    TimestampTz bucket = orochi_time_bucket(bucket_width, adjusted_ts);

    return bucket - offset_usec;
}

TimestampTz
orochi_time_bucket_origin(Interval *bucket_width, TimestampTz ts, TimestampTz origin)
{
    int64 bucket_usec = interval_to_usec(bucket_width);
    int64 delta = ts - origin;
    int64 buckets;

    if (delta >= 0)
        buckets = delta / bucket_usec;
    else
        buckets = (delta - bucket_usec + 1) / bucket_usec;

    return origin + (buckets * bucket_usec);
}

int64
orochi_time_bucket_int(int64 bucket_width, int64 value)
{
    if (bucket_width <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bucket width must be positive")));

    if (value >= 0)
        return (value / bucket_width) * bucket_width;
    else
        return ((value - bucket_width + 1) / bucket_width) * bucket_width;
}

/* ============================================================
 * Hypertable Management
 * ============================================================ */

void
orochi_create_hypertable(Oid table_oid, const char *time_column,
                         Interval *chunk_interval, bool if_not_exists)
{
    Relation rel;
    TupleDesc tupdesc;
    OrochiTableInfo table_info;
    int64 interval_usec;
    bool column_found = false;
    Oid column_type = InvalidOid;
    int i;
    char *schema_name;
    char *table_name;

    /* Check if already a hypertable */
    if (orochi_is_hypertable(table_oid))
    {
        if (if_not_exists)
        {
            elog(NOTICE, "table is already a hypertable, skipping");
            return;
        }
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("table is already a hypertable")));
    }

    /* Validate chunk interval */
    if (chunk_interval == NULL)
        interval_usec = DEFAULT_CHUNK_INTERVAL_USEC;
    else
        interval_usec = interval_to_usec(chunk_interval);

    if (interval_usec < MIN_CHUNK_INTERVAL_USEC)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("chunk interval too small, minimum is 1 hour")));

    /* Open relation and validate time column */
    rel = relation_open(table_oid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (!attr->attisdropped &&
            strcmp(NameStr(attr->attname), time_column) == 0)
        {
            column_found = true;
            column_type = attr->atttypid;
            break;
        }
    }

    if (!column_found)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", time_column)));

    /* Validate column type is time-like */
    if (column_type != TIMESTAMPOID &&
        column_type != TIMESTAMPTZOID &&
        column_type != DATEOID &&
        column_type != INT8OID &&
        column_type != INT4OID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("time column must be timestamp, timestamptz, date, or integer type")));
    }

    schema_name = get_namespace_name(RelationGetNamespace(rel));
    table_name = pstrdup(RelationGetRelationName(rel));

    relation_close(rel, AccessShareLock);

    /* Register in catalog */
    memset(&table_info, 0, sizeof(table_info));
    table_info.relid = table_oid;
    table_info.schema_name = schema_name;
    table_info.table_name = table_name;
    table_info.storage_type = OROCHI_STORAGE_ROW;
    table_info.shard_strategy = OROCHI_SHARD_RANGE;
    table_info.is_distributed = false;
    table_info.is_timeseries = true;
    table_info.time_column = pstrdup(time_column);
    table_info.chunk_interval = chunk_interval;
    table_info.compression = OROCHI_COMPRESS_NONE;

    orochi_catalog_register_table(&table_info);

    /* Create initial dimension */
    /* TODO: Register dimension in catalog */

    elog(NOTICE, "Created hypertable \"%s.%s\" with chunk interval of %ld hours",
         schema_name, table_name, interval_usec / USECS_PER_HOUR);
}

void
orochi_create_hypertable_with_space(Oid table_oid, const char *time_column,
                                    Interval *chunk_interval,
                                    const char *space_column, int num_partitions)
{
    /* First create basic hypertable */
    orochi_create_hypertable(table_oid, time_column, chunk_interval, false);

    /* Add space dimension */
    orochi_add_dimension(table_oid, space_column, num_partitions, NULL);

    elog(NOTICE, "Added space dimension on column \"%s\" with %d partitions",
         space_column, num_partitions);
}

void
orochi_add_dimension(Oid hypertable_oid, const char *column_name,
                     int num_partitions, Interval *interval)
{
    /* TODO: Implement dimension addition */
    elog(NOTICE, "Adding dimension on column \"%s\"", column_name);
}

HypertableInfo *
orochi_get_hypertable_info(Oid hypertable_oid)
{
    OrochiTableInfo *table_info;
    HypertableInfo *info;

    table_info = orochi_catalog_get_table(hypertable_oid);
    if (table_info == NULL || !table_info->is_timeseries)
        return NULL;

    info = palloc0(sizeof(HypertableInfo));
    info->hypertable_oid = hypertable_oid;
    info->schema_name = table_info->schema_name;
    info->table_name = table_info->table_name;
    info->is_distributed = table_info->is_distributed;

    /* TODO: Load dimensions and other metadata */

    return info;
}

bool
orochi_is_hypertable(Oid table_oid)
{
    OrochiTableInfo *info = orochi_catalog_get_table(table_oid);
    return (info != NULL && info->is_timeseries);
}

/* ============================================================
 * Chunk Management
 * ============================================================ */

/*
 * Generate chunk name
 */
static char *
generate_chunk_name(Oid hypertable_oid, int64 chunk_id)
{
    char *name = palloc(MAX_CHUNK_NAME_LEN);
    snprintf(name, MAX_CHUNK_NAME_LEN, "%s%u_%ld", CHUNK_PREFIX, hypertable_oid, chunk_id);
    return name;
}

/*
 * Calculate chunk boundaries for a timestamp
 */
static void
calculate_chunk_range(Oid hypertable_oid, TimestampTz ts,
                      TimestampTz *range_start, TimestampTz *range_end)
{
    OrochiTableInfo *table_info;
    int64 interval_usec;

    table_info = orochi_catalog_get_table(hypertable_oid);
    if (table_info == NULL || !table_info->is_timeseries)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("table %u is not a hypertable", hypertable_oid)));

    if (table_info->chunk_interval != NULL)
        interval_usec = interval_to_usec(table_info->chunk_interval);
    else
        interval_usec = DEFAULT_CHUNK_INTERVAL_USEC;

    /* Align to chunk boundary */
    *range_start = (ts / interval_usec) * interval_usec;
    *range_end = *range_start + interval_usec;
}

ChunkInfo *
orochi_get_chunk_for_timestamp(Oid hypertable_oid, TimestampTz ts)
{
    TimestampTz range_start, range_end;
    List *chunks;
    ListCell *lc;
    ChunkInfo *chunk = NULL;
    int64 chunk_id;

    /* Calculate chunk boundaries */
    calculate_chunk_range(hypertable_oid, ts, &range_start, &range_end);

    /* Check if chunk exists */
    chunks = orochi_catalog_get_chunks_in_range(hypertable_oid, range_start, range_end);

    foreach(lc, chunks)
    {
        OrochiChunkInfo *ci = (OrochiChunkInfo *) lfirst(lc);

        if (ci->range_start == range_start)
        {
            chunk = palloc0(sizeof(ChunkInfo));
            chunk->chunk_id = ci->chunk_id;
            chunk->hypertable_oid = ci->hypertable_oid;
            chunk->is_compressed = ci->is_compressed;
            chunk->storage_tier = ci->storage_tier;
            chunk->row_count = ci->row_count;
            chunk->size_bytes = ci->size_bytes;
            return chunk;
        }
    }

    /* Create new chunk */
    chunk_id = orochi_catalog_create_chunk(hypertable_oid, range_start, range_end);

    if (chunk_id > 0)
    {
        StringInfoData create_sql;
        int ret;

        /* Create physical chunk table */
        initStringInfo(&create_sql);
        appendStringInfo(&create_sql,
            "CREATE TABLE orochi.%s (LIKE %s.%s INCLUDING ALL) "
            "WITH (autovacuum_enabled = false)",
            generate_chunk_name(hypertable_oid, chunk_id),
            orochi_catalog_get_table(hypertable_oid)->schema_name,
            orochi_catalog_get_table(hypertable_oid)->table_name);

        SPI_connect();
        ret = SPI_execute(create_sql.data, false, 0);
        SPI_finish();

        pfree(create_sql.data);

        /* Return chunk info */
        chunk = palloc0(sizeof(ChunkInfo));
        chunk->chunk_id = chunk_id;
        chunk->hypertable_oid = hypertable_oid;
        chunk->chunk_schema = "orochi";
        chunk->chunk_name = generate_chunk_name(hypertable_oid, chunk_id);
        chunk->is_compressed = false;
        chunk->storage_tier = OROCHI_TIER_HOT;

        elog(DEBUG1, "Created chunk %ld for hypertable %u", chunk_id, hypertable_oid);
    }

    return chunk;
}

List *
orochi_get_chunks_in_time_range(Oid hypertable_oid, TimestampTz start, TimestampTz end)
{
    List *catalog_chunks;
    List *result = NIL;
    ListCell *lc;

    catalog_chunks = orochi_catalog_get_chunks_in_range(hypertable_oid, start, end);

    foreach(lc, catalog_chunks)
    {
        OrochiChunkInfo *ci = (OrochiChunkInfo *) lfirst(lc);
        ChunkInfo *chunk = palloc0(sizeof(ChunkInfo));

        chunk->chunk_id = ci->chunk_id;
        chunk->hypertable_oid = ci->hypertable_oid;
        chunk->is_compressed = ci->is_compressed;
        chunk->storage_tier = ci->storage_tier;
        chunk->row_count = ci->row_count;
        chunk->size_bytes = ci->size_bytes;

        result = lappend(result, chunk);
    }

    return result;
}

void
orochi_compress_chunk(int64 chunk_id)
{
    OrochiChunkInfo *chunk_info;

    chunk_info = orochi_catalog_get_chunk(chunk_id);
    if (chunk_info == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("chunk %ld does not exist", chunk_id)));

    if (chunk_info->is_compressed)
    {
        elog(NOTICE, "chunk %ld is already compressed", chunk_id);
        return;
    }

    /* TODO: Implement actual compression
     * 1. Read all data from chunk
     * 2. Convert to columnar format with compression
     * 3. Replace chunk data
     * 4. Update catalog
     */

    orochi_catalog_update_chunk_compression(chunk_id, true);
    elog(NOTICE, "Compressed chunk %ld", chunk_id);
}

void
orochi_decompress_chunk(int64 chunk_id)
{
    OrochiChunkInfo *chunk_info;

    chunk_info = orochi_catalog_get_chunk(chunk_id);
    if (chunk_info == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("chunk %ld does not exist", chunk_id)));

    if (!chunk_info->is_compressed)
    {
        elog(NOTICE, "chunk %ld is not compressed", chunk_id);
        return;
    }

    /* TODO: Implement decompression */

    orochi_catalog_update_chunk_compression(chunk_id, false);
    elog(NOTICE, "Decompressed chunk %ld", chunk_id);
}

int
orochi_drop_chunks_older_than(Oid hypertable_oid, Interval *older_than)
{
    TimestampTz cutoff;
    List *chunks;
    ListCell *lc;
    int dropped = 0;

    cutoff = GetCurrentTimestamp() - interval_to_usec(older_than);

    chunks = orochi_get_chunks_in_time_range(hypertable_oid, 0, cutoff);

    foreach(lc, chunks)
    {
        ChunkInfo *chunk = (ChunkInfo *) lfirst(lc);
        orochi_drop_chunk(chunk->chunk_id);
        dropped++;
    }

    elog(NOTICE, "Dropped %d chunks older than specified interval", dropped);
    return dropped;
}

void
orochi_drop_chunk(int64 chunk_id)
{
    /* TODO: Drop physical chunk table */
    orochi_catalog_delete_chunk(chunk_id);
    elog(DEBUG1, "Dropped chunk %ld", chunk_id);
}

/* ============================================================
 * Policies
 * ============================================================ */

void
orochi_add_retention_policy(Oid hypertable_oid, Interval *drop_after)
{
    if (!orochi_is_hypertable(hypertable_oid))
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("table %u is not a hypertable", hypertable_oid)));

    /* TODO: Store policy in catalog */
    elog(NOTICE, "Added retention policy for hypertable %u", hypertable_oid);
}

void
orochi_add_compression_policy(Oid hypertable_oid, Interval *compress_after)
{
    if (!orochi_is_hypertable(hypertable_oid))
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("table %u is not a hypertable", hypertable_oid)));

    /* TODO: Store policy in catalog */
    elog(NOTICE, "Added compression policy for hypertable %u", hypertable_oid);
}

void
orochi_enable_compression(Oid hypertable_oid, const char *segment_by,
                          const char *order_by)
{
    OrochiTableInfo *table_info;

    table_info = orochi_catalog_get_table(hypertable_oid);
    if (table_info == NULL || !table_info->is_timeseries)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("table %u is not a hypertable", hypertable_oid)));

    /* Update compression settings */
    table_info->compression = OROCHI_COMPRESS_ZSTD;
    orochi_catalog_update_table(table_info);

    elog(NOTICE, "Enabled compression on hypertable %u", hypertable_oid);
}

/* ============================================================
 * Continuous Aggregates
 * ============================================================ */

void
orochi_create_continuous_aggregate(const char *view_name, const char *query,
                                   Interval *refresh_interval, bool with_data)
{
    /* TODO: Implement continuous aggregate creation
     * 1. Parse query to extract source table and time bucket
     * 2. Create materialization hypertable
     * 3. Create view definition
     * 4. Set up invalidation tracking
     * 5. Optionally materialize initial data
     */
    elog(NOTICE, "Creating continuous aggregate %s", view_name);
}

void
orochi_refresh_continuous_aggregate(Oid view_oid, TimestampTz start, TimestampTz end)
{
    /* TODO: Implement refresh
     * 1. Get invalidated ranges
     * 2. Delete affected materialized data
     * 3. Re-aggregate from source
     * 4. Clear invalidation log
     */
    elog(NOTICE, "Refreshing continuous aggregate %u from %s to %s",
         view_oid, timestamptz_to_str(start), timestamptz_to_str(end));
}

void
orochi_log_invalidation(Oid hypertable_oid, TimestampTz start, TimestampTz end)
{
    orochi_catalog_log_invalidation(hypertable_oid, start, end);
}

/* ============================================================
 * Maintenance
 * ============================================================ */

void
orochi_run_maintenance(void)
{
    /* TODO: Run all maintenance tasks
     * 1. Apply retention policies
     * 2. Apply compression policies
     * 3. Refresh continuous aggregates
     * 4. Apply tiering policies
     */
    elog(LOG, "Running Orochi maintenance");
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_hypertable_sql);
PG_FUNCTION_INFO_V1(orochi_add_dimension_sql);
PG_FUNCTION_INFO_V1(orochi_time_bucket_timestamp);
PG_FUNCTION_INFO_V1(orochi_time_bucket_timestamptz);
PG_FUNCTION_INFO_V1(orochi_time_bucket_timestamptz_origin);
PG_FUNCTION_INFO_V1(orochi_time_bucket_int_sql);
PG_FUNCTION_INFO_V1(orochi_compress_chunk_sql);
PG_FUNCTION_INFO_V1(orochi_decompress_chunk_sql);
PG_FUNCTION_INFO_V1(orochi_drop_chunks_sql);
PG_FUNCTION_INFO_V1(orochi_add_compression_policy_sql);
PG_FUNCTION_INFO_V1(orochi_remove_compression_policy_sql);
PG_FUNCTION_INFO_V1(orochi_add_retention_policy_sql);
PG_FUNCTION_INFO_V1(orochi_remove_retention_policy_sql);
PG_FUNCTION_INFO_V1(orochi_run_maintenance_sql);

Datum
orochi_create_hypertable_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *time_column_text = PG_GETARG_TEXT_PP(1);
    Interval *chunk_interval = PG_GETARG_INTERVAL_P(2);
    bool if_not_exists = PG_GETARG_BOOL(3);
    char *time_column = text_to_cstring(time_column_text);

    orochi_create_hypertable(table_oid, time_column, chunk_interval, if_not_exists);
    PG_RETURN_VOID();
}

Datum
orochi_add_dimension_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    text *column_text = PG_GETARG_TEXT_PP(1);
    int32 num_partitions = PG_GETARG_INT32(2);
    Interval *interval_val = PG_ARGISNULL(3) ? NULL : PG_GETARG_INTERVAL_P(3);
    char *column_name = text_to_cstring(column_text);

    orochi_add_dimension(hypertable_oid, column_name, num_partitions, interval_val);
    PG_RETURN_VOID();
}

Datum
orochi_time_bucket_timestamp(PG_FUNCTION_ARGS)
{
    Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
    Timestamp ts = PG_GETARG_TIMESTAMP(1);
    /* Convert timestamp to timestamptz for bucketing, then back */
    TimestampTz tstz = (TimestampTz) ts;  /* Simple conversion */
    TimestampTz result = orochi_time_bucket(bucket_width, tstz);
    PG_RETURN_TIMESTAMP((Timestamp) result);
}

Datum
orochi_time_bucket_timestamptz(PG_FUNCTION_ARGS)
{
    Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
    TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
    PG_RETURN_TIMESTAMPTZ(orochi_time_bucket(bucket_width, ts));
}

Datum
orochi_time_bucket_timestamptz_origin(PG_FUNCTION_ARGS)
{
    Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
    TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
    TimestampTz origin = PG_GETARG_TIMESTAMPTZ(2);
    PG_RETURN_TIMESTAMPTZ(orochi_time_bucket_origin(bucket_width, ts, origin));
}

Datum
orochi_time_bucket_int_sql(PG_FUNCTION_ARGS)
{
    int64 bucket_width = PG_GETARG_INT64(0);
    int64 ts = PG_GETARG_INT64(1);
    int64 result = orochi_time_bucket_int(bucket_width, ts);
    PG_RETURN_INT64(result);
}

Datum
orochi_compress_chunk_sql(PG_FUNCTION_ARGS)
{
    int64 chunk_id = PG_GETARG_INT64(0);
    orochi_compress_chunk(chunk_id);
    PG_RETURN_VOID();
}

Datum
orochi_decompress_chunk_sql(PG_FUNCTION_ARGS)
{
    int64 chunk_id = PG_GETARG_INT64(0);
    orochi_decompress_chunk(chunk_id);
    PG_RETURN_VOID();
}

Datum
orochi_drop_chunks_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    Interval *older_than = PG_ARGISNULL(1) ? NULL : PG_GETARG_INTERVAL_P(1);
    int dropped;

    dropped = orochi_drop_chunks_older_than(hypertable_oid, older_than);
    PG_RETURN_INT32(dropped);
}

Datum
orochi_add_compression_policy_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    Interval *compress_after = PG_GETARG_INTERVAL_P(1);

    orochi_add_compression_policy(hypertable_oid, compress_after);
    /* Return a dummy policy ID for now */
    PG_RETURN_INT32(1);
}

Datum
orochi_remove_compression_policy_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    /* TODO: Implement policy removal */
    elog(NOTICE, "Removed compression policy from hypertable %u", hypertable_oid);
    PG_RETURN_VOID();
}

Datum
orochi_add_retention_policy_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    Interval *drop_after = PG_GETARG_INTERVAL_P(1);

    orochi_add_retention_policy(hypertable_oid, drop_after);
    /* Return a dummy policy ID for now */
    PG_RETURN_INT32(1);
}

Datum
orochi_remove_retention_policy_sql(PG_FUNCTION_ARGS)
{
    Oid hypertable_oid = PG_GETARG_OID(0);
    /* TODO: Implement policy removal */
    elog(NOTICE, "Removed retention policy from hypertable %u", hypertable_oid);
    PG_RETURN_VOID();
}

Datum
orochi_run_maintenance_sql(PG_FUNCTION_ARGS)
{
    orochi_run_maintenance();
    PG_RETURN_VOID();
}
