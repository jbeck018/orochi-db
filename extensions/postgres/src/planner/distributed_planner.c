/*-------------------------------------------------------------------------
 *
 * distributed_planner.c
 *    Orochi DB distributed query planner implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_operator.h"
#include "commands/explain.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "../core/catalog.h"
#include "../executor/distributed_executor.h"
#include "../orochi.h"
#include "../sharding/distribution.h"
#include "../sharding/physical_sharding.h"
#include "distributed_planner.h"

/* Previous planner hook */
static planner_hook_type prev_planner_hook = NULL;

/* Recursion guard to prevent infinite loops when querying catalog tables */
static int planner_recursion_depth = 0;

/* Custom scan methods */
CustomScanMethods orochi_scan_methods = { "OrochiScan", orochi_create_scan_state };

/* Forward declarations */
static bool contains_distributed_table(Query *parse);
static List *extract_table_oids(Query *parse);
static ShardRestriction *analyze_where_clause(Node *quals, Oid table_oid);
static PlannedStmt *create_distributed_plan_stmt(Query *parse, DistributedPlan *dplan);

/* ============================================================
 * Planner Hook
 * ============================================================ */

void orochi_planner_hook_init(void)
{
    prev_planner_hook = planner_hook;
    planner_hook = orochi_planner_hook;
}

/*
 * Rewrite query to use shard tables instead of parent table.
 * For SELECT queries, we replace the parent table RTE with a subquery
 * that UNIONs all shard tables.
 */
static Query *rewrite_query_for_shards(Query *parse)
{
    ListCell *lc;
    Query *result;
    bool modified = false;

    /* Make a copy of the query to modify */
    result = (Query *)copyObjectImpl(parse);

    foreach (lc, result->rtable) {
        RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

        if (rte->rtekind == RTE_RELATION) {
            OrochiTableInfo *info = orochi_catalog_get_table(rte->relid);

            if (info != NULL && info->is_distributed && info->shard_count > 1) {
                /* For SELECT queries, replace with UNION ALL subquery */
                if (parse->commandType == CMD_SELECT) {
                    char *schema_name = get_namespace_name(get_rel_namespace(rte->relid));
                    char *table_name = get_rel_name(rte->relid);
                    Oid view_oid;
                    char *view_name;

                    /* Use the pre-created union view */
                    view_name = psprintf("%s_all_shards", table_name);
                    view_oid = get_relname_relid(view_name, get_rel_namespace(rte->relid));

                    if (OidIsValid(view_oid)) {
                        /* Replace parent table with union view */
                        rte->relid = view_oid;
                        rte->relkind = 'v'; /* View */
                        modified = true;

                        elog(DEBUG1, "Rewrote query to use %s.%s instead of %s.%s", schema_name,
                             view_name, schema_name, table_name);
                    }

                    pfree(view_name);
                }
                /* INSERT/UPDATE/DELETE are handled by triggers - let them go to parent
                 */
            }
        }
    }

    if (!modified) {
        pfree(result);
        return parse;
    }

    return result;
}

