/*-------------------------------------------------------------------------
 *
 * ddl_task.c
 *    Implementation of Scheduled Task DDL for Orochi DB
 *
 * This module handles:
 *   - Parsing CREATE TASK statements
 *   - Storing task definitions in catalog
 *   - Task scheduling and execution (stub)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "catalog/namespace.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../core/catalog.h"
#include "ddl_stream.h"
#include "ddl_task.h"

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

/*
 * Allocate a new TaskSchedule
 */
static TaskSchedule *task_schedule_alloc(void) {
  TaskSchedule *schedule = palloc0(sizeof(TaskSchedule));
  schedule->type = SCHEDULE_TYPE_CRON;
  schedule->timezone = pstrdup("UTC");
  return schedule;
}

/*
 * Allocate a new TaskCondition
 */
static TaskCondition *task_condition_alloc(void) {
  TaskCondition *condition = palloc0(sizeof(TaskCondition));
  condition->type = TASK_CONDITION_NONE;
  return condition;
}

/*
 * Allocate a new Task
 */
static Task *task_alloc(void) {
  Task *task = palloc0(sizeof(Task));
  task->state = TASK_STATE_CREATED;
  task->timeout_seconds = TASK_DEFAULT_TIMEOUT;
  task->allow_overlapping = false;
  task->error_limit = TASK_DEFAULT_ERROR_LIMIT;
  task->suspend_after_failures = true;
  task->created_at = GetCurrentTimestamp();
  return task;
}

/* ============================================================
 * Cron Parsing Functions
 * ============================================================ */

/*
 * Validate a cron expression
 *
 * Format: MIN HOUR DAY MONTH WEEKDAY
 * Each field can be: * (any), number, range (1-5), list (1,3,5), step (* /5)
 */
bool cron_expression_validate(const char *expr, char **error_msg) {
  int field_count = 0;
  const char *p = expr;

  if (expr == NULL || strlen(expr) == 0) {
    *error_msg = pstrdup("cron expression is empty");
    return false;
  }

  /* Count fields (should be 5 for standard cron) */
  while (*p) {
    /* Skip whitespace */
    while (*p && isspace(*p))
      p++;
    if (*p == '\0')
      break;

    field_count++;

    /* Validate characters in field */
    while (*p && !isspace(*p)) {
      if (!isdigit(*p) && *p != '*' && *p != '-' && *p != '/' && *p != ',') {
        *error_msg = psprintf("invalid character '%c' in cron expression", *p);
        return false;
      }
      p++;
    }
  }

  if (field_count != 5) {
    *error_msg =
        psprintf("cron expression must have 5 fields, found %d", field_count);
    return false;
  }

  *error_msg = NULL;
  return true;
}

/*
 * Describe a cron expression in human-readable form
 */
char *cron_expression_describe(const char *expr) {
  /* Simple descriptions for common patterns */
  if (strcmp(expr, "* * * * *") == 0)
    return pstrdup("every minute");
  if (strcmp(expr, "0 * * * *") == 0)
    return pstrdup("every hour");
  if (strcmp(expr, "0 0 * * *") == 0)
    return pstrdup("every day at midnight");
  if (strcmp(expr, "0 0 * * 0") == 0)
    return pstrdup("every Sunday at midnight");
  if (strcmp(expr, "0 0 1 * *") == 0)
    return pstrdup("first day of every month");

  return pstrdup(expr);
}

/*
 * Parse a cron field and return the next matching value
 * Returns -1 if no match is found in the valid range
 */
static int cron_parse_field(const char *field, int current, int min_val,
                            int max_val) {
  const char *p = field;
  int step = 1;
  int range_start = min_val;
  int range_end = max_val;

  /* Handle wildcard */
  if (*p == '*') {
    p++;
    if (*p == '/') {
      p++;
      step = atoi(p);
    }
    /* Find next value >= current that matches the step */
    for (int v = current; v <= max_val; v++) {
      if ((v - min_val) % step == 0)
        return v;
    }
    return min_val; /* Wrap around */
  }

  /* Handle list (comma-separated values) */
  while (*p) {
    int val = atoi(p);
    if (val >= current && val <= max_val)
      return val;

    /* Skip to next comma or end */
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }

  /* No match found - return first value for wrap around */
  return atoi(field);
}

/*
 * Calculate next occurrence of a cron expression
 *
 * Cron format: MIN HOUR DAY MONTH WEEKDAY
 *   MIN: 0-59
 *   HOUR: 0-23
 *   DAY: 1-31
 *   MONTH: 1-12
 *   WEEKDAY: 0-6 (0=Sunday)
 */
