/*-------------------------------------------------------------------------
 *
 * workload.c
 *    Orochi DB workload management implementation
 *
 * This module implements:
 *   - Resource pool creation and management
 *   - Query admission control
 *   - Resource tracking per session
 *   - Query queuing when resources exhausted
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postgres.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "workload.h"

PG_MODULE_MAGIC;

/* ============================================================
 * GUC Variables
 * ============================================================ */

bool orochi_workload_enabled = true;
int orochi_default_pool_concurrency = 0; /* 0 = unlimited */
int orochi_default_query_timeout_ms = 30000;
int orochi_workload_check_interval_ms = 1000;
bool orochi_workload_log_rejections = true;

/* ============================================================
 * Shared Memory State
 * ============================================================ */

static WorkloadSharedState *WorkloadState = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ============================================================
 * Internal Function Declarations
 * ============================================================ */

static void init_default_pools(void);
static int64 allocate_pool_id(void);
static int64 allocate_class_id(void);
static ResourcePool *find_free_pool_slot(void);
static WorkloadClass *find_free_class_slot(void);
static bool check_pool_resources(ResourcePool *pool, int64 memory_requested);
static void update_pool_usage(ResourcePool *pool, ResourceUsage *delta);
static QueryEntry *find_queue_slot(void);
static void remove_from_queue(int backend_pid);
static int compare_query_priority(const void *a, const void *b);

/* Session tracking internal functions */
static uint32 session_hash_slot(int backend_pid);
static SessionResourceTracker *find_session_slot(int backend_pid);
static SessionResourceTracker *find_free_session_slot(int backend_pid);

/* ============================================================
 * Initialization
 * ============================================================ */

/*
 * orochi_workload_shmem_size
 *    Calculate shared memory size needed for workload management
 */
Size orochi_workload_shmem_size(void) {
  Size size = 0;

  size = add_size(size, sizeof(WorkloadSharedState));

  return size;
}

/*
 * orochi_workload_shmem_startup
 *    Initialize shared memory for workload management
 */
void orochi_workload_shmem_startup(void) {
  bool found;

  if (prev_shmem_startup_hook)
    prev_shmem_startup_hook();

  /* Create or attach to shared memory segment */
  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  WorkloadState = (WorkloadSharedState *)ShmemInitStruct(
      "Orochi Workload State", sizeof(WorkloadSharedState), &found);

  if (!found) {
    /* Initialize the shared state */
    memset(WorkloadState, 0, sizeof(WorkloadSharedState));

    WorkloadState->main_lock =
        &(GetNamedLWLockTranche("orochi_workload")->lock);
    WorkloadState->session_lock =
        &(GetNamedLWLockTranche("orochi_workload")[1].lock);
    WorkloadState->num_pools = 0;
    WorkloadState->num_classes = 0;
    WorkloadState->num_sessions = 0;
    WorkloadState->total_active_queries = 0;
    WorkloadState->total_queued_queries = 0;
    WorkloadState->queue_head = 0;
    WorkloadState->queue_tail = 0;

    /* Initialize pool locks */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      WorkloadState->pools[i].pool_id = 0;
      WorkloadState->pools[i].lock = NULL;
    }

    /* Initialize queue entries */
    for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
      WorkloadState->queue[i].backend_pid = 0;
      WorkloadState->queue[i].is_active = false;
    }

    /* Initialize session tracker entries */
    for (int i = 0; i < WORKLOAD_MAX_SESSIONS; i++) {
      WorkloadState->sessions[i].backend_pid = 0;
      WorkloadState->sessions[i].pool_id = 0;
      WorkloadState->sessions[i].class_id = 0;
      WorkloadState->sessions[i].is_active = false;
      memset(&WorkloadState->sessions[i].usage, 0, sizeof(ResourceUsage));
    }

    WorkloadState->is_initialized = true;

    /* Create default pools */
    init_default_pools();

    elog(LOG, "Orochi workload management initialized");
  }

  LWLockRelease(AddinShmemInitLock);
}

/*
 * orochi_workload_init
 *    Module initialization entry point
 */
void orochi_workload_init(void) {
  if (!process_shared_preload_libraries_in_progress)
    return;

#if PG_VERSION_NUM < 150000
  /* Pre-PG15: Request shared memory directly */
  RequestAddinShmemSpace(orochi_workload_shmem_size());
  RequestNamedLWLockTranche("orochi_workload", WORKLOAD_MAX_POOLS + 1);
#endif
  /* Note: PG15+ shared memory is requested via main init.c shmem_request_hook
   */

  /* Install shmem startup hook */
  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook = orochi_workload_shmem_startup;

  /* Define GUCs */
  DefineCustomBoolVariable(
      "orochi.workload_enabled", "Enable workload management", NULL,
      &orochi_workload_enabled, true, PGC_SUSET, 0, NULL, NULL, NULL);

  DefineCustomIntVariable("orochi.default_pool_concurrency",
                          "Default concurrent queries per pool (0 = unlimited)",
                          NULL, &orochi_default_pool_concurrency, 0, 0, 10000,
                          PGC_SUSET, 0, NULL, NULL, NULL);

  DefineCustomIntVariable("orochi.default_query_timeout_ms",
                          "Default query timeout in milliseconds", NULL,
                          &orochi_default_query_timeout_ms, 30000, 0, 3600000,
                          PGC_SUSET, 0, NULL, NULL, NULL);

  DefineCustomBoolVariable(
      "orochi.workload_log_rejections", "Log rejected queries", NULL,
      &orochi_workload_log_rejections, true, PGC_SUSET, 0, NULL, NULL, NULL);
}

/*
 * init_default_pools
 *    Create the default and admin resource pools
 */
static void init_default_pools(void) {
  ResourceLimits default_limits;
  ResourceLimits admin_limits;

  /* Default pool limits */
  memset(&default_limits, 0, sizeof(ResourceLimits));
  default_limits.cpu_percent = WORKLOAD_DEFAULT_CPU_PERCENT;
  default_limits.memory_mb = WORKLOAD_DEFAULT_MEMORY_MB;
  default_limits.max_concurrency = orochi_default_pool_concurrency;
  default_limits.query_timeout_ms = orochi_default_query_timeout_ms;

  /* Create default pool */
  orochi_create_resource_pool(WORKLOAD_DEFAULT_POOL_NAME, &default_limits, 50);

  /* Admin pool limits - higher priority, fewer restrictions */
  memset(&admin_limits, 0, sizeof(ResourceLimits));
  admin_limits.cpu_percent = 100;
  admin_limits.memory_mb = 0;        /* Unlimited */
  admin_limits.max_concurrency = 0;  /* Unlimited */
  admin_limits.query_timeout_ms = 0; /* No timeout */

  /* Create admin pool */
  orochi_create_resource_pool(WORKLOAD_ADMIN_POOL_NAME, &admin_limits, 100);

  /* Mark internal pools */
  for (int i = 0; i < WorkloadState->num_pools; i++) {
    if (strcmp(WorkloadState->pools[i].pool_name, WORKLOAD_DEFAULT_POOL_NAME) ==
            0 ||
        strcmp(WorkloadState->pools[i].pool_name, WORKLOAD_ADMIN_POOL_NAME) ==
            0) {
      WorkloadState->pools[i].is_internal = true;
    }
  }
}

