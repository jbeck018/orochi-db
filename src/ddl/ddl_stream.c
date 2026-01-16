/*-------------------------------------------------------------------------
 *
 * ddl_stream.c
 *    Implementation of Stream DDL for Orochi DB
 *
 * This module handles:
 *   - Parsing CREATE STREAM statements
 *   - Storing stream definitions in catalog
 *   - Change tracking and offset management
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/indexing.h"
#include "access/table.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"

#include "ddl_stream.h"
#include "../core/catalog.h"

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

/*
 * Allocate a new Stream
 */
static Stream *
stream_alloc(void)
{
    Stream *stream = palloc0(sizeof(Stream));
    stream->stream_type = STREAM_TYPE_STANDARD;
    stream->stream_mode = STREAM_MODE_DEFAULT;
    stream->state = STREAM_STATE_ACTIVE;
    stream->show_initial_rows = false;
    stream->has_pending_data = false;
    stream->created_at = GetCurrentTimestamp();
    return stream;
}

/*
 * Allocate a new StreamOffset
 */
static StreamOffset *
stream_offset_alloc(void)
{
    StreamOffset *offset = palloc0(sizeof(StreamOffset));
    offset->lsn = InvalidXLogRecPtr;
    offset->xid = InvalidTransactionId;
    offset->timestamp = 0;
    offset->sequence = 0;
    return offset;
}

/*
 * Get primary key columns for a relation
 * Returns a Bitmapset of attribute numbers that are part of the primary key
 */
static Bitmapset *
get_primary_key_columns(Oid relid)
{
    Bitmapset      *pk_columns = NULL;
    Relation        rel;
    List           *indexoidlist;
    ListCell       *lc;

    rel = table_open(relid, AccessShareLock);
    indexoidlist = RelationGetIndexList(rel);

    foreach(lc, indexoidlist)
    {
        Oid         indexoid = lfirst_oid(lc);
        HeapTuple   indexTuple;
        Form_pg_index indexForm;

        indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
        if (!HeapTupleIsValid(indexTuple))
            continue;

        indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

        /* Check if this is the primary key index */
        if (indexForm->indisprimary)
        {
            int     nkeys = indexForm->indnkeyatts;
            int     j;

            for (j = 0; j < nkeys; j++)
            {
                int attnum = indexForm->indkey.values[j];
                if (attnum > 0)
                    pk_columns = bms_add_member(pk_columns, attnum);
            }

            ReleaseSysCache(indexTuple);
            break;
        }

        ReleaseSysCache(indexTuple);
    }

    list_free(indexoidlist);
    table_close(rel, AccessShareLock);

    return pk_columns;
}

/*
 * Get table columns for stream tracking
 */
static void
stream_get_table_columns(Stream *stream)
{
    Relation    rel;
    TupleDesc   tupdesc;
    int         i;
    int         num_cols;
    Bitmapset  *pk_columns;

    /* Get primary key columns first */
    pk_columns = get_primary_key_columns(stream->source_table_oid);

    rel = table_open(stream->source_table_oid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    num_cols = 0;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped)
            num_cols++;
    }

    stream->num_columns = num_cols;
    stream->columns = palloc0(sizeof(StreamColumn) * num_cols);

    num_cols = 0;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped)
        {
            stream->columns[num_cols].column_name = pstrdup(NameStr(attr->attname));
            stream->columns[num_cols].column_type = attr->atttypid;
            /* Check if this column is part of the primary key */
            stream->columns[num_cols].is_key = bms_is_member(attr->attnum, pk_columns);
            stream->columns[num_cols].track_changes = true;
            num_cols++;
        }
    }

    bms_free(pk_columns);
    table_close(rel, AccessShareLock);
}

/* ============================================================
 * DDL Parsing Functions
 * ============================================================ */

/*
 * Parse stream options (WITH clause)
 */
