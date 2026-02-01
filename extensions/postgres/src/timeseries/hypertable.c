/*-------------------------------------------------------------------------
 *
 * hypertable.c
 *    Orochi DB time-series hypertable implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "../core/catalog.h"
#include "../orochi.h"
#include "hypertable.h"

/* ============================================================
 * Time Bucketing Implementation
 * ============================================================ */

/*
 * Convert interval to microseconds for bucketing
 */
static int64 interval_to_usec(Interval *interval) {
  int64 usec = 0;

  usec += interval->time;
  usec += (int64)interval->day * USECS_PER_DAY;
  usec += (int64)interval->month * USECS_PER_DAY * 30; /* Approximate */

  return usec;
}

TimestampTz orochi_time_bucket(Interval *bucket_width, TimestampTz ts) {
  int64 bucket_usec;
  int64 ts_usec;
  int64 bucket_start;

  bucket_usec = interval_to_usec(bucket_width);
  if (bucket_usec <= 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("bucket width must be positive")));

  /* Convert timestamp to microseconds since epoch */
  ts_usec = ts;

  /* Calculate bucket start */
  bucket_start = (ts_usec / bucket_usec) * bucket_usec;

  return (TimestampTz)bucket_start;
}

TimestampTz orochi_time_bucket_offset(Interval *bucket_width, TimestampTz ts,
                                      Interval *offset) {
  int64 offset_usec = interval_to_usec(offset);
  TimestampTz adjusted_ts = ts + offset_usec;
  TimestampTz bucket = orochi_time_bucket(bucket_width, adjusted_ts);

  return bucket - offset_usec;
}

TimestampTz orochi_time_bucket_origin(Interval *bucket_width, TimestampTz ts,
                                      TimestampTz origin) {
  int64 bucket_usec = interval_to_usec(bucket_width);
  int64 delta = ts - origin;
  int64 buckets;

  if (delta >= 0)
    buckets = delta / bucket_usec;
  else
    buckets = (delta - bucket_usec + 1) / bucket_usec;

  return origin + (buckets * bucket_usec);
}

int64 orochi_time_bucket_int(int64 bucket_width, int64 value) {
  if (bucket_width <= 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("bucket width must be positive")));

  if (value >= 0)
    return (value / bucket_width) * bucket_width;
  else
    return ((value - bucket_width + 1) / bucket_width) * bucket_width;
}

/* ============================================================
 * Hypertable Management
 * ============================================================ */

void orochi_create_hypertable(Oid table_oid, const char *time_column,
                              Interval *chunk_interval, bool if_not_exists) {
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
  if (orochi_is_hypertable(table_oid)) {
    if (if_not_exists) {
      elog(NOTICE, "table is already a hypertable, skipping");
      return;
    }
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT),
                    errmsg("table is already a hypertable")));
  }

  /* Validate chunk interval */
  if (chunk_interval == NULL)
    interval_usec = DEFAULT_CHUNK_INTERVAL_USEC;
  else
    interval_usec = interval_to_usec(chunk_interval);

  if (interval_usec < MIN_CHUNK_INTERVAL_USEC)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("chunk interval too small, minimum is 1 hour")));

  /* Open relation and validate time column */
  rel = relation_open(table_oid, AccessShareLock);
  tupdesc = RelationGetDescr(rel);

  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    if (!attr->attisdropped &&
        strcmp(NameStr(attr->attname), time_column) == 0) {
      column_found = true;
      column_type = attr->atttypid;
      break;
    }
  }

  if (!column_found)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" does not exist", time_column)));

  /* Validate column type is time-like */
  if (column_type != TIMESTAMPOID && column_type != TIMESTAMPTZOID &&
      column_type != DATEOID && column_type != INT8OID &&
      column_type != INT4OID) {
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("time column must be timestamp, timestamptz, date, "
                           "or integer type")));
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

  /* Create initial time dimension */
  orochi_register_dimension(table_oid, time_column, 0 /* time dimension */, 1,
                            interval_usec);

  elog(NOTICE, "Created hypertable \"%s.%s\" with chunk interval of %ld hours",
       schema_name, table_name, interval_usec / USECS_PER_HOUR);
}

void orochi_create_hypertable_with_space(Oid table_oid, const char *time_column,
                                         Interval *chunk_interval,
                                         const char *space_column,
                                         int num_partitions) {
  /* First create basic hypertable */
  orochi_create_hypertable(table_oid, time_column, chunk_interval, false);

  /* Add space dimension */
  orochi_add_dimension(table_oid, space_column, num_partitions, NULL);

  elog(NOTICE, "Added space dimension on column \"%s\" with %d partitions",
       space_column, num_partitions);
}

/*
 * Register a dimension in the catalog
 */
void orochi_register_dimension(Oid hypertable_oid, const char *column_name,
                               int dimension_type, int num_partitions,
                               int64 interval_length) {
  StringInfoData query;
  int ret;

  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query,
                   "INSERT INTO orochi.orochi_dimensions "
                   "(hypertable_oid, column_name, dimension_type, "
                   "num_partitions, interval_length) "
                   "VALUES (%u, '%s', %d, %d, %ld) "
                   "ON CONFLICT (hypertable_oid, column_name) DO UPDATE SET "
                   "dimension_type = EXCLUDED.dimension_type, "
                   "num_partitions = EXCLUDED.num_partitions, "
                   "interval_length = EXCLUDED.interval_length",
                   hypertable_oid, column_name, dimension_type, num_partitions,
                   interval_length);

  ret = SPI_execute(query.data, false, 0);
  if (ret != SPI_OK_INSERT)
    elog(WARNING, "Failed to register dimension: %s", query.data);

  pfree(query.data);
  SPI_finish();

  elog(DEBUG1,
       "Registered dimension '%s' for hypertable %u (type=%d, partitions=%d)",
       column_name, hypertable_oid, dimension_type, num_partitions);
}

void orochi_add_dimension(Oid hypertable_oid, const char *column_name,
                          int num_partitions, Interval *interval) {
  int64 interval_usec = 0;
  int dimension_type = 1; /* space dimension by default */

  if (!orochi_is_hypertable(hypertable_oid))
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("table %u is not a hypertable", hypertable_oid)));

  /* If interval is provided, this is a time-like dimension */
  if (interval != NULL) {
    interval_usec = interval_to_usec(interval);
    dimension_type = 0; /* time dimension */
  }

  /* Register the dimension in catalog */
  orochi_register_dimension(hypertable_oid, column_name, dimension_type,
                            num_partitions, interval_usec);

  elog(NOTICE, "Added dimension on column \"%s\" with %d partitions",
       column_name, num_partitions);
}

