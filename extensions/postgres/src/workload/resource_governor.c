/*-------------------------------------------------------------------------
 *
 * resource_governor.c
 *    Orochi DB resource governor - enforcement of resource limits
 *
 * This module implements:
 *   - CPU time tracking and limiting
 *   - Memory limit enforcement
 *   - I/O bandwidth throttling
 *   - Concurrent query limits
 *   - Query timeout management
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "fmgr.h"
#include "miscadmin.h"
#include "postgres.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

#include "workload.h"

/* ============================================================
 * Session Resource State (per-backend tracking)
 * ============================================================ */

#define MAX_TRACKED_SESSIONS 1024

typedef struct SessionResourceState {
  int backend_pid; /* Backend process ID */
  int64 pool_id;   /* Assigned resource pool */
  int64 class_id;  /* Assigned workload class */

  /* CPU tracking */
  int64 cpu_time_us;            /* Accumulated CPU time */
  int64 cpu_limit_us;           /* CPU time limit (0 = unlimited) */
  TimestampTz cpu_window_start; /* Start of current window */

  /* Memory tracking */
  int64 memory_bytes;       /* Current memory usage */
  int64 memory_limit_bytes; /* Memory limit (0 = unlimited) */
  int64 peak_memory_bytes;  /* Peak memory usage */

  /* I/O tracking */
  int64 io_read_bytes;         /* Total bytes read */
  int64 io_write_bytes;        /* Total bytes written */
  int64 io_read_limit_bps;     /* Read limit in bytes/sec */
  int64 io_write_limit_bps;    /* Write limit in bytes/sec */
  TimestampTz io_window_start; /* Start of I/O tracking window */
  int64 io_read_window;        /* Bytes read in current window */
  int64 io_write_window;       /* Bytes written in current window */

  /* Query tracking */
  int active_queries;           /* Active queries in session */
  TimestampTz query_start_time; /* Current query start time */
  int query_timeout_ms;         /* Query timeout (0 = unlimited) */

  /* Session state */
  bool is_active;            /* Session is active */
  TimestampTz last_activity; /* Last activity time */
} SessionResourceState;

/* ============================================================
 * Governor Shared State
 * ============================================================ */

typedef struct GovernorSharedState {
  LWLock *lock; /* Main lock */
  SessionResourceState sessions[MAX_TRACKED_SESSIONS];
  int num_sessions;         /* Number of active sessions */
  int64 total_cpu_time_us;  /* Total CPU time across all sessions */
  int64 total_memory_bytes; /* Total memory across all sessions */
  int64 total_io_bytes;     /* Total I/O across all sessions */
} GovernorSharedState;

static GovernorSharedState *GovernorState = NULL;

/* I/O window size for rate limiting (1 second) */
#define IO_WINDOW_MS 1000

/* CPU window size (1 second) */
#define CPU_WINDOW_MS 1000

/* ============================================================
 * Internal Function Declarations
 * ============================================================ */

static SessionResourceState *find_session_state(int backend_pid);
static SessionResourceState *find_or_create_session_state(int backend_pid);
static void cleanup_inactive_sessions(void);
static int64 get_pool_cpu_limit(int64 pool_id);
static int64 get_pool_memory_limit(int64 pool_id);
static int64 get_pool_io_limit(int64 pool_id, bool is_write);
static int get_pool_concurrency_limit(int64 pool_id);

/* ============================================================
 * Initialization
 * ============================================================ */

/*
 * orochi_governor_init
 *    Initialize the resource governor (called from workload module)
 */
void orochi_governor_init(void) {
  bool found;

  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  GovernorState = (GovernorSharedState *)ShmemInitStruct(
      "Orochi Resource Governor", sizeof(GovernorSharedState), &found);

  if (!found) {
    memset(GovernorState, 0, sizeof(GovernorSharedState));
    GovernorState->lock = &(GetNamedLWLockTranche("orochi_workload")->lock);
    GovernorState->num_sessions = 0;
    GovernorState->total_cpu_time_us = 0;
    GovernorState->total_memory_bytes = 0;
    GovernorState->total_io_bytes = 0;

    for (int i = 0; i < MAX_TRACKED_SESSIONS; i++) {
      GovernorState->sessions[i].backend_pid = 0;
      GovernorState->sessions[i].is_active = false;
    }

    elog(DEBUG1, "Resource governor initialized");
  }

  LWLockRelease(AddinShmemInitLock);
}

/* ============================================================
 * Session State Management
 * ============================================================ */

