/*-------------------------------------------------------------------------
 *
 * json_query.c
 *    Orochi DB optimized JSON query execution implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "../orochi.h"
#include "json_query.h"
#include "json_index.h"
#include "json_columnar.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Memory context for query operations */
static MemoryContext JsonQueryContext = NULL;

/* Forward declarations */
static void ensure_query_context(void);
static JsonQueryExpr *create_expr_node(JsonExprType type);
static double estimate_operator_selectivity(JsonQueryOperator op,
                                            JsonPathStats *stats);
static bool eval_operator(JsonQueryOperator op, Datum left, Datum right,
                          Oid left_type, Oid right_type);

/* ============================================================
 * Utility Functions
 * ============================================================ */

static void
ensure_query_context(void)
{
    if (JsonQueryContext == NULL)
    {
        JsonQueryContext = AllocSetContextCreate(TopMemoryContext,
                                                 "JsonQueryContext",
                                                 ALLOCSET_DEFAULT_SIZES);
    }
}

static JsonQueryExpr *
create_expr_node(JsonExprType type)
{
    JsonQueryExpr *expr;

    ensure_query_context();
    expr = MemoryContextAllocZero(JsonQueryContext, sizeof(JsonQueryExpr));
    expr->type = type;
    expr->can_use_index = false;
    expr->selectivity = 1.0;
    expr->estimated_rows = -1;

    return expr;
}

const char *
json_query_operator_name(JsonQueryOperator op)
{
    switch (op)
    {
        case JSON_OP_CONTAINS:
            return "@>";
        case JSON_OP_CONTAINED_BY:
            return "<@";
        case JSON_OP_EXISTS:
            return "?";
        case JSON_OP_EXISTS_ANY:
            return "?|";
        case JSON_OP_EXISTS_ALL:
            return "?&";
        case JSON_OP_PATH_EXISTS:
            return "@?";
        case JSON_OP_PATH_MATCH:
            return "@@";
        case JSON_OP_FIELD_ACCESS:
            return "->";
        case JSON_OP_FIELD_TEXT:
            return "->>";
        case JSON_OP_PATH_ACCESS:
            return "#>";
        case JSON_OP_PATH_TEXT:
            return "#>>";
        case JSON_OP_ARRAY_ELEMENT:
            return "[]";
        case JSON_OP_EQ:
            return "=";
        case JSON_OP_NE:
            return "!=";
        case JSON_OP_LT:
            return "<";
        case JSON_OP_LE:
            return "<=";
        case JSON_OP_GT:
            return ">";
        case JSON_OP_GE:
            return ">=";
        case JSON_OP_LIKE:
            return "LIKE";
        case JSON_OP_IN:
            return "IN";
        case JSON_OP_IS_NULL:
            return "IS NULL";
        case JSON_OP_IS_NOT_NULL:
            return "IS NOT NULL";
        default:
            return "UNKNOWN";
    }
}

JsonQueryOperator
json_query_parse_operator(const char *name)
{
    if (strcmp(name, "@>") == 0)
        return JSON_OP_CONTAINS;
    if (strcmp(name, "<@") == 0)
        return JSON_OP_CONTAINED_BY;
    if (strcmp(name, "?") == 0)
        return JSON_OP_EXISTS;
    if (strcmp(name, "?|") == 0)
        return JSON_OP_EXISTS_ANY;
    if (strcmp(name, "?&") == 0)
        return JSON_OP_EXISTS_ALL;
    if (strcmp(name, "@?") == 0)
        return JSON_OP_PATH_EXISTS;
    if (strcmp(name, "@@") == 0)
        return JSON_OP_PATH_MATCH;
    if (strcmp(name, "->") == 0)
        return JSON_OP_FIELD_ACCESS;
    if (strcmp(name, "->>") == 0)
        return JSON_OP_FIELD_TEXT;
    if (strcmp(name, "#>") == 0)
        return JSON_OP_PATH_ACCESS;
    if (strcmp(name, "#>>") == 0)
        return JSON_OP_PATH_TEXT;
    if (strcmp(name, "=") == 0)
        return JSON_OP_EQ;
    if (strcmp(name, "!=") == 0 || strcmp(name, "<>") == 0)
        return JSON_OP_NE;
    if (strcmp(name, "<") == 0)
        return JSON_OP_LT;
    if (strcmp(name, "<=") == 0)
        return JSON_OP_LE;
    if (strcmp(name, ">") == 0)
        return JSON_OP_GT;
    if (strcmp(name, ">=") == 0)
        return JSON_OP_GE;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown JSON operator: %s", name)));
    return JSON_OP_EQ; /* Not reached */
}

/* ============================================================
 * Expression Building
 * ============================================================ */

JsonQueryExpr *
json_query_parse(const char *query_text)
{
    /* Simplified parser - in production would use proper SQL parsing */
    JsonQueryExpr *expr;

    if (query_text == NULL || *query_text == '\0')
        return NULL;

    /* For now, treat as a simple path expression */
    expr = create_expr_node(JSON_EXPR_PATH);
    expr->path = pstrdup(query_text);
    expr->path_components = json_index_parse_path(query_text, &expr->path_depth);

    return expr;
}