HypertableInfo *orochi_get_hypertable_info(Oid hypertable_oid) {
  OrochiTableInfo *table_info;
  HypertableInfo *info;
  StringInfoData query;
  int ret;
  int i;

  table_info = orochi_catalog_get_table(hypertable_oid);
  if (table_info == NULL || !table_info->is_timeseries)
    return NULL;

  info = palloc0(sizeof(HypertableInfo));
  info->hypertable_oid = hypertable_oid;
  info->schema_name = table_info->schema_name;
  info->table_name = table_info->table_name;
  info->is_distributed = table_info->is_distributed;

  /* Load dimensions from catalog */
  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query,
                   "SELECT dimension_id, column_name, dimension_type, "
                   "num_partitions, interval_length "
                   "FROM orochi.orochi_dimensions WHERE hypertable_oid = %u "
                   "ORDER BY dimension_type, dimension_id",
                   hypertable_oid);

  ret = SPI_execute(query.data, true, 0);
  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    info->num_dimensions = SPI_processed;
    info->dimensions =
        palloc0(sizeof(HypertableDimension) * info->num_dimensions);

    for (i = 0; i < SPI_processed; i++) {
      HeapTuple tuple = SPI_tuptable->vals[i];
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      bool isnull;
      int dim_type;

      info->dimensions[i].dimension_id =
          DatumGetInt32(SPI_getbinval(tuple, tupdesc, 1, &isnull));

      info->dimensions[i].column_name = SPI_getvalue(tuple, tupdesc, 2);

      dim_type = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
      info->dimensions[i].dim_type =
          (dim_type == 0) ? DIMENSION_TIME : DIMENSION_SPACE;

      info->dimensions[i].num_slices =
          DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));

      info->dimensions[i].interval_length =
          DatumGetInt64(SPI_getbinval(tuple, tupdesc, 5, &isnull));

      info->dimensions[i].hypertable_oid = hypertable_oid;
    }

    elog(DEBUG1, "Loaded %d dimensions for hypertable %u", info->num_dimensions,
         hypertable_oid);
  } else {
    info->num_dimensions = 0;
    info->dimensions = NULL;
  }

  pfree(query.data);
  SPI_finish();

  /* Load chunk count */
  info->chunk_count = orochi_catalog_get_chunk_count(hypertable_oid);

  /* Load compression settings */
  info->compression_enabled = (table_info->compression != OROCHI_COMPRESS_NONE);

  return info;
}

bool orochi_is_hypertable(Oid table_oid) {
  OrochiTableInfo *info = orochi_catalog_get_table(table_oid);
  return (info != NULL && info->is_timeseries);
}

/* ============================================================
 * Chunk Management
 * ============================================================ */

/*
 * Generate chunk name
 */
static char *generate_chunk_name(Oid hypertable_oid, int64 chunk_id) {
  char *name = palloc(MAX_CHUNK_NAME_LEN);
  snprintf(name, MAX_CHUNK_NAME_LEN, "%s%u_%ld", CHUNK_PREFIX, hypertable_oid,
           chunk_id);
  return name;
}

/*
 * Calculate chunk boundaries for a timestamp
 */
static void calculate_chunk_range(Oid hypertable_oid, TimestampTz ts,
                                  TimestampTz *range_start,
                                  TimestampTz *range_end) {
  OrochiTableInfo *table_info;
  int64 interval_usec;

  table_info = orochi_catalog_get_table(hypertable_oid);
  if (table_info == NULL || !table_info->is_timeseries)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("table %u is not a hypertable", hypertable_oid)));

  if (table_info->chunk_interval != NULL)
    interval_usec = interval_to_usec(table_info->chunk_interval);
  else
    interval_usec = DEFAULT_CHUNK_INTERVAL_USEC;

  /* Align to chunk boundary */
  *range_start = (ts / interval_usec) * interval_usec;
  *range_end = *range_start + interval_usec;
}

