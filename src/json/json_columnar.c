/*-------------------------------------------------------------------------
 *
 * json_columnar.c
 *    Orochi DB columnar storage for JSON/JSONB data implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "../orochi.h"
#include "../storage/compression.h"
#include "json_columnar.h"
#include "json_index.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Memory context for columnar operations */
static MemoryContext JsonColumnarContext = NULL;

/* Sequence for column IDs */
static int32 NextColumnId = 1;

/* Forward declarations */
static void ensure_columnar_context(void);
static Datum extract_jsonb_path_value(Jsonb *jb, const char *path, Oid target_type,
                                      bool *isnull);
static Oid infer_path_type(Jsonb *jb, const char *path);
static void store_column_metadata(JsonExtractedColumn *col);
static JsonExtractedColumn *load_column_metadata(int32 column_id);

/* ============================================================
 * Utility Functions
 * ============================================================ */

static void
ensure_columnar_context(void)
{
    if (JsonColumnarContext == NULL)
    {
        JsonColumnarContext = AllocSetContextCreate(TopMemoryContext,
                                                    "JsonColumnarContext",
                                                    ALLOCSET_DEFAULT_SIZES);
    }
}

static Datum
extract_jsonb_path_value(Jsonb *jb, const char *path, Oid target_type,
                         bool *isnull)
{
    Datum       path_datum;
    Datum       result_datum;
    Jsonb      *result_jb;
    text       *result_text;

    if (jb == NULL)
    {
        *isnull = true;
        return (Datum) 0;
    }

    /* Extract using jsonb_path_query_first or ->> operator */
    path_datum = CStringGetTextDatum(path);

    PG_TRY();
    {
        /* Use jsonb ->> 'path' for text extraction */
        result_datum = DirectFunctionCall2(jsonb_object_field_text,
                                           JsonbPGetDatum(jb),
                                           path_datum);

        if (result_datum == (Datum) 0)
        {
            *isnull = true;
            return (Datum) 0;
        }

        result_text = DatumGetTextPP(result_datum);

        /* Cast to target type if needed */
        switch (target_type)
        {
            case TEXTOID:
                *isnull = false;
                return result_datum;

            case INT4OID:
                *isnull = false;
                return DirectFunctionCall1(int4in,
                    CStringGetDatum(text_to_cstring(result_text)));

            case INT8OID:
                *isnull = false;
                return DirectFunctionCall1(int8in,
                    CStringGetDatum(text_to_cstring(result_text)));

            case FLOAT8OID:
                *isnull = false;
                return DirectFunctionCall1(float8in,
                    CStringGetDatum(text_to_cstring(result_text)));

            case BOOLOID:
            {
                char *val = text_to_cstring(result_text);
                *isnull = false;
                return BoolGetDatum(pg_strcasecmp(val, "true") == 0 ||
                                    pg_strcasecmp(val, "t") == 0 ||
                                    pg_strcasecmp(val, "1") == 0);
            }

            case TIMESTAMPTZOID:
                *isnull = false;
                return DirectFunctionCall1(timestamptz_in,
                    CStringGetDatum(text_to_cstring(result_text)));

            case JSONBOID:
                /* Return the original JSONB sub-document */
                result_jb = DatumGetJsonbP(DirectFunctionCall2(jsonb_object_field,
                                                               JsonbPGetDatum(jb),
                                                               path_datum));
                *isnull = false;
                return JsonbPGetDatum(result_jb);

            default:
                *isnull = false;
                return result_datum;
        }
    }
    PG_CATCH();
    {
        FlushErrorState();
        *isnull = true;
        return (Datum) 0;
    }
    PG_END_TRY();

    *isnull = true;
    return (Datum) 0;
}

static Oid
infer_path_type(Jsonb *jb, const char *path)
{
    JsonPathType type;

    type = json_index_get_path_type(jb, path);

    switch (type)
    {
        case JSON_PATH_STRING:
            return TEXTOID;
        case JSON_PATH_NUMBER:
            return FLOAT8OID;
        case JSON_PATH_BOOLEAN:
            return BOOLOID;
        case JSON_PATH_ARRAY:
        case JSON_PATH_OBJECT:
            return JSONBOID;
        default:
            return TEXTOID;
    }
}

