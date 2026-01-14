/*-------------------------------------------------------------------------
 *
 * init.c
 *    Orochi DB initialization and extension lifecycle management
 *
 * This file handles:
 *   - Extension initialization (_PG_init)
 *   - GUC (Grand Unified Configuration) setup
 *   - Hook registration (planner, executor)
 *   - Background worker initialization
 *   - Shared memory setup
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "postmaster/bgworker.h"
#include "access/xact.h"

#include "../orochi.h"
#include "catalog.h"
#include "../planner/distributed_planner.h"
#include "../executor/distributed_executor.h"

PG_MODULE_MAGIC;

/* GUC variables */
int     orochi_default_shard_count = 32;
int     orochi_stripe_row_limit = 150000;
int     orochi_chunk_time_interval_hours = 24;
int     orochi_compression_level = 3;
char   *orochi_s3_endpoint = NULL;
char   *orochi_s3_bucket = NULL;
char   *orochi_s3_access_key = NULL;
char   *orochi_s3_secret_key = NULL;
char   *orochi_s3_region = NULL;
bool    orochi_enable_vectorized_execution = true;
bool    orochi_enable_parallel_query = true;
int     orochi_parallel_workers = 4;
bool    orochi_enable_tiering = true;
int     orochi_hot_threshold_hours = 24;
int     orochi_warm_threshold_days = 7;
int     orochi_cold_threshold_days = 30;
int     orochi_cache_size_mb = 256;

/* Shared memory size */
static Size orochi_shmem_size = 0;

/* Previous hooks for chaining */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorRun_hook_type prev_executor_run_hook = NULL;
static ExecutorFinish_hook_type prev_executor_finish_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

/* Forward declarations */
static void orochi_shmem_startup(void);
static void orochi_register_background_workers(void);
static void orochi_define_gucs(void);
static Size orochi_memsize(void);

/*
 * _PG_init
 *    Extension entry point - called when extension is loaded
 */
void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(WARNING,
                (errmsg("orochi extension should be loaded via shared_preload_libraries"),
                 errhint("Add 'orochi' to shared_preload_libraries in postgresql.conf")));
    }

    /* Define GUC variables */
    orochi_define_gucs();

    /* Request shared memory */
    RequestAddinShmemSpace(orochi_memsize());
    RequestNamedLWLockTranche("orochi", 8);

    /* Install shared memory hooks */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = orochi_shmem_startup;

    /* Install planner hook */
    prev_planner_hook = planner_hook;
    planner_hook = orochi_planner_hook;

    /* Install executor hooks */
    prev_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = orochi_executor_start_hook;

    prev_executor_run_hook = ExecutorRun_hook;
    ExecutorRun_hook = orochi_executor_run_hook;

    prev_executor_finish_hook = ExecutorFinish_hook;
    ExecutorFinish_hook = orochi_executor_finish_hook;

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = orochi_executor_end_hook;

    /* Register background workers */
    if (process_shared_preload_libraries_in_progress)
        orochi_register_background_workers();

    elog(LOG, "Orochi DB v%s initialized", OROCHI_VERSION_STRING);
}

/*
 * _PG_fini
 *    Extension cleanup - called when extension is unloaded
 */
void
_PG_fini(void)
{
    /* Restore previous hooks */
    shmem_startup_hook = prev_shmem_startup_hook;
    planner_hook = prev_planner_hook;
    ExecutorStart_hook = prev_executor_start_hook;
    ExecutorRun_hook = prev_executor_run_hook;
    ExecutorFinish_hook = prev_executor_finish_hook;
    ExecutorEnd_hook = prev_executor_end_hook;

    elog(LOG, "Orochi DB unloaded");
}

/*
 * orochi_define_gucs
 *    Define all GUC configuration parameters
 */