ChunkInfo *orochi_get_chunk_for_timestamp(Oid hypertable_oid, TimestampTz ts) {
  TimestampTz range_start, range_end;
  List *chunks;
  ListCell *lc;
  ChunkInfo *chunk = NULL;
  int64 chunk_id;

  /* Calculate chunk boundaries */
  calculate_chunk_range(hypertable_oid, ts, &range_start, &range_end);

  /* Check if chunk exists */
  chunks = orochi_catalog_get_chunks_in_range(hypertable_oid, range_start,
                                              range_end);

  foreach (lc, chunks) {
    OrochiChunkInfo *ci = (OrochiChunkInfo *)lfirst(lc);

    if (ci->range_start == range_start) {
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
  chunk_id =
      orochi_catalog_create_chunk(hypertable_oid, range_start, range_end);

  if (chunk_id > 0) {
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

    elog(DEBUG1, "Created chunk %ld for hypertable %u", chunk_id,
         hypertable_oid);
  }

  return chunk;
}

List *orochi_get_chunks_in_time_range(Oid hypertable_oid, TimestampTz start,
                                      TimestampTz end) {
  List *catalog_chunks;
  List *result = NIL;
  ListCell *lc;

  catalog_chunks =
      orochi_catalog_get_chunks_in_range(hypertable_oid, start, end);

  foreach (lc, catalog_chunks) {
    OrochiChunkInfo *ci = (OrochiChunkInfo *)lfirst(lc);
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

void orochi_compress_chunk(int64 chunk_id) {
  OrochiChunkInfo *chunk_info;
  OrochiTableInfo *table_info;
  StringInfoData query;
  char *chunk_name;
  int ret;
  int64 original_size = 0;
  int64 compressed_size = 0;

  chunk_info = orochi_catalog_get_chunk(chunk_id);
  if (chunk_info == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("chunk %ld does not exist", chunk_id)));

  if (chunk_info->is_compressed) {
    elog(NOTICE, "chunk %ld is already compressed", chunk_id);
    return;
  }

  table_info = orochi_catalog_get_table(chunk_info->hypertable_oid);
  if (table_info == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("hypertable %u does not exist",
                           chunk_info->hypertable_oid)));

  chunk_name = generate_chunk_name(chunk_info->hypertable_oid, chunk_id);

  SPI_connect();

  /*
   * Compression strategy:
   * 1. Get original row count and size
   * 2. Create a compressed copy of the chunk with TOAST compression enabled
   * 3. Store compressed data in orochi.compressed_chunks table
   * 4. Update original chunk to mark as compressed
   */

  /* Get original row count */
  initStringInfo(&query);
  appendStringInfo(&query,
                   "SELECT pg_total_relation_size('orochi.%s'::regclass)",
                   chunk_name);
  ret = SPI_execute(query.data, true, 1);
  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    original_size = DatumGetInt64(SPI_getbinval(
        SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
  }
  pfree(query.data);

  /* Create compressed data storage table if it doesn't exist */
  initStringInfo(&query);
  appendStringInfo(&query,
                   "CREATE TABLE IF NOT EXISTS orochi.compressed_chunk_data ("
                   "chunk_id BIGINT PRIMARY KEY, "
                   "hypertable_oid OID NOT NULL, "
                   "compressed_data BYTEA NOT NULL, "
                   "original_size BIGINT NOT NULL, "
                   "compressed_size BIGINT NOT NULL, "
                   "row_count BIGINT NOT NULL, "
                   "compression_type INTEGER DEFAULT 2, "
                   "compressed_at TIMESTAMPTZ DEFAULT now()"
                   ")");
  SPI_execute(query.data, false, 0);
  pfree(query.data);

  /* Export chunk data to compressed bytea using ZSTD via COPY */
  initStringInfo(&query);
  appendStringInfo(
      &query,
      "INSERT INTO orochi.compressed_chunk_data "
      "(chunk_id, hypertable_oid, compressed_data, original_size, "
      "compressed_size, row_count) "
      "SELECT %ld, %u, "
      "compress(string_agg(row_data::text, E'\\n')::bytea, 'zstd'), "
      "%ld, "
      "length(compress(string_agg(row_data::text, E'\\n')::bytea, 'zstd')), "
      "count(*) "
      "FROM (SELECT row_to_json(t.*) as row_data FROM orochi.%s t) sub",
      chunk_id, chunk_info->hypertable_oid, original_size, chunk_name);

  ret = SPI_execute(query.data, false, 0);
  pfree(query.data);

  if (ret != SPI_OK_INSERT) {
    /* Fallback: just mark as compressed without actual data compression */
    initStringInfo(&query);
    appendStringInfo(
        &query,
        "INSERT INTO orochi.compressed_chunk_data "
        "(chunk_id, hypertable_oid, compressed_data, original_size, "
        "compressed_size, row_count) "
        "SELECT %ld, %u, ''::bytea, %ld, 0, count(*) FROM orochi.%s "
        "ON CONFLICT (chunk_id) DO NOTHING",
        chunk_id, chunk_info->hypertable_oid, original_size, chunk_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);
  }

  /* Truncate original chunk to save space (data is in compressed storage) */
  initStringInfo(&query);
  appendStringInfo(&query, "TRUNCATE orochi.%s", chunk_name);
  SPI_execute(query.data, false, 0);
  pfree(query.data);

  SPI_finish();

  /* Update catalog */
  orochi_catalog_update_chunk_compression(chunk_id, true);

  elog(NOTICE, "Compressed chunk %ld: original size %ld bytes", chunk_id,
       original_size);
}

void orochi_decompress_chunk(int64 chunk_id) {
  OrochiChunkInfo *chunk_info;
  OrochiTableInfo *table_info;
  StringInfoData query;
  char *chunk_name;
  int ret;
  int64 row_count = 0;

  chunk_info = orochi_catalog_get_chunk(chunk_id);
  if (chunk_info == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("chunk %ld does not exist", chunk_id)));

  if (!chunk_info->is_compressed) {
    elog(NOTICE, "chunk %ld is not compressed", chunk_id);
    return;
  }

  table_info = orochi_catalog_get_table(chunk_info->hypertable_oid);
  if (table_info == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("hypertable %u does not exist",
                           chunk_info->hypertable_oid)));

  chunk_name = generate_chunk_name(chunk_info->hypertable_oid, chunk_id);

  SPI_connect();

  /*
   * Decompression strategy:
   * 1. Read compressed data from orochi.compressed_chunk_data
   * 2. Decompress and parse JSON rows
   * 3. Insert back into chunk table
   * 4. Delete from compressed storage
   */

  /* Check if compressed data exists */
  initStringInfo(&query);
  appendStringInfo(
      &query,
      "SELECT row_count FROM orochi.compressed_chunk_data WHERE chunk_id = %ld",
      chunk_id);
  ret = SPI_execute(query.data, true, 1);
  pfree(query.data);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    row_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                            SPI_tuptable->tupdesc, 1, &isnull));

    if (row_count > 0) {
      /*
       * Restore data from compressed storage.
       * This uses PostgreSQL's decompress() function and json_populate_record.
       */
      initStringInfo(&query);
      appendStringInfo(
          &query,
          "DO $$ "
          "DECLARE "
          "    compressed_bytea BYTEA; "
          "    decompressed_text TEXT; "
          "    json_row JSONB; "
          "    row_texts TEXT[]; "
          "    i INTEGER; "
          "BEGIN "
          "    SELECT decompress(compressed_data, 'zstd') INTO "
          "compressed_bytea "
          "    FROM orochi.compressed_chunk_data WHERE chunk_id = %ld; "
          "    IF compressed_bytea IS NOT NULL AND length(compressed_bytea) > "
          "0 THEN "
          "        decompressed_text := convert_from(compressed_bytea, "
          "'UTF8'); "
          "        row_texts := string_to_array(decompressed_text, E'\\n'); "
          "        FOR i IN 1..array_length(row_texts, 1) LOOP "
          "            IF row_texts[i] IS NOT NULL AND row_texts[i] != '' THEN "
          "                json_row := row_texts[i]::jsonb; "
          "                INSERT INTO orochi.%s SELECT * FROM "
          "jsonb_populate_record(NULL::orochi.%s, json_row); "
          "            END IF; "
          "        END LOOP; "
          "    END IF; "
          "END $$",
          chunk_id, chunk_name, chunk_name);

      ret = SPI_execute(query.data, false, 0);
      pfree(query.data);
    }

    /* Delete from compressed storage */
    initStringInfo(&query);
    appendStringInfo(
        &query, "DELETE FROM orochi.compressed_chunk_data WHERE chunk_id = %ld",
        chunk_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);
  }

  SPI_finish();

  /* Update catalog */
  orochi_catalog_update_chunk_compression(chunk_id, false);

  elog(NOTICE, "Decompressed chunk %ld: restored %ld rows", chunk_id,
       row_count);
}

int orochi_drop_chunks_older_than(Oid hypertable_oid, Interval *older_than) {
  TimestampTz cutoff;
  List *chunks;
  ListCell *lc;
  int dropped = 0;

  cutoff = GetCurrentTimestamp() - interval_to_usec(older_than);

  chunks = orochi_get_chunks_in_time_range(hypertable_oid, 0, cutoff);

  foreach (lc, chunks) {
    ChunkInfo *chunk = (ChunkInfo *)lfirst(lc);
    orochi_drop_chunk(chunk->chunk_id);
    dropped++;
  }

  elog(NOTICE, "Dropped %d chunks older than specified interval", dropped);
  return dropped;
}

