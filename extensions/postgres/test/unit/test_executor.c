/*-------------------------------------------------------------------------
 *
 * test_executor.c
 *    Unit tests for Orochi DB distributed query executor
 *
 * Tests query routing, result aggregation, and transaction coordination.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants and Types
 * ============================================================ */

#define MAX_TASKS           100
#define MAX_RESULTS         1000
#define MAX_SHARDS          50
#define MAX_NODES           10
#define DEFAULT_TIMEOUT_MS  30000
#define MAX_RETRIES         3

/* Task states */
typedef enum MockTaskState
{
    MOCK_TASK_PENDING = 0,
    MOCK_TASK_RUNNING,
    MOCK_TASK_COMPLETED,
    MOCK_TASK_FAILED,
    MOCK_TASK_CANCELLED
} MockTaskState;

/* Aggregate types */
typedef enum MockAggType
{
    MOCK_AGG_COUNT = 1,
    MOCK_AGG_SUM,
    MOCK_AGG_MIN,
    MOCK_AGG_MAX,
    MOCK_AGG_AVG
} MockAggType;

/* Fragment query */
typedef struct MockFragmentQuery
{
    int64               fragment_id;
    int64               shard_id;
    int32               node_id;
    char                query_text[256];
    bool                in_use;
} MockFragmentQuery;

/* Remote task */
typedef struct MockRemoteTask
{
    int64               task_id;
    MockFragmentQuery  *fragment;
    MockTaskState       state;
    int                 retry_count;
    int64               started_at;
    int64               completed_at;
    int64               rows_affected;
    char                error_message[256];
    int64              *result_values;
    int                 result_count;
    bool                in_use;
} MockRemoteTask;

/* Partial aggregate result */
typedef struct MockPartialAggResult
{
    int64               shard_id;
    int64               count;
    int64               sum;
    int64               min_val;
    int64               max_val;
} MockPartialAggResult;

/* Executor state */
typedef struct MockExecutorState
{
    MockRemoteTask     *tasks[MAX_TASKS];
    int                 task_count;
    int                 pending_count;
    int                 running_count;
    int                 completed_count;
    int                 failed_count;
    int                 max_parallel;
    bool                all_completed;
    bool                has_error;
    char                error_message[256];
    bool                active;
} MockExecutorState;

/* Result merger state */
typedef struct MockMergerState
{
    MockRemoteTask    **completed_tasks;
    int                 task_count;
    int                 current_task;
    int                 current_row;
    bool                needs_sorting;
    int                 total_rows;
} MockMergerState;

/* Streaming result state */
typedef struct MockStreamingState
{
    MockExecutorState  *executor;
    int                 current_shard;
    int64               rows_processed;
    int                 batch_size;
    bool                is_complete;
} MockStreamingState;

/* Transaction state */
typedef enum MockTxnState
{
    MOCK_TXN_NONE = 0,
    MOCK_TXN_STARTED,
    MOCK_TXN_PREPARED,
    MOCK_TXN_COMMITTED,
    MOCK_TXN_ABORTED
} MockTxnState;

typedef struct MockDistributedTxn
{
    int64               txn_id;
    MockTxnState        state;
    int                 participant_count;
    bool                prepared[MAX_NODES];
    bool                committed[MAX_NODES];
} MockDistributedTxn;

/* Query cache entry */
typedef struct MockCacheEntry
{
    char                query_string[256];
    int64              *result_data;
    int                 result_count;
    int64               created_at;
    bool                in_use;
} MockCacheEntry;

/* Global state */
static MockFragmentQuery mock_fragments[MAX_TASKS];
static MockRemoteTask mock_tasks[MAX_TASKS];
static MockCacheEntry mock_cache[100];
static MockDistributedTxn mock_txn = {0};
static int64 next_task_id = 1;
static int64 next_fragment_id = 1;
static int64 next_txn_id = 1;
static int64 mock_time = 1000000;