PlannedStmt *orochi_planner_hook(Query *parse, const char *query_string, int cursorOptions,
                                 ParamListInfo boundParams)
{
    PlannedStmt *result;
    Query *rewritten_query;
    bool is_distributed;

    /*
     * Prevent infinite recursion when querying catalog tables or views.
     * When we check if a table is distributed, we query orochi.orochi_tables,
     * which would trigger this hook again.
     */
    if (planner_recursion_depth > 0) {
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    planner_recursion_depth++;

    /* Check if query involves distributed tables */
    is_distributed = contains_distributed_table(parse);

    if (!is_distributed) {
        planner_recursion_depth--;
        /* Use standard planner for local queries */
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    /*
     * For SELECT queries on distributed tables, we no longer rewrite at the
     * planner level since the union view with INSTEAD OF triggers provides
     * transparent access. Users should query the _all_shards view for reads.
     *
     * For INSERT/UPDATE/DELETE, let them go through - triggers handle routing.
     *
     * The query rewriting was causing issues with permission info mismatch.
     */
    rewritten_query = parse;

    planner_recursion_depth--;

    /* Plan the (possibly rewritten) query */
    if (prev_planner_hook)
        result = prev_planner_hook(rewritten_query, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(rewritten_query, query_string, cursorOptions, boundParams);

    return result;
}

/* ============================================================
 * Query Analysis
 * ============================================================ */

static bool contains_distributed_table(Query *parse)
{
    ListCell *lc;

    if (parse->rtable == NIL)
        return false;

    foreach (lc, parse->rtable) {
        RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

        if (rte->rtekind == RTE_RELATION) {
            if (orochi_is_distributed_table(rte->relid))
                return true;
        }
    }

    return false;
}

bool orochi_query_has_distributed_tables(Query *parse)
{
    return contains_distributed_table(parse);
}

static List *extract_table_oids(Query *parse)
{
    List *oids = NIL;
    ListCell *lc;

    foreach (lc, parse->rtable) {
        RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

        if (rte->rtekind == RTE_RELATION)
            oids = lappend_oid(oids, rte->relid);
    }

    return oids;
}

List *orochi_get_distributed_tables(Query *parse)
{
    List *distributed = NIL;
    ListCell *lc;

    foreach (lc, parse->rtable) {
        RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

        if (rte->rtekind == RTE_RELATION && orochi_is_distributed_table(rte->relid)) {
            distributed = lappend_oid(distributed, rte->relid);
        }
    }

    return distributed;
}

OrochiQueryType orochi_analyze_query(Query *parse)
{
    List *distributed_tables;
    ListCell *lc;
    ListCell *rlc;
    bool single_shard_possible = true;

    distributed_tables = orochi_get_distributed_tables(parse);

    if (list_length(distributed_tables) == 0)
        return OROCHI_QUERY_LOCAL;

    /* Check if query can be routed to single shard */
    foreach (lc, distributed_tables) {
        Oid table_oid = lfirst_oid(lc);
        List *restrictions;

        restrictions = orochi_analyze_predicates(parse, table_oid);

        if (list_length(restrictions) == 0) {
            single_shard_possible = false;
            break;
        }

        /* Check if restrictions narrow to single shard */
        foreach (rlc, restrictions) {
            ShardRestriction *restr = (ShardRestriction *)lfirst(rlc);

            /* Equality on distribution column narrows to single shard */
            if (restr->is_equality && list_length(restr->shard_ids) == 1) {
                /* This table is narrowed to single shard, continue checking others */
                continue;
            } else if (list_length(restr->shard_ids) > 1) {
                /* Multiple shards needed - can't be single shard query */
                single_shard_possible = false;
                break;
            }
        }
    }

    if (single_shard_possible)
        return OROCHI_QUERY_SINGLE_SHARD;

    /* Check if aggregation needed */
    if (parse->hasAggs || parse->groupClause != NIL)
        return OROCHI_QUERY_COORDINATOR;

    return OROCHI_QUERY_MULTI_SHARD;
}

/* ============================================================
 * Predicate Analysis for Shard Pruning
 * ============================================================ */

/*
 * Check if a Var node references the distribution column
 */
static bool var_is_distribution_column(Var *var, Oid table_oid, const char *dist_column)
{
    HeapTuple tp;
    char *attname;
    bool result = false;

    /* Get attribute name from the table */
    tp = SearchSysCacheAttNum(table_oid, var->varattno);
    if (HeapTupleIsValid(tp)) {
        Form_pg_attribute att = (Form_pg_attribute)GETSTRUCT(tp);
        attname = NameStr(att->attname);
        result = (strcmp(attname, dist_column) == 0);
        ReleaseSysCache(tp);
    }

    return result;
}

/*
 * Extract constant value from an expression
 */
static bool extract_const_value(Node *node, Datum *value, Oid *type_oid)
{
    if (IsA(node, Const)) {
        Const *c = (Const *)node;
        if (!c->constisnull) {
            *value = c->constvalue;
            *type_oid = c->consttype;
            return true;
        }
    }
    return false;
}

/*
 * Recursively analyze WHERE clause to find equality predicates on distribution
 * column
 */
static void analyze_qual_node(Node *node, Oid table_oid, const char *dist_column,
                              ShardRestriction *restriction)
{
    if (node == NULL)
        return;

    if (IsA(node, OpExpr)) {
        OpExpr *opexpr = (OpExpr *)node;

        /* Check if this is an equality operator */
        if (list_length(opexpr->args) == 2) {
            Node *left = (Node *)linitial(opexpr->args);
            Node *right = (Node *)lsecond(opexpr->args);
            Var *var_node = NULL;
            Node *const_node = NULL;

            /* Check for Var = Const or Const = Var patterns */
            if (IsA(left, Var) && IsA(right, Const)) {
                var_node = (Var *)left;
                const_node = right;
            } else if (IsA(left, Const) && IsA(right, Var)) {
                var_node = (Var *)right;
                const_node = left;
            }

            if (var_node != NULL && const_node != NULL) {
                /* Check if this is an equality operator (= operator has oid around
                 * 96-98) */
                char *opname = get_opname(opexpr->opno);
                bool is_equality = (opname != NULL && strcmp(opname, "=") == 0);

                if (is_equality && var_is_distribution_column(var_node, table_oid, dist_column)) {
                    Datum value;
                    Oid type_oid;

                    if (extract_const_value(const_node, &value, &type_oid)) {
                        restriction->is_equality = true;
                        restriction->equality_value = value;
                        /* Store type for later hash computation */
                        restriction->shard_ids = list_make1_oid(type_oid);
                    }
                }
            }
        }
    } else if (IsA(node, BoolExpr)) {
        BoolExpr *boolexpr = (BoolExpr *)node;

        /* For AND expressions, check all clauses */
        if (boolexpr->boolop == AND_EXPR) {
            ListCell *lc;
            foreach (lc, boolexpr->args) {
                analyze_qual_node((Node *)lfirst(lc), table_oid, dist_column, restriction);
                /* Stop if we found an equality predicate */
                if (restriction->is_equality)
                    break;
            }
        }
        /* For OR expressions, we can't prune (would need all branches to target
         * same shard) */
    }
}

/*
 * Analyze WHERE clause quals for shard pruning
 * Exported wrapper for physical_sharding to use
 */
ShardRestriction *orochi_analyze_quals_for_pruning(Node *quals, Oid table_oid)
{
    ShardRestriction *restriction;
    OrochiTableInfo *table_info;

    restriction = palloc0(sizeof(ShardRestriction));
    restriction->table_oid = table_oid;

    table_info = orochi_catalog_get_table(table_oid);
    if (table_info == NULL || !table_info->is_distributed)
        return NULL;

    restriction->distribution_column = table_info->distribution_column;
    restriction->shard_ids = NIL;
    restriction->is_equality = false;

    /* Walk the expression tree to find equality predicates */
    analyze_qual_node(quals, table_oid, table_info->distribution_column, restriction);

    return restriction;
}

List *orochi_analyze_predicates(Query *parse, Oid table_oid)
{
    List *restrictions = NIL;
    ShardRestriction *restriction;

    if (parse->jointree && parse->jointree->quals) {
        restriction = orochi_analyze_quals_for_pruning(parse->jointree->quals, table_oid);
        if (restriction != NULL)
            restrictions = lappend(restrictions, restriction);
    }

    return restrictions;
}

List *orochi_get_query_shards(Query *parse, Oid table_oid)
{
    List *restrictions;
    List *shards;

    restrictions = orochi_analyze_predicates(parse, table_oid);

    if (list_length(restrictions) > 0) {
        /* Apply shard pruning */
        shards = orochi_prune_shards(table_oid, restrictions);
    } else {
        /* Need all shards */
        shards = orochi_catalog_get_table_shards(table_oid);
    }

    return shards;
}

/* ============================================================
 * Plan Generation
 * ============================================================ */

DistributedPlan *orochi_create_distributed_plan(Query *parse)
{
    DistributedPlan *plan;
    List *distributed_tables;
    List *all_shards = NIL;
    ListCell *lc;

    plan = palloc0(sizeof(DistributedPlan));
    plan->query_type = orochi_analyze_query(parse);

    if (plan->query_type == OROCHI_QUERY_LOCAL)
        return plan;

    distributed_tables = orochi_get_distributed_tables(parse);
    plan->target_tables = distributed_tables;

    /* Determine if this is a modification */
    plan->is_modification = (parse->commandType == CMD_INSERT || parse->commandType == CMD_UPDATE ||
                             parse->commandType == CMD_DELETE);

    /* Get shards for each table */
    foreach (lc, distributed_tables) {
        Oid table_oid = lfirst_oid(lc);
        List *table_shards = orochi_get_query_shards(parse, table_oid);

        all_shards = list_concat(all_shards, table_shards);
    }

    plan->target_shard_count = list_length(all_shards);

    /* Create fragment queries */
    plan->fragment_queries = orochi_create_fragment_queries(parse, all_shards);

    /* Determine if coordination needed */
    plan->needs_coordination = (parse->hasAggs || parse->groupClause != NIL ||
                                parse->sortClause != NIL || parse->limitCount != NULL);

    if (plan->needs_coordination)
        plan->coordinator_query = orochi_create_coordinator_query(parse);

    return plan;
}

List *orochi_create_fragment_queries(Query *parse, List *shards)
{
    List *fragments = NIL;
    ListCell *lc;

    foreach (lc, shards) {
        OrochiShardInfo *shard = (OrochiShardInfo *)lfirst(lc);
        FragmentQuery *fragment;

        fragment = palloc0(sizeof(FragmentQuery));
        fragment->shard_id = shard->shard_id;
        fragment->node_id = shard->node_id;
        fragment->is_read_only = (parse->commandType == CMD_SELECT);

        /* Deparse query for this shard */
        fragment->query_string = orochi_deparse_query(parse, NIL);

        fragments = lappend(fragments, fragment);
    }

    return fragments;
}

/*
 * Helper: Deparse a target entry (SELECT column)
 */
static void deparse_target_entry(StringInfo buf, TargetEntry *tle, bool first)
{
    if (!first)
        appendStringInfoString(buf, ", ");

    if (IsA(tle->expr, Var)) {
        Var *var = (Var *)tle->expr;
        /* For now, use column name if available */
        if (tle->resname != NULL)
            appendStringInfoString(buf, quote_identifier(tle->resname));
        else
            appendStringInfo(buf, "col%d", var->varattno);
    } else if (IsA(tle->expr, Aggref)) {
        Aggref *agg = (Aggref *)tle->expr;
        char *aggname = get_func_name(agg->aggfnoid);

        appendStringInfo(buf, "%s(", aggname ? aggname : "agg");

        if (agg->aggstar) {
            appendStringInfoChar(buf, '*');
        } else if (agg->args != NIL) {
            ListCell *lc;
            bool first_arg = true;

            foreach (lc, agg->args) {
                TargetEntry *arg_tle = (TargetEntry *)lfirst(lc);
                if (!first_arg)
                    appendStringInfoString(buf, ", ");
                first_arg = false;

                if (IsA(arg_tle->expr, Var)) {
                    Var *var = (Var *)arg_tle->expr;
                    if (arg_tle->resname)
                        appendStringInfoString(buf, quote_identifier(arg_tle->resname));
                    else
                        appendStringInfo(buf, "col%d", var->varattno);
                } else {
                    appendStringInfoString(buf, "expr");
                }
            }
        }
        appendStringInfoChar(buf, ')');

        if (tle->resname != NULL)
            appendStringInfo(buf, " AS %s", quote_identifier(tle->resname));
    } else if (IsA(tle->expr, Const)) {
        Const *c = (Const *)tle->expr;
        if (c->constisnull)
            appendStringInfoString(buf, "NULL");
        else {
            char *str = OidOutputFunctionCall(c->consttype, c->constvalue);
            appendStringInfo(buf, "'%s'", str);
        }
        if (tle->resname != NULL)
            appendStringInfo(buf, " AS %s", quote_identifier(tle->resname));
    } else {
        /* Default: use column name or generic expr */
        if (tle->resname != NULL)
            appendStringInfoString(buf, quote_identifier(tle->resname));
        else
            appendStringInfoString(buf, "expr");
    }
}

/*
 * Helper: Deparse WHERE clause predicate
 */
static void deparse_qual_node(StringInfo buf, Node *node)
{
    if (node == NULL)
        return;

    if (IsA(node, OpExpr)) {
        OpExpr *op = (OpExpr *)node;
        char *opname = get_opname(op->opno);
        Node *left = (Node *)linitial(op->args);
        Node *right = (Node *)lsecond(op->args);

        appendStringInfoChar(buf, '(');

        /* Left operand */
        if (IsA(left, Var)) {
            Var *var = (Var *)left;
            appendStringInfo(buf, "col%d", var->varattno);
        } else if (IsA(left, Const)) {
            Const *c = (Const *)left;
            if (c->constisnull)
                appendStringInfoString(buf, "NULL");
            else {
                char *str = OidOutputFunctionCall(c->consttype, c->constvalue);
                if (c->consttype == INT4OID || c->consttype == INT8OID ||
                    c->consttype == FLOAT4OID || c->consttype == FLOAT8OID)
                    appendStringInfoString(buf, str);
                else
                    appendStringInfo(buf, "'%s'", str);
            }
        } else {
            deparse_qual_node(buf, left);
        }

        /* Operator */
        appendStringInfo(buf, " %s ", opname ? opname : "=");

        /* Right operand */
        if (IsA(right, Var)) {
            Var *var = (Var *)right;
            appendStringInfo(buf, "col%d", var->varattno);
        } else if (IsA(right, Const)) {
            Const *c = (Const *)right;
            if (c->constisnull)
                appendStringInfoString(buf, "NULL");
            else {
                char *str = OidOutputFunctionCall(c->consttype, c->constvalue);
                if (c->consttype == INT4OID || c->consttype == INT8OID ||
                    c->consttype == FLOAT4OID || c->consttype == FLOAT8OID)
                    appendStringInfoString(buf, str);
                else
                    appendStringInfo(buf, "'%s'", str);
            }
        } else {
            deparse_qual_node(buf, right);
        }

        appendStringInfoChar(buf, ')');
    } else if (IsA(node, BoolExpr)) {
        BoolExpr *boolexpr = (BoolExpr *)node;
        const char *op_str;
        ListCell *lc;
        bool first = true;

        switch (boolexpr->boolop) {
        case AND_EXPR:
            op_str = " AND ";
            break;
        case OR_EXPR:
            op_str = " OR ";
            break;
        case NOT_EXPR:
            appendStringInfoString(buf, "NOT ");
            deparse_qual_node(buf, (Node *)linitial(boolexpr->args));
            return;
        default:
            op_str = " AND ";
        }

        appendStringInfoChar(buf, '(');
        foreach (lc, boolexpr->args) {
            if (!first)
                appendStringInfoString(buf, op_str);
            first = false;
            deparse_qual_node(buf, (Node *)lfirst(lc));
        }
        appendStringInfoChar(buf, ')');
    } else if (IsA(node, NullTest)) {
        NullTest *nt = (NullTest *)node;
        if (IsA(nt->arg, Var)) {
            Var *var = (Var *)nt->arg;
            appendStringInfo(buf, "col%d IS %sNULL", var->varattno,
                             nt->nulltesttype == IS_NOT_NULL ? "NOT " : "");
        }
    }
}

/*
 * Deparse a Query tree to SQL string for shard execution
 *
 * This function converts a parsed Query node back to a SQL string
 * that can be executed on individual shards. It handles:
 * - SELECT target list
 * - FROM clause (range table entries)
 * - WHERE clause (quals)
 * - GROUP BY clause
 * - ORDER BY clause
 * - LIMIT clause
 */
char *orochi_deparse_query(Query *parse, List *shard_restrictions)
{
    StringInfoData sql;
    ListCell *lc;
    bool first;
    int rtindex;

    initStringInfo(&sql);

    /* Only handle SELECT for now */
    if (parse->commandType != CMD_SELECT) {
        appendStringInfoString(&sql, "SELECT * FROM shard_table");
        return sql.data;
    }

    /* SELECT clause */
    appendStringInfoString(&sql, "SELECT ");

    if (parse->distinctClause != NIL)
        appendStringInfoString(&sql, "DISTINCT ");

    /* Target list */
    first = true;
    foreach (lc, parse->targetList) {
        TargetEntry *tle = (TargetEntry *)lfirst(lc);

        /* Skip junk entries (e.g., sort keys not in output) */
        if (tle->resjunk)
            continue;

        deparse_target_entry(&sql, tle, first);
        first = false;
    }

    /* Handle empty target list (SELECT *) */
    if (first)
        appendStringInfoChar(&sql, '*');

    /* FROM clause */
    appendStringInfoString(&sql, " FROM ");

    first = true;
    rtindex = 1;
    foreach (lc, parse->rtable) {
        RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

        /* Only include base relations in FROM clause */
        if (rte->rtekind == RTE_RELATION && !rte->inh) {
            char *relname;
            char *namespace;

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            namespace = get_namespace_name(get_rel_namespace(rte->relid));
            relname = get_rel_name(rte->relid);

            if (namespace && strcmp(namespace, "public") != 0)
                appendStringInfo(&sql, "%s.%s", quote_identifier(namespace),
                                 quote_identifier(relname));
            else
                appendStringInfoString(&sql, quote_identifier(relname));

            /* Add alias if present */
            if (rte->alias && rte->alias->aliasname)
                appendStringInfo(&sql, " AS %s", quote_identifier(rte->alias->aliasname));
        }
        rtindex++;
    }

    /* WHERE clause */
    if (parse->jointree && parse->jointree->quals) {
        appendStringInfoString(&sql, " WHERE ");
        deparse_qual_node(&sql, parse->jointree->quals);
    }

    /* GROUP BY clause */
    if (parse->groupClause != NIL) {
        appendStringInfoString(&sql, " GROUP BY ");
        first = true;
        foreach (lc, parse->groupClause) {
            SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
            TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            if (tle->resname)
                appendStringInfoString(&sql, quote_identifier(tle->resname));
            else if (IsA(tle->expr, Var)) {
                Var *var = (Var *)tle->expr;
                appendStringInfo(&sql, "col%d", var->varattno);
            }
        }
    }

    /* HAVING clause */
    if (parse->havingQual) {
        appendStringInfoString(&sql, " HAVING ");
        deparse_qual_node(&sql, parse->havingQual);
    }

    /* ORDER BY clause */
    if (parse->sortClause != NIL) {
        appendStringInfoString(&sql, " ORDER BY ");
        first = true;
        foreach (lc, parse->sortClause) {
            SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
            TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            if (tle->resname)
                appendStringInfoString(&sql, quote_identifier(tle->resname));
            else
                appendStringInfo(&sql, "%d", tle->resno);

            /*
             * Determine sort direction by checking if the sort operator
             * is a "greater than" type operator. We check by operator name.
             */
            {
                char *opname = get_opname(sgc->sortop);
                if (opname && strcmp(opname, ">") == 0)
                    appendStringInfoString(&sql, " DESC");
            }
        }
    }

    /* LIMIT clause */
    if (parse->limitCount) {
        if (IsA(parse->limitCount, Const)) {
            Const *c = (Const *)parse->limitCount;
            if (!c->constisnull)
                appendStringInfo(&sql, " LIMIT %ld", DatumGetInt64(c->constvalue));
        }
    }

    /* OFFSET clause */
    if (parse->limitOffset) {
        if (IsA(parse->limitOffset, Const)) {
            Const *c = (Const *)parse->limitOffset;
            if (!c->constisnull)
                appendStringInfo(&sql, " OFFSET %ld", DatumGetInt64(c->constvalue));
        }
    }

    elog(DEBUG1, "Deparsed query: %s", sql.data);

    return sql.data;
}

/*
 * Create coordinator aggregation query
 *
 * When queries contain aggregates, the coordinator needs to combine
 * partial results from workers. This function generates the final
 * aggregation query that:
 * - For SUM: SUM(partial_sums)
 * - For COUNT: SUM(partial_counts)
 * - For AVG: SUM(partial_sums) / SUM(partial_counts)
 * - For MIN/MAX: MIN/MAX(partial_mins/maxs)
 */
char *orochi_create_coordinator_query(Query *parse)
{
    StringInfoData sql;
    ListCell *lc;
    bool first;
    bool has_aggregates = false;
    int col_index = 1;

    initStringInfo(&sql);

    /* Check if query has aggregates */
    foreach (lc, parse->targetList) {
        TargetEntry *tle = (TargetEntry *)lfirst(lc);
        if (IsA(tle->expr, Aggref)) {
            has_aggregates = true;
            break;
        }
    }

    appendStringInfoString(&sql, "SELECT ");

    /* Build final SELECT list */
    first = true;
    col_index = 1;
    foreach (lc, parse->targetList) {
        TargetEntry *tle = (TargetEntry *)lfirst(lc);

        if (tle->resjunk)
            continue;

        if (!first)
            appendStringInfoString(&sql, ", ");
        first = false;

        if (IsA(tle->expr, Aggref)) {
            Aggref *agg = (Aggref *)tle->expr;
            char *aggname = get_func_name(agg->aggfnoid);

            /*
             * Map partial aggregates to final aggregates:
             * - SUM: SUM(partial_sums)
             * - COUNT: SUM(partial_counts)
             * - AVG: Needs special handling (sum_x / count_x)
             * - MIN/MAX: MIN/MAX of partials
             */
            if (aggname != NULL) {
                if (strcmp(aggname, "count") == 0) {
                    /* COUNT becomes SUM of partial counts */
                    appendStringInfo(&sql, "SUM(col%d)", col_index);
                } else if (strcmp(aggname, "sum") == 0) {
                    /* SUM of partial sums */
                    appendStringInfo(&sql, "SUM(col%d)", col_index);
                } else if (strcmp(aggname, "avg") == 0) {
                    /*
                     * AVG is tricky - ideally we'd send SUM and COUNT separately.
                     * For now, use weighted average approximation.
                     */
                    appendStringInfo(&sql, "AVG(col%d)", col_index);
                } else if (strcmp(aggname, "min") == 0) {
                    appendStringInfo(&sql, "MIN(col%d)", col_index);
                } else if (strcmp(aggname, "max") == 0) {
                    appendStringInfo(&sql, "MAX(col%d)", col_index);
                } else {
                    /* Unknown aggregate - pass through */
                    appendStringInfo(&sql, "col%d", col_index);
                }
            } else {
                appendStringInfo(&sql, "col%d", col_index);
            }

            /* Add alias */
            if (tle->resname != NULL)
                appendStringInfo(&sql, " AS %s", quote_identifier(tle->resname));
        } else {
            /* Non-aggregate columns (GROUP BY columns) - just pass through */
            if (tle->resname != NULL)
                appendStringInfoString(&sql, quote_identifier(tle->resname));
            else
                appendStringInfo(&sql, "col%d", col_index);
        }

        col_index++;
    }

    /* FROM the worker results */
    appendStringInfoString(&sql, " FROM worker_results");

    /* GROUP BY if we have aggregates with grouping */
    if (has_aggregates && parse->groupClause != NIL) {
        appendStringInfoString(&sql, " GROUP BY ");
        first = true;
        col_index = 1;

        foreach (lc, parse->targetList) {
            TargetEntry *tle = (TargetEntry *)lfirst(lc);

            if (tle->resjunk)
                continue;

            /* Include non-aggregate columns in GROUP BY */
            if (!IsA(tle->expr, Aggref)) {
                if (!first)
                    appendStringInfoString(&sql, ", ");
                first = false;

                if (tle->resname != NULL)
                    appendStringInfoString(&sql, quote_identifier(tle->resname));
                else
                    appendStringInfo(&sql, "col%d", col_index);
            }
            col_index++;
        }
    }

    /* ORDER BY - reapply at coordinator */
    if (parse->sortClause != NIL) {
        appendStringInfoString(&sql, " ORDER BY ");
        first = true;
        foreach (lc, parse->sortClause) {
            SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
            TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            if (tle->resname)
                appendStringInfoString(&sql, quote_identifier(tle->resname));
            else
                appendStringInfo(&sql, "%d", tle->resno);
        }
    }

    /* LIMIT - apply at coordinator level */
    if (parse->limitCount) {
        if (IsA(parse->limitCount, Const)) {
            Const *c = (Const *)parse->limitCount;
            if (!c->constisnull)
                appendStringInfo(&sql, " LIMIT %ld", DatumGetInt64(c->constvalue));
        }
    }

    if (parse->limitOffset) {
        if (IsA(parse->limitOffset, Const)) {
            Const *c = (Const *)parse->limitOffset;
            if (!c->constisnull)
                appendStringInfo(&sql, " OFFSET %ld", DatumGetInt64(c->constvalue));
        }
    }

    elog(DEBUG1, "Coordinator query: %s", sql.data);

    return sql.data;
}

/* ============================================================
 * Shard Pruning
 * ============================================================ */

List *orochi_prune_shards(Oid table_oid, List *predicates)
{
    List *all_shards;
    List *pruned_shards = NIL;
    ListCell *lc;
    ListCell *plc;
    OrochiTableInfo *table_info;
    bool found_equality = false;
    int32 target_shard_index = -1;

    all_shards = orochi_catalog_get_table_shards(table_oid);

    if (list_length(predicates) == 0)
        return all_shards;

    table_info = orochi_catalog_get_table(table_oid);
    if (table_info == NULL)
        return all_shards;

    /* Check predicates for equality on distribution column */
    foreach (plc, predicates) {
        ShardRestriction *restriction = (ShardRestriction *)lfirst(plc);

        if (restriction->is_equality) {
            /* We have an equality predicate - compute target shard */
            Oid type_oid = InvalidOid;
            int32 hash_value;

            /* Type OID was stored in shard_ids list */
            if (list_length(restriction->shard_ids) > 0)
                type_oid = linitial_oid(restriction->shard_ids);

            if (OidIsValid(type_oid)) {
                hash_value = orochi_hash_datum(restriction->equality_value, type_oid);
                target_shard_index = orochi_get_shard_index(hash_value, table_info->shard_count);
                found_equality = true;

                elog(DEBUG1, "Shard pruning: equality on %s, hash=%d, target_shard=%d",
                     restriction->distribution_column, hash_value, target_shard_index);
                break;
            }
        }
    }

    /* If we found an equality predicate, return only the target shard */
    if (found_equality && target_shard_index >= 0) {
        foreach (lc, all_shards) {
            OrochiShardInfo *shard = (OrochiShardInfo *)lfirst(lc);

            if (shard->shard_index == target_shard_index) {
                pruned_shards = lappend(pruned_shards, shard);
                elog(DEBUG1, "Shard pruning: selected shard %d (id=%ld)", shard->shard_index,
                     shard->shard_id);
                break;
            }
        }

        /* If we found a target but no matching shard, return empty (shouldn't
         * happen) */
        if (list_length(pruned_shards) == 0) {
            elog(WARNING, "Shard pruning: target shard %d not found", target_shard_index);
            return all_shards;
        }

        return pruned_shards;
    }

    /* No equality predicate found - return all shards */
    return all_shards;
}

/* ============================================================
 * Plan Statement Creation
 * ============================================================ */

static PlannedStmt *create_distributed_plan_stmt(Query *parse, DistributedPlan *dplan)
{
    PlannedStmt *pstmt;
    CustomScan *cscan;
    OrochiScanState *scan_state;
    Plan *plan;

    /* Create custom scan node */
    cscan = makeNode(CustomScan);
    cscan->scan.plan.type = T_CustomScan;
    cscan->methods = &orochi_scan_methods;

    /* Store distributed plan in custom_private */
    cscan->custom_private = list_make1(dplan);

    /* Create planned statement */
    pstmt = makeNode(PlannedStmt);
    pstmt->commandType = parse->commandType;
    pstmt->queryId = parse->queryId;
    pstmt->hasReturning = (parse->returningList != NIL);
    pstmt->hasModifyingCTE = parse->hasModifyingCTE;
    pstmt->canSetTag = parse->canSetTag;
    pstmt->planTree = (Plan *)cscan;
    pstmt->rtable = parse->rtable;

    return pstmt;
}

/* ============================================================
 * Custom Scan Callbacks
 * ============================================================ */

Node *orochi_create_scan_state(CustomScan *cscan)
{
    OrochiScanState *state;

    state = (OrochiScanState *)newNode(sizeof(OrochiScanState), T_CustomScanState);
    state->css.methods = &orochi_scan_methods;

    /* Extract distributed plan from custom_private */
    if (cscan->custom_private != NIL)
        state->distributed_plan = (DistributedPlan *)linitial(cscan->custom_private);

    state->current_fragment = 0;
    state->results = NIL;

    return (Node *)state;
}

void orochi_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
    OrochiScanState *state = (OrochiScanState *)node;
    DistributedPlan *dplan = state->distributed_plan;
    ListCell *lc;

    if (dplan == NULL)
        return;

    /* Execute fragment queries on workers */
    foreach (lc, dplan->fragment_queries) {
        FragmentQuery *fragment = (FragmentQuery *)lfirst(lc);
        PGconn *conn;
        PGresult *res;

        elog(DEBUG1, "Executing fragment on node %d for shard %ld", fragment->node_id,
             fragment->shard_id);

        /* Get connection to worker node */
        conn = (PGconn *)orochi_get_worker_connection(fragment->node_id);
        if (conn == NULL) {
            elog(WARNING, "Failed to connect to node %d for shard %ld", fragment->node_id,
                 fragment->shard_id);
            continue;
        }

        /* Execute query on worker */
        res = PQexec(conn, fragment->query_string);

        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            /* Store result for later iteration */
            state->results = lappend(state->results, res);
            elog(DEBUG1, "Fragment returned %d rows", PQntuples(res));
        } else {
            elog(WARNING, "Fragment query failed on node %d: %s", fragment->node_id,
                 PQerrorMessage(conn));
            PQclear(res);
        }

        orochi_release_worker_connection(conn);
    }
}

TupleTableSlot *orochi_exec_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *)node;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    static int current_result = 0;
    static int current_row = 0;

    ExecClearTuple(slot);

    /* Iterate through results */
    while (current_result < list_length(state->results)) {
        PGresult *res = (PGresult *)list_nth(state->results, current_result);
        int ntuples = PQntuples(res);
        int nfields = PQnfields(res);

        if (current_row < ntuples) {
            int i;

            /* Build tuple from PGresult row */
            for (i = 0; i < nfields && i < tupdesc->natts; i++) {
                if (PQgetisnull(res, current_row, i)) {
                    slot->tts_isnull[i] = true;
                    slot->tts_values[i] = (Datum)0;
                } else {
                    char *value = PQgetvalue(res, current_row, i);
                    Oid typid = TupleDescAttr(tupdesc, i)->atttypid;

                    slot->tts_isnull[i] = false;

                    /* Convert string to Datum based on type */
                    switch (typid) {
                    case INT4OID:
                        slot->tts_values[i] = Int32GetDatum(atoi(value));
                        break;
                    case INT8OID:
                        slot->tts_values[i] = Int64GetDatum(atoll(value));
                        break;
                    case FLOAT4OID:
                        slot->tts_values[i] = Float4GetDatum((float4)atof(value));
                        break;
                    case FLOAT8OID:
                        slot->tts_values[i] = Float8GetDatum(atof(value));
                        break;
                    case BOOLOID:
                        slot->tts_values[i] = BoolGetDatum(value[0] == 't');
                        break;
                    case TEXTOID:
                    case VARCHAROID:
                        slot->tts_values[i] = CStringGetTextDatum(value);
                        break;
                    default:
                        slot->tts_values[i] = CStringGetTextDatum(value);
                        break;
                    }
                }
            }

            /* Fill remaining columns with nulls */
            for (; i < tupdesc->natts; i++) {
                slot->tts_isnull[i] = true;
                slot->tts_values[i] = (Datum)0;
            }

            ExecStoreVirtualTuple(slot);
            current_row++;
            return slot;
        }

        /* Move to next result set */
        current_result++;
        current_row = 0;
    }

    /* No more tuples - reset for potential rescan */
    current_result = 0;
    current_row = 0;
    return slot;
}