/* ============================================================
 * Resource Pool Management
 * ============================================================ */

/*
 * allocate_pool_id
 *    Generate a unique pool ID
 */
static int64 allocate_pool_id(void) {
  static int64 next_pool_id = 1;
  return next_pool_id++;
}

/*
 * find_free_pool_slot
 *    Find an unused pool slot
 */
static ResourcePool *find_free_pool_slot(void) {
  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == 0)
      return &WorkloadState->pools[i];
  }
  return NULL;
}

/*
 * orochi_create_resource_pool
 *    Create a new resource pool with specified limits
 */
int64 orochi_create_resource_pool(const char *pool_name, ResourceLimits *limits,
                                  int priority_weight) {
  ResourcePool *pool;
  int64 pool_id;

  if (pool_name == NULL || strlen(pool_name) == 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("pool name cannot be empty")));
  }

  if (strlen(pool_name) >= WORKLOAD_MAX_POOL_NAME) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("pool name too long (max %d characters)",
                           WORKLOAD_MAX_POOL_NAME - 1)));
  }

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Check for duplicate name */
  for (int i = 0; i < WorkloadState->num_pools; i++) {
    if (WorkloadState->pools[i].pool_id != 0 &&
        strcmp(WorkloadState->pools[i].pool_name, pool_name) == 0) {
      LWLockRelease(WorkloadState->main_lock);
      ereport(ERROR,
              (errcode(ERRCODE_DUPLICATE_OBJECT),
               errmsg("resource pool \"%s\" already exists", pool_name)));
    }
  }

  /* Find free slot */
  pool = find_free_pool_slot();
  if (pool == NULL) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("maximum number of resource pools reached (%d)",
                           WORKLOAD_MAX_POOLS)));
  }

  /* Initialize pool */
  pool_id = allocate_pool_id();
  pool->pool_id = pool_id;
  strlcpy(pool->pool_name, pool_name, WORKLOAD_MAX_POOL_NAME);

  if (limits != NULL)
    memcpy(&pool->limits, limits, sizeof(ResourceLimits));
  else {
    memset(&pool->limits, 0, sizeof(ResourceLimits));
    pool->limits.cpu_percent = WORKLOAD_DEFAULT_CPU_PERCENT;
    pool->limits.query_timeout_ms = orochi_default_query_timeout_ms;
  }

  memset(&pool->usage, 0, sizeof(ResourceUsage));
  pool->state = POOL_STATE_ACTIVE;
  pool->active_queries = 0;
  pool->queued_queries = 0;
  pool->priority_weight = (priority_weight > 0) ? priority_weight : 50;
  pool->is_internal = false;
  pool->created_at = GetCurrentTimestamp();
  pool->lock = &(GetNamedLWLockTranche("orochi_workload")->lock);

  WorkloadState->num_pools++;

  LWLockRelease(WorkloadState->main_lock);

  elog(DEBUG1, "Created resource pool \"%s\" (id=%ld)", pool_name, pool_id);

  return pool_id;
}

/*
 * orochi_alter_resource_pool
 *    Modify an existing resource pool
 */
bool orochi_alter_resource_pool(int64 pool_id, ResourceLimits *new_limits,
                                int priority_weight) {
  ResourcePool *pool = NULL;
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      pool = &WorkloadState->pools[i];
      found = true;
      break;
    }
  }

  if (!found) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("resource pool with id %ld does not exist", pool_id)));
  }

  if (new_limits != NULL) {
    if (!orochi_validate_limits(new_limits)) {
      LWLockRelease(WorkloadState->main_lock);
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid resource limits")));
    }
    memcpy(&pool->limits, new_limits, sizeof(ResourceLimits));
  }

  if (priority_weight > 0)
    pool->priority_weight = priority_weight;

  LWLockRelease(WorkloadState->main_lock);

  elog(DEBUG1, "Altered resource pool \"%s\" (id=%ld)", pool->pool_name,
       pool_id);

  return true;
}

/*
 * orochi_drop_resource_pool
 *    Remove a resource pool
 */
bool orochi_drop_resource_pool(int64 pool_id, bool if_exists) {
  ResourcePool *pool = NULL;
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      pool = &WorkloadState->pools[i];
      found = true;
      break;
    }
  }

  if (!found) {
    LWLockRelease(WorkloadState->main_lock);
    if (if_exists)
      return false;
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("resource pool with id %ld does not exist", pool_id)));
  }

  /* Cannot drop internal pools */
  if (pool->is_internal) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("cannot drop internal resource pool \"%s\"",
                           pool->pool_name)));
  }

  /* Cannot drop pool with active queries */
  if (pool->active_queries > 0) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                    errmsg("resource pool \"%s\" has %d active queries",
                           pool->pool_name, pool->active_queries)));
  }

  elog(DEBUG1, "Dropped resource pool \"%s\" (id=%ld)", pool->pool_name,
       pool_id);

  /* Clear the slot */
  memset(pool, 0, sizeof(ResourcePool));
  WorkloadState->num_pools--;

  LWLockRelease(WorkloadState->main_lock);

  return true;
}

/*
 * orochi_get_resource_pool
 *    Get resource pool by ID
 */
ResourcePool *orochi_get_resource_pool(int64 pool_id) {
  ResourcePool *result = NULL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      result = (ResourcePool *)palloc(sizeof(ResourcePool));
      memcpy(result, &WorkloadState->pools[i], sizeof(ResourcePool));
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return result;
}

/*
 * orochi_get_resource_pool_by_name
 *    Get resource pool by name
 */
ResourcePool *orochi_get_resource_pool_by_name(const char *pool_name) {
  ResourcePool *result = NULL;

  if (pool_name == NULL)
    return NULL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id != 0 &&
        strcmp(WorkloadState->pools[i].pool_name, pool_name) == 0) {
      result = (ResourcePool *)palloc(sizeof(ResourcePool));
      memcpy(result, &WorkloadState->pools[i], sizeof(ResourcePool));
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return result;
}