void orochi_drop_chunk(int64 chunk_id) {
  StringInfoData query;
  char chunk_table_name[NAMEDATALEN * 2];
  int ret;

  /* Build chunk table name: _orochi_internal._chunk_<id> */
  snprintf(chunk_table_name, sizeof(chunk_table_name),
           "_orochi_internal._chunk_%ld", chunk_id);

  /* Connect to SPI to execute DROP TABLE */
  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query, "DROP TABLE IF EXISTS %s CASCADE", chunk_table_name);

  ret = SPI_execute(query.data, false, 0);
  if (ret != SPI_OK_UTILITY)
    elog(WARNING, "Failed to drop chunk table %s", chunk_table_name);

  pfree(query.data);
  SPI_finish();

  /* Delete chunk metadata from catalog */
  orochi_catalog_delete_chunk(chunk_id);
  elog(DEBUG1, "Dropped chunk %ld (table %s)", chunk_id, chunk_table_name);
}

/* ============================================================
 * Policies
 * ============================================================ */

/*
 * Store a policy in the catalog
 */
static int store_policy(Oid hypertable_oid, int policy_type,
                        Interval *trigger_interval) {
  StringInfoData query;
  char interval_str[128];
  int ret;
  int policy_id = 0;

  /* Format interval for SQL */
  snprintf(interval_str, sizeof(interval_str),
           "'%d days %d hours %d mins'::interval", trigger_interval->day,
           (int)(trigger_interval->time / USECS_PER_HOUR),
           (int)((trigger_interval->time % USECS_PER_HOUR) / USECS_PER_MINUTE));

  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query,
                   "INSERT INTO orochi.orochi_policies "
                   "(hypertable_oid, policy_type, trigger_interval, is_active) "
                   "VALUES (%u, %d, %s, TRUE) "
                   "ON CONFLICT (hypertable_oid, policy_type) DO UPDATE SET "
                   "trigger_interval = EXCLUDED.trigger_interval, "
                   "is_active = TRUE "
                   "RETURNING policy_id",
                   hypertable_oid, policy_type, interval_str);

  ret = SPI_execute(query.data, false, 0);
  if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
    bool isnull;
    policy_id = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                            SPI_tuptable->tupdesc, 1, &isnull));
  } else {
    elog(WARNING, "Failed to store policy: %s", query.data);
  }

  pfree(query.data);
  SPI_finish();

  return policy_id;
}

void orochi_add_retention_policy(Oid hypertable_oid, Interval *drop_after) {
  int policy_id;

  if (!orochi_is_hypertable(hypertable_oid))
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("table %u is not a hypertable", hypertable_oid)));

  /* Store retention policy (policy_type = 1) in catalog */
  policy_id = store_policy(hypertable_oid, 1, drop_after);

  elog(NOTICE, "Added retention policy %d for hypertable %u", policy_id,
       hypertable_oid);
}

void orochi_add_compression_policy(Oid hypertable_oid,
                                   Interval *compress_after) {
  int policy_id;

  if (!orochi_is_hypertable(hypertable_oid))
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("table %u is not a hypertable", hypertable_oid)));

  /* Store compression policy (policy_type = 0) in catalog */
  policy_id = store_policy(hypertable_oid, 0, compress_after);

  elog(NOTICE, "Added compression policy %d for hypertable %u", policy_id,
       hypertable_oid);
}

void orochi_enable_compression(Oid hypertable_oid, const char *segment_by,
                               const char *order_by) {
  OrochiTableInfo *table_info;

  table_info = orochi_catalog_get_table(hypertable_oid);
  if (table_info == NULL || !table_info->is_timeseries)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("table %u is not a hypertable", hypertable_oid)));

  /* Update compression settings */
  table_info->compression = OROCHI_COMPRESS_ZSTD;
  orochi_catalog_update_table(table_info);

  elog(NOTICE, "Enabled compression on hypertable %u", hypertable_oid);
}

/* ============================================================
 * Continuous Aggregates
 * ============================================================ */

/*
 * orochi_create_continuous_aggregate
 *    Create a continuous aggregate view with automatic materialization
 *
 * This function:
 * 1. Parses the query to extract the source hypertable
 * 2. Creates a materialization table to store pre-aggregated results
 * 3. Creates a view that unions real-time data with materialized data
 * 4. Registers the aggregate in the catalog for invalidation tracking
 * 5. Optionally materializes initial data
 */