TimestampTz cron_next_occurrence(const char *cron_expr, const char *timezone,
                                 TimestampTz from) {
  char fields[5][64];
  int field_idx = 0;
  const char *p = cron_expr;
  int i = 0;
  struct pg_tm tm_from;
  fsec_t fsec;
  int tz_offset;
  TimestampTz next;
  int next_minute, next_hour, next_day, next_month;
  int year;
  int iterations = 0;
  const int max_iterations = 366 * 24 * 60; /* Max 1 year search */

  /* Parse cron expression into 5 fields */
  memset(fields, 0, sizeof(fields));

  while (*p && field_idx < 5) {
    while (*p && isspace(*p))
      p++;

    i = 0;
    while (*p && !isspace(*p) && i < 63)
      fields[field_idx][i++] = *p++;
    fields[field_idx][i] = '\0';

    field_idx++;
  }

  /* If we don't have 5 fields, return default (1 hour from now) */
  if (field_idx < 5) {
    Interval one_hour;
    one_hour.time = 3600 * USECS_PER_SEC;
    one_hour.day = 0;
    one_hour.month = 0;

    return DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval,
                                                   TimestampTzGetDatum(from),
                                                   PointerGetDatum(&one_hour)));
  }

  /* Convert from timestamp to struct */
  if (timestamp2tm(from, &tz_offset, &tm_from, &fsec, NULL, NULL) != 0) {
    /* Fallback on conversion error */
    Interval one_hour;
    one_hour.time = 3600 * USECS_PER_SEC;
    one_hour.day = 0;
    one_hour.month = 0;

    return DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval,
                                                   TimestampTzGetDatum(from),
                                                   PointerGetDatum(&one_hour)));
  }

  /* Start from next minute */
  tm_from.tm_min++;
  if (tm_from.tm_min >= 60) {
    tm_from.tm_min = 0;
    tm_from.tm_hour++;
  }
  if (tm_from.tm_hour >= 24) {
    tm_from.tm_hour = 0;
    tm_from.tm_mday++;
  }

  year = tm_from.tm_year;

  /* Find next matching time */
  while (iterations < max_iterations) {
    iterations++;

    /* Check month */
    next_month = cron_parse_field(fields[3], tm_from.tm_mon, 1, 12);
    if (next_month > tm_from.tm_mon) {
      tm_from.tm_mon = next_month;
      tm_from.tm_mday = 1;
      tm_from.tm_hour = 0;
      tm_from.tm_min = 0;
    } else if (next_month < tm_from.tm_mon) {
      tm_from.tm_year++;
      tm_from.tm_mon = next_month;
      tm_from.tm_mday = 1;
      tm_from.tm_hour = 0;
      tm_from.tm_min = 0;
      continue;
    }

    /* Check day */
    next_day = cron_parse_field(fields[2], tm_from.tm_mday, 1, 31);
    if (next_day > tm_from.tm_mday) {
      tm_from.tm_mday = next_day;
      tm_from.tm_hour = 0;
      tm_from.tm_min = 0;
    } else if (next_day < tm_from.tm_mday) {
      tm_from.tm_mon++;
      if (tm_from.tm_mon > 12) {
        tm_from.tm_mon = 1;
        tm_from.tm_year++;
      }
      tm_from.tm_mday = 1;
      tm_from.tm_hour = 0;
      tm_from.tm_min = 0;
      continue;
    }

    /* Check hour */
    next_hour = cron_parse_field(fields[1], tm_from.tm_hour, 0, 23);
    if (next_hour > tm_from.tm_hour) {
      tm_from.tm_hour = next_hour;
      tm_from.tm_min = 0;
    } else if (next_hour < tm_from.tm_hour) {
      tm_from.tm_mday++;
      tm_from.tm_hour = 0;
      tm_from.tm_min = 0;
      continue;
    }

    /* Check minute */
    next_minute = cron_parse_field(fields[0], tm_from.tm_min, 0, 59);
    if (next_minute >= tm_from.tm_min) {
      tm_from.tm_min = next_minute;
      break; /* Found a match */
    } else {
      tm_from.tm_hour++;
      if (tm_from.tm_hour >= 24) {
        tm_from.tm_hour = 0;
        tm_from.tm_mday++;
      }
      tm_from.tm_min = 0;
      continue;
    }
  }

  /* Convert back to timestamp */
  tm_from.tm_sec = 0;
  if (tm2timestamp(&tm_from, 0, &tz_offset, &next) != 0) {
    /* Fallback on conversion error */
    Interval one_hour;
    one_hour.time = 3600 * USECS_PER_SEC;
    one_hour.day = 0;
    one_hour.month = 0;

    return DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval,
                                                   TimestampTzGetDatum(from),
                                                   PointerGetDatum(&one_hour)));
  }

  return next;
}

/* ============================================================
 * DDL Parsing Functions
 * ============================================================ */

/*
 * Parse SCHEDULE clause
 */