/*
 * orochi_list_resource_pools
 *    Get list of all resource pools
 */
List *orochi_list_resource_pools(void) {
  List *pools = NIL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id != 0) {
      ResourcePool *copy = (ResourcePool *)palloc(sizeof(ResourcePool));
      memcpy(copy, &WorkloadState->pools[i], sizeof(ResourcePool));
      pools = lappend(pools, copy);
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return pools;
}

/*
 * orochi_enable_resource_pool
 *    Enable a resource pool
 */
bool orochi_enable_resource_pool(int64 pool_id) {
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      WorkloadState->pools[i].state = POOL_STATE_ACTIVE;
      found = true;
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return found;
}

/*
 * orochi_disable_resource_pool
 *    Disable a resource pool (drains first)
 */
bool orochi_disable_resource_pool(int64 pool_id) {
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      if (WorkloadState->pools[i].is_internal) {
        LWLockRelease(WorkloadState->main_lock);
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("cannot disable internal pool")));
      }
      WorkloadState->pools[i].state = POOL_STATE_DRAINING;
      found = true;
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return found;
}

/* ============================================================
 * Workload Class Management
 * ============================================================ */

/*
 * allocate_class_id
 *    Generate a unique class ID
 */
static int64 allocate_class_id(void) {
  static int64 next_class_id = 1;
  return next_class_id++;
}

/*
 * find_free_class_slot
 *    Find an unused class slot
 */
static WorkloadClass *find_free_class_slot(void) {
  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id == 0)
      return &WorkloadState->classes[i];
  }
  return NULL;
}

/*
 * orochi_create_workload_class
 *    Create a new workload class
 */
int64 orochi_create_workload_class(const char *class_name, int64 pool_id,
                                   QueryPriority default_priority,
                                   ResourceLimits *limits) {
  WorkloadClass *wclass;
  int64 class_id;
  bool pool_found = false;

  if (class_name == NULL || strlen(class_name) == 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("class name cannot be empty")));
  }

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Verify pool exists */
  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      pool_found = true;
      break;
    }
  }

  if (!pool_found) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("resource pool with id %ld does not exist", pool_id)));
  }

  /* Check for duplicate name */
  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id != 0 &&
        strcmp(WorkloadState->classes[i].class_name, class_name) == 0) {
      LWLockRelease(WorkloadState->main_lock);
      ereport(ERROR,
              (errcode(ERRCODE_DUPLICATE_OBJECT),
               errmsg("workload class \"%s\" already exists", class_name)));
    }
  }

  /* Find free slot */
  wclass = find_free_class_slot();
  if (wclass == NULL) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("maximum number of workload classes reached (%d)",
                           WORKLOAD_MAX_CLASSES)));
  }

  /* Initialize class */
  class_id = allocate_class_id();
  wclass->class_id = class_id;
  strlcpy(wclass->class_name, class_name, WORKLOAD_MAX_CLASS_NAME);
  wclass->pool_id = pool_id;
  wclass->default_priority = default_priority;

  if (limits != NULL)
    memcpy(&wclass->limits, limits, sizeof(ResourceLimits));
  else
    memset(&wclass->limits, 0, sizeof(ResourceLimits));

  wclass->state = CLASS_STATE_ACTIVE;
  wclass->queries_executed = 0;
  wclass->queries_rejected = 0;
  wclass->created_at = GetCurrentTimestamp();

  WorkloadState->num_classes++;

  LWLockRelease(WorkloadState->main_lock);

  elog(DEBUG1, "Created workload class \"%s\" (id=%ld) in pool %ld", class_name,
       class_id, pool_id);

  return class_id;
}

/*
 * orochi_alter_workload_class
 *    Modify an existing workload class
 */
bool orochi_alter_workload_class(int64 class_id, int64 pool_id,
                                 QueryPriority priority,
                                 ResourceLimits *limits) {
  WorkloadClass *wclass = NULL;
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id == class_id) {
      wclass = &WorkloadState->classes[i];
      found = true;
      break;
    }
  }

  if (!found) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("workload class with id %ld does not exist", class_id)));
  }

  if (pool_id > 0) {
    /* Verify new pool exists */
    bool pool_found = false;
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == pool_id) {
        pool_found = true;
        break;
      }
    }
    if (!pool_found) {
      LWLockRelease(WorkloadState->main_lock);
      ereport(ERROR,
              (errcode(ERRCODE_UNDEFINED_OBJECT),
               errmsg("resource pool with id %ld does not exist", pool_id)));
    }
    wclass->pool_id = pool_id;
  }

  wclass->default_priority = priority;

  if (limits != NULL)
    memcpy(&wclass->limits, limits, sizeof(ResourceLimits));

  LWLockRelease(WorkloadState->main_lock);

  return true;
}

/*
 * orochi_drop_workload_class
 *    Remove a workload class
 */
bool orochi_drop_workload_class(int64 class_id, bool if_exists) {
  WorkloadClass *wclass = NULL;
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id == class_id) {
      wclass = &WorkloadState->classes[i];
      found = true;
      break;
    }
  }

  if (!found) {
    LWLockRelease(WorkloadState->main_lock);
    if (if_exists)
      return false;
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("workload class with id %ld does not exist", class_id)));
  }

  elog(DEBUG1, "Dropped workload class \"%s\" (id=%ld)", wclass->class_name,
       class_id);

  memset(wclass, 0, sizeof(WorkloadClass));
  WorkloadState->num_classes--;

  LWLockRelease(WorkloadState->main_lock);

  return true;
}

/*
 * orochi_get_workload_class
 *    Get workload class by ID
 */
WorkloadClass *orochi_get_workload_class(int64 class_id) {
  WorkloadClass *result = NULL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id == class_id) {
      result = (WorkloadClass *)palloc(sizeof(WorkloadClass));
      memcpy(result, &WorkloadState->classes[i], sizeof(WorkloadClass));
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return result;
}

/*
 * orochi_get_workload_class_by_name
 *    Get workload class by name
 */
WorkloadClass *orochi_get_workload_class_by_name(const char *class_name) {
  WorkloadClass *result = NULL;

  if (class_name == NULL)
    return NULL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id != 0 &&
        strcmp(WorkloadState->classes[i].class_name, class_name) == 0) {
      result = (WorkloadClass *)palloc(sizeof(WorkloadClass));
      memcpy(result, &WorkloadState->classes[i], sizeof(WorkloadClass));
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return result;
}

/*
 * orochi_list_workload_classes
 *    Get list of all workload classes
 */
