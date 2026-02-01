/*-------------------------------------------------------------------------
 *
 * distributed_executor.c
 *    Orochi DB distributed query executor implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/executor.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#ifndef WIN32
#include <sys/select.h>
#endif

#include "../consensus/raft_integration.h"
#include "../core/catalog.h"
#include "../orochi.h"
#include "distributed_executor.h"

/* GUC variable for Raft-based distributed commits */
bool orochi_use_raft_consensus = true;

/* ============================================================
 * LWLock Protection for Thread Safety
 * ============================================================ */

/* Lock tranche names for connection pool and query cache */
#if PG_VERSION_NUM >= 180000
/* PG18 simplified the tranche API - just use names directly */
static const char *connection_pool_tranche_name = "OrochiConnectionPool";
static const char *query_cache_tranche_name = "OrochiQueryCache";
#else
static LWLockTranche connection_pool_tranche = {.name = "OrochiConnectionPool"};
static LWLockTranche query_cache_tranche = {.name = "OrochiQueryCache"};
#endif

/* LWLocks for protecting shared state */
static LWLockPadded connection_pool_lock_padded;
static LWLockPadded query_cache_lock_padded;
static LWLock *connection_pool_lock = NULL;
static LWLock *query_cache_lock = NULL;
static bool locks_initialized = false;

/*
 * Initialize LWLocks for thread safety
 *
 * Security: Prevents race conditions in concurrent access to shared data
 * structures
 */
static void init_executor_locks(void) {
  if (locks_initialized)
    return;

  /* Initialize connection pool lock */
#if PG_VERSION_NUM >= 180000
  LWLockRegisterTranche(LWTRANCHE_FIRST_USER_DEFINED,
                        connection_pool_tranche_name);
#else
  LWLockRegisterTranche(LWTRANCHE_FIRST_USER_DEFINED, &connection_pool_tranche);
#endif
  LWLockInitialize(&connection_pool_lock_padded.lock,
                   LWTRANCHE_FIRST_USER_DEFINED);
  connection_pool_lock = &connection_pool_lock_padded.lock;

  /* Initialize query cache lock */
#if PG_VERSION_NUM >= 180000
  LWLockRegisterTranche(LWTRANCHE_FIRST_USER_DEFINED + 1,
                        query_cache_tranche_name);
#else
  LWLockRegisterTranche(LWTRANCHE_FIRST_USER_DEFINED + 1, &query_cache_tranche);
#endif
  LWLockInitialize(&query_cache_lock_padded.lock,
                   LWTRANCHE_FIRST_USER_DEFINED + 1);
  query_cache_lock = &query_cache_lock_padded.lock;

  locks_initialized = true;
}

/* ============================================================
 * Connection Pool Management
 * ============================================================ */

#define MAX_CONNECTION_POOL_SIZE 64
#define CONNECTION_TIMEOUT_SEC 30

typedef struct ConnectionPoolEntry {
  int32 node_id;
  PGconn *conn;
  bool in_use;
  TimestampTz last_used;
} ConnectionPoolEntry;

static ConnectionPoolEntry connection_pool[MAX_CONNECTION_POOL_SIZE];
static int connection_pool_size = 0;
static bool pool_initialized = false;

/* Transaction state for 2PC */
typedef struct DistributedTransactionState {
  char gid[64];       /* Global transaction ID */
  List *participants; /* List of node_ids */
  bool prepared;      /* Phase 1 complete? */
} DistributedTransactionState;

static DistributedTransactionState *current_dtxn = NULL;

/*
 * Initialize connection pool
 *
 * Security: Also initializes LWLocks for thread-safe access
 */
static void init_connection_pool(void) {
  if (pool_initialized)
    return;

  /* Initialize locks first for thread safety */
  init_executor_locks();

  memset(connection_pool, 0, sizeof(connection_pool));
  pool_initialized = true;
}

/*
 * Build libpq connection string for a node
 */
static char *build_connection_string(OrochiNodeInfo *node) {
  StringInfoData connstr;

  initStringInfo(&connstr);
  appendStringInfo(&connstr, "host=%s port=%d dbname=%s connect_timeout=%d",
                   node->hostname, node->port, get_database_name(MyDatabaseId),
                   CONNECTION_TIMEOUT_SEC);

  return connstr.data;
}

/* Previous executor hooks */
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorRun_hook_type prev_executor_run_hook = NULL;
static ExecutorFinish_hook_type prev_executor_finish_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

/* ============================================================
 * Executor Hooks
 * ============================================================ */

void orochi_executor_hook_init(void) {
  prev_executor_start_hook = ExecutorStart_hook;
  ExecutorStart_hook = orochi_executor_start_hook;

  prev_executor_run_hook = ExecutorRun_hook;
  ExecutorRun_hook = orochi_executor_run_hook;

  prev_executor_finish_hook = ExecutorFinish_hook;
  ExecutorFinish_hook = orochi_executor_finish_hook;

  prev_executor_end_hook = ExecutorEnd_hook;
  ExecutorEnd_hook = orochi_executor_end_hook;
}

void orochi_executor_start_hook(QueryDesc *queryDesc, int eflags) {
  /* Call previous hook */
  if (prev_executor_start_hook)
    prev_executor_start_hook(queryDesc, eflags);
  else
    standard_ExecutorStart(queryDesc, eflags);
}

void
#if PG_VERSION_NUM >= 180000
orochi_executor_run_hook(QueryDesc *queryDesc, ScanDirection direction,
                         uint64 count)
#else
orochi_executor_run_hook(QueryDesc *queryDesc, ScanDirection direction,
                         uint64 count, bool execute_once)
#endif
{
  /* Call previous hook */
  if (prev_executor_run_hook)
#if PG_VERSION_NUM >= 180000
    prev_executor_run_hook(queryDesc, direction, count);
  else
    standard_ExecutorRun(queryDesc, direction, count);
#else
    prev_executor_run_hook(queryDesc, direction, count, execute_once);
  else
    standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
}

void orochi_executor_finish_hook(QueryDesc *queryDesc) {
  /* Call previous hook */
  if (prev_executor_finish_hook)
    prev_executor_finish_hook(queryDesc);
  else
    standard_ExecutorFinish(queryDesc);
}

void orochi_executor_end_hook(QueryDesc *queryDesc) {
  /* Call previous hook */
  if (prev_executor_end_hook)
    prev_executor_end_hook(queryDesc);
  else
    standard_ExecutorEnd(queryDesc);
}