TaskSchedule *ddl_parse_schedule(const char *schedule_str) {
  TaskSchedule *schedule;
  const char *p;

  schedule = task_schedule_alloc();

  p = schedule_str;
  while (*p && isspace(*p))
    p++;

  /* Check for USING CRON format */
  if (strncasecmp(p, "USING CRON", 10) == 0) {
    p += 10;
    while (*p && isspace(*p))
      p++;

    schedule->type = SCHEDULE_TYPE_CRON;
    schedule->cron_expression = pstrdup(p);

    return schedule;
  }

  /* Check for interval format (e.g., "5 MINUTE", "1 HOUR", "30 SECONDS") */
  if (isdigit(*p)) {
    int value;
    char unit[32];
    int unit_len = 0;

    schedule->type = SCHEDULE_TYPE_INTERVAL;
    schedule->interval = palloc0(sizeof(Interval));

    /* Parse numeric value */
    value = atoi(p);
    while (*p && isdigit(*p))
      p++;
    while (*p && isspace(*p))
      p++;

    /* Parse unit */
    while (*p && !isspace(*p) && unit_len < 31)
      unit[unit_len++] = tolower(*p++);
    unit[unit_len] = '\0';

    /* Handle plural forms */
    if (unit_len > 0 && unit[unit_len - 1] == 's')
      unit[unit_len - 1] = '\0';

    /* Convert to interval */
    if (strcmp(unit, "second") == 0) {
      schedule->interval->time = value * USECS_PER_SEC;
    } else if (strcmp(unit, "minute") == 0) {
      schedule->interval->time = value * 60 * USECS_PER_SEC;
    } else if (strcmp(unit, "hour") == 0) {
      schedule->interval->time = value * 3600 * USECS_PER_SEC;
    } else if (strcmp(unit, "day") == 0) {
      schedule->interval->day = value;
    } else if (strcmp(unit, "week") == 0) {
      schedule->interval->day = value * 7;
    } else if (strcmp(unit, "month") == 0) {
      schedule->interval->month = value;
    } else {
      /* Default to minutes if unit not recognized */
      schedule->interval->time = value * 60 * USECS_PER_SEC;
    }

    return schedule;
  }

  /* Default: treat as cron expression */
  schedule->type = SCHEDULE_TYPE_CRON;
  schedule->cron_expression = pstrdup(p);

  return schedule;
}

/*
 * Parse cron schedule with timezone
 */
TaskSchedule *ddl_parse_cron_schedule(const char *cron_expr,
                                      const char *timezone) {
  TaskSchedule *schedule;

  schedule = task_schedule_alloc();
  schedule->type = SCHEDULE_TYPE_CRON;
  schedule->cron_expression = pstrdup(cron_expr);
  schedule->timezone = timezone ? pstrdup(timezone) : pstrdup("UTC");

  return schedule;
}

/*
 * Parse interval schedule
 */
TaskSchedule *ddl_parse_interval_schedule(const char *interval_str) {
  TaskSchedule *schedule;

  schedule = task_schedule_alloc();
  schedule->type = SCHEDULE_TYPE_INTERVAL;

  /* Parse interval (e.g., "5 MINUTE", "1 HOUR") */
  schedule->interval = palloc0(sizeof(Interval));

  /* Simple parsing - just handle common cases */
  if (strstr(interval_str, "MINUTE") || strstr(interval_str, "minute")) {
    int minutes = atoi(interval_str);
    schedule->interval->time = minutes * 60 * USECS_PER_SEC;
  } else if (strstr(interval_str, "HOUR") || strstr(interval_str, "hour")) {
    int hours = atoi(interval_str);
    schedule->interval->time = hours * 3600 * USECS_PER_SEC;
  } else if (strstr(interval_str, "DAY") || strstr(interval_str, "day")) {
    int days = atoi(interval_str);
    schedule->interval->day = days;
  }

  return schedule;
}

/*
 * Parse WHEN condition clause
 */
TaskCondition *ddl_parse_task_condition(const char *when_clause) {
  TaskCondition *condition;
  const char *p;

  condition = task_condition_alloc();

  if (when_clause == NULL) {
    condition->type = TASK_CONDITION_NONE;
    return condition;
  }

  p = when_clause;
  while (*p && isspace(*p))
    p++;

  /* Check for SYSTEM$STREAM_HAS_DATA */
  if (strncasecmp(p, TASK_COND_STREAM_HAS_DATA,
                  strlen(TASK_COND_STREAM_HAS_DATA)) == 0) {
    condition->type = TASK_CONDITION_STREAM;
    p += strlen(TASK_COND_STREAM_HAS_DATA);

    /* Parse stream name from parentheses */
    while (*p && *p != '(')
      p++;
    if (*p == '(') {
      p++;
      const char *start = p;
      while (*p && *p != ')')
        p++;

      /* Remove quotes if present */
      if (*start == '\'')
        start++;
      size_t len = p - start;
      if (len > 0 && start[len - 1] == '\'')
        len--;

      condition->stream_name = pnstrdup(start, len);
    }

    return condition;
  }

  /* Default: custom expression */
  condition->type = TASK_CONDITION_EXPRESSION;
  condition->expression = pstrdup(when_clause);

  return condition;
}

/*
 * Parse CREATE TASK statement
 *
 * Format:
 *   CREATE TASK name
 *     [SCHEDULE = 'schedule_expr']
 *     [WHEN condition]
 *     [AFTER predecessor, ...]
 *     AS sql_statement
 */
