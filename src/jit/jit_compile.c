/*-------------------------------------------------------------------------
 *
 * jit_compile.c
 *    Orochi DB JIT compilation for expressions, filters, and aggregations
 *
 * This module compiles expression trees to specialized function pointers
 * for efficient vectorized query execution.
 *
 * The approach uses template-based code generation with function pointers
 * rather than LLVM for simplicity while still achieving significant
 * performance improvements through:
 *   - Type-specialized evaluation functions
 *   - SIMD-accelerated batch operations
 *   - Expression caching
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/memutils.h"

#include <string.h>
#include <math.h>
#include <stdint.h>

#include "jit.h"
#include "jit_expr.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* ============================================================
 * GUC Variables
 * ============================================================ */

bool orochi_jit_enabled = true;
int orochi_jit_min_cost = 10;
int orochi_jit_optimization_level = 1;
bool orochi_jit_cache_enabled = true;

/* ============================================================
 * Global JIT Context
 * ============================================================ */

static JitContext *global_jit_context = NULL;

/* ============================================================
 * Forward Declarations - Filter Template Functions
 * ============================================================ */

/* Int64 filter templates */
static void jit_filter_int64_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int64_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int64_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int64_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int64_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int64_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);

/* Float64 filter templates */
static void jit_filter_float64_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float64_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float64_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float64_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float64_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float64_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);

/* Int32 filter templates */
static void jit_filter_int32_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int32_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int32_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int32_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int32_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_int32_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);

/* Float32 filter templates */
static void jit_filter_float32_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float32_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float32_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float32_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float32_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);
static void jit_filter_float32_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask);

/* Forward declarations - Aggregation template functions */
static void jit_agg_sum_int64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_sum_int32_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_sum_float64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_sum_float32_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_count_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_min_int64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_max_int64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_min_float64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_max_float64_update(VectorizedAggState *state, VectorBatch *batch);
static void jit_agg_avg_update(VectorizedAggState *state, VectorBatch *batch);

/* Expression hash computation */
static uint32 jit_compute_expr_hash(JitExprNode *expr);

/* ============================================================
 * JIT Context Management
 * ============================================================ */

JitContext *
jit_context_create(void)
{
    JitOptions default_opts = JIT_OPTIONS_DEFAULT;
    return jit_context_create_with_options(&default_opts);
}

JitContext *
jit_context_create_with_options(const JitOptions *options)
{
    JitContext *ctx;
    MemoryContext jit_ctx;

    /* Create dedicated memory context */
    jit_ctx = AllocSetContextCreate(CurrentMemoryContext,
                                    "JitContext",
                                    ALLOCSET_DEFAULT_SIZES);

    ctx = MemoryContextAllocZero(jit_ctx, sizeof(JitContext));
    ctx->jit_memory_ctx = jit_ctx;
    ctx->options = *options;

    /* Allocate expression cache */
    ctx->cache_size = JIT_CACHE_SIZE;
    ctx->cache_count = 0;
    ctx->expr_cache = MemoryContextAllocZero(jit_ctx,
                                             sizeof(JitCompiledExpr *) * ctx->cache_size);

    /* Detect CPU capabilities */
#ifdef __AVX2__
    ctx->cpu_has_avx2 = true;
#else
    ctx->cpu_has_avx2 = false;
#endif

#ifdef __AVX512F__
    ctx->cpu_has_avx512 = true;
#else
    ctx->cpu_has_avx512 = false;
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    ctx->cpu_has_neon = true;
#else
    ctx->cpu_has_neon = false;
#endif

    /* Initialize statistics */
    ctx->expressions_compiled = 0;
    ctx->cache_hits = 0;
    ctx->cache_misses = 0;
    ctx->total_compile_time_us = 0;
    ctx->total_execution_time_us = 0;

    ctx->last_error = NULL;
    ctx->compile_failed = false;

    return ctx;
}

void
jit_context_free(JitContext *ctx)
{
    if (ctx == NULL)
        return;

    /* Free the entire memory context */
    MemoryContextDelete(ctx->jit_memory_ctx);
}

void
jit_context_reset(JitContext *ctx)
{
    int i;

    if (ctx == NULL)
        return;

    /* Clear cache entries */
    for (i = 0; i < ctx->cache_count; i++)
    {
        ctx->expr_cache[i] = NULL;
    }
    ctx->cache_count = 0;

    /* Reset statistics */
    ctx->expressions_compiled = 0;
    ctx->cache_hits = 0;
    ctx->cache_misses = 0;
    ctx->total_compile_time_us = 0;
    ctx->total_execution_time_us = 0;

    ctx->last_error = NULL;
    ctx->compile_failed = false;
}

JitContext *
jit_get_global_context(void)
{
    if (global_jit_context == NULL)
    {
        global_jit_context = jit_context_create();
    }
    return global_jit_context;
}

/* ============================================================
 * Expression Tree Building
 * ============================================================ */

static JitExprNode *
jit_expr_alloc(JitContext *ctx, JitExprType type, VectorizedDataType result_type)
{
    JitExprNode *node;
    MemoryContext old_ctx;

    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);
    node = palloc0(sizeof(JitExprNode));
    node->type = type;
    node->result_type = result_type;
    node->is_nullable = true;
    node->depth = 0;
    node->cost = 1;
    MemoryContextSwitchTo(old_ctx);

    return node;
}