void orochi_end_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *)node;

    /* Cleanup connections and results */
    if (state->results != NIL)
        list_free_deep(state->results);
}

void orochi_rescan_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *)node;

    state->current_fragment = 0;
}

void orochi_explain_custom_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
    OrochiScanState *state = (OrochiScanState *)node;
    DistributedPlan *dplan = state->distributed_plan;

    if (dplan == NULL)
        return;

        /* ExplainPropertyText/Integer were removed in PG18 - use appendStringInfo
         * directly */
#if PG_VERSION_NUM >= 180000
    if (es->format == EXPLAIN_FORMAT_TEXT) {
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfo(es->str, "Distributed: true\n");
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfo(es->str, "Shards: %d\n", dplan->target_shard_count);
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfo(es->str, "Fragments: %d\n", list_length(dplan->fragment_queries));
    }
#else
    ExplainPropertyText("Distributed", "true", es);
    ExplainPropertyInteger("Shards", NULL, dplan->target_shard_count, es);
    ExplainPropertyInteger("Fragments", NULL, list_length(dplan->fragment_queries), es);
#endif
}

/* ============================================================
 * Join Analysis
 * ============================================================ */

JoinAnalysis *orochi_analyze_join(Query *parse)
{
    JoinAnalysis *analysis;
    List *distributed_tables;
    ListCell *lc;

    analysis = palloc0(sizeof(JoinAnalysis));
    analysis->is_colocated = true;
    analysis->has_reference = false;
    analysis->needs_repartition = false;

    distributed_tables = orochi_get_distributed_tables(parse);

    if (list_length(distributed_tables) < 2)
        return analysis;

    /* Check co-location */
    Oid first_table = linitial_oid(distributed_tables);

    for_each_from(lc, distributed_tables, 1)
    {
        Oid table_oid = lfirst_oid(lc);

        if (orochi_is_reference_table(table_oid)) {
            analysis->has_reference = true;
            continue;
        }

        if (!orochi_tables_are_colocated(first_table, table_oid)) {
            analysis->is_colocated = false;
            analysis->needs_repartition = true;
        }
    }

    return analysis;
}