JsonQueryExpr *
json_query_build_expr(Node *pg_expr, Oid table_oid, int16 column_attnum)
{
    JsonQueryExpr *result = NULL;

    if (pg_expr == NULL)
        return NULL;

    switch (nodeTag(pg_expr))
    {
        case T_Const:
        {
            Const *c = (Const *) pg_expr;
            result = create_expr_node(JSON_EXPR_CONST);
            result->const_value = c->constvalue;
            result->const_type = c->consttype;
            result->const_isnull = c->constisnull;
            break;
        }

        case T_Var:
        {
            Var *v = (Var *) pg_expr;
            if (v->varattno == column_attnum)
            {
                result = create_expr_node(JSON_EXPR_COLUMN);
                result->column_attnum = v->varattno;
                result->column_name = get_attname(table_oid, v->varattno, false);
            }
            break;
        }

        case T_OpExpr:
        {
            OpExpr *op = (OpExpr *) pg_expr;
            char *op_name = get_opname(op->opno);

            result = create_expr_node(JSON_EXPR_OPERATOR);
            result->operator = json_query_parse_operator(op_name);

            if (list_length(op->args) >= 1)
            {
                result->left = json_query_build_expr(linitial(op->args),
                                                     table_oid, column_attnum);
            }
            if (list_length(op->args) >= 2)
            {
                result->right = json_query_build_expr(lsecond(op->args),
                                                      table_oid, column_attnum);
            }
            break;
        }

        case T_BoolExpr:
        {
            BoolExpr *b = (BoolExpr *) pg_expr;
            ListCell *lc;

            switch (b->boolop)
            {
                case AND_EXPR:
                    result = create_expr_node(JSON_EXPR_AND);
                    break;
                case OR_EXPR:
                    result = create_expr_node(JSON_EXPR_OR);
                    break;
                case NOT_EXPR:
                    result = create_expr_node(JSON_EXPR_NOT);
                    break;
            }

            result->children = NIL;
            foreach(lc, b->args)
            {
                JsonQueryExpr *child = json_query_build_expr(lfirst(lc),
                                                             table_oid,
                                                             column_attnum);
                if (child != NULL)
                    result->children = lappend(result->children, child);
            }
            break;
        }

        case T_FuncExpr:
        {
            FuncExpr *f = (FuncExpr *) pg_expr;
            ListCell *lc;

            result = create_expr_node(JSON_EXPR_FUNCTION);
            result->func_name = get_func_name(f->funcid);
            result->func_args = NIL;

            foreach(lc, f->args)
            {
                JsonQueryExpr *arg = json_query_build_expr(lfirst(lc),
                                                           table_oid,
                                                           column_attnum);
                if (arg != NULL)
                    result->func_args = lappend(result->func_args, arg);
            }
            break;
        }

        default:
            /* Unsupported expression type */
            break;
    }

    return result;
}

/* ============================================================
 * Expression Optimization
 * ============================================================ */

JsonQueryExpr *
json_query_optimize(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum)
{
    JsonPathStats *stats;
    ListCell *lc;

    if (expr == NULL)
        return NULL;

    switch (expr->type)
    {
        case JSON_EXPR_PATH:
            /* Get statistics for path */
            stats = json_index_get_path_stats(table_oid, column_attnum,
                                              expr->path);
            if (stats != NULL)
            {
                expr->selectivity = stats->selectivity;
                expr->can_use_index = stats->is_indexed;
                pfree(stats);
            }
            break;

        case JSON_EXPR_OPERATOR:
            /* Optimize children first */
            expr->left = json_query_optimize(expr->left, table_oid,
                                             column_attnum);
            expr->right = json_query_optimize(expr->right, table_oid,
                                              column_attnum);

            /* Estimate selectivity based on operator */
            if (expr->left != NULL && expr->left->type == JSON_EXPR_PATH)
            {
                stats = json_index_get_path_stats(table_oid, column_attnum,
                                                  expr->left->path);
                if (stats != NULL)
                {
                    expr->selectivity = estimate_operator_selectivity(
                        expr->operator, stats);
                    pfree(stats);
                }
            }

            /* Check if operator can use index */
            switch (expr->operator)
            {
                case JSON_OP_CONTAINS:
                case JSON_OP_EXISTS:
                case JSON_OP_EXISTS_ANY:
                case JSON_OP_EXISTS_ALL:
                    expr->can_use_index = true;
                    break;
                case JSON_OP_EQ:
                    expr->can_use_index = (expr->left != NULL &&
                                           expr->left->can_use_index);
                    break;
                default:
                    expr->can_use_index = false;
            }
            break;

        case JSON_EXPR_AND:
            /* Optimize AND by computing combined selectivity */
            expr->selectivity = 1.0;
            expr->can_use_index = false;
            foreach(lc, expr->children)
            {
                JsonQueryExpr *child = lfirst(lc);
                child = json_query_optimize(child, table_oid, column_attnum);
                lfirst(lc) = child;
                expr->selectivity *= child->selectivity;
                if (child->can_use_index)
                    expr->can_use_index = true;
            }
            break;

        case JSON_EXPR_OR:
            /* Optimize OR - use max selectivity */
            expr->selectivity = 0.0;
            expr->can_use_index = true;  /* All children must support index */
            foreach(lc, expr->children)
            {
                JsonQueryExpr *child = lfirst(lc);
                child = json_query_optimize(child, table_oid, column_attnum);
                lfirst(lc) = child;
                if (child->selectivity > expr->selectivity)
                    expr->selectivity = child->selectivity;
                if (!child->can_use_index)
                    expr->can_use_index = false;
            }
            break;

        case JSON_EXPR_NOT:
            if (expr->children != NIL)
            {
                JsonQueryExpr *child = linitial(expr->children);
                child = json_query_optimize(child, table_oid, column_attnum);
                linitial(expr->children) = child;
                expr->selectivity = 1.0 - child->selectivity;
                expr->can_use_index = false;  /* NOT typically can't use index */
            }
            break;

        case JSON_EXPR_FUNCTION:
            /* Optimize function arguments */
            foreach(lc, expr->func_args)
            {
                JsonQueryExpr *arg = lfirst(lc);
                arg = json_query_optimize(arg, table_oid, column_attnum);
                lfirst(lc) = arg;
            }
            break;

        default:
            break;
    }

    return expr;
}

static double
estimate_operator_selectivity(JsonQueryOperator op, JsonPathStats *stats)
{
    if (stats == NULL)
        return JSON_INDEX_DEFAULT_SELECTIVITY;

    switch (op)
    {
        case JSON_OP_EQ:
            /* Equality has selectivity 1/distinct_count */
            if (stats->distinct_count > 0)
                return 1.0 / stats->distinct_count;
            return 0.01;

        case JSON_OP_NE:
            /* Not equal is 1 - equality */
            if (stats->distinct_count > 0)
                return 1.0 - (1.0 / stats->distinct_count);
            return 0.99;

        case JSON_OP_LT:
        case JSON_OP_LE:
        case JSON_OP_GT:
        case JSON_OP_GE:
            /* Range predicates typically select ~33% */
            return 0.33;

        case JSON_OP_CONTAINS:
        case JSON_OP_EXISTS:
            /* Existence checks - based on null ratio */
            return 1.0 - ((double)stats->null_count / Max(stats->access_count, 1));

        case JSON_OP_LIKE:
            /* Pattern match - typically low selectivity */
            return 0.1;

        case JSON_OP_IS_NULL:
            return (double)stats->null_count / Max(stats->access_count, 1);

        case JSON_OP_IS_NOT_NULL:
            return 1.0 - ((double)stats->null_count / Max(stats->access_count, 1));

        default:
            return stats->selectivity;
    }
}

