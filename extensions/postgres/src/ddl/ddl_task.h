/*-------------------------------------------------------------------------
 *
 * ddl_task.h
 *    Orochi DB Scheduled Task DDL definitions
 *
 * This module provides scheduled task DDL similar to Snowflake Tasks:
 *   - CREATE TASK for scheduled SQL execution
 *   - Cron-based and interval-based scheduling
 *   - Task dependencies and DAG execution
 *
 * Example:
 *   CREATE TASK my_task
 *     SCHEDULE = 'USING CRON 0 * * * *'
 *     AS INSERT INTO target SELECT * FROM my_stream;
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_DDL_TASK_H
#define OROCHI_DDL_TASK_H

#include "fmgr.h"
#include "postgres.h"
#include "utils/timestamp.h"

/* ============================================================
 * Task States
 * ============================================================ */

typedef enum TaskState {
    TASK_STATE_CREATED = 0,   /* Task created but suspended */
    TASK_STATE_STARTED = 1,   /* Task is scheduled and active */
    TASK_STATE_SUSPENDED = 2, /* Task manually suspended */
    TASK_STATE_FAILED = 3     /* Task failed and suspended */
} TaskState;

/* ============================================================
 * Task Run States
 * ============================================================ */

typedef enum TaskRunState {
    TASK_RUN_SCHEDULED = 0, /* Run is scheduled */
    TASK_RUN_RUNNING = 1,   /* Currently executing */
    TASK_RUN_SUCCEEDED = 2, /* Completed successfully */
    TASK_RUN_FAILED = 3,    /* Failed execution */
    TASK_RUN_SKIPPED = 4,   /* Skipped (condition not met) */
    TASK_RUN_CANCELLED = 5  /* Cancelled by user */
} TaskRunState;

/* ============================================================
 * Schedule Types
 * ============================================================ */

typedef enum ScheduleType {
    SCHEDULE_TYPE_CRON = 0,     /* Cron expression */
    SCHEDULE_TYPE_INTERVAL = 1, /* Fixed interval */
    SCHEDULE_TYPE_TRIGGER = 2   /* Triggered by parent task */
} ScheduleType;

/* ============================================================
 * Task Condition Types
 * ============================================================ */

typedef enum TaskConditionType {
    TASK_CONDITION_NONE = 0,      /* No condition */
    TASK_CONDITION_STREAM = 1,    /* WHEN SYSTEM$STREAM_HAS_DATA */
    TASK_CONDITION_EXPRESSION = 2 /* Custom boolean expression */
} TaskConditionType;

/* ============================================================
 * Schedule Configuration
 * ============================================================ */

typedef struct TaskSchedule {
    ScheduleType type; /* Schedule type */

    /* For CRON schedule */
    char *cron_expression; /* Cron expression string */
    char *timezone;        /* Timezone for cron */

    /* For INTERVAL schedule */
    Interval *interval; /* Fixed interval */

    /* Common */
    TimestampTz start_time; /* First execution time */
    TimestampTz end_time;   /* Last execution time (NULL=forever) */
} TaskSchedule;

/* ============================================================
 * Task Condition Configuration
 * ============================================================ */

typedef struct TaskCondition {
    TaskConditionType type; /* Condition type */
    char *expression;       /* Boolean SQL expression */

    /* For STREAM condition */
    int64 stream_id;   /* Stream to check */
    char *stream_name; /* Stream name */
} TaskCondition;

/* ============================================================
 * Task Run Statistics
 * ============================================================ */

typedef struct TaskRunStats {
    int64 run_id;               /* Run identifier */
    int64 task_id;              /* Task ID */
    TaskRunState state;         /* Run state */
    TimestampTz scheduled_time; /* Scheduled execution time */
    TimestampTz started_at;     /* Actual start time */
    TimestampTz finished_at;    /* Completion time */
    int64 duration_ms;          /* Execution duration */
    int64 rows_affected;        /* Rows affected by SQL */
    int64 rows_produced;        /* Rows produced */
    char *error_message;        /* Error if failed */
    int attempt_number;         /* Retry attempt number */

    /* Query details */
    char *query_id;      /* Query identifier */
    int64 bytes_scanned; /* Bytes read */
    int64 bytes_written; /* Bytes written */
} TaskRunStats;

