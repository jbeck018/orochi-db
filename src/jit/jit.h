/*-------------------------------------------------------------------------
 *
 * jit.h
 *    Orochi DB JIT (Just-In-Time) query compilation
 *
 * The JIT module provides:
 *   - Expression tree to specialized function compilation
 *   - Filter expression JIT for WHERE clauses
 *   - Aggregation JIT for SUM, COUNT, etc.
 *   - Integration with vectorized executor
 *
 * This uses a function pointer approach rather than LLVM for simplicity
 * while still achieving significant performance improvements through
 * specialized code generation.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JIT_H
#define OROCHI_JIT_H

#include "postgres.h"
#include "fmgr.h"
#include "nodes/primnodes.h"

#include "../executor/vectorized.h"

/* ============================================================
 * JIT Configuration Constants
 * ============================================================ */

/* Maximum expression tree depth */
#define JIT_MAX_EXPR_DEPTH              64

/* Maximum number of compiled expressions in cache */
#define JIT_CACHE_SIZE                  256

/* Minimum batch size to use JIT */
#define JIT_MIN_BATCH_SIZE              64

/* Maximum number of function arguments */
#define JIT_MAX_ARGS                    16

/* ============================================================
 * JIT Expression Types
 * ============================================================ */

typedef enum JitExprType
{
    JIT_EXPR_CONST,             /* Constant value */
    JIT_EXPR_COLUMN,            /* Column reference */
    JIT_EXPR_ARITH_ADD,         /* Addition */
    JIT_EXPR_ARITH_SUB,         /* Subtraction */
    JIT_EXPR_ARITH_MUL,         /* Multiplication */
    JIT_EXPR_ARITH_DIV,         /* Division */
    JIT_EXPR_ARITH_MOD,         /* Modulo */
    JIT_EXPR_ARITH_NEG,         /* Negation (unary minus) */
    JIT_EXPR_CMP_EQ,            /* Equal */
    JIT_EXPR_CMP_NE,            /* Not equal */
    JIT_EXPR_CMP_LT,            /* Less than */
    JIT_EXPR_CMP_LE,            /* Less than or equal */
    JIT_EXPR_CMP_GT,            /* Greater than */
    JIT_EXPR_CMP_GE,            /* Greater than or equal */
    JIT_EXPR_BOOL_AND,          /* Logical AND */
    JIT_EXPR_BOOL_OR,           /* Logical OR */
    JIT_EXPR_BOOL_NOT,          /* Logical NOT */
    JIT_EXPR_IS_NULL,           /* IS NULL */
    JIT_EXPR_IS_NOT_NULL,       /* IS NOT NULL */
    JIT_EXPR_BETWEEN,           /* BETWEEN low AND high */
    JIT_EXPR_IN,                /* IN (list) */
    JIT_EXPR_CASE,              /* CASE expression */
    JIT_EXPR_COALESCE,          /* COALESCE */
    JIT_EXPR_CAST               /* Type cast */
} JitExprType;

/* ============================================================
 * JIT Code Generation Options
 * ============================================================ */

typedef enum JitOptLevel
{
    JIT_OPT_NONE = 0,           /* No optimization */
    JIT_OPT_BASIC,              /* Basic optimizations */
    JIT_OPT_FULL                /* Full optimizations */
} JitOptLevel;

typedef struct JitOptions
{
    JitOptLevel     opt_level;              /* Optimization level */
    bool            use_simd;               /* Use SIMD when available */
    bool            check_nulls;            /* Generate null checking code */
    bool            enable_cse;             /* Common subexpression elimination */
    bool            inline_constants;       /* Inline constant values */
    bool            vectorize_loops;        /* Vectorize inner loops */
    int32           unroll_factor;          /* Loop unrolling factor */
} JitOptions;

/* Default JIT options */
#define JIT_OPTIONS_DEFAULT { \
    .opt_level = JIT_OPT_BASIC, \
    .use_simd = true, \
    .check_nulls = true, \
    .enable_cse = true, \
    .inline_constants = true, \
    .vectorize_loops = true, \
    .unroll_factor = 4 \
}

/* ============================================================
 * JIT Expression Node
 * ============================================================ */

typedef struct JitExprNode JitExprNode;