/* ============================================================
 * Distributed Execution
 * ============================================================ */

void orochi_execute_distributed_plan(DistributedPlan *plan,
                                     DistributedExecutorState *state) {
  ListCell *lc;

  state->plan = plan;
  state->tasks = NIL;
  state->pending_count = 0;
  state->running_count = 0;
  state->completed_count = 0;
  state->failed_count = 0;
  state->all_completed = false;
  state->has_error = false;

  /* Create tasks for each fragment */
  foreach (lc, plan->fragment_queries) {
    FragmentQuery *fragment = (FragmentQuery *)lfirst(lc);
    RemoteTask *task = palloc0(sizeof(RemoteTask));

    task->fragment = fragment;
    task->state = TASK_PENDING;
    task->retry_count = 0;

    state->tasks = lappend(state->tasks, task);
    state->pending_count++;
  }

  /* Execute using adaptive algorithm */
  orochi_adaptive_execute(state);
}

/*
 * Get connection from pool or create new
 *
 * Security: Uses LWLock to prevent race conditions in concurrent access
 */
void *orochi_get_worker_connection(int32 node_id) {
  int i;
  PGconn *conn = NULL;
  PGconn *found_conn = NULL;
  OrochiNodeInfo *node;
  char *connstr;
  bool need_new_connection = true;

  init_connection_pool();

  /* Acquire exclusive lock for pool access */
  LWLockAcquire(connection_pool_lock, LW_EXCLUSIVE);

  /* Check for existing idle connection */
  for (i = 0; i < connection_pool_size; i++) {
    if (connection_pool[i].node_id == node_id && !connection_pool[i].in_use) {
      /* Verify connection is still valid */
      if (PQstatus(connection_pool[i].conn) == CONNECTION_OK) {
        connection_pool[i].in_use = true;
        connection_pool[i].last_used = GetCurrentTimestamp();
        found_conn = connection_pool[i].conn;
        need_new_connection = false;
        break;
      } else {
        /* Connection died, clean up */
        PQfinish(connection_pool[i].conn);
        connection_pool[i].conn = NULL;
        connection_pool[i].node_id = 0;
      }
    }
  }

  LWLockRelease(connection_pool_lock);

  if (!need_new_connection)
    return found_conn;

  /* Create new connection (outside of lock to avoid blocking) */
  node = orochi_catalog_get_node(node_id);
  if (node == NULL) {
    elog(WARNING, "Node %d not found in catalog", node_id);
    return NULL;
  }

  connstr = build_connection_string(node);
  conn = PQconnectdb(connstr);
  pfree(connstr);

  if (PQstatus(conn) != CONNECTION_OK) {
    elog(WARNING, "Failed to connect to node %d: %s", node_id,
         PQerrorMessage(conn));
    PQfinish(conn);
    return NULL;
  }

  /* Add to pool under lock */
  LWLockAcquire(connection_pool_lock, LW_EXCLUSIVE);

  if (connection_pool_size < MAX_CONNECTION_POOL_SIZE) {
    connection_pool[connection_pool_size].node_id = node_id;
    connection_pool[connection_pool_size].conn = conn;
    connection_pool[connection_pool_size].in_use = true;
    connection_pool[connection_pool_size].last_used = GetCurrentTimestamp();
    connection_pool_size++;
  }

  LWLockRelease(connection_pool_lock);

  return conn;
}

/*
 * Release connection back to pool
 *
 * Security: Uses LWLock to prevent race conditions
 */
void orochi_release_worker_connection(void *conn) {
  int i;
  bool found = false;

  if (conn == NULL)
    return;

  /* Acquire lock before modifying pool state */
  LWLockAcquire(connection_pool_lock, LW_EXCLUSIVE);

  for (i = 0; i < connection_pool_size; i++) {
    if (connection_pool[i].conn == conn) {
      connection_pool[i].in_use = false;
      connection_pool[i].last_used = GetCurrentTimestamp();
      found = true;
      break;
    }
  }

  LWLockRelease(connection_pool_lock);

  /* Not in pool, just close it (outside lock) */
  if (!found)
    PQfinish((PGconn *)conn);
}

/*
 * Close all connections
 *
 * Security: Uses LWLock to prevent race conditions
 */
void orochi_close_all_connections(void) {
  int i;
  PGconn *conns_to_close[MAX_CONNECTION_POOL_SIZE];
  int num_to_close = 0;

  /* Acquire lock to get list of connections */
  LWLockAcquire(connection_pool_lock, LW_EXCLUSIVE);

  for (i = 0; i < connection_pool_size; i++) {
    if (connection_pool[i].conn != NULL) {
      conns_to_close[num_to_close++] = connection_pool[i].conn;
      connection_pool[i].conn = NULL;
    }
  }

  connection_pool_size = 0;

  LWLockRelease(connection_pool_lock);

  /* Close connections outside of lock to avoid blocking */
  for (i = 0; i < num_to_close; i++) {
    PQfinish(conns_to_close[i]);
  }
}

void orochi_execute_remote_task(RemoteTask *task) {
  PGconn *conn;
  PGresult *res;
  int32 node_id;

  task->state = TASK_RUNNING;
  task->started_at = GetCurrentTimestamp();

  node_id = task->fragment->node_id;
  elog(DEBUG1, "Executing task on shard %ld (node %d)",
       task->fragment->shard_id, node_id);

  /* Get connection to worker */
  conn = (PGconn *)orochi_get_worker_connection(node_id);
  if (conn == NULL) {
    task->state = TASK_FAILED;
    task->error_message = pstrdup("Failed to connect to worker node");
    return;
  }

  task->connection = conn;

  /* Execute query synchronously */
  res = PQexec(conn, task->fragment->query_string);

  if (PQresultStatus(res) == PGRES_COMMAND_OK ||
      PQresultStatus(res) == PGRES_TUPLES_OK) {
    task->state = TASK_COMPLETED;
    task->result = res;
    task->rows_affected = atol(PQcmdTuples(res));
    elog(DEBUG1, "Task completed: %ld rows", task->rows_affected);
  } else {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    elog(WARNING, "Task failed: %s", task->error_message);
    PQclear(res);
  }

  task->completed_at = GetCurrentTimestamp();
}

