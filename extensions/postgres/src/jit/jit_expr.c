/*-------------------------------------------------------------------------
 *
 * jit_expr.c
 *    JIT expression compilation for arithmetic, comparison, and boolean ops
 *
 * This module generates specialized evaluation functions for different
 * expression types. Instead of interpreting expression trees at runtime,
 * we generate type-specific function pointers that directly evaluate
 * the expression with minimal overhead.
 *
 * The approach uses template functions with captured expression context
 * rather than actual machine code generation (which would require LLVM).
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "fmgr.h"
#include "postgres.h"
#include "utils/memutils.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "jit.h"
#include "jit_expr.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* ============================================================
 * Expression Evaluation Context
 *
 * Since we can't generate actual machine code, we use a closure-like
 * pattern where the compiled function captures the expression tree
 * and evaluates it with the given data.
 * ============================================================ */

/*
 * Evaluation context passed to generated functions
 * This holds pointers to column data for fast access
 */
typedef struct JitEvalContext {
  VectorBatch *batch;
  JitExprNode *expr;
  int32 row_idx;
  bool *is_null;
} JitEvalContext;

/* Thread-local expression context for evaluation */
static __thread JitExprNode *tls_current_expr = NULL;

/* ============================================================
 * Forward Declarations - Expression Evaluators
 * ============================================================ */

static int64 jit_eval_int64_internal(VectorBatch *batch, JitExprNode *expr,
                                     int32 row_idx, bool *is_null);
static int32 jit_eval_int32_internal(VectorBatch *batch, JitExprNode *expr,
                                     int32 row_idx, bool *is_null);
static double jit_eval_float64_internal(VectorBatch *batch, JitExprNode *expr,
                                        int32 row_idx, bool *is_null);
static float jit_eval_float32_internal(VectorBatch *batch, JitExprNode *expr,
                                       int32 row_idx, bool *is_null);
static bool jit_eval_bool_internal(VectorBatch *batch, JitExprNode *expr,
                                   int32 row_idx, bool *is_null);

/* ============================================================
 * Type Conversion Helpers
 * ============================================================ */

int64 jit_convert_to_int64(Datum value, VectorizedDataType from_type) {
  switch (from_type) {
  case VECTORIZED_TYPE_INT64:
  case VECTORIZED_TYPE_TIMESTAMP:
    return DatumGetInt64(value);
  case VECTORIZED_TYPE_INT32:
  case VECTORIZED_TYPE_DATE:
    return (int64)DatumGetInt32(value);
  case VECTORIZED_TYPE_FLOAT64:
    return (int64)DatumGetFloat8(value);
  case VECTORIZED_TYPE_FLOAT32:
    return (int64)DatumGetFloat4(value);
  case VECTORIZED_TYPE_BOOL:
    return DatumGetBool(value) ? 1 : 0;
  default:
    return 0;
  }
}

int32 jit_convert_to_int32(Datum value, VectorizedDataType from_type) {
  switch (from_type) {
  case VECTORIZED_TYPE_INT64:
  case VECTORIZED_TYPE_TIMESTAMP:
    return (int32)DatumGetInt64(value);
  case VECTORIZED_TYPE_INT32:
  case VECTORIZED_TYPE_DATE:
    return DatumGetInt32(value);
  case VECTORIZED_TYPE_FLOAT64:
    return (int32)DatumGetFloat8(value);
  case VECTORIZED_TYPE_FLOAT32:
    return (int32)DatumGetFloat4(value);
  case VECTORIZED_TYPE_BOOL:
    return DatumGetBool(value) ? 1 : 0;
  default:
    return 0;
  }
}

double jit_convert_to_float64(Datum value, VectorizedDataType from_type) {
  switch (from_type) {
  case VECTORIZED_TYPE_INT64:
  case VECTORIZED_TYPE_TIMESTAMP:
    return (double)DatumGetInt64(value);
  case VECTORIZED_TYPE_INT32:
  case VECTORIZED_TYPE_DATE:
    return (double)DatumGetInt32(value);
  case VECTORIZED_TYPE_FLOAT64:
    return DatumGetFloat8(value);
  case VECTORIZED_TYPE_FLOAT32:
    return (double)DatumGetFloat4(value);
  case VECTORIZED_TYPE_BOOL:
    return DatumGetBool(value) ? 1.0 : 0.0;
  default:
    return 0.0;
  }
}