/*
 * JitExprNode - A node in the JIT expression tree
 */
struct JitExprNode
{
    JitExprType         type;               /* Expression type */
    VectorizedDataType  result_type;        /* Result data type */
    bool                is_nullable;        /* Can result be NULL? */

    /* For constants */
    Datum               const_value;
    bool                const_is_null;

    /* For column references */
    int32               column_index;

    /* For binary/unary operations */
    JitExprNode        *left;
    JitExprNode        *right;

    /* For CASE/IN expressions - list of child expressions */
    JitExprNode       **children;
    int32               child_count;

    /* For type casts */
    VectorizedDataType  cast_to_type;

    /* Compilation metadata */
    int32               depth;              /* Tree depth */
    int32               cost;               /* Estimated execution cost */
    uint32              expr_id;            /* Unique expression ID */
};

/* ============================================================
 * Compiled Expression Function Types
 * ============================================================ */

/*
 * Scalar evaluation function types
 * These operate on single values
 */
typedef int64   (*JitEvalInt64Fn)(const void *data, int32 row_idx,
                                  const uint64 *nulls, bool *is_null);
typedef int32   (*JitEvalInt32Fn)(const void *data, int32 row_idx,
                                  const uint64 *nulls, bool *is_null);
typedef double  (*JitEvalFloat64Fn)(const void *data, int32 row_idx,
                                    const uint64 *nulls, bool *is_null);
typedef float   (*JitEvalFloat32Fn)(const void *data, int32 row_idx,
                                    const uint64 *nulls, bool *is_null);
typedef bool    (*JitEvalBoolFn)(const void *data, int32 row_idx,
                                 const uint64 *nulls, bool *is_null);

/*
 * Vectorized evaluation function types
 * These operate on entire batches for better performance
 */
typedef void (*JitFilterFn)(VectorBatch *batch, int32 column_index,
                            Datum const_value, bool *out_mask);

typedef void (*JitAggUpdateFn)(VectorizedAggState *state, VectorBatch *batch);

typedef int64 (*JitSumInt64Fn)(VectorBatch *batch, int32 column_index);
typedef double (*JitSumFloat64Fn)(VectorBatch *batch, int32 column_index);

typedef void (*JitFilterExprFn)(VectorBatch *batch, JitExprNode *expr,
                                bool *out_mask);

/* ============================================================
 * JIT Compiled Expression
 * ============================================================ */

typedef struct JitCompiledExpr
{
    JitExprNode        *expr_tree;          /* Original expression tree */
    VectorizedDataType  result_type;        /* Result type */

    /* Compiled evaluation functions based on result type */
    union
    {
        JitEvalInt64Fn      eval_int64;
        JitEvalInt32Fn      eval_int32;
        JitEvalFloat64Fn    eval_float64;
        JitEvalFloat32Fn    eval_float32;
        JitEvalBoolFn       eval_bool;
    } scalar_fn;

    /* Vectorized batch function */
    JitFilterExprFn     batch_filter_fn;

    /* Metadata */
    uint32              expr_hash;          /* Expression hash for caching */
    int64               compile_time_us;    /* Compilation time */
    int64               execution_count;    /* Times executed */
    int64               total_rows;         /* Total rows processed */

    /* JIT options used */
    JitOptions          options;
} JitCompiledExpr;

/* ============================================================
 * JIT Compiled Filter
 * ============================================================ */

typedef struct JitCompiledFilter
{
    JitExprNode        *predicate;          /* Filter predicate tree */
    int32               column_indices[JIT_MAX_ARGS];  /* Columns referenced */
    int32               column_count;       /* Number of columns */

    /* Specialized filter functions */
    JitFilterFn         filter_fn;          /* Single-column filter */
    JitFilterExprFn     complex_filter_fn;  /* Complex filter expression */

    /* SIMD-optimized paths */
    bool                has_simd_path;      /* Has SIMD implementation */

    /* Statistics */
    int64               rows_checked;
    int64               rows_passed;
    double              selectivity;        /* Estimated selectivity */
} JitCompiledFilter;

/* ============================================================
 * JIT Compiled Aggregation
 * ============================================================ */