static void
store_column_metadata(JsonExtractedColumn *col)
{
    int ret;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    /* Create catalog table if not exists */
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS orochi.orochi_json_columns ("
        "  column_id INTEGER PRIMARY KEY,"
        "  table_oid OID NOT NULL,"
        "  source_attnum SMALLINT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  target_type OID NOT NULL,"
        "  storage_type INTEGER NOT NULL DEFAULT 0,"
        "  state INTEGER NOT NULL DEFAULT 0,"
        "  row_count BIGINT NOT NULL DEFAULT 0,"
        "  null_count BIGINT NOT NULL DEFAULT 0,"
        "  distinct_count BIGINT NOT NULL DEFAULT 0,"
        "  storage_size BIGINT NOT NULL DEFAULT 0,"
        "  compression_ratio DOUBLE PRECISION NOT NULL DEFAULT 1.0,"
        "  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "  last_refreshed TIMESTAMPTZ,"
        "  UNIQUE(table_oid, source_attnum, path)"
        ")",
        false, 0);

    /* Insert or update column metadata */
    SPI_execute_with_args(
        "INSERT INTO orochi.orochi_json_columns "
        "(column_id, table_oid, source_attnum, path, target_type, "
        " storage_type, state, row_count, null_count, distinct_count, "
        " storage_size, compression_ratio, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) "
        "ON CONFLICT (table_oid, source_attnum, path) DO UPDATE SET "
        "  storage_type = EXCLUDED.storage_type,"
        "  state = EXCLUDED.state,"
        "  row_count = EXCLUDED.row_count,"
        "  null_count = EXCLUDED.null_count,"
        "  distinct_count = EXCLUDED.distinct_count,"
        "  storage_size = EXCLUDED.storage_size,"
        "  compression_ratio = EXCLUDED.compression_ratio,"
        "  last_refreshed = NOW()",
        13,
        (Oid[]){INT4OID, OIDOID, INT2OID, TEXTOID, OIDOID,
                INT4OID, INT4OID, INT8OID, INT8OID, INT8OID,
                INT8OID, FLOAT8OID, TIMESTAMPTZOID},
        (Datum[]){
            Int32GetDatum(col->column_id),
            ObjectIdGetDatum(col->table_oid),
            Int16GetDatum(col->source_attnum),
            CStringGetTextDatum(col->path),
            ObjectIdGetDatum(col->target_type),
            Int32GetDatum(col->storage_type),
            Int32GetDatum(col->state),
            Int64GetDatum(col->row_count),
            Int64GetDatum(col->null_count),
            Int64GetDatum(col->distinct_count),
            Int64GetDatum(col->storage_size),
            Float8GetDatum(col->compression_ratio),
            TimestampTzGetDatum(col->created_at)
        },
        NULL, false, 0);

    SPI_finish();
}

static JsonExtractedColumn *
load_column_metadata(int32 column_id)
{
    JsonExtractedColumn *col;
    int         ret;
    bool        isnull;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute_with_args(
        "SELECT column_id, table_oid, source_attnum, path, target_type, "
        "       storage_type, state, row_count, null_count, distinct_count, "
        "       storage_size, compression_ratio, created_at, last_refreshed "
        "FROM orochi.orochi_json_columns WHERE column_id = $1",
        1,
        (Oid[]){INT4OID},
        (Datum[]){Int32GetDatum(column_id)},
        NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        SPI_finish();
        return NULL;
    }

    ensure_columnar_context();

    col = MemoryContextAllocZero(JsonColumnarContext, sizeof(JsonExtractedColumn));

    col->column_id = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                  SPI_tuptable->tupdesc, 1, &isnull));
    col->table_oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0],
                                                     SPI_tuptable->tupdesc, 2, &isnull));
    col->source_attnum = DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[0],
                                                      SPI_tuptable->tupdesc, 3, &isnull));
    col->path = MemoryContextStrdup(JsonColumnarContext,
                                    TextDatumGetCString(SPI_getbinval(SPI_tuptable->vals[0],
                                                                       SPI_tuptable->tupdesc, 4, &isnull)));
    col->target_type = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0],
                                                       SPI_tuptable->tupdesc, 5, &isnull));
    col->storage_type = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                     SPI_tuptable->tupdesc, 6, &isnull));
    col->state = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                              SPI_tuptable->tupdesc, 7, &isnull));
    col->row_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                  SPI_tuptable->tupdesc, 8, &isnull));
    col->null_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                   SPI_tuptable->tupdesc, 9, &isnull));
    col->distinct_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                       SPI_tuptable->tupdesc, 10, &isnull));
    col->storage_size = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                     SPI_tuptable->tupdesc, 11, &isnull));
    col->compression_ratio = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[0],
                                                           SPI_tuptable->tupdesc, 12, &isnull));
    col->created_at = DatumGetTimestampTz(SPI_getbinval(SPI_tuptable->vals[0],
                                                         SPI_tuptable->tupdesc, 13, &isnull));

    SPI_finish();
    return col;
}

/* ============================================================
 * Column Extraction Implementation
 * ============================================================ */

