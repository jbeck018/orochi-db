/*-------------------------------------------------------------------------
 *
 * compat.h
 *    PostgreSQL version compatibility macros for Orochi DB
 *
 * This header provides compatibility macros to support PostgreSQL versions
 * 16, 17, and 18. It abstracts away API differences between versions.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_COMPAT_H
#define OROCHI_COMPAT_H

#include "postgres.h"

/*
 * PostgreSQL version detection
 *
 * PG_VERSION_NUM format: XXYYZZ where XX=major, YY=minor, ZZ=patch
 * Example: 160000 = PostgreSQL 16.0.0
 *          180000 = PostgreSQL 18.0.0
 */

/* Minimum supported version */
#define OROCHI_MIN_PG_VERSION_NUM 160000
#define OROCHI_MIN_PG_VERSION_STR "16"

/* Maximum tested version */
#define OROCHI_MAX_PG_VERSION_NUM 189999
#define OROCHI_MAX_PG_VERSION_STR "18"

/* Version check at compile time */
#if PG_VERSION_NUM < OROCHI_MIN_PG_VERSION_NUM
#error "Orochi DB requires PostgreSQL 16 or later"
#endif

/*
 * PG_MODULE_MAGIC_EXT - New in PostgreSQL 18
 *
 * PostgreSQL 18 introduces PG_MODULE_MAGIC_EXT which allows extensions
 * to report their name and version. This information can be accessed
 * via pg_get_loaded_modules().
 *
 * For PG16/17, we fall back to the standard PG_MODULE_MAGIC.
 */
#ifdef PG_MODULE_MAGIC_EXT
#define OROCHI_MODULE_MAGIC \
    PG_MODULE_MAGIC_EXT( \
        .name = "orochi", \
        .version = OROCHI_VERSION_STRING \
    )
#else
#define OROCHI_MODULE_MAGIC PG_MODULE_MAGIC
#endif

/*
 * PostgreSQL 18 AIO (Asynchronous I/O) compatibility
 *
 * PostgreSQL 18 introduces an asynchronous I/O subsystem. Extensions can
 * optionally leverage this for improved sequential scan performance.
 * For now, we provide compatibility stubs.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_AIO 1
#else
#define OROCHI_HAS_AIO 0
#endif

/*
 * PostgreSQL 18 Virtual Generated Columns
 *
 * PostgreSQL 18 changes generated columns to be virtual by default.
 * This affects how we handle generated column values in columnar storage.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_GENERATED_COLS_VIRTUAL_DEFAULT 1
#else
#define OROCHI_GENERATED_COLS_VIRTUAL_DEFAULT 0
#endif

/*
 * PostgreSQL 18 Skip Scan for B-tree indexes
 *
 * PostgreSQL 18 adds skip scan support for multicolumn B-tree indexes.
 * This can benefit our distributed index scans.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_BTREE_SKIP_SCAN 1
#else
#define OROCHI_HAS_BTREE_SKIP_SCAN 0
#endif

/*
 * PostgreSQL 18 amgettreeheight index AM API
 *
 * PostgreSQL 18 adds amgettreeheight to the index AM API for getting
 * the height of a B-tree index. Future versions of Orochi may use this
 * for cost estimation.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_AM_GETTREEHEIGHT 1
#else
#define OROCHI_HAS_AM_GETTREEHEIGHT 0
#endif

/*
 * PostgreSQL 18 EXPLAIN hooks
 *
 * PostgreSQL 18 adds new EXPLAIN-related hooks:
 * - explain_per_node_hook
 * - explain_per_plan_hook
 * - explain_validate_options_hook
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_EXPLAIN_HOOKS 1
#else
#define OROCHI_HAS_EXPLAIN_HOOKS 0
#endif

/*
 * PostgreSQL 18 extension_control_path
 *
 * PostgreSQL 18 adds a server variable to specify extension control file
 * locations, enabling dynamic extension loading from custom paths.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_EXTENSION_CONTROL_PATH 1
#else
#define OROCHI_HAS_EXTENSION_CONTROL_PATH 0
#endif

/*
 * PostgreSQL 18 Cumulative Statistics API
 *
 * PostgreSQL 18 allows extensions to use the server's cumulative
 * statistics API for tracking custom statistics.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_CUMULATIVE_STATS_API 1
#else
#define OROCHI_HAS_CUMULATIVE_STATS_API 0
#endif

/*
 * PostgreSQL 18 Temporal Constraints
 *
 * PostgreSQL 18 adds temporal constraints (PRIMARY KEY, UNIQUE, FOREIGN KEY
 * over ranges). Our hypertable time-series features may leverage this.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_TEMPORAL_CONSTRAINTS 1
#else
#define OROCHI_HAS_TEMPORAL_CONSTRAINTS 0
#endif

/*
 * PostgreSQL 18 UUIDv7 support
 *
 * PostgreSQL 18 adds uuidv7() for timestamp-ordered UUIDs.
 * This can be useful for distributed ID generation.
 */