List *orochi_list_workload_classes(void) {
  List *classes = NIL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_CLASSES; i++) {
    if (WorkloadState->classes[i].class_id != 0) {
      WorkloadClass *copy = (WorkloadClass *)palloc(sizeof(WorkloadClass));
      memcpy(copy, &WorkloadState->classes[i], sizeof(WorkloadClass));
      classes = lappend(classes, copy);
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return classes;
}

/* ============================================================
 * Query Admission Control
 * ============================================================ */

/*
 * check_pool_resources
 *    Check if pool has enough resources for a new query
 */
static bool check_pool_resources(ResourcePool *pool, int64 memory_requested) {
  /* Check concurrency limit */
  if (pool->limits.max_concurrency > 0 &&
      pool->active_queries >= pool->limits.max_concurrency) {
    return false;
  }

  /* Check memory limit */
  if (pool->limits.memory_mb > 0) {
    int64 available =
        (pool->limits.memory_mb * 1024 * 1024) - pool->usage.memory_bytes;
    if (memory_requested > available)
      return false;
  }

  return true;
}

/*
 * orochi_admit_query
 *    Attempt to admit a query for execution
 */
bool orochi_admit_query(int backend_pid, int64 pool_id, QueryPriority priority,
                        int64 estimated_cost, int64 memory_requested) {
  ResourcePool *pool = NULL;
  bool admitted = false;

  if (!orochi_workload_enabled)
    return true; /* Bypass when disabled */

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Find pool */
  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      pool = &WorkloadState->pools[i];
      break;
    }
  }

  if (pool == NULL) {
    LWLockRelease(WorkloadState->main_lock);
    ereport(WARNING,
            (errmsg("resource pool %ld not found, using default", pool_id)));
    return true; /* Allow query to proceed */
  }

  /* Check pool state */
  if (pool->state != POOL_STATE_ACTIVE) {
    LWLockRelease(WorkloadState->main_lock);
    if (orochi_workload_log_rejections) {
      elog(LOG, "Query rejected: pool \"%s\" is not active", pool->pool_name);
    }
    return false;
  }

  /* Check resources */
  if (check_pool_resources(pool, memory_requested)) {
    pool->active_queries++;
    pool->usage.queries_executed++;
    pool->usage.memory_bytes += memory_requested;
    WorkloadState->total_active_queries++;
    admitted = true;

    elog(DEBUG2,
         "Admitted query for backend %d in pool \"%s\" "
         "(active=%d, memory=%ld)",
         backend_pid, pool->pool_name, pool->active_queries,
         pool->usage.memory_bytes);
  } else {
    pool->usage.queries_queued++;
    if (orochi_workload_log_rejections) {
      elog(LOG,
           "Query for backend %d queued: pool \"%s\" at capacity "
           "(active=%d/%d)",
           backend_pid, pool->pool_name, pool->active_queries,
           pool->limits.max_concurrency);
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return admitted;
}

/*
 * orochi_queue_query
 *    Add a query to the waiting queue
 */
bool orochi_queue_query(int backend_pid, int64 pool_id, QueryPriority priority,
                        int64 estimated_cost) {
  QueryEntry *entry;
  ResourcePool *pool = NULL;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Find pool and check queue limit */
  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      pool = &WorkloadState->pools[i];
      break;
    }
  }

  if (pool == NULL) {
    LWLockRelease(WorkloadState->main_lock);
    return false;
  }

  if (pool->limits.max_queue_size > 0 &&
      pool->queued_queries >= pool->limits.max_queue_size) {
    LWLockRelease(WorkloadState->main_lock);
    pool->usage.queries_rejected++;
    if (orochi_workload_log_rejections) {
      elog(LOG, "Query rejected: pool \"%s\" queue full (%d/%d)",
           pool->pool_name, pool->queued_queries, pool->limits.max_queue_size);
    }
    return false;
  }

  /* Find free queue slot */
  entry = find_queue_slot();
  if (entry == NULL) {
    LWLockRelease(WorkloadState->main_lock);
    elog(WARNING, "Global query queue is full");
    return false;
  }

  /* Initialize queue entry */
  entry->backend_pid = backend_pid;
  entry->pool_id = pool_id;
  entry->priority = priority;
  entry->enqueue_time = GetCurrentTimestamp();
  entry->start_time = 0;
  entry->estimated_cost = estimated_cost;
  entry->is_active = false;

  pool->queued_queries++;
  WorkloadState->total_queued_queries++;

  LWLockRelease(WorkloadState->main_lock);

  elog(DEBUG2, "Queued query for backend %d in pool %ld (position=%d)",
       backend_pid, pool_id, pool->queued_queries);

  return true;
}

/*
 * find_queue_slot
 *    Find a free slot in the query queue
 */
static QueryEntry *find_queue_slot(void) {
  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    if (WorkloadState->queue[i].backend_pid == 0)
      return &WorkloadState->queue[i];
  }
  return NULL;
}

/*
 * compare_query_priority
 *    Comparison function for sorting queued queries
 *    Sorts by priority (descending) then enqueue_time (ascending)
 */
static int compare_query_priority(const void *a, const void *b) {
  const QueryEntry *qa = *(const QueryEntry **)a;
  const QueryEntry *qb = *(const QueryEntry **)b;

  /* Higher priority first */
  if (qa->priority != qb->priority)
    return (qb->priority - qa->priority);

  /* Same priority - earlier enqueue time first (FIFO) */
  if (qa->enqueue_time < qb->enqueue_time)
    return -1;
  else if (qa->enqueue_time > qb->enqueue_time)
    return 1;

  return 0;
}

/*
 * update_pool_usage
 *    Update a pool's resource usage with delta values
 */
static void update_pool_usage(ResourcePool *pool, ResourceUsage *delta) {
  if (pool == NULL || delta == NULL)
    return;

  pool->usage.cpu_time_us += delta->cpu_time_us;
  pool->usage.memory_bytes += delta->memory_bytes;
  pool->usage.io_read_bytes += delta->io_read_bytes;
  pool->usage.io_write_bytes += delta->io_write_bytes;
  pool->usage.queries_executed += delta->queries_executed;
  pool->usage.queries_queued += delta->queries_queued;
  pool->usage.queries_rejected += delta->queries_rejected;

  /* Track peak memory */
  if (pool->usage.memory_bytes > pool->usage.peak_memory_bytes)
    pool->usage.peak_memory_bytes = pool->usage.memory_bytes;

  pool->usage.last_updated = GetCurrentTimestamp();
}

/*
 * orochi_release_query
 *    Release resources when a query completes
 */