/*
 * find_session_state
 *    Find session state for a backend
 */
static SessionResourceState *find_session_state(int backend_pid) {
  for (int i = 0; i < MAX_TRACKED_SESSIONS; i++) {
    if (GovernorState->sessions[i].backend_pid == backend_pid &&
        GovernorState->sessions[i].is_active) {
      return &GovernorState->sessions[i];
    }
  }
  return NULL;
}

/*
 * find_or_create_session_state
 *    Find or create session state for a backend
 */
static SessionResourceState *find_or_create_session_state(int backend_pid) {
  SessionResourceState *session;
  int free_slot = -1;

  /* First, try to find existing session */
  for (int i = 0; i < MAX_TRACKED_SESSIONS; i++) {
    if (GovernorState->sessions[i].backend_pid == backend_pid &&
        GovernorState->sessions[i].is_active) {
      return &GovernorState->sessions[i];
    }
    if (free_slot < 0 && !GovernorState->sessions[i].is_active) {
      free_slot = i;
    }
  }

  /* Create new session state */
  if (free_slot >= 0) {
    session = &GovernorState->sessions[free_slot];
    memset(session, 0, sizeof(SessionResourceState));
    session->backend_pid = backend_pid;
    session->is_active = true;
    session->cpu_window_start = GetCurrentTimestamp();
    session->io_window_start = GetCurrentTimestamp();
    session->last_activity = GetCurrentTimestamp();
    GovernorState->num_sessions++;
    return session;
  }

  /* No free slots - try to clean up inactive sessions */
  cleanup_inactive_sessions();

  /* Try again */
  for (int i = 0; i < MAX_TRACKED_SESSIONS; i++) {
    if (!GovernorState->sessions[i].is_active) {
      session = &GovernorState->sessions[i];
      memset(session, 0, sizeof(SessionResourceState));
      session->backend_pid = backend_pid;
      session->is_active = true;
      session->cpu_window_start = GetCurrentTimestamp();
      session->io_window_start = GetCurrentTimestamp();
      session->last_activity = GetCurrentTimestamp();
      GovernorState->num_sessions++;
      return session;
    }
  }

  elog(WARNING, "Cannot track session for backend %d: no free slots",
       backend_pid);
  return NULL;
}

/*
 * cleanup_inactive_sessions
 *    Remove stale session tracking entries
 */
static void cleanup_inactive_sessions(void) {
  TimestampTz now = GetCurrentTimestamp();
  TimestampTz cutoff;

  /* Sessions inactive for more than 5 minutes are cleaned up */
  cutoff = TimestampTzPlusMilliseconds(now, -300000);

  for (int i = 0; i < MAX_TRACKED_SESSIONS; i++) {
    SessionResourceState *session = &GovernorState->sessions[i];
    if (session->is_active && session->last_activity < cutoff &&
        session->active_queries == 0) {
      /* Update totals before clearing */
      GovernorState->total_memory_bytes -= session->memory_bytes;

      session->is_active = false;
      session->backend_pid = 0;
      GovernorState->num_sessions--;

      elog(DEBUG2, "Cleaned up stale session tracking entry");
    }
  }
}

/* ============================================================
 * CPU Tracking and Enforcement
 * ============================================================ */

/*
 * orochi_governor_track_cpu
 *    Track CPU time for a backend
 */
void orochi_governor_track_cpu(int backend_pid, int64 cpu_time_us) {
  SessionResourceState *session;

  if (GovernorState == NULL)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_or_create_session_state(backend_pid);
  if (session != NULL) {
    TimestampTz now = GetCurrentTimestamp();
    int64 elapsed_ms;

    /* Check if we need to reset the window */
    elapsed_ms = (now - session->cpu_window_start) / 1000;
    if (elapsed_ms >= CPU_WINDOW_MS) {
      /* New window - don't reset total, just the window */
      session->cpu_window_start = now;
    }

    session->cpu_time_us += cpu_time_us;
    session->last_activity = now;
    GovernorState->total_cpu_time_us += cpu_time_us;
  }

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_check_cpu_limit
 *    Check if backend has exceeded CPU limit
 */
bool orochi_governor_check_cpu_limit(int backend_pid) {
  SessionResourceState *session;
  bool exceeded = false;

  if (GovernorState == NULL)
    return false;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL && session->cpu_limit_us > 0) {
    exceeded = (session->cpu_time_us >= session->cpu_limit_us);
  }

  LWLockRelease(GovernorState->lock);

  return exceeded;
}

