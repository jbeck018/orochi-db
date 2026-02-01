/*-------------------------------------------------------------------------
 *
 * json_index.c
 *    Orochi DB JSON path indexing implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_utilcmd.h"
#include "postgres.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* PostgreSQL 18 removed some AM OID macros - define them if needed */
#if PG_VERSION_NUM >= 180000
#ifndef GIN_AM_OID
#define GIN_AM_OID 2742
#endif
#ifndef BTREE_AM_OID
#define BTREE_AM_OID 403
#endif
#ifndef HASH_AM_OID
#define HASH_AM_OID 405
#endif
#endif

#include "../orochi.h"
#include "json_index.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Hash table for path statistics (in shared memory) */
static HTAB *JsonPathStatsHash = NULL;

/* Memory context for JSON index operations */
static MemoryContext JsonIndexContext = NULL;

/* Forward declarations */
static void ensure_json_index_context(void);
static void analyze_jsonb_value(Jsonb *jb, const char *current_path, JsonPathStatsCollection *stats,
                                int depth);
static JsonIndexType recommend_index_type(JsonPathStats *path_stats);
static char *build_index_create_sql(Oid table_oid, int16 column_attnum, const char *path,
                                    JsonIndexType type);

/* ============================================================
 * Utility Functions
 * ============================================================ */

static void ensure_json_index_context(void)
{
    if (JsonIndexContext == NULL) {
        JsonIndexContext =
            AllocSetContextCreate(TopMemoryContext, "JsonIndexContext", ALLOCSET_DEFAULT_SIZES);
    }
}

char **json_index_parse_path(const char *path, int *depth)
{
    char *path_copy;
    char *token;
    char *saveptr;
    char **components;
    int count = 0;
    int capacity = 8;

    if (path == NULL || *path == '\0') {
        *depth = 0;
        return NULL;
    }

    ensure_json_index_context();

    components = palloc(capacity * sizeof(char *));
    path_copy = pstrdup(path);

    /* Handle leading $ or root indicator */
    if (path_copy[0] == '$')
        path_copy++;
    if (path_copy[0] == '.')
        path_copy++;

    token = strtok_r(path_copy, ".", &saveptr);
    while (token != NULL && count < JSON_INDEX_MAX_PATH_DEPTH) {
        if (count >= capacity) {
            capacity *= 2;
            components = repalloc(components, capacity * sizeof(char *));
        }

        /* Handle array notation like [0] or [*] */
        char *bracket = strchr(token, '[');
        if (bracket != NULL) {
            *bracket = '\0';
            if (*token != '\0') {
                components[count++] = pstrdup(token);
            }
            components[count++] = pstrdup(bracket + 1);
            /* Remove trailing ] */
            char *end = strchr(components[count - 1], ']');
            if (end)
                *end = '\0';
        } else {
            components[count++] = pstrdup(token);
        }

        token = strtok_r(NULL, ".", &saveptr);
    }

    *depth = count;
    return components;
}

char *json_index_normalize_path(const char *path)
{
    StringInfoData buf;
    int depth;
    char **components;
    int i;

    if (path == NULL || *path == '\0')
        return pstrdup("$");

    components = json_index_parse_path(path, &depth);
    if (depth == 0)
        return pstrdup("$");

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '$');

    for (i = 0; i < depth; i++) {
        /* Check if this is an array index */
        if (components[i][0] >= '0' && components[i][0] <= '9') {
            appendStringInfo(&buf, "[%s]", components[i]);
        } else if (components[i][0] == '*') {
            appendStringInfoString(&buf, "[*]");
        } else {
            appendStringInfo(&buf, ".%s", components[i]);
        }
    }

    return buf.data;
}

bool json_index_is_valid_path(const char *path)
{
    int depth;
    char **components;

    if (path == NULL || *path == '\0')
        return false;

    components = json_index_parse_path(path, &depth);
    if (depth == 0 || depth > JSON_INDEX_MAX_PATH_DEPTH)
        return false;

    /* All components should be non-empty */
    for (int i = 0; i < depth; i++) {
        if (components[i] == NULL || *components[i] == '\0')
            return false;
    }

    return true;
}

JsonPathType json_index_get_path_type(Jsonb *jb, const char *path)
{
    JsonbValue *v;
    Datum path_datum;
    Datum result_datum;
    Jsonb *result;

    if (jb == NULL)
        return JSON_PATH_NULL;

    /* Use jsonb_path_query_first to get value at path */
    path_datum = CStringGetTextDatum(path);

    PG_TRY();
    {
        result_datum = DirectFunctionCall2(jsonb_path_query_first, JsonbPGetDatum(jb), path_datum);
        if (result_datum == (Datum)0)
            return JSON_PATH_NULL;

        result = DatumGetJsonbP(result_datum);
        v = &result->root;

        if (v == NULL || v->type == jbvNull)
            return JSON_PATH_NULL;

        switch (v->type) {
        case jbvString:
            return JSON_PATH_STRING;
        case jbvNumeric:
            return JSON_PATH_NUMBER;
        case jbvBool:
            return JSON_PATH_BOOLEAN;
        case jbvArray:
            return JSON_PATH_ARRAY;
        case jbvObject:
            return JSON_PATH_OBJECT;
        default:
            return JSON_PATH_NULL;
        }
    }
    PG_CATCH();
    {
        /* Path not found or invalid */
        FlushErrorState();
        return JSON_PATH_NULL;
    }
    PG_END_TRY();

    return JSON_PATH_NULL;
}

