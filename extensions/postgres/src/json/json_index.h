/*-------------------------------------------------------------------------
 *
 * json_index.h
 *    Orochi DB JSON path indexing for semi-structured data
 *
 * This module provides:
 *   - Automatic index creation on frequently accessed JSON paths
 *   - GIN index integration for JSONB columns
 *   - Path statistics collection and analysis
 *   - Index advisor for optimal JSON indexing strategies
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JSON_INDEX_H
#define OROCHI_JSON_INDEX_H

#include "postgres.h"
#include "fmgr.h"
#include "access/gin.h"
#include "nodes/pg_list.h"
#include "utils/jsonb.h"

#include "../orochi.h"

/* ============================================================
 * Configuration Constants
 * ============================================================ */

/* Maximum number of paths to track per table */
#define JSON_INDEX_MAX_TRACKED_PATHS        1000

/* Minimum access count before suggesting index */
#define JSON_INDEX_MIN_ACCESS_COUNT         100

/* Default selectivity threshold for index recommendation */
#define JSON_INDEX_DEFAULT_SELECTIVITY      0.1

/* Maximum path depth for indexing */
#define JSON_INDEX_MAX_PATH_DEPTH           16

/* Path analysis sample size */
#define JSON_INDEX_SAMPLE_SIZE              10000

/* ============================================================
 * JSON Path Index Types
 * ============================================================ */

/*
 * JsonIndexType - Type of JSON index
 */
typedef enum JsonIndexType
{
    JSON_INDEX_GIN = 0,           /* GIN index for containment queries */
    JSON_INDEX_BTREE,             /* B-tree for extracted scalar values */
    JSON_INDEX_HASH,              /* Hash index for equality on paths */
    JSON_INDEX_EXPRESSION,        /* Expression index on specific paths */
    JSON_INDEX_PARTIAL            /* Partial index with JSON condition */
} JsonIndexType;

/*
 * JsonPathType - Type of value at a JSON path
 */
typedef enum JsonPathType
{
    JSON_PATH_NULL = 0,
    JSON_PATH_STRING,
    JSON_PATH_NUMBER,
    JSON_PATH_BOOLEAN,
    JSON_PATH_ARRAY,
    JSON_PATH_OBJECT,
    JSON_PATH_MIXED               /* Mixed types at this path */
} JsonPathType;

/* ============================================================
 * JSON Path Statistics
 * ============================================================ */

/*
 * JsonPathStats - Statistics for a single JSON path
 */
typedef struct JsonPathStats
{
    char               *path;               /* JSON path expression */
    JsonPathType        value_type;         /* Predominant value type */
    int64               access_count;       /* Number of accesses */
    int64               null_count;         /* Number of null values */
    int64               distinct_count;     /* Estimated distinct values */
    double              selectivity;        /* Estimated selectivity */
    double              avg_value_size;     /* Average value size in bytes */
    TimestampTz         first_seen;         /* First access timestamp */
    TimestampTz         last_accessed;      /* Last access timestamp */
    bool                is_indexed;         /* Currently indexed? */
    JsonIndexType       recommended_index;  /* Recommended index type */
} JsonPathStats;

/*
 * JsonPathStatsCollection - Collection of path statistics for a table
 */
typedef struct JsonPathStatsCollection
{
    Oid                 table_oid;          /* Table OID */
    int16               column_attnum;      /* JSONB column attribute number */
    int32               path_count;         /* Number of tracked paths */
    JsonPathStats      *paths;              /* Array of path statistics */
    int64               total_rows;         /* Total rows analyzed */
    TimestampTz         last_analyzed;      /* Last analysis timestamp */
} JsonPathStatsCollection;

/* ============================================================
 * JSON Index Metadata
 * ============================================================ */

/*
 * JsonIndexInfo - Information about a JSON index
 */
typedef struct JsonIndexInfo
{
    Oid                 index_oid;          /* PostgreSQL index OID */
    Oid                 table_oid;          /* Parent table OID */
    int16               column_attnum;      /* JSONB column attribute number */
    char               *index_name;         /* Index name */
    JsonIndexType       index_type;         /* Type of index */
    char              **paths;              /* Indexed paths */
    int32               path_count;         /* Number of indexed paths */
    bool                is_unique;          /* Is unique index? */
    bool                is_partial;         /* Is partial index? */
    char               *partial_predicate; /* Partial index predicate */
    int64               size_bytes;         /* Index size */
    TimestampTz         created_at;         /* Creation timestamp */
} JsonIndexInfo;

/*
 * JsonIndexRecommendation - Index recommendation from advisor
 */
typedef struct JsonIndexRecommendation
{
    char               *path;               /* JSON path to index */
    JsonIndexType       index_type;         /* Recommended index type */
    double              estimated_benefit;  /* Estimated query speedup */
    int64               estimated_size;     /* Estimated index size */
    char               *create_statement;   /* DDL to create index */
    char               *reason;             /* Explanation for recommendation */
    int32               priority;           /* Priority (1 = highest) */
} JsonIndexRecommendation;