Task *ddl_parse_create_task(const char *sql) {
  Task *task;
  const char *p;
  size_t len;

  task = task_alloc();

  p = sql;

  /* Skip CREATE TASK */
  if (strncasecmp(p, "CREATE", 6) == 0)
    p += 6;
  while (*p && isspace(*p))
    p++;
  if (strncasecmp(p, "TASK", 4) == 0)
    p += 4;
  while (*p && isspace(*p))
    p++;

  /* Parse task name */
  len = 0;
  while (p[len] && !isspace(p[len]))
    len++;

  task->task_name = pnstrdup(p, len);
  p += len;

  /* Parse optional clauses */
  while (*p) {
    while (*p && isspace(*p))
      p++;

    /* SCHEDULE clause */
    if (strncasecmp(p, "SCHEDULE", 8) == 0) {
      p += 8;
      while (*p && (*p == '=' || isspace(*p)))
        p++;

      /* Find schedule value (quoted or until next keyword) */
      if (*p == '\'') {
        p++;
        const char *start = p;
        while (*p && *p != '\'')
          p++;

        task->schedule = ddl_parse_schedule(pnstrdup(start, p - start));
        if (*p == '\'')
          p++;
      }
      continue;
    }

    /* WHEN clause */
    if (strncasecmp(p, "WHEN", 4) == 0) {
      p += 4;
      while (*p && isspace(*p))
        p++;

      /* Find condition (until AS keyword) */
      const char *start = p;
      while (*p && strncasecmp(p, " AS ", 4) != 0)
        p++;

      task->condition = ddl_parse_task_condition(pnstrdup(start, p - start));
      continue;
    }

    /* AFTER clause (dependencies) */
    if (strncasecmp(p, "AFTER", 5) == 0) {
      p += 5;
      while (*p && isspace(*p))
        p++;

      /* Parse comma-separated task names */
      List *deps = NIL;
      while (*p && strncasecmp(p, "AS", 2) != 0 &&
             strncasecmp(p, "SCHEDULE", 8) != 0 &&
             strncasecmp(p, "WHEN", 4) != 0) {
        while (*p && isspace(*p))
          p++;

        const char *start = p;
        while (*p && *p != ',' && !isspace(*p))
          p++;

        if (p > start)
          deps = lappend(deps, pnstrdup(start, p - start));

        while (*p && (*p == ',' || isspace(*p)))
          p++;
      }

      /* Convert list to arrays */
      if (list_length(deps) > 0) {
        ListCell *lc;
        int i = 0;

        task->num_predecessors = list_length(deps);
        task->predecessor_names =
            palloc(sizeof(char *) * task->num_predecessors);

        foreach (lc, deps) {
          task->predecessor_names[i++] = (char *)lfirst(lc);
        }
      }
      continue;
    }

    /* AS clause (SQL statement) */
    if (strncasecmp(p, "AS", 2) == 0) {
      p += 2;
      while (*p && isspace(*p))
        p++;

      /* Rest is the SQL statement */
      task->sql_text = pstrdup(p);
      break;
    }

    /* Unknown token - skip */
    p++;
  }

  /* Set default schedule if none provided */
  if (task->schedule == NULL && task->num_predecessors == 0) {
    task->schedule = task_schedule_alloc();
    task->schedule->type = SCHEDULE_TYPE_CRON;
    task->schedule->cron_expression = pstrdup("0 * * * *"); /* Every hour */
  }

  return task;
}

/*
 * Validate task definition
 */
bool ddl_validate_task(Task *task, char **error_msg) {
  char *cron_error;

  if (task == NULL) {
    *error_msg = pstrdup("task is NULL");
    return false;
  }

  if (task->task_name == NULL || strlen(task->task_name) == 0) {
    *error_msg = pstrdup("task name is required");
    return false;
  }

  if (strlen(task->task_name) > TASK_MAX_NAME_LENGTH) {
    *error_msg = psprintf("task name exceeds maximum length of %d",
                          TASK_MAX_NAME_LENGTH);
    return false;
  }

  if (task->sql_text == NULL || strlen(task->sql_text) == 0) {
    *error_msg = pstrdup("task SQL statement is required");
    return false;
  }

  if (strlen(task->sql_text) > TASK_MAX_SQL_LENGTH) {
    *error_msg =
        psprintf("task SQL exceeds maximum length of %d", TASK_MAX_SQL_LENGTH);
    return false;
  }

  /* Validate schedule if present */
  if (task->schedule != NULL && task->schedule->type == SCHEDULE_TYPE_CRON &&
      task->schedule->cron_expression != NULL) {
    if (!cron_expression_validate(task->schedule->cron_expression,
                                  &cron_error)) {
      *error_msg = cron_error;
      return false;
    }
  }

  *error_msg = NULL;
  return true;
}

/* ============================================================
 * Catalog Operations
 * ============================================================ */

/*
 * Store task in catalog
 */
void ddl_catalog_store_task(Task *task) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "INSERT INTO orochi.%s "
      "(task_name, task_schema, sql_text, schedule_type, cron_expression, "
      "timeout_seconds, state, error_limit, created_at) "
      "VALUES ('%s', '%s', '%s', %d, %s, %d, %d, %d, NOW()) "
      "RETURNING task_id",
      OROCHI_TASKS_TABLE, task->task_name,
      task->task_schema ? task->task_schema : "public", task->sql_text,
      task->schedule ? (int)task->schedule->type : 0,
      (task->schedule && task->schedule->cron_expression)
          ? psprintf("'%s'", task->schedule->cron_expression)
          : "NULL",
      task->timeout_seconds, (int)task->state, task->error_limit);

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);

  if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
    bool isnull;
    Datum id_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      task->task_id = DatumGetInt64(id_datum);
  }

  /* Store task dependencies */
  for (int i = 0; i < task->num_predecessors; i++) {
    resetStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO orochi.%s "
                     "(task_id, predecessor_name) "
                     "VALUES (%ld, '%s')",
                     OROCHI_TASK_DEPS_TABLE, task->task_id,
                     task->predecessor_names[i]);

    SPI_execute(query.data, false, 0);
  }

  SPI_finish();
  pfree(query.data);
}