/* ============================================================
 * Mock Executor Functions
 * ============================================================ */

static void mock_executor_init(void)
{
    memset(mock_fragments, 0, sizeof(mock_fragments));
    memset(mock_tasks, 0, sizeof(mock_tasks));
    memset(mock_cache, 0, sizeof(mock_cache));
    memset(&mock_txn, 0, sizeof(mock_txn));
    next_task_id = 1;
    next_fragment_id = 1;
    next_txn_id = 1;
    mock_time = 1000000;
}

static void mock_executor_cleanup(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (mock_tasks[i].result_values)
        {
            free(mock_tasks[i].result_values);
            mock_tasks[i].result_values = NULL;
        }
    }
    for (int i = 0; i < 100; i++)
    {
        if (mock_cache[i].result_data)
        {
            free(mock_cache[i].result_data);
            mock_cache[i].result_data = NULL;
        }
    }
    memset(mock_fragments, 0, sizeof(mock_fragments));
    memset(mock_tasks, 0, sizeof(mock_tasks));
}

/* Fragment operations */
static MockFragmentQuery *mock_create_fragment(int64 shard_id, int32 node_id,
                                                const char *query)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (!mock_fragments[i].in_use)
        {
            mock_fragments[i].fragment_id = next_fragment_id++;
            mock_fragments[i].shard_id = shard_id;
            mock_fragments[i].node_id = node_id;
            strncpy(mock_fragments[i].query_text, query, 255);
            mock_fragments[i].in_use = true;
            return &mock_fragments[i];
        }
    }
    return NULL;
}

/* Task operations */
static MockRemoteTask *mock_create_task(MockFragmentQuery *fragment)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (!mock_tasks[i].in_use)
        {
            mock_tasks[i].task_id = next_task_id++;
            mock_tasks[i].fragment = fragment;
            mock_tasks[i].state = MOCK_TASK_PENDING;
            mock_tasks[i].retry_count = 0;
            mock_tasks[i].rows_affected = 0;
            mock_tasks[i].result_values = NULL;
            mock_tasks[i].result_count = 0;
            mock_tasks[i].error_message[0] = '\0';
            mock_tasks[i].in_use = true;
            return &mock_tasks[i];
        }
    }
    return NULL;
}

static void mock_execute_task(MockRemoteTask *task)
{
    if (!task) return;

    task->state = MOCK_TASK_RUNNING;
    task->started_at = mock_time++;

    /* Simulate execution - generate some results */
    task->result_count = 10;
    task->result_values = calloc(task->result_count, sizeof(int64));
    for (int i = 0; i < task->result_count; i++)
    {
        task->result_values[i] = task->fragment->shard_id * 100 + i;
    }
    task->rows_affected = task->result_count;

    task->state = MOCK_TASK_COMPLETED;
    task->completed_at = mock_time++;
}

static void mock_fail_task(MockRemoteTask *task, const char *error)
{
    if (!task) return;

    task->state = MOCK_TASK_FAILED;
    strncpy(task->error_message, error, 255);
    task->completed_at = mock_time++;
}

static bool mock_retry_task(MockRemoteTask *task)
{
    if (!task || task->retry_count >= MAX_RETRIES)
        return false;

    task->retry_count++;
    task->state = MOCK_TASK_PENDING;
    task->error_message[0] = '\0';
    return true;
}

static void mock_cancel_task(MockRemoteTask *task)
{
    if (!task) return;
    task->state = MOCK_TASK_CANCELLED;
    task->completed_at = mock_time++;
}

static bool mock_wait_for_task(MockRemoteTask *task, int timeout_ms)
{
    if (!task) return false;

    /* Simulated wait - just check state */
    return (task->state == MOCK_TASK_COMPLETED ||
            task->state == MOCK_TASK_FAILED ||
            task->state == MOCK_TASK_CANCELLED);
}

