/*-------------------------------------------------------------------------
 *
 * ddl_dynamic.h
 *    Orochi DB Dynamic Table DDL definitions
 *
 * This module provides dynamic tables (auto-refreshing materialized views)
 * similar to Snowflake Dynamic Tables:
 *   - CREATE DYNAMIC TABLE for declarative pipelines
 *   - Automatic refresh based on target lag
 *   - Dependency tracking and refresh ordering
 *
 * Example:
 *   CREATE DYNAMIC TABLE my_dyn
 *     TARGET_LAG = '1 hour'
 *     AS SELECT * FROM source;
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_DDL_DYNAMIC_H
#define OROCHI_DDL_DYNAMIC_H

#include "fmgr.h"
#include "postgres.h"
#include "utils/timestamp.h"

/* ============================================================
 * Dynamic Table Refresh Modes
 * ============================================================ */

typedef enum DynamicTableRefreshMode {
  DYNAMIC_REFRESH_AUTO = 0,       /* Automatic incremental */
  DYNAMIC_REFRESH_FULL = 1,       /* Full refresh only */
  DYNAMIC_REFRESH_INCREMENTAL = 2 /* Incremental when possible */
} DynamicTableRefreshMode;

/* ============================================================
 * Dynamic Table States
 * ============================================================ */

typedef enum DynamicTableState {
  DYNAMIC_STATE_ACTIVE = 0,       /* Active and refreshing */
  DYNAMIC_STATE_SUSPENDED = 1,    /* Manually suspended */
  DYNAMIC_STATE_FAILED = 2,       /* Last refresh failed */
  DYNAMIC_STATE_INITIALIZING = 3, /* Initial data load */
  DYNAMIC_STATE_REFRESHING = 4    /* Currently refreshing */
} DynamicTableState;

/* ============================================================
 * Scheduling Modes
 * ============================================================ */

typedef enum DynamicSchedulingMode {
  DYNAMIC_SCHEDULE_TARGET_LAG = 0, /* Based on target lag */
  DYNAMIC_SCHEDULE_DOWNSTREAM = 1  /* Based on downstream needs */
} DynamicSchedulingMode;

/* ============================================================
 * Data Freshness Info
 * ============================================================ */

typedef struct DynamicTableFreshness {
  TimestampTz data_timestamp; /* Data freshness timestamp */
  Interval *current_lag;      /* Current lag from source */
  Interval *target_lag;       /* Target lag setting */
  bool is_fresh;              /* Within target lag? */
  TimestampTz last_refresh;   /* Last successful refresh */
  TimestampTz next_refresh;   /* Next scheduled refresh */
} DynamicTableFreshness;

/* ============================================================
 * Refresh Statistics
 * ============================================================ */

typedef struct DynamicRefreshStats {
  int64 refresh_id;             /* Refresh identifier */
  int64 dynamic_table_id;       /* Dynamic table ID */
  DynamicTableRefreshMode mode; /* Refresh mode used */

  /* Timing */
  TimestampTz started_at;  /* Start time */
  TimestampTz finished_at; /* End time */
  int64 duration_ms;       /* Duration in milliseconds */

  /* Data metrics */
  int64 rows_inserted;  /* Rows inserted */
  int64 rows_updated;   /* Rows updated */
  int64 rows_deleted;   /* Rows deleted */
  int64 bytes_produced; /* Bytes written */

  /* Source metrics */
  int64 source_changes; /* Changes from sources */
  int64 bytes_scanned;  /* Bytes read */

  /* Status */
  bool success;        /* Refresh succeeded */
  char *error_message; /* Error if failed */

  /* Lag info */
  Interval *lag_before; /* Lag before refresh */
  Interval *lag_after;  /* Lag after refresh */
} DynamicRefreshStats;

/* ============================================================
 * Source Dependency
 * ============================================================ */

typedef struct DynamicSourceDep {
  Oid source_oid;          /* Source table/view OID */
  char *source_schema;     /* Source schema */
  char *source_name;       /* Source name */
  bool is_dynamic_table;   /* Is source also dynamic? */
  int64 source_dynamic_id; /* If dynamic, its ID */
} DynamicSourceDep;

/* ============================================================
 * Main Dynamic Table Structure
 * ============================================================ */

