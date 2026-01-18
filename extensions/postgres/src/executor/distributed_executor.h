/*-------------------------------------------------------------------------
 *
 * distributed_executor.h
 *    Orochi DB distributed query executor
 *
 * The executor handles:
 *   - Sending fragment queries to worker nodes
 *   - Collecting and merging results
 *   - Handling failures and retries
 *   - Adaptive execution (slow start)
 *   - Transaction coordination
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_DISTRIBUTED_EXECUTOR_H
#define OROCHI_DISTRIBUTED_EXECUTOR_H

#include "postgres.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"

#include "../orochi.h"
#include "../planner/distributed_planner.h"

/* ============================================================
 * Executor Configuration
 * ============================================================ */

#define EXECUTOR_DEFAULT_TIMEOUT_MS     30000
#define EXECUTOR_MAX_RETRIES            3
#define EXECUTOR_SLOW_START_INTERVAL_MS 10
#define EXECUTOR_MAX_CONNECTIONS        64

/* ============================================================
 * Task States
 * ============================================================ */

typedef enum TaskState
{
    TASK_PENDING = 0,
    TASK_RUNNING,
    TASK_COMPLETED,
    TASK_FAILED,
    TASK_CANCELLED
} TaskState;

/* ============================================================
 * Executor Structures
 * ============================================================ */

/*
 * Remote task - execution of single fragment
 */
typedef struct RemoteTask
{
    int64               task_id;
    FragmentQuery      *fragment;
    TaskState           state;
    int                 retry_count;
    void               *connection;         /* PGconn */
    TimestampTz         started_at;
    TimestampTz         completed_at;
    void               *result;             /* PGresult */
    char               *error_message;
    int64               rows_affected;
} RemoteTask;

/*
 * Executor state for distributed query
 */
typedef struct DistributedExecutorState
{
    DistributedPlan    *plan;
    List               *tasks;              /* List of RemoteTask */
    int                 pending_count;
    int                 running_count;
    int                 completed_count;
    int                 failed_count;
    int                 max_parallel;
    bool                all_completed;
    bool                has_error;
    char               *error_message;
    MemoryContext       exec_context;
} DistributedExecutorState;

/*
 * Result merger state
 */
typedef struct ResultMergerState
{
    List               *completed_tasks;
    int                 current_task;
    int                 current_row;
    bool                needs_sorting;
    List               *sort_keys;
    TupleDesc           result_tupdesc;
} ResultMergerState;

/* Aggregate types for pushdown */
#define AGG_TYPE_COUNT  1
#define AGG_TYPE_SUM    2
#define AGG_TYPE_MIN    3
#define AGG_TYPE_MAX    4
#define AGG_TYPE_AVG    5

/*
 * Partial aggregation result from worker
 */
typedef struct PartialAggResult
{
    int64               shard_id;
    int64               count;
    int64               sum;
    int64               min_val;
    int64               max_val;
} PartialAggResult;

/*
 * Aggregate pushdown plan
 */
typedef struct AggPushdownPlan
{
    int                 agg_type;           /* AGG_TYPE_* */
    List               *worker_queries;     /* Partial aggregate queries */
    Datum               combined_result;    /* Combined final result */
} AggPushdownPlan;

/*
 * Streaming result state for large results
 */
typedef struct StreamingResultState
{
    DistributedExecutorState *executor_state;
    int                 current_shard;
    int64               rows_processed;
    int                 batch_size;
    bool                is_complete;
} StreamingResultState;

/* ============================================================
 * Executor Hook Functions
 * ============================================================ */

/*
 * Initialize executor hooks
 */
extern void orochi_executor_hook_init(void);

/*
 * Executor start hook
 */
extern void orochi_executor_start_hook(QueryDesc *queryDesc, int eflags);

/*
 * Executor run hook
 */
#if PG_VERSION_NUM >= 180000
extern void orochi_executor_run_hook(QueryDesc *queryDesc,
                                     ScanDirection direction,
                                     uint64 count);
#else
extern void orochi_executor_run_hook(QueryDesc *queryDesc,
                                     ScanDirection direction,
                                     uint64 count, bool execute_once);
#endif

/*
 * Executor finish hook
 */
extern void orochi_executor_finish_hook(QueryDesc *queryDesc);

/*
 * Executor end hook
 */
extern void orochi_executor_end_hook(QueryDesc *queryDesc);

/* ============================================================
 * Execution Functions
 * ============================================================ */

/*
 * Execute distributed plan
 */
extern void orochi_execute_distributed_plan(DistributedPlan *plan,
                                            DistributedExecutorState *state);