/*
 * Update task in catalog
 */
void ddl_catalog_update_task(Task *task) {
  StringInfoData query;

  initStringInfo(&query);

  appendStringInfo(&query,
                   "UPDATE orochi.%s SET "
                   "sql_text = '%s', "
                   "state = %d, "
                   "consecutive_failures = %d, "
                   "last_run_at = %s, "
                   "next_run_at = %s "
                   "WHERE task_id = %ld",
                   OROCHI_TASKS_TABLE, task->sql_text, (int)task->state,
                   task->consecutive_failures,
                   task->last_run_at
                       ? psprintf("'%s'", timestamptz_to_str(task->last_run_at))
                       : "NULL",
                   task->next_run_at
                       ? psprintf("'%s'", timestamptz_to_str(task->next_run_at))
                       : "NULL",
                   task->task_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);
}

/*
 * Delete task from catalog
 */
void ddl_catalog_delete_task(int64 task_id) {
  StringInfoData query;

  initStringInfo(&query);

  /* Delete dependencies first */
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE task_id = %ld",
                   OROCHI_TASK_DEPS_TABLE, task_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);

  /* Delete runs */
  resetStringInfo(&query);
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE task_id = %ld",
                   OROCHI_TASK_RUNS_TABLE, task_id);
  SPI_execute(query.data, false, 0);

  /* Delete task */
  resetStringInfo(&query);
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE task_id = %ld",
                   OROCHI_TASKS_TABLE, task_id);
  SPI_execute(query.data, false, 0);

  SPI_finish();
  pfree(query.data);
}

/*
 * Create a task run record
 */
int64 ddl_catalog_create_task_run(int64 task_id, TimestampTz scheduled) {
  StringInfoData query;
  int64 run_id = 0;
  int ret;

  initStringInfo(&query);

  appendStringInfo(&query,
                   "INSERT INTO orochi.%s "
                   "(task_id, state, scheduled_time, started_at) "
                   "VALUES (%ld, %d, '%s', NOW()) "
                   "RETURNING run_id",
                   OROCHI_TASK_RUNS_TABLE, task_id, (int)TASK_RUN_RUNNING,
                   timestamptz_to_str(scheduled));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);

  if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
    bool isnull;
    Datum id_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      run_id = DatumGetInt64(id_datum);
  }

  SPI_finish();
  pfree(query.data);

  return run_id;
}

/*
 * Finish a task run
 */
void ddl_catalog_finish_task_run(int64 run_id, TaskRunState state,
                                 const char *error_msg) {
  StringInfoData query;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "UPDATE orochi.%s SET "
      "state = %d, "
      "finished_at = NOW(), "
      "duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
      "error_message = %s "
      "WHERE run_id = %ld",
      OROCHI_TASK_RUNS_TABLE, (int)state,
      error_msg ? psprintf("'%s'", error_msg) : "NULL", run_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);
}

/* ============================================================
 * Task Management Functions
 * ============================================================ */

/*
 * Create a task
 */
int64 orochi_create_task(const char *name, const char *sql,
                         TaskSchedule *schedule, TaskCondition *condition) {
  Task *task;
  char *error_msg;

  task = task_alloc();
  task->task_name = pstrdup(name);
  task->sql_text = pstrdup(sql);
  task->schedule = schedule;
  task->condition = condition;
  task->task_schema = pstrdup("public");

  /* Validate */
  if (!ddl_validate_task(task, &error_msg)) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid task definition: %s", error_msg)));
  }

  /* Store in catalog */
  ddl_catalog_store_task(task);

  elog(LOG, "created task '%s' with ID %ld", task->task_name, task->task_id);

  return task->task_id;
}

/*
 * Drop a task
 */
bool orochi_drop_task(int64 task_id, bool if_exists) {
  ddl_catalog_delete_task(task_id);
  elog(LOG, "dropped task with ID %ld", task_id);
  return true;
}

/*
 * Load task from catalog
 */
static Task *task_load_from_catalog(int64 task_id) {
  StringInfoData query;
  Task *task = NULL;
  int ret;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "SELECT task_name, task_schema, sql_text, schedule_type, "
      "       cron_expression, timeout_seconds, state, error_limit, "
      "       consecutive_failures "
      "FROM orochi.%s "
      "WHERE task_id = %ld",
      OROCHI_TASKS_TABLE, task_id);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    task = palloc0(sizeof(Task));
    task->task_id = task_id;

    Datum name_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
    if (!isnull)
      task->task_name = pstrdup(TextDatumGetCString(name_datum));

    Datum schema_datum = SPI_getbinval(tuple, tupdesc, 2, &isnull);
    if (!isnull)
      task->task_schema = pstrdup(TextDatumGetCString(schema_datum));

    Datum sql_datum = SPI_getbinval(tuple, tupdesc, 3, &isnull);
    if (!isnull)
      task->sql_text = pstrdup(TextDatumGetCString(sql_datum));

    Datum state_datum = SPI_getbinval(tuple, tupdesc, 7, &isnull);
    if (!isnull)
      task->state = (TaskState)DatumGetInt32(state_datum);

    Datum failures_datum = SPI_getbinval(tuple, tupdesc, 9, &isnull);
    if (!isnull)
      task->consecutive_failures = DatumGetInt32(failures_datum);
  }

  SPI_finish();
  pfree(query.data);

  return task;
}