/*
 * orochi_governor_get_cpu_usage
 *    Get accumulated CPU time for a backend
 */
int64 orochi_governor_get_cpu_usage(int backend_pid) {
  SessionResourceState *session;
  int64 cpu_time = 0;

  if (GovernorState == NULL)
    return 0;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    cpu_time = session->cpu_time_us;
  }

  LWLockRelease(GovernorState->lock);

  return cpu_time;
}

/* ============================================================
 * Memory Enforcement
 * ============================================================ */

/*
 * orochi_governor_request_memory
 *    Request memory allocation - returns false if would exceed limit
 */
bool orochi_governor_request_memory(int backend_pid, int64 bytes) {
  SessionResourceState *session;
  bool allowed = true;

  if (GovernorState == NULL || bytes <= 0)
    return true;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_or_create_session_state(backend_pid);
  if (session != NULL) {
    /* Check against session limit */
    if (session->memory_limit_bytes > 0) {
      if (session->memory_bytes + bytes > session->memory_limit_bytes) {
        allowed = false;
        elog(DEBUG1,
             "Memory request denied for backend %d: "
             "would exceed limit (%ld + %ld > %ld)",
             backend_pid, session->memory_bytes, bytes,
             session->memory_limit_bytes);
      }
    }

    /* Check against pool limit */
    if (allowed && session->pool_id > 0) {
      int64 pool_limit = get_pool_memory_limit(session->pool_id);
      if (pool_limit > 0) {
        /* Would need to sum all sessions in this pool */
        /* For now, just check individual session */
      }
    }

    if (allowed) {
      session->memory_bytes += bytes;
      if (session->memory_bytes > session->peak_memory_bytes)
        session->peak_memory_bytes = session->memory_bytes;
      GovernorState->total_memory_bytes += bytes;
      session->last_activity = GetCurrentTimestamp();
    }
  }

  LWLockRelease(GovernorState->lock);

  return allowed;
}

/*
 * orochi_governor_release_memory
 *    Release memory from tracking
 */
void orochi_governor_release_memory(int backend_pid, int64 bytes) {
  SessionResourceState *session;

  if (GovernorState == NULL || bytes <= 0)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    session->memory_bytes -= bytes;
    if (session->memory_bytes < 0)
      session->memory_bytes = 0;
    GovernorState->total_memory_bytes -= bytes;
    if (GovernorState->total_memory_bytes < 0)
      GovernorState->total_memory_bytes = 0;
    session->last_activity = GetCurrentTimestamp();
  }

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_check_memory_limit
 *    Check if backend has exceeded memory limit
 */
bool orochi_governor_check_memory_limit(int backend_pid) {
  SessionResourceState *session;
  bool exceeded = false;

  if (GovernorState == NULL)
    return false;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL && session->memory_limit_bytes > 0) {
    exceeded = (session->memory_bytes >= session->memory_limit_bytes);
  }

  LWLockRelease(GovernorState->lock);

  return exceeded;
}

/*
 * orochi_governor_get_memory_usage
 *    Get current memory usage for a backend
 */
int64 orochi_governor_get_memory_usage(int backend_pid) {
  SessionResourceState *session;
  int64 memory = 0;

  if (GovernorState == NULL)
    return 0;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    memory = session->memory_bytes;
  }

  LWLockRelease(GovernorState->lock);

  return memory;
}

/* ============================================================
 * I/O Throttling
 * ============================================================ */

/*
 * orochi_governor_check_io_limit
 *    Check if I/O operation would exceed rate limit
 */
bool orochi_governor_check_io_limit(int backend_pid, int64 bytes,
                                    bool is_write) {
  SessionResourceState *session;
  bool allowed = true;

  if (GovernorState == NULL || bytes <= 0)
    return true;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    TimestampTz now = GetCurrentTimestamp();
    int64 elapsed_ms = (now - session->io_window_start) / 1000;
    int64 limit_bps =
        is_write ? session->io_write_limit_bps : session->io_read_limit_bps;

    if (limit_bps > 0) {
      int64 window_bytes =
          is_write ? session->io_write_window : session->io_read_window;
      int64 allowed_bytes;

      /* Calculate how many bytes are allowed in the current window */
      if (elapsed_ms <= 0)
        elapsed_ms = 1;
      allowed_bytes = (limit_bps * elapsed_ms) / 1000;

      if (window_bytes + bytes > allowed_bytes) {
        allowed = false;
      }
    }
  }

  LWLockRelease(GovernorState->lock);

  return allowed;
}