const char *json_index_type_name(JsonIndexType type)
{
    switch (type) {
    case JSON_INDEX_GIN:
        return "gin";
    case JSON_INDEX_BTREE:
        return "btree";
    case JSON_INDEX_HASH:
        return "hash";
    case JSON_INDEX_EXPRESSION:
        return "expression";
    case JSON_INDEX_PARTIAL:
        return "partial";
    default:
        return "unknown";
    }
}

JsonIndexType json_index_parse_type(const char *name)
{
    if (pg_strcasecmp(name, "gin") == 0)
        return JSON_INDEX_GIN;
    if (pg_strcasecmp(name, "btree") == 0)
        return JSON_INDEX_BTREE;
    if (pg_strcasecmp(name, "hash") == 0)
        return JSON_INDEX_HASH;
    if (pg_strcasecmp(name, "expression") == 0)
        return JSON_INDEX_EXPRESSION;
    if (pg_strcasecmp(name, "partial") == 0)
        return JSON_INDEX_PARTIAL;

    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unknown JSON index type: %s", name)));
    return JSON_INDEX_GIN; /* Not reached */
}

/* ============================================================
 * Path Statistics Implementation
 * ============================================================ */

static void analyze_jsonb_value(Jsonb *jb, const char *current_path, JsonPathStatsCollection *stats,
                                int depth)
{
    JsonbIterator *it;
    JsonbIteratorToken token;
    JsonbValue v;
    StringInfoData path_buf;
    int array_index = 0;

    if (depth > JSON_INDEX_MAX_PATH_DEPTH)
        return;

    if (jb == NULL)
        return;

    it = JsonbIteratorInit(&jb->root);

    initStringInfo(&path_buf);

    while ((token = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        switch (token) {
        case WJB_BEGIN_OBJECT:
            break;

        case WJB_KEY: {
            /* Build path for this key */
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            resetStringInfo(&path_buf);
            if (current_path && *current_path)
                appendStringInfo(&path_buf, "%s.%s", current_path, key);
            else
                appendStringInfo(&path_buf, "$.%s", key);
            pfree(key);
            break;
        }

        case WJB_VALUE:
        case WJB_ELEM: {
            char *full_path;
            JsonPathStats *path_stats = NULL;

            if (token == WJB_ELEM) {
                /* Array element - use [*] for wildcard */
                resetStringInfo(&path_buf);
                if (current_path && *current_path)
                    appendStringInfo(&path_buf, "%s[*]", current_path);
                else
                    appendStringInfo(&path_buf, "$[*]");
                array_index++;
            }

            full_path = path_buf.data;

            /* Find or create stats entry */
            for (int i = 0; i < stats->path_count; i++) {
                if (strcmp(stats->paths[i].path, full_path) == 0) {
                    path_stats = &stats->paths[i];
                    break;
                }
            }

            if (path_stats == NULL && stats->path_count < JSON_INDEX_MAX_TRACKED_PATHS) {
                path_stats = &stats->paths[stats->path_count++];
                path_stats->path = pstrdup(full_path);
                path_stats->value_type = JSON_PATH_NULL;
                path_stats->access_count = 0;
                path_stats->null_count = 0;
                path_stats->distinct_count = 0;
                path_stats->selectivity = 1.0;
                path_stats->avg_value_size = 0;
                path_stats->first_seen = GetCurrentTimestamp();
                path_stats->is_indexed = false;
            }

            if (path_stats != NULL) {
                path_stats->access_count++;
                path_stats->last_accessed = GetCurrentTimestamp();

                /* Update type info */
                JsonPathType current_type;
                switch (v.type) {
                case jbvNull:
                    current_type = JSON_PATH_NULL;
                    path_stats->null_count++;
                    break;
                case jbvString:
                    current_type = JSON_PATH_STRING;
                    path_stats->avg_value_size =
                        (path_stats->avg_value_size * (path_stats->access_count - 1) +
                         v.val.string.len) /
                        path_stats->access_count;
                    break;
                case jbvNumeric:
                    current_type = JSON_PATH_NUMBER;
                    break;
                case jbvBool:
                    current_type = JSON_PATH_BOOLEAN;
                    break;
                case jbvBinary:
                case jbvArray:
                    current_type = JSON_PATH_ARRAY;
                    break;
                case jbvObject:
                    current_type = JSON_PATH_OBJECT;
                    break;
                default:
                    current_type = JSON_PATH_NULL;
                }

                if (path_stats->value_type == JSON_PATH_NULL)
                    path_stats->value_type = current_type;
                else if (path_stats->value_type != current_type && current_type != JSON_PATH_NULL)
                    path_stats->value_type = JSON_PATH_MIXED;
            }

            /* Recursively analyze nested structures */
            if (v.type == jbvBinary) {
                Jsonb nested;
                memcpy(&nested.root, v.val.binary.data, v.val.binary.len);
                analyze_jsonb_value(&nested, full_path, stats, depth + 1);
            }
            break;
        }

        case WJB_BEGIN_ARRAY:
            array_index = 0;
            break;

        case WJB_END_OBJECT:
        case WJB_END_ARRAY:
            break;

        default:
            break;
        }
    }

    pfree(path_buf.data);
}

JsonPathStatsCollection *json_index_analyze_paths(Oid table_oid, int16 column_attnum,
                                                  int sample_size)
{
    JsonPathStatsCollection *stats;
    Relation relation;
    TableScanDesc scan;
    TupleTableSlot *slot;
    int rows_sampled = 0;
    MemoryContext old_context;

    if (sample_size <= 0)
        sample_size = JSON_INDEX_SAMPLE_SIZE;

    ensure_json_index_context();
    old_context = MemoryContextSwitchTo(JsonIndexContext);

    stats = palloc0(sizeof(JsonPathStatsCollection));
    stats->table_oid = table_oid;
    stats->column_attnum = column_attnum;
    stats->paths = palloc0(JSON_INDEX_MAX_TRACKED_PATHS * sizeof(JsonPathStats));
    stats->path_count = 0;
    stats->total_rows = 0;
    stats->last_analyzed = GetCurrentTimestamp();

    /* Open relation and scan */
    relation = table_open(table_oid, AccessShareLock);
    slot = table_slot_create(relation, NULL);
    scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL);

    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        bool isnull;
        Datum json_datum;

        /* Use reservoir sampling for large tables */
        stats->total_rows++;

        if (rows_sampled >= sample_size) {
            /* Reservoir sampling: replace with probability sample_size/total_rows */
            int64 rand_val = random() % stats->total_rows;
            if (rand_val >= sample_size)
                continue;
        }

        json_datum = slot_getattr(slot, column_attnum, &isnull);
        if (!isnull) {
            Jsonb *jb = DatumGetJsonbP(json_datum);
            analyze_jsonb_value(jb, "", stats, 0);
        }

        rows_sampled++;

        /* Allow interrupts */
        CHECK_FOR_INTERRUPTS();
    }

    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    table_close(relation, AccessShareLock);

    /* Calculate selectivity for each path */
    for (int i = 0; i < stats->path_count; i++) {
        JsonPathStats *ps = &stats->paths[i];
        if (ps->access_count > 0 && stats->total_rows > 0) {
            /* Estimate distinct count using HyperLogLog-like approach */
            ps->distinct_count = ps->access_count / 10; /* Simplified estimation */
            if (ps->distinct_count < 1)
                ps->distinct_count = 1;

            ps->selectivity = (double)ps->distinct_count / stats->total_rows;
            if (ps->selectivity > 1.0)
                ps->selectivity = 1.0;
        }

        /* Recommend index type */
        ps->recommended_index = recommend_index_type(ps);
    }

    MemoryContextSwitchTo(old_context);
    return stats;
}