float jit_convert_to_float32(Datum value, VectorizedDataType from_type) {
  switch (from_type) {
  case VECTORIZED_TYPE_INT64:
  case VECTORIZED_TYPE_TIMESTAMP:
    return (float)DatumGetInt64(value);
  case VECTORIZED_TYPE_INT32:
  case VECTORIZED_TYPE_DATE:
    return (float)DatumGetInt32(value);
  case VECTORIZED_TYPE_FLOAT64:
    return (float)DatumGetFloat8(value);
  case VECTORIZED_TYPE_FLOAT32:
    return DatumGetFloat4(value);
  case VECTORIZED_TYPE_BOOL:
    return DatumGetBool(value) ? 1.0f : 0.0f;
  default:
    return 0.0f;
  }
}

bool jit_convert_to_bool(Datum value, VectorizedDataType from_type) {
  switch (from_type) {
  case VECTORIZED_TYPE_INT64:
  case VECTORIZED_TYPE_TIMESTAMP:
    return DatumGetInt64(value) != 0;
  case VECTORIZED_TYPE_INT32:
  case VECTORIZED_TYPE_DATE:
    return DatumGetInt32(value) != 0;
  case VECTORIZED_TYPE_FLOAT64:
    return DatumGetFloat8(value) != 0.0;
  case VECTORIZED_TYPE_FLOAT32:
    return DatumGetFloat4(value) != 0.0f;
  case VECTORIZED_TYPE_BOOL:
    return DatumGetBool(value);
  default:
    return false;
  }
}

/* ============================================================
 * Expression Tree Utilities
 * ============================================================ */

bool jit_expr_is_const(JitExprNode *expr) {
  return expr != NULL && expr->type == JIT_EXPR_CONST;
}

bool jit_expr_is_column(JitExprNode *expr) {
  return expr != NULL && expr->type == JIT_EXPR_COLUMN;
}

bool jit_expr_is_simple_comparison(JitExprNode *expr) {
  if (expr == NULL)
    return false;

  /* Check if it's a comparison operator */
  if (expr->type < JIT_EXPR_CMP_EQ || expr->type > JIT_EXPR_CMP_GE)
    return false;

  /* Check if one side is column and other is constant */
  bool left_col = jit_expr_is_column(expr->left);
  bool right_col = jit_expr_is_column(expr->right);
  bool left_const = jit_expr_is_const(expr->left);
  bool right_const = jit_expr_is_const(expr->right);

  return (left_col && right_const) || (left_const && right_col);
}

void jit_extract_column_refs(JitExprNode *expr, int32 *columns, int32 *count,
                             int32 max_columns) {
  int i;

  if (expr == NULL || *count >= max_columns)
    return;

  if (expr->type == JIT_EXPR_COLUMN) {
    /* Check if column already recorded */
    for (i = 0; i < *count; i++) {
      if (columns[i] == expr->column_index)
        return; /* Already have this column */
    }

    /* Add new column reference */
    if (*count < max_columns) {
      columns[(*count)++] = expr->column_index;
    }
    return;
  }

  /* Recursively extract from children */
  if (expr->left != NULL)
    jit_extract_column_refs(expr->left, columns, count, max_columns);
  if (expr->right != NULL)
    jit_extract_column_refs(expr->right, columns, count, max_columns);

  if (expr->children != NULL) {
    for (i = 0; i < expr->child_count; i++) {
      jit_extract_column_refs(expr->children[i], columns, count, max_columns);
    }
  }
}