/*
 * orochi_governor_track_io
 *    Track I/O operation
 */
void orochi_governor_track_io(int backend_pid, int64 bytes, bool is_write) {
  SessionResourceState *session;

  if (GovernorState == NULL || bytes <= 0)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_or_create_session_state(backend_pid);
  if (session != NULL) {
    TimestampTz now = GetCurrentTimestamp();
    int64 elapsed_ms = (now - session->io_window_start) / 1000;

    /* Reset window if needed */
    if (elapsed_ms >= IO_WINDOW_MS) {
      session->io_window_start = now;
      session->io_read_window = 0;
      session->io_write_window = 0;
    }

    /* Update counters */
    if (is_write) {
      session->io_write_bytes += bytes;
      session->io_write_window += bytes;
    } else {
      session->io_read_bytes += bytes;
      session->io_read_window += bytes;
    }

    GovernorState->total_io_bytes += bytes;
    session->last_activity = now;
  }

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_throttle_io
 *    Delay backend to enforce I/O rate limit
 */
void orochi_governor_throttle_io(int backend_pid, int delay_ms) {
  if (delay_ms > 0 && delay_ms < 10000) /* Max 10 second delay */
  {
    elog(DEBUG2, "Throttling I/O for backend %d: %d ms delay", backend_pid,
         delay_ms);
    pg_usleep(delay_ms * 1000L);
  }
}

/* ============================================================
 * Concurrency Limits
 * ============================================================ */

/*
 * orochi_governor_can_start_query
 *    Check if pool has capacity for another query
 */
bool orochi_governor_can_start_query(int64 pool_id) {
  ResourcePool *pool;
  bool can_start = true;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    if (pool->limits.max_concurrency > 0) {
      can_start = (pool->active_queries < pool->limits.max_concurrency);
    }
    pfree(pool);
  }

  return can_start;
}

/*
 * orochi_governor_query_started
 *    Record that a query started in a pool
 */
void orochi_governor_query_started(int64 pool_id) {
  /* This is handled in workload.c pool management */
  elog(DEBUG2, "Query started in pool %ld", pool_id);
}

/*
 * orochi_governor_query_finished
 *    Record that a query finished in a pool
 */
void orochi_governor_query_finished(int64 pool_id) {
  /* This is handled in workload.c pool management */
  elog(DEBUG2, "Query finished in pool %ld", pool_id);
}

/*
 * orochi_governor_get_active_queries
 *    Get number of active queries in a pool
 */
int orochi_governor_get_active_queries(int64 pool_id) {
  ResourcePool *pool;
  int count = 0;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    count = pool->active_queries;
    pfree(pool);
  }

  return count;
}

/* ============================================================
 * Query Timeout Management
 * ============================================================ */

/*
 * orochi_governor_check_timeout
 *    Check if current query has exceeded timeout
 */
bool orochi_governor_check_timeout(int backend_pid) {
  SessionResourceState *session;
  bool timed_out = false;

  if (GovernorState == NULL)
    return false;

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  session = find_session_state(backend_pid);
  if (session != NULL && session->query_timeout_ms > 0 &&
      session->query_start_time != 0) {
    TimestampTz now = GetCurrentTimestamp();
    int64 elapsed_ms = (now - session->query_start_time) / 1000;

    if (elapsed_ms >= session->query_timeout_ms) {
      timed_out = true;
      elog(DEBUG1, "Query timeout for backend %d: %ld ms >= %d ms", backend_pid,
           elapsed_ms, session->query_timeout_ms);
    }
  }

  LWLockRelease(GovernorState->lock);

  return timed_out;
}

/*
 * orochi_governor_set_timeout
 *    Set query timeout for a session
 */
void orochi_governor_set_timeout(int backend_pid, int timeout_ms) {
  SessionResourceState *session;

  if (GovernorState == NULL)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_or_create_session_state(backend_pid);
  if (session != NULL) {
    session->query_timeout_ms = timeout_ms;
    session->query_start_time = GetCurrentTimestamp();
  }

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_clear_timeout
 *    Clear query timeout (query completed)
 */
void orochi_governor_clear_timeout(int backend_pid) {
  SessionResourceState *session;

  if (GovernorState == NULL)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    session->query_start_time = 0;
  }

  LWLockRelease(GovernorState->lock);
}

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * get_pool_cpu_limit
 *    Get CPU limit for a pool (in microseconds per second)
 */