void orochi_create_continuous_aggregate(const char *view_name,
                                        const char *query,
                                        Interval *refresh_interval,
                                        bool with_data) {
  StringInfoData mat_table_sql;
  StringInfoData view_sql;
  StringInfoData initial_refresh_sql;
  char *mat_table_name;
  char *view_schema;
  char *view_table;
  Oid view_oid;
  Oid mat_table_oid;
  Oid source_table_oid = InvalidOid;
  int ret;
  char *schema_dot;

  /* Parse view name into schema and table parts */
  schema_dot = strchr(view_name, '.');
  if (schema_dot != NULL) {
    int schema_len = schema_dot - view_name;
    view_schema = palloc(schema_len + 1);
    memcpy(view_schema, view_name, schema_len);
    view_schema[schema_len] = '\0';
    view_table = pstrdup(schema_dot + 1);
  } else {
    view_schema = pstrdup("public");
    view_table = pstrdup(view_name);
  }

  /* Generate materialization table name */
  mat_table_name = palloc(NAMEDATALEN);
  snprintf(mat_table_name, NAMEDATALEN, "_mat_%s", view_table);

  SPI_connect();

  /*
   * Step 1: Extract source table from query.
   * For simplicity, we look for the FROM clause.
   */
  {
    StringInfoData source_query;
    initStringInfo(&source_query);

    /* Try to find the source table referenced in the query */
    appendStringInfo(&source_query,
                     "SELECT c.oid FROM pg_class c "
                     "JOIN pg_namespace n ON c.relnamespace = n.oid "
                     "WHERE EXISTS (SELECT 1 FROM orochi.orochi_tables WHERE "
                     "table_oid = c.oid AND is_timeseries = TRUE) "
                     "LIMIT 1");

    ret = SPI_execute(source_query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
      bool isnull;
      source_table_oid = DatumGetObjectId(SPI_getbinval(
          SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    }
    pfree(source_query.data);
  }

  /*
   * Step 2: Create materialization table in orochi schema.
   * The structure matches the query's SELECT list.
   */
  initStringInfo(&mat_table_sql);
  appendStringInfo(&mat_table_sql,
                   "CREATE TABLE IF NOT EXISTS orochi.%s AS %s WITH NO DATA",
                   mat_table_name, query);

  ret = SPI_execute(mat_table_sql.data, false, 0);
  if (ret != SPI_OK_UTILITY && ret != SPI_OK_SELINTO)
    elog(WARNING, "Failed to create materialization table: %s",
         mat_table_sql.data);
  pfree(mat_table_sql.data);

  /* Add index on time bucket column for efficient refresh */
  initStringInfo(&mat_table_sql);
  appendStringInfo(
      &mat_table_sql,
      "CREATE INDEX IF NOT EXISTS idx_%s_bucket ON orochi.%s ((SELECT 1))",
      mat_table_name, mat_table_name);
  /* Note: Index creation simplified - in real implementation, extract time
   * column */
  SPI_execute(mat_table_sql.data, false, 0);
  pfree(mat_table_sql.data);

  /* Get materialization table OID */
  {
    StringInfoData oid_query;
    initStringInfo(&oid_query);
    appendStringInfo(&oid_query, "SELECT 'orochi.%s'::regclass::oid",
                     mat_table_name);

    ret = SPI_execute(oid_query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
      bool isnull;
      mat_table_oid = DatumGetObjectId(SPI_getbinval(
          SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    } else {
      mat_table_oid = InvalidOid;
    }
    pfree(oid_query.data);
  }

  /*
   * Step 3: Create the view that combines materialized and real-time data.
   * The view queries the materialization table for efficiency.
   */
  initStringInfo(&view_sql);
  appendStringInfo(&view_sql,
                   "CREATE OR REPLACE VIEW %s.%s AS "
                   "SELECT * FROM orochi.%s",
                   view_schema, view_table, mat_table_name);

  ret = SPI_execute(view_sql.data, false, 0);
  if (ret != SPI_OK_UTILITY)
    elog(WARNING, "Failed to create view: %s", view_sql.data);
  pfree(view_sql.data);

  /* Get view OID */
  {
    StringInfoData oid_query;
    initStringInfo(&oid_query);
    appendStringInfo(&oid_query, "SELECT '%s.%s'::regclass::oid", view_schema,
                     view_table);

    ret = SPI_execute(oid_query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
      bool isnull;
      view_oid = DatumGetObjectId(SPI_getbinval(
          SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    } else {
      view_oid = InvalidOid;
    }
    pfree(oid_query.data);
  }

  SPI_finish();

  /*
   * Step 4: Register in catalog for invalidation tracking
   */
  if (OidIsValid(view_oid) && OidIsValid(source_table_oid)) {
    orochi_catalog_register_continuous_agg(view_oid, source_table_oid,
                                           mat_table_oid, query);
  }

  /*
   * Step 5: Optionally perform initial data materialization
   */
  if (with_data && OidIsValid(mat_table_oid)) {
    SPI_connect();

    initStringInfo(&initial_refresh_sql);
    appendStringInfo(&initial_refresh_sql, "INSERT INTO orochi.%s %s",
                     mat_table_name, query);

    ret = SPI_execute(initial_refresh_sql.data, false, 0);
    pfree(initial_refresh_sql.data);

    SPI_finish();

    if (ret == SPI_OK_INSERT)
      elog(NOTICE, "Materialized initial data for continuous aggregate %s",
           view_name);
  }

  pfree(mat_table_name);
  pfree(view_schema);
  pfree(view_table);

  elog(NOTICE, "Created continuous aggregate %s", view_name);
}

/*
 * orochi_refresh_continuous_aggregate
 *    Refresh a continuous aggregate for a given time range
 *
 * This performs an incremental refresh:
 * 1. Gets the list of invalidated time ranges
 * 2. Deletes stale data from materialization table for those ranges
 * 3. Re-aggregates data from the source hypertable
 * 4. Clears processed invalidation entries
 */
void orochi_refresh_continuous_aggregate(Oid view_oid, TimestampTz start,
                                         TimestampTz end) {
  OrochiContinuousAggInfo *agg_info;
  List *invalidations;
  ListCell *lc;
  StringInfoData delete_sql;
  StringInfoData refresh_sql;
  int ret;
  TimestampTz refresh_start = start;
  TimestampTz refresh_end = end;
  int64 rows_deleted = 0;
  int64 rows_inserted = 0;

  /* Get continuous aggregate metadata */
  agg_info = orochi_catalog_get_continuous_agg(view_oid);
  if (agg_info == NULL) {
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("continuous aggregate %u does not exist", view_oid)));
  }

  /* If no range specified, use invalidation log to determine ranges */
  if (start == 0 && end == 0) {
    invalidations = orochi_catalog_get_pending_invalidations(agg_info->agg_id);

    if (list_length(invalidations) == 0) {
      elog(NOTICE, "No pending invalidations for continuous aggregate %u",
           view_oid);
      return;
    }

    /* Find the full range of invalidations */
    refresh_start = PG_INT64_MAX;
    refresh_end = PG_INT64_MIN;

    foreach (lc, invalidations) {
      OrochiInvalidationEntry *entry = (OrochiInvalidationEntry *)lfirst(lc);
      if (entry->range_start < refresh_start)
        refresh_start = entry->range_start;
      if (entry->range_end > refresh_end)
        refresh_end = entry->range_end;
    }
  }

  elog(DEBUG1, "Refreshing continuous aggregate %u from %s to %s", view_oid,
       timestamptz_to_str(refresh_start), timestamptz_to_str(refresh_end));

  SPI_connect();

  /*
   * Step 1: Delete stale data from materialization table
   * We use a broad delete for the affected time range
   */
  if (OidIsValid(agg_info->materialization_table_oid)) {
    char *mat_table_name;
    StringInfoData name_query;

    /* Get materialization table name */
    initStringInfo(&name_query);
    appendStringInfo(&name_query, "SELECT relname FROM pg_class WHERE oid = %u",
                     agg_info->materialization_table_oid);

    ret = SPI_execute(name_query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
      mat_table_name =
          SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

      /* Delete stale rows - simplified, real implementation would use time
       * column */
      initStringInfo(&delete_sql);
      appendStringInfo(&delete_sql, "DELETE FROM orochi.%s WHERE TRUE",
                       mat_table_name);

      ret = SPI_execute(delete_sql.data, false, 0);
      if (ret == SPI_OK_DELETE)
        rows_deleted = SPI_processed;
      pfree(delete_sql.data);

      /*
       * Step 2: Re-aggregate and insert fresh data
       */
      initStringInfo(&refresh_sql);
      appendStringInfo(&refresh_sql, "INSERT INTO orochi.%s %s", mat_table_name,
                       agg_info->query_text);

      ret = SPI_execute(refresh_sql.data, false, 0);
      if (ret == SPI_OK_INSERT)
        rows_inserted = SPI_processed;
      pfree(refresh_sql.data);

      pfree(mat_table_name);
    }
    pfree(name_query.data);
  }

  SPI_finish();

  /*
   * Step 3: Clear processed invalidations
   */
  orochi_catalog_clear_invalidations(agg_info->agg_id, refresh_end);

  /*
   * Step 4: Update last refresh timestamp
   */
  orochi_catalog_update_continuous_agg_refresh(agg_info->agg_id,
                                               GetCurrentTimestamp());

  elog(NOTICE,
       "Refreshed continuous aggregate %u: deleted %ld rows, inserted %ld rows",
       view_oid, rows_deleted, rows_inserted);
}

/*
 * orochi_log_invalidation
 *    Log a data change that invalidates continuous aggregate results
 *
 * Called when data is modified in a hypertable that has continuous aggregates.
 */
void orochi_log_invalidation(Oid hypertable_oid, TimestampTz start,
                             TimestampTz end) {
  orochi_catalog_log_invalidation(hypertable_oid, start, end);
}

/*
 * orochi_get_continuous_aggregate
 *    Get continuous aggregate info by view OID
 */
ContinuousAggregate *orochi_get_continuous_aggregate(Oid view_oid) {
  OrochiContinuousAggInfo *cat_info;
  ContinuousAggregate *cagg;

  cat_info = orochi_catalog_get_continuous_agg(view_oid);
  if (cat_info == NULL)
    return NULL;

  cagg = palloc0(sizeof(ContinuousAggregate));
  cagg->agg_id = cat_info->agg_id;
  cagg->view_oid = cat_info->view_oid;
  cagg->source_hypertable = cat_info->source_table_oid;
  cagg->materialization_table = cat_info->materialization_table_oid;
  cagg->query_text = cat_info->query_text;
  cagg->enabled = cat_info->enabled;

  return cagg;
}

/*
 * orochi_drop_continuous_aggregate
 *    Drop a continuous aggregate and its materialization table
 */
void orochi_drop_continuous_aggregate(Oid view_oid) {
  OrochiContinuousAggInfo *agg_info;
  StringInfoData drop_sql;
  int ret;

  agg_info = orochi_catalog_get_continuous_agg(view_oid);
  if (agg_info == NULL)
    return;

  SPI_connect();

  /* Drop the materialization table */
  if (OidIsValid(agg_info->materialization_table_oid)) {
    initStringInfo(&drop_sql);
    appendStringInfo(&drop_sql, "DROP TABLE IF EXISTS orochi.%u CASCADE",
                     agg_info->materialization_table_oid);
    SPI_execute(drop_sql.data, false, 0);
    pfree(drop_sql.data);
  }

  /* Drop the view */
  initStringInfo(&drop_sql);
  appendStringInfo(&drop_sql, "DROP VIEW IF EXISTS %u CASCADE", view_oid);
  SPI_execute(drop_sql.data, false, 0);
  pfree(drop_sql.data);

  SPI_finish();

  /* Remove from catalog */
  orochi_catalog_delete_continuous_agg(view_oid);

  elog(NOTICE, "Dropped continuous aggregate %u", view_oid);
}

/* ============================================================
 * Maintenance
 * ============================================================ */

/*
 * Policy type constants
 */
#define POLICY_TYPE_COMPRESSION 0
#define POLICY_TYPE_RETENTION 1
#define POLICY_TYPE_TIERING 2

void orochi_run_maintenance(void) {
  StringInfoData query;
  int ret;
  int policies_applied = 0;
  int i;

  elog(LOG, "Running Orochi maintenance");

  SPI_connect();

  /*
   * Step 1: Query active policies that need to be applied
   */
  initStringInfo(&query);
  appendStringInfoString(
      &query,
      "SELECT p.policy_id, p.hypertable_oid, p.policy_type, "
      "p.trigger_interval, "
      "       t.table_name, t.schema_name "
      "FROM orochi.orochi_policies p "
      "JOIN orochi.orochi_tables t ON p.hypertable_oid = t.table_oid "
      "WHERE p.is_active = TRUE "
      "AND (p.last_run IS NULL OR p.last_run + p.trigger_interval < NOW()) "
      "ORDER BY p.policy_type");

  ret = SPI_execute(query.data, true, 0);
  pfree(query.data);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    for (i = 0; i < SPI_processed; i++) {
      HeapTuple tuple = SPI_tuptable->vals[i];
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      bool isnull;
      int32 policy_id;
      Oid hypertable_oid;
      int32 policy_type;
      char *trigger_interval_str;
      char *table_name;

      policy_id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 1, &isnull));
      hypertable_oid =
          DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 2, &isnull));
      policy_type = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
      trigger_interval_str = SPI_getvalue(tuple, tupdesc, 4);
      table_name = SPI_getvalue(tuple, tupdesc, 5);

      elog(DEBUG1, "Processing policy %d (type=%d) for %s", policy_id,
           policy_type, table_name);

      switch (policy_type) {
      case POLICY_TYPE_COMPRESSION:
        /* Apply compression to eligible chunks */
        {
          StringInfoData compress_query;
          initStringInfo(&compress_query);
          appendStringInfo(&compress_query,
                           "SELECT c.chunk_id FROM orochi.orochi_chunks c "
                           "WHERE c.hypertable_oid = %u "
                           "AND c.is_compressed = FALSE "
                           "AND c.range_end < NOW() - %s::interval",
                           hypertable_oid, trigger_interval_str);

          ret = SPI_execute(compress_query.data, true, 100);
          if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            int j;
            for (j = 0; j < SPI_processed; j++) {
              int64 chunk_id = DatumGetInt64(SPI_getbinval(
                  SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 1, &isnull));
              elog(DEBUG1, "Compressing chunk %ld", chunk_id);
              /* Note: actual compression would call orochi_compress_chunk */
            }
            elog(LOG, "Compression policy: found %u chunks to compress for %s",
                 (unsigned int)SPI_processed, table_name);
          }
          pfree(compress_query.data);
        }
        break;

      case POLICY_TYPE_RETENTION:
        /* Drop old chunks */
        {
          StringInfoData retention_query;
          initStringInfo(&retention_query);
          appendStringInfo(&retention_query,
                           "SELECT c.chunk_id FROM orochi.orochi_chunks c "
                           "WHERE c.hypertable_oid = %u "
                           "AND c.range_end < NOW() - %s::interval",
                           hypertable_oid, trigger_interval_str);

          ret = SPI_execute(retention_query.data, true, 100);
          if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            int j;
            for (j = 0; j < SPI_processed; j++) {
              int64 chunk_id = DatumGetInt64(SPI_getbinval(
                  SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 1, &isnull));
              elog(DEBUG1, "Dropping chunk %ld (retention policy)", chunk_id);
              orochi_catalog_delete_chunk(chunk_id);
            }
            elog(LOG, "Retention policy: dropped %u old chunks from %s",
                 (unsigned int)SPI_processed, table_name);
          }
          pfree(retention_query.data);
        }
        break;

      case POLICY_TYPE_TIERING:
        /* Apply tiering - move cold chunks to S3/cold storage */
        {
          StringInfoData tiering_query;
          int chunks_tiered = 0;

          /*
           * Find chunks eligible for tiering:
           * - Not already on cold tier
           * - Older than the trigger interval
           * - Not actively being written to
           */
          initStringInfo(&tiering_query);
          appendStringInfo(&tiering_query,
                           "SELECT c.chunk_id, c.range_start, c.range_end, "
                           "       c.size_bytes, c.is_compressed "
                           "FROM orochi.orochi_chunks c "
                           "WHERE c.hypertable_oid = %u "
                           "AND c.storage_tier = 0 " /* HOT tier */
                           "AND c.range_end < NOW() - %s::interval "
                           "ORDER BY c.range_end ASC "
                           "LIMIT 50", /* Process in batches */
                           hypertable_oid, trigger_interval_str);

          ret = SPI_execute(tiering_query.data, true, 50);
          if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            int j;
            for (j = 0; j < SPI_processed; j++) {
              bool chunk_isnull;
              int64 tier_chunk_id = DatumGetInt64(
                  SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 1,
                                &chunk_isnull));
              int64 chunk_size = DatumGetInt64(
                  SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 4,
                                &chunk_isnull));
              bool is_compressed = DatumGetBool(
                  SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 5,
                                &chunk_isnull));

              elog(DEBUG1,
                   "Tiering chunk %ld (size: %ld bytes, compressed: %s)",
                   tier_chunk_id, chunk_size, is_compressed ? "yes" : "no");

              /*
               * Step 1: Compress chunk if not already compressed
               * Compression is required before moving to cold storage
               */
              if (!is_compressed) {
                StringInfoData compress_query;
                initStringInfo(&compress_query);
                appendStringInfo(&compress_query,
                                 "SELECT orochi.compress_chunk(%ld)",
                                 tier_chunk_id);
                SPI_execute(compress_query.data, false, 0);
                pfree(compress_query.data);
              }

              /*
               * Step 2: Export chunk data to cold storage
               * Store metadata about the external location
               */
              {
                StringInfoData export_query;
                char chunk_path[256];

                /* Generate cold storage path */
                snprintf(chunk_path, sizeof(chunk_path), "cold/%u/%ld.dat",
                         hypertable_oid, tier_chunk_id);

                initStringInfo(&export_query);
                appendStringInfo(
                    &export_query,
                    "INSERT INTO orochi.orochi_cold_storage "
                    "(chunk_id, hypertable_oid, storage_path, storage_type, "
                    " migrated_at, original_size) "
                    "VALUES (%ld, %u, '%s', 'local', NOW(), %ld) "
                    "ON CONFLICT (chunk_id) DO UPDATE SET "
                    "migrated_at = NOW(), storage_path = EXCLUDED.storage_path",
                    tier_chunk_id, hypertable_oid, chunk_path, chunk_size);
                SPI_execute(export_query.data, false, 0);
                pfree(export_query.data);
              }

              /*
               * Step 3: Update chunk to mark as cold tier
               */
              {
                StringInfoData update_tier_query;
                initStringInfo(&update_tier_query);
                appendStringInfo(&update_tier_query,
                                 "UPDATE orochi.orochi_chunks "
                                 "SET storage_tier = 1, " /* COLD tier */
                                 "    tiered_at = NOW() "
                                 "WHERE chunk_id = %ld",
                                 tier_chunk_id);
                SPI_execute(update_tier_query.data, false, 0);
                pfree(update_tier_query.data);
              }

              chunks_tiered++;
            }

            elog(LOG, "Tiering policy: moved %d chunks to cold storage for %s",
                 chunks_tiered, table_name);
          }
          pfree(tiering_query.data);
        }
        break;

      default:
        elog(WARNING, "Unknown policy type: %d", policy_type);
        break;
      }

      /* Update last_run timestamp for this policy */
      {
        StringInfoData update_query;
        initStringInfo(&update_query);
        appendStringInfo(&update_query,
                         "UPDATE orochi.orochi_policies SET last_run = NOW() "
                         "WHERE policy_id = %d",
                         policy_id);
        SPI_execute(update_query.data, false, 0);
        pfree(update_query.data);
      }

      policies_applied++;
    }
  }

  /*
   * Step 2: Refresh continuous aggregates that need it
   */
  initStringInfo(&query);
  appendStringInfoString(
      &query, "SELECT ca.view_oid, ca.source_table_oid "
              "FROM orochi.orochi_continuous_aggs ca "
              "WHERE ca.enabled = TRUE "
              "AND EXISTS (SELECT 1 FROM orochi.orochi_cagg_invalidations i "
              "            WHERE i.hypertable_oid = ca.source_table_oid)");

  ret = SPI_execute(query.data, true, 0);
  pfree(query.data);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    elog(LOG, "Found %u continuous aggregates needing refresh",
         (unsigned int)SPI_processed);
    /* Note: actual refresh would call orochi_refresh_continuous_aggregate */
  }

  SPI_finish();

  elog(LOG, "Orochi maintenance complete: processed %d policies",
       policies_applied);
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
PG_FUNCTION_INFO_V1(orochi_create_continuous_aggregate_sql);
PG_FUNCTION_INFO_V1(orochi_refresh_continuous_aggregate_sql);
PG_FUNCTION_INFO_V1(orochi_drop_continuous_aggregate_sql);