#if PG_VERSION_NUM >= 180000
#define OROCHI_HAS_UUIDV7 1
#else
#define OROCHI_HAS_UUIDV7 0
#endif

/*
 * PostgreSQL 17 compatibility
 *
 * PostgreSQL 17 introduced some changes that we need to handle.
 */
#if PG_VERSION_NUM >= 170000
#define OROCHI_PG17_COMPAT 1
#else
#define OROCHI_PG17_COMPAT 0
#endif

/*
 * LWLock tranche registration compatibility
 *
 * The LWLock API has been stable across PG16-18, but we define
 * a wrapper for future compatibility.
 */
#define OROCHI_LWLOCK_REGISTER_TRANCHE(id, tranche) \
    LWLockRegisterTranche(id, tranche)

/*
 * Memory context compatibility
 *
 * Memory context API is stable across PG16-18.
 */
#define OROCHI_MEMORY_CONTEXT_SWITCH(ctx) \
    MemoryContextSwitchTo(ctx)

/*
 * Hash table creation compatibility
 *
 * Hash table API is stable across PG16-18.
 */
#define OROCHI_HASH_CREATE(name, nelem, info, flags) \
    hash_create(name, nelem, info, flags)

/*
 * Background worker flags compatibility
 *
 * Background worker API is stable across PG16-18.
 */
#define OROCHI_BGW_FLAGS \
    (BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION)

/*
 * GUC context compatibility
 *
 * GUC API is stable across PG16-18.
 */
#define OROCHI_GUC_USERSET PGC_USERSET
#define OROCHI_GUC_SIGHUP  PGC_SIGHUP
#define OROCHI_GUC_POSTMASTER PGC_POSTMASTER

/*
 * Executor hook signature compatibility
 *
 * Executor hooks have been stable across PG16-18.
 * We define these for documentation and future-proofing.
 */
#define OROCHI_EXECUTOR_START_HOOK_ARGS QueryDesc *queryDesc, int eflags
#define OROCHI_EXECUTOR_RUN_HOOK_ARGS QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once
#define OROCHI_EXECUTOR_FINISH_HOOK_ARGS QueryDesc *queryDesc
#define OROCHI_EXECUTOR_END_HOOK_ARGS QueryDesc *queryDesc

/*
 * Planner hook signature compatibility
 *
 * Planner hooks have been stable across PG16-18.
 */
#define OROCHI_PLANNER_HOOK_ARGS Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams

/*
 * Utility for runtime version checking
 *
 * Returns true if the running PostgreSQL version is at least the specified
 * major version.
 */
static inline bool
orochi_pg_version_at_least(int major_version)
{
    return (PG_VERSION_NUM / 10000) >= major_version;
}

/*
 * Get the PostgreSQL major version number
 */
static inline int
orochi_pg_major_version(void)
{
    return PG_VERSION_NUM / 10000;
}

/*
 * Feature availability logging
 *
 * Call this during initialization to log which PG18 features are available.
 */
#define OROCHI_LOG_COMPAT_INFO() \
    do { \
        elog(DEBUG1, "Orochi DB compiled for PostgreSQL %d", PG_VERSION_NUM / 10000); \
        elog(DEBUG1, "  AIO support: %s", OROCHI_HAS_AIO ? "yes" : "no"); \
        elog(DEBUG1, "  Temporal constraints: %s", OROCHI_HAS_TEMPORAL_CONSTRAINTS ? "yes" : "no"); \
        elog(DEBUG1, "  UUIDv7: %s", OROCHI_HAS_UUIDV7 ? "yes" : "no"); \
        elog(DEBUG1, "  EXPLAIN hooks: %s", OROCHI_HAS_EXPLAIN_HOOKS ? "yes" : "no"); \
    } while (0)

#endif /* OROCHI_COMPAT_H */