List *
json_columnar_extract_columns(Oid table_oid, int16 source_attnum,
                              const char **paths, int path_count)
{
    List       *result = NIL;
    Relation    relation;
    TableScanDesc scan;
    TupleTableSlot *slot;
    MemoryContext old_context;
    int64       total_rows = 0;

    if (path_count <= 0 || paths == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("at least one path must be specified")));

    if (path_count > JSON_COLUMNAR_MAX_EXTRACTED_PATHS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("maximum %d paths can be extracted",
                        JSON_COLUMNAR_MAX_EXTRACTED_PATHS)));

    ensure_columnar_context();
    old_context = MemoryContextSwitchTo(JsonColumnarContext);

    /* Create extracted column metadata for each path */
    for (int i = 0; i < path_count; i++)
    {
        JsonExtractedColumn *col;

        if (!json_index_is_valid_path(paths[i]))
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid JSON path: %s", paths[i])));

        col = palloc0(sizeof(JsonExtractedColumn));
        col->column_id = NextColumnId++;
        col->table_oid = table_oid;
        col->source_attnum = source_attnum;
        col->path = pstrdup(paths[i]);
        col->target_type = TEXTOID;  /* Will be inferred */
        col->storage_type = JSON_COLUMNAR_RAW;
        col->state = JSON_COLUMN_ACTIVE;
        col->row_count = 0;
        col->null_count = 0;
        col->distinct_count = 0;
        col->storage_size = 0;
        col->compression_ratio = 1.0;
        col->created_at = GetCurrentTimestamp();

        result = lappend(result, col);
    }

    /* Scan table to extract values and infer types */
    relation = table_open(table_oid, AccessShareLock);
    slot = table_slot_create(relation, NULL);
    scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL);

    /* Sample first rows to infer types */
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot) &&
           total_rows < 100)
    {
        bool        isnull;
        Datum       json_datum;
        Jsonb      *jb;

        json_datum = slot_getattr(slot, source_attnum, &isnull);
        if (isnull)
        {
            total_rows++;
            continue;
        }

        jb = DatumGetJsonbP(json_datum);

        /* Infer type for each path */
        ListCell *lc;
        foreach(lc, result)
        {
            JsonExtractedColumn *col = lfirst(lc);
            if (col->target_type == TEXTOID)
            {
                Oid inferred = infer_path_type(jb, col->path);
                if (inferred != TEXTOID)
                    col->target_type = inferred;
            }
        }

        total_rows++;
    }

    table_endscan(scan);

    /* Now perform full extraction */
    scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL);
    total_rows = 0;

    /* Create data storage table for extracted columns */
    {
        StringInfoData sql;
        int ret;

        initStringInfo(&sql);
        appendStringInfo(&sql,
            "CREATE TABLE IF NOT EXISTS orochi.orochi_json_data_%u_%d ("
            "  row_id BIGINT NOT NULL,"
            "  chunk_id BIGINT NOT NULL DEFAULT 0",
            table_oid, source_attnum);

        ListCell *lc;
        int col_num = 0;
        foreach(lc, result)
        {
            JsonExtractedColumn *col = lfirst(lc);
            char *type_name = format_type_be(col->target_type);
            appendStringInfo(&sql, ", col_%d %s", col_num, type_name);
            col_num++;
        }

        appendStringInfo(&sql,
            ", PRIMARY KEY (chunk_id, row_id)"
            ")");

        ret = SPI_connect();
        if (ret == SPI_OK_CONNECT)
        {
            SPI_execute(sql.data, false, 0);
            SPI_finish();
        }
        pfree(sql.data);
    }

    /* Extract data */
    {
        StringInfoData insert_sql;
        int ret;
        int64 batch_size = 0;
        int64 chunk_id = 0;

        ret = SPI_connect();
        if (ret != SPI_OK_CONNECT)
        {
            table_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            table_close(relation, AccessShareLock);
            MemoryContextSwitchTo(old_context);
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("SPI_connect failed")));
        }

        initStringInfo(&insert_sql);

        while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
        {
            bool        src_isnull;
            Datum       json_datum;
            Jsonb      *jb;

            json_datum = slot_getattr(slot, source_attnum, &src_isnull);
            if (src_isnull)
            {
                total_rows++;
                continue;
            }

            jb = DatumGetJsonbP(json_datum);

            /* Build INSERT statement */
            resetStringInfo(&insert_sql);
            appendStringInfo(&insert_sql,
                "INSERT INTO orochi.orochi_json_data_%u_%d (row_id, chunk_id",
                table_oid, source_attnum);

            ListCell *lc;
            int col_num = 0;
            foreach(lc, result)
            {
                appendStringInfo(&insert_sql, ", col_%d", col_num++);
            }

            appendStringInfo(&insert_sql, ") VALUES (%ld, %ld",
                             total_rows, chunk_id);

            col_num = 0;
            foreach(lc, result)
            {
                JsonExtractedColumn *col = lfirst(lc);
                bool value_isnull;
                Datum value;

                value = extract_jsonb_path_value(jb, col->path,
                                                 col->target_type, &value_isnull);

                if (value_isnull)
                {
                    appendStringInfo(&insert_sql, ", NULL");
                    col->null_count++;
                }
                else
                {
                    char *value_str;

                    switch (col->target_type)
                    {
                        case TEXTOID:
                            value_str = TextDatumGetCString(value);
                            appendStringInfo(&insert_sql, ", '%s'",
                                             value_str);  /* TODO: proper escaping */
                            break;
                        case INT4OID:
                            appendStringInfo(&insert_sql, ", %d",
                                             DatumGetInt32(value));
                            break;
                        case INT8OID:
                            appendStringInfo(&insert_sql, ", %ld",
                                             DatumGetInt64(value));
                            break;
                        case FLOAT8OID:
                            appendStringInfo(&insert_sql, ", %g",
                                             DatumGetFloat8(value));
                            break;
                        case BOOLOID:
                            appendStringInfo(&insert_sql, ", %s",
                                             DatumGetBool(value) ? "true" : "false");
                            break;
                        default:
                            appendStringInfo(&insert_sql, ", NULL");
                    }
                }

                col->row_count++;
                col_num++;
            }

            appendStringInfo(&insert_sql, ")");

            SPI_execute(insert_sql.data, false, 0);

            total_rows++;
            batch_size++;

            if (batch_size >= JSON_COLUMNAR_CHUNK_SIZE)
            {
                chunk_id++;
                batch_size = 0;
            }

            /* Allow interrupts */
            if (total_rows % 10000 == 0)
                CHECK_FOR_INTERRUPTS();
        }

        pfree(insert_sql.data);
        SPI_finish();
    }

    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    table_close(relation, AccessShareLock);

    /* Store metadata for each column */
    {
        ListCell *lc;
        foreach(lc, result)
        {
            JsonExtractedColumn *col = lfirst(lc);
            store_column_metadata(col);
        }
    }

    MemoryContextSwitchTo(old_context);

    elog(NOTICE, "Extracted %d columns from %ld rows",
         path_count, total_rows);

    return result;
}

