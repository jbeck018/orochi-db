/*-------------------------------------------------------------------------
 *
 * distributed_planner.c
 *    Orochi DB distributed query planner implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "../sharding/distribution.h"
#include "distributed_planner.h"

/* Previous planner hook */
static planner_hook_type prev_planner_hook = NULL;

/* Custom scan methods */
CustomScanMethods orochi_scan_methods = {
    "OrochiScan",
    orochi_create_scan_state
};

/* Forward declarations */
static bool contains_distributed_table(Query *parse);
static List *extract_table_oids(Query *parse);
static ShardRestriction *analyze_where_clause(Node *quals, Oid table_oid);
static PlannedStmt *create_distributed_plan_stmt(Query *parse, DistributedPlan *dplan);

/* ============================================================
 * Planner Hook
 * ============================================================ */

void
orochi_planner_hook_init(void)
{
    prev_planner_hook = planner_hook;
    planner_hook = orochi_planner_hook;
}

PlannedStmt *
orochi_planner_hook(Query *parse, const char *query_string, int cursorOptions,
                    ParamListInfo boundParams)
{
    PlannedStmt *result;
    DistributedPlan *dplan;

    /* Check if query involves distributed tables */
    if (!contains_distributed_table(parse))
    {
        /* Use standard planner for local queries */
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    /* Create distributed plan */
    dplan = orochi_create_distributed_plan(parse);

    if (dplan->query_type == OROCHI_QUERY_LOCAL)
    {
        /* Query can be executed locally */
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    /* Create distributed execution plan */
    result = create_distributed_plan_stmt(parse, dplan);

    return result;
}

/* ============================================================
 * Query Analysis
 * ============================================================ */

static bool
contains_distributed_table(Query *parse)
{
    ListCell *lc;

    if (parse->rtable == NIL)
        return false;

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

        if (rte->rtekind == RTE_RELATION)
        {
            if (orochi_is_distributed_table(rte->relid))
                return true;
        }
    }

    return false;
}

bool
orochi_query_has_distributed_tables(Query *parse)
{
    return contains_distributed_table(parse);
}

static List *
extract_table_oids(Query *parse)
{
    List *oids = NIL;
    ListCell *lc;

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

        if (rte->rtekind == RTE_RELATION)
            oids = lappend_oid(oids, rte->relid);
    }

    return oids;
}

List *
orochi_get_distributed_tables(Query *parse)
{
    List *distributed = NIL;
    ListCell *lc;

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

        if (rte->rtekind == RTE_RELATION &&
            orochi_is_distributed_table(rte->relid))
        {
            distributed = lappend_oid(distributed, rte->relid);
        }
    }

    return distributed;
}