/*
 * Execute single remote task
 */
extern void orochi_execute_remote_task(RemoteTask *task);

/*
 * Wait for task completion
 */
extern bool orochi_wait_for_task(RemoteTask *task, int timeout_ms);

/*
 * Cancel running task
 */
extern void orochi_cancel_task(RemoteTask *task);

/*
 * Retry failed task
 */
extern bool orochi_retry_task(RemoteTask *task);

/* ============================================================
 * Adaptive Execution
 * ============================================================ */

/*
 * Start tasks with slow-start algorithm
 */
extern void orochi_adaptive_execute(DistributedExecutorState *state);

/*
 * Adjust parallelism based on performance
 */
extern void orochi_adjust_parallelism(DistributedExecutorState *state);

/* ============================================================
 * Result Handling
 * ============================================================ */

/*
 * Initialize result merger
 */
extern ResultMergerState *orochi_init_result_merger(DistributedExecutorState *state);

/*
 * Get next tuple from merged results
 */
extern bool orochi_get_next_result(ResultMergerState *merger, TupleTableSlot *slot);

/*
 * Free result merger
 */
extern void orochi_free_result_merger(ResultMergerState *merger);

/* ============================================================
 * Transaction Coordination
 * ============================================================ */

/*
 * Begin distributed transaction
 */
extern void orochi_begin_distributed_transaction(void);

/*
 * Prepare distributed transaction (2PC phase 1)
 */
extern bool orochi_prepare_distributed_transaction(DistributedExecutorState *state);

/*
 * Commit distributed transaction (2PC phase 2)
 */
extern void orochi_commit_distributed_transaction(DistributedExecutorState *state);

/*
 * Rollback distributed transaction
 */
extern void orochi_rollback_distributed_transaction(DistributedExecutorState *state);

/* ============================================================
 * Connection Management
 * ============================================================ */

/*
 * Get connection to worker node
 */
extern void *orochi_get_worker_connection(int32 node_id);

/*
 * Release connection back to pool
 */
extern void orochi_release_worker_connection(void *conn);

/*
 * Close all worker connections
 */
extern void orochi_close_all_connections(void);

/* ============================================================
 * Error Handling
 * ============================================================ */

/*
 * Handle task failure
 */
extern void orochi_handle_task_failure(RemoteTask *task,
                                       DistributedExecutorState *state);

/*
 * Check if error is retryable
 */
extern bool orochi_is_retryable_error(const char *error_message);

/*
 * Propagate error to client
 */
extern void orochi_propagate_error(DistributedExecutorState *state);

/* ============================================================
 * Statistics and Monitoring
 * ============================================================ */

/*
 * Record execution statistics
 */
extern void orochi_record_execution_stats(DistributedExecutorState *state);

/*
 * Get execution summary
 */
extern char *orochi_get_execution_summary(DistributedExecutorState *state);

/* ============================================================
 * Parallel Execution
 * ============================================================ */

/*
 * Execute with parallel shard access
 */
extern void orochi_parallel_execute(DistributedExecutorState *state, int parallelism);

/*
 * Start async task
 */
extern void orochi_start_async_task(RemoteTask *task);

/*
 * Check task completion
 */
extern bool orochi_check_task_completion(RemoteTask *task);

/* ============================================================
 * Aggregate Pushdown
 * ============================================================ */

/*
 * Execute aggregate with pushdown
 */
extern void orochi_execute_aggregate_pushdown(DistributedExecutorState *state,
                                              AggPushdownPlan *agg_plan);

/*
 * Combine partial aggregates
 */
extern Datum orochi_combine_partial_aggregates(List *partials, int agg_type);

/* ============================================================
 * Query Result Caching
 * ============================================================ */

/*
 * Initialize query cache
 */
extern void orochi_init_query_cache(void);

/*
 * Store result in cache
 */
extern void orochi_cache_store(const char *query_string, const char *result_data,
                               int64 result_size);

/*
 * Invalidate cache for table
 */
extern void orochi_cache_invalidate(Oid table_oid);

/* ============================================================
 * Streaming Results
 * ============================================================ */

/*
 * Initialize streaming result processor
 */
extern StreamingResultState *orochi_init_streaming_result(DistributedExecutorState *state);

/*
 * Get next batch of rows
 */
extern List *orochi_get_next_batch(StreamingResultState *stream);

/*
 * Cleanup streaming state
 */
extern void orochi_cleanup_streaming_result(StreamingResultState *stream);

#endif /* OROCHI_DISTRIBUTED_EXECUTOR_H */