void json_index_record_access(Oid table_oid, int16 column_attnum, const char *path)
{
    /* This would update shared memory statistics in production */
    /* For now, we just validate the path */
    if (!json_index_is_valid_path(path)) {
        elog(DEBUG1, "Invalid JSON path recorded: %s", path);
    }
}

JsonPathStats *json_index_get_path_stats(Oid table_oid, int16 column_attnum, const char *path)
{
    JsonPathStatsCollection *all_stats;
    char *normalized_path;

    all_stats = json_index_get_all_stats(table_oid, column_attnum);
    if (all_stats == NULL)
        return NULL;

    normalized_path = json_index_normalize_path(path);

    for (int i = 0; i < all_stats->path_count; i++) {
        if (strcmp(all_stats->paths[i].path, normalized_path) == 0) {
            JsonPathStats *result = palloc(sizeof(JsonPathStats));
            memcpy(result, &all_stats->paths[i], sizeof(JsonPathStats));
            result->path = pstrdup(all_stats->paths[i].path);
            return result;
        }
    }

    return NULL;
}

JsonPathStatsCollection *json_index_get_all_stats(Oid table_oid, int16 column_attnum)
{
    /* In production, this would read from catalog table */
    /* For now, perform fresh analysis */
    return json_index_analyze_paths(table_oid, column_attnum, JSON_INDEX_SAMPLE_SIZE);
}

void json_index_reset_stats(Oid table_oid, int16 column_attnum)
{
    /* Reset statistics in catalog */
    int ret;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    SPI_execute_with_args("DELETE FROM orochi.orochi_json_path_stats "
                          "WHERE table_oid = $1 AND column_attnum = $2",
                          2, (Oid[]){ OIDOID, INT2OID },
                          (Datum[]){ ObjectIdGetDatum(table_oid), Int16GetDatum(column_attnum) },
                          NULL, false, 0);

    SPI_finish();
}

void json_index_free_stats(JsonPathStatsCollection *stats)
{
    if (stats == NULL)
        return;

    for (int i = 0; i < stats->path_count; i++) {
        if (stats->paths[i].path)
            pfree(stats->paths[i].path);
    }

    if (stats->paths)
        pfree(stats->paths);

    pfree(stats);
}

/* ============================================================
 * Index Recommendation Implementation
 * ============================================================ */