void orochi_release_query(int backend_pid) {
  if (!orochi_workload_enabled)
    return;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Remove from queue if present */
  remove_from_queue(backend_pid);

  /* Update pool counters */
  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id != 0 &&
        WorkloadState->pools[i].active_queries > 0) {
      /* Note: In a real implementation, we'd track which pool
       * the query was in. For now, we just decrement. */
    }
  }

  WorkloadState->total_active_queries--;

  LWLockRelease(WorkloadState->main_lock);
}

/*
 * remove_from_queue
 *    Remove a backend from the query queue
 */
static void remove_from_queue(int backend_pid) {
  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    if (WorkloadState->queue[i].backend_pid == backend_pid) {
      /* Find pool to update counter */
      int64 pool_id = WorkloadState->queue[i].pool_id;
      for (int j = 0; j < WORKLOAD_MAX_POOLS; j++) {
        if (WorkloadState->pools[j].pool_id == pool_id) {
          WorkloadState->pools[j].queued_queries--;
          break;
        }
      }

      memset(&WorkloadState->queue[i], 0, sizeof(QueryEntry));
      WorkloadState->total_queued_queries--;
      break;
    }
  }
}

/*
 * orochi_dequeue_next_query
 *    Get the next query to run from the queue (by priority)
 */
bool orochi_dequeue_next_query(int64 pool_id, int *backend_pid) {
  QueryEntry *best = NULL;
  int best_idx = -1;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  /* Find highest priority query for this pool */
  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    QueryEntry *entry = &WorkloadState->queue[i];
    if (entry->backend_pid != 0 && entry->pool_id == pool_id &&
        !entry->is_active) {
      if (best == NULL || entry->priority > best->priority) {
        best = entry;
        best_idx = i;
      } else if (entry->priority == best->priority) {
        /* Same priority - use FIFO */
        if (entry->enqueue_time < best->enqueue_time) {
          best = entry;
          best_idx = i;
        }
      }
    }
  }

  if (best != NULL) {
    *backend_pid = best->backend_pid;
    best->is_active = true;
    best->start_time = GetCurrentTimestamp();

    /* Update pool counters */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == pool_id) {
        WorkloadState->pools[i].active_queries++;
        WorkloadState->pools[i].queued_queries--;
        break;
      }
    }

    WorkloadState->total_active_queries++;
    WorkloadState->total_queued_queries--;

    LWLockRelease(WorkloadState->main_lock);
    return true;
  }

  LWLockRelease(WorkloadState->main_lock);
  return false;
}

/*
 * orochi_get_queue_length
 *    Get number of queued queries for a pool
 */