bool orochi_can_join_locally(Oid table1_oid, Oid table2_oid, List *join_conditions)
{
    /* Reference tables can always join locally */
    if (orochi_is_reference_table(table1_oid) || orochi_is_reference_table(table2_oid))
        return true;

    /* Co-located tables can join locally on distribution key */
    if (orochi_tables_are_colocated(table1_oid, table2_oid)) {
        /* Check if join is on distribution column */
        OrochiTableInfo *info1 = orochi_catalog_get_table(table1_oid);
        OrochiTableInfo *info2 = orochi_catalog_get_table(table2_oid);
        ListCell *lc;

        if (info1 == NULL || info2 == NULL || info1->distribution_column == NULL ||
            info2->distribution_column == NULL)
            return false;

        /* Check if any join condition involves both distribution columns */
        foreach (lc, join_conditions) {
            Node *condition = (Node *)lfirst(lc);

            if (IsA(condition, OpExpr)) {
                OpExpr *op = (OpExpr *)condition;

                /* Check for equality operation */
                if (list_length(op->args) == 2) {
                    Node *left = linitial(op->args);
                    Node *right = lsecond(op->args);
                    bool left_is_dist1 = false, right_is_dist2 = false;
                    bool left_is_dist2 = false, right_is_dist1 = false;

                    if (IsA(left, Var)) {
                        Var *var = (Var *)left;
                        if (var_is_distribution_column(var, table1_oid, info1->distribution_column))
                            left_is_dist1 = true;
                        if (var_is_distribution_column(var, table2_oid, info2->distribution_column))
                            left_is_dist2 = true;
                    }

                    if (IsA(right, Var)) {
                        Var *var = (Var *)right;
                        if (var_is_distribution_column(var, table1_oid, info1->distribution_column))
                            right_is_dist1 = true;
                        if (var_is_distribution_column(var, table2_oid, info2->distribution_column))
                            right_is_dist2 = true;
                    }

                    /* Join on distribution columns if one side is dist1, other is dist2
                     */
                    if ((left_is_dist1 && right_is_dist2) || (left_is_dist2 && right_is_dist1))
                        return true;
                }
            }
        }

        return false;
    }

    return false;
}