/*
 * Execute a task
 */
int64 orochi_execute_task(int64 task_id) {
  int64 run_id;
  Task *task;
  int ret;
  TaskRunState final_state = TASK_RUN_SUCCEEDED;
  char *error_msg = NULL;
  StringInfoData update_query;

  /* Load task definition */
  task = task_load_from_catalog(task_id);
  if (task == NULL) {
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("task with ID %ld does not exist", task_id)));
  }

  /* Check if task is in a runnable state */
  if (task->state == TASK_STATE_SUSPENDED || task->state == TASK_STATE_FAILED) {
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("task %s is not in a runnable state", task->task_name)));
  }

  /* Create run record */
  run_id = ddl_catalog_create_task_run(task_id, GetCurrentTimestamp());

  elog(LOG, "started task %ld (%s) run %ld", task_id, task->task_name, run_id);

  /* Check condition if any */
  if (task->condition != NULL && !task_condition_evaluate(task->condition)) {
    /* Condition not met, skip execution */
    ddl_catalog_finish_task_run(run_id, TASK_RUN_SKIPPED, "Condition not met");
    elog(LOG, "skipped task %ld run %ld - condition not met", task_id, run_id);
    task_free(task);
    return run_id;
  }

  /* Execute the SQL statement */
  SPI_connect();

  PG_TRY();
  {
    ret = SPI_execute(task->sql_text, false, 0);

    if (ret < 0) {
      final_state = TASK_RUN_FAILED;
      error_msg = psprintf("SPI execution failed with code %d", ret);
    } else {
      final_state = TASK_RUN_SUCCEEDED;
      elog(LOG, "task %ld run %ld completed successfully, %lu rows affected",
           task_id, run_id, (unsigned long)SPI_processed);
    }
  }
  PG_CATCH();
  {
    /* Capture error information */
    ErrorData *edata = CopyErrorData();
    FlushErrorState();

    final_state = TASK_RUN_FAILED;
    error_msg = pstrdup(edata->message);
    FreeErrorData(edata);
  }
  PG_END_TRY();

  SPI_finish();

  /* Update run status */
  ddl_catalog_finish_task_run(run_id, final_state, error_msg);

  /* Update task state based on run result */
  initStringInfo(&update_query);

  if (final_state == TASK_RUN_SUCCEEDED) {
    /* Reset consecutive failures on success */
    appendStringInfo(
        &update_query,
        "UPDATE orochi.%s SET "
        "consecutive_failures = 0, "
        "last_run_at = NOW(), "
        "next_run_at = %s "
        "WHERE task_id = %ld",
        OROCHI_TASKS_TABLE,
        "NOW() + INTERVAL '1 hour'", /* Will be recalculated by scheduler */
        task_id);
  } else {
    /* Increment consecutive failures */
    appendStringInfo(&update_query,
                     "UPDATE orochi.%s SET "
                     "consecutive_failures = consecutive_failures + 1, "
                     "state = CASE WHEN consecutive_failures + 1 >= "
                     "error_limit THEN %d ELSE state END, "
                     "last_run_at = NOW() "
                     "WHERE task_id = %ld",
                     OROCHI_TASKS_TABLE, (int)TASK_STATE_SUSPENDED, task_id);
  }

  SPI_connect();
  SPI_execute(update_query.data, false, 0);
  SPI_finish();

  pfree(update_query.data);
  if (error_msg)
    pfree(error_msg);
  task_free(task);

  return run_id;
}

/*
 * Resume a task
 */
bool orochi_resume_task(int64 task_id) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);

  /* Update task state to STARTED and reset failure count */
  appendStringInfo(&query,
                   "UPDATE orochi.%s SET "
                   "state = %d, "
                   "consecutive_failures = 0, "
                   "next_run_at = NOW() "
                   "WHERE task_id = %ld "
                   "RETURNING task_name",
                   OROCHI_TASKS_TABLE, (int)TASK_STATE_STARTED, task_id);

  SPI_connect();
  ret = SPI_execute(query.data, false, 1);

  if (ret != SPI_OK_UPDATE_RETURNING || SPI_processed == 0) {
    SPI_finish();
    pfree(query.data);
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("task with ID %ld does not exist", task_id)));
  }

  SPI_finish();
  pfree(query.data);

  elog(LOG, "resumed task %ld", task_id);
  return true;
}

/*
 * Suspend a task
 */