List *
json_columnar_auto_extract(Oid table_oid, int16 source_attnum, int max_columns)
{
    JsonPathStatsCollection *stats;
    const char **paths;
    int         path_count = 0;
    List       *result;

    if (max_columns <= 0)
        max_columns = 10;

    if (max_columns > JSON_COLUMNAR_MAX_EXTRACTED_PATHS)
        max_columns = JSON_COLUMNAR_MAX_EXTRACTED_PATHS;

    /* Analyze paths to find candidates */
    stats = json_index_analyze_paths(table_oid, source_attnum,
                                     JSON_INDEX_SAMPLE_SIZE);
    if (stats == NULL || stats->path_count == 0)
        return NIL;

    /* Sort by access count and selectivity */
    for (int i = 0; i < stats->path_count - 1; i++)
    {
        for (int j = i + 1; j < stats->path_count; j++)
        {
            double score_i = stats->paths[i].access_count * (1.0 - stats->paths[i].selectivity);
            double score_j = stats->paths[j].access_count * (1.0 - stats->paths[j].selectivity);
            if (score_j > score_i)
            {
                JsonPathStats temp = stats->paths[i];
                stats->paths[i] = stats->paths[j];
                stats->paths[j] = temp;
            }
        }
    }

    /* Select top paths */
    paths = palloc(max_columns * sizeof(char *));
    for (int i = 0; i < stats->path_count && path_count < max_columns; i++)
    {
        JsonPathStats *ps = &stats->paths[i];

        /* Skip array/object types - they don't extract well */
        if (ps->value_type == JSON_PATH_ARRAY ||
            ps->value_type == JSON_PATH_OBJECT)
            continue;

        /* Skip paths with very low selectivity (too unique) */
        if (ps->selectivity > 0.9)
            continue;

        paths[path_count++] = ps->path;
    }

    if (path_count == 0)
    {
        pfree(paths);
        json_index_free_stats(stats);
        return NIL;
    }

    result = json_columnar_extract_columns(table_oid, source_attnum,
                                           paths, path_count);

    pfree(paths);
    json_index_free_stats(stats);

    return result;
}

void
json_columnar_drop_column(int32 column_id)
{
    JsonExtractedColumn *col;
    int         ret;

    col = load_column_metadata(column_id);
    if (col == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("column %d does not exist", column_id)));

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    /* Delete from metadata table */
    SPI_execute_with_args(
        "DELETE FROM orochi.orochi_json_columns WHERE column_id = $1",
        1,
        (Oid[]){INT4OID},
        (Datum[]){Int32GetDatum(column_id)},
        NULL, false, 0);

    SPI_finish();

    json_columnar_free_column(col);
}

void
json_columnar_refresh_column(int32 column_id)
{
    JsonExtractedColumn *col;
    const char *paths[1];

    col = load_column_metadata(column_id);
    if (col == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("column %d does not exist", column_id)));

    /* Mark as stale during refresh */
    col->state = JSON_COLUMN_STALE;
    store_column_metadata(col);

    /* Re-extract the column */
    paths[0] = col->path;
    json_columnar_extract_columns(col->table_oid, col->source_attnum,
                                  paths, 1);

    json_columnar_free_column(col);
}

void
json_columnar_refresh_all(Oid table_oid, int16 source_attnum)
{
    List       *columns;
    ListCell   *lc;

    columns = json_columnar_get_columns(table_oid, source_attnum);

    foreach(lc, columns)
    {
        JsonExtractedColumn *col = lfirst(lc);
        json_columnar_refresh_column(col->column_id);
    }

    list_free_deep(columns);
}

List *
json_columnar_get_columns(Oid table_oid, int16 source_attnum)
{
    List       *result = NIL;
    int         ret;
    uint64      i;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute_with_args(
        "SELECT column_id FROM orochi.orochi_json_columns "
        "WHERE table_oid = $1 AND source_attnum = $2 "
        "ORDER BY column_id",
        2,
        (Oid[]){OIDOID, INT2OID},
        (Datum[]){ObjectIdGetDatum(table_oid), Int16GetDatum(source_attnum)},
        NULL, true, 0);

    if (ret == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            bool isnull;
            int32 column_id;
            JsonExtractedColumn *col;

            column_id = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],
                                                    SPI_tuptable->tupdesc, 1, &isnull));

            col = load_column_metadata(column_id);
            if (col != NULL)
                result = lappend(result, col);
        }
    }

    SPI_finish();
    return result;
}

