/*-------------------------------------------------------------------------
 *
 * json_query.h
 *    Orochi DB optimized JSON query execution
 *
 * This module provides:
 *   - Path expression optimization
 *   - Predicate pushdown for JSON filters
 *   - Batch JSON parsing for improved performance
 *   - Query plan integration for JSON operations
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JSON_QUERY_H
#define OROCHI_JSON_QUERY_H

#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "postgres.h"
#include "utils/jsonb.h"

#include "../orochi.h"
#include "json_index.h"

/* ============================================================
 * Configuration Constants
 * ============================================================ */

/* Maximum paths in a single query */
#define JSON_QUERY_MAX_PATHS 32

/* Batch size for JSON parsing */
#define JSON_QUERY_BATCH_SIZE 1000

/* Maximum expression tree depth */
#define JSON_QUERY_MAX_EXPR_DEPTH 16

/* Cache size for parsed JSON paths */
#define JSON_QUERY_PATH_CACHE_SIZE 256

/* ============================================================
 * JSON Query Types
 * ============================================================ */

/*
 * JsonQueryOperator - JSON query operators
 */
typedef enum JsonQueryOperator {
    JSON_OP_CONTAINS = 0,  /* @> containment */
    JSON_OP_CONTAINED_BY,  /* <@ contained by */
    JSON_OP_EXISTS,        /* ? key exists */
    JSON_OP_EXISTS_ANY,    /* ?| any key exists */
    JSON_OP_EXISTS_ALL,    /* ?& all keys exist */
    JSON_OP_PATH_EXISTS,   /* @? path exists */
    JSON_OP_PATH_MATCH,    /* @@ path match */
    JSON_OP_FIELD_ACCESS,  /* -> field access */
    JSON_OP_FIELD_TEXT,    /* ->> field as text */
    JSON_OP_PATH_ACCESS,   /* #> path access */
    JSON_OP_PATH_TEXT,     /* #>> path as text */
    JSON_OP_ARRAY_ELEMENT, /* Array element access */
    JSON_OP_EQ,            /* = equality */
    JSON_OP_NE,            /* != inequality */
    JSON_OP_LT,            /* < less than */
    JSON_OP_LE,            /* <= less or equal */
    JSON_OP_GT,            /* > greater than */
    JSON_OP_GE,            /* >= greater or equal */
    JSON_OP_LIKE,          /* String pattern match */
    JSON_OP_IN,            /* IN list */
    JSON_OP_IS_NULL,       /* IS NULL */
    JSON_OP_IS_NOT_NULL    /* IS NOT NULL */
} JsonQueryOperator;

/*
 * JsonExprType - Type of JSON expression node
 */
typedef enum JsonExprType {
    JSON_EXPR_CONST = 0, /* Constant value */
    JSON_EXPR_PATH,      /* Path reference */
    JSON_EXPR_COLUMN,    /* Column reference */
    JSON_EXPR_OPERATOR,  /* Operator application */
    JSON_EXPR_FUNCTION,  /* Function call */
    JSON_EXPR_AND,       /* AND conjunction */
    JSON_EXPR_OR,        /* OR disjunction */
    JSON_EXPR_NOT        /* NOT negation */
} JsonExprType;

/* ============================================================
 * JSON Query Expression Tree
 * ============================================================ */

/*
 * JsonQueryExpr - JSON query expression node
 */
typedef struct JsonQueryExpr {
    JsonExprType type; /* Expression type */

    /* For CONST */
    Datum const_value; /* Constant value */
    Oid const_type;    /* Value type */
    bool const_isnull; /* Is NULL? */

    /* For PATH */
    char *path;             /* JSON path string */
    char **path_components; /* Parsed path components */
    int path_depth;         /* Path depth */

    /* For COLUMN */
    int16 column_attnum; /* Column attribute number */
    char *column_name;   /* Column name */

    /* For OPERATOR */
    JsonQueryOperator operator;  /* Operator type */
    struct JsonQueryExpr *left;  /* Left operand */
    struct JsonQueryExpr *right; /* Right operand */

    /* For FUNCTION */
    char *func_name; /* Function name */
    List *func_args; /* Function arguments */

    /* For AND/OR */
    List *children; /* Child expressions */

    /* Optimization hints */
    bool can_use_index;   /* Can use JSON index */
    double selectivity;   /* Estimated selectivity */
    int64 estimated_rows; /* Estimated matching rows */
} JsonQueryExpr;