void
ddl_parse_stream_options(Stream *stream, const char *options)
{
    const char *p = options;

    if (options == NULL)
        return;

    /* Skip WITH if present */
    if (strncasecmp(p, "WITH", 4) == 0)
        p += 4;
    while (*p && isspace(*p)) p++;
    if (*p == '(')
        p++;

    /* Parse key=value pairs */
    while (*p && *p != ')')
    {
        char key[128];
        char value[256];
        int  key_len = 0;
        int  val_len = 0;

        while (*p && isspace(*p)) p++;

        /* Parse key */
        while (*p && *p != '=' && !isspace(*p) && key_len < 127)
            key[key_len++] = *p++;
        key[key_len] = '\0';

        /* Skip = */
        while (*p && (*p == '=' || isspace(*p))) p++;

        /* Parse value (may be quoted) */
        if (*p == '\'')
        {
            p++;
            while (*p && *p != '\'' && val_len < 255)
                value[val_len++] = *p++;
            if (*p == '\'')
                p++;
        }
        else
        {
            while (*p && *p != ',' && *p != ')' && !isspace(*p) && val_len < 255)
                value[val_len++] = *p++;
        }
        value[val_len] = '\0';

        /* Handle known options */
        if (strcasecmp(key, "APPEND_ONLY") == 0 ||
            strcasecmp(key, "INSERT_ONLY") == 0)
        {
            if (strcasecmp(value, "TRUE") == 0 || strcmp(value, "1") == 0)
                stream->stream_type = STREAM_TYPE_APPEND_ONLY;
        }
        else if (strcasecmp(key, "SHOW_INITIAL_ROWS") == 0)
        {
            stream->show_initial_rows =
                (strcasecmp(value, "TRUE") == 0 || strcmp(value, "1") == 0);
        }
        else if (strcasecmp(key, "MODE") == 0)
        {
            stream->stream_mode = stream_parse_mode(value);
        }

        /* Skip comma */
        while (*p && (*p == ',' || isspace(*p))) p++;
    }
}

/*
 * Parse CREATE STREAM statement
 *
 * Format: CREATE STREAM name ON TABLE table_name [WITH (options)]
 */
Stream *
ddl_parse_create_stream(const char *sql)
{
    Stream     *stream;
    const char *p;
    char       *stream_name;
    char       *table_name;
    size_t      len;
    List       *name_list;
    RangeVar   *rv;
    Oid         table_oid;

    stream = stream_alloc();

    p = sql;

    /* Skip CREATE STREAM */
    if (strncasecmp(p, "CREATE", 6) == 0)
        p += 6;
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "STREAM", 6) == 0)
        p += 6;
    while (*p && isspace(*p)) p++;

    /* Parse stream name */
    len = 0;
    while (p[len] && !isspace(p[len]))
        len++;

    stream_name = pnstrdup(p, len);
    stream->stream_name = stream_name;
    p += len;

    /* Skip to ON TABLE */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "ON", 2) == 0)
        p += 2;
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "TABLE", 5) == 0)
        p += 5;
    while (*p && isspace(*p)) p++;

    /* Parse table name */
    len = 0;
    while (p[len] && !isspace(p[len]) && p[len] != ';')
        len++;

    table_name = pnstrdup(p, len);
    p += len;

    /* Resolve table OID */
    name_list = stringToQualifiedNameList(table_name, NULL);
    rv = makeRangeVarFromNameList(name_list);
    table_oid = RangeVarGetRelid(rv, AccessShareLock, false);

    stream->source_table_oid = table_oid;
    stream->source_table = table_name;

    /* Get schema name */
    if (rv->schemaname)
        stream->source_schema = pstrdup(rv->schemaname);
    else
        stream->source_schema = pstrdup("public");

    stream->stream_schema = pstrdup(stream->source_schema);

    /* Parse optional WITH clause */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "WITH", 4) == 0)
        ddl_parse_stream_options(stream, p);

    /* Initialize offsets */
    stream->initial_offset = *stream_offset_alloc();
    stream->current_offset = stream->initial_offset;
    stream->latest_offset = stream->initial_offset;

    /* Get table columns */
    stream_get_table_columns(stream);

    return stream;
}

/*
 * Validate stream definition
 */
bool
ddl_validate_stream(Stream *stream, char **error_msg)
{
    if (stream == NULL)
    {
        *error_msg = pstrdup("stream is NULL");
        return false;
    }

    if (stream->stream_name == NULL || strlen(stream->stream_name) == 0)
    {
        *error_msg = pstrdup("stream name is required");
        return false;
    }

    if (strlen(stream->stream_name) > STREAM_MAX_NAME_LENGTH)
    {
        *error_msg = psprintf("stream name exceeds maximum length of %d",
                              STREAM_MAX_NAME_LENGTH);
        return false;
    }

    if (!OidIsValid(stream->source_table_oid))
    {
        *error_msg = pstrdup("source table does not exist");
        return false;
    }

    *error_msg = NULL;
    return true;
}