bool orochi_suspend_task(int64 task_id) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);

  /* Update task state to SUSPENDED */
  appendStringInfo(&query,
                   "UPDATE orochi.%s SET "
                   "state = %d, "
                   "next_run_at = NULL "
                   "WHERE task_id = %ld "
                   "RETURNING task_name",
                   OROCHI_TASKS_TABLE, (int)TASK_STATE_SUSPENDED, task_id);

  SPI_connect();
  ret = SPI_execute(query.data, false, 1);

  if (ret != SPI_OK_UPDATE_RETURNING || SPI_processed == 0) {
    SPI_finish();
    pfree(query.data);
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("task with ID %ld does not exist", task_id)));
  }

  SPI_finish();
  pfree(query.data);

  elog(LOG, "suspended task %ld", task_id);
  return true;
}

/*
 * Calculate next run time for a task
 */
TimestampTz task_calculate_next_run(Task *task) {
  if (task->schedule == NULL)
    return 0;

  switch (task->schedule->type) {
  case SCHEDULE_TYPE_CRON:
    return cron_next_occurrence(task->schedule->cron_expression,
                                task->schedule->timezone,
                                GetCurrentTimestamp());

  case SCHEDULE_TYPE_INTERVAL:
    if (task->schedule->interval) {
      return DatumGetTimestampTz(DirectFunctionCall2(
          timestamptz_pl_interval, TimestampTzGetDatum(GetCurrentTimestamp()),
          PointerGetDatum(task->schedule->interval)));
    }
    break;

  case SCHEDULE_TYPE_TRIGGER:
    /* Triggered by predecessor - no scheduled time */
    return 0;
  }

  return 0;
}

/*
 * Check if task should execute
 */
bool task_should_execute(Task *task) {
  if (task->state != TASK_STATE_STARTED)
    return false;

  /* Check condition if present */
  if (task->condition != NULL)
    return task_condition_evaluate(task->condition);

  return true;
}

/*
 * Evaluate task condition
 */
bool task_condition_evaluate(TaskCondition *condition) {
  if (condition == NULL || condition->type == TASK_CONDITION_NONE)
    return true;

  switch (condition->type) {
  case TASK_CONDITION_STREAM:
    /* Check if stream has data using the stream functions */
    if (condition->stream_id > 0)
      return orochi_stream_has_data(condition->stream_id);
    else if (condition->stream_name != NULL) {
      /* Look up stream by name to get ID */
      StringInfoData query;
      int64 stream_id = 0;
      int ret;

      initStringInfo(&query);
      appendStringInfo(
          &query, "SELECT stream_id FROM orochi.%s WHERE stream_name = '%s'",
          OROCHI_STREAMS_TABLE, condition->stream_name);

      SPI_connect();
      ret = SPI_execute(query.data, true, 1);

      if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        Datum id_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                       SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
          stream_id = DatumGetInt64(id_datum);
      }

      SPI_finish();
      pfree(query.data);

      if (stream_id > 0)
        return orochi_stream_has_data(stream_id);
    }
    return false;

  case TASK_CONDITION_EXPRESSION:
    /* Execute SQL expression and check result */
    if (condition->expression != NULL) {
      StringInfoData query;
      bool result = false;
      int ret;

      initStringInfo(&query);
      /* Wrap the expression in a SELECT to evaluate it */
      appendStringInfo(&query, "SELECT (%s)::boolean", condition->expression);

      SPI_connect();

      PG_TRY();
      {
        ret = SPI_execute(query.data, true, 1);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
          bool isnull;
          Datum result_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                             SPI_tuptable->tupdesc, 1, &isnull);
          if (!isnull)
            result = DatumGetBool(result_datum);
        }
      }
      PG_CATCH();
      {
        /* On error, treat condition as false */
        FlushErrorState();
        result = false;
      }
      PG_END_TRY();

      SPI_finish();
      pfree(query.data);

      return result;
    }
    return true;

  default:
    return true;
  }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *task_state_name(TaskState state) {
  switch (state) {
  case TASK_STATE_CREATED:
    return "CREATED";
  case TASK_STATE_STARTED:
    return "STARTED";
  case TASK_STATE_SUSPENDED:
    return "SUSPENDED";
  case TASK_STATE_FAILED:
    return "FAILED";
  default:
    return "UNKNOWN";
  }
}

TaskState task_parse_state(const char *name) {
  if (strcasecmp(name, "CREATED") == 0)
    return TASK_STATE_CREATED;
  if (strcasecmp(name, "STARTED") == 0)
    return TASK_STATE_STARTED;
  if (strcasecmp(name, "SUSPENDED") == 0)
    return TASK_STATE_SUSPENDED;
  if (strcasecmp(name, "FAILED") == 0)
    return TASK_STATE_FAILED;
  return TASK_STATE_CREATED;
}

const char *task_run_state_name(TaskRunState state) {
  switch (state) {
  case TASK_RUN_SCHEDULED:
    return "SCHEDULED";
  case TASK_RUN_RUNNING:
    return "RUNNING";
  case TASK_RUN_SUCCEEDED:
    return "SUCCEEDED";
  case TASK_RUN_FAILED:
    return "FAILED";
  case TASK_RUN_SKIPPED:
    return "SKIPPED";
  case TASK_RUN_CANCELLED:
    return "CANCELLED";
  default:
    return "UNKNOWN";
  }
}