int orochi_get_queue_length(int64 pool_id) {
  int count = 0;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
    if (WorkloadState->pools[i].pool_id == pool_id) {
      count = WorkloadState->pools[i].queued_queries;
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return count;
}

/*
 * orochi_cancel_queued_query
 *    Remove a query from the queue
 */
bool orochi_cancel_queued_query(int backend_pid) {
  bool found = false;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    if (WorkloadState->queue[i].backend_pid == backend_pid) {
      remove_from_queue(backend_pid);
      found = true;
      break;
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  return found;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * orochi_priority_name
 *    Convert priority enum to string
 */
const char *orochi_priority_name(QueryPriority priority) {
  switch (priority) {
  case QUERY_PRIORITY_LOW:
    return "low";
  case QUERY_PRIORITY_NORMAL:
    return "normal";
  case QUERY_PRIORITY_HIGH:
    return "high";
  case QUERY_PRIORITY_CRITICAL:
    return "critical";
  default:
    return "unknown";
  }
}

/*
 * orochi_parse_priority
 *    Convert string to priority enum
 */
QueryPriority orochi_parse_priority(const char *name) {
  if (pg_strcasecmp(name, "low") == 0)
    return QUERY_PRIORITY_LOW;
  if (pg_strcasecmp(name, "normal") == 0)
    return QUERY_PRIORITY_NORMAL;
  if (pg_strcasecmp(name, "high") == 0)
    return QUERY_PRIORITY_HIGH;
  if (pg_strcasecmp(name, "critical") == 0)
    return QUERY_PRIORITY_CRITICAL;
  return QUERY_PRIORITY_NORMAL;
}

/*
 * orochi_pool_state_name
 *    Convert pool state enum to string
 */
const char *orochi_pool_state_name(ResourcePoolState state) {
  switch (state) {
  case POOL_STATE_ACTIVE:
    return "active";
  case POOL_STATE_DRAINING:
    return "draining";
  case POOL_STATE_DISABLED:
    return "disabled";
  default:
    return "unknown";
  }
}

/*
 * orochi_create_default_limits
 *    Create a ResourceLimits struct with default values
 */
ResourceLimits *orochi_create_default_limits(void) {
  ResourceLimits *limits = (ResourceLimits *)palloc0(sizeof(ResourceLimits));

  limits->cpu_percent = WORKLOAD_DEFAULT_CPU_PERCENT;
  limits->memory_mb = WORKLOAD_DEFAULT_MEMORY_MB;
  limits->io_read_mbps = WORKLOAD_DEFAULT_IO_MBPS;
  limits->io_write_mbps = WORKLOAD_DEFAULT_IO_MBPS;
  limits->max_concurrency = WORKLOAD_DEFAULT_CONCURRENCY;
  limits->max_queue_size = 0;
  limits->query_timeout_ms = orochi_default_query_timeout_ms;
  limits->idle_timeout_ms = 0;

  return limits;
}

/*
 * orochi_copy_limits
 *    Copy resource limits
 */
void orochi_copy_limits(ResourceLimits *dst, ResourceLimits *src) {
  if (dst != NULL && src != NULL)
    memcpy(dst, src, sizeof(ResourceLimits));
}

/*
 * orochi_validate_limits
 *    Validate resource limits are reasonable
 */
bool orochi_validate_limits(ResourceLimits *limits) {
  if (limits == NULL)
    return true;

  if (limits->cpu_percent < 0 || limits->cpu_percent > 100)
    return false;

  if (limits->memory_mb < 0)
    return false;

  if (limits->max_concurrency < 0)
    return false;

  if (limits->query_timeout_ms < 0)
    return false;

  return true;
}

/*
 * orochi_log_pool_stats
 *    Log statistics for a pool (for debugging)
 */
void orochi_log_pool_stats(int64 pool_id) {
  ResourcePool *pool = orochi_get_resource_pool(pool_id);

  if (pool != NULL) {
    elog(LOG,
         "Pool \"%s\" (id=%ld): active=%d, queued=%d, "
         "cpu_time=%ld us, memory=%ld bytes, "
         "executed=%ld, rejected=%ld",
         pool->pool_name, pool->pool_id, pool->active_queries,
         pool->queued_queries, pool->usage.cpu_time_us,
         pool->usage.memory_bytes, pool->usage.queries_executed,
         pool->usage.queries_rejected);
    pfree(pool);
  }
}

/*
 * orochi_log_workload_status
 *    Log overall workload status
 */
void orochi_log_workload_status(void) {
  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  elog(LOG,
       "Workload status: pools=%d, classes=%d, "
       "active_queries=%d, queued_queries=%d",
       WorkloadState->num_pools, WorkloadState->num_classes,
       WorkloadState->total_active_queries,
       WorkloadState->total_queued_queries);

  LWLockRelease(WorkloadState->main_lock);
}

/* ============================================================
 * SQL Function Implementations
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_resource_pool_sql);
Datum orochi_create_resource_pool_sql(PG_FUNCTION_ARGS) {
  text *pool_name_text = PG_GETARG_TEXT_PP(0);
  char *pool_name = text_to_cstring(pool_name_text);
  int cpu_percent = PG_ARGISNULL(1) ? 100 : PG_GETARG_INT32(1);
  int64 memory_mb = PG_ARGISNULL(2) ? 0 : PG_GETARG_INT64(2);
  int max_concurrency = PG_ARGISNULL(3) ? 0 : PG_GETARG_INT32(3);
  int priority_weight = PG_ARGISNULL(4) ? 50 : PG_GETARG_INT32(4);

  ResourceLimits limits;
  int64 pool_id;

  memset(&limits, 0, sizeof(ResourceLimits));
  limits.cpu_percent = cpu_percent;
  limits.memory_mb = memory_mb;
  limits.max_concurrency = max_concurrency;
  limits.query_timeout_ms = orochi_default_query_timeout_ms;

  pool_id = orochi_create_resource_pool(pool_name, &limits, priority_weight);

  pfree(pool_name);

  PG_RETURN_INT64(pool_id);
}

PG_FUNCTION_INFO_V1(orochi_alter_resource_pool_sql);
Datum orochi_alter_resource_pool_sql(PG_FUNCTION_ARGS) {
  int64 pool_id = PG_GETARG_INT64(0);
  int cpu_percent = PG_ARGISNULL(1) ? -1 : PG_GETARG_INT32(1);
  int64 memory_mb = PG_ARGISNULL(2) ? -1 : PG_GETARG_INT64(2);
  int max_concurrency = PG_ARGISNULL(3) ? -1 : PG_GETARG_INT32(3);
  int priority_weight = PG_ARGISNULL(4) ? -1 : PG_GETARG_INT32(4);

  ResourcePool *current = orochi_get_resource_pool(pool_id);
  ResourceLimits limits;
  bool result;

  if (current == NULL)
    PG_RETURN_BOOL(false);

  memcpy(&limits, &current->limits, sizeof(ResourceLimits));

  if (cpu_percent >= 0)
    limits.cpu_percent = cpu_percent;
  if (memory_mb >= 0)
    limits.memory_mb = memory_mb;
  if (max_concurrency >= 0)
    limits.max_concurrency = max_concurrency;

  result = orochi_alter_resource_pool(pool_id, &limits, priority_weight);

  pfree(current);

  PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(orochi_drop_resource_pool_sql);
Datum orochi_drop_resource_pool_sql(PG_FUNCTION_ARGS) {
  text *pool_name_text = PG_GETARG_TEXT_PP(0);
  char *pool_name = text_to_cstring(pool_name_text);
  bool if_exists = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);

  ResourcePool *pool = orochi_get_resource_pool_by_name(pool_name);
  bool result = false;

  if (pool != NULL) {
    result = orochi_drop_resource_pool(pool->pool_id, if_exists);
    pfree(pool);
  } else if (!if_exists) {
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("resource pool \"%s\" does not exist", pool_name)));
  }

  pfree(pool_name);

  PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(orochi_assign_workload_class_sql);
Datum orochi_assign_workload_class_sql(PG_FUNCTION_ARGS) {
  text *class_name_text = PG_GETARG_TEXT_PP(0);
  char *class_name = text_to_cstring(class_name_text);
  bool result;

  result = orochi_assign_workload_class_by_name(MyProcPid, class_name);

  pfree(class_name);

  PG_RETURN_BOOL(result);
}

/* Session and class assignment */
bool orochi_assign_workload_class_by_name(int backend_pid,
                                          const char *class_name) {
  WorkloadClass *wclass = orochi_get_workload_class_by_name(class_name);

  if (wclass == NULL) {
    ereport(WARNING, (errmsg("workload class \"%s\" not found", class_name)));
    return false;
  }

  return orochi_assign_workload_class(backend_pid, wclass->class_id);
}

/* ============================================================
 * Session Tracking Implementation
 * ============================================================ */

/*
 * session_hash_slot
 *    Calculate hash slot for a backend PID
 */
static uint32 session_hash_slot(int backend_pid) {
  /* Simple hash using the PID */
  uint32 hash = (uint32)backend_pid;
  hash = hash * 2654435761U; /* Knuth multiplicative hash */
  return hash % WORKLOAD_MAX_SESSIONS;
}

/*
 * find_session_slot
 *    Find a session slot by backend PID (must hold session_lock)
 */
static SessionResourceTracker *find_session_slot(int backend_pid) {
  uint32 slot;
  int probe_count;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return NULL;

  slot = session_hash_slot(backend_pid);

  /* Linear probing with limited probes */
  for (probe_count = 0; probe_count < 16; probe_count++) {
    SessionResourceTracker *entry = &WorkloadState->sessions[slot];

    /* Empty slot means not found */
    if (entry->backend_pid == 0)
      return NULL;

    /* Check for match */
    if (entry->backend_pid == backend_pid && entry->is_active)
      return entry;

    /* Move to next slot */
    slot = (slot + 1) % WORKLOAD_MAX_SESSIONS;
  }

  return NULL; /* Not found after probing */
}

/*
 * find_free_session_slot
 *    Find an empty session slot for insertion (must hold session_lock)
 */
static SessionResourceTracker *find_free_session_slot(int backend_pid) {
  uint32 slot;
  int probe_count;
  TimestampTz now = GetCurrentTimestamp();

  if (WorkloadState == NULL)
    return NULL;

  slot = session_hash_slot(backend_pid);

  /* Linear probing to find empty or expired slot */
  for (probe_count = 0; probe_count < 16; probe_count++) {
    SessionResourceTracker *entry = &WorkloadState->sessions[slot];

    /* Empty slot - use it */
    if (entry->backend_pid == 0)
      return entry;

    /* Inactive entry - can reuse */
    if (!entry->is_active)
      return entry;

    /* Expired entry (idle for too long) - can reuse */
    if (entry->last_activity > 0 &&
        (now - entry->last_activity) > WORKLOAD_SESSION_IDLE_TIMEOUT) {
      entry->is_active = false;
      WorkloadState->num_sessions--;
      return entry;
    }

    /* Move to next slot */
    slot = (slot + 1) % WORKLOAD_MAX_SESSIONS;
  }

  return NULL; /* No free slot found */
}

/*
 * orochi_assign_workload_class
 *    Assign a workload class to a session
 */
bool orochi_assign_workload_class(int backend_pid, int64 class_id) {
  SessionResourceTracker *session;
  WorkloadClass *wclass;
  bool found = false;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return false;

  /* Verify class exists */
  wclass = orochi_get_workload_class(class_id);
  if (wclass == NULL) {
    ereport(WARNING, (errmsg("workload class %ld not found", class_id)));
    return false;
  }

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    session->class_id = class_id;
    session->pool_id = wclass->pool_id;
    session->last_activity = GetCurrentTimestamp();
    found = true;
  } else {
    /* Session not registered - register it now */
    session = find_free_session_slot(backend_pid);
    if (session != NULL) {
      session->backend_pid = backend_pid;
      session->class_id = class_id;
      session->pool_id = wclass->pool_id;
      session->is_active = true;
      session->session_start = GetCurrentTimestamp();
      session->last_activity = session->session_start;
      session->active_queries = 0;
      memset(&session->usage, 0, sizeof(ResourceUsage));
      WorkloadState->num_sessions++;
      found = true;
    }
  }

  LWLockRelease(WorkloadState->session_lock);

  pfree(wclass);

  elog(DEBUG1, "Assigned backend %d to workload class %ld (found=%d)",
       backend_pid, class_id, found);

  return found;
}

/*
 * orochi_get_session_class_id
 *    Get the workload class ID for a session
 */
int64 orochi_get_session_class_id(int backend_pid) {
  SessionResourceTracker *session;
  int64 class_id = 0;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return 0;

  LWLockAcquire(WorkloadState->session_lock, LW_SHARED);

  session = find_session_slot(backend_pid);
  if (session != NULL)
    class_id = session->class_id;

  LWLockRelease(WorkloadState->session_lock);

  return class_id;
}

/*
 * orochi_register_session
 *    Register a new session in shared memory
 */
bool orochi_register_session(int backend_pid, int64 pool_id, int64 class_id) {
  SessionResourceTracker *session;
  bool registered = false;

  if (WorkloadState == NULL || !WorkloadState->is_initialized) {
    elog(DEBUG1, "WorkloadState not initialized, cannot register session");
    return false;
  }

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  /* Check if already registered */
  session = find_session_slot(backend_pid);
  if (session != NULL) {
    /* Update existing session */
    session->pool_id = pool_id;
    session->class_id = class_id;
    session->last_activity = GetCurrentTimestamp();
    registered = true;
    elog(DEBUG1, "Updated existing session for backend %d", backend_pid);
  } else {
    /* Find a free slot */
    session = find_free_session_slot(backend_pid);
    if (session != NULL) {
      TimestampTz now = GetCurrentTimestamp();

      session->backend_pid = backend_pid;
      session->pool_id = pool_id;
      session->class_id = class_id;
      session->is_active = true;
      session->session_start = now;
      session->last_activity = now;
      session->active_queries = 0;
      memset(&session->usage, 0, sizeof(ResourceUsage));
      session->usage.last_updated = now;

      WorkloadState->num_sessions++;
      registered = true;

      elog(DEBUG1,
           "Registered new session for backend %d in pool %ld class %ld",
           backend_pid, pool_id, class_id);
    } else {
      elog(WARNING,
           "Cannot register session for backend %d: no free slots "
           "(max sessions: %d)",
           backend_pid, WORKLOAD_MAX_SESSIONS);
    }
  }

  LWLockRelease(WorkloadState->session_lock);

  return registered;
}

/*
 * orochi_unregister_session
 *    Unregister a session from shared memory
 */
void orochi_unregister_session(int backend_pid) {
  SessionResourceTracker *session;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    /* Update pool resource usage before clearing */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == session->pool_id) {
        /* Add session usage to pool totals */
        WorkloadState->pools[i].usage.cpu_time_us += session->usage.cpu_time_us;
        WorkloadState->pools[i].usage.io_read_bytes +=
            session->usage.io_read_bytes;
        WorkloadState->pools[i].usage.io_write_bytes +=
            session->usage.io_write_bytes;
        break;
      }
    }

    /* Clear the session slot */
    session->backend_pid = 0;
    session->pool_id = 0;
    session->class_id = 0;
    session->is_active = false;
    session->active_queries = 0;
    memset(&session->usage, 0, sizeof(ResourceUsage));

    WorkloadState->num_sessions--;
    if (WorkloadState->num_sessions < 0)
      WorkloadState->num_sessions = 0;

    elog(DEBUG1, "Unregistered session for backend %d", backend_pid);
  }

  LWLockRelease(WorkloadState->session_lock);
}