static int64 get_pool_cpu_limit(int64 pool_id) {
  ResourcePool *pool;
  int64 limit = 0;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    /* Convert CPU percent to microseconds per second */
    if (pool->limits.cpu_percent > 0 && pool->limits.cpu_percent < 100) {
      limit = (pool->limits.cpu_percent * 1000000L) / 100;
    }
    pfree(pool);
  }

  return limit;
}

/*
 * get_pool_memory_limit
 *    Get memory limit for a pool (in bytes)
 */
static int64 get_pool_memory_limit(int64 pool_id) {
  ResourcePool *pool;
  int64 limit = 0;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    if (pool->limits.memory_mb > 0) {
      limit = pool->limits.memory_mb * 1024 * 1024;
    }
    pfree(pool);
  }

  return limit;
}

/*
 * get_pool_io_limit
 *    Get I/O limit for a pool (in bytes per second)
 */
static int64 get_pool_io_limit(int64 pool_id, bool is_write) {
  ResourcePool *pool;
  int64 limit = 0;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    if (is_write && pool->limits.io_write_mbps > 0) {
      limit = pool->limits.io_write_mbps * 1024 * 1024;
    } else if (!is_write && pool->limits.io_read_mbps > 0) {
      limit = pool->limits.io_read_mbps * 1024 * 1024;
    }
    pfree(pool);
  }

  return limit;
}

/*
 * get_pool_concurrency_limit
 *    Get concurrency limit for a pool
 */
static int get_pool_concurrency_limit(int64 pool_id) {
  ResourcePool *pool;
  int limit = 0;

  pool = orochi_get_resource_pool(pool_id);
  if (pool != NULL) {
    limit = pool->limits.max_concurrency;
    pfree(pool);
  }

  return limit;
}

/* ============================================================
 * Monitoring Functions
 * ============================================================ */

/*
 * orochi_governor_get_stats
 *    Get overall governor statistics
 */
void orochi_governor_get_stats(int64 *total_cpu_us, int64 *total_memory_bytes,
                               int64 *total_io_bytes, int *num_sessions) {
  if (GovernorState == NULL) {
    *total_cpu_us = 0;
    *total_memory_bytes = 0;
    *total_io_bytes = 0;
    *num_sessions = 0;
    return;
  }

  LWLockAcquire(GovernorState->lock, LW_SHARED);

  *total_cpu_us = GovernorState->total_cpu_time_us;
  *total_memory_bytes = GovernorState->total_memory_bytes;
  *total_io_bytes = GovernorState->total_io_bytes;
  *num_sessions = GovernorState->num_sessions;

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_reset_session_stats
 *    Reset statistics for a session
 */
void orochi_governor_reset_session_stats(int backend_pid) {
  SessionResourceState *session;

  if (GovernorState == NULL)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_session_state(backend_pid);
  if (session != NULL) {
    /* Subtract from totals */
    GovernorState->total_cpu_time_us -= session->cpu_time_us;
    GovernorState->total_memory_bytes -= session->memory_bytes;

    /* Reset session stats */
    session->cpu_time_us = 0;
    session->memory_bytes = 0;
    session->peak_memory_bytes = 0;
    session->io_read_bytes = 0;
    session->io_write_bytes = 0;
    session->cpu_window_start = GetCurrentTimestamp();
    session->io_window_start = GetCurrentTimestamp();
    session->io_read_window = 0;
    session->io_write_window = 0;
  }

  LWLockRelease(GovernorState->lock);
}

/*
 * orochi_governor_set_session_limits
 *    Set resource limits for a session
 */
void orochi_governor_set_session_limits(int backend_pid, int64 pool_id,
                                        int64 class_id,
                                        ResourceLimits *limits) {
  SessionResourceState *session;

  if (GovernorState == NULL)
    return;

  LWLockAcquire(GovernorState->lock, LW_EXCLUSIVE);

  session = find_or_create_session_state(backend_pid);
  if (session != NULL) {
    session->pool_id = pool_id;
    session->class_id = class_id;

    if (limits != NULL) {
      /* Convert limits to internal representation */
      session->cpu_limit_us =
          (limits->cpu_percent > 0 && limits->cpu_percent < 100)
              ? (limits->cpu_percent * 1000000L) / 100
              : 0;
      session->memory_limit_bytes = limits->memory_mb * 1024 * 1024;
      session->io_read_limit_bps = limits->io_read_mbps * 1024 * 1024;
      session->io_write_limit_bps = limits->io_write_mbps * 1024 * 1024;
      session->query_timeout_ms = limits->query_timeout_ms;
    }
  }

  LWLockRelease(GovernorState->lock);
}