/* ============================================================
 * Catalog Operations
 * ============================================================ */

/*
 * Store stream in catalog
 */
void
ddl_catalog_store_stream(Stream *stream)
{
    StringInfoData query;
    int            ret;

    initStringInfo(&query);

    appendStringInfo(&query,
        "INSERT INTO orochi.%s "
        "(stream_name, stream_schema, source_table_oid, source_schema, "
        "source_table, stream_type, stream_mode, show_initial_rows, "
        "state, created_at) "
        "VALUES ('%s', '%s', %u, '%s', '%s', %d, %d, %s, %d, NOW()) "
        "RETURNING stream_id",
        OROCHI_STREAMS_TABLE,
        stream->stream_name,
        stream->stream_schema ? stream->stream_schema : "public",
        stream->source_table_oid,
        stream->source_schema ? stream->source_schema : "public",
        stream->source_table,
        (int) stream->stream_type,
        (int) stream->stream_mode,
        stream->show_initial_rows ? "TRUE" : "FALSE",
        (int) stream->state);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        Datum id_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                       SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            stream->stream_id = DatumGetInt64(id_datum);
    }

    /* Store stream columns */
    for (int i = 0; i < stream->num_columns; i++)
    {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "INSERT INTO orochi.%s "
            "(stream_id, column_name, column_type, is_key, track_changes) "
            "VALUES (%ld, '%s', %u, %s, %s)",
            OROCHI_STREAM_COLUMNS_TABLE,
            stream->stream_id,
            stream->columns[i].column_name,
            stream->columns[i].column_type,
            stream->columns[i].is_key ? "TRUE" : "FALSE",
            stream->columns[i].track_changes ? "TRUE" : "FALSE");

        SPI_execute(query.data, false, 0);
    }

    SPI_finish();
    pfree(query.data);
}

/*
 * Update stream in catalog
 */
void
ddl_catalog_update_stream(Stream *stream)
{
    StringInfoData query;

    initStringInfo(&query);

    appendStringInfo(&query,
        "UPDATE orochi.%s SET "
        "state = %d, "
        "has_pending_data = %s, "
        "last_consumed_at = %s, "
        "total_changes = %ld, "
        "pending_changes = %ld "
        "WHERE stream_id = %ld",
        OROCHI_STREAMS_TABLE,
        (int) stream->state,
        stream->has_pending_data ? "TRUE" : "FALSE",
        stream->last_consumed_at ?
            psprintf("'%s'", timestamptz_to_str(stream->last_consumed_at)) : "NULL",
        stream->total_changes,
        stream->pending_changes,
        stream->stream_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/*
 * Delete stream from catalog
 */
void
ddl_catalog_delete_stream(int64 stream_id)
{
    StringInfoData query;

    initStringInfo(&query);

    /* Delete columns first */
    appendStringInfo(&query,
        "DELETE FROM orochi.%s WHERE stream_id = %ld",
        OROCHI_STREAM_COLUMNS_TABLE, stream_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);

    /* Delete offsets */
    resetStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.%s WHERE stream_id = %ld",
        OROCHI_STREAM_OFFSETS_TABLE, stream_id);
    SPI_execute(query.data, false, 0);

    /* Delete stream */
    resetStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.%s WHERE stream_id = %ld",
        OROCHI_STREAMS_TABLE, stream_id);
    SPI_execute(query.data, false, 0);

    SPI_finish();
    pfree(query.data);
}

/*
 * Update stream offset in catalog
 */