/* Executor state operations */
static MockExecutorState *mock_create_executor_state(int max_parallel)
{
    MockExecutorState *state = calloc(1, sizeof(MockExecutorState));
    state->max_parallel = max_parallel;
    state->active = true;
    return state;
}

static void mock_add_task_to_executor(MockExecutorState *state, MockRemoteTask *task)
{
    if (!state || state->task_count >= MAX_TASKS) return;

    state->tasks[state->task_count++] = task;
    state->pending_count++;
}

static void mock_execute_all_tasks(MockExecutorState *state)
{
    if (!state) return;

    for (int i = 0; i < state->task_count; i++)
    {
        MockRemoteTask *task = state->tasks[i];
        if (task->state == MOCK_TASK_PENDING)
        {
            state->pending_count--;
            state->running_count++;
            mock_execute_task(task);
            state->running_count--;

            if (task->state == MOCK_TASK_COMPLETED)
                state->completed_count++;
            else if (task->state == MOCK_TASK_FAILED)
            {
                state->failed_count++;
                state->has_error = true;
                strncpy(state->error_message, task->error_message, 255);
            }
        }
    }

    state->all_completed = (state->pending_count == 0 &&
                            state->running_count == 0);
}

static void mock_free_executor_state(MockExecutorState *state)
{
    if (state)
    {
        state->active = false;
        free(state);
    }
}

/* Result merger */
static MockMergerState *mock_create_merger(MockExecutorState *state)
{
    if (!state) return NULL;

    MockMergerState *merger = calloc(1, sizeof(MockMergerState));
    merger->completed_tasks = calloc(state->task_count, sizeof(MockRemoteTask *));
    merger->task_count = 0;

    for (int i = 0; i < state->task_count; i++)
    {
        if (state->tasks[i]->state == MOCK_TASK_COMPLETED)
        {
            merger->completed_tasks[merger->task_count++] = state->tasks[i];
            merger->total_rows += state->tasks[i]->result_count;
        }
    }

    return merger;
}

static bool mock_get_next_result(MockMergerState *merger, int64 *value)
{
    if (!merger) return false;

    while (merger->current_task < merger->task_count)
    {
        MockRemoteTask *task = merger->completed_tasks[merger->current_task];

        if (merger->current_row < task->result_count)
        {
            *value = task->result_values[merger->current_row++];
            return true;
        }

        merger->current_task++;
        merger->current_row = 0;
    }

    return false;
}

static void mock_free_merger(MockMergerState *merger)
{
    if (merger)
    {
        free(merger->completed_tasks);
        free(merger);
    }
}

/* Aggregate operations */
static int64 mock_combine_aggregates(MockPartialAggResult *partials, int count,
                                      MockAggType agg_type)
{
    if (count == 0) return 0;

    int64 result = 0;
    int64 total_count = 0;
    int64 total_sum = 0;

    switch (agg_type)
    {
        case MOCK_AGG_COUNT:
            for (int i = 0; i < count; i++)
                result += partials[i].count;
            break;

        case MOCK_AGG_SUM:
            for (int i = 0; i < count; i++)
                result += partials[i].sum;
            break;

        case MOCK_AGG_MIN:
            result = partials[0].min_val;
            for (int i = 1; i < count; i++)
            {
                if (partials[i].min_val < result)
                    result = partials[i].min_val;
            }
            break;

        case MOCK_AGG_MAX:
            result = partials[0].max_val;
            for (int i = 1; i < count; i++)
            {
                if (partials[i].max_val > result)
                    result = partials[i].max_val;
            }
            break;

        case MOCK_AGG_AVG:
            for (int i = 0; i < count; i++)
            {
                total_count += partials[i].count;
                total_sum += partials[i].sum;
            }
            result = (total_count > 0) ? total_sum / total_count : 0;
            break;
    }

    return result;
}

/* Transaction operations */
static void mock_begin_distributed_txn(void)
{
    mock_txn.txn_id = next_txn_id++;
    mock_txn.state = MOCK_TXN_STARTED;
    mock_txn.participant_count = 0;
    memset(mock_txn.prepared, 0, sizeof(mock_txn.prepared));
    memset(mock_txn.committed, 0, sizeof(mock_txn.committed));
}