bool orochi_wait_for_task(RemoteTask *task, int timeout_ms) {
  PGconn *conn;
  int socket;
  fd_set input_mask;
  struct timeval timeout;
  int ret;

  if (task->state != TASK_RUNNING)
    return (task->state == TASK_COMPLETED);

  conn = (PGconn *)task->connection;
  if (conn == NULL)
    return false;

  socket = PQsocket(conn);
  if (socket < 0)
    return false;

  /* Set up select timeout */
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  FD_ZERO(&input_mask);
  FD_SET(socket, &input_mask);

  /* Wait for data to become available */
  ret = select(socket + 1, &input_mask, NULL, NULL, &timeout);

  if (ret < 0) {
    /* Error in select */
    return false;
  } else if (ret == 0) {
    /* Timeout - still waiting */
    return false;
  }

  /* Data available - consume it */
  if (!PQconsumeInput(conn)) {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    return false;
  }

  /* Check if result is ready */
  if (PQisBusy(conn))
    return false; /* Still processing */

  /* Get the result */
  PGresult *res = PQgetResult(conn);
  if (res == NULL) {
    /* No more results */
    return (task->state == TASK_COMPLETED);
  }

  if (PQresultStatus(res) == PGRES_COMMAND_OK ||
      PQresultStatus(res) == PGRES_TUPLES_OK) {
    task->state = TASK_COMPLETED;
    task->result = res;
    task->rows_affected = atol(PQcmdTuples(res));
  } else {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    PQclear(res);
  }

  task->completed_at = GetCurrentTimestamp();
  return (task->state == TASK_COMPLETED);
}

void orochi_cancel_task(RemoteTask *task) {
  PGconn *conn;
  PGcancel *cancel;
  char errbuf[256];

  if (task->state != TASK_RUNNING)
    return;

  conn = (PGconn *)task->connection;
  if (conn == NULL) {
    task->state = TASK_CANCELLED;
    return;
  }

  /* Send cancel request to backend */
  cancel = PQgetCancel(conn);
  if (cancel != NULL) {
    if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
      elog(WARNING, "Failed to cancel task: %s", errbuf);
    PQfreeCancel(cancel);
  }

  task->state = TASK_CANCELLED;
  task->completed_at = GetCurrentTimestamp();

  /* Clear any pending results */
  while (true) {
    PGresult *res = PQgetResult(conn);
    if (res == NULL)
      break;
    PQclear(res);
  }
}

bool orochi_retry_task(RemoteTask *task) {
  if (task->retry_count >= EXECUTOR_MAX_RETRIES)
    return false;

  task->retry_count++;
  task->state = TASK_PENDING;

  return true;
}

/* ============================================================
 * Adaptive Execution
 * ============================================================ */

void orochi_adaptive_execute(DistributedExecutorState *state) {
  int active_connections = 1; /* Start with 1 */
  ListCell *lc;

  while (!state->all_completed && !state->has_error) {
    int started = 0;

    /* Start tasks up to active_connections limit */
    foreach (lc, state->tasks) {
      RemoteTask *task = (RemoteTask *)lfirst(lc);

      if (task->state == TASK_PENDING &&
          state->running_count < active_connections) {
        orochi_execute_remote_task(task);
        state->pending_count--;
        state->running_count++;
        started++;
      }
    }

    /* Wait for any task to complete */
    foreach (lc, state->tasks) {
      RemoteTask *task = (RemoteTask *)lfirst(lc);

      if (task->state == TASK_RUNNING) {
        if (orochi_wait_for_task(task, EXECUTOR_SLOW_START_INTERVAL_MS)) {
          state->running_count--;
          state->completed_count++;
        }
      }
    }

    /* Check completion */
    state->all_completed = (state->completed_count + state->failed_count ==
                            list_length(state->tasks));

    /* Slow start: increase parallelism */
    if (active_connections < state->max_parallel)
      active_connections++;
  }
}

void orochi_adjust_parallelism(DistributedExecutorState *state) {
  int64 avg_latency_ms = 0;
  int completed = 0;
  ListCell *lc;

  /* Calculate average task completion time */
  foreach (lc, state->tasks) {
    RemoteTask *task = (RemoteTask *)lfirst(lc);

    if (task->state == TASK_COMPLETED &&
        task->completed_at > task->started_at) {
      /* Calculate latency in milliseconds */
      int64 latency = (task->completed_at - task->started_at) / 1000;
      avg_latency_ms += latency;
      completed++;
    }
  }

  if (completed == 0)
    return;

  avg_latency_ms /= completed;

  /* Adjust parallelism based on average latency */
  if (avg_latency_ms < 10) {
    /* Tasks completing very fast - increase parallelism aggressively */
    state->max_parallel =
        Min(state->max_parallel * 2, EXECUTOR_MAX_CONNECTIONS);
  } else if (avg_latency_ms < 50) {
    /* Tasks completing reasonably fast - increase parallelism */
    state->max_parallel =
        Min(state->max_parallel + 2, EXECUTOR_MAX_CONNECTIONS);
  } else if (avg_latency_ms > 1000) {
    /* Tasks taking too long - decrease parallelism */
    state->max_parallel = Max(state->max_parallel / 2, 1);
  } else if (avg_latency_ms > 500) {
    /* Tasks a bit slow - decrease parallelism slightly */
    state->max_parallel = Max(state->max_parallel - 1, 1);
  }

  elog(DEBUG1, "Adjusted parallelism to %d (avg latency: %ld ms)",
       state->max_parallel, avg_latency_ms);
}

/* ============================================================
 * Result Handling
 * ============================================================ */

ResultMergerState *orochi_init_result_merger(DistributedExecutorState *state) {
  ResultMergerState *merger = palloc0(sizeof(ResultMergerState));
  ListCell *lc;

  /* Collect completed tasks */
  foreach (lc, state->tasks) {
    RemoteTask *task = (RemoteTask *)lfirst(lc);

    if (task->state == TASK_COMPLETED)
      merger->completed_tasks = lappend(merger->completed_tasks, task);
  }

  merger->current_task = 0;
  merger->current_row = 0;

  return merger;
}

