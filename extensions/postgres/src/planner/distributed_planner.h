/*-------------------------------------------------------------------------
 *
 * distributed_planner.h
 *    Orochi DB distributed query planner
 *
 * The planner intercepts PostgreSQL's planning process to:
 *   - Identify queries on distributed tables
 *   - Create distributed execution plans
 *   - Apply shard pruning based on predicates
 *   - Generate fragment queries for workers
 *   - Handle co-located joins efficiently
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_DISTRIBUTED_PLANNER_H
#define OROCHI_DISTRIBUTED_PLANNER_H

#include "postgres.h"
#include "nodes/plannodes.h"
#include "nodes/pathnodes.h"
#include "nodes/extensible.h"
#include "optimizer/planner.h"
#include "commands/explain.h"

#include "../orochi.h"

/* ============================================================
 * Query Types
 * ============================================================ */

typedef enum OrochiQueryType
{
    OROCHI_QUERY_LOCAL = 0,      /* Non-distributed query */
    OROCHI_QUERY_SINGLE_SHARD,   /* Routes to single shard */
    OROCHI_QUERY_MULTI_SHARD,    /* Spans multiple shards */
    OROCHI_QUERY_COORDINATOR     /* Requires coordinator aggregation */
} OrochiQueryType;

/* ============================================================
 * Distributed Plan Structures
 * ============================================================ */

/*
 * Fragment query - sent to worker nodes
 */
typedef struct FragmentQuery
{
    int64               shard_id;           /* Target shard */
    int32               node_id;            /* Target node */
    char               *query_string;       /* SQL to execute */
    List               *param_values;       /* Parameter values */
    bool                is_read_only;       /* For routing decisions */
} FragmentQuery;

/*
 * Distributed plan - overall execution strategy
 */
typedef struct DistributedPlan
{
    OrochiQueryType     query_type;         /* Type of distributed query */
    List               *fragment_queries;   /* List of FragmentQuery */
    int32               target_shard_count; /* Shards involved */
    bool                needs_coordination; /* Requires final aggregation */
    bool                is_modification;    /* INSERT/UPDATE/DELETE */
    List               *target_tables;      /* Tables involved */
    char               *coordinator_query;  /* Final aggregation query */
} DistributedPlan;

/*
 * Shard restriction - predicate-based shard filtering
 */
typedef struct ShardRestriction
{
    Oid                 table_oid;          /* Distributed table */
    char               *distribution_column; /* Distribution key */
    List               *shard_ids;          /* Matching shards */
    bool                is_equality;        /* Equality predicate? */
    Datum               equality_value;     /* If equality, the value */
} ShardRestriction;

/*
 * Join analysis result
 */
typedef struct JoinAnalysis
{
    bool                is_colocated;       /* Tables are co-located */
    bool                has_reference;      /* Involves reference table */
    bool                needs_repartition;  /* Requires data movement */
    List               *join_keys;          /* Join key columns */
} JoinAnalysis;

/* ============================================================
 * Custom Scan Node
 * ============================================================ */

/*
 * OrochiScan - custom scan for distributed execution
 */
typedef struct OrochiScanState
{
    CustomScanState     css;
    DistributedPlan    *distributed_plan;
    int                 current_fragment;
    List               *results;
    void               *connection_state;
} OrochiScanState;

/* ============================================================
 * Planner Hook Functions
 * ============================================================ */

/*
 * Main planner hook
 */
extern PlannedStmt *orochi_planner_hook(Query *parse, const char *query_string,
                                        int cursorOptions,
                                        ParamListInfo boundParams);

/*
 * Initialize planner hooks
 */
extern void orochi_planner_hook_init(void);

/* ============================================================
 * Query Analysis Functions
 * ============================================================ */

/*
 * Analyze query for distribution
 */
extern OrochiQueryType orochi_analyze_query(Query *parse);

/*
 * Check if query involves distributed tables
 */
extern bool orochi_query_has_distributed_tables(Query *parse);

/*
 * Get distributed tables in query
 */
extern List *orochi_get_distributed_tables(Query *parse);

/*
 * Analyze predicates for shard pruning
 */
extern List *orochi_analyze_predicates(Query *parse, Oid table_oid);

/*
 * Analyze quals (WHERE clause) for shard pruning
 * Takes raw Node* quals instead of Query*
 */
extern ShardRestriction *orochi_analyze_quals_for_pruning(Node *quals, Oid table_oid);

/*
 * Determine shards for query
 */
extern List *orochi_get_query_shards(Query *parse, Oid table_oid);

/* ============================================================
 * Plan Generation Functions
 * ============================================================ */

/*
 * Create distributed plan
 */
extern DistributedPlan *orochi_create_distributed_plan(Query *parse);

/*
 * Create fragment queries
 */
extern List *orochi_create_fragment_queries(Query *parse, List *shards);

/*
 * Deparse query for worker
 */
extern char *orochi_deparse_query(Query *parse, List *shard_restrictions);

/*
 * Create coordinator query
 */
extern char *orochi_create_coordinator_query(Query *parse);

/* ============================================================
 * Join Planning Functions
 * ============================================================ */

/*
 * Analyze join for co-location
 */
extern JoinAnalysis *orochi_analyze_join(Query *parse);

/*
 * Check if tables can be joined locally
 */
extern bool orochi_can_join_locally(Oid table1_oid, Oid table2_oid,
                                    List *join_conditions);

/*
 * Plan repartition join if needed
 */
extern DistributedPlan *orochi_plan_repartition_join(Query *parse);

/* ============================================================
 * Aggregate Planning Functions
 * ============================================================ */

/*
 * Check if aggregate can be pushed down
 */
extern bool orochi_can_pushdown_aggregate(Aggref *agg);

/*
 * Create partial aggregate plan
 */
extern char *orochi_create_partial_aggregate_query(Query *parse);

/*
 * Create final aggregate plan
 */
extern char *orochi_create_final_aggregate_query(Query *parse);

/* ============================================================
 * Optimization Functions
 * ============================================================ */

/*
 * Apply shard pruning
 */
extern List *orochi_prune_shards(Oid table_oid, List *predicates);

/*
 * Estimate distributed query cost
 */
extern double orochi_estimate_distributed_cost(DistributedPlan *plan);

/*
 * Optimize fragment order
 */
extern void orochi_optimize_fragment_order(DistributedPlan *plan);

/* ============================================================
 * Custom Scan Methods
 * ============================================================ */

extern CustomScanMethods orochi_scan_methods;

extern Node *orochi_create_scan_state(CustomScan *cscan);
extern void orochi_begin_custom_scan(CustomScanState *node, EState *estate, int eflags);
extern TupleTableSlot *orochi_exec_custom_scan(CustomScanState *node);
extern void orochi_end_custom_scan(CustomScanState *node);
extern void orochi_rescan_custom_scan(CustomScanState *node);
extern void orochi_explain_custom_scan(CustomScanState *node, List *ancestors,
                                       ExplainState *es);

#endif /* OROCHI_DISTRIBUTED_PLANNER_H */