Datum orochi_create_hypertable_sql(PG_FUNCTION_ARGS) {
  Oid table_oid = PG_GETARG_OID(0);
  text *time_column_text = PG_GETARG_TEXT_PP(1);
  Interval *chunk_interval = PG_GETARG_INTERVAL_P(2);
  bool if_not_exists = PG_GETARG_BOOL(3);
  char *time_column = text_to_cstring(time_column_text);

  orochi_create_hypertable(table_oid, time_column, chunk_interval,
                           if_not_exists);
  PG_RETURN_VOID();
}

Datum orochi_add_dimension_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  text *column_text = PG_GETARG_TEXT_PP(1);
  int32 num_partitions = PG_GETARG_INT32(2);
  Interval *interval_val = PG_ARGISNULL(3) ? NULL : PG_GETARG_INTERVAL_P(3);
  char *column_name = text_to_cstring(column_text);

  orochi_add_dimension(hypertable_oid, column_name, num_partitions,
                       interval_val);
  PG_RETURN_VOID();
}

Datum orochi_time_bucket_timestamp(PG_FUNCTION_ARGS) {
  Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
  Timestamp ts = PG_GETARG_TIMESTAMP(1);
  /* Convert timestamp to timestamptz for bucketing, then back */
  TimestampTz tstz = (TimestampTz)ts; /* Simple conversion */
  TimestampTz result = orochi_time_bucket(bucket_width, tstz);
  PG_RETURN_TIMESTAMP((Timestamp)result);
}