static void
orochi_define_gucs(void)
{
    /* Sharding configuration */
    DefineCustomIntVariable("orochi.default_shard_count",
                           "Default number of shards for distributed tables",
                           NULL,
                           &orochi_default_shard_count,
                           32,        /* default */
                           1,         /* min */
                           1024,      /* max */
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    /* Columnar storage configuration */
    DefineCustomIntVariable("orochi.stripe_row_limit",
                           "Maximum rows per columnar stripe",
                           NULL,
                           &orochi_stripe_row_limit,
                           150000,
                           1000,
                           10000000,
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    /* Time-series configuration */
    DefineCustomIntVariable("orochi.chunk_time_interval_hours",
                           "Default chunk interval for hypertables (hours)",
                           NULL,
                           &orochi_chunk_time_interval_hours,
                           24,
                           1,
                           8760,      /* 1 year */
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    /* Compression configuration */
    DefineCustomIntVariable("orochi.compression_level",
                           "Default compression level (1-19 for ZSTD)",
                           NULL,
                           &orochi_compression_level,
                           3,
                           1,
                           19,
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    /* S3 configuration */
    DefineCustomStringVariable("orochi.s3_endpoint",
                              "S3 endpoint URL for cold storage",
                              NULL,
                              &orochi_s3_endpoint,
                              NULL,
                              PGC_SIGHUP,
                              0,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.s3_bucket",
                              "S3 bucket for cold storage",
                              NULL,
                              &orochi_s3_bucket,
                              NULL,
                              PGC_SIGHUP,
                              0,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.s3_access_key",
                              "S3 access key",
                              NULL,
                              &orochi_s3_access_key,
                              NULL,
                              PGC_SIGHUP,
                              GUC_SUPERUSER_ONLY,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.s3_secret_key",
                              "S3 secret key",
                              NULL,
                              &orochi_s3_secret_key,
                              NULL,
                              PGC_SIGHUP,
                              GUC_SUPERUSER_ONLY,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.s3_region",
                              "S3 region",
                              NULL,
                              &orochi_s3_region,
                              "us-east-1",
                              PGC_SIGHUP,
                              0,
                              NULL, NULL, NULL);

    /* Vectorization configuration */
    DefineCustomBoolVariable("orochi.enable_vectorized_execution",
                            "Enable vectorized query execution for columnar tables",
                            NULL,
                            &orochi_enable_vectorized_execution,
                            true,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    /* Parallel query configuration */
    DefineCustomBoolVariable("orochi.enable_parallel_query",
                            "Enable parallel distributed query execution",
                            NULL,
                            &orochi_enable_parallel_query,
                            true,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.parallel_workers",
                           "Number of parallel workers per shard",
                           NULL,
                           &orochi_parallel_workers,
                           4,
                           1,
                           64,
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    /* Tiered storage configuration */
    DefineCustomBoolVariable("orochi.enable_tiering",
                            "Enable automatic data tiering",
                            NULL,
                            &orochi_enable_tiering,
                            true,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.hot_threshold_hours",
                           "Hours until data moves from hot to warm tier",
                           NULL,
                           &orochi_hot_threshold_hours,
                           24,
                           1,
                           720,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.warm_threshold_days",
                           "Days until data moves from warm to cold tier",
                           NULL,
                           &orochi_warm_threshold_days,
                           7,
                           1,
                           365,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.cold_threshold_days",
                           "Days until data moves from cold to frozen tier",
                           NULL,
                           &orochi_cold_threshold_days,
                           30,
                           7,
                           3650,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    /* Cache configuration */
    DefineCustomIntVariable("orochi.cache_size_mb",
                           "Size of columnar cache in megabytes",
                           NULL,
                           &orochi_cache_size_mb,
                           256,
                           16,
                           16384,
                           PGC_POSTMASTER,
                           0,
                           NULL, NULL, NULL);
}

/*
 * orochi_memsize
 *    Calculate shared memory requirements
 */
static Size
orochi_memsize(void)
{
    Size size = 0;

    /* Columnar cache */
    size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);

    /* Shard metadata cache */
    size = add_size(size, 1024 * 1024);  /* 1MB for shard info */

    /* Node registry */
    size = add_size(size, 64 * 1024);    /* 64KB for node info */

    /* Statistics counters */
    size = add_size(size, 64 * 1024);    /* 64KB for stats */

    return size;
}

/*
 * orochi_shmem_startup
 *    Initialize shared memory structures
 */
static void
orochi_shmem_startup(void)
{
    bool found;

    /* Call previous hook if exists */
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Initialize shared memory for Orochi */
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    /* Columnar cache shared memory */
    ShmemInitStruct("orochi_columnar_cache",
                   (Size) orochi_cache_size_mb * 1024 * 1024,
                   &found);

    /* Shard metadata cache */
    ShmemInitStruct("orochi_shard_metadata",
                   1024 * 1024,
                   &found);

    /* Node registry */
    ShmemInitStruct("orochi_node_registry",
                   64 * 1024,
                   &found);

    /* Statistics */
    ShmemInitStruct("orochi_statistics",
                   64 * 1024,
                   &found);

    LWLockRelease(AddinShmemInitLock);

    elog(LOG, "Orochi shared memory initialized: %zu bytes", orochi_memsize());
}

/*
 * orochi_register_background_workers
 *    Register background workers for maintenance tasks
 */
static void
orochi_register_background_workers(void)
{
    BackgroundWorker worker;

    /* Tiering worker - moves data between storage tiers */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi tiering worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi tiering");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60;  /* Restart after 60 seconds on crash */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_tiering_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Compression worker - compresses cold chunks */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi compression worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi compression");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_compression_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Stats collector - gathers cluster statistics */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi stats collector");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi stats");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_stats_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Rebalancer worker - balances shards across nodes */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi rebalancer");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi rebalancer");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 300;  /* Restart after 5 minutes */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_rebalancer_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    elog(LOG, "Orochi background workers registered");
}