JitExprNode *jit_optimize_expr(JitContext *ctx, JitExprNode *expr) {
  /* Simple constant folding optimization */
  if (expr == NULL)
    return NULL;

  /* Recursively optimize children first */
  if (expr->left != NULL)
    expr->left = jit_optimize_expr(ctx, expr->left);
  if (expr->right != NULL)
    expr->right = jit_optimize_expr(ctx, expr->right);

  /* Check for constant folding opportunities */
  if (expr->left != NULL && expr->right != NULL &&
      jit_expr_is_const(expr->left) && jit_expr_is_const(expr->right)) {
    /* Both operands are constants - evaluate at compile time */
    bool is_null = false;

    switch (expr->type) {
    case JIT_EXPR_ARITH_ADD:
    case JIT_EXPR_ARITH_SUB:
    case JIT_EXPR_ARITH_MUL:
    case JIT_EXPR_ARITH_DIV:
    case JIT_EXPR_ARITH_MOD: {
      /* Fold arithmetic operations */
      if (expr->result_type == VECTORIZED_TYPE_INT64) {
        int64 left_val = jit_convert_to_int64(expr->left->const_value,
                                              expr->left->result_type);
        int64 right_val = jit_convert_to_int64(expr->right->const_value,
                                               expr->right->result_type);
        int64 result = 0;

        switch (expr->type) {
        case JIT_EXPR_ARITH_ADD:
          result = left_val + right_val;
          break;
        case JIT_EXPR_ARITH_SUB:
          result = left_val - right_val;
          break;
        case JIT_EXPR_ARITH_MUL:
          result = left_val * right_val;
          break;
        case JIT_EXPR_ARITH_DIV:
          if (right_val != 0)
            result = left_val / right_val;
          break;
        case JIT_EXPR_ARITH_MOD:
          if (right_val != 0)
            result = left_val % right_val;
          break;
        default:
          break;
        }

        return jit_expr_const_int64(ctx, result);
      } else if (expr->result_type == VECTORIZED_TYPE_FLOAT64) {
        double left_val = jit_convert_to_float64(expr->left->const_value,
                                                 expr->left->result_type);
        double right_val = jit_convert_to_float64(expr->right->const_value,
                                                  expr->right->result_type);
        double result = 0.0;

        switch (expr->type) {
        case JIT_EXPR_ARITH_ADD:
          result = left_val + right_val;
          break;
        case JIT_EXPR_ARITH_SUB:
          result = left_val - right_val;
          break;
        case JIT_EXPR_ARITH_MUL:
          result = left_val * right_val;
          break;
        case JIT_EXPR_ARITH_DIV:
          if (right_val != 0.0)
            result = left_val / right_val;
          break;
        default:
          break;
        }

        return jit_expr_const_float64(ctx, result);
      }
    } break;

    case JIT_EXPR_CMP_EQ:
    case JIT_EXPR_CMP_NE:
    case JIT_EXPR_CMP_LT:
    case JIT_EXPR_CMP_LE:
    case JIT_EXPR_CMP_GT:
    case JIT_EXPR_CMP_GE: {
      /* Fold comparison operations */
      double left_val = jit_convert_to_float64(expr->left->const_value,
                                               expr->left->result_type);
      double right_val = jit_convert_to_float64(expr->right->const_value,
                                                expr->right->result_type);
      bool result = false;

      switch (expr->type) {
      case JIT_EXPR_CMP_EQ:
        result = (left_val == right_val);
        break;
      case JIT_EXPR_CMP_NE:
        result = (left_val != right_val);
        break;
      case JIT_EXPR_CMP_LT:
        result = (left_val < right_val);
        break;
      case JIT_EXPR_CMP_LE:
        result = (left_val <= right_val);
        break;
      case JIT_EXPR_CMP_GT:
        result = (left_val > right_val);
        break;
      case JIT_EXPR_CMP_GE:
        result = (left_val >= right_val);
        break;
      default:
        break;
      }

      return jit_expr_const_bool(ctx, result);
    } break;

    case JIT_EXPR_BOOL_AND: {
      bool left_val =
          jit_convert_to_bool(expr->left->const_value, expr->left->result_type);
      bool right_val = jit_convert_to_bool(expr->right->const_value,
                                           expr->right->result_type);
      return jit_expr_const_bool(ctx, left_val && right_val);
    }

    case JIT_EXPR_BOOL_OR: {
      bool left_val =
          jit_convert_to_bool(expr->left->const_value, expr->left->result_type);
      bool right_val = jit_convert_to_bool(expr->right->const_value,
                                           expr->right->result_type);
      return jit_expr_const_bool(ctx, left_val || right_val);
    }

    default:
      break;
    }
  }

  /* Simplify boolean expressions with constant operands */
  if (expr->type == JIT_EXPR_BOOL_AND) {
    if (jit_expr_is_const(expr->left)) {
      bool val =
          jit_convert_to_bool(expr->left->const_value, expr->left->result_type);
      if (!val)
        return jit_expr_const_bool(ctx, false); /* false AND x = false */
      else
        return expr->right; /* true AND x = x */
    }
    if (jit_expr_is_const(expr->right)) {
      bool val = jit_convert_to_bool(expr->right->const_value,
                                     expr->right->result_type);
      if (!val)
        return jit_expr_const_bool(ctx, false); /* x AND false = false */
      else
        return expr->left; /* x AND true = x */
    }
  }

  if (expr->type == JIT_EXPR_BOOL_OR) {
    if (jit_expr_is_const(expr->left)) {
      bool val =
          jit_convert_to_bool(expr->left->const_value, expr->left->result_type);
      if (val)
        return jit_expr_const_bool(ctx, true); /* true OR x = true */
      else
        return expr->right; /* false OR x = x */
    }
    if (jit_expr_is_const(expr->right)) {
      bool val = jit_convert_to_bool(expr->right->const_value,
                                     expr->right->result_type);
      if (val)
        return jit_expr_const_bool(ctx, true); /* x OR true = true */
      else
        return expr->left; /* x OR false = x */
    }
  }

  if (expr->type == JIT_EXPR_BOOL_NOT && jit_expr_is_const(expr->left)) {
    bool val =
        jit_convert_to_bool(expr->left->const_value, expr->left->result_type);
    return jit_expr_const_bool(ctx, !val);
  }

  return expr;
}