static void mock_add_txn_participant(int node_id)
{
    if (node_id >= 0 && node_id < MAX_NODES)
    {
        mock_txn.participant_count++;
    }
}

static bool mock_prepare_distributed_txn(void)
{
    if (mock_txn.state != MOCK_TXN_STARTED)
        return false;

    /* Simulate all participants preparing successfully */
    for (int i = 0; i < mock_txn.participant_count; i++)
    {
        mock_txn.prepared[i] = true;
    }

    mock_txn.state = MOCK_TXN_PREPARED;
    return true;
}

static bool mock_commit_distributed_txn(void)
{
    if (mock_txn.state != MOCK_TXN_PREPARED)
        return false;

    for (int i = 0; i < mock_txn.participant_count; i++)
    {
        mock_txn.committed[i] = true;
    }

    mock_txn.state = MOCK_TXN_COMMITTED;
    return true;
}

static void mock_rollback_distributed_txn(void)
{
    mock_txn.state = MOCK_TXN_ABORTED;
}

/* Query cache operations */
static bool mock_cache_store(const char *query, int64 *results, int count)
{
    for (int i = 0; i < 100; i++)
    {
        if (!mock_cache[i].in_use)
        {
            strncpy(mock_cache[i].query_string, query, 255);
            mock_cache[i].result_data = calloc(count, sizeof(int64));
            memcpy(mock_cache[i].result_data, results, count * sizeof(int64));
            mock_cache[i].result_count = count;
            mock_cache[i].created_at = mock_time++;
            mock_cache[i].in_use = true;
            return true;
        }
    }
    return false;
}

static MockCacheEntry *mock_cache_lookup(const char *query)
{
    for (int i = 0; i < 100; i++)
    {
        if (mock_cache[i].in_use &&
            strcmp(mock_cache[i].query_string, query) == 0)
            return &mock_cache[i];
    }
    return NULL;
}

static void mock_cache_invalidate(Oid table_oid)
{
    /* Simplified: invalidate all cache entries */
    for (int i = 0; i < 100; i++)
    {
        if (mock_cache[i].in_use)
        {
            free(mock_cache[i].result_data);
            mock_cache[i].result_data = NULL;
            mock_cache[i].in_use = false;
        }
    }
}

/* Error handling */
static bool mock_is_retryable_error(const char *error)
{
    if (!error) return false;

    /* List of retryable error patterns */
    if (strstr(error, "connection") != NULL) return true;
    if (strstr(error, "timeout") != NULL) return true;
    if (strstr(error, "temporary") != NULL) return true;

    return false;
}

/* Streaming results */
static MockStreamingState *mock_create_streaming(MockExecutorState *executor,
                                                   int batch_size)
{
    MockStreamingState *stream = calloc(1, sizeof(MockStreamingState));
    stream->executor = executor;
    stream->batch_size = batch_size;
    stream->current_shard = 0;
    stream->rows_processed = 0;
    stream->is_complete = false;
    return stream;
}

static int mock_get_next_batch(MockStreamingState *stream, int64 *batch)
{
    if (!stream || stream->is_complete) return 0;

    int count = 0;
    MockMergerState *merger = mock_create_merger(stream->executor);

    int64 value;
    while (count < stream->batch_size && mock_get_next_result(merger, &value))
    {
        batch[count++] = value;
        stream->rows_processed++;
    }

    if (count < stream->batch_size)
        stream->is_complete = true;

    mock_free_merger(merger);
    return count;
}

static void mock_free_streaming(MockStreamingState *stream)
{
    if (stream) free(stream);
}

/* ============================================================
 * Test Cases
 * ============================================================ */