/* ============================================================
 * Main Task Structure
 * ============================================================ */

typedef struct Task {
    int64 task_id;     /* Unique identifier */
    char *task_name;   /* Task name */
    char *task_schema; /* Schema containing task */
    char *description; /* Description */

    /* SQL to execute */
    char *sql_text;    /* SQL statement */
    bool is_procedure; /* Is stored procedure call */

    /* Schedule */
    TaskSchedule *schedule; /* Schedule configuration */

    /* Condition */
    TaskCondition *condition; /* When condition */

    /* Dependencies (for DAG) */
    int64 *predecessor_ids;   /* Predecessor task IDs */
    char **predecessor_names; /* Predecessor task names */
    int num_predecessors;     /* Number of predecessors */

    /* Execution settings */
    char *warehouse;             /* Compute warehouse */
    int timeout_seconds;         /* Execution timeout */
    bool allow_overlapping;      /* Allow concurrent runs */
    int error_limit;             /* Max consecutive failures */
    bool suspend_after_failures; /* Suspend after error limit */

    /* State */
    TaskState state;          /* Current state */
    int consecutive_failures; /* Current failure count */

    /* Timestamps */
    TimestampTz created_at;  /* Creation time */
    TimestampTz last_run_at; /* Last execution time */
    TimestampTz next_run_at; /* Next scheduled run */

    /* Owner */
    Oid owner_oid;    /* Owner user OID */
    char *owner_name; /* Owner username */

    /* Statistics */
    int64 total_runs;       /* Total executions */
    int64 successful_runs;  /* Successful runs */
    int64 failed_runs;      /* Failed runs */
    int64 skipped_runs;     /* Skipped runs */
    double avg_duration_ms; /* Average duration */

    /* Memory context */
    MemoryContext task_context; /* Memory context */
} Task;

/* ============================================================
 * Task Graph (DAG) Structure
 * ============================================================ */

typedef struct TaskGraph {
    int64 root_task_id; /* Root task ID */
    int num_tasks;      /* Total tasks in graph */
    Task **tasks;       /* All tasks in graph */
    int **edges;        /* Adjacency matrix */

    /* Execution state */
    int *execution_order; /* Topological order */
    bool *completed;      /* Task completion flags */
} TaskGraph;

/* ============================================================
 * Function Prototypes - Task Management
 * ============================================================ */

/* Task lifecycle */
extern int64 orochi_create_task(const char *name, const char *sql, TaskSchedule *schedule,
                                TaskCondition *condition);
extern bool orochi_drop_task(int64 task_id, bool if_exists);
extern bool orochi_alter_task(int64 task_id, const char *alterations);

/* Task execution */
extern int64 orochi_execute_task(int64 task_id);
extern bool orochi_cancel_task_run(int64 run_id);

/* Task state management */
extern bool orochi_resume_task(int64 task_id);
extern bool orochi_suspend_task(int64 task_id);
extern TaskState orochi_get_task_state(int64 task_id);

/* Task configuration */
extern Task *orochi_get_task(int64 task_id);
extern Task *orochi_get_task_by_name(const char *schema, const char *name);
extern List *orochi_list_tasks(void);
extern List *orochi_list_tasks_in_schema(const char *schema);

/* Task dependencies */
extern bool orochi_add_task_predecessor(int64 task_id, int64 predecessor_id);
extern bool orochi_remove_task_predecessor(int64 task_id, int64 predecessor_id);
extern List *orochi_get_task_predecessors(int64 task_id);
extern List *orochi_get_task_successors(int64 task_id);

/* Task runs */
extern TaskRunStats *orochi_get_task_run(int64 run_id);
extern List *orochi_get_task_runs(int64 task_id, int limit);
extern List *orochi_get_pending_task_runs(void);

/* ============================================================
 * Function Prototypes - DDL Parsing
 * ============================================================ */