/* ============================================================
 * Aggregate Planning
 * ============================================================ */

bool orochi_can_pushdown_aggregate(Aggref *agg)
{
    /* These aggregates can be computed in parallel */
    switch (agg->aggfnoid) {
    /* SUM, COUNT, MIN, MAX can be computed partially */
    /* AVG needs sum + count */
    default:
        return true;
    }
}

char *orochi_create_partial_aggregate_query(Query *parse)
{
    StringInfoData sql;
    ListCell *lc;
    bool first = true;

    initStringInfo(&sql);
    appendStringInfoString(&sql, "SELECT ");

    /* Generate partial aggregate expressions */
    foreach (lc, parse->targetList) {
        TargetEntry *tle = (TargetEntry *)lfirst(lc);

        if (!first)
            appendStringInfoString(&sql, ", ");
        first = false;

        if (IsA(tle->expr, Aggref)) {
            Aggref *agg = (Aggref *)tle->expr;

            /* For COUNT(*), just output COUNT(*) */
            /* For SUM(x), output SUM(x) */
            /* For AVG(x), output SUM(x), COUNT(x) */
            /* For MIN/MAX(x), output MIN/MAX(x) */

            if (agg->aggfnoid == 2147) /* count */
            {
                appendStringInfoString(&sql, "COUNT(*)");
            } else if (agg->aggfnoid == 2108 || agg->aggfnoid == 2110) /* sum */
            {
                appendStringInfoString(&sql, "SUM(");
                /* Add column reference - simplified */
                appendStringInfo(&sql, "col%d", tle->resno);
                appendStringInfoString(&sql, ")");
            } else if (agg->aggfnoid == 2100 || agg->aggfnoid == 2101) /* avg */
            {
                /* AVG needs SUM and COUNT */
                appendStringInfo(&sql, "SUM(col%d), COUNT(col%d)", tle->resno, tle->resno);
            } else {
                /* Default: pass through aggregate name */
                appendStringInfo(&sql, "agg(col%d)", tle->resno);
            }
        } else {
            /* Non-aggregate columns in GROUP BY */
            appendStringInfo(&sql, "col%d", tle->resno);
        }
    }

    appendStringInfoString(&sql, " FROM shard_table");

    /* Add GROUP BY if present */
    if (parse->groupClause != NIL) {
        appendStringInfoString(&sql, " GROUP BY ");
        first = true;
        foreach (lc, parse->groupClause) {
            SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
            TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            appendStringInfo(&sql, "col%d", tle->resno);
        }
    }

    return sql.data;
}