typedef struct JitCompiledAgg
{
    VectorizedAggType   agg_type;           /* Aggregation type */
    int32               column_index;       /* Column to aggregate */
    VectorizedDataType  input_type;         /* Input data type */
    VectorizedDataType  result_type;        /* Result data type */

    /* Compiled aggregation functions */
    JitAggUpdateFn      update_fn;          /* Batch update function */
    JitSumInt64Fn       sum_int64_fn;       /* Int64 sum function */
    JitSumFloat64Fn     sum_float64_fn;     /* Float64 sum function */

    /* Optional filter predicate */
    JitCompiledFilter  *filter;             /* Aggregation filter (WHERE) */

    /* Statistics */
    int64               batches_processed;
    int64               total_rows;
} JitCompiledAgg;

/* ============================================================
 * JIT Context
 * ============================================================ */

typedef struct JitContext
{
    MemoryContext       jit_memory_ctx;     /* Memory context for JIT */

    /* Compilation options */
    JitOptions          options;

    /* Expression cache */
    JitCompiledExpr   **expr_cache;
    int32               cache_size;
    int32               cache_count;

    /* CPU capabilities */
    bool                cpu_has_avx2;
    bool                cpu_has_avx512;
    bool                cpu_has_neon;

    /* Statistics */
    int64               expressions_compiled;
    int64               cache_hits;
    int64               cache_misses;
    int64               total_compile_time_us;
    int64               total_execution_time_us;

    /* Error handling */
    char               *last_error;
    bool                compile_failed;
} JitContext;

/* ============================================================
 * JIT Context Management
 * ============================================================ */

/*
 * Create a new JIT context
 */
extern JitContext *jit_context_create(void);

/*
 * Create JIT context with specific options
 */
extern JitContext *jit_context_create_with_options(const JitOptions *options);

/*
 * Free JIT context
 */
extern void jit_context_free(JitContext *ctx);

/*
 * Reset JIT context (clear cache, keep options)
 */
extern void jit_context_reset(JitContext *ctx);

/*
 * Get global JIT context
 */
extern JitContext *jit_get_global_context(void);

/* ============================================================
 * Expression Tree Building
 * ============================================================ */

/*
 * Create a constant expression node
 */
extern JitExprNode *jit_expr_const_int64(JitContext *ctx, int64 value);
extern JitExprNode *jit_expr_const_int32(JitContext *ctx, int32 value);
extern JitExprNode *jit_expr_const_float64(JitContext *ctx, double value);
extern JitExprNode *jit_expr_const_float32(JitContext *ctx, float value);
extern JitExprNode *jit_expr_const_bool(JitContext *ctx, bool value);
extern JitExprNode *jit_expr_const_null(JitContext *ctx, VectorizedDataType type);

/*
 * Create a column reference node
 */
extern JitExprNode *jit_expr_column(JitContext *ctx, int32 column_index,
                                    VectorizedDataType data_type);

/*
 * Create arithmetic expression nodes
 */