JsonExtractedColumn *
json_columnar_get_column(int32 column_id)
{
    return load_column_metadata(column_id);
}

void
json_columnar_free_column(JsonExtractedColumn *col)
{
    if (col == NULL)
        return;

    if (col->path)
        pfree(col->path);

    if (col->dict_values)
    {
        for (int i = 0; i < col->dict_size; i++)
        {
            if (col->dict_values[i])
                pfree(col->dict_values[i]);
        }
        pfree(col->dict_values);
    }

    pfree(col);
}

/* ============================================================
 * Write Operations Implementation
 * ============================================================ */

JsonColumnarWriteState *
json_columnar_begin_write(Oid table_oid, int16 source_attnum)
{
    JsonColumnarWriteState *state;
    List       *columns;
    ListCell   *lc;
    int         col_idx = 0;

    ensure_columnar_context();

    state = MemoryContextAllocZero(JsonColumnarContext,
                                   sizeof(JsonColumnarWriteState));
    state->table_oid = table_oid;
    state->source_attnum = source_attnum;
    state->write_context = AllocSetContextCreate(JsonColumnarContext,
                                                 "JsonColumnarWrite",
                                                 ALLOCSET_DEFAULT_SIZES);

    /* Load existing extracted columns */
    columns = json_columnar_get_columns(table_oid, source_attnum);
    state->extracted_columns = columns;
    state->num_columns = list_length(columns);

    if (state->num_columns > 0)
    {
        state->column_buffers = MemoryContextAllocZero(state->write_context,
            state->num_columns * sizeof(struct ColumnBuffer));

        foreach(lc, columns)
        {
            JsonExtractedColumn *col = lfirst(lc);
            state->column_buffers[col_idx].column_id = col->column_id;
            state->column_buffers[col_idx].capacity = JSON_COLUMNAR_CHUNK_SIZE;
            state->column_buffers[col_idx].values =
                palloc(JSON_COLUMNAR_CHUNK_SIZE * sizeof(Datum));
            state->column_buffers[col_idx].nulls =
                palloc(JSON_COLUMNAR_CHUNK_SIZE * sizeof(bool));
            state->column_buffers[col_idx].count = 0;
            col_idx++;
        }
    }

    state->rows_written = 0;
    state->chunks_created = 0;

    return state;
}

void
json_columnar_write_value(JsonColumnarWriteState *state,
                          Jsonb *value, int64 row_id)
{
    ListCell   *lc;
    int         col_idx = 0;

    foreach(lc, state->extracted_columns)
    {
        JsonExtractedColumn *col = lfirst(lc);
        struct ColumnBuffer *buf = &state->column_buffers[col_idx];
        bool isnull;
        Datum extracted;

        extracted = extract_jsonb_path_value(value, col->path,
                                             col->target_type, &isnull);

        buf->values[buf->count] = extracted;
        buf->nulls[buf->count] = isnull;
        buf->count++;

        if (buf->count >= buf->capacity)
        {
            /* Flush this column buffer */
            json_columnar_flush_writes(state);
        }

        col_idx++;
    }

    state->rows_written++;
}

void
json_columnar_flush_writes(JsonColumnarWriteState *state)
{
    /* Flush all column buffers to storage */
    for (int i = 0; i < state->num_columns; i++)
    {
        struct ColumnBuffer *buf = &state->column_buffers[i];
        if (buf->count == 0)
            continue;

        /* In production, this would write to the columnar storage table */
        buf->count = 0;
    }

    state->chunks_created++;
}

void
json_columnar_end_write(JsonColumnarWriteState *state)
{
    if (state == NULL)
        return;

    /* Flush any remaining data */
    json_columnar_flush_writes(state);

    /* Free column buffers */
    for (int i = 0; i < state->num_columns; i++)
    {
        if (state->column_buffers[i].values)
            pfree(state->column_buffers[i].values);
        if (state->column_buffers[i].nulls)
            pfree(state->column_buffers[i].nulls);
    }

    if (state->column_buffers)
        pfree(state->column_buffers);

    /* Free extracted columns list */
    if (state->extracted_columns)
    {
        ListCell *lc;
        foreach(lc, state->extracted_columns)
        {
            json_columnar_free_column(lfirst(lc));
        }
        list_free(state->extracted_columns);
    }

    if (state->write_context)
        MemoryContextDelete(state->write_context);

    pfree(state);
}

/* ============================================================
 * Read Operations Implementation
 * ============================================================ */