static JsonIndexType recommend_index_type(JsonPathStats *path_stats)
{
    if (path_stats == NULL)
        return JSON_INDEX_GIN;

    /* Low cardinality strings -> BTREE or HASH */
    if (path_stats->value_type == JSON_PATH_STRING) {
        if (path_stats->selectivity < 0.01)
            return JSON_INDEX_BTREE;
        else if (path_stats->selectivity < 0.1)
            return JSON_INDEX_HASH;
    }

    /* Numbers -> BTREE for range queries */
    if (path_stats->value_type == JSON_PATH_NUMBER)
        return JSON_INDEX_BTREE;

    /* Booleans -> Partial index */
    if (path_stats->value_type == JSON_PATH_BOOLEAN)
        return JSON_INDEX_PARTIAL;

    /* Arrays/Objects -> GIN */
    if (path_stats->value_type == JSON_PATH_ARRAY || path_stats->value_type == JSON_PATH_OBJECT)
        return JSON_INDEX_GIN;

    /* Default to GIN for general JSONB */
    return JSON_INDEX_GIN;
}

static char *build_index_create_sql(Oid table_oid, int16 column_attnum, const char *path,
                                    JsonIndexType type)
{
    StringInfoData buf;
    char *table_name;
    char *column_name;
    char *schema_name;
    char *safe_path;

    /* Get table and column names */
    table_name = get_rel_name(table_oid);
    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    column_name = get_attname(table_oid, column_attnum, false);

    /* Make path safe for index name */
    safe_path = pstrdup(path);
    for (char *p = safe_path; *p; p++) {
        if (*p == '.' || *p == '[' || *p == ']' || *p == '$' || *p == '*')
            *p = '_';
    }

    initStringInfo(&buf);

    switch (type) {
    case JSON_INDEX_GIN:
        appendStringInfo(&buf, "CREATE INDEX idx_%s_%s_gin ON %s.%s USING gin ((%s -> '%s'))",
                         table_name, safe_path, schema_name, table_name, column_name, path);
        break;

    case JSON_INDEX_BTREE:
        appendStringInfo(&buf, "CREATE INDEX idx_%s_%s_btree ON %s.%s ((%s ->> '%s'))", table_name,
                         safe_path, schema_name, table_name, column_name, path);
        break;

    case JSON_INDEX_HASH:
        appendStringInfo(&buf, "CREATE INDEX idx_%s_%s_hash ON %s.%s USING hash ((%s ->> '%s'))",
                         table_name, safe_path, schema_name, table_name, column_name, path);
        break;

    case JSON_INDEX_EXPRESSION:
        appendStringInfo(&buf, "CREATE INDEX idx_%s_%s_expr ON %s.%s ((%s #>> '{%s}'))", table_name,
                         safe_path, schema_name, table_name, column_name, path);
        break;

    case JSON_INDEX_PARTIAL:
        appendStringInfo(&buf,
                         "CREATE INDEX idx_%s_%s_partial ON %s.%s ((%s ->> '%s')) "
                         "WHERE %s ? '%s'",
                         table_name, safe_path, schema_name, table_name, column_name, path,
                         column_name, path);
        break;
    }

    pfree(safe_path);
    return buf.data;
}

List *json_index_get_recommendations(Oid table_oid, int16 column_attnum, int max_recommendations)
{
    JsonPathStatsCollection *stats;
    List *recommendations = NIL;
    MemoryContext old_context;

    if (max_recommendations <= 0)
        max_recommendations = 10;

    stats = json_index_analyze_paths(table_oid, column_attnum, JSON_INDEX_SAMPLE_SIZE);
    if (stats == NULL)
        return NIL;

    ensure_json_index_context();
    old_context = MemoryContextSwitchTo(JsonIndexContext);

    /* Sort paths by access count (descending) */
    for (int i = 0; i < stats->path_count - 1; i++) {
        for (int j = i + 1; j < stats->path_count; j++) {
            if (stats->paths[j].access_count > stats->paths[i].access_count) {
                JsonPathStats temp = stats->paths[i];
                stats->paths[i] = stats->paths[j];
                stats->paths[j] = temp;
            }
        }
    }

    /* Generate recommendations for top paths */
    for (int i = 0; i < stats->path_count && i < max_recommendations; i++) {
        JsonPathStats *ps = &stats->paths[i];
        JsonIndexRecommendation *rec;

        /* Skip already indexed paths */
        if (ps->is_indexed)
            continue;

        /* Skip paths with low access count */
        if (ps->access_count < JSON_INDEX_MIN_ACCESS_COUNT)
            continue;

        rec = palloc0(sizeof(JsonIndexRecommendation));
        rec->path = pstrdup(ps->path);
        rec->index_type = ps->recommended_index;
        rec->estimated_benefit = (1.0 - ps->selectivity) * ps->access_count;
        rec->estimated_size = stats->total_rows * ps->avg_value_size / 4;
        rec->create_statement =
            build_index_create_sql(table_oid, column_attnum, ps->path, rec->index_type);
        rec->priority = i + 1;

        /* Build reason */
        StringInfoData reason;
        initStringInfo(&reason);
        appendStringInfo(&reason,
                         "Path '%s' accessed %ld times with %.2f%% selectivity. "
                         "Recommended %s index.",
                         ps->path, ps->access_count, ps->selectivity * 100,
                         json_index_type_name(rec->index_type));
        rec->reason = reason.data;

        recommendations = lappend(recommendations, rec);
    }

    MemoryContextSwitchTo(old_context);
    json_index_free_stats(stats);

    return recommendations;
}

