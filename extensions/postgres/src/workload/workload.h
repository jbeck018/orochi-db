/*-------------------------------------------------------------------------
 *
 * workload.h
 *    Orochi DB workload management - resource pools and query admission
 *
 * This module provides:
 *   - Resource pool definitions (CPU, memory, I/O limits)
 *   - Query priority levels
 *   - Workload class management
 *   - Query admission control
 *   - Resource tracking per session
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_WORKLOAD_H
#define OROCHI_WORKLOAD_H

#include "postgres.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define WORKLOAD_MAX_POOLS           32
#define WORKLOAD_MAX_CLASSES         64
#define WORKLOAD_MAX_SESSIONS        1024 /* Max tracked sessions */
#define WORKLOAD_MAX_POOL_NAME       64
#define WORKLOAD_MAX_CLASS_NAME      64
#define WORKLOAD_DEFAULT_POOL_NAME   "default"
#define WORKLOAD_ADMIN_POOL_NAME     "admin"
#define WORKLOAD_QUEUE_SIZE          1024
#define WORKLOAD_DEFAULT_TIMEOUT_MS  30000 /* 30 seconds */
#define WORKLOAD_DEFAULT_CPU_PERCENT 100
#define WORKLOAD_DEFAULT_MEMORY_MB   0 /* Unlimited */
#define WORKLOAD_DEFAULT_IO_MBPS     0 /* Unlimited */
#define WORKLOAD_DEFAULT_CONCURRENCY 0 /* Unlimited */
#define WORKLOAD_SESSION_IDLE_TIMEOUT      \
    (30 * 60 * 1000000L) /* 30 min in usec \
                          */

/* ============================================================
 * Query Priority Levels
 * ============================================================ */

typedef enum QueryPriority {
    QUERY_PRIORITY_LOW = 0,     /* Background jobs, reports */
    QUERY_PRIORITY_NORMAL = 1,  /* Standard user queries */
    QUERY_PRIORITY_HIGH = 2,    /* Interactive queries */
    QUERY_PRIORITY_CRITICAL = 3 /* System/admin queries */
} QueryPriority;

/* ============================================================
 * Resource Pool State
 * ============================================================ */

typedef enum ResourcePoolState {
    POOL_STATE_ACTIVE = 0,   /* Pool is active and accepting queries */
    POOL_STATE_DRAINING = 1, /* Pool is draining, no new queries */
    POOL_STATE_DISABLED = 2  /* Pool is disabled */
} ResourcePoolState;

/* ============================================================
 * Workload Class State
 * ============================================================ */

typedef enum WorkloadClassState {
    CLASS_STATE_ACTIVE = 0,  /* Class is active */
    CLASS_STATE_DISABLED = 1 /* Class is disabled */
} WorkloadClassState;

/* ============================================================
 * Resource Limits Structure
 * ============================================================ */

typedef struct ResourceLimits {
    int cpu_percent;      /* Max CPU % (1-100, 0 = unlimited) */
    int64 memory_mb;      /* Max memory in MB (0 = unlimited) */
    int64 io_read_mbps;   /* Max read I/O in MB/s (0 = unlimited) */
    int64 io_write_mbps;  /* Max write I/O in MB/s (0 = unlimited) */
    int max_concurrency;  /* Max concurrent queries (0 = unlimited) */
    int max_queue_size;   /* Max queued queries (0 = unlimited) */
    int query_timeout_ms; /* Query timeout in ms (0 = unlimited) */
    int idle_timeout_ms;  /* Idle session timeout (0 = unlimited) */
} ResourceLimits;

/* ============================================================
 * Resource Usage Statistics
 * ============================================================ */

typedef struct ResourceUsage {
    int64 cpu_time_us;        /* Total CPU time in microseconds */
    int64 memory_bytes;       /* Current memory usage */
    int64 peak_memory_bytes;  /* Peak memory usage */
    int64 io_read_bytes;      /* Total bytes read */
    int64 io_write_bytes;     /* Total bytes written */
    int64 queries_executed;   /* Number of queries executed */
    int64 queries_queued;     /* Number of queries queued */
    int64 queries_rejected;   /* Number of queries rejected */
    TimestampTz last_updated; /* Last update timestamp */
} ResourceUsage;

/* ============================================================
 * Resource Pool Structure
 * ============================================================ */

typedef struct ResourcePool {
    int64 pool_id;                          /* Unique pool identifier */
    char pool_name[WORKLOAD_MAX_POOL_NAME]; /* Pool name */
    ResourceLimits limits;                  /* Resource limits */
    ResourceUsage usage;                    /* Current resource usage */
    ResourcePoolState state;                /* Pool state */
    int active_queries;                     /* Currently running queries */
    int queued_queries;                     /* Currently queued queries */
    int priority_weight;                    /* Priority weight for scheduling */
    bool is_internal;                       /* Is this an internal pool? */
    TimestampTz created_at;                 /* Creation timestamp */
    LWLock *lock;                           /* Lock for this pool */
} ResourcePool;