/* Test 1: Fragment creation */
static void test_fragment_creation(void)
{
    TEST_BEGIN("fragment_creation")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT * FROM users");
        TEST_ASSERT_NOT_NULL(frag);
        TEST_ASSERT_EQ(1, frag->shard_id);
        TEST_ASSERT_EQ(1, frag->node_id);
        TEST_ASSERT_STR_EQ("SELECT * FROM users", frag->query_text);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 2: Task creation */
static void test_task_creation(void)
{
    TEST_BEGIN("task_creation")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT id FROM users");
        MockRemoteTask *task = mock_create_task(frag);

        TEST_ASSERT_NOT_NULL(task);
        TEST_ASSERT_EQ(MOCK_TASK_PENDING, task->state);
        TEST_ASSERT_EQ(0, task->retry_count);
        TEST_ASSERT_EQ(frag, task->fragment);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 3: Task execution */
static void test_task_execution(void)
{
    TEST_BEGIN("task_execution")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT count(*) FROM orders");
        MockRemoteTask *task = mock_create_task(frag);

        mock_execute_task(task);

        TEST_ASSERT_EQ(MOCK_TASK_COMPLETED, task->state);
        TEST_ASSERT_GT(task->rows_affected, 0);
        TEST_ASSERT_GT(task->completed_at, task->started_at);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 4: Task failure */
static void test_task_failure(void)
{
    TEST_BEGIN("task_failure")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT * FROM missing_table");
        MockRemoteTask *task = mock_create_task(frag);

        mock_fail_task(task, "relation does not exist");

        TEST_ASSERT_EQ(MOCK_TASK_FAILED, task->state);
        TEST_ASSERT_STR_EQ("relation does not exist", task->error_message);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 5: Task retry */
static void test_task_retry(void)
{
    TEST_BEGIN("task_retry")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT * FROM users");
        MockRemoteTask *task = mock_create_task(frag);

        mock_fail_task(task, "connection timeout");

        bool retry1 = mock_retry_task(task);
        TEST_ASSERT(retry1);
        TEST_ASSERT_EQ(1, task->retry_count);
        TEST_ASSERT_EQ(MOCK_TASK_PENDING, task->state);

        mock_fail_task(task, "connection timeout");
        bool retry2 = mock_retry_task(task);
        TEST_ASSERT(retry2);
        TEST_ASSERT_EQ(2, task->retry_count);

        mock_fail_task(task, "connection timeout");
        bool retry3 = mock_retry_task(task);
        TEST_ASSERT(retry3);
        TEST_ASSERT_EQ(3, task->retry_count);

        /* Should fail after max retries */
        mock_fail_task(task, "connection timeout");
        bool retry4 = mock_retry_task(task);
        TEST_ASSERT_FALSE(retry4);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 6: Task cancellation */
static void test_task_cancellation(void)
{
    TEST_BEGIN("task_cancellation")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT * FROM large_table");
        MockRemoteTask *task = mock_create_task(frag);

        task->state = MOCK_TASK_RUNNING;
        mock_cancel_task(task);

        TEST_ASSERT_EQ(MOCK_TASK_CANCELLED, task->state);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 7: Executor state management */
static void test_executor_state(void)
{
    TEST_BEGIN("executor_state")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(4);
        TEST_ASSERT_NOT_NULL(state);
        TEST_ASSERT_EQ(4, state->max_parallel);
        TEST_ASSERT_EQ(0, state->task_count);
        TEST_ASSERT(state->active);

        /* Add tasks */
        for (int i = 0; i < 3; i++)
        {
            MockFragmentQuery *frag = mock_create_fragment(i + 1, 1, "SELECT 1");
            MockRemoteTask *task = mock_create_task(frag);
            mock_add_task_to_executor(state, task);
        }

        TEST_ASSERT_EQ(3, state->task_count);
        TEST_ASSERT_EQ(3, state->pending_count);

        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 8: Execute all tasks */
static void test_execute_all_tasks(void)
{
    TEST_BEGIN("execute_all_tasks")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(4);

        for (int i = 0; i < 5; i++)
        {
            MockFragmentQuery *frag = mock_create_fragment(i + 1, 1, "SELECT * FROM shard");
            MockRemoteTask *task = mock_create_task(frag);
            mock_add_task_to_executor(state, task);
        }

        mock_execute_all_tasks(state);

        TEST_ASSERT(state->all_completed);
        TEST_ASSERT_EQ(5, state->completed_count);
        TEST_ASSERT_EQ(0, state->failed_count);
        TEST_ASSERT_EQ(0, state->pending_count);

        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 9: Result merger */
static void test_result_merger(void)
{
    TEST_BEGIN("result_merger")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(4);

        /* Create and execute tasks */
        for (int i = 0; i < 3; i++)
        {
            MockFragmentQuery *frag = mock_create_fragment(i + 1, 1, "SELECT id FROM users");
            MockRemoteTask *task = mock_create_task(frag);
            mock_add_task_to_executor(state, task);
        }

        mock_execute_all_tasks(state);

        /* Create merger and read results */
        MockMergerState *merger = mock_create_merger(state);
        TEST_ASSERT_NOT_NULL(merger);
        TEST_ASSERT_EQ(3, merger->task_count);
        TEST_ASSERT_EQ(30, merger->total_rows);  /* 3 tasks * 10 rows each */

        int count = 0;
        int64 value;
        while (mock_get_next_result(merger, &value))
        {
            count++;
        }

        TEST_ASSERT_EQ(30, count);

        mock_free_merger(merger);
        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 10: Aggregate combination - COUNT */
static void test_aggregate_count(void)
{
    TEST_BEGIN("aggregate_count")
        MockPartialAggResult partials[] = {
            {.shard_id = 1, .count = 100},
            {.shard_id = 2, .count = 150},
            {.shard_id = 3, .count = 200}
        };

        int64 result = mock_combine_aggregates(partials, 3, MOCK_AGG_COUNT);
        TEST_ASSERT_EQ(450, result);
    TEST_END();
}

/* Test 11: Aggregate combination - SUM */
static void test_aggregate_sum(void)
{
    TEST_BEGIN("aggregate_sum")
        MockPartialAggResult partials[] = {
            {.shard_id = 1, .sum = 1000},
            {.shard_id = 2, .sum = 2500},
            {.shard_id = 3, .sum = 1500}
        };

        int64 result = mock_combine_aggregates(partials, 3, MOCK_AGG_SUM);
        TEST_ASSERT_EQ(5000, result);
    TEST_END();
}

/* Test 12: Aggregate combination - MIN/MAX */
static void test_aggregate_min_max(void)
{
    TEST_BEGIN("aggregate_min_max")
        MockPartialAggResult partials[] = {
            {.shard_id = 1, .min_val = 10, .max_val = 100},
            {.shard_id = 2, .min_val = 5,  .max_val = 200},
            {.shard_id = 3, .min_val = 15, .max_val = 150}
        };

        int64 min_result = mock_combine_aggregates(partials, 3, MOCK_AGG_MIN);
        int64 max_result = mock_combine_aggregates(partials, 3, MOCK_AGG_MAX);

        TEST_ASSERT_EQ(5, min_result);
        TEST_ASSERT_EQ(200, max_result);
    TEST_END();
}

/* Test 13: Aggregate combination - AVG */
static void test_aggregate_avg(void)
{
    TEST_BEGIN("aggregate_avg")
        MockPartialAggResult partials[] = {
            {.shard_id = 1, .count = 100, .sum = 1000},
            {.shard_id = 2, .count = 100, .sum = 2000},
            {.shard_id = 3, .count = 100, .sum = 3000}
        };

        int64 result = mock_combine_aggregates(partials, 3, MOCK_AGG_AVG);
        TEST_ASSERT_EQ(20, result);  /* (1000+2000+3000) / 300 = 20 */
    TEST_END();
}

/* Test 14: Distributed transaction - success path */
static void test_distributed_txn_success(void)
{
    TEST_BEGIN("distributed_txn_success")
        mock_executor_init();

        mock_begin_distributed_txn();
        TEST_ASSERT_EQ(MOCK_TXN_STARTED, mock_txn.state);

        mock_add_txn_participant(1);
        mock_add_txn_participant(2);
        TEST_ASSERT_EQ(2, mock_txn.participant_count);

        bool prepared = mock_prepare_distributed_txn();
        TEST_ASSERT(prepared);
        TEST_ASSERT_EQ(MOCK_TXN_PREPARED, mock_txn.state);

        bool committed = mock_commit_distributed_txn();
        TEST_ASSERT(committed);
        TEST_ASSERT_EQ(MOCK_TXN_COMMITTED, mock_txn.state);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 15: Distributed transaction - rollback */
static void test_distributed_txn_rollback(void)
{
    TEST_BEGIN("distributed_txn_rollback")
        mock_executor_init();

        mock_begin_distributed_txn();
        mock_add_txn_participant(1);
        mock_add_txn_participant(2);

        mock_prepare_distributed_txn();
        mock_rollback_distributed_txn();

        TEST_ASSERT_EQ(MOCK_TXN_ABORTED, mock_txn.state);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 16: Query cache store and lookup */
static void test_query_cache(void)
{
    TEST_BEGIN("query_cache")
        mock_executor_init();

        const char *query = "SELECT * FROM users WHERE id = 1";
        int64 results[] = {1, 100, 200};

        bool stored = mock_cache_store(query, results, 3);
        TEST_ASSERT(stored);

        MockCacheEntry *entry = mock_cache_lookup(query);
        TEST_ASSERT_NOT_NULL(entry);
        TEST_ASSERT_EQ(3, entry->result_count);
        TEST_ASSERT_EQ(1, entry->result_data[0]);
        TEST_ASSERT_EQ(100, entry->result_data[1]);

        /* Miss for different query */
        MockCacheEntry *miss = mock_cache_lookup("SELECT * FROM orders");
        TEST_ASSERT_NULL(miss);

        mock_executor_cleanup();
    TEST_END();
}

/* Test 17: Query cache invalidation */
static void test_cache_invalidation(void)
{
    TEST_BEGIN("cache_invalidation")
        mock_executor_init();

        int64 results[] = {1, 2, 3};
        mock_cache_store("SELECT * FROM users", results, 3);
        mock_cache_store("SELECT * FROM orders", results, 3);

        TEST_ASSERT_NOT_NULL(mock_cache_lookup("SELECT * FROM users"));
        TEST_ASSERT_NOT_NULL(mock_cache_lookup("SELECT * FROM orders"));

        mock_cache_invalidate(1001);

        /* All entries should be invalidated */
        TEST_ASSERT_NULL(mock_cache_lookup("SELECT * FROM users"));
        TEST_ASSERT_NULL(mock_cache_lookup("SELECT * FROM orders"));

        mock_executor_cleanup();
    TEST_END();
}

/* Test 18: Retryable error detection */
static void test_retryable_errors(void)
{
    TEST_BEGIN("retryable_errors")
        TEST_ASSERT(mock_is_retryable_error("connection refused"));
        TEST_ASSERT(mock_is_retryable_error("timeout exceeded"));
        TEST_ASSERT(mock_is_retryable_error("temporary failure"));

        TEST_ASSERT_FALSE(mock_is_retryable_error("syntax error"));
        TEST_ASSERT_FALSE(mock_is_retryable_error("permission denied"));
        TEST_ASSERT_FALSE(mock_is_retryable_error(NULL));
    TEST_END();
}

/* Test 19: Streaming results */
static void test_streaming_results(void)
{
    TEST_BEGIN("streaming_results")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(4);

        for (int i = 0; i < 3; i++)
        {
            MockFragmentQuery *frag = mock_create_fragment(i + 1, 1, "SELECT * FROM large_table");
            MockRemoteTask *task = mock_create_task(frag);
            mock_add_task_to_executor(state, task);
        }

        mock_execute_all_tasks(state);

        MockStreamingState *stream = mock_create_streaming(state, 5);
        TEST_ASSERT_NOT_NULL(stream);
        TEST_ASSERT_EQ(5, stream->batch_size);
        TEST_ASSERT_FALSE(stream->is_complete);

        int64 batch[10];
        int batch_count = mock_get_next_batch(stream, batch);
        TEST_ASSERT_GT(batch_count, 0);

        mock_free_streaming(stream);
        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 20: Parallel execution tracking */
static void test_parallel_execution_tracking(void)
{
    TEST_BEGIN("parallel_execution_tracking")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(2);

        /* Add more tasks than max_parallel */
        for (int i = 0; i < 5; i++)
        {
            MockFragmentQuery *frag = mock_create_fragment(i + 1, i % 3 + 1, "SELECT 1");
            MockRemoteTask *task = mock_create_task(frag);
            mock_add_task_to_executor(state, task);
        }

        TEST_ASSERT_EQ(5, state->task_count);
        TEST_ASSERT_EQ(5, state->pending_count);
        TEST_ASSERT_EQ(2, state->max_parallel);

        mock_execute_all_tasks(state);

        TEST_ASSERT_EQ(5, state->completed_count);
        TEST_ASSERT(state->all_completed);

        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 21: Error propagation */
static void test_error_propagation(void)
{
    TEST_BEGIN("error_propagation")
        mock_executor_init();

        MockExecutorState *state = mock_create_executor_state(4);

        MockFragmentQuery *frag1 = mock_create_fragment(1, 1, "SELECT 1");
        MockFragmentQuery *frag2 = mock_create_fragment(2, 2, "SELECT 2");

        MockRemoteTask *task1 = mock_create_task(frag1);
        MockRemoteTask *task2 = mock_create_task(frag2);

        mock_add_task_to_executor(state, task1);
        mock_add_task_to_executor(state, task2);

        /* Execute first task normally */
        mock_execute_task(task1);
        state->pending_count--;
        state->completed_count++;

        /* Fail second task */
        state->pending_count--;
        mock_fail_task(task2, "shard unavailable");
        state->failed_count++;
        state->has_error = true;
        strncpy(state->error_message, task2->error_message, 255);

        TEST_ASSERT(state->has_error);
        TEST_ASSERT_STR_EQ("shard unavailable", state->error_message);
        TEST_ASSERT_EQ(1, state->failed_count);
        TEST_ASSERT_EQ(1, state->completed_count);

        mock_free_executor_state(state);
        mock_executor_cleanup();
    TEST_END();
}

/* Test 22: Task wait */
static void test_task_wait(void)
{
    TEST_BEGIN("task_wait")
        mock_executor_init();

        MockFragmentQuery *frag = mock_create_fragment(1, 1, "SELECT sleep(1)");
        MockRemoteTask *task = mock_create_task(frag);

        /* Pending task should not be ready */
        bool ready = mock_wait_for_task(task, 100);
        TEST_ASSERT_FALSE(ready);

        mock_execute_task(task);

        /* Completed task should be ready */
        ready = mock_wait_for_task(task, 100);
        TEST_ASSERT(ready);

        mock_executor_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_executor(void)
{
    test_fragment_creation();
    test_task_creation();
    test_task_execution();
    test_task_failure();
    test_task_retry();
    test_task_cancellation();
    test_executor_state();
    test_execute_all_tasks();
    test_result_merger();
    test_aggregate_count();
    test_aggregate_sum();
    test_aggregate_min_max();
    test_aggregate_avg();
    test_distributed_txn_success();
    test_distributed_txn_rollback();
    test_query_cache();
    test_cache_invalidation();
    test_retryable_errors();
    test_streaming_results();
    test_parallel_execution_tracking();
    test_error_propagation();
    test_task_wait();
}