extern JitExprNode *jit_expr_add(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_sub(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_mul(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_div(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_mod(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_neg(JitContext *ctx, JitExprNode *operand);

/*
 * Create comparison expression nodes
 */
extern JitExprNode *jit_expr_eq(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_ne(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_lt(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_le(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_gt(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_ge(JitContext *ctx, JitExprNode *left, JitExprNode *right);

/*
 * Create boolean expression nodes
 */
extern JitExprNode *jit_expr_and(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_or(JitContext *ctx, JitExprNode *left, JitExprNode *right);
extern JitExprNode *jit_expr_not(JitContext *ctx, JitExprNode *operand);

/*
 * Create null check nodes
 */
extern JitExprNode *jit_expr_is_null(JitContext *ctx, JitExprNode *operand);
extern JitExprNode *jit_expr_is_not_null(JitContext *ctx, JitExprNode *operand);

/*
 * Create BETWEEN expression
 */
extern JitExprNode *jit_expr_between(JitContext *ctx, JitExprNode *value,
                                     JitExprNode *low, JitExprNode *high);

/*
 * Create type cast expression
 */
extern JitExprNode *jit_expr_cast(JitContext *ctx, JitExprNode *operand,
                                  VectorizedDataType target_type);

/*
 * Free expression node (recursive)
 */
extern void jit_expr_free(JitContext *ctx, JitExprNode *node);

/* ============================================================
 * Expression Compilation
 * ============================================================ */

/*
 * Compile an expression tree to specialized code
 */
extern JitCompiledExpr *jit_compile_expression(JitContext *ctx, JitExprNode *expr);

/*
 * Check if expression can be JIT compiled
 */
extern bool jit_can_compile_expression(JitExprNode *expr);

/*
 * Get estimated cost of expression
 */
extern int32 jit_estimate_expression_cost(JitExprNode *expr);

/* ============================================================
 * Filter Compilation
 * ============================================================ */

/*
 * Compile a filter predicate for vectorized evaluation
 */
extern JitCompiledFilter *jit_compile_filter(JitContext *ctx, JitExprNode *predicate);

/*
 * Compile a simple column comparison filter
 */
extern JitCompiledFilter *jit_compile_simple_filter(JitContext *ctx,
                                                    int32 column_index,
                                                    VectorizedCompareOp op,
                                                    Datum const_value,
                                                    VectorizedDataType data_type);

/*
 * Execute compiled filter on batch
 */
extern void jit_execute_filter(JitCompiledFilter *filter, VectorBatch *batch);

/*
 * Free compiled filter
 */
extern void jit_filter_free(JitContext *ctx, JitCompiledFilter *filter);

/* ============================================================
 * Aggregation Compilation
 * ============================================================ */

/*
 * Compile an aggregation for vectorized evaluation
 */
extern JitCompiledAgg *jit_compile_agg(JitContext *ctx,
                                       VectorizedAggType agg_type,
                                       int32 column_index,
                                       VectorizedDataType data_type);

/*
 * Compile aggregation with filter
 */
extern JitCompiledAgg *jit_compile_filtered_agg(JitContext *ctx,
                                                VectorizedAggType agg_type,
                                                int32 column_index,
                                                VectorizedDataType data_type,
                                                JitExprNode *filter_predicate);

/*
 * Execute compiled aggregation update on batch
 */
extern void jit_execute_agg_update(JitCompiledAgg *agg,
                                   VectorizedAggState *state,
                                   VectorBatch *batch);

/*
 * Free compiled aggregation
 */
extern void jit_agg_free(JitContext *ctx, JitCompiledAgg *agg);

/* ============================================================
 * Vectorized Executor Integration
 * ============================================================ */

/*
 * Apply JIT-compiled filter to batch
 * Falls back to interpreted execution if JIT unavailable
 */
extern void jit_vectorized_filter(VectorBatch *batch,
                                  JitCompiledFilter *filter);

/*
 * Execute JIT-compiled aggregation on batch
 * Falls back to interpreted execution if JIT unavailable
 */
extern void jit_vectorized_aggregate(VectorizedAggState *state,
                                     VectorBatch *batch,
                                     JitCompiledAgg *agg);

/*
 * Check if JIT is available and beneficial for this operation
 */
extern bool jit_should_use_jit(VectorBatch *batch, int32 expr_cost);

/* ============================================================
 * Statistics and Debugging
 * ============================================================ */

/*
 * Get JIT statistics
 */
typedef struct JitStats
{
    int64       expressions_compiled;
    int64       filters_compiled;
    int64       aggregations_compiled;
    int64       cache_hits;
    int64       cache_misses;
    int64       total_compile_time_us;
    int64       total_execution_time_us;
    int64       total_rows_processed;
    double      avg_compile_time_us;
    double      avg_execution_time_us;
} JitStats;

extern void jit_get_stats(JitContext *ctx, JitStats *stats);

/*
 * Reset JIT statistics
 */
extern void jit_reset_stats(JitContext *ctx);

/*
 * Print expression tree for debugging
 */
extern void jit_expr_debug_print(JitExprNode *expr, int indent);

/*
 * Get expression type name
 */
extern const char *jit_expr_type_name(JitExprType type);

/* ============================================================
 * GUC Variables
 * ============================================================ */

/* Enable JIT compilation */
extern bool orochi_jit_enabled;

/* Minimum expression cost for JIT */
extern int orochi_jit_min_cost;

/* JIT optimization level */
extern int orochi_jit_optimization_level;

/* Enable JIT expression caching */
extern bool orochi_jit_cache_enabled;

#endif /* OROCHI_JIT_H */