JitExprNode *
jit_expr_const_int64(JitContext *ctx, int64 value)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, VECTORIZED_TYPE_INT64);
    node->const_value = Int64GetDatum(value);
    node->const_is_null = false;
    node->is_nullable = false;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_const_int32(JitContext *ctx, int32 value)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, VECTORIZED_TYPE_INT32);
    node->const_value = Int32GetDatum(value);
    node->const_is_null = false;
    node->is_nullable = false;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_const_float64(JitContext *ctx, double value)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, VECTORIZED_TYPE_FLOAT64);
    node->const_value = Float8GetDatum(value);
    node->const_is_null = false;
    node->is_nullable = false;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_const_float32(JitContext *ctx, float value)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, VECTORIZED_TYPE_FLOAT32);
    node->const_value = Float4GetDatum(value);
    node->const_is_null = false;
    node->is_nullable = false;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_const_bool(JitContext *ctx, bool value)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, VECTORIZED_TYPE_BOOL);
    node->const_value = BoolGetDatum(value);
    node->const_is_null = false;
    node->is_nullable = false;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_const_null(JitContext *ctx, VectorizedDataType type)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_CONST, type);
    node->const_value = (Datum) 0;
    node->const_is_null = true;
    node->is_nullable = true;
    node->cost = 0;
    return node;
}

JitExprNode *
jit_expr_column(JitContext *ctx, int32 column_index, VectorizedDataType data_type)
{
    JitExprNode *node = jit_expr_alloc(ctx, JIT_EXPR_COLUMN, data_type);
    node->column_index = column_index;
    node->cost = 1;
    return node;
}

static JitExprNode *
jit_expr_binary(JitContext *ctx, JitExprType type, JitExprNode *left, JitExprNode *right,
                VectorizedDataType result_type)
{
    JitExprNode *node = jit_expr_alloc(ctx, type, result_type);
    node->left = left;
    node->right = right;
    node->depth = 1 + (left->depth > right->depth ? left->depth : right->depth);
    node->cost = 1 + left->cost + right->cost;
    node->is_nullable = left->is_nullable || right->is_nullable;
    return node;
}

static JitExprNode *
jit_expr_unary(JitContext *ctx, JitExprType type, JitExprNode *operand,
               VectorizedDataType result_type)
{
    JitExprNode *node = jit_expr_alloc(ctx, type, result_type);
    node->left = operand;
    node->right = NULL;
    node->depth = 1 + operand->depth;
    node->cost = 1 + operand->cost;
    node->is_nullable = operand->is_nullable;
    return node;
}

/* Determine result type for arithmetic operations */
static VectorizedDataType
jit_arith_result_type(VectorizedDataType left, VectorizedDataType right)
{
    /* Float64 dominates */
    if (left == VECTORIZED_TYPE_FLOAT64 || right == VECTORIZED_TYPE_FLOAT64)
        return VECTORIZED_TYPE_FLOAT64;

    /* Float32 next */
    if (left == VECTORIZED_TYPE_FLOAT32 || right == VECTORIZED_TYPE_FLOAT32)
        return VECTORIZED_TYPE_FLOAT64;  /* Promote to float64 for precision */

    /* Int64 next */
    if (left == VECTORIZED_TYPE_INT64 || right == VECTORIZED_TYPE_INT64)
        return VECTORIZED_TYPE_INT64;

    /* Default to int32 */
    return VECTORIZED_TYPE_INT64;  /* Promote to int64 to prevent overflow */
}

JitExprNode *
jit_expr_add(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    VectorizedDataType result_type = jit_arith_result_type(left->result_type,
                                                           right->result_type);
    return jit_expr_binary(ctx, JIT_EXPR_ARITH_ADD, left, right, result_type);
}

JitExprNode *
jit_expr_sub(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    VectorizedDataType result_type = jit_arith_result_type(left->result_type,
                                                           right->result_type);
    return jit_expr_binary(ctx, JIT_EXPR_ARITH_SUB, left, right, result_type);
}

JitExprNode *
jit_expr_mul(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    VectorizedDataType result_type = jit_arith_result_type(left->result_type,
                                                           right->result_type);
    return jit_expr_binary(ctx, JIT_EXPR_ARITH_MUL, left, right, result_type);
}

JitExprNode *
jit_expr_div(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    /* Division always returns float64 for accuracy */
    return jit_expr_binary(ctx, JIT_EXPR_ARITH_DIV, left, right, VECTORIZED_TYPE_FLOAT64);
}

JitExprNode *
jit_expr_mod(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    /* Modulo operates on integers */
    return jit_expr_binary(ctx, JIT_EXPR_ARITH_MOD, left, right, VECTORIZED_TYPE_INT64);
}

JitExprNode *
jit_expr_neg(JitContext *ctx, JitExprNode *operand)
{
    return jit_expr_unary(ctx, JIT_EXPR_ARITH_NEG, operand, operand->result_type);
}