/* ============================================================
 * Query Planning
 * ============================================================ */

JsonQueryPlan *
json_query_plan(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum)
{
    JsonQueryPlan *plan;
    List *indexes;
    ListCell *lc;

    ensure_query_context();

    plan = MemoryContextAllocZero(JsonQueryContext, sizeof(JsonQueryPlan));
    plan->table_oid = table_oid;
    plan->column_attnum = column_attnum;
    plan->root_expr = expr;
    plan->usable_indexes = NIL;
    plan->selected_index = InvalidOid;
    plan->use_columnar = false;
    plan->use_batch = true;
    plan->push_down = json_query_can_pushdown(expr);

    /* Find usable indexes */
    indexes = json_index_get_indexes(table_oid, column_attnum);
    foreach(lc, indexes)
    {
        JsonIndexInfo *idx = lfirst(lc);
        if (json_query_index_compatible(expr, idx->index_oid))
        {
            plan->usable_indexes = lappend_oid(plan->usable_indexes,
                                               idx->index_oid);
        }
        json_index_free_info(idx);
    }
    list_free(indexes);

    /* Select best index */
    if (expr->can_use_index && plan->usable_indexes != NIL)
    {
        plan->selected_index = json_query_select_index(expr, table_oid,
                                                       column_attnum);
    }

    /* Check for extracted columns */
    plan->extracted_columns = NIL;
    if (expr->type == JSON_EXPR_PATH ||
        (expr->type == JSON_EXPR_OPERATOR && expr->left != NULL &&
         expr->left->type == JSON_EXPR_PATH))
    {
        List *columns = json_columnar_get_columns(table_oid, column_attnum);
        if (columns != NIL)
        {
            char *target_path = (expr->type == JSON_EXPR_PATH) ?
                expr->path : expr->left->path;

            foreach(lc, columns)
            {
                JsonExtractedColumn *col = lfirst(lc);
                if (strcmp(col->path, target_path) == 0)
                {
                    plan->extracted_columns = lappend_int(plan->extracted_columns,
                                                          col->column_id);
                    plan->use_columnar = true;
                }
                json_columnar_free_column(col);
            }
            list_free(columns);
        }
    }

    /* Estimate costs */
    json_query_estimate_cost(expr, table_oid, column_attnum,
                             &plan->startup_cost, &plan->total_cost,
                             NULL, &plan->estimated_rows);

    return plan;
}

void
json_query_free_expr(JsonQueryExpr *expr)
{
    ListCell *lc;

    if (expr == NULL)
        return;

    /* Free path components */
    if (expr->path)
        pfree(expr->path);

    if (expr->path_components)
    {
        for (int i = 0; i < expr->path_depth; i++)
        {
            if (expr->path_components[i])
                pfree(expr->path_components[i]);
        }
        pfree(expr->path_components);
    }

    /* Free column name */
    if (expr->column_name)
        pfree(expr->column_name);

    /* Free function name */
    if (expr->func_name)
        pfree(expr->func_name);

    /* Free children recursively */
    if (expr->left)
        json_query_free_expr(expr->left);
    if (expr->right)
        json_query_free_expr(expr->right);

    foreach(lc, expr->children)
    {
        json_query_free_expr(lfirst(lc));
    }
    list_free(expr->children);

    foreach(lc, expr->func_args)
    {
        json_query_free_expr(lfirst(lc));
    }
    list_free(expr->func_args);

    pfree(expr);
}

void
json_query_free_plan(JsonQueryPlan *plan)
{
    if (plan == NULL)
        return;

    /* Don't free root_expr - it might be referenced elsewhere */

    if (plan->usable_indexes)
        list_free(plan->usable_indexes);

    if (plan->extracted_columns)
        list_free(plan->extracted_columns);

    pfree(plan);
}

/* ============================================================
 * Predicate Pushdown
 * ============================================================ */

bool
json_query_can_pushdown(JsonQueryExpr *expr)
{
    if (expr == NULL)
        return false;

    switch (expr->type)
    {
        case JSON_EXPR_OPERATOR:
            /* Most operators can be pushed down */
            switch (expr->operator)
            {
                case JSON_OP_CONTAINS:
                case JSON_OP_EXISTS:
                case JSON_OP_EQ:
                case JSON_OP_NE:
                case JSON_OP_LT:
                case JSON_OP_LE:
                case JSON_OP_GT:
                case JSON_OP_GE:
                case JSON_OP_IS_NULL:
                case JSON_OP_IS_NOT_NULL:
                    return true;
                default:
                    return false;
            }

        case JSON_EXPR_AND:
            /* AND can be pushed if all children can */
            {
                ListCell *lc;
                foreach(lc, expr->children)
                {
                    if (!json_query_can_pushdown(lfirst(lc)))
                        return false;
                }
                return true;
            }

        case JSON_EXPR_OR:
            /* OR is harder to push down */
            return false;

        default:
            return false;
    }
}

List *
json_query_extract_pushable(JsonQueryExpr *expr)
{
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL)
        return NIL;

    if (json_query_can_pushdown(expr))
    {
        if (expr->type == JSON_EXPR_AND)
        {
            /* Extract all AND children */
            foreach(lc, expr->children)
            {
                List *child_pushable = json_query_extract_pushable(lfirst(lc));
                result = list_concat(result, child_pushable);
            }
        }
        else
        {
            result = lappend(result, expr);
        }
    }

    return result;
}

JsonQueryExpr *
json_query_apply_pushdown(JsonQueryExpr *expr, List *pushed_predicates)
{
    /* Remove pushed predicates from expression tree */
    /* This is a simplified implementation */
    if (list_member_ptr(pushed_predicates, expr))
        return NULL;

    return expr;
}

ScanKey
json_query_to_scankey(JsonQueryExpr *expr, int *nkeys)
{
    /* Convert expression to scan keys for index scan */
    /* Simplified implementation */
    *nkeys = 0;
    return NULL;
}

/* ============================================================
 * Index Integration
 * ============================================================ */