JsonColumnarReadState *
json_columnar_begin_read(Oid table_oid, int16 source_attnum, List *column_ids)
{
    JsonColumnarReadState *state;

    ensure_columnar_context();

    state = MemoryContextAllocZero(JsonColumnarContext,
                                   sizeof(JsonColumnarReadState));
    state->table_oid = table_oid;
    state->source_attnum = source_attnum;
    state->columns_to_read = list_copy(column_ids);
    state->read_context = AllocSetContextCreate(JsonColumnarContext,
                                                "JsonColumnarRead",
                                                ALLOCSET_DEFAULT_SIZES);

    state->current_row = 0;
    state->current_chunk_index = 0;
    state->current_chunk = NULL;
    state->rows_read = 0;
    state->chunks_read = 0;

    return state;
}

bool
json_columnar_read_value(JsonColumnarReadState *state,
                         int32 column_id, int64 row_id,
                         Datum *value, bool *isnull)
{
    int         ret;
    StringInfoData sql;
    int         col_idx = -1;
    ListCell   *lc;

    /* Find column index */
    int idx = 0;
    foreach(lc, state->columns_to_read)
    {
        if (lfirst_int(lc) == column_id)
        {
            col_idx = idx;
            break;
        }
        idx++;
    }

    if (col_idx < 0)
    {
        *isnull = true;
        return false;
    }

    /* Query the data table */
    initStringInfo(&sql);
    appendStringInfo(&sql,
        "SELECT col_%d FROM orochi.orochi_json_data_%u_%d WHERE row_id = %ld",
        col_idx, state->table_oid, state->source_attnum, row_id);

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
    {
        pfree(sql.data);
        *isnull = true;
        return false;
    }

    ret = SPI_execute(sql.data, true, 1);
    pfree(sql.data);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        SPI_finish();
        *isnull = true;
        return false;
    }

    *value = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                           1, isnull);

    SPI_finish();
    state->rows_read++;

    return true;
}

int
json_columnar_read_batch(JsonColumnarReadState *state,
                         int32 column_id, int64 start_row,
                         int max_rows, Datum *values, bool *nulls)
{
    int         ret;
    StringInfoData sql;
    int         col_idx = -1;
    ListCell   *lc;
    int         rows_read = 0;

    /* Find column index */
    int idx = 0;
    foreach(lc, state->columns_to_read)
    {
        if (lfirst_int(lc) == column_id)
        {
            col_idx = idx;
            break;
        }
        idx++;
    }

    if (col_idx < 0)
        return 0;

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "SELECT col_%d FROM orochi.orochi_json_data_%u_%d "
        "WHERE row_id >= %ld ORDER BY row_id LIMIT %d",
        col_idx, state->table_oid, state->source_attnum,
        start_row, max_rows);

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
    {
        pfree(sql.data);
        return 0;
    }

    ret = SPI_execute(sql.data, true, max_rows);
    pfree(sql.data);

    if (ret != SPI_OK_SELECT)
    {
        SPI_finish();
        return 0;
    }

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        values[i] = SPI_getbinval(SPI_tuptable->vals[i],
                                  SPI_tuptable->tupdesc, 1, &nulls[i]);
        rows_read++;
    }

    SPI_finish();
    state->rows_read += rows_read;

    return rows_read;
}

void
json_columnar_end_read(JsonColumnarReadState *state)
{
    if (state == NULL)
        return;

    if (state->columns_to_read)
        list_free(state->columns_to_read);

    if (state->decompressed)
    {
        for (int i = 0; i < state->num_decompressed; i++)
        {
            if (state->decompressed[i].values)
                pfree(state->decompressed[i].values);
            if (state->decompressed[i].nulls)
                pfree(state->decompressed[i].nulls);
        }
        pfree(state->decompressed);
    }

    if (state->read_context)
        MemoryContextDelete(state->read_context);

    pfree(state);
}

/* ============================================================
 * Compression Operations Implementation
 * ============================================================ */

JsonColumnarStorageType
json_columnar_select_storage(JsonExtractedColumn *col,
                             Datum *sample_values, int sample_count)
{
    double      distinct_ratio;
    int         run_count = 0;
    Datum       prev_value = (Datum) 0;
    bool        first = true;

    if (col == NULL || sample_count < 10)
        return JSON_COLUMNAR_RAW;

    /* Calculate distinct ratio */
    distinct_ratio = (double)col->distinct_count / col->row_count;

    /* Low cardinality -> Dictionary */
    if (distinct_ratio < 0.1)
        return JSON_COLUMNAR_DICTIONARY;

    /* Check for runs (consecutive equal values) */
    for (int i = 0; i < sample_count; i++)
    {
        if (first || sample_values[i] != prev_value)
        {
            run_count++;
            prev_value = sample_values[i];
            first = false;
        }
    }

    /* High run ratio -> RLE */
    if ((double)run_count / sample_count < 0.3)
        return JSON_COLUMNAR_RLE;

    /* Numeric types with sequential pattern -> Delta */
    if (col->target_type == INT4OID ||
        col->target_type == INT8OID ||
        col->target_type == FLOAT8OID)
    {
        return JSON_COLUMNAR_DELTA;
    }

    /* Default to raw for small data, inline for larger */
    if (col->storage_size < 1024)
        return JSON_COLUMNAR_INLINE;

    return JSON_COLUMNAR_RAW;
}