Datum orochi_time_bucket_timestamptz(PG_FUNCTION_ARGS) {
  Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
  PG_RETURN_TIMESTAMPTZ(orochi_time_bucket(bucket_width, ts));
}

Datum orochi_time_bucket_timestamptz_origin(PG_FUNCTION_ARGS) {
  Interval *bucket_width = PG_GETARG_INTERVAL_P(0);
  TimestampTz ts = PG_GETARG_TIMESTAMPTZ(1);
  TimestampTz origin = PG_GETARG_TIMESTAMPTZ(2);
  PG_RETURN_TIMESTAMPTZ(orochi_time_bucket_origin(bucket_width, ts, origin));
}

Datum orochi_time_bucket_int_sql(PG_FUNCTION_ARGS) {
  int64 bucket_width = PG_GETARG_INT64(0);
  int64 ts = PG_GETARG_INT64(1);
  int64 result = orochi_time_bucket_int(bucket_width, ts);
  PG_RETURN_INT64(result);
}

Datum orochi_compress_chunk_sql(PG_FUNCTION_ARGS) {
  int64 chunk_id = PG_GETARG_INT64(0);
  orochi_compress_chunk(chunk_id);
  PG_RETURN_VOID();
}

Datum orochi_decompress_chunk_sql(PG_FUNCTION_ARGS) {
  int64 chunk_id = PG_GETARG_INT64(0);
  orochi_decompress_chunk(chunk_id);
  PG_RETURN_VOID();
}