bool
json_query_index_compatible(JsonQueryExpr *expr, Oid index_oid)
{
    Relation    index_rel;
    bool        compatible = false;

    index_rel = index_open(index_oid, AccessShareLock);

    /* GIN indexes support containment and existence operators */
    if (index_rel->rd_rel->relam == GIN_AM_OID)
    {
        if (expr->type == JSON_EXPR_OPERATOR)
        {
            switch (expr->operator)
            {
                case JSON_OP_CONTAINS:
                case JSON_OP_EXISTS:
                case JSON_OP_EXISTS_ANY:
                case JSON_OP_EXISTS_ALL:
                    compatible = true;
                    break;
                default:
                    compatible = false;
            }
        }
    }
    /* B-tree indexes support comparison operators */
    else if (index_rel->rd_rel->relam == BTREE_AM_OID)
    {
        if (expr->type == JSON_EXPR_OPERATOR)
        {
            switch (expr->operator)
            {
                case JSON_OP_EQ:
                case JSON_OP_LT:
                case JSON_OP_LE:
                case JSON_OP_GT:
                case JSON_OP_GE:
                    compatible = true;
                    break;
                default:
                    compatible = false;
            }
        }
    }
    /* Hash indexes support equality only */
    else if (index_rel->rd_rel->relam == HASH_AM_OID)
    {
        compatible = (expr->type == JSON_EXPR_OPERATOR &&
                      expr->operator == JSON_OP_EQ);
    }

    index_close(index_rel, AccessShareLock);
    return compatible;
}

Oid
json_query_select_index(JsonQueryExpr *expr, Oid table_oid, int16 column_attnum)
{
    List       *indexes;
    ListCell   *lc;
    Oid         best_index = InvalidOid;
    double      best_cost = DBL_MAX;

    indexes = json_index_get_indexes(table_oid, column_attnum);

    foreach(lc, indexes)
    {
        JsonIndexInfo *idx = lfirst(lc);
        double startup, total;

        if (!json_query_index_compatible(expr, idx->index_oid))
        {
            json_index_free_info(idx);
            continue;
        }

        json_query_estimate_index_cost(expr, idx->index_oid, &startup, &total);

        if (total < best_cost)
        {
            best_cost = total;
            best_index = idx->index_oid;
        }

        json_index_free_info(idx);
    }

    list_free(indexes);
    return best_index;
}

void
json_query_build_index_scan(JsonQueryExpr *expr, Oid index_oid,
                            ScanKey *keys, int *nkeys)
{
    /* Build scan keys for index scan */
    *keys = NULL;
    *nkeys = 0;

    /* Simplified - in production would build proper scan keys */
}

/* ============================================================
 * Query Execution
 * ============================================================ */

JsonQueryState *
json_query_begin(JsonQueryPlan *plan, EState *estate)
{
    JsonQueryState *state;
    Relation    relation;

    ensure_query_context();

    state = MemoryContextAllocZero(JsonQueryContext, sizeof(JsonQueryState));
    state->plan = plan;
    state->estate = estate;
    state->query_context = AllocSetContextCreate(JsonQueryContext,
                                                 "JsonQueryExec",
                                                 ALLOCSET_DEFAULT_SIZES);

    /* Open table for scanning */
    relation = table_open(plan->table_oid, AccessShareLock);
    state->slot = table_slot_create(relation, NULL);

    /* Initialize batch buffers */
    if (plan->use_batch)
    {
        state->batch_values = palloc(JSON_QUERY_BATCH_SIZE * sizeof(Datum));
        state->batch_nulls = palloc(JSON_QUERY_BATCH_SIZE * sizeof(bool));
        state->batch_results = palloc(JSON_QUERY_BATCH_SIZE * sizeof(bool));
        state->batch_size = 0;
        state->batch_pos = 0;
    }

    /* Start table scan or index scan */
    if (plan->selected_index != InvalidOid)
    {
        Relation index_rel = index_open(plan->selected_index, AccessShareLock);
        ScanKey keys = NULL;
        int nkeys = 0;

        json_query_build_index_scan(plan->root_expr, plan->selected_index,
                                    &keys, &nkeys);

        state->index_scan = index_beginscan(relation, index_rel,
                                            GetActiveSnapshot(),
                                            nkeys, 0);
        if (nkeys > 0 && keys != NULL)
            index_rescan(state->index_scan, keys, nkeys, NULL, 0);

        state->use_index = true;
        index_close(index_rel, AccessShareLock);
    }
    else
    {
        state->scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL);
        state->use_index = false;
    }

    state->current_row = 0;
    state->rows_scanned = 0;
    state->rows_matched = 0;

    table_close(relation, AccessShareLock);

    return state;
}

bool
json_query_next(JsonQueryState *state, TupleTableSlot *result_slot)
{
    Relation    relation;
    bool        found = false;

    relation = table_open(state->plan->table_oid, AccessShareLock);

    while (!found)
    {
        bool        got_tuple = false;
        bool        isnull;
        Datum       json_datum;
        Jsonb      *jb;
        bool        matches;
        bool        result_null;

        /* Get next tuple */
        if (state->use_index)
        {
            got_tuple = index_getnext_slot(state->index_scan,
                                           ForwardScanDirection,
                                           state->slot);
        }
        else
        {
            got_tuple = table_scan_getnextslot(state->scan,
                                               ForwardScanDirection,
                                               state->slot);
        }

        if (!got_tuple)
            break;

        state->rows_scanned++;

        /* Get JSON column value */
        json_datum = slot_getattr(state->slot, state->plan->column_attnum,
                                  &isnull);
        if (isnull)
            continue;

        jb = DatumGetJsonbP(json_datum);

        /* Evaluate filter expression */
        matches = json_query_eval(state->plan->root_expr, jb, &result_null);

        if (matches && !result_null)
        {
            /* Copy tuple to result slot */
            ExecCopySlot(result_slot, state->slot);
            state->rows_matched++;
            found = true;
        }

        state->current_row++;

        /* Allow interrupts */
        if (state->rows_scanned % 10000 == 0)
            CHECK_FOR_INTERRUPTS();
    }

    table_close(relation, AccessShareLock);

    return found;
}

int
json_query_next_batch(JsonQueryState *state, TupleTableSlot **slots,
                      int max_slots)
{
    int         count = 0;

    while (count < max_slots)
    {
        if (!json_query_next(state, slots[count]))
            break;
        count++;
    }

    return count;
}