bool orochi_get_next_result(ResultMergerState *merger, TupleTableSlot *slot) {
  RemoteTask *task;
  PGresult *res;
  int nfields;
  int i;

  if (merger == NULL || list_length(merger->completed_tasks) == 0)
    return false;

  /* Get current task */
  while (merger->current_task < list_length(merger->completed_tasks)) {
    task =
        (RemoteTask *)list_nth(merger->completed_tasks, merger->current_task);
    res = (PGresult *)task->result;

    if (res == NULL) {
      merger->current_task++;
      merger->current_row = 0;
      continue;
    }

    /* Check if we have more rows in current result */
    if (merger->current_row < PQntuples(res)) {
      /* Build tuple from PGresult row */
      nfields = PQnfields(res);

      /* Clear the slot first */
      ExecClearTuple(slot);

      /* Get values from result */
      for (i = 0; i < nfields && i < slot->tts_tupleDescriptor->natts; i++) {
        if (PQgetisnull(res, merger->current_row, i)) {
          slot->tts_isnull[i] = true;
          slot->tts_values[i] = (Datum)0;
        } else {
          char *value = PQgetvalue(res, merger->current_row, i);
          Oid typid = TupleDescAttr(slot->tts_tupleDescriptor, i)->atttypid;

          slot->tts_isnull[i] = false;

          /* Convert string value to appropriate Datum based on type */
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
            /* For other types, store as text */
            slot->tts_values[i] = CStringGetTextDatum(value);
            break;
          }
        }
      }

      /* Mark remaining columns as null */
      for (; i < slot->tts_tupleDescriptor->natts; i++) {
        slot->tts_isnull[i] = true;
        slot->tts_values[i] = (Datum)0;
      }

      ExecStoreVirtualTuple(slot);
      merger->current_row++;
      return true;
    }

    /* Move to next task */
    merger->current_task++;
    merger->current_row = 0;
  }

  return false;
}

void orochi_free_result_merger(ResultMergerState *merger) {
  if (merger != NULL)
    pfree(merger);
}

/* ============================================================
 * Parallel Execution Engine
 * ============================================================ */

/*
 * Execute distributed query with parallel shard access.
 * Uses async execution to maximize throughput.
 */
void orochi_parallel_execute(DistributedExecutorState *state, int parallelism) {
  ListCell *lc;
  int active = 0;
  List *pending = NIL;
  List *running = NIL;

  /* Initialize pending list */
  foreach (lc, state->tasks) {
    RemoteTask *task = (RemoteTask *)lfirst(lc);
    pending = lappend(pending, task);
  }

  while (list_length(pending) > 0 || list_length(running) > 0) {
    ListCell *next;

    /* Start new tasks up to parallelism limit */
    while (list_length(pending) > 0 && active < parallelism) {
      RemoteTask *task = (RemoteTask *)linitial(pending);
      pending = list_delete_first(pending);

      /* Start async execution */
      orochi_start_async_task(task);
      running = lappend(running, task);
      active++;
    }

    /* Check running tasks */
    for (lc = list_head(running); lc != NULL; lc = next) {
      RemoteTask *task = (RemoteTask *)lfirst(lc);
      next = lnext(running, lc);

      if (orochi_check_task_completion(task)) {
        running = list_delete_ptr(running, task);
        active--;

        if (task->state == TASK_COMPLETED)
          state->completed_count++;
        else if (task->state == TASK_FAILED) {
          if (orochi_retry_task(task))
            pending = lappend(pending, task);
          else
            state->failed_count++;
        }
      }
    }

    /* Small sleep to avoid busy-waiting */
    if (list_length(running) > 0)
      pg_usleep(1000); /* 1ms */
  }

  state->all_completed = true;
}

/*
 * Start a task in async mode
 */
void orochi_start_async_task(RemoteTask *task) {
  PGconn *conn;
  int32 node_id;

  task->state = TASK_RUNNING;
  task->started_at = GetCurrentTimestamp();

  node_id = task->fragment->node_id;

  /* Get connection to worker */
  conn = (PGconn *)orochi_get_worker_connection(node_id);
  if (conn == NULL) {
    task->state = TASK_FAILED;
    task->error_message = pstrdup("Failed to connect to worker node");
    return;
  }

  task->connection = conn;

  /* Send query asynchronously */
  if (!PQsendQuery(conn, task->fragment->query_string)) {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    elog(WARNING, "Failed to send async query: %s", task->error_message);
    return;
  }

  elog(DEBUG1, "Started async task on shard %ld (node %d)",
       task->fragment->shard_id, node_id);
}

/*
 * Check if a task has completed
 */
bool orochi_check_task_completion(RemoteTask *task) {
  PGconn *conn;
  PGresult *res;

  if (task->state != TASK_RUNNING)
    return true;

  conn = (PGconn *)task->connection;
  if (conn == NULL)
    return true;

  /* Consume any available input */
  if (!PQconsumeInput(conn)) {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    task->completed_at = GetCurrentTimestamp();
    return true;
  }

  /* Check if result is ready */
  if (PQisBusy(conn))
    return false; /* Still processing */

  /* Get the result */
  res = PQgetResult(conn);
  if (res == NULL) {
    /* Query finished - mark as complete if not already failed */
    if (task->state == TASK_RUNNING)
      task->state = TASK_COMPLETED;
    task->completed_at = GetCurrentTimestamp();
    return true;
  }

  if (PQresultStatus(res) == PGRES_COMMAND_OK ||
      PQresultStatus(res) == PGRES_TUPLES_OK) {
    task->state = TASK_COMPLETED;
    task->result = res;
    task->rows_affected = atol(PQcmdTuples(res));
    elog(DEBUG1, "Async task completed: %ld rows", task->rows_affected);
  } else {
    task->state = TASK_FAILED;
    task->error_message = pstrdup(PQerrorMessage(conn));
    elog(WARNING, "Async task failed: %s", task->error_message);
    PQclear(res);
  }

  task->completed_at = GetCurrentTimestamp();
  return true;
}

/* ============================================================
 * Aggregate Pushdown Execution
 * ============================================================ */

/*
 * Execute distributed aggregation query with pushdown.
 * Aggregates are computed on workers, then combined at coordinator.
 */