char *
json_columnar_compress(Datum *values, bool *nulls, int count,
                       JsonColumnarStorageType storage_type,
                       int64 *compressed_size)
{
    /* Simplified compression - in production would use actual algorithms */
    char *result;
    int64 size = count * sizeof(Datum);

    result = palloc(size);
    memcpy(result, values, size);
    *compressed_size = size;

    return result;
}

void
json_columnar_decompress(const char *compressed, int64 compressed_size,
                         JsonColumnarStorageType storage_type,
                         int count, Datum *values, bool *nulls)
{
    /* Simplified decompression */
    memcpy(values, compressed, count * sizeof(Datum));
    memset(nulls, false, count * sizeof(bool));
}

void
json_columnar_build_dictionary(Datum *values, int count,
                               char ***dict_values, int *dict_size,
                               int32 **dict_indices)
{
    HTAB       *dict_hash;
    HASHCTL     hash_ctl;
    int         unique_count = 0;

    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(Datum);
    hash_ctl.entrysize = sizeof(Datum) + sizeof(int32);
    hash_ctl.hcxt = CurrentMemoryContext;

    dict_hash = hash_create("DictHash", count / 10 + 1,
                            &hash_ctl, HASH_ELEM | HASH_CONTEXT);

    *dict_indices = palloc(count * sizeof(int32));

    /* First pass: build dictionary */
    for (int i = 0; i < count; i++)
    {
        bool found;
        void *entry = hash_search(dict_hash, &values[i], HASH_ENTER, &found);
        if (!found)
        {
            *(int32 *)((char *)entry + sizeof(Datum)) = unique_count++;
        }
        (*dict_indices)[i] = *(int32 *)((char *)entry + sizeof(Datum));
    }

    /* Extract dictionary values */
    *dict_size = unique_count;
    *dict_values = palloc(unique_count * sizeof(char *));

    /* Note: simplified - would need proper iteration in production */

    hash_destroy(dict_hash);
}

/* ============================================================
 * Statistics Implementation
 * ============================================================ */

JsonColumnarStats *
json_columnar_get_stats(Oid table_oid, int16 source_attnum)
{
    JsonColumnarStats *stats;
    List       *columns;
    ListCell   *lc;

    ensure_columnar_context();

    stats = MemoryContextAllocZero(JsonColumnarContext,
                                   sizeof(JsonColumnarStats));
    stats->table_oid = table_oid;
    stats->source_attnum = source_attnum;

    columns = json_columnar_get_columns(table_oid, source_attnum);
    stats->num_columns = list_length(columns);

    foreach(lc, columns)
    {
        JsonExtractedColumn *col = lfirst(lc);
        stats->total_rows = Max(stats->total_rows, col->row_count);
        stats->columnar_size += col->storage_size;
        stats->avg_compression += col->compression_ratio;
    }

    if (stats->num_columns > 0)
        stats->avg_compression /= stats->num_columns;

    /* Estimate original size */
    stats->original_size = stats->columnar_size * 2;  /* Rough estimate */
    if (stats->original_size > 0)
        stats->space_savings = 1.0 - (double)stats->columnar_size / stats->original_size;

    list_free_deep(columns);

    return stats;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_extract_columns);
Datum
orochi_extract_columns(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    ArrayType  *paths_array = PG_GETARG_ARRAYTYPE_P(2);
    char       *column_name;
    int16       column_attnum;
    const char **paths;
    int         path_count;
    Datum      *path_datums;
    bool       *path_nulls;
    List       *result;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    /* Extract paths from array */
    deconstruct_array(paths_array, TEXTOID, -1, false, TYPALIGN_INT,
                      &path_datums, &path_nulls, &path_count);

    paths = palloc(path_count * sizeof(char *));
    for (int i = 0; i < path_count; i++)
    {
        if (path_nulls[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("path cannot be null")));
        paths[i] = TextDatumGetCString(path_datums[i]);
    }

    result = json_columnar_extract_columns(table_oid, column_attnum,
                                           paths, path_count);

    pfree(paths);

    PG_RETURN_INT32(list_length(result));
}