int json_index_apply_recommendations(Oid table_oid, int16 column_attnum, int max_indexes)
{
    List *recommendations;
    ListCell *lc;
    int created = 0;
    int ret;

    recommendations = json_index_get_recommendations(table_oid, column_attnum, max_indexes);
    if (recommendations == NIL)
        return 0;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT) {
        json_index_free_recommendations(recommendations);
        elog(ERROR, "SPI_connect failed: %d", ret);
    }

    foreach (lc, recommendations) {
        JsonIndexRecommendation *rec = lfirst(lc);

        if (created >= max_indexes)
            break;

        ret = SPI_execute(rec->create_statement, false, 0);
        if (ret == SPI_OK_UTILITY) {
            created++;
            elog(NOTICE, "Created JSON index: %s", rec->create_statement);
        } else {
            elog(WARNING, "Failed to create JSON index: %s", rec->create_statement);
        }
    }

    SPI_finish();
    json_index_free_recommendations(recommendations);

    return created;
}

double json_index_estimate_benefit(Oid table_oid, int16 column_attnum, const char *path,
                                   JsonIndexType type)
{
    JsonPathStats *stats;
    double benefit = 0.0;

    stats = json_index_get_path_stats(table_oid, column_attnum, path);
    if (stats == NULL)
        return 0.0;

    /* Benefit = query_speedup * access_frequency */
    benefit = (1.0 - stats->selectivity) * stats->access_count;

    /* Adjust for index type overhead */
    switch (type) {
    case JSON_INDEX_GIN:
        benefit *= 0.8; /* GIN has higher maintenance cost */
        break;
    case JSON_INDEX_BTREE:
        benefit *= 1.0; /* BTREE is most efficient */
        break;
    case JSON_INDEX_HASH:
        benefit *= 0.9; /* Hash is good for equality */
        break;
    default:
        benefit *= 0.7;
        break;
    }

    pfree(stats);
    return benefit;
}

void json_index_free_recommendations(List *recommendations)
{
    ListCell *lc;

    foreach (lc, recommendations) {
        JsonIndexRecommendation *rec = lfirst(lc);
        if (rec->path)
            pfree(rec->path);
        if (rec->create_statement)
            pfree(rec->create_statement);
        if (rec->reason)
            pfree(rec->reason);
        pfree(rec);
    }

    list_free(recommendations);
}

/* ============================================================
 * Index Creation Implementation
 * ============================================================ */

Oid json_index_create(Oid table_oid, int16 column_attnum, const char **paths, int path_count,
                      JsonIndexType index_type)
{
    return json_index_create_with_options(table_oid, column_attnum, paths, path_count, index_type,
                                          false, NULL);
}

Oid json_index_create_with_options(Oid table_oid, int16 column_attnum, const char **paths,
                                   int path_count, JsonIndexType index_type, bool is_unique,
                                   const char *predicate)
{
    StringInfoData sql;
    int ret;
    Oid index_oid = InvalidOid;
    char *table_name;
    char *schema_name;
    char *column_name;

    if (path_count <= 0 || paths == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("at least one path must be specified")));

    /* Validate all paths */
    for (int i = 0; i < path_count; i++) {
        if (!json_index_is_valid_path(paths[i]))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("invalid JSON path: %s", paths[i])));
    }

    table_name = get_rel_name(table_oid);
    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    column_name = get_attname(table_oid, column_attnum, false);

    initStringInfo(&sql);

    /* Build CREATE INDEX statement */
    appendStringInfo(&sql, "CREATE %sINDEX idx_%s_%s_json ON %s.%s ", is_unique ? "UNIQUE " : "",
                     table_name, column_name, schema_name, table_name);

    /* Index method and expression */
    switch (index_type) {
    case JSON_INDEX_GIN:
        if (path_count == 1) {
            appendStringInfo(&sql, "USING gin ((%s -> '%s'))", column_name, paths[0]);
        } else {
            appendStringInfo(&sql, "USING gin (%s jsonb_path_ops)", column_name);
        }
        break;

    case JSON_INDEX_BTREE:
        appendStringInfo(&sql, "(");
        for (int i = 0; i < path_count; i++) {
            if (i > 0)
                appendStringInfo(&sql, ", ");
            appendStringInfo(&sql, "(%s ->> '%s')", column_name, paths[i]);
        }
        appendStringInfo(&sql, ")");
        break;

    case JSON_INDEX_HASH:
        if (path_count > 1)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("hash index supports only one path")));
        appendStringInfo(&sql, "USING hash ((%s ->> '%s'))", column_name, paths[0]);
        break;

    case JSON_INDEX_EXPRESSION:
    case JSON_INDEX_PARTIAL:
        appendStringInfo(&sql, "(");
        for (int i = 0; i < path_count; i++) {
            if (i > 0)
                appendStringInfo(&sql, ", ");
            appendStringInfo(&sql, "(%s #>> '{%s}')", column_name, paths[i]);
        }
        appendStringInfo(&sql, ")");
        break;
    }

    /* Add predicate for partial index */
    if (predicate != NULL && *predicate != '\0') {
        appendStringInfo(&sql, " WHERE %s", predicate);
    } else if (index_type == JSON_INDEX_PARTIAL && path_count == 1) {
        appendStringInfo(&sql, " WHERE %s ? '%s'", column_name, paths[0]);
    }

    /* Execute the CREATE INDEX */
    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute(sql.data, false, 0);
    if (ret != SPI_OK_UTILITY) {
        SPI_finish();
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create JSON index"),
                        errdetail("SQL: %s", sql.data)));
    }

    /* Look up the created index OID by name */
    {
        StringInfoData lookup_sql;
        char *index_name;

        index_name = psprintf("idx_%s_%s_json", table_name, column_name);

        initStringInfo(&lookup_sql);
        appendStringInfo(&lookup_sql,
                         "SELECT oid FROM pg_class "
                         "WHERE relname = '%s' AND relnamespace = "
                         "(SELECT oid FROM pg_namespace WHERE nspname = '%s')",
                         index_name, schema_name);

        ret = SPI_execute(lookup_sql.data, true, 1);
        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            bool isnull;
            Datum oid_datum =
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
            if (!isnull)
                index_oid = DatumGetObjectId(oid_datum);
        }

        pfree(lookup_sql.data);
        pfree(index_name);
    }

    SPI_finish();

    elog(NOTICE, "Created JSON index with OID %u", index_oid);

    pfree(sql.data);
    return index_oid;
}