/* ============================================================
 * Workload Class Structure
 * ============================================================ */

typedef struct WorkloadClass {
    int64 class_id;                           /* Unique class identifier */
    char class_name[WORKLOAD_MAX_CLASS_NAME]; /* Class name */
    int64 pool_id;                            /* Associated resource pool */
    QueryPriority default_priority;           /* Default query priority */
    ResourceLimits limits;                    /* Per-query limits (overrides pool) */
    WorkloadClassState state;                 /* Class state */
    int64 queries_executed;                   /* Total queries executed */
    int64 queries_rejected;                   /* Total queries rejected */
    TimestampTz created_at;                   /* Creation timestamp */
} WorkloadClass;

/* ============================================================
 * Query Entry Structure (for queue)
 * ============================================================ */

typedef struct QueryEntry {
    int backend_pid;          /* Backend process ID */
    int64 pool_id;            /* Resource pool ID */
    int64 class_id;           /* Workload class ID */
    QueryPriority priority;   /* Query priority */
    TimestampTz enqueue_time; /* When query was enqueued */
    TimestampTz start_time;   /* When query started (0 if queued) */
    int64 estimated_cost;     /* Estimated query cost */
    int64 memory_requested;   /* Requested memory in bytes */
    bool is_active;           /* Is query currently running? */
    char query_id[64];        /* Query identifier */
} QueryEntry;

/* ============================================================
 * Session Resource Tracker
 * ============================================================ */

typedef struct SessionResourceTracker {
    int backend_pid;           /* Backend process ID */
    int64 pool_id;             /* Assigned resource pool */
    int64 class_id;            /* Assigned workload class */
    ResourceUsage usage;       /* Session resource usage */
    int active_queries;        /* Active queries in session */
    TimestampTz session_start; /* Session start time */
    TimestampTz last_activity; /* Last activity timestamp */
    bool is_active;            /* Is session active? */
} SessionResourceTracker;

/* ============================================================
 * Shared Memory State
 * ============================================================ */

typedef struct WorkloadSharedState {
    LWLock *main_lock;                                      /* Main workload lock */
    LWLock *session_lock;                                   /* Lock for session tracker */
    int num_pools;                                          /* Number of resource pools */
    int num_classes;                                        /* Number of workload classes */
    int num_sessions;                                       /* Number of tracked sessions */
    int total_active_queries;                               /* Total active queries */
    int total_queued_queries;                               /* Total queued queries */
    ResourcePool pools[WORKLOAD_MAX_POOLS];                 /* Resource pools */
    WorkloadClass classes[WORKLOAD_MAX_CLASSES];            /* Workload classes */
    QueryEntry queue[WORKLOAD_QUEUE_SIZE];                  /* Query queue */
    SessionResourceTracker sessions[WORKLOAD_MAX_SESSIONS]; /* Session trackers */
    int queue_head;                                         /* Queue head index */
    int queue_tail;                                         /* Queue tail index */
    bool is_initialized;                                    /* Initialization flag */
} WorkloadSharedState;

/* ============================================================
 * Function Prototypes - Initialization
 * ============================================================ */

/* Module initialization */
extern void orochi_workload_init(void);
extern void orochi_workload_shmem_startup(void);
extern Size orochi_workload_shmem_size(void);

/* ============================================================
 * Function Prototypes - Resource Pool Management
 * ============================================================ */

/* Pool lifecycle */
extern int64 orochi_create_resource_pool(const char *pool_name, ResourceLimits *limits,
                                         int priority_weight);
extern bool orochi_alter_resource_pool(int64 pool_id, ResourceLimits *new_limits,
                                       int priority_weight);
extern bool orochi_drop_resource_pool(int64 pool_id, bool if_exists);
extern bool orochi_enable_resource_pool(int64 pool_id);
extern bool orochi_disable_resource_pool(int64 pool_id);

/* Pool queries */
extern ResourcePool *orochi_get_resource_pool(int64 pool_id);
extern ResourcePool *orochi_get_resource_pool_by_name(const char *pool_name);
extern List *orochi_list_resource_pools(void);
extern ResourceUsage *orochi_get_pool_usage(int64 pool_id);

/* ============================================================
 * Function Prototypes - Workload Class Management
 * ============================================================ */

/* Class lifecycle */
extern int64 orochi_create_workload_class(const char *class_name, int64 pool_id,
                                          QueryPriority default_priority, ResourceLimits *limits);
extern bool orochi_alter_workload_class(int64 class_id, int64 pool_id, QueryPriority priority,
                                        ResourceLimits *limits);
extern bool orochi_drop_workload_class(int64 class_id, bool if_exists);

/* Class queries */
extern WorkloadClass *orochi_get_workload_class(int64 class_id);
extern WorkloadClass *orochi_get_workload_class_by_name(const char *class_name);
extern List *orochi_list_workload_classes(void);