PG_FUNCTION_INFO_V1(orochi_json_columnar_info);
Datum
orochi_json_columnar_info(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    char       *column_name;
    int16       column_attnum;
    List       *columns;
    FuncCallContext *funcctx;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext old_context;
        TupleDesc   tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        columns = json_columnar_get_columns(table_oid, column_attnum);
        funcctx->user_fctx = columns;
        funcctx->max_calls = list_length(columns);

        tupdesc = CreateTemplateTupleDesc(8);
        TupleDescInitEntry(tupdesc, 1, "column_id", INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "path", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "target_type", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "row_count", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "null_count", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 6, "storage_size", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 7, "compression_ratio", FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 8, "state", TEXTOID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    columns = funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        JsonExtractedColumn *col;
        Datum       values[8];
        bool        nulls[8] = {false};
        HeapTuple   tuple;
        const char *state_names[] = {"active", "stale", "deprecated"};

        col = list_nth(columns, funcctx->call_cntr);

        values[0] = Int32GetDatum(col->column_id);
        values[1] = CStringGetTextDatum(col->path);
        values[2] = CStringGetTextDatum(format_type_be(col->target_type));
        values[3] = Int64GetDatum(col->row_count);
        values[4] = Int64GetDatum(col->null_count);
        values[5] = Int64GetDatum(col->storage_size);
        values[6] = Float8GetDatum(col->compression_ratio);
        values[7] = CStringGetTextDatum(state_names[col->state]);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    /* Free columns list */
    {
        ListCell *lc;
        foreach(lc, columns)
        {
            json_columnar_free_column(lfirst(lc));
        }
        list_free(columns);
    }

    SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(orochi_json_columnar_refresh);
Datum
orochi_json_columnar_refresh(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    char       *column_name;
    int16       column_attnum;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    json_columnar_refresh_all(table_oid, column_attnum);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_json_columnar_stats);
Datum
orochi_json_columnar_stats(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    char       *column_name;
    int16       column_attnum;
    JsonColumnarStats *stats;
    TupleDesc   tupdesc;
    Datum       values[6];
    bool        nulls[6] = {false};
    HeapTuple   tuple;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    stats = json_columnar_get_stats(table_oid, column_attnum);

    tupdesc = CreateTemplateTupleDesc(6);
    TupleDescInitEntry(tupdesc, 1, "num_columns", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "total_rows", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "columnar_size", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "original_size", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 5, "space_savings", FLOAT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 6, "avg_compression", FLOAT8OID, -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = Int32GetDatum(stats->num_columns);
    values[1] = Int64GetDatum(stats->total_rows);
    values[2] = Int64GetDatum(stats->columnar_size);
    values[3] = Int64GetDatum(stats->original_size);
    values[4] = Float8GetDatum(stats->space_savings);
    values[5] = Float8GetDatum(stats->avg_compression);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    pfree(stats);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(orochi_json_enable_hybrid);
Datum
orochi_json_enable_hybrid(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    ArrayType  *paths_array = PG_GETARG_ARRAYTYPE_P(2);
    char       *column_name;
    int16       column_attnum;
    const char **paths;
    int         path_count;
    Datum      *path_datums;
    bool       *path_nulls;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    /* Extract paths from array */
    deconstruct_array(paths_array, TEXTOID, -1, false, TYPALIGN_INT,
                      &path_datums, &path_nulls, &path_count);

    paths = palloc(path_count * sizeof(char *));
    for (int i = 0; i < path_count; i++)
    {
        if (path_nulls[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("path cannot be null")));
        paths[i] = TextDatumGetCString(path_datums[i]);
    }

    json_columnar_enable_hybrid(table_oid, column_attnum, paths, path_count);

    pfree(paths);

    PG_RETURN_VOID();
}

/* ============================================================
 * Hybrid Storage Implementation Stubs
 * ============================================================ */

void
json_columnar_enable_hybrid(Oid table_oid, int16 source_attnum,
                            const char **columnar_paths, int path_count)
{
    /* Extract the specified paths to columnar storage */
    json_columnar_extract_columns(table_oid, source_attnum,
                                  columnar_paths, path_count);

    elog(NOTICE, "Enabled hybrid storage for %d paths", path_count);
}

void
json_columnar_disable_hybrid(Oid table_oid, int16 source_attnum)
{
    List       *columns;
    ListCell   *lc;

    columns = json_columnar_get_columns(table_oid, source_attnum);

    foreach(lc, columns)
    {
        JsonExtractedColumn *col = lfirst(lc);
        json_columnar_drop_column(col->column_id);
    }

    list_free_deep(columns);

    elog(NOTICE, "Disabled hybrid storage");
}

bool
json_columnar_is_hybrid(Oid table_oid, int16 source_attnum)
{
    List *columns = json_columnar_get_columns(table_oid, source_attnum);
    bool is_hybrid = list_length(columns) > 0;
    list_free_deep(columns);
    return is_hybrid;
}

JsonHybridConfig *
json_columnar_get_hybrid_config(Oid table_oid, int16 source_attnum)
{
    JsonHybridConfig *config;
    List       *columns;
    ListCell   *lc;

    ensure_columnar_context();

    config = MemoryContextAllocZero(JsonColumnarContext,
                                    sizeof(JsonHybridConfig));
    config->table_oid = table_oid;
    config->source_attnum = source_attnum;
    config->keep_original = true;
    config->size_threshold = 1024;

    columns = json_columnar_get_columns(table_oid, source_attnum);

    foreach(lc, columns)
    {
        JsonExtractedColumn *col = lfirst(lc);
        config->columnar_paths = lappend(config->columnar_paths,
                                         pstrdup(col->path));
    }

    list_free_deep(columns);

    return config;
}

void
json_columnar_compact(Oid table_oid, int16 source_attnum)
{
    /* In production, would merge small chunks */
    elog(NOTICE, "Compacting JSON columnar storage");
}

void
json_columnar_vacuum(Oid table_oid, int16 source_attnum)
{
    /* In production, would remove stale/deleted data */
    elog(NOTICE, "Vacuuming JSON columnar storage");
}

List *
json_columnar_analyze(Oid table_oid, int16 source_attnum)
{
    /* Returns recommendations for columnar optimization */
    return NIL;
}