typedef struct DynamicTable {
  int64 dynamic_table_id; /* Unique identifier */
  char *table_name;       /* Table name */
  char *table_schema;     /* Schema name */
  Oid table_oid;          /* Materialization table OID */
  char *description;      /* Description */

  /* Query definition */
  char *query_text;   /* Source query text */
  Oid query_view_oid; /* Internal query view OID */

  /* Target lag configuration */
  Interval *target_lag;                /* Maximum acceptable lag */
  DynamicSchedulingMode schedule_mode; /* Scheduling mode */

  /* Refresh configuration */
  DynamicTableRefreshMode refresh_mode; /* Refresh mode */
  bool initialize;                      /* Initialize with data? */
  char *warehouse;                      /* Compute warehouse */

  /* Dependencies */
  int num_sources;           /* Number of source deps */
  DynamicSourceDep *sources; /* Source dependencies */

  /* State */
  DynamicTableState state;          /* Current state */
  DynamicTableFreshness *freshness; /* Freshness info */

  /* Cluster keys (optional) */
  char **cluster_keys;  /* Clustering columns */
  int num_cluster_keys; /* Number of cluster keys */

  /* Owner */
  Oid owner_oid;    /* Owner user OID */
  char *owner_name; /* Owner username */

  /* Timestamps */
  TimestampTz created_at;      /* Creation time */
  TimestampTz last_refresh_at; /* Last refresh time */
  TimestampTz next_refresh_at; /* Next scheduled refresh */

  /* Statistics */
  int64 total_refreshes;      /* Total refresh count */
  int64 successful_refreshes; /* Successful refreshes */
  int64 failed_refreshes;     /* Failed refreshes */
  double avg_refresh_ms;      /* Average refresh duration */
  int64 total_rows;           /* Current row count */
  int64 total_bytes;          /* Current size */

  /* Memory context */
  MemoryContext dynamic_context; /* Memory context */
} DynamicTable;

/* ============================================================
 * Function Prototypes - Dynamic Table Management
 * ============================================================ */

/* Dynamic table lifecycle */
extern int64 orochi_create_dynamic_table(const char *name, const char *query,
                                         Interval *target_lag,
                                         DynamicTableRefreshMode mode,
                                         bool initialize);
extern bool orochi_drop_dynamic_table(int64 dynamic_table_id, bool if_exists);
extern bool orochi_alter_dynamic_table(int64 dynamic_table_id,
                                       const char *alterations);

/* Refresh operations */
extern int64 orochi_refresh_dynamic_table(int64 dynamic_table_id);
extern bool orochi_cancel_refresh(int64 refresh_id);

/* State management */
extern bool orochi_suspend_dynamic_table(int64 dynamic_table_id);
extern bool orochi_resume_dynamic_table(int64 dynamic_table_id);
extern DynamicTableState orochi_get_dynamic_table_state(int64 dynamic_table_id);

/* Configuration */
extern DynamicTable *orochi_get_dynamic_table(int64 dynamic_table_id);
extern DynamicTable *orochi_get_dynamic_table_by_name(const char *schema,
                                                      const char *name);
extern List *orochi_list_dynamic_tables(void);

/* Freshness and lag */
extern DynamicTableFreshness *orochi_get_freshness(int64 dynamic_table_id);
extern bool orochi_set_target_lag(int64 dynamic_table_id, Interval *lag);
extern Interval *orochi_get_current_lag(int64 dynamic_table_id);

/* Refresh history */
extern DynamicRefreshStats *orochi_get_refresh_stats(int64 refresh_id);
extern List *orochi_get_refresh_history(int64 dynamic_table_id, int limit);

/* Dependency tracking */
extern List *orochi_get_dynamic_table_sources(int64 dynamic_table_id);
extern List *orochi_get_dynamic_table_dependents(int64 dynamic_table_id);

/* ============================================================
 * Function Prototypes - DDL Parsing
 * ============================================================ */

/* Parse CREATE DYNAMIC TABLE statement */
extern DynamicTable *ddl_parse_create_dynamic_table(const char *sql);

/* Parse target lag expression */
extern Interval *ddl_parse_target_lag(const char *lag_str);

/* Parse refresh mode */
extern DynamicTableRefreshMode ddl_parse_refresh_mode(const char *mode_str);

/* Extract query dependencies */
extern List *ddl_extract_query_sources(const char *query);