/*
 * JsonQueryPlan - Optimized JSON query execution plan
 */
typedef struct JsonQueryPlan {
    Oid table_oid;            /* Target table */
    int16 column_attnum;      /* JSON column */
    JsonQueryExpr *root_expr; /* Root expression */

    /* Index usage */
    List *usable_indexes; /* List of Oid */
    Oid selected_index;   /* Best index to use */

    /* Extracted columns */
    List *extracted_columns; /* Column IDs to use */

    /* Optimization flags */
    bool use_columnar; /* Use columnar storage */
    bool use_batch;    /* Use batch processing */
    bool push_down;    /* Push predicates down */

    /* Cost estimates */
    double startup_cost;  /* Startup cost */
    double total_cost;    /* Total cost */
    int64 estimated_rows; /* Estimated result rows */
} JsonQueryPlan;

/* ============================================================
 * JSON Query Execution State
 * ============================================================ */

/*
 * JsonQueryState - Runtime state for JSON query execution
 */
typedef struct JsonQueryState {
    JsonQueryPlan *plan; /* Query plan */
    EState *estate;      /* Executor state */

    /* Scan state */
    TableScanDesc scan;   /* Table scan descriptor */
    TupleTableSlot *slot; /* Current tuple slot */
    int64 current_row;    /* Current row number */

    /* Batch processing */
    Datum *batch_values; /* Batch of JSON values */
    bool *batch_nulls;   /* Batch null flags */
    bool *batch_results; /* Batch filter results */
    int batch_size;      /* Current batch size */
    int batch_pos;       /* Position in batch */

    /* Index scan state */
    IndexScanDesc index_scan; /* Index scan descriptor */
    bool use_index;           /* Using index scan */

    /* Result accumulator */
    int64 rows_scanned; /* Total rows scanned */
    int64 rows_matched; /* Rows matching filter */

    /* Memory context */
    MemoryContext query_context;
} JsonQueryState;

/* ============================================================
 * Query Planning Functions
 * ============================================================ */

/*
 * Parse a JSON query expression from SQL
 */
extern JsonQueryExpr *json_query_parse(const char *query_text);

/*
 * Build expression from PostgreSQL expression tree
 */
extern JsonQueryExpr *json_query_build_expr(Node *pg_expr, Oid table_oid, int16 column_attnum);

/*
 * Optimize a JSON query expression
 */
extern JsonQueryExpr *json_query_optimize(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum);

/*
 * Create execution plan for JSON query
 */
extern JsonQueryPlan *json_query_plan(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum);

/*
 * Free query expression
 */
extern void json_query_free_expr(JsonQueryExpr *expr);

/*
 * Free query plan
 */
extern void json_query_free_plan(JsonQueryPlan *plan);

/* ============================================================
 * Predicate Pushdown
 * ============================================================ */

/*
 * Check if expression can be pushed down
 */
extern bool json_query_can_pushdown(JsonQueryExpr *expr);

/*
 * Extract pushable predicates
 */
extern List *json_query_extract_pushable(JsonQueryExpr *expr);

/*
 * Apply predicate pushdown
 */
extern JsonQueryExpr *json_query_apply_pushdown(JsonQueryExpr *expr, List *pushed_predicates);

/*
 * Convert pushed predicate to scan key
 */
extern ScanKey json_query_to_scankey(JsonQueryExpr *expr, int *nkeys);

/* ============================================================
 * Index Integration
 * ============================================================ */

/*
 * Check if expression can use an index
 */
extern bool json_query_index_compatible(JsonQueryExpr *expr, Oid index_oid);

/*
 * Select best index for query
 */
extern Oid json_query_select_index(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum);

/*
 * Build index scan parameters
 */
extern void json_query_build_index_scan(JsonQueryExpr *expr, Oid index_oid, ScanKey *keys,
                                        int *nkeys);

/* ============================================================
 * Query Execution Functions
 * ============================================================ */

/*
 * Begin JSON query execution
 */
extern JsonQueryState *json_query_begin(JsonQueryPlan *plan, EState *estate);

/*
 * Get next matching row
 */
extern bool json_query_next(JsonQueryState *state, TupleTableSlot *slot);

/*
 * Get next batch of matching rows
 */
extern int json_query_next_batch(JsonQueryState *state, TupleTableSlot **slots, int max_slots);