/* Class assignment */
extern bool orochi_assign_workload_class(int backend_pid, int64 class_id);
extern bool orochi_assign_workload_class_by_name(int backend_pid, const char *class_name);
extern int64 orochi_get_session_class_id(int backend_pid);

/* ============================================================
 * Function Prototypes - Query Admission Control
 * ============================================================ */

/* Query admission */
extern bool orochi_admit_query(int backend_pid, int64 pool_id, QueryPriority priority,
                               int64 estimated_cost, int64 memory_requested);
extern bool orochi_queue_query(int backend_pid, int64 pool_id, QueryPriority priority,
                               int64 estimated_cost);
extern void orochi_release_query(int backend_pid);
extern bool orochi_dequeue_next_query(int64 pool_id, int *backend_pid);

/* Queue management */
extern int orochi_get_queue_length(int64 pool_id);
extern List *orochi_get_queued_queries(int64 pool_id);
extern bool orochi_cancel_queued_query(int backend_pid);
extern void orochi_drain_pool_queue(int64 pool_id);

/* ============================================================
 * Function Prototypes - Session Tracking
 * ============================================================ */

/* Session management */
extern bool orochi_register_session(int backend_pid, int64 pool_id, int64 class_id);
extern void orochi_unregister_session(int backend_pid);
extern SessionResourceTracker *orochi_get_session_tracker(int backend_pid);
extern void orochi_update_session_activity(int backend_pid);

/* Session resource tracking */
extern void orochi_track_session_cpu(int backend_pid, int64 cpu_time_us);
extern void orochi_track_session_memory(int backend_pid, int64 memory_bytes);
extern void orochi_track_session_io(int backend_pid, int64 read_bytes, int64 write_bytes);

/* ============================================================
 * Function Prototypes - Resource Governor (from resource_governor.c)
 * ============================================================ */

/* CPU tracking */
extern void orochi_governor_track_cpu(int backend_pid, int64 cpu_time_us);
extern bool orochi_governor_check_cpu_limit(int backend_pid);
extern int64 orochi_governor_get_cpu_usage(int backend_pid);

/* Memory enforcement */
extern bool orochi_governor_request_memory(int backend_pid, int64 bytes);
extern void orochi_governor_release_memory(int backend_pid, int64 bytes);
extern bool orochi_governor_check_memory_limit(int backend_pid);
extern int64 orochi_governor_get_memory_usage(int backend_pid);

/* I/O throttling */
extern bool orochi_governor_check_io_limit(int backend_pid, int64 bytes, bool is_write);
extern void orochi_governor_track_io(int backend_pid, int64 bytes, bool is_write);
extern void orochi_governor_throttle_io(int backend_pid, int delay_ms);

/* Concurrency limits */
extern bool orochi_governor_can_start_query(int64 pool_id);
extern void orochi_governor_query_started(int64 pool_id);
extern void orochi_governor_query_finished(int64 pool_id);
extern int orochi_governor_get_active_queries(int64 pool_id);

/* Query timeout */
extern bool orochi_governor_check_timeout(int backend_pid);
extern void orochi_governor_set_timeout(int backend_pid, int timeout_ms);
extern void orochi_governor_clear_timeout(int backend_pid);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_resource_pool_sql(PG_FUNCTION_ARGS);
extern Datum orochi_alter_resource_pool_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_resource_pool_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_resource_pools_sql(PG_FUNCTION_ARGS);
extern Datum orochi_pool_stats_sql(PG_FUNCTION_ARGS);

extern Datum orochi_create_workload_class_sql(PG_FUNCTION_ARGS);
extern Datum orochi_alter_workload_class_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_workload_class_sql(PG_FUNCTION_ARGS);
extern Datum orochi_assign_workload_class_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_workload_classes_sql(PG_FUNCTION_ARGS);

extern Datum orochi_workload_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_session_resources_sql(PG_FUNCTION_ARGS);
extern Datum orochi_queued_queries_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Name conversion */
extern const char *orochi_priority_name(QueryPriority priority);
extern QueryPriority orochi_parse_priority(const char *name);
extern const char *orochi_pool_state_name(ResourcePoolState state);
extern ResourcePoolState orochi_parse_pool_state(const char *name);

/* Limit helpers */
extern ResourceLimits *orochi_create_default_limits(void);
extern void orochi_copy_limits(ResourceLimits *dst, ResourceLimits *src);
extern bool orochi_validate_limits(ResourceLimits *limits);

/* Debug/monitoring */
extern void orochi_log_pool_stats(int64 pool_id);
extern void orochi_log_workload_status(void);

/* ============================================================
 * GUC Variables
 * ============================================================ */

extern bool orochi_workload_enabled;
extern int orochi_default_pool_concurrency;
extern int orochi_default_query_timeout_ms;
extern int orochi_workload_check_interval_ms;
extern bool orochi_workload_log_rejections;

#endif /* OROCHI_WORKLOAD_H */