/*
 * orochi_get_session_tracker
 *    Get a copy of the session tracker for a backend
 *    Returns palloc'd copy or NULL if not found
 */
SessionResourceTracker *orochi_get_session_tracker(int backend_pid) {
  SessionResourceTracker *session;
  SessionResourceTracker *result = NULL;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return NULL;

  LWLockAcquire(WorkloadState->session_lock, LW_SHARED);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    result = (SessionResourceTracker *)palloc(sizeof(SessionResourceTracker));
    memcpy(result, session, sizeof(SessionResourceTracker));
  }

  LWLockRelease(WorkloadState->session_lock);

  return result;
}

/*
 * orochi_update_session_activity
 *    Update the last activity timestamp for a session
 */
void orochi_update_session_activity(int backend_pid) {
  SessionResourceTracker *session;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    session->last_activity = GetCurrentTimestamp();
    session->usage.last_updated = session->last_activity;
  }

  LWLockRelease(WorkloadState->session_lock);
}

/*
 * orochi_track_session_cpu
 *    Track CPU time for a session
 */
void orochi_track_session_cpu(int backend_pid, int64 cpu_time_us) {
  SessionResourceTracker *session;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  if (cpu_time_us <= 0)
    return;

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    session->usage.cpu_time_us += cpu_time_us;
    session->usage.last_updated = GetCurrentTimestamp();
    session->last_activity = session->usage.last_updated;

    /* Also update the pool's CPU usage */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == session->pool_id) {
        WorkloadState->pools[i].usage.cpu_time_us += cpu_time_us;
        WorkloadState->pools[i].usage.last_updated =
            session->usage.last_updated;
        break;
      }
    }

    elog(DEBUG2, "Tracked %ld us CPU for backend %d (total: %ld us)",
         cpu_time_us, backend_pid, session->usage.cpu_time_us);
  }

  LWLockRelease(WorkloadState->session_lock);
}