void orochi_execute_aggregate_pushdown(DistributedExecutorState *state,
                                       AggPushdownPlan *agg_plan) {
  ListCell *lc;
  ListCell *task_lc;
  List *partial_results = NIL;
  int task_idx = 0;

  /* Phase 1: Execute partial aggregations on workers */
  forboth(lc, agg_plan->worker_queries, task_lc, state->tasks) {
    const char *query = (const char *)lfirst(lc);
    RemoteTask *task = (RemoteTask *)lfirst(task_lc);
    PartialAggResult *partial = palloc0(sizeof(PartialAggResult));
    PGconn *conn;
    PGresult *res;

    partial->shard_id = task->fragment->shard_id;

    /* Get connection to worker */
    conn = (PGconn *)orochi_get_worker_connection(task->fragment->node_id);
    if (conn == NULL) {
      elog(WARNING, "Failed to connect to worker for aggregate pushdown");
      partial->count = 0;
      partial->sum = 0;
      partial->min_val = 0;
      partial->max_val = 0;
      partial_results = lappend(partial_results, partial);
      continue;
    }

    /* Execute partial aggregate query on worker */
    res = PQexec(conn, query);

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
      /* Parse result - expected format: count, sum, min, max */
      int nfields = PQnfields(res);

      if (nfields >= 1 && !PQgetisnull(res, 0, 0))
        partial->count = atoll(PQgetvalue(res, 0, 0));
      if (nfields >= 2 && !PQgetisnull(res, 0, 1))
        partial->sum = atoll(PQgetvalue(res, 0, 1));
      if (nfields >= 3 && !PQgetisnull(res, 0, 2))
        partial->min_val = atoll(PQgetvalue(res, 0, 2));
      if (nfields >= 4 && !PQgetisnull(res, 0, 3))
        partial->max_val = atoll(PQgetvalue(res, 0, 3));

      elog(DEBUG1, "Partial aggregate from shard %ld: count=%ld, sum=%ld",
           partial->shard_id, partial->count, partial->sum);
    } else {
      elog(WARNING, "Aggregate query failed on shard %ld: %s",
           partial->shard_id, PQerrorMessage(conn));
    }

    PQclear(res);
    orochi_release_worker_connection(conn);

    partial_results = lappend(partial_results, partial);
    task_idx++;
  }

  /* Phase 2: Combine partial results at coordinator */
  agg_plan->combined_result =
      orochi_combine_partial_aggregates(partial_results, agg_plan->agg_type);

  /* Cleanup partial results */
  list_free_deep(partial_results);
}

/*
 * Combine partial aggregation results
 */
Datum orochi_combine_partial_aggregates(List *partials, int agg_type) {
  ListCell *lc;
  int64 total_count = 0;
  int64 total_sum = 0;
  int64 min_val = PG_INT64_MAX;
  int64 max_val = PG_INT64_MIN;

  foreach (lc, partials) {
    PartialAggResult *partial = (PartialAggResult *)lfirst(lc);

    total_count += partial->count;
    total_sum += partial->sum;

    if (partial->min_val < min_val)
      min_val = partial->min_val;
    if (partial->max_val > max_val)
      max_val = partial->max_val;
  }

  /* Return appropriate value based on aggregate type */
  switch (agg_type) {
  case AGG_TYPE_COUNT:
    return Int64GetDatum(total_count);
  case AGG_TYPE_SUM:
    return Int64GetDatum(total_sum);
  case AGG_TYPE_MIN:
    return Int64GetDatum(min_val);
  case AGG_TYPE_MAX:
    return Int64GetDatum(max_val);
  case AGG_TYPE_AVG:
    if (total_count > 0)
      return Float8GetDatum((double)total_sum / total_count);
    else
      return Float8GetDatum(0.0);
  default:
    return Int64GetDatum(0);
  }
}

/* ============================================================
 * Query Result Caching with Table Dependency Tracking
 * ============================================================ */

/* Simple hash table for query cache */
static HTAB *query_cache = NULL;

/* Hash table for tracking table dependencies */
static HTAB *cache_dependencies = NULL;

#define MAX_DEPENDENT_TABLES 16

typedef struct QueryCacheEntry {
  uint64 query_hash;
  char *result_data;
  int64 result_size;
  TimestampTz cached_at;
  int64 hit_count;
  Oid dependent_tables[MAX_DEPENDENT_TABLES]; /* Tables this entry depends on */
  int num_dependencies;
} QueryCacheEntry;

typedef struct CacheDependencyEntry {
  Oid table_oid;                /* Key: table OID */
  uint64 dependent_queries[64]; /* Query hashes that depend on this table */
  int num_queries;
} CacheDependencyEntry;

/*
 * Initialize cache dependency tracking
 */