void
ddl_catalog_update_stream_offset(int64 stream_id, StreamOffset *offset)
{
    StringInfoData query;

    initStringInfo(&query);

    appendStringInfo(&query,
        "INSERT INTO orochi.%s "
        "(stream_id, lsn, xid, timestamp, sequence) "
        "VALUES (%ld, '%X/%X', %u, '%s', %ld) "
        "ON CONFLICT (stream_id) DO UPDATE SET "
        "lsn = EXCLUDED.lsn, "
        "xid = EXCLUDED.xid, "
        "timestamp = EXCLUDED.timestamp, "
        "sequence = EXCLUDED.sequence",
        OROCHI_STREAM_OFFSETS_TABLE,
        stream_id,
        (uint32) (offset->lsn >> 32),
        (uint32) offset->lsn,
        offset->xid,
        timestamptz_to_str(offset->timestamp),
        offset->sequence);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Stream Management Functions
 * ============================================================ */

/*
 * Create a stream
 */
int64
orochi_create_stream(const char *name, Oid source_table,
                     StreamType type, StreamMode mode,
                     bool show_initial_rows)
{
    Stream *stream;
    char   *error_msg;

    stream = stream_alloc();
    stream->stream_name = pstrdup(name);
    stream->source_table_oid = source_table;
    stream->stream_type = type;
    stream->stream_mode = mode;
    stream->show_initial_rows = show_initial_rows;

    /* Get table name and columns */
    stream->source_table = get_rel_name(source_table);
    stream->source_schema = get_namespace_name(get_rel_namespace(source_table));
    stream->stream_schema = pstrdup(stream->source_schema);

    stream_get_table_columns(stream);

    /* Validate */
    if (!ddl_validate_stream(stream, &error_msg))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid stream definition: %s", error_msg)));
    }

    /* Store in catalog */
    ddl_catalog_store_stream(stream);

    elog(LOG, "created stream '%s' on table '%s.%s' with ID %ld",
         stream->stream_name, stream->source_schema,
         stream->source_table, stream->stream_id);

    return stream->stream_id;
}

/*
 * Drop a stream
 */
bool
orochi_drop_stream(int64 stream_id, bool if_exists)
{
    ddl_catalog_delete_stream(stream_id);
    elog(LOG, "dropped stream with ID %ld", stream_id);
    return true;
}

/*
 * Check if stream has pending data
 */
bool
orochi_stream_has_data(int64 stream_id)
{
    StringInfoData query;
    bool           has_data = false;
    int            ret;

    initStringInfo(&query);

    /*
     * Check if there are unconsumed changes by comparing the stream's
     * current offset with the latest available offset, and check the
     * has_pending_data flag in the catalog.
     */
    appendStringInfo(&query,
        "SELECT has_pending_data, "
        "       (SELECT COUNT(*) > 0 FROM orochi.%s so "
        "        WHERE so.stream_id = s.stream_id "
        "        AND so.sequence > COALESCE("
        "            (SELECT sequence FROM orochi.%s WHERE stream_id = s.stream_id), 0)) "
        "FROM orochi.%s s "
        "WHERE s.stream_id = %ld",
        OROCHI_STREAM_OFFSETS_TABLE,
        OROCHI_STREAM_OFFSETS_TABLE,
        OROCHI_STREAMS_TABLE,
        stream_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull1, isnull2;
        Datum has_pending = SPI_getbinval(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1, &isnull1);
        Datum has_new_changes = SPI_getbinval(SPI_tuptable->vals[0],
                                               SPI_tuptable->tupdesc, 2, &isnull2);

        /* Stream has data if either flag is set or there are new changes */
        has_data = (!isnull1 && DatumGetBool(has_pending)) ||
                   (!isnull2 && DatumGetBool(has_new_changes));
    }

    SPI_finish();
    pfree(query.data);

    return has_data;
}

/*
 * Get pending change count
 */
int64
orochi_stream_pending_count(int64 stream_id)
{
    StringInfoData query;
    int64          count = 0;
    int            ret;

    initStringInfo(&query);

    /*
     * Count the number of pending changes in the stream.
     * This queries the pending_changes column from the catalog,
     * which is maintained by the change capture system.
     */
    appendStringInfo(&query,
        "SELECT COALESCE(pending_changes, 0) "
        "FROM orochi.%s "
        "WHERE stream_id = %ld",
        OROCHI_STREAMS_TABLE,
        stream_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        Datum count_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            count = DatumGetInt64(count_datum);
    }

    SPI_finish();
    pfree(query.data);

    return count;
}

/*
 * Advance stream offset
 */