void
json_query_rescan(JsonQueryState *state)
{
    if (state->use_index)
    {
        index_rescan(state->index_scan, NULL, 0, NULL, 0);
    }
    else
    {
        table_rescan(state->scan, NULL);
    }

    state->current_row = 0;
    state->batch_size = 0;
    state->batch_pos = 0;
}

void
json_query_end(JsonQueryState *state)
{
    if (state == NULL)
        return;

    if (state->use_index && state->index_scan)
    {
        index_endscan(state->index_scan);
    }
    else if (state->scan)
    {
        table_endscan(state->scan);
    }

    if (state->slot)
        ExecDropSingleTupleTableSlot(state->slot);

    if (state->batch_values)
        pfree(state->batch_values);
    if (state->batch_nulls)
        pfree(state->batch_nulls);
    if (state->batch_results)
        pfree(state->batch_results);

    if (state->query_context)
        MemoryContextDelete(state->query_context);

    pfree(state);
}

/* ============================================================
 * Expression Evaluation
 * ============================================================ */

static bool
eval_operator(JsonQueryOperator op, Datum left, Datum right,
              Oid left_type, Oid right_type)
{
    switch (op)
    {
        case JSON_OP_EQ:
            if (left_type == TEXTOID && right_type == TEXTOID)
            {
                return DatumGetBool(DirectFunctionCall2(texteq, left, right));
            }
            if (left_type == INT4OID && right_type == INT4OID)
            {
                return DatumGetInt32(left) == DatumGetInt32(right);
            }
            if (left_type == FLOAT8OID && right_type == FLOAT8OID)
            {
                return DatumGetFloat8(left) == DatumGetFloat8(right);
            }
            return false;

        case JSON_OP_NE:
            return !eval_operator(JSON_OP_EQ, left, right, left_type, right_type);

        case JSON_OP_LT:
            if (left_type == INT4OID && right_type == INT4OID)
                return DatumGetInt32(left) < DatumGetInt32(right);
            if (left_type == FLOAT8OID && right_type == FLOAT8OID)
                return DatumGetFloat8(left) < DatumGetFloat8(right);
            return false;

        case JSON_OP_LE:
            return eval_operator(JSON_OP_LT, left, right, left_type, right_type) ||
                   eval_operator(JSON_OP_EQ, left, right, left_type, right_type);

        case JSON_OP_GT:
            return !eval_operator(JSON_OP_LE, left, right, left_type, right_type);

        case JSON_OP_GE:
            return !eval_operator(JSON_OP_LT, left, right, left_type, right_type);

        default:
            return false;
    }
}

bool
json_query_eval(JsonQueryExpr *expr, Jsonb *value, bool *is_null)
{
    ListCell *lc;

    if (expr == NULL)
    {
        *is_null = true;
        return false;
    }

    *is_null = false;

    switch (expr->type)
    {
        case JSON_EXPR_CONST:
            if (expr->const_isnull)
            {
                *is_null = true;
                return false;
            }
            if (expr->const_type == BOOLOID)
                return DatumGetBool(expr->const_value);
            return true;  /* Non-null value is truthy */

        case JSON_EXPR_PATH:
        {
            /* Check if path exists and has non-null value */
            JsonPathType type = json_index_get_path_type(value, expr->path);
            if (type == JSON_PATH_NULL)
            {
                *is_null = true;
                return false;
            }
            return true;
        }

        case JSON_EXPR_COLUMN:
            /* Column reference evaluates to the input value */
            if (value == NULL)
            {
                *is_null = true;
                return false;
            }
            return true;

        case JSON_EXPR_OPERATOR:
        {
            Datum left_val = (Datum) 0;
            Datum right_val = (Datum) 0;
            Oid left_type = UNKNOWNOID;
            Oid right_type = UNKNOWNOID;
            bool left_null = true;
            bool right_null = true;

            /* Handle special operators first */
            switch (expr->operator)
            {
                case JSON_OP_CONTAINS:
                {
                    /* Check JSONB containment */
                    if (expr->right == NULL || expr->right->type != JSON_EXPR_CONST)
                        return false;

                    Jsonb *pattern = DatumGetJsonbP(expr->right->const_value);
                    return DatumGetBool(DirectFunctionCall2(jsonb_contains,
                                                            JsonbPGetDatum(value),
                                                            JsonbPGetDatum(pattern)));
                }

                case JSON_OP_EXISTS:
                {
                    /* Check key existence */
                    if (expr->right == NULL || expr->right->type != JSON_EXPR_CONST)
                        return false;

                    text *key = DatumGetTextP(expr->right->const_value);
                    return DatumGetBool(DirectFunctionCall2(jsonb_exists,
                                                            JsonbPGetDatum(value),
                                                            PointerGetDatum(key)));
                }

                case JSON_OP_IS_NULL:
                {
                    if (expr->left != NULL && expr->left->type == JSON_EXPR_PATH)
                    {
                        JsonPathType type = json_index_get_path_type(value,
                                                                     expr->left->path);
                        return type == JSON_PATH_NULL;
                    }
                    return value == NULL;
                }

                case JSON_OP_IS_NOT_NULL:
                {
                    if (expr->left != NULL && expr->left->type == JSON_EXPR_PATH)
                    {
                        JsonPathType type = json_index_get_path_type(value,
                                                                     expr->left->path);
                        return type != JSON_PATH_NULL;
                    }
                    return value != NULL;
                }

                default:
                    break;
            }

            /* Evaluate operands for comparison operators */
            if (expr->left != NULL)
            {
                if (expr->left->type == JSON_EXPR_PATH)
                {
                    Datum path_datum = CStringGetTextDatum(expr->left->path);
                    PG_TRY();
                    {
                        left_val = DirectFunctionCall2(jsonb_object_field_text,
                                                       JsonbPGetDatum(value),
                                                       path_datum);
                        left_null = (left_val == (Datum) 0);
                        left_type = TEXTOID;
                    }
                    PG_CATCH();
                    {
                        FlushErrorState();
                        left_null = true;
                    }
                    PG_END_TRY();
                }
                else if (expr->left->type == JSON_EXPR_CONST)
                {
                    left_val = expr->left->const_value;
                    left_type = expr->left->const_type;
                    left_null = expr->left->const_isnull;
                }
            }

            if (expr->right != NULL)
            {
                if (expr->right->type == JSON_EXPR_CONST)
                {
                    right_val = expr->right->const_value;
                    right_type = expr->right->const_type;
                    right_null = expr->right->const_isnull;
                }
            }

            if (left_null || right_null)
            {
                *is_null = true;
                return false;
            }

            return eval_operator(expr->operator, left_val, right_val,
                                 left_type, right_type);
        }

        case JSON_EXPR_AND:
        {
            bool result = true;
            foreach(lc, expr->children)
            {
                bool child_null;
                bool child_result = json_query_eval(lfirst(lc), value,
                                                    &child_null);
                if (child_null)
                {
                    *is_null = true;
                    return false;
                }
                if (!child_result)
                {
                    result = false;
                    break;
                }
            }
            return result;
        }

        case JSON_EXPR_OR:
        {
            bool result = false;
            foreach(lc, expr->children)
            {
                bool child_null;
                bool child_result = json_query_eval(lfirst(lc), value,
                                                    &child_null);
                if (!child_null && child_result)
                {
                    result = true;
                    break;
                }
            }
            return result;
        }

        case JSON_EXPR_NOT:
        {
            if (expr->children == NIL)
            {
                *is_null = true;
                return false;
            }
            bool child_null;
            bool child_result = json_query_eval(linitial(expr->children),
                                                value, &child_null);
            if (child_null)
            {
                *is_null = true;
                return false;
            }
            return !child_result;
        }

        default:
            *is_null = true;
            return false;
    }
}