void json_index_drop(Oid index_oid)
{
    char *index_name;
    char *schema_name;
    StringInfoData sql;
    int ret;

    index_name = get_rel_name(index_oid);
    if (index_name == NULL)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("index with OID %u does not exist", index_oid)));

    schema_name = get_namespace_name(get_rel_namespace(index_oid));

    initStringInfo(&sql);
    appendStringInfo(&sql, "DROP INDEX %s.%s", schema_name, index_name);

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute(sql.data, false, 0);
    if (ret != SPI_OK_UTILITY) {
        SPI_finish();
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to drop JSON index")));
    }

    SPI_finish();
    pfree(sql.data);
}

bool json_index_path_is_indexed(Oid table_oid, int16 column_attnum, const char *path)
{
    List *indexes;
    ListCell *lc;
    bool found = false;

    indexes = json_index_get_indexes(table_oid, column_attnum);

    foreach (lc, indexes) {
        JsonIndexInfo *info = lfirst(lc);
        for (int i = 0; i < info->path_count; i++) {
            if (strcmp(info->paths[i], path) == 0) {
                found = true;
                break;
            }
        }
        json_index_free_info(info);
        if (found)
            break;
    }

    list_free(indexes);
    return found;
}

List *json_index_get_indexes(Oid table_oid, int16 column_attnum)
{
    List *result = NIL;
    Relation rel;
    List *index_oids;
    ListCell *lc;

    rel = table_open(table_oid, AccessShareLock);
    index_oids = RelationGetIndexList(rel);

    foreach (lc, index_oids) {
        Oid index_oid = lfirst_oid(lc);
        Relation index_rel;
        Form_pg_index index_form;

        index_rel = index_open(index_oid, AccessShareLock);
        index_form = index_rel->rd_index;

        /* Check if this index involves our JSONB column */
        for (int i = 0; i < index_form->indnatts; i++) {
            if (index_form->indkey.values[i] == column_attnum) {
                JsonIndexInfo *info = json_index_get_info(index_oid);
                if (info != NULL)
                    result = lappend(result, info);
                break;
            }
        }

        index_close(index_rel, AccessShareLock);
    }

    table_close(rel, AccessShareLock);
    list_free(index_oids);

    return result;
}

JsonIndexInfo *json_index_get_info(Oid index_oid)
{
    JsonIndexInfo *info;
    Relation index_rel;
    char *index_name;

    index_name = get_rel_name(index_oid);
    if (index_name == NULL)
        return NULL;

    index_rel = index_open(index_oid, AccessShareLock);

    info = palloc0(sizeof(JsonIndexInfo));
    info->index_oid = index_oid;
    info->table_oid = index_rel->rd_index->indrelid;
    info->index_name = pstrdup(index_name);
    info->path_count = 0;
    info->paths = NULL;
    info->is_unique = index_rel->rd_index->indisunique;
    info->size_bytes = 0; /* Would need pg_relation_size() */
    info->created_at = GetCurrentTimestamp();

    /* Determine index type from access method */
    if (index_rel->rd_rel->relam == GIN_AM_OID)
        info->index_type = JSON_INDEX_GIN;
    else if (index_rel->rd_rel->relam == BTREE_AM_OID)
        info->index_type = JSON_INDEX_BTREE;
    else if (index_rel->rd_rel->relam == HASH_AM_OID)
        info->index_type = JSON_INDEX_HASH;
    else
        info->index_type = JSON_INDEX_EXPRESSION;

    index_close(index_rel, AccessShareLock);

    return info;
}

void json_index_free_info(JsonIndexInfo *info)
{
    if (info == NULL)
        return;

    if (info->index_name)
        pfree(info->index_name);

    for (int i = 0; i < info->path_count; i++) {
        if (info->paths[i])
            pfree(info->paths[i]);
    }

    if (info->paths)
        pfree(info->paths);

    if (info->partial_predicate)
        pfree(info->partial_predicate);

    pfree(info);
}

/* ============================================================
 * GIN Index Support
 * ============================================================ */