/*
 * Rescan JSON query
 */
extern void json_query_rescan(JsonQueryState *state);

/*
 * End JSON query execution
 */
extern void json_query_end(JsonQueryState *state);

/* ============================================================
 * Expression Evaluation
 * ============================================================ */

/*
 * Evaluate expression against a JSONB value
 */
extern bool json_query_eval(JsonQueryExpr *expr, Jsonb *value, bool *is_null);

/*
 * Evaluate expression and return result
 */
extern Datum json_query_eval_datum(JsonQueryExpr *expr, Jsonb *value, Oid *result_type,
                                   bool *is_null);

/*
 * Evaluate batch of values
 */
extern void json_query_eval_batch(JsonQueryExpr *expr, Jsonb **values, int count, bool *results);

/* ============================================================
 * Batch Parsing Functions
 * ============================================================ */

/*
 * BatchParseContext - Context for batch JSON parsing
 */
typedef struct BatchParseContext {
    MemoryContext parse_context; /* Memory context */
    int batch_size;              /* Batch size */

    /* Path extraction cache */
    struct PathCache {
        char *path;
        int depth;
        char **components;
    } *path_cache;
    int cache_count;
    int cache_size;

    /* Statistics */
    int64 values_parsed;
    int64 cache_hits;
    int64 cache_misses;
} BatchParseContext;

/*
 * Initialize batch parsing context
 */
extern BatchParseContext *json_query_batch_init(int batch_size);

/*
 * Parse batch of JSONB values
 */
extern void json_query_batch_parse(BatchParseContext *ctx, Datum *json_datums, int count,
                                   const char **paths, int path_count, Datum **results,
                                   bool **nulls);

/*
 * Extract paths from batch of values
 */
extern void json_query_batch_extract(BatchParseContext *ctx, Jsonb **values, int count,
                                     const char *path, Oid target_type, Datum *results,
                                     bool *nulls);

/*
 * Free batch parsing context
 */
extern void json_query_batch_free(BatchParseContext *ctx);

/* ============================================================
 * Path Expression Optimization
 * ============================================================ */

/*
 * Simplify path expression
 */
extern char *json_query_simplify_path(const char *path);

/*
 * Merge multiple path accesses
 */
extern List *json_query_merge_paths(List *path_exprs);

/*
 * Reorder path accesses for efficiency
 */
extern List *json_query_reorder_paths(List *path_exprs, JsonPathStatsCollection *stats);

/*
 * Cache parsed path for reuse
 */
extern void json_query_cache_path(BatchParseContext *ctx, const char *path);

/* ============================================================
 * Cost Estimation
 * ============================================================ */

/*
 * Estimate cost of JSON operation
 */
extern void json_query_estimate_cost(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum,
                                     double *startup_cost, double *total_cost, double *selectivity,
                                     int64 *rows);

/*
 * Estimate cost with index
 */
extern void json_query_estimate_index_cost(JsonQueryExpr *expr, Oid index_oid, double *startup_cost,
                                           double *total_cost);

/* ============================================================
 * Planner Hook Integration
 * ============================================================ */

/*
 * JSON query planner hook
 */
extern void json_query_planner_hook(PlannerInfo *root, RelOptInfo *rel, Oid table_oid,
                                    RangeTblEntry *rte);

/*
 * Add JSON-specific paths to planner
 */
extern void json_query_add_paths(PlannerInfo *root, RelOptInfo *rel, Oid table_oid,
                                 int16 column_attnum);

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

/* Optimized JSON path query */
extern Datum orochi_json_query(PG_FUNCTION_ARGS);

/* Batch JSON extraction */
extern Datum orochi_json_extract_batch(PG_FUNCTION_ARGS);

/* JSON query explain */
extern Datum orochi_json_query_explain(PG_FUNCTION_ARGS);

/* JSON query statistics */
extern Datum orochi_json_query_stats(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Get operator name
 */
extern const char *json_query_operator_name(JsonQueryOperator op);

/*
 * Parse operator from name
 */
extern JsonQueryOperator json_query_parse_operator(const char *name);

/*
 * Expression to string (for explain/debug)
 */
extern char *json_query_expr_to_string(JsonQueryExpr *expr);

/*
 * Plan to string (for explain)
 */
extern char *json_query_plan_to_string(JsonQueryPlan *plan);

#endif /* OROCHI_JSON_QUERY_H */
