/*-------------------------------------------------------------------------
 *
 * ddl_workflow.h
 *    Orochi DB Workflow DDL definitions
 *
 * This module provides workflow automation DDL similar to Snowflake/Databricks:
 *   - CREATE WORKFLOW for ETL pipeline definitions
 *   - STAGE, TRANSFORM, LOAD operations
 *   - Workflow execution and monitoring
 *
 * Example:
 *   CREATE WORKFLOW my_etl AS (
 *     STAGE raw_data FROM 's3://bucket/data/*.parquet',
 *     TRANSFORM WITH query,
 *     LOAD INTO target_table
 *   );
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_DDL_WORKFLOW_H
#define OROCHI_DDL_WORKFLOW_H

#include "fmgr.h"
#include "postgres.h"
#include "utils/timestamp.h"

/* ============================================================
 * Workflow Step Types
 * ============================================================ */

typedef enum WorkflowStepType {
  WORKFLOW_STEP_STAGE = 0,     /* Stage data from external source */
  WORKFLOW_STEP_TRANSFORM = 1, /* Transform data with SQL */
  WORKFLOW_STEP_LOAD = 2,      /* Load into target table */
  WORKFLOW_STEP_MERGE = 3,     /* Merge/upsert into target */
  WORKFLOW_STEP_VALIDATE = 4,  /* Data validation step */
  WORKFLOW_STEP_NOTIFY = 5,    /* Send notification */
  WORKFLOW_STEP_CUSTOM = 6     /* Custom SQL execution */
} WorkflowStepType;

/* ============================================================
 * Workflow States
 * ============================================================ */

typedef enum WorkflowState {
  WORKFLOW_STATE_CREATED = 0,   /* Workflow defined but never run */
  WORKFLOW_STATE_RUNNING = 1,   /* Currently executing */
  WORKFLOW_STATE_SUCCEEDED = 2, /* Last run succeeded */
  WORKFLOW_STATE_FAILED = 3,    /* Last run failed */
  WORKFLOW_STATE_PAUSED = 4,    /* Manually paused */
  WORKFLOW_STATE_SUSPENDED = 5  /* Suspended due to errors */
} WorkflowState;

/* ============================================================
 * Stage Source Types
 * ============================================================ */

typedef enum StageSourceType {
  STAGE_SOURCE_S3 = 0,    /* Amazon S3 */
  STAGE_SOURCE_GCS = 1,   /* Google Cloud Storage */
  STAGE_SOURCE_AZURE = 2, /* Azure Blob Storage */
  STAGE_SOURCE_LOCAL = 3, /* Local filesystem */
  STAGE_SOURCE_HTTP = 4,  /* HTTP/HTTPS URL */
  STAGE_SOURCE_TABLE = 5  /* Another database table */
} StageSourceType;

/* ============================================================
 * Stage File Formats
 * ============================================================ */

typedef enum StageFileFormat {
  STAGE_FORMAT_PARQUET = 0, /* Apache Parquet */
  STAGE_FORMAT_CSV = 1,     /* Comma-separated values */
  STAGE_FORMAT_JSON = 2,    /* JSON/NDJSON */
  STAGE_FORMAT_AVRO = 3,    /* Apache Avro */
  STAGE_FORMAT_ORC = 4      /* Apache ORC */
} StageFileFormat;

/* ============================================================
 * Workflow Step Configuration
 * ============================================================ */

/*
 * StageConfig - Configuration for STAGE step
 */
typedef struct StageConfig {
  char *stage_name;            /* Name for this staging area */
  StageSourceType source_type; /* Type of source */
  char *source_url;            /* Source URL/path */
  char *file_pattern;          /* File glob pattern */
  StageFileFormat file_format; /* File format */
  char *credentials_name;      /* Named credentials reference */
  char *compression;           /* Compression type (gzip, etc.) */
  bool recursive;              /* Recurse into subdirectories */
  int max_files;               /* Maximum files to process */
  int64 max_file_size;         /* Maximum file size in bytes */
  char *copy_options;          /* Additional COPY options */
} StageConfig;

/*
 * TransformConfig - Configuration for TRANSFORM step
 */
typedef struct TransformConfig {
  char *query_text;   /* SQL query for transformation */
  char *source_stage; /* Source stage name */
  char *target_stage; /* Target stage name (temp) */
  bool materialize;   /* Materialize intermediate result */
  int parallelism;    /* Degree of parallelism */
  int64 row_limit;    /* Limit rows processed */
} TransformConfig;