Oid json_index_create_gin(Oid table_oid, int16 column_attnum, const char *operator_class)
{
    StringInfoData sql;
    int ret;
    char *table_name;
    char *schema_name;
    char *column_name;

    table_name = get_rel_name(table_oid);
    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    column_name = get_attname(table_oid, column_attnum, false);

    initStringInfo(&sql);

    if (operator_class && *operator_class) {
        appendStringInfo(&sql, "CREATE INDEX idx_%s_%s_gin ON %s.%s USING gin (%s %s)", table_name,
                         column_name, schema_name, table_name, column_name, operator_class);
    } else {
        appendStringInfo(&sql, "CREATE INDEX idx_%s_%s_gin ON %s.%s USING gin (%s)", table_name,
                         column_name, schema_name, table_name, column_name);
    }

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute(sql.data, false, 0);
    SPI_finish();

    if (ret != SPI_OK_UTILITY)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create GIN index")));

    pfree(sql.data);
    return InvalidOid; /* Would return actual OID in production */
}

Oid json_index_create_gin_path_ops(Oid table_oid, int16 column_attnum)
{
    return json_index_create_gin(table_oid, column_attnum, "jsonb_path_ops");
}

bool json_index_gin_can_accelerate(Oid index_oid, const char *path, int operator_type)
{
    Relation index_rel;
    bool can_accelerate = false;

    index_rel = index_open(index_oid, AccessShareLock);

    /* GIN with jsonb_ops can accelerate @>, ?, ?|, ?& operators */
    /* GIN with jsonb_path_ops can only accelerate @> operator */
    if (index_rel->rd_rel->relam == GIN_AM_OID) {
        /* Check operator class */
        /* Simplified check - would need to examine index options */
        can_accelerate = true;
    }

    index_close(index_rel, AccessShareLock);
    return can_accelerate;
}

/* ============================================================
 * Expression Index Support
 * ============================================================ */

Oid json_index_create_expression(Oid table_oid, int16 column_attnum, const char *path,
                                 Oid cast_type)
{
    StringInfoData sql;
    int ret;
    char *table_name;
    char *schema_name;
    char *column_name;
    char *type_name;
    char *safe_path;

    table_name = get_rel_name(table_oid);
    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    column_name = get_attname(table_oid, column_attnum, false);

    /* Get type name for cast */
    if (cast_type != InvalidOid)
        type_name = format_type_be(cast_type);
    else
        type_name = "text";

    /* Make path safe for index name */
    safe_path = pstrdup(path);
    for (char *p = safe_path; *p; p++) {
        if (*p == '.' || *p == '[' || *p == ']' || *p == '$')
            *p = '_';
    }

    initStringInfo(&sql);
    appendStringInfo(&sql, "CREATE INDEX idx_%s_%s_expr ON %s.%s (((%s ->> '%s')::%s))", table_name,
                     safe_path, schema_name, table_name, column_name, path, type_name);

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute(sql.data, false, 0);
    SPI_finish();

    if (ret != SPI_OK_UTILITY)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create expression index")));

    pfree(sql.data);
    pfree(safe_path);
    return InvalidOid;
}

Oid json_index_create_composite(Oid table_oid, int16 column_attnum, const char **paths,
                                Oid *cast_types, int path_count)
{
    StringInfoData sql;
    int ret;
    char *table_name;
    char *schema_name;
    char *column_name;

    if (path_count <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("at least one path required")));

    table_name = get_rel_name(table_oid);
    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    column_name = get_attname(table_oid, column_attnum, false);

    initStringInfo(&sql);
    appendStringInfo(&sql, "CREATE INDEX idx_%s_%s_composite ON %s.%s (", table_name, column_name,
                     schema_name, table_name);

    for (int i = 0; i < path_count; i++) {
        char *type_name;

        if (i > 0)
            appendStringInfo(&sql, ", ");

        if (cast_types && cast_types[i] != InvalidOid)
            type_name = format_type_be(cast_types[i]);
        else
            type_name = "text";

        appendStringInfo(&sql, "((%s ->> '%s')::%s)", column_name, paths[i], type_name);
    }

    appendStringInfo(&sql, ")");

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %d", ret);

    ret = SPI_execute(sql.data, false, 0);
    SPI_finish();

    if (ret != SPI_OK_UTILITY)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create composite index")));

    pfree(sql.data);
    return InvalidOid;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_json_index);
Datum orochi_create_json_index(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *column_name_text = PG_GETARG_TEXT_PP(1);
    ArrayType *paths_array = PG_GETARG_ARRAYTYPE_P(2);
    char *column_name;
    int16 column_attnum;
    const char **paths;
    int path_count;
    Datum *path_datums;
    bool *path_nulls;
    Oid index_oid;

    column_name = text_to_cstring(column_name_text);

    /* Get column attribute number */
    column_attnum = get_attnum(table_oid, column_name);
    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" does not exist", column_name)));

    /* Extract paths from array */
    deconstruct_array(paths_array, TEXTOID, -1, false, TYPALIGN_INT, &path_datums, &path_nulls,
                      &path_count);

    paths = palloc(path_count * sizeof(char *));
    for (int i = 0; i < path_count; i++) {
        if (path_nulls[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("path cannot be null")));
        paths[i] = TextDatumGetCString(path_datums[i]);
    }

    /* Create the index */
    index_oid = json_index_create(table_oid, column_attnum, paths, path_count, JSON_INDEX_GIN);

    pfree(paths);
    PG_RETURN_OID(index_oid);
}