JitExprNode *
jit_expr_eq(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_EQ, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_ne(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_NE, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_lt(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_LT, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_le(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_LE, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_gt(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_GT, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_ge(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_CMP_GE, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_and(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_BOOL_AND, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_or(JitContext *ctx, JitExprNode *left, JitExprNode *right)
{
    return jit_expr_binary(ctx, JIT_EXPR_BOOL_OR, left, right, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_not(JitContext *ctx, JitExprNode *operand)
{
    return jit_expr_unary(ctx, JIT_EXPR_BOOL_NOT, operand, VECTORIZED_TYPE_BOOL);
}

JitExprNode *
jit_expr_is_null(JitContext *ctx, JitExprNode *operand)
{
    JitExprNode *node = jit_expr_unary(ctx, JIT_EXPR_IS_NULL, operand, VECTORIZED_TYPE_BOOL);
    node->is_nullable = false;  /* IS NULL result is never null */
    return node;
}

JitExprNode *
jit_expr_is_not_null(JitContext *ctx, JitExprNode *operand)
{
    JitExprNode *node = jit_expr_unary(ctx, JIT_EXPR_IS_NOT_NULL, operand, VECTORIZED_TYPE_BOOL);
    node->is_nullable = false;  /* IS NOT NULL result is never null */
    return node;
}

JitExprNode *
jit_expr_between(JitContext *ctx, JitExprNode *value,
                 JitExprNode *low, JitExprNode *high)
{
    JitExprNode *node;
    MemoryContext old_ctx;

    node = jit_expr_alloc(ctx, JIT_EXPR_BETWEEN, VECTORIZED_TYPE_BOOL);

    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);
    node->children = palloc(sizeof(JitExprNode *) * 3);
    MemoryContextSwitchTo(old_ctx);

    node->children[0] = value;
    node->children[1] = low;
    node->children[2] = high;
    node->child_count = 3;
    node->depth = 1 + value->depth;
    node->cost = value->cost + low->cost + high->cost + 2;
    node->is_nullable = value->is_nullable || low->is_nullable || high->is_nullable;

    return node;
}

JitExprNode *
jit_expr_cast(JitContext *ctx, JitExprNode *operand, VectorizedDataType target_type)
{
    JitExprNode *node = jit_expr_unary(ctx, JIT_EXPR_CAST, operand, target_type);
    node->cast_to_type = target_type;
    return node;
}

void
jit_expr_free(JitContext *ctx, JitExprNode *node)
{
    int i;

    if (node == NULL)
        return;

    /* Recursively free children */
    if (node->left != NULL)
        jit_expr_free(ctx, node->left);
    if (node->right != NULL)
        jit_expr_free(ctx, node->right);

    if (node->children != NULL)
    {
        for (i = 0; i < node->child_count; i++)
        {
            jit_expr_free(ctx, node->children[i]);
        }
        pfree(node->children);
    }

    pfree(node);
}

/* ============================================================
 * Expression Hash Computation
 * ============================================================ */

static uint32
jit_compute_expr_hash(JitExprNode *expr)
{
    uint32 hash = 0;

    if (expr == NULL)
        return 0;

    /* Simple hash combining expression type and structure */
    hash = (uint32)expr->type;
    hash = hash * 31 + (uint32)expr->result_type;

    if (expr->type == JIT_EXPR_CONST)
    {
        /* Include constant value in hash */
        hash = hash * 31 + (uint32)(expr->const_value & 0xFFFFFFFF);
        hash = hash * 31 + (uint32)(expr->const_value >> 32);
        hash = hash * 31 + (expr->const_is_null ? 1 : 0);
    }
    else if (expr->type == JIT_EXPR_COLUMN)
    {
        hash = hash * 31 + (uint32)expr->column_index;
    }

    /* Recursively hash children */
    if (expr->left != NULL)
        hash = hash * 31 + jit_compute_expr_hash(expr->left);
    if (expr->right != NULL)
        hash = hash * 31 + jit_compute_expr_hash(expr->right);

    return hash;
}

/* ============================================================
 * Expression Compilation
 * ============================================================ */

bool
jit_can_compile_expression(JitExprNode *expr)
{
    if (expr == NULL)
        return false;

    /* Check depth limit */
    if (expr->depth > JIT_MAX_EXPR_DEPTH)
        return false;

    /* Check supported types */
    switch (expr->type)
    {
        case JIT_EXPR_CONST:
        case JIT_EXPR_COLUMN:
        case JIT_EXPR_ARITH_ADD:
        case JIT_EXPR_ARITH_SUB:
        case JIT_EXPR_ARITH_MUL:
        case JIT_EXPR_ARITH_DIV:
        case JIT_EXPR_ARITH_MOD:
        case JIT_EXPR_ARITH_NEG:
        case JIT_EXPR_CMP_EQ:
        case JIT_EXPR_CMP_NE:
        case JIT_EXPR_CMP_LT:
        case JIT_EXPR_CMP_LE:
        case JIT_EXPR_CMP_GT:
        case JIT_EXPR_CMP_GE:
        case JIT_EXPR_BOOL_AND:
        case JIT_EXPR_BOOL_OR:
        case JIT_EXPR_BOOL_NOT:
        case JIT_EXPR_IS_NULL:
        case JIT_EXPR_IS_NOT_NULL:
        case JIT_EXPR_BETWEEN:
        case JIT_EXPR_CAST:
            break;

        default:
            return false;
    }

    /* Recursively check children */
    if (expr->left != NULL && !jit_can_compile_expression(expr->left))
        return false;
    if (expr->right != NULL && !jit_can_compile_expression(expr->right))
        return false;

    return true;
}

int32
jit_estimate_expression_cost(JitExprNode *expr)
{
    if (expr == NULL)
        return 0;

    return expr->cost;
}

JitCompiledExpr *
jit_compile_expression(JitContext *ctx, JitExprNode *expr)
{
    JitCompiledExpr *compiled;
    MemoryContext old_ctx;
    uint32 expr_hash;
    int i;

    if (ctx == NULL || expr == NULL)
        return NULL;

    if (!jit_can_compile_expression(expr))
    {
        ctx->compile_failed = true;
        ctx->last_error = "Expression cannot be JIT compiled";
        return NULL;
    }

    /* Check cache */
    expr_hash = jit_compute_expr_hash(expr);

    if (orochi_jit_cache_enabled)
    {
        for (i = 0; i < ctx->cache_count; i++)
        {
            if (ctx->expr_cache[i] != NULL &&
                ctx->expr_cache[i]->expr_hash == expr_hash)
            {
                ctx->cache_hits++;
                return ctx->expr_cache[i];
            }
        }
        ctx->cache_misses++;
    }

    /* Compile expression */
    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);

    compiled = palloc0(sizeof(JitCompiledExpr));
    compiled->expr_tree = expr;
    compiled->result_type = expr->result_type;
    compiled->expr_hash = expr_hash;
    compiled->options = ctx->options;
    compiled->compile_time_us = 0;
    compiled->execution_count = 0;
    compiled->total_rows = 0;

    /* Generate scalar evaluation function based on result type */
    switch (expr->result_type)
    {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            compiled->scalar_fn.eval_int64 = jit_generate_eval_int64(ctx, expr);
            break;

        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            compiled->scalar_fn.eval_int32 = jit_generate_eval_int32(ctx, expr);
            break;

        case VECTORIZED_TYPE_FLOAT64:
            compiled->scalar_fn.eval_float64 = jit_generate_eval_float64(ctx, expr);
            break;

        case VECTORIZED_TYPE_FLOAT32:
            compiled->scalar_fn.eval_float32 = jit_generate_eval_float32(ctx, expr);
            break;

        case VECTORIZED_TYPE_BOOL:
            compiled->scalar_fn.eval_bool = jit_generate_eval_bool(ctx, expr);
            break;

        default:
            break;
    }

    /* Generate batch filter function if expression is boolean */
    if (expr->result_type == VECTORIZED_TYPE_BOOL)
    {
        compiled->batch_filter_fn = jit_generate_batch_filter(ctx, expr);
    }

    MemoryContextSwitchTo(old_ctx);

    /* Add to cache */
    if (orochi_jit_cache_enabled && ctx->cache_count < ctx->cache_size)
    {
        ctx->expr_cache[ctx->cache_count++] = compiled;
    }

    ctx->expressions_compiled++;

    return compiled;
}

/* ============================================================
 * Filter Compilation
 * ============================================================ */

JitCompiledFilter *
jit_compile_simple_filter(JitContext *ctx, int32 column_index,
                          VectorizedCompareOp op, Datum const_value,
                          VectorizedDataType data_type)
{
    JitCompiledFilter *filter;
    MemoryContext old_ctx;

    if (ctx == NULL)
        return NULL;

    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);

    filter = palloc0(sizeof(JitCompiledFilter));
    filter->column_indices[0] = column_index;
    filter->column_count = 1;
    filter->rows_checked = 0;
    filter->rows_passed = 0;
    filter->selectivity = 0.5;  /* Default estimate */

    /* Build a simple predicate expression */
    filter->predicate = jit_expr_column(ctx, column_index, data_type);

    /* Select appropriate filter function based on type and operation */
    filter->has_simd_path = ctx->cpu_has_avx2;

    switch (data_type)
    {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            switch (op)
            {
                case VECTORIZED_CMP_EQ:
                    filter->filter_fn = jit_filter_int64_eq;
                    break;
                case VECTORIZED_CMP_NE:
                    filter->filter_fn = jit_filter_int64_ne;
                    break;
                case VECTORIZED_CMP_LT:
                    filter->filter_fn = jit_filter_int64_lt;
                    break;
                case VECTORIZED_CMP_LE:
                    filter->filter_fn = jit_filter_int64_le;
                    break;
                case VECTORIZED_CMP_GT:
                    filter->filter_fn = jit_filter_int64_gt;
                    break;
                case VECTORIZED_CMP_GE:
                    filter->filter_fn = jit_filter_int64_ge;
                    break;
                default:
                    filter->filter_fn = NULL;
                    break;
            }
            break;

        case VECTORIZED_TYPE_FLOAT64:
            switch (op)
            {
                case VECTORIZED_CMP_EQ:
                    filter->filter_fn = jit_filter_float64_eq;
                    break;
                case VECTORIZED_CMP_NE:
                    filter->filter_fn = jit_filter_float64_ne;
                    break;
                case VECTORIZED_CMP_LT:
                    filter->filter_fn = jit_filter_float64_lt;
                    break;
                case VECTORIZED_CMP_LE:
                    filter->filter_fn = jit_filter_float64_le;
                    break;
                case VECTORIZED_CMP_GT:
                    filter->filter_fn = jit_filter_float64_gt;
                    break;
                case VECTORIZED_CMP_GE:
                    filter->filter_fn = jit_filter_float64_ge;
                    break;
                default:
                    filter->filter_fn = NULL;
                    break;
            }
            break;

        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            switch (op)
            {
                case VECTORIZED_CMP_EQ:
                    filter->filter_fn = jit_filter_int32_eq;
                    break;
                case VECTORIZED_CMP_NE:
                    filter->filter_fn = jit_filter_int32_ne;
                    break;
                case VECTORIZED_CMP_LT:
                    filter->filter_fn = jit_filter_int32_lt;
                    break;
                case VECTORIZED_CMP_LE:
                    filter->filter_fn = jit_filter_int32_le;
                    break;
                case VECTORIZED_CMP_GT:
                    filter->filter_fn = jit_filter_int32_gt;
                    break;
                case VECTORIZED_CMP_GE:
                    filter->filter_fn = jit_filter_int32_ge;
                    break;
                default:
                    filter->filter_fn = NULL;
                    break;
            }
            break;

        case VECTORIZED_TYPE_FLOAT32:
            switch (op)
            {
                case VECTORIZED_CMP_EQ:
                    filter->filter_fn = jit_filter_float32_eq;
                    break;
                case VECTORIZED_CMP_NE:
                    filter->filter_fn = jit_filter_float32_ne;
                    break;
                case VECTORIZED_CMP_LT:
                    filter->filter_fn = jit_filter_float32_lt;
                    break;
                case VECTORIZED_CMP_LE:
                    filter->filter_fn = jit_filter_float32_le;
                    break;
                case VECTORIZED_CMP_GT:
                    filter->filter_fn = jit_filter_float32_gt;
                    break;
                case VECTORIZED_CMP_GE:
                    filter->filter_fn = jit_filter_float32_ge;
                    break;
                default:
                    filter->filter_fn = NULL;
                    break;
            }
            break;

        default:
            filter->filter_fn = NULL;
            break;
    }

    MemoryContextSwitchTo(old_ctx);

    return filter;
}

JitCompiledFilter *
jit_compile_filter(JitContext *ctx, JitExprNode *predicate)
{
    JitCompiledFilter *filter;
    JitCompiledExpr *compiled_expr;
    MemoryContext old_ctx;

    if (ctx == NULL || predicate == NULL)
        return NULL;

    /* Predicate must be boolean */
    if (predicate->result_type != VECTORIZED_TYPE_BOOL)
        return NULL;

    /* Compile the expression */
    compiled_expr = jit_compile_expression(ctx, predicate);
    if (compiled_expr == NULL)
        return NULL;

    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);

    filter = palloc0(sizeof(JitCompiledFilter));
    filter->predicate = predicate;
    filter->complex_filter_fn = compiled_expr->batch_filter_fn;
    filter->column_count = 0;
    filter->rows_checked = 0;
    filter->rows_passed = 0;
    filter->selectivity = 0.5;
    filter->has_simd_path = ctx->cpu_has_avx2;

    /* Extract column references */
    jit_extract_column_refs(predicate, filter->column_indices, &filter->column_count,
                            JIT_MAX_ARGS);

    MemoryContextSwitchTo(old_ctx);

    return filter;
}

void
jit_execute_filter(JitCompiledFilter *filter, VectorBatch *batch)
{
    bool *mask;
    int32 i;

    if (filter == NULL || batch == NULL || batch->count == 0)
        return;

    mask = palloc(batch->capacity * sizeof(bool));

    /* Initialize mask to all true */
    memset(mask, 1, batch->count * sizeof(bool));

    /* Execute appropriate filter function */
    if (filter->filter_fn != NULL)
    {
        /* Simple single-column filter */
        filter->filter_fn(batch, filter->column_indices[0],
                         filter->predicate->const_value, mask);
    }
    else if (filter->complex_filter_fn != NULL)
    {
        /* Complex expression filter */
        filter->complex_filter_fn(batch, filter->predicate, mask);
    }

    /* Apply mask to selection vector */
    if (!batch->has_selection)
    {
        /* Initialize selection vector */
        batch->selected_count = 0;
        batch->has_selection = true;

        for (i = 0; i < batch->count; i++)
        {
            if (mask[i])
            {
                batch->selection[batch->selected_count++] = i;
            }
        }
    }
    else
    {
        /* Intersect with existing selection */
        int32 write_pos = 0;

        for (i = 0; i < batch->selected_count; i++)
        {
            uint32 idx = batch->selection[i];
            if (mask[idx])
            {
                batch->selection[write_pos++] = idx;
            }
        }
        batch->selected_count = write_pos;
    }

    /* Update statistics */
    filter->rows_checked += batch->count;
    filter->rows_passed += batch->selected_count;

    pfree(mask);
}

void
jit_filter_free(JitContext *ctx, JitCompiledFilter *filter)
{
    if (filter == NULL)
        return;

    /* Note: filter->predicate is managed by the JIT context memory */
    pfree(filter);
}

/* ============================================================
 * Aggregation Compilation
 * ============================================================ */

JitCompiledAgg *
jit_compile_agg(JitContext *ctx, VectorizedAggType agg_type,
                int32 column_index, VectorizedDataType data_type)
{
    JitCompiledAgg *agg;
    MemoryContext old_ctx;

    if (ctx == NULL)
        return NULL;

    old_ctx = MemoryContextSwitchTo(ctx->jit_memory_ctx);

    agg = palloc0(sizeof(JitCompiledAgg));
    agg->agg_type = agg_type;
    agg->column_index = column_index;
    agg->input_type = data_type;
    agg->filter = NULL;
    agg->batches_processed = 0;
    agg->total_rows = 0;

    /* Determine result type and select update function */
    switch (agg_type)
    {
        case VECTORIZED_AGG_COUNT:
            agg->result_type = VECTORIZED_TYPE_INT64;
            agg->update_fn = jit_agg_count_update;
            break;

        case VECTORIZED_AGG_SUM:
            switch (data_type)
            {
                case VECTORIZED_TYPE_INT64:
                case VECTORIZED_TYPE_TIMESTAMP:
                    agg->result_type = VECTORIZED_TYPE_INT64;
                    agg->update_fn = jit_agg_sum_int64_update;
                    agg->sum_int64_fn = jit_generate_sum_int64(ctx, column_index);
                    break;
                case VECTORIZED_TYPE_INT32:
                case VECTORIZED_TYPE_DATE:
                    agg->result_type = VECTORIZED_TYPE_INT64;
                    agg->update_fn = jit_agg_sum_int32_update;
                    break;
                case VECTORIZED_TYPE_FLOAT64:
                    agg->result_type = VECTORIZED_TYPE_FLOAT64;
                    agg->update_fn = jit_agg_sum_float64_update;
                    agg->sum_float64_fn = jit_generate_sum_float64(ctx, column_index);
                    break;
                case VECTORIZED_TYPE_FLOAT32:
                    agg->result_type = VECTORIZED_TYPE_FLOAT64;
                    agg->update_fn = jit_agg_sum_float32_update;
                    break;
                default:
                    agg->result_type = VECTORIZED_TYPE_FLOAT64;
                    agg->update_fn = NULL;
                    break;
            }
            break;

        case VECTORIZED_AGG_AVG:
            agg->result_type = VECTORIZED_TYPE_FLOAT64;
            agg->update_fn = jit_agg_avg_update;
            break;

        case VECTORIZED_AGG_MIN:
            switch (data_type)
            {
                case VECTORIZED_TYPE_INT64:
                case VECTORIZED_TYPE_TIMESTAMP:
                    agg->result_type = VECTORIZED_TYPE_INT64;
                    agg->update_fn = jit_agg_min_int64_update;
                    break;
                case VECTORIZED_TYPE_FLOAT64:
                    agg->result_type = VECTORIZED_TYPE_FLOAT64;
                    agg->update_fn = jit_agg_min_float64_update;
                    break;
                default:
                    agg->result_type = data_type;
                    agg->update_fn = NULL;
                    break;
            }
            break;

        case VECTORIZED_AGG_MAX:
            switch (data_type)
            {
                case VECTORIZED_TYPE_INT64:
                case VECTORIZED_TYPE_TIMESTAMP:
                    agg->result_type = VECTORIZED_TYPE_INT64;
                    agg->update_fn = jit_agg_max_int64_update;
                    break;
                case VECTORIZED_TYPE_FLOAT64:
                    agg->result_type = VECTORIZED_TYPE_FLOAT64;
                    agg->update_fn = jit_agg_max_float64_update;
                    break;
                default:
                    agg->result_type = data_type;
                    agg->update_fn = NULL;
                    break;
            }
            break;

        default:
            agg->update_fn = NULL;
            break;
    }

    MemoryContextSwitchTo(old_ctx);

    return agg;
}

JitCompiledAgg *
jit_compile_filtered_agg(JitContext *ctx, VectorizedAggType agg_type,
                         int32 column_index, VectorizedDataType data_type,
                         JitExprNode *filter_predicate)
{
    JitCompiledAgg *agg;

    agg = jit_compile_agg(ctx, agg_type, column_index, data_type);
    if (agg == NULL)
        return NULL;

    if (filter_predicate != NULL)
    {
        agg->filter = jit_compile_filter(ctx, filter_predicate);
    }

    return agg;
}

void
jit_execute_agg_update(JitCompiledAgg *agg, VectorizedAggState *state,
                       VectorBatch *batch)
{
    if (agg == NULL || state == NULL || batch == NULL)
        return;

    /* Apply filter if present */
    if (agg->filter != NULL)
    {
        jit_execute_filter(agg->filter, batch);
    }

    /* Execute aggregation update */
    if (agg->update_fn != NULL)
    {
        agg->update_fn(state, batch);
    }

    /* Update statistics */
    agg->batches_processed++;
    agg->total_rows += batch->has_selection ? batch->selected_count : batch->count;
}

void
jit_agg_free(JitContext *ctx, JitCompiledAgg *agg)
{
    if (agg == NULL)
        return;

    if (agg->filter != NULL)
    {
        jit_filter_free(ctx, agg->filter);
    }

    pfree(agg);
}

/* ============================================================
 * Vectorized Executor Integration
 * ============================================================ */

void
jit_vectorized_filter(VectorBatch *batch, JitCompiledFilter *filter)
{
    if (!orochi_jit_enabled || filter == NULL)
    {
        /* Fall back to interpreted execution */
        return;
    }

    jit_execute_filter(filter, batch);
}

void
jit_vectorized_aggregate(VectorizedAggState *state, VectorBatch *batch,
                         JitCompiledAgg *agg)
{
    if (!orochi_jit_enabled || agg == NULL)
    {
        /* Fall back to interpreted execution */
        vectorized_agg_update(state, batch);
        return;
    }

    jit_execute_agg_update(agg, state, batch);
}

bool
jit_should_use_jit(VectorBatch *batch, int32 expr_cost)
{
    if (!orochi_jit_enabled)
        return false;

    /* Use JIT if batch is large enough and expression is complex enough */
    if (batch->count < JIT_MIN_BATCH_SIZE)
        return false;

    if (expr_cost < orochi_jit_min_cost)
        return false;

    return true;
}

/* ============================================================
 * Filter Template Functions - Int64
 * ============================================================ */

static void
jit_filter_int64_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi64x(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpeq_epi64(data_vec, val_vec);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, cmp);
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] == value);
    }

    /* Handle nulls */
    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int64_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] != value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int64_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi64x(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpgt_epi64(val_vec, data_vec);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, cmp);
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] < value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int64_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] <= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int64_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi64x(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpgt_epi64(data_vec, val_vec);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, cmp);
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] > value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int64_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int64 *data = (int64 *)column->data;
    int64 value = DatumGetInt64(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] >= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