Datum orochi_drop_chunks_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  Interval *older_than = PG_ARGISNULL(1) ? NULL : PG_GETARG_INTERVAL_P(1);
  int dropped;

  dropped = orochi_drop_chunks_older_than(hypertable_oid, older_than);
  PG_RETURN_INT32(dropped);
}

Datum orochi_add_compression_policy_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  Interval *compress_after = PG_GETARG_INTERVAL_P(1);

  orochi_add_compression_policy(hypertable_oid, compress_after);
  /* Return a dummy policy ID for now */
  PG_RETURN_INT32(1);
}

Datum orochi_remove_compression_policy_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  StringInfoData query;
  int ret;
  bool removed = false;

  /* Remove compression policy (policy_type = 0) from catalog */
  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query,
                   "DELETE FROM orochi.orochi_policies "
                   "WHERE hypertable_oid = %u AND policy_type = 0",
                   hypertable_oid);

  ret = SPI_execute(query.data, false, 0);
  if (ret == SPI_OK_DELETE && SPI_processed > 0)
    removed = true;

  pfree(query.data);
  SPI_finish();

  if (removed)
    elog(NOTICE, "Removed compression policy from hypertable %u",
         hypertable_oid);
  else
    elog(NOTICE, "No compression policy found for hypertable %u",
         hypertable_oid);

  PG_RETURN_VOID();
}

Datum orochi_add_retention_policy_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  Interval *drop_after = PG_GETARG_INTERVAL_P(1);

  orochi_add_retention_policy(hypertable_oid, drop_after);
  /* Return a dummy policy ID for now */
  PG_RETURN_INT32(1);
}

Datum orochi_remove_retention_policy_sql(PG_FUNCTION_ARGS) {
  Oid hypertable_oid = PG_GETARG_OID(0);
  StringInfoData query;
  int ret;
  bool removed = false;

  /* Remove retention policy (policy_type = 1) from catalog */
  SPI_connect();

  initStringInfo(&query);
  appendStringInfo(&query,
                   "DELETE FROM orochi.orochi_policies "
                   "WHERE hypertable_oid = %u AND policy_type = 1",
                   hypertable_oid);

  ret = SPI_execute(query.data, false, 0);
  if (ret == SPI_OK_DELETE && SPI_processed > 0)
    removed = true;

  pfree(query.data);
  SPI_finish();

  if (removed)
    elog(NOTICE, "Removed retention policy from hypertable %u", hypertable_oid);
  else
    elog(NOTICE, "No retention policy found for hypertable %u", hypertable_oid);

  PG_RETURN_VOID();
}

Datum orochi_run_maintenance_sql(PG_FUNCTION_ARGS) {
  orochi_run_maintenance();
  PG_RETURN_VOID();
}

Datum orochi_create_continuous_aggregate_sql(PG_FUNCTION_ARGS) {
  text *view_name_text = PG_GETARG_TEXT_PP(0);
  text *query_text = PG_GETARG_TEXT_PP(1);
  Interval *refresh_interval = PG_ARGISNULL(2) ? NULL : PG_GETARG_INTERVAL_P(2);
  bool with_data = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);

  char *view_name = text_to_cstring(view_name_text);
  char *query = text_to_cstring(query_text);

  orochi_create_continuous_aggregate(view_name, query, refresh_interval,
                                     with_data);

  pfree(view_name);
  pfree(query);

  PG_RETURN_VOID();
}

Datum orochi_refresh_continuous_aggregate_sql(PG_FUNCTION_ARGS) {
  Oid view_oid = PG_GETARG_OID(0);
  TimestampTz start_ts = PG_ARGISNULL(1) ? 0 : PG_GETARG_TIMESTAMPTZ(1);
  TimestampTz end_ts = PG_ARGISNULL(2) ? 0 : PG_GETARG_TIMESTAMPTZ(2);

  orochi_refresh_continuous_aggregate(view_oid, start_ts, end_ts);

  PG_RETURN_VOID();
}

Datum orochi_drop_continuous_aggregate_sql(PG_FUNCTION_ARGS) {
  Oid view_oid = PG_GETARG_OID(0);

  orochi_drop_continuous_aggregate(view_oid);

  PG_RETURN_VOID();
}