/*
 * LoadConfig - Configuration for LOAD step
 */
typedef struct LoadConfig {
  Oid target_table_oid;  /* Target table OID */
  char *target_table;    /* Target table name */
  char *source_stage;    /* Source stage name */
  char **column_mapping; /* Source->target column mapping */
  int num_mappings;      /* Number of column mappings */
  bool truncate_target;  /* Truncate before load */
  bool use_copy;         /* Use COPY for bulk loading */
  char *on_error;        /* ABORT, CONTINUE, SKIP */
  int error_limit;       /* Max errors before abort */
} LoadConfig;

/*
 * MergeConfig - Configuration for MERGE step
 */
typedef struct MergeConfig {
  Oid target_table_oid;   /* Target table OID */
  char *target_table;     /* Target table name */
  char *source_stage;     /* Source stage name */
  char **key_columns;     /* Merge key columns */
  int num_key_columns;    /* Number of key columns */
  char *when_matched;     /* UPDATE clause */
  char *when_not_matched; /* INSERT clause */
  bool delete_unmatched;  /* Delete rows not in source */
} MergeConfig;

/*
 * WorkflowStep - Single step in a workflow
 */
typedef struct WorkflowStep {
  int step_id;                /* Step sequence number */
  char *step_name;            /* Human-readable name */
  WorkflowStepType step_type; /* Type of step */

  /* Step-specific configuration (union based on type) */
  union {
    StageConfig *stage;
    TransformConfig *transform;
    LoadConfig *load;
    MergeConfig *merge;
    char *custom_sql; /* For CUSTOM type */
  } config;

  /* Dependencies */
  int *depends_on;      /* Step IDs this depends on */
  int num_dependencies; /* Number of dependencies */

  /* Execution settings */
  int timeout_seconds;    /* Step timeout */
  int retry_count;        /* Retry attempts */
  int retry_delay_ms;     /* Delay between retries */
  bool continue_on_error; /* Continue workflow on error */

  /* Condition */
  char *condition; /* Optional SQL condition */
} WorkflowStep;

/* ============================================================
 * Workflow Run Statistics
 * ============================================================ */

typedef struct WorkflowRunStats {
  int64 run_id;            /* Run identifier */
  int64 workflow_id;       /* Workflow ID */
  WorkflowState state;     /* Run state */
  TimestampTz started_at;  /* Start time */
  TimestampTz finished_at; /* End time */
  int64 duration_ms;       /* Total duration */
  int steps_total;         /* Total steps */
  int steps_completed;     /* Completed steps */
  int steps_failed;        /* Failed steps */
  int64 rows_processed;    /* Total rows processed */
  int64 bytes_processed;   /* Total bytes processed */
  char *error_message;     /* Error if failed */
  int failed_step_id;      /* Step that failed */
} WorkflowRunStats;

/* ============================================================
 * Main Workflow Structure
 * ============================================================ */

typedef struct Workflow {
  int64 workflow_id;   /* Unique identifier */
  char *workflow_name; /* Human-readable name */
  char *description;   /* Description */

  /* Steps */
  int num_steps;        /* Number of steps */
  WorkflowStep **steps; /* Array of steps */

  /* Scheduling */
  char *schedule; /* Cron expression or NULL */
  bool enabled;   /* Is workflow enabled */

  /* Options */
  bool auto_resume;        /* Auto-resume on failure */
  int max_concurrent;      /* Max concurrent runs */
  char *warehouse;         /* Compute warehouse name */
  char *error_integration; /* Error notification target */

  /* State */
  WorkflowState state;     /* Current state */
  int64 last_run_id;       /* Last run ID */
  TimestampTz created_at;  /* Creation time */
  TimestampTz last_run_at; /* Last execution time */
  TimestampTz next_run_at; /* Next scheduled run */

  /* Statistics */
  int64 total_runs;      /* Total executions */
  int64 successful_runs; /* Successful runs */
  int64 failed_runs;     /* Failed runs */

  /* Memory context */
  MemoryContext workflow_context; /* Memory context */
} Workflow;

/* ============================================================
 * Function Prototypes - Workflow Management
 * ============================================================ */