/* ============================================================
 * Filter Template Functions - Float64
 * ============================================================ */

static void
jit_filter_float64_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

#ifdef __AVX2__
    __m256d val_vec = _mm256_set1_pd(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256d data_vec = _mm256_loadu_pd(data + i);
        __m256d cmp = _mm256_cmp_pd(data_vec, val_vec, _CMP_EQ_OQ);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, _mm256_castpd_si256(cmp));
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] == value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float64_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] != value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float64_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

#ifdef __AVX2__
    __m256d val_vec = _mm256_set1_pd(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256d data_vec = _mm256_loadu_pd(data + i);
        __m256d cmp = _mm256_cmp_pd(data_vec, val_vec, _CMP_LT_OQ);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, _mm256_castpd_si256(cmp));
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] < value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float64_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] <= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float64_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

#ifdef __AVX2__
    __m256d val_vec = _mm256_set1_pd(value);

    for (i = 0; i + 4 <= batch->count; i += 4)
    {
        __m256d data_vec = _mm256_loadu_pd(data + i);
        __m256d cmp = _mm256_cmp_pd(data_vec, val_vec, _CMP_GT_OQ);
        int64 results[4];
        _mm256_storeu_si256((__m256i *)results, _mm256_castpd_si256(cmp));
        mask[i] = (results[0] != 0);
        mask[i + 1] = (results[1] != 0);
        mask[i + 2] = (results[2] != 0);
        mask[i + 3] = (results[3] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] > value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float64_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    double *data = (double *)column->data;
    double value = DatumGetFloat8(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] >= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

/* ============================================================
 * Filter Template Functions - Int32
 * ============================================================ */

static void
jit_filter_int32_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi32(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpeq_epi32(data_vec, val_vec);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, cmp);
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] == value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int32_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] != value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int32_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi32(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpgt_epi32(val_vec, data_vec);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, cmp);
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] < value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int32_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] <= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int32_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