/* Validate dynamic table definition */
extern bool ddl_validate_dynamic_table(DynamicTable *dt, char **error_msg);

/* ============================================================
 * Function Prototypes - Catalog Operations
 * ============================================================ */

/* Store dynamic table in catalog */
extern void ddl_catalog_store_dynamic_table(DynamicTable *dt);
extern void ddl_catalog_update_dynamic_table(DynamicTable *dt);
extern void ddl_catalog_delete_dynamic_table(int64 dynamic_table_id);

/* Load dynamic table from catalog */
extern DynamicTable *ddl_catalog_load_dynamic_table(int64 dynamic_table_id);
extern DynamicTable *ddl_catalog_load_dynamic_table_by_name(const char *schema,
                                                            const char *name);

/* Store refresh information */
extern int64 ddl_catalog_create_refresh(int64 dynamic_table_id);
extern void ddl_catalog_update_refresh(DynamicRefreshStats *stats);
extern void ddl_catalog_finish_refresh(int64 refresh_id, bool success,
                                       const char *error_msg);

/* Dependency tracking */
extern void ddl_catalog_store_source_deps(int64 dynamic_table_id,
                                          List *sources);
extern void ddl_catalog_update_source_deps(int64 dynamic_table_id,
                                           List *sources);

/* ============================================================
 * Function Prototypes - Refresh Logic
 * ============================================================ */

/* Initialize refresh scheduler */
extern void dynamic_table_scheduler_init(void);
extern void dynamic_table_scheduler_shutdown(void);

/* Calculate refresh scheduling */
extern TimestampTz dynamic_table_next_refresh(DynamicTable *dt);
extern bool dynamic_table_needs_refresh(DynamicTable *dt);

/* Execute refresh */
extern bool dynamic_table_execute_refresh(DynamicTable *dt,
                                          DynamicRefreshStats *stats);
extern bool dynamic_table_incremental_refresh(DynamicTable *dt,
                                              DynamicRefreshStats *stats);
extern bool dynamic_table_full_refresh(DynamicTable *dt,
                                       DynamicRefreshStats *stats);

/* Dependency ordering */
extern List *dynamic_table_refresh_order(List *dynamic_tables);
extern bool dynamic_table_dependencies_fresh(DynamicTable *dt);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_alter_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_refresh_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_suspend_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_resume_dynamic_table_sql(PG_FUNCTION_ARGS);
extern Datum orochi_dynamic_table_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_dynamic_tables_sql(PG_FUNCTION_ARGS);
extern Datum orochi_dynamic_table_refresh_history_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *dynamic_table_state_name(DynamicTableState state);
extern DynamicTableState dynamic_table_parse_state(const char *name);
extern const char *dynamic_refresh_mode_name(DynamicTableRefreshMode mode);
extern DynamicTableRefreshMode dynamic_refresh_parse_mode(const char *name);
extern const char *dynamic_schedule_mode_name(DynamicSchedulingMode mode);

/* Interval utilities */
extern Interval *interval_subtract(Interval *a, Interval *b);
extern bool interval_is_less_than(Interval *a, Interval *b);
extern char *interval_to_string(Interval *interval);

/* Free structures */
extern void dynamic_table_free(DynamicTable *dt);
extern void dynamic_freshness_free(DynamicTableFreshness *freshness);
extern void dynamic_refresh_stats_free(DynamicRefreshStats *stats);

/* ============================================================
 * Constants
 * ============================================================ */

#define DYNAMIC_TABLE_MAX_NAME_LENGTH 128
#define DYNAMIC_TABLE_MAX_QUERY_LENGTH 262144 /* 256KB */
#define DYNAMIC_TABLE_MAX_SOURCES 100
#define DYNAMIC_TABLE_MAX_CLUSTER_KEYS 10
#define DYNAMIC_TABLE_MIN_LAG_SECONDS 60 /* 1 minute minimum */
#define DYNAMIC_TABLE_DEFAULT_LAG_HOURS 1

/* Catalog table names */
#define OROCHI_DYNAMIC_TABLES_TABLE "orochi_dynamic_tables"
#define OROCHI_DYNAMIC_SOURCES_TABLE "orochi_dynamic_sources"
#define OROCHI_DYNAMIC_REFRESHES_TABLE "orochi_dynamic_refreshes"

#endif /* OROCHI_DDL_DYNAMIC_H */