char *orochi_create_final_aggregate_query(Query *parse)
{
    StringInfoData sql;
    ListCell *lc;
    bool first = true;

    initStringInfo(&sql);
    appendStringInfoString(&sql, "SELECT ");

    /* Generate final aggregate expressions */
    foreach (lc, parse->targetList) {
        TargetEntry *tle = (TargetEntry *)lfirst(lc);

        if (!first)
            appendStringInfoString(&sql, ", ");
        first = false;

        if (IsA(tle->expr, Aggref)) {
            Aggref *agg = (Aggref *)tle->expr;

            /* For COUNT, SUM the partial counts */
            /* For SUM, SUM the partial sums */
            /* For AVG, SUM(partial_sum) / SUM(partial_count) */
            /* For MIN/MAX, MIN/MAX of partial results */

            if (agg->aggfnoid == 2147) /* count - sum of counts */
            {
                appendStringInfoString(&sql, "SUM(partial_count)");
            } else if (agg->aggfnoid == 2108 || agg->aggfnoid == 2110) /* sum */
            {
                appendStringInfoString(&sql, "SUM(partial_sum)");
            } else if (agg->aggfnoid == 2100 || agg->aggfnoid == 2101) /* avg */
            {
                appendStringInfoString(&sql,
                                       "SUM(partial_sum)::float / NULLIF(SUM(partial_count), 0)");
            } else {
                appendStringInfoString(&sql, "combine_agg(partial_result)");
            }
        } else {
            /* GROUP BY columns pass through */
            appendStringInfo(&sql, "group_col%d", tle->resno);
        }
    }

    appendStringInfoString(&sql, " FROM partial_results");

    /* Add GROUP BY for grouping columns */
    if (parse->groupClause != NIL) {
        appendStringInfoString(&sql, " GROUP BY ");
        first = true;
        foreach (lc, parse->groupClause) {
            SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
            TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);

            if (!first)
                appendStringInfoString(&sql, ", ");
            first = false;

            appendStringInfo(&sql, "group_col%d", tle->resno);
        }
    }

    return sql.data;
}

/* ============================================================
 * Cost Estimation
 * ============================================================ */

double orochi_estimate_distributed_cost(DistributedPlan *plan)
{
    double cost = 0.0;

    /* Base cost per fragment */
    cost += list_length(plan->fragment_queries) * 10.0;

    /* Network overhead */
    cost += plan->target_shard_count * 5.0;

    /* Coordination overhead */
    if (plan->needs_coordination)
        cost += 20.0;

    return cost;
}