PG_FUNCTION_INFO_V1(orochi_analyze_json_paths);
Datum orochi_analyze_json_paths(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *column_name_text = PG_GETARG_TEXT_PP(1);
    char *column_name;
    int16 column_attnum;
    JsonPathStatsCollection *stats;
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" does not exist", column_name)));

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext old_context;
        TupleDesc tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Analyze paths */
        stats = json_index_analyze_paths(table_oid, column_attnum, JSON_INDEX_SAMPLE_SIZE);
        funcctx->user_fctx = stats;
        funcctx->max_calls = stats->path_count;

        /* Build tuple descriptor */
        tupdesc = CreateTemplateTupleDesc(7);
        TupleDescInitEntry(tupdesc, 1, "path", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "value_type", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "access_count", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "distinct_count", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "selectivity", FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 6, "is_indexed", BOOLOID, -1, 0);
        TupleDescInitEntry(tupdesc, 7, "recommended_index", TEXTOID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    stats = funcctx->user_fctx;
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if (call_cntr < max_calls) {
        JsonPathStats *ps = &stats->paths[call_cntr];
        Datum values[7];
        bool nulls[7] = { false };
        HeapTuple tuple;
        const char *type_names[] = { "null",  "string", "number", "boolean",
                                     "array", "object", "mixed" };

        values[0] = CStringGetTextDatum(ps->path);
        values[1] = CStringGetTextDatum(type_names[ps->value_type]);
        values[2] = Int64GetDatum(ps->access_count);
        values[3] = Int64GetDatum(ps->distinct_count);
        values[4] = Float8GetDatum(ps->selectivity);
        values[5] = BoolGetDatum(ps->is_indexed);
        values[6] = CStringGetTextDatum(json_index_type_name(ps->recommended_index));

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    json_index_free_stats(stats);
    SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(orochi_json_index_recommendations);
Datum orochi_json_index_recommendations(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *column_name_text = PG_GETARG_TEXT_PP(1);
    int32 max_recs = PG_GETARG_INT32(2);
    char *column_name;
    int16 column_attnum;
    List *recommendations;
    FuncCallContext *funcctx;
    ListCell *lc;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" does not exist", column_name)));

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext old_context;
        TupleDesc tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        recommendations = json_index_get_recommendations(table_oid, column_attnum, max_recs);
        funcctx->user_fctx = recommendations;
        funcctx->max_calls = list_length(recommendations);

        tupdesc = CreateTemplateTupleDesc(5);
        TupleDescInitEntry(tupdesc, 1, "path", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "index_type", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "priority", INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "create_statement", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "reason", TEXTOID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    recommendations = funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls) {
        JsonIndexRecommendation *rec;
        Datum values[5];
        bool nulls[5] = { false };
        HeapTuple tuple;

        rec = list_nth(recommendations, funcctx->call_cntr);

        values[0] = CStringGetTextDatum(rec->path);
        values[1] = CStringGetTextDatum(json_index_type_name(rec->index_type));
        values[2] = Int32GetDatum(rec->priority);
        values[3] = CStringGetTextDatum(rec->create_statement);
        values[4] = CStringGetTextDatum(rec->reason);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    json_index_free_recommendations(recommendations);
    SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(orochi_json_path_is_indexed);
Datum orochi_json_path_is_indexed(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *column_name_text = PG_GETARG_TEXT_PP(1);
    text *path_text = PG_GETARG_TEXT_PP(2);
    char *column_name;
    char *path;
    int16 column_attnum;
    bool is_indexed;

    column_name = text_to_cstring(column_name_text);
    path = text_to_cstring(path_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" does not exist", column_name)));

    is_indexed = json_index_path_is_indexed(table_oid, column_attnum, path);

    PG_RETURN_BOOL(is_indexed);
}

PG_FUNCTION_INFO_V1(orochi_json_path_stats);
Datum orochi_json_path_stats(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    text *column_name_text = PG_GETARG_TEXT_PP(1);
    text *path_text = PG_GETARG_TEXT_PP(2);
    char *column_name;
    char *path;
    int16 column_attnum;
    JsonPathStats *stats;
    TupleDesc tupdesc;
    Datum values[6];
    bool nulls[6] = { false };
    HeapTuple tuple;

    column_name = text_to_cstring(column_name_text);
    path = text_to_cstring(path_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" does not exist", column_name)));

    stats = json_index_get_path_stats(table_oid, column_attnum, path);

    if (stats == NULL)
        PG_RETURN_NULL();

    tupdesc = CreateTemplateTupleDesc(6);
    TupleDescInitEntry(tupdesc, 1, "path", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "access_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "distinct_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "null_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 5, "selectivity", FLOAT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 6, "avg_value_size", FLOAT8OID, -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = CStringGetTextDatum(stats->path);
    values[1] = Int64GetDatum(stats->access_count);
    values[2] = Int64GetDatum(stats->distinct_count);
    values[3] = Int64GetDatum(stats->null_count);
    values[4] = Float8GetDatum(stats->selectivity);
    values[5] = Float8GetDatum(stats->avg_value_size);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    pfree(stats);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