#ifdef __AVX2__
    __m256i val_vec = _mm256_set1_epi32(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256i data_vec = _mm256_loadu_si256((__m256i *)(data + i));
        __m256i cmp = _mm256_cmpgt_epi32(data_vec, val_vec);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, cmp);
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] > value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_int32_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    int32 *data = (int32 *)column->data;
    int32 value = DatumGetInt32(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] >= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

/* ============================================================
 * Filter Template Functions - Float32
 * ============================================================ */

static void
jit_filter_float32_eq(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

#ifdef __AVX2__
    __m256 val_vec = _mm256_set1_ps(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256 data_vec = _mm256_loadu_ps(data + i);
        __m256 cmp = _mm256_cmp_ps(data_vec, val_vec, _CMP_EQ_OQ);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, _mm256_castps_si256(cmp));
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] == value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float32_ne(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] != value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float32_lt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

#ifdef __AVX2__
    __m256 val_vec = _mm256_set1_ps(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256 data_vec = _mm256_loadu_ps(data + i);
        __m256 cmp = _mm256_cmp_ps(data_vec, val_vec, _CMP_LT_OQ);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, _mm256_castps_si256(cmp));
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] < value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float32_le(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] <= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float32_gt(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

#ifdef __AVX2__
    __m256 val_vec = _mm256_set1_ps(value);

    for (i = 0; i + 8 <= batch->count; i += 8)
    {
        __m256 data_vec = _mm256_loadu_ps(data + i);
        __m256 cmp = _mm256_cmp_ps(data_vec, val_vec, _CMP_GT_OQ);
        int32 results[8];
        _mm256_storeu_si256((__m256i *)results, _mm256_castps_si256(cmp));
        for (int j = 0; j < 8; j++)
            mask[i + j] = (results[j] != 0);
    }
#else
    i = 0;
#endif

    for (; i < batch->count; i++)
    {
        mask[i] = (data[i] > value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

static void
jit_filter_float32_ge(VectorBatch *batch, int32 col_idx, Datum val, bool *mask)
{
    VectorColumn *column = batch->columns[col_idx];
    float *data = (float *)column->data;
    float value = DatumGetFloat4(val);
    int32 i;

    for (i = 0; i < batch->count; i++)
    {
        mask[i] = (data[i] >= value);
    }

    if (column->has_nulls)
    {
        for (i = 0; i < batch->count; i++)
        {
            if (column->nulls[i / 64] & (1ULL << (i % 64)))
                mask[i] = false;
        }
    }
}

/* ============================================================
 * Aggregation Template Functions
 * ============================================================ */

static void
jit_agg_count_update(VectorizedAggState *state, VectorBatch *batch)
{
    if (state->column_index < 0)
    {
        /* COUNT(*) */
        state->count += batch->has_selection ? batch->selected_count : batch->count;
    }
    else
    {
        /* COUNT(column) - count non-null values */
        state->count += vectorized_count_column(batch, state->column_index);
    }
}

static void
jit_agg_sum_int64_update(VectorizedAggState *state, VectorBatch *batch)
{
    state->sum_int64 += vectorized_sum_int64(batch, state->column_index);
    state->has_value = true;
}

static void
jit_agg_sum_int32_update(VectorizedAggState *state, VectorBatch *batch)
{
    state->sum_int64 += vectorized_sum_int32(batch, state->column_index);
    state->has_value = true;
}

static void
jit_agg_sum_float64_update(VectorizedAggState *state, VectorBatch *batch)
{
    state->sum_float64 += vectorized_sum_float64(batch, state->column_index);
    state->has_value = true;
}

static void
jit_agg_sum_float32_update(VectorizedAggState *state, VectorBatch *batch)
{
    state->sum_float64 += vectorized_sum_float32(batch, state->column_index);
    state->has_value = true;
}

static void
jit_agg_min_int64_update(VectorizedAggState *state, VectorBatch *batch)
{
    bool found;
    int64 val = vectorized_min_int64(batch, state->column_index, &found);

    if (found && (!state->has_value || val < state->min_int64))
    {
        state->min_int64 = val;
        state->has_value = true;
    }
}

static void
jit_agg_max_int64_update(VectorizedAggState *state, VectorBatch *batch)
{
    bool found;
    int64 val = vectorized_max_int64(batch, state->column_index, &found);

    if (found && (!state->has_value || val > state->max_int64))
    {
        state->max_int64 = val;
        state->has_value = true;
    }
}

static void
jit_agg_min_float64_update(VectorizedAggState *state, VectorBatch *batch)
{
    bool found;
    double val = vectorized_min_float64(batch, state->column_index, &found);

    if (found && (!state->has_value || val < state->min_float64))
    {
        state->min_float64 = val;
        state->has_value = true;
    }
}

static void
jit_agg_max_float64_update(VectorizedAggState *state, VectorBatch *batch)
{
    bool found;
    double val = vectorized_max_float64(batch, state->column_index, &found);

    if (found && (!state->has_value || val > state->max_float64))
    {
        state->max_float64 = val;
        state->has_value = true;
    }
}

static void
jit_agg_avg_update(VectorizedAggState *state, VectorBatch *batch)
{
    state->count += vectorized_count_column(batch, state->column_index);

    switch (state->data_type)
    {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP:
            state->sum_float64 += (double)vectorized_sum_int64(batch, state->column_index);
            break;
        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE:
            state->sum_float64 += (double)vectorized_sum_int32(batch, state->column_index);
            break;
        case VECTORIZED_TYPE_FLOAT64:
            state->sum_float64 += vectorized_sum_float64(batch, state->column_index);
            break;
        case VECTORIZED_TYPE_FLOAT32:
            state->sum_float64 += vectorized_sum_float32(batch, state->column_index);
            break;
        default:
            break;
    }

    state->has_value = true;
}

/* ============================================================
 * Statistics and Debugging
 * ============================================================ */

void
jit_get_stats(JitContext *ctx, JitStats *stats)
{
    if (ctx == NULL || stats == NULL)
        return;

    memset(stats, 0, sizeof(JitStats));

    stats->expressions_compiled = ctx->expressions_compiled;
    stats->cache_hits = ctx->cache_hits;
    stats->cache_misses = ctx->cache_misses;
    stats->total_compile_time_us = ctx->total_compile_time_us;
    stats->total_execution_time_us = ctx->total_execution_time_us;

    if (ctx->expressions_compiled > 0)
    {
        stats->avg_compile_time_us = (double)ctx->total_compile_time_us /
                                     ctx->expressions_compiled;
    }
}

void
jit_reset_stats(JitContext *ctx)
{
    if (ctx == NULL)
        return;

    ctx->expressions_compiled = 0;
    ctx->cache_hits = 0;
    ctx->cache_misses = 0;
    ctx->total_compile_time_us = 0;
    ctx->total_execution_time_us = 0;
}

const char *
jit_expr_type_name(JitExprType type)
{
    switch (type)
    {
        case JIT_EXPR_CONST:        return "CONST";
        case JIT_EXPR_COLUMN:       return "COLUMN";
        case JIT_EXPR_ARITH_ADD:    return "ADD";
        case JIT_EXPR_ARITH_SUB:    return "SUB";
        case JIT_EXPR_ARITH_MUL:    return "MUL";
        case JIT_EXPR_ARITH_DIV:    return "DIV";
        case JIT_EXPR_ARITH_MOD:    return "MOD";
        case JIT_EXPR_ARITH_NEG:    return "NEG";
        case JIT_EXPR_CMP_EQ:       return "EQ";
        case JIT_EXPR_CMP_NE:       return "NE";
        case JIT_EXPR_CMP_LT:       return "LT";
        case JIT_EXPR_CMP_LE:       return "LE";
        case JIT_EXPR_CMP_GT:       return "GT";
        case JIT_EXPR_CMP_GE:       return "GE";
        case JIT_EXPR_BOOL_AND:     return "AND";
        case JIT_EXPR_BOOL_OR:      return "OR";
        case JIT_EXPR_BOOL_NOT:     return "NOT";
        case JIT_EXPR_IS_NULL:      return "IS_NULL";
        case JIT_EXPR_IS_NOT_NULL:  return "IS_NOT_NULL";
        case JIT_EXPR_BETWEEN:      return "BETWEEN";
        case JIT_EXPR_IN:           return "IN";
        case JIT_EXPR_CASE:         return "CASE";
        case JIT_EXPR_COALESCE:     return "COALESCE";
        case JIT_EXPR_CAST:         return "CAST";
        default:                    return "UNKNOWN";
    }
}

void
jit_expr_debug_print(JitExprNode *expr, int indent)
{
    int i;

    if (expr == NULL)
        return;

    /* Print indentation */
    for (i = 0; i < indent; i++)
        elog(DEBUG1, "  ");

    elog(DEBUG1, "%s (type=%s, cost=%d, depth=%d)",
         jit_expr_type_name(expr->type),
         vectorized_type_name(expr->result_type),
         expr->cost, expr->depth);

    if (expr->type == JIT_EXPR_CONST)
    {
        elog(DEBUG1, "  value=%ld, is_null=%s",
             (long)expr->const_value,
             expr->const_is_null ? "true" : "false");
    }
    else if (expr->type == JIT_EXPR_COLUMN)
    {
        elog(DEBUG1, "  column_index=%d", expr->column_index);
    }

    /* Recursively print children */
    if (expr->left != NULL)
        jit_expr_debug_print(expr->left, indent + 1);
    if (expr->right != NULL)
        jit_expr_debug_print(expr->right, indent + 1);

    if (expr->children != NULL)
    {
        for (i = 0; i < expr->child_count; i++)
        {
            jit_expr_debug_print(expr->children[i], indent + 1);
        }
    }
}