/* Parse CREATE TASK statement */
extern Task *ddl_parse_create_task(const char *sql);

/* Parse schedule expression */
extern TaskSchedule *ddl_parse_schedule(const char *schedule_str);
extern TaskSchedule *ddl_parse_cron_schedule(const char *cron_expr, const char *timezone);
extern TaskSchedule *ddl_parse_interval_schedule(const char *interval_str);

/* Parse task condition */
extern TaskCondition *ddl_parse_task_condition(const char *when_clause);

/* Validate task definition */
extern bool ddl_validate_task(Task *task, char **error_msg);

/* ============================================================
 * Function Prototypes - Catalog Operations
 * ============================================================ */

/* Store task in catalog */
extern void ddl_catalog_store_task(Task *task);
extern void ddl_catalog_update_task(Task *task);
extern void ddl_catalog_delete_task(int64 task_id);

/* Load task from catalog */
extern Task *ddl_catalog_load_task(int64 task_id);
extern Task *ddl_catalog_load_task_by_name(const char *schema, const char *name);

/* Task run operations */
extern int64 ddl_catalog_create_task_run(int64 task_id, TimestampTz scheduled);
extern void ddl_catalog_update_task_run(TaskRunStats *stats);
extern void ddl_catalog_finish_task_run(int64 run_id, TaskRunState state, const char *error_msg);

/* ============================================================
 * Function Prototypes - Scheduler
 * ============================================================ */

/* Initialize task scheduler */
extern void task_scheduler_init(void);
extern void task_scheduler_shutdown(void);

/* Calculate next run time */
extern TimestampTz task_calculate_next_run(Task *task);
extern TimestampTz cron_next_occurrence(const char *cron_expr, const char *timezone,
                                        TimestampTz from);

/* Check if task should run */
extern bool task_should_execute(Task *task);
extern bool task_condition_evaluate(TaskCondition *condition);

/* Task graph operations */
extern TaskGraph *task_build_graph(int64 root_task_id);
extern bool task_graph_validate(TaskGraph *graph, char **error_msg);
extern int *task_graph_topological_sort(TaskGraph *graph);
extern void task_graph_free(TaskGraph *graph);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_alter_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_resume_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_suspend_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_execute_task_sql(PG_FUNCTION_ARGS);
extern Datum orochi_task_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_tasks_sql(PG_FUNCTION_ARGS);
extern Datum orochi_task_runs_sql(PG_FUNCTION_ARGS);
extern Datum orochi_task_dependents_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *task_state_name(TaskState state);
extern TaskState task_parse_state(const char *name);
extern const char *task_run_state_name(TaskRunState state);
extern TaskRunState task_parse_run_state(const char *name);
extern const char *schedule_type_name(ScheduleType type);

/* Cron parsing utilities */
extern bool cron_expression_validate(const char *expr, char **error_msg);
extern char *cron_expression_describe(const char *expr);

/* Free task structures */
extern void task_free(Task *task);
extern void task_schedule_free(TaskSchedule *schedule);
extern void task_condition_free(TaskCondition *condition);
extern void task_run_stats_free(TaskRunStats *stats);

/* ============================================================
 * Constants
 * ============================================================ */

#define TASK_MAX_NAME_LENGTH       128
#define TASK_MAX_SQL_LENGTH        65536
#define TASK_DEFAULT_TIMEOUT       3600 /* 1 hour */
#define TASK_MAX_PREDECESSORS      100
#define TASK_DEFAULT_ERROR_LIMIT   3
#define TASK_SCHEDULER_INTERVAL_MS 1000 /* 1 second */

/* Catalog table names */
#define OROCHI_TASKS_TABLE     "orochi_tasks"
#define OROCHI_TASK_DEPS_TABLE "orochi_task_dependencies"
#define OROCHI_TASK_RUNS_TABLE "orochi_task_runs"

/* Special condition functions */
#define TASK_COND_STREAM_HAS_DATA "SYSTEM$STREAM_HAS_DATA"

#endif /* OROCHI_DDL_TASK_H */