Datum
json_query_eval_datum(JsonQueryExpr *expr, Jsonb *value,
                      Oid *result_type, bool *is_null)
{
    if (expr == NULL)
    {
        *is_null = true;
        *result_type = UNKNOWNOID;
        return (Datum) 0;
    }

    switch (expr->type)
    {
        case JSON_EXPR_CONST:
            *is_null = expr->const_isnull;
            *result_type = expr->const_type;
            return expr->const_value;

        case JSON_EXPR_PATH:
        {
            Datum path_datum = CStringGetTextDatum(expr->path);
            Datum result;

            PG_TRY();
            {
                result = DirectFunctionCall2(jsonb_object_field_text,
                                             JsonbPGetDatum(value),
                                             path_datum);
                *is_null = (result == (Datum) 0);
                *result_type = TEXTOID;
            }
            PG_CATCH();
            {
                FlushErrorState();
                *is_null = true;
                *result_type = UNKNOWNOID;
                result = (Datum) 0;
            }
            PG_END_TRY();

            return result;
        }

        default:
        {
            bool result = json_query_eval(expr, value, is_null);
            *result_type = BOOLOID;
            return BoolGetDatum(result);
        }
    }
}

void
json_query_eval_batch(JsonQueryExpr *expr, Jsonb **values,
                      int count, bool *results)
{
    for (int i = 0; i < count; i++)
    {
        bool is_null;
        results[i] = json_query_eval(expr, values[i], &is_null);
        if (is_null)
            results[i] = false;
    }
}

/* ============================================================
 * Batch Parsing
 * ============================================================ */

BatchParseContext *
json_query_batch_init(int batch_size)
{
    BatchParseContext *ctx;

    ensure_query_context();

    ctx = MemoryContextAllocZero(JsonQueryContext, sizeof(BatchParseContext));
    ctx->parse_context = AllocSetContextCreate(JsonQueryContext,
                                               "BatchParse",
                                               ALLOCSET_DEFAULT_SIZES);
    ctx->batch_size = batch_size;
    ctx->cache_size = JSON_QUERY_PATH_CACHE_SIZE;
    ctx->path_cache = MemoryContextAllocZero(ctx->parse_context,
                                             ctx->cache_size * sizeof(struct PathCache));
    ctx->cache_count = 0;
    ctx->values_parsed = 0;
    ctx->cache_hits = 0;
    ctx->cache_misses = 0;

    return ctx;
}

void
json_query_batch_parse(BatchParseContext *ctx,
                       Datum *json_datums, int count,
                       const char **paths, int path_count,
                       Datum **results, bool **nulls)
{
    MemoryContext old_context = MemoryContextSwitchTo(ctx->parse_context);

    for (int p = 0; p < path_count; p++)
    {
        results[p] = palloc(count * sizeof(Datum));
        nulls[p] = palloc(count * sizeof(bool));

        for (int i = 0; i < count; i++)
        {
            Jsonb *jb = DatumGetJsonbP(json_datums[i]);
            Datum path_datum = CStringGetTextDatum(paths[p]);

            PG_TRY();
            {
                results[p][i] = DirectFunctionCall2(jsonb_object_field_text,
                                                    JsonbPGetDatum(jb),
                                                    path_datum);
                nulls[p][i] = (results[p][i] == (Datum) 0);
            }
            PG_CATCH();
            {
                FlushErrorState();
                results[p][i] = (Datum) 0;
                nulls[p][i] = true;
            }
            PG_END_TRY();

            ctx->values_parsed++;
        }
    }

    MemoryContextSwitchTo(old_context);
}

void
json_query_batch_extract(BatchParseContext *ctx,
                         Jsonb **values, int count,
                         const char *path, Oid target_type,
                         Datum *results, bool *nulls)
{
    Datum path_datum = CStringGetTextDatum(path);

    for (int i = 0; i < count; i++)
    {
        if (values[i] == NULL)
        {
            results[i] = (Datum) 0;
            nulls[i] = true;
            continue;
        }

        PG_TRY();
        {
            results[i] = DirectFunctionCall2(jsonb_object_field_text,
                                             JsonbPGetDatum(values[i]),
                                             path_datum);
            nulls[i] = (results[i] == (Datum) 0);
        }
        PG_CATCH();
        {
            FlushErrorState();
            results[i] = (Datum) 0;
            nulls[i] = true;
        }
        PG_END_TRY();

        ctx->values_parsed++;
    }
}

void
json_query_batch_free(BatchParseContext *ctx)
{
    if (ctx == NULL)
        return;

    if (ctx->parse_context)
        MemoryContextDelete(ctx->parse_context);

    pfree(ctx);
}