OrochiQueryType
orochi_analyze_query(Query *parse)
{
    List *distributed_tables;
    ListCell *lc;
    bool single_shard_possible = true;

    distributed_tables = orochi_get_distributed_tables(parse);

    if (list_length(distributed_tables) == 0)
        return OROCHI_QUERY_LOCAL;

    /* Check if query can be routed to single shard */
    foreach(lc, distributed_tables)
    {
        Oid table_oid = lfirst_oid(lc);
        List *restrictions;

        restrictions = orochi_analyze_predicates(parse, table_oid);

        if (list_length(restrictions) == 0)
        {
            single_shard_possible = false;
            break;
        }

        /* Check if restrictions narrow to single shard */
        /* TODO: Implement proper shard narrowing check */
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

static ShardRestriction *
analyze_where_clause(Node *quals, Oid table_oid)
{
    ShardRestriction *restriction;
    OrochiTableInfo *table_info;

    restriction = palloc0(sizeof(ShardRestriction));
    restriction->table_oid = table_oid;

    table_info = orochi_catalog_get_table(table_oid);
    if (table_info == NULL || !table_info->is_distributed)
        return NULL;

    restriction->distribution_column = table_info->distribution_column;

    /* TODO: Walk expression tree to find equality predicates on distribution column */
    restriction->shard_ids = NIL;
    restriction->is_equality = false;

    return restriction;
}

List *
orochi_analyze_predicates(Query *parse, Oid table_oid)
{
    List *restrictions = NIL;
    ShardRestriction *restriction;

    if (parse->jointree && parse->jointree->quals)
    {
        restriction = analyze_where_clause(parse->jointree->quals, table_oid);
        if (restriction != NULL)
            restrictions = lappend(restrictions, restriction);
    }

    return restrictions;
}

List *
orochi_get_query_shards(Query *parse, Oid table_oid)
{
    List *restrictions;
    List *shards;

    restrictions = orochi_analyze_predicates(parse, table_oid);

    if (list_length(restrictions) > 0)
    {
        /* Apply shard pruning */
        shards = orochi_prune_shards(table_oid, restrictions);
    }
    else
    {
        /* Need all shards */
        shards = orochi_catalog_get_table_shards(table_oid);
    }

    return shards;
}

/* ============================================================
 * Plan Generation
 * ============================================================ */

DistributedPlan *
orochi_create_distributed_plan(Query *parse)
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
    plan->is_modification = (parse->commandType == CMD_INSERT ||
                             parse->commandType == CMD_UPDATE ||
                             parse->commandType == CMD_DELETE);

    /* Get shards for each table */
    foreach(lc, distributed_tables)
    {
        Oid table_oid = lfirst_oid(lc);
        List *table_shards = orochi_get_query_shards(parse, table_oid);

        all_shards = list_concat(all_shards, table_shards);
    }

    plan->target_shard_count = list_length(all_shards);

    /* Create fragment queries */
    plan->fragment_queries = orochi_create_fragment_queries(parse, all_shards);

    /* Determine if coordination needed */
    plan->needs_coordination = (parse->hasAggs ||
                                parse->groupClause != NIL ||
                                parse->sortClause != NIL ||
                                parse->limitCount != NULL);

    if (plan->needs_coordination)
        plan->coordinator_query = orochi_create_coordinator_query(parse);

    return plan;
}

List *
orochi_create_fragment_queries(Query *parse, List *shards)
{
    List *fragments = NIL;
    ListCell *lc;

    foreach(lc, shards)
    {
        OrochiShardInfo *shard = (OrochiShardInfo *) lfirst(lc);
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

char *
orochi_deparse_query(Query *parse, List *shard_restrictions)
{
    StringInfoData sql;

    initStringInfo(&sql);

    /* TODO: Implement proper query deparsing */
    /* For now, return a placeholder */
    appendStringInfoString(&sql, "SELECT * FROM shard_table");

    return sql.data;
}

char *
orochi_create_coordinator_query(Query *parse)
{
    StringInfoData sql;

    initStringInfo(&sql);

    /* TODO: Create final aggregation query */
    appendStringInfoString(&sql, "SELECT * FROM worker_results");

    return sql.data;
}

/* ============================================================
 * Shard Pruning
 * ============================================================ */

List *
orochi_prune_shards(Oid table_oid, List *predicates)
{
    List *all_shards;
    List *pruned_shards = NIL;
    ListCell *lc;

    all_shards = orochi_catalog_get_table_shards(table_oid);

    if (list_length(predicates) == 0)
        return all_shards;

    /* TODO: Implement actual pruning based on predicates */
    /* For now, return all shards */
    foreach(lc, all_shards)
    {
        OrochiShardInfo *shard = (OrochiShardInfo *) lfirst(lc);
        pruned_shards = lappend(pruned_shards, shard);
    }

    return pruned_shards;
}

/* ============================================================
 * Plan Statement Creation
 * ============================================================ */

static PlannedStmt *
create_distributed_plan_stmt(Query *parse, DistributedPlan *dplan)
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
    pstmt->planTree = (Plan *) cscan;
    pstmt->rtable = parse->rtable;

    return pstmt;
}

/* ============================================================
 * Custom Scan Callbacks
 * ============================================================ */

Node *
orochi_create_scan_state(CustomScan *cscan)
{
    OrochiScanState *state;

    state = (OrochiScanState *) newNode(sizeof(OrochiScanState), T_CustomScanState);
    state->css.methods = &orochi_scan_methods;

    /* Extract distributed plan from custom_private */
    if (cscan->custom_private != NIL)
        state->distributed_plan = (DistributedPlan *) linitial(cscan->custom_private);

    state->current_fragment = 0;
    state->results = NIL;

    return (Node *) state;
}

void
orochi_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
    OrochiScanState *state = (OrochiScanState *) node;
    DistributedPlan *dplan = state->distributed_plan;
    ListCell *lc;

    if (dplan == NULL)
        return;

    /* Execute fragment queries on workers */
    foreach(lc, dplan->fragment_queries)
    {
        FragmentQuery *fragment = (FragmentQuery *) lfirst(lc);

        /* TODO: Send query to worker node and collect results */
        elog(DEBUG1, "Executing fragment on node %d for shard %ld",
             fragment->node_id, fragment->shard_id);
    }
}

TupleTableSlot *
orochi_exec_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *) node;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    /* TODO: Return next tuple from results */
    ExecClearTuple(slot);

    return slot;
}