/* ============================================================
 * Internal Expression Evaluators
 * ============================================================ */

static int64 jit_eval_int64_internal(VectorBatch *batch, JitExprNode *expr,
                                     int32 row_idx, bool *is_null) {
  int64 left_val, right_val;
  bool left_null, right_null;

  *is_null = false;

  switch (expr->type) {
  case JIT_EXPR_CONST:
    *is_null = expr->const_is_null;
    return jit_convert_to_int64(expr->const_value, expr->result_type);

  case JIT_EXPR_COLUMN: {
    VectorColumn *col = batch->columns[expr->column_index];
    if (col->has_nulls &&
        (col->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
      *is_null = true;
      return 0;
    }
    return ((int64 *)col->data)[row_idx];
  }

  case JIT_EXPR_ARITH_ADD:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val + right_val;

  case JIT_EXPR_ARITH_SUB:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val - right_val;

  case JIT_EXPR_ARITH_MUL:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val * right_val;

  case JIT_EXPR_ARITH_DIV:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    if (right_val == 0) {
      *is_null = true;
      return 0;
    }
    return left_val / right_val;

  case JIT_EXPR_ARITH_MOD:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    if (right_val == 0) {
      *is_null = true;
      return 0;
    }
    return left_val % right_val;

  case JIT_EXPR_ARITH_NEG:
    left_val = jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
    *is_null = left_null;
    return -left_val;

  case JIT_EXPR_CAST: {
    /* Handle type conversion */
    switch (expr->left->result_type) {
    case VECTORIZED_TYPE_INT64:
    case VECTORIZED_TYPE_TIMESTAMP:
      return jit_eval_int64_internal(batch, expr->left, row_idx, is_null);
    case VECTORIZED_TYPE_INT32:
    case VECTORIZED_TYPE_DATE:
      return (int64)jit_eval_int32_internal(batch, expr->left, row_idx,
                                            is_null);
    case VECTORIZED_TYPE_FLOAT64:
      return (int64)jit_eval_float64_internal(batch, expr->left, row_idx,
                                              is_null);
    case VECTORIZED_TYPE_FLOAT32:
      return (int64)jit_eval_float32_internal(batch, expr->left, row_idx,
                                              is_null);
    case VECTORIZED_TYPE_BOOL:
      return jit_eval_bool_internal(batch, expr->left, row_idx, is_null) ? 1
                                                                         : 0;
    default:
      return 0;
    }
  }

  default:
    *is_null = true;
    return 0;
  }
}

static int32 jit_eval_int32_internal(VectorBatch *batch, JitExprNode *expr,
                                     int32 row_idx, bool *is_null) {
  int32 left_val, right_val;
  bool left_null, right_null;

  *is_null = false;

  switch (expr->type) {
  case JIT_EXPR_CONST:
    *is_null = expr->const_is_null;
    return jit_convert_to_int32(expr->const_value, expr->result_type);

  case JIT_EXPR_COLUMN: {
    VectorColumn *col = batch->columns[expr->column_index];
    if (col->has_nulls &&
        (col->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
      *is_null = true;
      return 0;
    }
    return ((int32 *)col->data)[row_idx];
  }

  case JIT_EXPR_ARITH_ADD:
    left_val = jit_eval_int32_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int32_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val + right_val;

  case JIT_EXPR_ARITH_SUB:
    left_val = jit_eval_int32_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int32_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val - right_val;

  case JIT_EXPR_ARITH_MUL:
    left_val = jit_eval_int32_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_int32_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val * right_val;

  case JIT_EXPR_ARITH_NEG:
    left_val = jit_eval_int32_internal(batch, expr->left, row_idx, &left_null);
    *is_null = left_null;
    return -left_val;

  case JIT_EXPR_CAST:
    return (int32)jit_eval_int64_internal(batch, expr->left, row_idx, is_null);

  default:
    *is_null = true;
    return 0;
  }
}

static double jit_eval_float64_internal(VectorBatch *batch, JitExprNode *expr,
                                        int32 row_idx, bool *is_null) {
  double left_val, right_val;
  bool left_null, right_null;

  *is_null = false;

  switch (expr->type) {
  case JIT_EXPR_CONST:
    *is_null = expr->const_is_null;
    return jit_convert_to_float64(expr->const_value, expr->result_type);

  case JIT_EXPR_COLUMN: {
    VectorColumn *col = batch->columns[expr->column_index];
    if (col->has_nulls &&
        (col->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
      *is_null = true;
      return 0.0;
    }
    return ((double *)col->data)[row_idx];
  }

  case JIT_EXPR_ARITH_ADD:
    left_val =
        jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_float64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val + right_val;

  case JIT_EXPR_ARITH_SUB:
    left_val =
        jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_float64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val - right_val;

  case JIT_EXPR_ARITH_MUL:
    left_val =
        jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_float64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    return left_val * right_val;

  case JIT_EXPR_ARITH_DIV:
    left_val =
        jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
    right_val =
        jit_eval_float64_internal(batch, expr->right, row_idx, &right_null);
    *is_null = left_null || right_null;
    if (right_val == 0.0) {
      *is_null = true;
      return 0.0;
    }
    return left_val / right_val;

  case JIT_EXPR_ARITH_NEG:
    left_val =
        jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
    *is_null = left_null;
    return -left_val;

  case JIT_EXPR_CAST: {
    switch (expr->left->result_type) {
    case VECTORIZED_TYPE_INT64:
    case VECTORIZED_TYPE_TIMESTAMP:
      return (double)jit_eval_int64_internal(batch, expr->left, row_idx,
                                             is_null);
    case VECTORIZED_TYPE_INT32:
    case VECTORIZED_TYPE_DATE:
      return (double)jit_eval_int32_internal(batch, expr->left, row_idx,
                                             is_null);
    case VECTORIZED_TYPE_FLOAT64:
      return jit_eval_float64_internal(batch, expr->left, row_idx, is_null);
    case VECTORIZED_TYPE_FLOAT32:
      return (double)jit_eval_float32_internal(batch, expr->left, row_idx,
                                               is_null);
    default:
      return 0.0;
    }
  }

  default:
    *is_null = true;
    return 0.0;
  }
}

static float jit_eval_float32_internal(VectorBatch *batch, JitExprNode *expr,
                                       int32 row_idx, bool *is_null) {
  *is_null = false;

  switch (expr->type) {
  case JIT_EXPR_CONST:
    *is_null = expr->const_is_null;
    return jit_convert_to_float32(expr->const_value, expr->result_type);

  case JIT_EXPR_COLUMN: {
    VectorColumn *col = batch->columns[expr->column_index];
    if (col->has_nulls &&
        (col->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
      *is_null = true;
      return 0.0f;
    }
    return ((float *)col->data)[row_idx];
  }

  case JIT_EXPR_CAST:
    return (float)jit_eval_float64_internal(batch, expr->left, row_idx,
                                            is_null);

  default:
    /* Delegate to float64 for other operations */
    return (float)jit_eval_float64_internal(batch, expr, row_idx, is_null);
  }
}

static bool jit_eval_bool_internal(VectorBatch *batch, JitExprNode *expr,
                                   int32 row_idx, bool *is_null) {
  bool left_val, right_val;
  bool left_null, right_null;

  *is_null = false;

  switch (expr->type) {
  case JIT_EXPR_CONST:
    *is_null = expr->const_is_null;
    return jit_convert_to_bool(expr->const_value, expr->result_type);

  case JIT_EXPR_COLUMN: {
    VectorColumn *col = batch->columns[expr->column_index];
    if (col->has_nulls &&
        (col->nulls[row_idx / 64] & (1ULL << (row_idx % 64)))) {
      *is_null = true;
      return false;
    }
    return ((bool *)col->data)[row_idx];
  }

  case JIT_EXPR_CMP_EQ:
  case JIT_EXPR_CMP_NE:
  case JIT_EXPR_CMP_LT:
  case JIT_EXPR_CMP_LE:
  case JIT_EXPR_CMP_GT:
  case JIT_EXPR_CMP_GE: {
    /* Handle comparison based on operand types */
    VectorizedDataType op_type = expr->left->result_type;
    if (expr->right->result_type == VECTORIZED_TYPE_FLOAT64 ||
        expr->left->result_type == VECTORIZED_TYPE_FLOAT64) {
      double left_f =
          jit_eval_float64_internal(batch, expr->left, row_idx, &left_null);
      double right_f =
          jit_eval_float64_internal(batch, expr->right, row_idx, &right_null);
      *is_null = left_null || right_null;
      if (*is_null)
        return false;

      switch (expr->type) {
      case JIT_EXPR_CMP_EQ:
        return left_f == right_f;
      case JIT_EXPR_CMP_NE:
        return left_f != right_f;
      case JIT_EXPR_CMP_LT:
        return left_f < right_f;
      case JIT_EXPR_CMP_LE:
        return left_f <= right_f;
      case JIT_EXPR_CMP_GT:
        return left_f > right_f;
      case JIT_EXPR_CMP_GE:
        return left_f >= right_f;
      default:
        return false;
      }
    } else {
      int64 left_i =
          jit_eval_int64_internal(batch, expr->left, row_idx, &left_null);
      int64 right_i =
          jit_eval_int64_internal(batch, expr->right, row_idx, &right_null);
      *is_null = left_null || right_null;
      if (*is_null)
        return false;

      switch (expr->type) {
      case JIT_EXPR_CMP_EQ:
        return left_i == right_i;
      case JIT_EXPR_CMP_NE:
        return left_i != right_i;
      case JIT_EXPR_CMP_LT:
        return left_i < right_i;
      case JIT_EXPR_CMP_LE:
        return left_i <= right_i;
      case JIT_EXPR_CMP_GT:
        return left_i > right_i;
      case JIT_EXPR_CMP_GE:
        return left_i >= right_i;
      default:
        return false;
      }
    }
  }

  case JIT_EXPR_BOOL_AND:
    left_val = jit_eval_bool_internal(batch, expr->left, row_idx, &left_null);
    if (left_null) {
      *is_null = true;
      return false;
    }
    if (!left_val)
      return false; /* Short-circuit: false AND x = false */
    right_val =
        jit_eval_bool_internal(batch, expr->right, row_idx, &right_null);
    *is_null = right_null;
    return right_val;

  case JIT_EXPR_BOOL_OR:
    left_val = jit_eval_bool_internal(batch, expr->left, row_idx, &left_null);
    if (left_null) {
      *is_null = true;
      return false;
    }
    if (left_val)
      return true; /* Short-circuit: true OR x = true */
    right_val =
        jit_eval_bool_internal(batch, expr->right, row_idx, &right_null);
    *is_null = right_null;
    return right_val;

  case JIT_EXPR_BOOL_NOT:
    left_val = jit_eval_bool_internal(batch, expr->left, row_idx, &left_null);
    *is_null = left_null;
    return !left_val;

  case JIT_EXPR_IS_NULL:
    jit_eval_bool_internal(batch, expr->left, row_idx, &left_null);
    return left_null;

  case JIT_EXPR_IS_NOT_NULL:
    jit_eval_bool_internal(batch, expr->left, row_idx, &left_null);
    return !left_null;

  case JIT_EXPR_BETWEEN: {
    if (expr->child_count < 3) {
      *is_null = true;
      return false;
    }

    double val = jit_eval_float64_internal(batch, expr->children[0], row_idx,
                                           &left_null);
    double low = jit_eval_float64_internal(batch, expr->children[1], row_idx,
                                           &right_null);
    bool high_null;
    double high = jit_eval_float64_internal(batch, expr->children[2], row_idx,
                                            &high_null);

    *is_null = left_null || right_null || high_null;
    if (*is_null)
      return false;

    return val >= low && val <= high;
  }

  default:
    *is_null = true;
    return false;
  }
}

/* ============================================================
 * Wrapper Functions for Generated Code
 *
 * These functions wrap the internal evaluators and store the
 * expression in thread-local storage for access.
 * ============================================================ */

static int64 jit_wrapper_eval_int64(const void *data, int32 row_idx,
                                    const uint64 *nulls, bool *is_null) {
  VectorBatch *batch = (VectorBatch *)data;
  return jit_eval_int64_internal(batch, tls_current_expr, row_idx, is_null);
}

static int32 jit_wrapper_eval_int32(const void *data, int32 row_idx,
                                    const uint64 *nulls, bool *is_null) {
  VectorBatch *batch = (VectorBatch *)data;
  return jit_eval_int32_internal(batch, tls_current_expr, row_idx, is_null);
}

static double jit_wrapper_eval_float64(const void *data, int32 row_idx,
                                       const uint64 *nulls, bool *is_null) {
  VectorBatch *batch = (VectorBatch *)data;
  return jit_eval_float64_internal(batch, tls_current_expr, row_idx, is_null);
}

static float jit_wrapper_eval_float32(const void *data, int32 row_idx,
                                      const uint64 *nulls, bool *is_null) {
  VectorBatch *batch = (VectorBatch *)data;
  return jit_eval_float32_internal(batch, tls_current_expr, row_idx, is_null);
}

static bool jit_wrapper_eval_bool(const void *data, int32 row_idx,
                                  const uint64 *nulls, bool *is_null) {
  VectorBatch *batch = (VectorBatch *)data;
  return jit_eval_bool_internal(batch, tls_current_expr, row_idx, is_null);
}

/* ============================================================
 * Batch Filter Function
 * ============================================================ */

static void jit_batch_filter_internal(VectorBatch *batch, JitExprNode *expr,
                                      bool *out_mask) {
  int32 i;
  bool is_null;

  /* Evaluate filter for each row */
  for (i = 0; i < batch->count; i++) {
    out_mask[i] = jit_eval_bool_internal(batch, expr, i, &is_null);
    if (is_null)
      out_mask[i] = false; /* NULL treated as false in filters */
  }
}

/* ============================================================
 * Code Generation Functions
 * ============================================================ */

JitEvalInt64Fn jit_generate_eval_int64(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  /* Store expression for wrapper function */
  /* Note: In a real implementation, we would generate specialized code */
  return jit_wrapper_eval_int64;
}

JitEvalInt32Fn jit_generate_eval_int32(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  return jit_wrapper_eval_int32;
}

JitEvalFloat64Fn jit_generate_eval_float64(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  return jit_wrapper_eval_float64;
}

JitEvalFloat32Fn jit_generate_eval_float32(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  return jit_wrapper_eval_float32;
}

JitEvalBoolFn jit_generate_eval_bool(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  return jit_wrapper_eval_bool;
}

JitFilterExprFn jit_generate_batch_filter(JitContext *ctx, JitExprNode *expr) {
  if (ctx == NULL || expr == NULL)
    return NULL;

  if (expr->result_type != VECTORIZED_TYPE_BOOL)
    return NULL;

  /* Return the batch filter function */
  return jit_batch_filter_internal;
}

/* ============================================================
 * Specialized Sum Functions
 * ============================================================ */

static int64 jit_sum_int64_internal(VectorBatch *batch, int32 column_index) {
  return vectorized_sum_int64(batch, column_index);
}

static double jit_sum_float64_internal(VectorBatch *batch, int32 column_index) {
  return vectorized_sum_float64(batch, column_index);
}

JitSumInt64Fn jit_generate_sum_int64(JitContext *ctx, int32 column_index) {
  if (ctx == NULL)
    return NULL;

  /* For now, return the internal function
   * A more sophisticated implementation could generate
   * SIMD-optimized code specific to the column layout */
  return jit_sum_int64_internal;
}

JitSumFloat64Fn jit_generate_sum_float64(JitContext *ctx, int32 column_index) {
  if (ctx == NULL)
    return NULL;

  return jit_sum_float64_internal;
}