void
json_query_cache_path(BatchParseContext *ctx, const char *path)
{
    if (ctx->cache_count >= ctx->cache_size)
        return;

    ctx->path_cache[ctx->cache_count].path = MemoryContextStrdup(ctx->parse_context,
                                                                  path);
    ctx->path_cache[ctx->cache_count].components =
        json_index_parse_path(path, &ctx->path_cache[ctx->cache_count].depth);
    ctx->cache_count++;
}

/* ============================================================
 * Cost Estimation
 * ============================================================ */

void
json_query_estimate_cost(JsonQueryExpr *expr, Oid table_oid,
                         int16 column_attnum,
                         double *startup_cost, double *total_cost,
                         double *selectivity, int64 *rows)
{
    double      sel;
    int64       table_rows;
    double      seq_page_cost = 1.0;
    double      cpu_tuple_cost = 0.01;
    double      cpu_operator_cost = 0.0025;

    /* Get table row estimate */
    table_rows = 1000;  /* Default estimate */

    {
        Relation rel = table_open(table_oid, AccessShareLock);
        table_rows = Max(rel->rd_rel->reltuples, 1);
        table_close(rel, AccessShareLock);
    }

    /* Get expression selectivity */
    sel = (expr != NULL) ? expr->selectivity : 1.0;

    /* Estimate costs */
    *startup_cost = 0.0;
    *total_cost = seq_page_cost * (table_rows / 100) +  /* I/O cost */
                  cpu_tuple_cost * table_rows +          /* Tuple processing */
                  cpu_operator_cost * table_rows;        /* JSON evaluation */

    if (selectivity != NULL)
        *selectivity = sel;

    if (rows != NULL)
        *rows = (int64)(table_rows * sel);
}

void
json_query_estimate_index_cost(JsonQueryExpr *expr, Oid index_oid,
                               double *startup_cost, double *total_cost)
{
    double      index_selectivity = 0.1;  /* Typical index selectivity */
    double      random_page_cost = 4.0;
    double      cpu_index_tuple_cost = 0.005;

    if (expr != NULL)
        index_selectivity = expr->selectivity;

    *startup_cost = random_page_cost;
    *total_cost = random_page_cost * 10 +
                  cpu_index_tuple_cost * 100 * index_selectivity;
}

/* ============================================================
 * Expression/Plan to String
 * ============================================================ */

char *
json_query_expr_to_string(JsonQueryExpr *expr)
{
    StringInfoData buf;
    ListCell *lc;

    initStringInfo(&buf);

    if (expr == NULL)
    {
        appendStringInfoString(&buf, "(null)");
        return buf.data;
    }

    switch (expr->type)
    {
        case JSON_EXPR_CONST:
            if (expr->const_isnull)
                appendStringInfoString(&buf, "NULL");
            else if (expr->const_type == TEXTOID)
                appendStringInfo(&buf, "'%s'",
                                 TextDatumGetCString(expr->const_value));
            else if (expr->const_type == INT4OID)
                appendStringInfo(&buf, "%d", DatumGetInt32(expr->const_value));
            else
                appendStringInfoString(&buf, "<const>");
            break;

        case JSON_EXPR_PATH:
            appendStringInfo(&buf, "$.%s", expr->path);
            break;

        case JSON_EXPR_COLUMN:
            appendStringInfo(&buf, "%s", expr->column_name);
            break;

        case JSON_EXPR_OPERATOR:
            appendStringInfo(&buf, "(%s %s %s)",
                             json_query_expr_to_string(expr->left),
                             json_query_operator_name(expr->operator),
                             json_query_expr_to_string(expr->right));
            break;

        case JSON_EXPR_AND:
            appendStringInfoString(&buf, "(");
            foreach(lc, expr->children)
            {
                if (lc != list_head(expr->children))
                    appendStringInfoString(&buf, " AND ");
                appendStringInfoString(&buf,
                                       json_query_expr_to_string(lfirst(lc)));
            }
            appendStringInfoString(&buf, ")");
            break;

        case JSON_EXPR_OR:
            appendStringInfoString(&buf, "(");
            foreach(lc, expr->children)
            {
                if (lc != list_head(expr->children))
                    appendStringInfoString(&buf, " OR ");
                appendStringInfoString(&buf,
                                       json_query_expr_to_string(lfirst(lc)));
            }
            appendStringInfoString(&buf, ")");
            break;

        case JSON_EXPR_NOT:
            appendStringInfo(&buf, "NOT %s",
                             json_query_expr_to_string(linitial(expr->children)));
            break;

        case JSON_EXPR_FUNCTION:
            appendStringInfo(&buf, "%s(", expr->func_name);
            foreach(lc, expr->func_args)
            {
                if (lc != list_head(expr->func_args))
                    appendStringInfoString(&buf, ", ");
                appendStringInfoString(&buf,
                                       json_query_expr_to_string(lfirst(lc)));
            }
            appendStringInfoString(&buf, ")");
            break;
    }

    return buf.data;
}

char *
json_query_plan_to_string(JsonQueryPlan *plan)
{
    StringInfoData buf;

    initStringInfo(&buf);

    if (plan == NULL)
    {
        appendStringInfoString(&buf, "(null plan)");
        return buf.data;
    }

    appendStringInfo(&buf, "JSON Query Plan:\n");
    appendStringInfo(&buf, "  Table OID: %u\n", plan->table_oid);
    appendStringInfo(&buf, "  Column: %d\n", plan->column_attnum);
    appendStringInfo(&buf, "  Expression: %s\n",
                     json_query_expr_to_string(plan->root_expr));
    appendStringInfo(&buf, "  Use Index: %s (OID: %u)\n",
                     plan->selected_index != InvalidOid ? "yes" : "no",
                     plan->selected_index);
    appendStringInfo(&buf, "  Use Columnar: %s\n",
                     plan->use_columnar ? "yes" : "no");
    appendStringInfo(&buf, "  Batch Processing: %s\n",
                     plan->use_batch ? "yes" : "no");
    appendStringInfo(&buf, "  Predicate Pushdown: %s\n",
                     plan->push_down ? "yes" : "no");
    appendStringInfo(&buf, "  Estimated Rows: %ld\n", plan->estimated_rows);
    appendStringInfo(&buf, "  Total Cost: %.2f\n", plan->total_cost);

    return buf.data;
}

/* ============================================================
 * Path Expression Optimization
 * ============================================================ */

char *
json_query_simplify_path(const char *path)
{
    return json_index_normalize_path(path);
}