/*
 * orochi_track_session_memory
 *    Track memory usage for a session
 */
void orochi_track_session_memory(int backend_pid, int64 memory_bytes) {
  SessionResourceTracker *session;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    /* Update current memory usage */
    session->usage.memory_bytes = memory_bytes;

    /* Track peak memory */
    if (memory_bytes > session->usage.peak_memory_bytes)
      session->usage.peak_memory_bytes = memory_bytes;

    session->usage.last_updated = GetCurrentTimestamp();
    session->last_activity = session->usage.last_updated;

    /* Update pool memory tracking */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == session->pool_id) {
        /* Note: Pool memory is aggregated from all sessions */
        WorkloadState->pools[i].usage.last_updated =
            session->usage.last_updated;
        break;
      }
    }

    elog(DEBUG2, "Tracked %ld bytes memory for backend %d (peak: %ld)",
         memory_bytes, backend_pid, session->usage.peak_memory_bytes);
  }

  LWLockRelease(WorkloadState->session_lock);
}

/*
 * orochi_track_session_io
 *    Track I/O usage for a session
 */
void orochi_track_session_io(int backend_pid, int64 read_bytes,
                             int64 write_bytes) {
  SessionResourceTracker *session;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  if (read_bytes <= 0 && write_bytes <= 0)
    return;

  LWLockAcquire(WorkloadState->session_lock, LW_EXCLUSIVE);

  session = find_session_slot(backend_pid);
  if (session != NULL) {
    TimestampTz now = GetCurrentTimestamp();

    if (read_bytes > 0)
      session->usage.io_read_bytes += read_bytes;
    if (write_bytes > 0)
      session->usage.io_write_bytes += write_bytes;

    session->usage.last_updated = now;
    session->last_activity = now;

    /* Update pool I/O tracking */
    for (int i = 0; i < WORKLOAD_MAX_POOLS; i++) {
      if (WorkloadState->pools[i].pool_id == session->pool_id) {
        if (read_bytes > 0)
          WorkloadState->pools[i].usage.io_read_bytes += read_bytes;
        if (write_bytes > 0)
          WorkloadState->pools[i].usage.io_write_bytes += write_bytes;
        WorkloadState->pools[i].usage.last_updated = now;
        break;
      }
    }

    elog(DEBUG2, "Tracked I/O for backend %d: read=%ld, write=%ld", backend_pid,
         read_bytes, write_bytes);
  }

  LWLockRelease(WorkloadState->session_lock);
}

/*
 * orochi_get_queued_queries
 *    Get a list of queued queries for a pool
 *    Returns a List of palloc'd QueryEntry copies
 */
List *orochi_get_queued_queries(int64 pool_id) {
  List *queries = NIL;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return NIL;

  LWLockAcquire(WorkloadState->main_lock, LW_SHARED);

  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    QueryEntry *entry = &WorkloadState->queue[i];

    /* Include entries that match pool_id and are not yet active */
    if (entry->backend_pid != 0 &&
        (pool_id == 0 || entry->pool_id == pool_id) && !entry->is_active) {
      QueryEntry *copy = (QueryEntry *)palloc(sizeof(QueryEntry));
      memcpy(copy, entry, sizeof(QueryEntry));
      queries = lappend(queries, copy);
    }
  }

  LWLockRelease(WorkloadState->main_lock);

  /* Sort by priority (descending) and enqueue time (ascending) */
  if (list_length(queries) > 1) {
    /* Use qsort on the list - first convert to array */
    int count = list_length(queries);
    QueryEntry **arr = (QueryEntry **)palloc(count * sizeof(QueryEntry *));
    ListCell *lc;
    int idx = 0;

    foreach (lc, queries) {
      arr[idx++] = (QueryEntry *)lfirst(lc);
    }

    qsort(arr, count, sizeof(QueryEntry *), compare_query_priority);

    /* Rebuild list in sorted order */
    list_free(queries);
    queries = NIL;
    for (idx = 0; idx < count; idx++) {
      queries = lappend(queries, arr[idx]);
    }
    pfree(arr);
  }

  return queries;
}

/*
 * orochi_drain_pool_queue
 *    Drain all queued queries for a pool (cancel them)
 */
void orochi_drain_pool_queue(int64 pool_id) {
  int drained = 0;

  if (WorkloadState == NULL || !WorkloadState->is_initialized)
    return;

  LWLockAcquire(WorkloadState->main_lock, LW_EXCLUSIVE);

  for (int i = 0; i < WORKLOAD_QUEUE_SIZE; i++) {
    QueryEntry *entry = &WorkloadState->queue[i];

    if (entry->backend_pid != 0 && entry->pool_id == pool_id &&
        !entry->is_active) {
      int64 entry_pool_id = entry->pool_id;

      /* Clear the queue entry */
      memset(entry, 0, sizeof(QueryEntry));
      drained++;

      /* Update pool queued count */
      for (int j = 0; j < WORKLOAD_MAX_POOLS; j++) {
        if (WorkloadState->pools[j].pool_id == entry_pool_id) {
          WorkloadState->pools[j].queued_queries--;
          if (WorkloadState->pools[j].queued_queries < 0)
            WorkloadState->pools[j].queued_queries = 0;
          WorkloadState->pools[j].usage.queries_rejected++;
          break;
        }
      }
    }
  }

  WorkloadState->total_queued_queries -= drained;
  if (WorkloadState->total_queued_queries < 0)
    WorkloadState->total_queued_queries = 0;

  LWLockRelease(WorkloadState->main_lock);

  if (drained > 0) {
    elog(LOG, "Drained %d queries from pool %ld queue", drained, pool_id);
  }
}

ResourceUsage *orochi_get_pool_usage(int64 pool_id) {
  ResourcePool *pool = orochi_get_resource_pool(pool_id);
  ResourceUsage *usage = NULL;

  if (pool != NULL) {
    usage = (ResourceUsage *)palloc(sizeof(ResourceUsage));
    memcpy(usage, &pool->usage, sizeof(ResourceUsage));
    pfree(pool);
  }

  return usage;
}

ResourcePoolState orochi_parse_pool_state(const char *name) {
  if (pg_strcasecmp(name, "active") == 0)
    return POOL_STATE_ACTIVE;
  if (pg_strcasecmp(name, "draining") == 0)
    return POOL_STATE_DRAINING;
  if (pg_strcasecmp(name, "disabled") == 0)
    return POOL_STATE_DISABLED;
  return POOL_STATE_ACTIVE;
}