TaskRunState task_parse_run_state(const char *name) {
  if (strcasecmp(name, "SCHEDULED") == 0)
    return TASK_RUN_SCHEDULED;
  if (strcasecmp(name, "RUNNING") == 0)
    return TASK_RUN_RUNNING;
  if (strcasecmp(name, "SUCCEEDED") == 0)
    return TASK_RUN_SUCCEEDED;
  if (strcasecmp(name, "FAILED") == 0)
    return TASK_RUN_FAILED;
  if (strcasecmp(name, "SKIPPED") == 0)
    return TASK_RUN_SKIPPED;
  if (strcasecmp(name, "CANCELLED") == 0)
    return TASK_RUN_CANCELLED;
  return TASK_RUN_SCHEDULED;
}

const char *schedule_type_name(ScheduleType type) {
  switch (type) {
  case SCHEDULE_TYPE_CRON:
    return "CRON";
  case SCHEDULE_TYPE_INTERVAL:
    return "INTERVAL";
  case SCHEDULE_TYPE_TRIGGER:
    return "TRIGGER";
  default:
    return "UNKNOWN";
  }
}

void task_schedule_free(TaskSchedule *schedule) {
  if (schedule == NULL)
    return;

  if (schedule->cron_expression)
    pfree(schedule->cron_expression);
  if (schedule->timezone)
    pfree(schedule->timezone);
  if (schedule->interval)
    pfree(schedule->interval);

  pfree(schedule);
}

void task_condition_free(TaskCondition *condition) {
  if (condition == NULL)
    return;

  if (condition->expression)
    pfree(condition->expression);
  if (condition->stream_name)
    pfree(condition->stream_name);

  pfree(condition);
}

void task_run_stats_free(TaskRunStats *stats) {
  if (stats == NULL)
    return;

  if (stats->error_message)
    pfree(stats->error_message);
  if (stats->query_id)
    pfree(stats->query_id);

  pfree(stats);
}

void task_free(Task *task) {
  if (task == NULL)
    return;

  if (task->task_name)
    pfree(task->task_name);
  if (task->task_schema)
    pfree(task->task_schema);
  if (task->description)
    pfree(task->description);
  if (task->sql_text)
    pfree(task->sql_text);

  task_schedule_free(task->schedule);
  task_condition_free(task->condition);

  if (task->predecessor_ids)
    pfree(task->predecessor_ids);

  for (int i = 0; i < task->num_predecessors; i++) {
    if (task->predecessor_names && task->predecessor_names[i])
      pfree(task->predecessor_names[i]);
  }
  if (task->predecessor_names)
    pfree(task->predecessor_names);

  pfree(task);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_task_sql);
Datum orochi_create_task_sql(PG_FUNCTION_ARGS) {
  text *name_text = PG_GETARG_TEXT_PP(0);
  text *schedule_text = PG_GETARG_TEXT_PP(1);
  text *sql_text = PG_GETARG_TEXT_PP(2);
  char *name = text_to_cstring(name_text);
  char *schedule_str = text_to_cstring(schedule_text);
  char *sql = text_to_cstring(sql_text);
  int64 task_id;

  TaskSchedule *schedule = ddl_parse_schedule(schedule_str);

  task_id = orochi_create_task(name, sql, schedule, NULL);

  PG_RETURN_INT64(task_id);
}

PG_FUNCTION_INFO_V1(orochi_drop_task_sql);
Datum orochi_drop_task_sql(PG_FUNCTION_ARGS) {
  int64 task_id = PG_GETARG_INT64(0);
  bool if_exists = PG_GETARG_BOOL(1);

  orochi_drop_task(task_id, if_exists);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_execute_task_sql);
Datum orochi_execute_task_sql(PG_FUNCTION_ARGS) {
  int64 task_id = PG_GETARG_INT64(0);
  int64 run_id;

  run_id = orochi_execute_task(task_id);

  PG_RETURN_INT64(run_id);
}

PG_FUNCTION_INFO_V1(orochi_resume_task_sql);
Datum orochi_resume_task_sql(PG_FUNCTION_ARGS) {
  int64 task_id = PG_GETARG_INT64(0);

  orochi_resume_task(task_id);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_suspend_task_sql);
Datum orochi_suspend_task_sql(PG_FUNCTION_ARGS) {
  int64 task_id = PG_GETARG_INT64(0);

  orochi_suspend_task(task_id);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_task_status_sql);
Datum orochi_task_status_sql(PG_FUNCTION_ARGS) {
  int64 task_id = PG_GETARG_INT64(0);
  StringInfoData query;
  const char *state_name;
  int ret;
  TaskState state = TASK_STATE_CREATED;

  initStringInfo(&query);

  /* Query the task state from the catalog */
  appendStringInfo(&query, "SELECT state FROM orochi.%s WHERE task_id = %ld",
                   OROCHI_TASKS_TABLE, task_id);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    Datum state_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      state = (TaskState)DatumGetInt32(state_datum);
  } else {
    /* Task not found */
    SPI_finish();
    pfree(query.data);
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("task with ID %ld does not exist", task_id)));
  }

  SPI_finish();
  pfree(query.data);

  state_name = task_state_name(state);

  PG_RETURN_TEXT_P(cstring_to_text(state_name));
}