/* Workflow lifecycle */
extern int64 orochi_create_workflow(const char *name, const char *definition);
extern bool orochi_drop_workflow(int64 workflow_id, bool if_exists);
extern bool orochi_alter_workflow(int64 workflow_id, const char *alterations);

/* Workflow execution */
extern int64 orochi_run_workflow(int64 workflow_id);
extern bool orochi_cancel_workflow_run(int64 run_id);
extern bool orochi_pause_workflow(int64 workflow_id);
extern bool orochi_resume_workflow(int64 workflow_id);

/* Workflow configuration */
extern Workflow *orochi_get_workflow(int64 workflow_id);
extern Workflow *orochi_get_workflow_by_name(const char *name);
extern List *orochi_list_workflows(void);
extern bool orochi_enable_workflow(int64 workflow_id);
extern bool orochi_disable_workflow(int64 workflow_id);

/* Workflow status */
extern WorkflowState orochi_get_workflow_state(int64 workflow_id);
extern WorkflowRunStats *orochi_get_workflow_run(int64 run_id);
extern List *orochi_get_workflow_runs(int64 workflow_id, int limit);

/* ============================================================
 * Function Prototypes - DDL Parsing
 * ============================================================ */

/* Parse CREATE WORKFLOW statement */
extern Workflow *ddl_parse_create_workflow(const char *sql);

/* Parse individual step definitions */
extern WorkflowStep *ddl_parse_stage_step(const char *definition);
extern WorkflowStep *ddl_parse_transform_step(const char *definition);
extern WorkflowStep *ddl_parse_load_step(const char *definition);
extern WorkflowStep *ddl_parse_merge_step(const char *definition);

/* Validate workflow definition */
extern bool ddl_validate_workflow(Workflow *workflow, char **error_msg);

/* ============================================================
 * Function Prototypes - Catalog Operations
 * ============================================================ */

/* Store workflow in catalog */
extern void ddl_catalog_store_workflow(Workflow *workflow);
extern void ddl_catalog_update_workflow(Workflow *workflow);
extern void ddl_catalog_delete_workflow(int64 workflow_id);

/* Store workflow run information */
extern int64 ddl_catalog_create_run(int64 workflow_id);
extern void ddl_catalog_update_run(WorkflowRunStats *stats);
extern void ddl_catalog_finish_run(int64 run_id, WorkflowState state,
                                   const char *error_msg);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_workflow_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_workflow_sql(PG_FUNCTION_ARGS);
extern Datum orochi_run_workflow_sql(PG_FUNCTION_ARGS);
extern Datum orochi_pause_workflow_sql(PG_FUNCTION_ARGS);
extern Datum orochi_resume_workflow_sql(PG_FUNCTION_ARGS);
extern Datum orochi_workflow_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_workflows_sql(PG_FUNCTION_ARGS);
extern Datum orochi_workflow_runs_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *workflow_state_name(WorkflowState state);
extern WorkflowState workflow_parse_state(const char *name);
extern const char *workflow_step_type_name(WorkflowStepType type);
extern WorkflowStepType workflow_parse_step_type(const char *name);
extern const char *stage_source_type_name(StageSourceType type);
extern StageSourceType stage_parse_source_type(const char *name);
extern const char *stage_format_name(StageFileFormat format);
extern StageFileFormat stage_parse_format(const char *name);

/* Free workflow structures */
extern void workflow_free(Workflow *workflow);
extern void workflow_step_free(WorkflowStep *step);

/* ============================================================
 * Constants
 * ============================================================ */

#define WORKFLOW_MAX_NAME_LENGTH 128
#define WORKFLOW_MAX_STEPS 100
#define WORKFLOW_DEFAULT_TIMEOUT 3600 /* 1 hour */
#define WORKFLOW_DEFAULT_RETRY_COUNT 3
#define WORKFLOW_DEFAULT_RETRY_DELAY 60000 /* 1 minute */
#define WORKFLOW_MAX_CONCURRENT_RUNS 5

/* Catalog table names */
#define OROCHI_WORKFLOWS_TABLE "orochi_workflows"
#define OROCHI_WORKFLOW_STEPS_TABLE "orochi_workflow_steps"
#define OROCHI_WORKFLOW_RUNS_TABLE "orochi_workflow_runs"
#define OROCHI_WORKFLOW_STEP_RUNS_TABLE "orochi_workflow_step_runs"

#endif /* OROCHI_DDL_WORKFLOW_H */
