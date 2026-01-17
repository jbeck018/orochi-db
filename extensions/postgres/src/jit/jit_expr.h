/*-------------------------------------------------------------------------
 *
 * jit_expr.h
 *    Internal header for JIT expression compilation
 *
 * This header declares internal functions used by the JIT compiler
 * for generating specialized evaluation code.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JIT_EXPR_H
#define OROCHI_JIT_EXPR_H

#include "jit.h"

/* ============================================================
 * Expression Evaluation Code Generation
 * ============================================================ */

/*
 * Generate scalar evaluation function for int64 result
 */
extern JitEvalInt64Fn jit_generate_eval_int64(JitContext *ctx, JitExprNode *expr);

/*
 * Generate scalar evaluation function for int32 result
 */
extern JitEvalInt32Fn jit_generate_eval_int32(JitContext *ctx, JitExprNode *expr);

/*
 * Generate scalar evaluation function for float64 result
 */
extern JitEvalFloat64Fn jit_generate_eval_float64(JitContext *ctx, JitExprNode *expr);

/*
 * Generate scalar evaluation function for float32 result
 */
extern JitEvalFloat32Fn jit_generate_eval_float32(JitContext *ctx, JitExprNode *expr);

/*
 * Generate scalar evaluation function for boolean result
 */
extern JitEvalBoolFn jit_generate_eval_bool(JitContext *ctx, JitExprNode *expr);

/*
 * Generate vectorized batch filter function
 */
extern JitFilterExprFn jit_generate_batch_filter(JitContext *ctx, JitExprNode *expr);

/*
 * Generate specialized sum function for int64
 */
extern JitSumInt64Fn jit_generate_sum_int64(JitContext *ctx, int32 column_index);

/*
 * Generate specialized sum function for float64
 */
extern JitSumFloat64Fn jit_generate_sum_float64(JitContext *ctx, int32 column_index);

/* ============================================================
 * Expression Tree Utilities
 * ============================================================ */

/*
 * Extract all column references from an expression tree
 */
extern void jit_extract_column_refs(JitExprNode *expr, int32 *columns,
                                    int32 *count, int32 max_columns);

/*
 * Check if expression is a simple constant
 */
extern bool jit_expr_is_const(JitExprNode *expr);

/*
 * Check if expression is a simple column reference
 */
extern bool jit_expr_is_column(JitExprNode *expr);

/*
 * Check if expression is a simple comparison (column op const)
 */
extern bool jit_expr_is_simple_comparison(JitExprNode *expr);

/*
 * Optimize expression tree (constant folding, etc.)
 */
extern JitExprNode *jit_optimize_expr(JitContext *ctx, JitExprNode *expr);

/* ============================================================
 * Type Conversion Helpers
 * ============================================================ */

/*
 * Convert value between numeric types
 */
extern int64 jit_convert_to_int64(Datum value, VectorizedDataType from_type);
extern int32 jit_convert_to_int32(Datum value, VectorizedDataType from_type);
extern double jit_convert_to_float64(Datum value, VectorizedDataType from_type);
extern float jit_convert_to_float32(Datum value, VectorizedDataType from_type);
extern bool jit_convert_to_bool(Datum value, VectorizedDataType from_type);

#endif /* OROCHI_JIT_EXPR_H */