static void init_cache_dependencies(void) {
  HASHCTL hash_ctl;

  if (cache_dependencies != NULL)
    return;

  memset(&hash_ctl, 0, sizeof(hash_ctl));
  hash_ctl.keysize = sizeof(Oid);
  hash_ctl.entrysize = sizeof(CacheDependencyEntry);
  hash_ctl.hcxt = TopMemoryContext;

  cache_dependencies = hash_create("OrochiCacheDependencies", 256, &hash_ctl,
                                   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Register a cache entry's dependency on a table
 */
static void register_cache_dependency(uint64 query_hash, Oid table_oid) {
  CacheDependencyEntry *entry;
  bool found;

  init_cache_dependencies();

  entry = hash_search(cache_dependencies, &table_oid, HASH_ENTER, &found);

  if (!found) {
    entry->table_oid = table_oid;
    entry->num_queries = 0;
  }

  /* Add query hash if not already present and space available */
  if (entry->num_queries < 64) {
    int i;
    bool exists = false;

    for (i = 0; i < entry->num_queries; i++) {
      if (entry->dependent_queries[i] == query_hash) {
        exists = true;
        break;
      }
    }

    if (!exists)
      entry->dependent_queries[entry->num_queries++] = query_hash;
  }
}

/*
 * Initialize query result cache
 *
 * Security: Thread-safe initialization with lock protection
 */
void orochi_init_query_cache(void) {
  HASHCTL hash_ctl;

  /* Ensure locks are initialized first */
  init_executor_locks();

  if (query_cache != NULL)
    return;

  memset(&hash_ctl, 0, sizeof(hash_ctl));
  hash_ctl.keysize = sizeof(uint64);
  hash_ctl.entrysize = sizeof(QueryCacheEntry);
  hash_ctl.hcxt = TopMemoryContext;

  query_cache = hash_create("OrochiQueryCache", 1024, &hash_ctl,
                            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Compute hash for a query string
 */
static uint64 compute_query_hash(const char *query_string) {
  uint64 hash = 5381;
  int c;

  while ((c = *query_string++))
    hash = ((hash << 5) + hash) + c;

  return hash;
}

/*
 * Look up query in cache
 *
 * Security: Uses LWLock to prevent race conditions
 */
QueryCacheEntry *orochi_cache_lookup(const char *query_string) {
  uint64 hash;
  QueryCacheEntry *entry;
  QueryCacheEntry *result = NULL;
  bool found;
  bool expired = false;

  if (query_cache == NULL)
    orochi_init_query_cache();

  hash = compute_query_hash(query_string);

  /* Acquire shared lock for read access */
  LWLockAcquire(query_cache_lock, LW_SHARED);

  entry = hash_search(query_cache, &hash, HASH_FIND, &found);

  if (found) {
    /* Check if cache entry is still valid (5 minute TTL) */
    TimestampTz now = GetCurrentTimestamp();
    if (now - entry->cached_at < 5 * 60 * 1000000L) {
      entry->hit_count++;
      result = entry;
    } else {
      expired = true;
    }
  }

  LWLockRelease(query_cache_lock);

  /* If expired, remove under exclusive lock */
  if (expired) {
    LWLockAcquire(query_cache_lock, LW_EXCLUSIVE);
    hash_search(query_cache, &hash, HASH_REMOVE, NULL);
    LWLockRelease(query_cache_lock);
  }

  return result;
}

/*
 * Store query result in cache
 *
 * Security: Uses LWLock to prevent race conditions
 */
void orochi_cache_store(const char *query_string, const char *result_data,
                        int64 result_size) {
  uint64 hash;
  QueryCacheEntry *entry;
  bool found;

  if (query_cache == NULL)
    orochi_init_query_cache();

  hash = compute_query_hash(query_string);

  /* Acquire exclusive lock for write access */
  LWLockAcquire(query_cache_lock, LW_EXCLUSIVE);

  entry = hash_search(query_cache, &hash, HASH_ENTER, &found);

  if (!found || entry->result_data == NULL) {
    entry->query_hash = hash;
    entry->result_data = MemoryContextStrdup(TopMemoryContext, result_data);
    entry->result_size = result_size;
    entry->cached_at = GetCurrentTimestamp();
    entry->hit_count = 0;
  }

  LWLockRelease(query_cache_lock);
}

/*
 * Invalidate cache entries for a table using dependency tracking
 *
 * Security: Uses LWLock to prevent race conditions
 */
void orochi_cache_invalidate(Oid table_oid) {
  CacheDependencyEntry *dep_entry;
  bool found;
  int i;
  int invalidated = 0;
  uint64 hashes_to_remove[64];
  int num_to_remove = 0;
  char *data_to_free[64];
  int num_to_free = 0;

  if (query_cache == NULL)
    return;

  /* Acquire exclusive lock for modification */
  LWLockAcquire(query_cache_lock, LW_EXCLUSIVE);

  /* Look up which queries depend on this table */
  if (cache_dependencies != NULL) {
    dep_entry = hash_search(cache_dependencies, &table_oid, HASH_FIND, &found);

    if (found && dep_entry->num_queries > 0) {
      /* Collect entries to remove */
      for (i = 0; i < dep_entry->num_queries && num_to_remove < 64; i++) {
        uint64 hash = dep_entry->dependent_queries[i];
        QueryCacheEntry *cache_entry;

        cache_entry = hash_search(query_cache, &hash, HASH_FIND, &found);
        if (found) {
          if (cache_entry->result_data != NULL && num_to_free < 64)
            data_to_free[num_to_free++] = cache_entry->result_data;

          hashes_to_remove[num_to_remove++] = hash;
          invalidated++;
        }
      }

      /* Remove entries from hash table */
      for (i = 0; i < num_to_remove; i++) {
        hash_search(query_cache, &hashes_to_remove[i], HASH_REMOVE, NULL);
      }

      /* Clear the dependency entry */
      dep_entry->num_queries = 0;

      LWLockRelease(query_cache_lock);

      /* Free result data outside of lock */
      for (i = 0; i < num_to_free; i++) {
        pfree(data_to_free[i]);
      }

      elog(DEBUG1, "Invalidated %d cache entries for table OID %u", invalidated,
           table_oid);
      return;
    }
  }

  /* Fallback: if no dependency tracking or table not found,
   * clear the entire cache to be safe */
  elog(DEBUG1, "No dependency info for table OID %u, clearing entire cache",
       table_oid);
  hash_destroy(query_cache);
  query_cache = NULL;

  LWLockRelease(query_cache_lock);
}

/*
 * Store cache entry with table dependencies
 *
 * Security: Uses LWLock to prevent race conditions
 */
void orochi_cache_store_with_deps(const char *query_string,
                                  const char *result_data, int64 result_size,
                                  Oid *table_oids, int num_tables) {
  uint64 hash;
  QueryCacheEntry *entry;
  bool found;
  int i;

  if (query_cache == NULL)
    orochi_init_query_cache();

  hash = compute_query_hash(query_string);

  /* Acquire exclusive lock for write access */
  LWLockAcquire(query_cache_lock, LW_EXCLUSIVE);

  entry = hash_search(query_cache, &hash, HASH_ENTER, &found);

  if (!found || entry->result_data == NULL) {
    entry->query_hash = hash;
    entry->result_data = MemoryContextStrdup(TopMemoryContext, result_data);
    entry->result_size = result_size;
    entry->cached_at = GetCurrentTimestamp();
    entry->hit_count = 0;
    entry->num_dependencies = 0;

    /* Record table dependencies */
    for (i = 0; i < num_tables && i < MAX_DEPENDENT_TABLES; i++) {
      entry->dependent_tables[i] = table_oids[i];
      entry->num_dependencies++;

      /* Also register in reverse lookup */
      register_cache_dependency(hash, table_oids[i]);
    }
  }

  LWLockRelease(query_cache_lock);
}

/* ============================================================
 * Streaming Result Processing
 * ============================================================ */

/*
 * Initialize streaming result processor
 */
StreamingResultState *
orochi_init_streaming_result(DistributedExecutorState *state) {
  StreamingResultState *stream = palloc0(sizeof(StreamingResultState));

  stream->executor_state = state;
  stream->current_shard = 0;
  stream->rows_processed = 0;
  stream->batch_size = 1000; /* Process 1000 rows at a time */
  stream->is_complete = false;

  return stream;
}

/*
 * Result row for batch processing
 */
typedef struct BatchRow {
  Datum *values;
  bool *nulls;
  int nfields;
} BatchRow;

/*
 * Get next batch of rows from streaming result
 */
List *orochi_get_next_batch(StreamingResultState *stream) {
  List *batch = NIL;
  int rows_in_batch = 0;

  if (stream == NULL || stream->is_complete)
    return NIL;

  /* Process each shard's results */
  while (stream->current_shard < list_length(stream->executor_state->tasks)) {
    RemoteTask *task = (RemoteTask *)list_nth(stream->executor_state->tasks,
                                              stream->current_shard);
    PGresult *res;
    int ntuples;
    int nfields;
    int row_start;

    /* Skip incomplete tasks */
    if (task->state != TASK_COMPLETED || task->result == NULL) {
      stream->current_shard++;
      continue;
    }

    res = (PGresult *)task->result;
    ntuples = PQntuples(res);
    nfields = PQnfields(res);

    /* Calculate starting row for this shard */
    row_start = (int)(stream->rows_processed % ntuples);
    if (stream->rows_processed > 0 && row_start == 0) {
      stream->current_shard++;
      continue;
    }

    /* Read rows from current position */
    while (row_start < ntuples && rows_in_batch < stream->batch_size) {
      BatchRow *row = palloc(sizeof(BatchRow));
      int i;

      row->nfields = nfields;
      row->values = palloc(sizeof(Datum) * nfields);
      row->nulls = palloc(sizeof(bool) * nfields);

      for (i = 0; i < nfields; i++) {
        if (PQgetisnull(res, row_start, i)) {
          row->nulls[i] = true;
          row->values[i] = (Datum)0;
        } else {
          char *value = PQgetvalue(res, row_start, i);
          Oid typid = PQftype(res, i);

          row->nulls[i] = false;

          /* Convert based on type */
          switch (typid) {
          case INT4OID:
            row->values[i] = Int32GetDatum(atoi(value));
            break;
          case INT8OID:
            row->values[i] = Int64GetDatum(atoll(value));
            break;
          case FLOAT4OID:
            row->values[i] = Float4GetDatum((float4)atof(value));
            break;
          case FLOAT8OID:
            row->values[i] = Float8GetDatum(atof(value));
            break;
          case BOOLOID:
            row->values[i] = BoolGetDatum(value[0] == 't');
            break;
          default:
            row->values[i] = CStringGetTextDatum(value);
            break;
          }
        }
      }

      batch = lappend(batch, row);
      rows_in_batch++;
      row_start++;
      stream->rows_processed++;
    }

    /* If we filled the batch, return it */
    if (rows_in_batch >= stream->batch_size)
      return batch;

    /* Move to next shard */
    stream->current_shard++;
  }

  /* Reached end of all shards */
  if (stream->current_shard >= list_length(stream->executor_state->tasks))
    stream->is_complete = true;

  return batch;
}

/*
 * Clean up streaming result state
 */
void orochi_cleanup_streaming_result(StreamingResultState *stream) {
  if (stream != NULL)
    pfree(stream);
}

/* ============================================================
 * Transaction Coordination
 * ============================================================ */

/*
 * Generate unique global transaction ID
 */
static void generate_transaction_gid(char *gid, size_t size) {
  static uint64 txn_counter = 0;
  TimestampTz now = GetCurrentTimestamp();

  snprintf(gid, size, "orochi_2pc_%d_%lu_%lu", MyProcPid, (unsigned long)now,
           (unsigned long)++txn_counter);
}

/*
 * Execute a command on a connection and check result
 */
static bool execute_2pc_command(PGconn *conn, const char *command) {
  PGresult *res;
  bool success;

  res = PQexec(conn, command);
  success = (PQresultStatus(res) == PGRES_COMMAND_OK);

  if (!success)
    elog(WARNING, "2PC command failed: %s", PQerrorMessage(conn));

  PQclear(res);
  return success;
}

void orochi_begin_distributed_transaction(void) {
  if (current_dtxn != NULL) {
    elog(WARNING, "Nested distributed transaction detected");
    return;
  }

  current_dtxn = (DistributedTransactionState *)palloc0(
      sizeof(DistributedTransactionState));
  generate_transaction_gid(current_dtxn->gid, sizeof(current_dtxn->gid));
  current_dtxn->participants = NIL;
  current_dtxn->prepared = false;

  elog(DEBUG1, "Started distributed transaction: %s", current_dtxn->gid);
}

bool orochi_prepare_distributed_transaction(DistributedExecutorState *state) {
  ListCell *lc;
  bool all_prepared = true;
  StringInfoData cmd;
  List *shard_ids = NIL;

  if (current_dtxn == NULL) {
    elog(WARNING, "No active distributed transaction to prepare");
    return false;
  }

  /* Collect all participant nodes and shard IDs */
  foreach (lc, state->tasks) {
    RemoteTask *task = (RemoteTask *)lfirst(lc);
    int32 node_id = task->fragment->node_id;
    bool already_added = false;
    ListCell *plc;

    /* Check if node already in participants */
    foreach (plc, current_dtxn->participants) {
      if (lfirst_int(plc) == node_id) {
        already_added = true;
        break;
      }
    }

    if (!already_added) {
      current_dtxn->participants =
          lappend_int(current_dtxn->participants, node_id);
      shard_ids = lappend_int(shard_ids, (int)task->fragment->shard_id);
    }
  }

  initStringInfo(&cmd);

  /* Phase 1: PREPARE on all participants */
  foreach (lc, current_dtxn->participants) {
    int32 node_id = lfirst_int(lc);
    PGconn *conn = (PGconn *)orochi_get_worker_connection(node_id);

    if (conn == NULL) {
      elog(WARNING, "Failed to get connection for node %d during 2PC prepare",
           node_id);
      all_prepared = false;
      break;
    }

    resetStringInfo(&cmd);
    appendStringInfo(&cmd, "PREPARE TRANSACTION '%s_%d'", current_dtxn->gid,
                     node_id);

    if (!execute_2pc_command(conn, cmd.data)) {
      elog(WARNING, "PREPARE TRANSACTION failed on node %d", node_id);
      all_prepared = false;
      break;
    }

    orochi_release_worker_connection(conn);
  }

  pfree(cmd.data);

  if (all_prepared) {
    current_dtxn->prepared = true;

    /*
     * Log the prepare decision to Raft for durability
     * This ensures the commit decision survives coordinator failures
     */
    if (orochi_use_raft_consensus && orochi_raft_is_leader()) {
      if (!orochi_raft_prepare_transaction(current_dtxn->gid, shard_ids)) {
        elog(WARNING, "Failed to log prepare decision to Raft for %s",
             current_dtxn->gid);
        /* Continue anyway - Raft logging is best-effort for now */
      } else {
        elog(DEBUG1, "Prepare decision logged to Raft: %s", current_dtxn->gid);
      }
    }

    elog(DEBUG1, "All participants prepared: %s", current_dtxn->gid);
  } else {
    /* Rollback any prepared transactions */
    orochi_rollback_distributed_transaction(state);
  }

  list_free(shard_ids);
  return all_prepared;
}

void orochi_commit_distributed_transaction(DistributedExecutorState *state) {
  ListCell *lc;
  StringInfoData cmd;

  if (current_dtxn == NULL || !current_dtxn->prepared) {
    elog(WARNING, "No prepared distributed transaction to commit");
    return;
  }

  /*
   * Log the commit decision to Raft BEFORE committing on participants.
   * This ensures that if the coordinator crashes after deciding to commit,
   * the decision is durable and can be recovered from the Raft log.
   */
  if (orochi_use_raft_consensus) {
    if (orochi_raft_is_leader()) {
      if (!orochi_raft_commit_transaction(current_dtxn->gid)) {
        elog(WARNING,
             "Failed to log commit decision to Raft for %s, proceeding anyway",
             current_dtxn->gid);
      } else {
        /* Wait for Raft commit to be replicated for durability */
        if (!orochi_raft_wait_for_commit(current_dtxn->gid, 5000)) {
          elog(WARNING,
               "Raft commit replication timeout for %s, proceeding with 2PC "
               "commit",
               current_dtxn->gid);
        } else {
          elog(DEBUG1, "Commit decision replicated via Raft: %s",
               current_dtxn->gid);
        }
      }
    } else {
      /* Not the Raft leader - forward to leader or proceed with local commit */
      int32 leader_id = orochi_raft_get_leader();
      if (leader_id > 0) {
        elog(DEBUG1,
             "Not Raft leader (leader=%d), proceeding with local 2PC commit",
             leader_id);
      }
    }
  }

  initStringInfo(&cmd);

  /* Phase 2: COMMIT PREPARED on all participants */
  foreach (lc, current_dtxn->participants) {
    int32 node_id = lfirst_int(lc);
    PGconn *conn = (PGconn *)orochi_get_worker_connection(node_id);

    if (conn == NULL) {
      /* Critical: participant unreachable after prepare
       * This is a serious error - prepared transaction may be left dangling
       */
      elog(ERROR,
           "Failed to connect to node %d during 2PC commit - "
           "transaction %s_%d may be left prepared",
           node_id, current_dtxn->gid, node_id);
      continue;
    }

    resetStringInfo(&cmd);
    appendStringInfo(&cmd, "COMMIT PREPARED '%s_%d'", current_dtxn->gid,
                     node_id);

    if (!execute_2pc_command(conn, cmd.data)) {
      elog(ERROR,
           "COMMIT PREPARED failed on node %d - "
           "manual resolution may be required for %s_%d",
           node_id, current_dtxn->gid, node_id);
    }

    orochi_release_worker_connection(conn);
  }

  pfree(cmd.data);

  elog(DEBUG1, "Distributed transaction committed: %s", current_dtxn->gid);

  list_free(current_dtxn->participants);
  pfree(current_dtxn);
  current_dtxn = NULL;
}

void orochi_rollback_distributed_transaction(DistributedExecutorState *state) {
  ListCell *lc;
  StringInfoData cmd;

  if (current_dtxn == NULL)
    return;

  /*
   * Log the abort decision to Raft for durability.
   * This ensures that the abort decision is replicated across the cluster.
   */
  if (orochi_use_raft_consensus && current_dtxn->prepared) {
    if (orochi_raft_is_leader()) {
      if (!orochi_raft_abort_transaction(current_dtxn->gid)) {
        elog(WARNING, "Failed to log abort decision to Raft for %s",
             current_dtxn->gid);
      } else {
        elog(DEBUG1, "Abort decision logged to Raft: %s", current_dtxn->gid);
      }
    }
  }

  initStringInfo(&cmd);

  /* Rollback prepared transactions if any */
  if (current_dtxn->prepared) {
    foreach (lc, current_dtxn->participants) {
      int32 node_id = lfirst_int(lc);
      PGconn *conn = (PGconn *)orochi_get_worker_connection(node_id);

      if (conn == NULL) {
        elog(WARNING,
             "Failed to connect to node %d during rollback - "
             "transaction %s_%d may be left prepared",
             node_id, current_dtxn->gid, node_id);
        continue;
      }

      resetStringInfo(&cmd);
      appendStringInfo(&cmd, "ROLLBACK PREPARED '%s_%d'", current_dtxn->gid,
                       node_id);

      /* Best effort - continue even if rollback fails */
      execute_2pc_command(conn, cmd.data);

      orochi_release_worker_connection(conn);
    }
  } else {
    /* Just rollback any active transactions */
    foreach (lc, current_dtxn->participants) {
      int32 node_id = lfirst_int(lc);
      PGconn *conn = (PGconn *)orochi_get_worker_connection(node_id);

      if (conn != NULL) {
        execute_2pc_command(conn, "ROLLBACK");
        orochi_release_worker_connection(conn);
      }
    }
  }

  pfree(cmd.data);

  elog(DEBUG1, "Distributed transaction rolled back: %s", current_dtxn->gid);

  list_free(current_dtxn->participants);
  pfree(current_dtxn);
  current_dtxn = NULL;
}

/* ============================================================
 * Error Handling
 * ============================================================ */

void orochi_handle_task_failure(RemoteTask *task,
                                DistributedExecutorState *state) {
  if (orochi_is_retryable_error(task->error_message) &&
      orochi_retry_task(task)) {
    /* Task will be retried */
    return;
  }

  task->state = TASK_FAILED;
  state->failed_count++;
  state->has_error = true;
  state->error_message = task->error_message;
}

bool orochi_is_retryable_error(const char *error_message) {
  if (error_message == NULL)
    return false;

  /* Network errors are retryable */
  if (strstr(error_message, "connection") != NULL ||
      strstr(error_message, "timeout") != NULL)
    return true;

  return false;
}

void orochi_propagate_error(DistributedExecutorState *state) {
  if (state->has_error && state->error_message) {
    ereport(ERROR,
            (errcode(ERRCODE_CONNECTION_FAILURE),
             errmsg("distributed query failed: %s", state->error_message)));
  }
}