void
orochi_end_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *) node;

    /* Cleanup connections and results */
    if (state->results != NIL)
        list_free_deep(state->results);
}

void
orochi_rescan_custom_scan(CustomScanState *node)
{
    OrochiScanState *state = (OrochiScanState *) node;

    state->current_fragment = 0;
}

void
orochi_explain_custom_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
    OrochiScanState *state = (OrochiScanState *) node;
    DistributedPlan *dplan = state->distributed_plan;

    if (dplan == NULL)
        return;

    ExplainPropertyText("Distributed", "true", es);
    ExplainPropertyInteger("Shards", NULL, dplan->target_shard_count, es);
    ExplainPropertyInteger("Fragments", NULL,
                           list_length(dplan->fragment_queries), es);
}

/* ============================================================
 * Join Analysis
 * ============================================================ */

JoinAnalysis *
orochi_analyze_join(Query *parse)
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

        if (orochi_is_reference_table(table_oid))
        {
            analysis->has_reference = true;
            continue;
        }

        if (!orochi_tables_are_colocated(first_table, table_oid))
        {
            analysis->is_colocated = false;
            analysis->needs_repartition = true;
        }
    }

    return analysis;
}

bool
orochi_can_join_locally(Oid table1_oid, Oid table2_oid, List *join_conditions)
{
    /* Reference tables can always join locally */
    if (orochi_is_reference_table(table1_oid) ||
        orochi_is_reference_table(table2_oid))
        return true;

    /* Co-located tables can join locally on distribution key */
    if (orochi_tables_are_colocated(table1_oid, table2_oid))
    {
        /* TODO: Check if join is on distribution column */
        return true;
    }

    return false;
}

/* ============================================================
 * Aggregate Planning
 * ============================================================ */

bool
orochi_can_pushdown_aggregate(Aggref *agg)
{
    /* These aggregates can be computed in parallel */
    switch (agg->aggfnoid)
    {
        /* SUM, COUNT, MIN, MAX can be computed partially */
        /* AVG needs sum + count */
        default:
            return true;
    }
}

char *
orochi_create_partial_aggregate_query(Query *parse)
{
    StringInfoData sql;

    initStringInfo(&sql);

    /* TODO: Create partial aggregation query */
    appendStringInfoString(&sql, "SELECT partial_agg(*) FROM shard");

    return sql.data;
}

char *
orochi_create_final_aggregate_query(Query *parse)
{
    StringInfoData sql;

    initStringInfo(&sql);

    /* TODO: Create final aggregation query */
    appendStringInfoString(&sql, "SELECT final_agg(*) FROM partials");

    return sql.data;
}

/* ============================================================
 * Cost Estimation
 * ============================================================ */

double
orochi_estimate_distributed_cost(DistributedPlan *plan)
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