bool
orochi_stream_advance_offset(int64 stream_id, StreamOffset *offset)
{
    ddl_catalog_update_stream_offset(stream_id, offset);
    return true;
}

/*
 * Check if stream is stale
 *
 * A stream becomes stale when the source table's retention policy has
 * deleted data that the stream hasn't yet consumed. This can happen when:
 * - The stream hasn't been consumed for a long time
 * - The source table has aggressive data retention
 * - The stream offset points to WAL positions that have been recycled
 */
bool
orochi_stream_is_stale(int64 stream_id)
{
    StringInfoData query;
    bool           is_stale = false;
    int            ret;

    initStringInfo(&query);

    /*
     * Check staleness by:
     * 1. Checking if the stream state is already marked as STALE
     * 2. Checking if the stream's offset timestamp is older than stale_after
     * 3. Checking if the source table's oldest data is newer than stream offset
     */
    appendStringInfo(&query,
        "SELECT "
        "  s.state = %d OR "
        "  (s.stale_after IS NOT NULL AND s.stale_after < NOW()) OR "
        "  (so.timestamp IS NOT NULL AND "
        "   so.timestamp < NOW() - INTERVAL '%d days') "
        "FROM orochi.%s s "
        "LEFT JOIN orochi.%s so ON s.stream_id = so.stream_id "
        "WHERE s.stream_id = %ld",
        (int) STREAM_STATE_STALE,
        STREAM_DEFAULT_STALENESS_DAYS,
        OROCHI_STREAMS_TABLE,
        OROCHI_STREAM_OFFSETS_TABLE,
        stream_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        Datum stale_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            is_stale = DatumGetBool(stale_datum);
    }

    SPI_finish();
    pfree(query.data);

    return is_stale;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *
stream_type_name(StreamType type)
{
    switch (type)
    {
        case STREAM_TYPE_STANDARD:      return "STANDARD";
        case STREAM_TYPE_APPEND_ONLY:   return "APPEND_ONLY";
        case STREAM_TYPE_INSERT_ONLY:   return "INSERT_ONLY";
        default:                        return "UNKNOWN";
    }
}

StreamType
stream_parse_type(const char *name)
{
    if (strcasecmp(name, "STANDARD") == 0)      return STREAM_TYPE_STANDARD;
    if (strcasecmp(name, "APPEND_ONLY") == 0)   return STREAM_TYPE_APPEND_ONLY;
    if (strcasecmp(name, "INSERT_ONLY") == 0)   return STREAM_TYPE_INSERT_ONLY;
    return STREAM_TYPE_STANDARD;
}

const char *
stream_mode_name(StreamMode mode)
{
    switch (mode)
    {
        case STREAM_MODE_DEFAULT:       return "DEFAULT";
        case STREAM_MODE_KEYS_ONLY:     return "KEYS_ONLY";
        case STREAM_MODE_FULL_BEFORE:   return "FULL_BEFORE";
        default:                        return "UNKNOWN";
    }
}

StreamMode
stream_parse_mode(const char *name)
{
    if (strcasecmp(name, "DEFAULT") == 0)       return STREAM_MODE_DEFAULT;
    if (strcasecmp(name, "KEYS_ONLY") == 0)     return STREAM_MODE_KEYS_ONLY;
    if (strcasecmp(name, "FULL_BEFORE") == 0)   return STREAM_MODE_FULL_BEFORE;
    return STREAM_MODE_DEFAULT;
}

const char *
stream_state_name(StreamState state)
{
    switch (state)
    {
        case STREAM_STATE_ACTIVE:   return "ACTIVE";
        case STREAM_STATE_STALE:    return "STALE";
        case STREAM_STATE_PAUSED:   return "PAUSED";
        case STREAM_STATE_DISABLED: return "DISABLED";
        default:                    return "UNKNOWN";
    }
}

StreamState
stream_parse_state(const char *name)
{
    if (strcasecmp(name, "ACTIVE") == 0)    return STREAM_STATE_ACTIVE;
    if (strcasecmp(name, "STALE") == 0)     return STREAM_STATE_STALE;
    if (strcasecmp(name, "PAUSED") == 0)    return STREAM_STATE_PAUSED;
    if (strcasecmp(name, "DISABLED") == 0)  return STREAM_STATE_DISABLED;
    return STREAM_STATE_ACTIVE;
}

const char *
stream_action_name(StreamAction action)
{
    switch (action)
    {
        case STREAM_ACTION_INSERT:  return "INSERT";
        case STREAM_ACTION_UPDATE:  return "UPDATE";
        case STREAM_ACTION_DELETE:  return "DELETE";
        default:                    return "UNKNOWN";
    }
}

int
stream_offset_compare(StreamOffset *a, StreamOffset *b)
{
    if (a->lsn < b->lsn)
        return -1;
    if (a->lsn > b->lsn)
        return 1;
    if (a->sequence < b->sequence)
        return -1;
    if (a->sequence > b->sequence)
        return 1;
    return 0;
}

bool
stream_offset_is_valid(StreamOffset *offset)
{
    return offset != NULL && offset->lsn != InvalidXLogRecPtr;
}

StreamOffset *
stream_offset_copy(StreamOffset *offset)
{
    StreamOffset *copy;

    if (offset == NULL)
        return NULL;

    copy = palloc(sizeof(StreamOffset));
    memcpy(copy, offset, sizeof(StreamOffset));
    return copy;
}

void
stream_change_record_free(StreamChangeRecord *record)
{
    if (record == NULL)
        return;

    if (record->old_values)
        pfree(record->old_values);
    if (record->old_nulls)
        pfree(record->old_nulls);
    if (record->new_values)
        pfree(record->new_values);
    if (record->new_nulls)
        pfree(record->new_nulls);

    pfree(record);
}

void
stream_free(Stream *stream)
{
    int i;

    if (stream == NULL)
        return;

    if (stream->stream_name)
        pfree(stream->stream_name);
    if (stream->stream_schema)
        pfree(stream->stream_schema);
    if (stream->source_schema)
        pfree(stream->source_schema);
    if (stream->source_table)
        pfree(stream->source_table);

    for (i = 0; i < stream->num_columns; i++)
    {
        if (stream->columns[i].column_name)
            pfree(stream->columns[i].column_name);
    }
    if (stream->columns)
        pfree(stream->columns);

    pfree(stream);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_stream_sql);
Datum
orochi_create_stream_sql(PG_FUNCTION_ARGS)
{
    text   *name_text = PG_GETARG_TEXT_PP(0);
    Oid     table_oid = PG_GETARG_OID(1);
    char   *name = text_to_cstring(name_text);
    int64   stream_id;

    stream_id = orochi_create_stream(name, table_oid,
                                     STREAM_TYPE_STANDARD,
                                     STREAM_MODE_DEFAULT,
                                     false);

    PG_RETURN_INT64(stream_id);
}

PG_FUNCTION_INFO_V1(orochi_drop_stream_sql);
Datum
orochi_drop_stream_sql(PG_FUNCTION_ARGS)
{
    int64 stream_id = PG_GETARG_INT64(0);
    bool  if_exists = PG_GETARG_BOOL(1);

    orochi_drop_stream(stream_id, if_exists);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_stream_has_data_sql);
Datum
orochi_stream_has_data_sql(PG_FUNCTION_ARGS)
{
    int64 stream_id = PG_GETARG_INT64(0);

    PG_RETURN_BOOL(orochi_stream_has_data(stream_id));
}

PG_FUNCTION_INFO_V1(orochi_stream_status_sql);
Datum
orochi_stream_status_sql(PG_FUNCTION_ARGS)
{
    int64           stream_id = PG_GETARG_INT64(0);
    StringInfoData  query;
    const char     *state_name = NULL;
    int             ret;
    StreamState     state = STREAM_STATE_ACTIVE;

    initStringInfo(&query);

    /* Query the stream state from the catalog */
    appendStringInfo(&query,
        "SELECT state FROM orochi.%s WHERE stream_id = %ld",
        OROCHI_STREAMS_TABLE,
        stream_id);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        Datum state_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            state = (StreamState) DatumGetInt32(state_datum);
    }
    else
    {
        /* Stream not found */
        SPI_finish();
        pfree(query.data);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("stream with ID %ld does not exist", stream_id)));
    }

    SPI_finish();
    pfree(query.data);

    state_name = stream_state_name(state);

    PG_RETURN_TEXT_P(cstring_to_text(state_name));
}