List *
json_query_merge_paths(List *path_exprs)
{
    /* Merge multiple path accesses into a single multi-path extraction */
    /* Simplified - returns input unchanged */
    return path_exprs;
}

List *
json_query_reorder_paths(List *path_exprs, JsonPathStatsCollection *stats)
{
    /* Reorder paths by selectivity for efficient short-circuit evaluation */
    /* Simplified - returns input unchanged */
    return path_exprs;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_json_query);
Datum
orochi_json_query(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    text       *query_text = PG_GETARG_TEXT_PP(2);
    char       *column_name;
    char       *query;
    int16       column_attnum;
    JsonQueryExpr *expr;
    JsonQueryPlan *plan;
    FuncCallContext *funcctx;

    column_name = text_to_cstring(column_name_text);
    query = text_to_cstring(query_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext old_context;
        TupleDesc   tupdesc;
        JsonQueryState *state;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Parse and optimize query */
        expr = json_query_parse(query);
        expr = json_query_optimize(expr, table_oid, column_attnum);
        plan = json_query_plan(expr, table_oid, column_attnum);

        /* Start execution */
        state = json_query_begin(plan, NULL);
        funcctx->user_fctx = state;

        /* Build output tuple descriptor */
        tupdesc = CreateTemplateTupleDesc(1);
        TupleDescInitEntry(tupdesc, 1, "result", JSONBOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();

    {
        JsonQueryState *state = funcctx->user_fctx;
        TupleTableSlot *slot = MakeSingleTupleTableSlot(funcctx->tuple_desc,
                                                        &TTSOpsVirtual);

        if (json_query_next(state, slot))
        {
            HeapTuple   tuple;
            Datum       values[1];
            bool        nulls[1] = {false};

            values[0] = slot_getattr(slot, state->plan->column_attnum, &nulls[0]);
            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

            ExecDropSingleTupleTableSlot(slot);
            SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
        }

        ExecDropSingleTupleTableSlot(slot);
        json_query_end(state);
        SRF_RETURN_DONE(funcctx);
    }
}

PG_FUNCTION_INFO_V1(orochi_json_extract_batch);
Datum
orochi_json_extract_batch(PG_FUNCTION_ARGS)
{
    ArrayType  *json_array = PG_GETARG_ARRAYTYPE_P(0);
    text       *path_text = PG_GETARG_TEXT_PP(1);
    char       *path;
    Datum      *json_datums;
    bool       *json_nulls;
    int         count;
    ArrayType  *result_array;
    Datum      *results;
    bool       *result_nulls;
    BatchParseContext *ctx;

    path = text_to_cstring(path_text);

    /* Deconstruct input array */
    deconstruct_array(json_array, JSONBOID, -1, false, TYPALIGN_INT,
                      &json_datums, &json_nulls, &count);

    /* Allocate result arrays */
    results = palloc(count * sizeof(Datum));
    result_nulls = palloc(count * sizeof(bool));

    /* Batch extract */
    ctx = json_query_batch_init(count);

    for (int i = 0; i < count; i++)
    {
        if (json_nulls[i])
        {
            results[i] = (Datum) 0;
            result_nulls[i] = true;
            continue;
        }

        Jsonb *jb = DatumGetJsonbP(json_datums[i]);
        Datum path_datum = CStringGetTextDatum(path);

        PG_TRY();
        {
            results[i] = DirectFunctionCall2(jsonb_object_field_text,
                                             JsonbPGetDatum(jb),
                                             path_datum);
            result_nulls[i] = (results[i] == (Datum) 0);
        }
        PG_CATCH();
        {
            FlushErrorState();
            results[i] = (Datum) 0;
            result_nulls[i] = true;
        }
        PG_END_TRY();
    }

    json_query_batch_free(ctx);

    /* Build result array */
    result_array = construct_array(results, count, TEXTOID, -1, false, TYPALIGN_INT);

    pfree(results);
    pfree(result_nulls);

    PG_RETURN_ARRAYTYPE_P(result_array);
}

PG_FUNCTION_INFO_V1(orochi_json_query_explain);
Datum
orochi_json_query_explain(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    text       *query_text = PG_GETARG_TEXT_PP(2);
    char       *column_name;
    char       *query;
    int16       column_attnum;
    JsonQueryExpr *expr;
    JsonQueryPlan *plan;
    char       *explain_text;

    column_name = text_to_cstring(column_name_text);
    query = text_to_cstring(query_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    /* Parse and plan query */
    expr = json_query_parse(query);
    expr = json_query_optimize(expr, table_oid, column_attnum);
    plan = json_query_plan(expr, table_oid, column_attnum);

    /* Generate explain output */
    explain_text = json_query_plan_to_string(plan);

    json_query_free_plan(plan);
    json_query_free_expr(expr);

    PG_RETURN_TEXT_P(cstring_to_text(explain_text));
}

PG_FUNCTION_INFO_V1(orochi_json_query_stats);
Datum
orochi_json_query_stats(PG_FUNCTION_ARGS)
{
    Oid         table_oid = PG_GETARG_OID(0);
    text       *column_name_text = PG_GETARG_TEXT_PP(1);
    char       *column_name;
    int16       column_attnum;
    JsonPathStatsCollection *stats;
    TupleDesc   tupdesc;
    Datum       values[4];
    bool        nulls[4] = {false};
    HeapTuple   tuple;

    column_name = text_to_cstring(column_name_text);
    column_attnum = get_attnum(table_oid, column_name);

    if (column_attnum == InvalidAttrNumber)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" does not exist", column_name)));

    stats = json_index_analyze_paths(table_oid, column_attnum,
                                     JSON_INDEX_SAMPLE_SIZE);

    tupdesc = CreateTemplateTupleDesc(4);
    TupleDescInitEntry(tupdesc, 1, "total_rows", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "paths_found", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "indexed_paths", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "last_analyzed", TIMESTAMPTZOID, -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    int indexed_count = 0;
    for (int i = 0; i < stats->path_count; i++)
    {
        if (stats->paths[i].is_indexed)
            indexed_count++;
    }

    values[0] = Int64GetDatum(stats->total_rows);
    values[1] = Int32GetDatum(stats->path_count);
    values[2] = Int32GetDatum(indexed_count);
    values[3] = TimestampTzGetDatum(stats->last_analyzed);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    json_index_free_stats(stats);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