/* ============================================================
 * JSON Index Operations
 * ============================================================ */

/*
 * Create a JSON path index
 */
extern Oid json_index_create(Oid table_oid, int16 column_attnum,
                             const char **paths, int path_count,
                             JsonIndexType index_type);

/*
 * Create a JSON path index with options
 */
extern Oid json_index_create_with_options(Oid table_oid, int16 column_attnum,
                                          const char **paths, int path_count,
                                          JsonIndexType index_type,
                                          bool is_unique, const char *predicate);

/*
 * Drop a JSON path index
 */
extern void json_index_drop(Oid index_oid);

/*
 * Check if a path is indexed
 */
extern bool json_index_path_is_indexed(Oid table_oid, int16 column_attnum,
                                       const char *path);

/*
 * Get all JSON indexes for a table/column
 */
extern List *json_index_get_indexes(Oid table_oid, int16 column_attnum);

/*
 * Get index info by OID
 */
extern JsonIndexInfo *json_index_get_info(Oid index_oid);

/*
 * Free index info
 */
extern void json_index_free_info(JsonIndexInfo *info);

/* ============================================================
 * Path Statistics Operations
 * ============================================================ */

/*
 * Analyze JSON paths in a table
 */
extern JsonPathStatsCollection *json_index_analyze_paths(Oid table_oid,
                                                         int16 column_attnum,
                                                         int sample_size);

/*
 * Record path access for statistics
 */
extern void json_index_record_access(Oid table_oid, int16 column_attnum,
                                     const char *path);

/*
 * Get path statistics
 */
extern JsonPathStats *json_index_get_path_stats(Oid table_oid,
                                                int16 column_attnum,
                                                const char *path);

/*
 * Get all path statistics for a column
 */
extern JsonPathStatsCollection *json_index_get_all_stats(Oid table_oid,
                                                         int16 column_attnum);

/*
 * Reset path statistics
 */
extern void json_index_reset_stats(Oid table_oid, int16 column_attnum);

/*
 * Free path statistics collection
 */
extern void json_index_free_stats(JsonPathStatsCollection *stats);

/* ============================================================
 * Index Advisor Operations
 * ============================================================ */

/*
 * Get index recommendations for a table/column
 */
extern List *json_index_get_recommendations(Oid table_oid, int16 column_attnum,
                                            int max_recommendations);

/*
 * Apply index recommendations (create recommended indexes)
 */
extern int json_index_apply_recommendations(Oid table_oid, int16 column_attnum,
                                            int max_indexes);

/*
 * Get estimated benefit of indexing a path
 */
extern double json_index_estimate_benefit(Oid table_oid, int16 column_attnum,
                                          const char *path, JsonIndexType type);

/*
 * Free recommendation list
 */
extern void json_index_free_recommendations(List *recommendations);

/* ============================================================
 * GIN Index Support
 * ============================================================ */

/*
 * Create GIN index for JSONB column
 */
extern Oid json_index_create_gin(Oid table_oid, int16 column_attnum,
                                 const char *operator_class);

/*
 * Create GIN index for specific paths (jsonb_path_ops)
 */
extern Oid json_index_create_gin_path_ops(Oid table_oid, int16 column_attnum);

/*
 * Check if GIN index can accelerate a query
 */
extern bool json_index_gin_can_accelerate(Oid index_oid, const char *path,
                                          int operator_type);

/* ============================================================
 * Expression Index Support
 * ============================================================ */

/*
 * Create expression index on JSON path
 */
extern Oid json_index_create_expression(Oid table_oid, int16 column_attnum,
                                        const char *path, Oid cast_type);

/*
 * Create composite expression index on multiple paths
 */
extern Oid json_index_create_composite(Oid table_oid, int16 column_attnum,
                                       const char **paths, Oid *cast_types,
                                       int path_count);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Parse a JSON path string into components
 */
extern char **json_index_parse_path(const char *path, int *depth);

/*
 * Normalize a JSON path
 */
extern char *json_index_normalize_path(const char *path);

/*
 * Check if a path is valid
 */
extern bool json_index_is_valid_path(const char *path);

/*
 * Get the value type at a path (from sample data)
 */
extern JsonPathType json_index_get_path_type(Jsonb *jb, const char *path);

/*
 * Get index type name
 */
extern const char *json_index_type_name(JsonIndexType type);

/*
 * Parse index type from name
 */
extern JsonIndexType json_index_parse_type(const char *name);

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

/* Create JSON index */
extern Datum orochi_create_json_index(PG_FUNCTION_ARGS);

/* Analyze JSON paths */
extern Datum orochi_analyze_json_paths(PG_FUNCTION_ARGS);

/* Get index recommendations */
extern Datum orochi_json_index_recommendations(PG_FUNCTION_ARGS);

/* Check if path is indexed */
extern Datum orochi_json_path_is_indexed(PG_FUNCTION_ARGS);

/* Get path statistics */
extern Datum orochi_json_path_stats(PG_FUNCTION_ARGS);

#endif /* OROCHI_JSON_INDEX_H */
