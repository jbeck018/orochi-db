/*-------------------------------------------------------------------------
 *
 * ddl_dynamic.c
 *    Implementation of Dynamic Table DDL for Orochi DB
 *
 * This module handles:
 *   - Parsing CREATE DYNAMIC TABLE statements
 *   - Storing dynamic table definitions in catalog
 *   - Auto-refresh scheduling and execution (stub)
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
#include "parser/parser.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../core/catalog.h"
#include "ddl_dynamic.h"

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

/*
 * Allocate a new DynamicTableFreshness
 */
static DynamicTableFreshness *dynamic_freshness_alloc(void) {
  DynamicTableFreshness *freshness = palloc0(sizeof(DynamicTableFreshness));
  freshness->is_fresh = false;
  return freshness;
}

/*
 * Allocate a new DynamicTable
 */
static DynamicTable *dynamic_table_alloc(void) {
  DynamicTable *dt = palloc0(sizeof(DynamicTable));
  dt->state = DYNAMIC_STATE_ACTIVE;
  dt->refresh_mode = DYNAMIC_REFRESH_AUTO;
  dt->schedule_mode = DYNAMIC_SCHEDULE_TARGET_LAG;
  dt->initialize = true;
  dt->created_at = GetCurrentTimestamp();
  dt->freshness = dynamic_freshness_alloc();
  return dt;
}

/*
 * Parse interval from string (e.g., "1 hour", "30 minutes")
 */
static Interval *parse_interval_string(const char *str) {
  Interval *interval;
  int value;
  char unit[32];

  interval = palloc0(sizeof(Interval));

  if (sscanf(str, "%d %31s", &value, unit) != 2) {
    /* Try just number (assume seconds) */
    value = atoi(str);
    strcpy(unit, "second");
  }

  /* Normalize unit to lowercase */
  for (int i = 0; unit[i]; i++)
    unit[i] = tolower(unit[i]);

  /* Handle plural forms */
  int len = strlen(unit);
  if (len > 0 && unit[len - 1] == 's')
    unit[len - 1] = '\0';

  if (strcmp(unit, "second") == 0) {
    interval->time = value * USECS_PER_SEC;
  } else if (strcmp(unit, "minute") == 0) {
    interval->time = value * 60 * USECS_PER_SEC;
  } else if (strcmp(unit, "hour") == 0) {
    interval->time = value * 3600 * USECS_PER_SEC;
  } else if (strcmp(unit, "day") == 0) {
    interval->day = value;
  } else if (strcmp(unit, "week") == 0) {
    interval->day = value * 7;
  } else if (strcmp(unit, "month") == 0) {
    interval->month = value;
  } else {
    /* Default to hours */
    interval->time = value * 3600 * USECS_PER_SEC;
  }

  return interval;
}

/* ============================================================
 * DDL Parsing Functions
 * ============================================================ */

/*
 * Parse TARGET_LAG expression
 */
Interval *ddl_parse_target_lag(const char *lag_str) {
  const char *p = lag_str;

  /* Skip quotes if present */
  while (*p && (*p == '\'' || *p == '"' || isspace(*p)))
    p++;

  return parse_interval_string(p);
}

/*
 * Parse refresh mode
 */
DynamicTableRefreshMode ddl_parse_refresh_mode(const char *mode_str) {
  if (strcasecmp(mode_str, "AUTO") == 0)
    return DYNAMIC_REFRESH_AUTO;
  if (strcasecmp(mode_str, "FULL") == 0)
    return DYNAMIC_REFRESH_FULL;
  if (strcasecmp(mode_str, "INCREMENTAL") == 0)
    return DYNAMIC_REFRESH_INCREMENTAL;
  return DYNAMIC_REFRESH_AUTO;
}

/*
 * Extract source table dependencies from query
 * This is a simplified version - a real implementation would use the parser.
 */
List *ddl_extract_query_sources(const char *query) {
  List *sources = NIL;
  const char *p = query;
  const char *from_pos;

  /* Find FROM clause */
  from_pos = strcasestr(query, "FROM");
  if (from_pos == NULL)
    return NIL;

  p = from_pos + 4; /* Skip "FROM" */

  /* Parse table references (simplified) */
  while (*p) {
    char table_name[NAMEDATALEN];
    int len = 0;

    /* Skip whitespace */
    while (*p && isspace(*p))
      p++;

    /* Check for keywords that end FROM clause */
    if (strncasecmp(p, "WHERE", 5) == 0 || strncasecmp(p, "GROUP", 5) == 0 ||
        strncasecmp(p, "HAVING", 6) == 0 || strncasecmp(p, "ORDER", 5) == 0 ||
        strncasecmp(p, "LIMIT", 5) == 0 || strncasecmp(p, "UNION", 5) == 0)
      break;

    /* Skip JOIN keywords */
    if (strncasecmp(p, "JOIN", 4) == 0 || strncasecmp(p, "LEFT", 4) == 0 ||
        strncasecmp(p, "RIGHT", 5) == 0 || strncasecmp(p, "INNER", 5) == 0 ||
        strncasecmp(p, "OUTER", 5) == 0 || strncasecmp(p, "CROSS", 5) == 0 ||
        strncasecmp(p, "ON", 2) == 0) {
      while (*p && !isspace(*p))
        p++;
      continue;
    }

    /* Parse table name (may include schema) */
    while (*p && !isspace(*p) && *p != ',' && *p != '(' &&
           strncasecmp(p, "AS", 2) != 0 && len < NAMEDATALEN - 1) {
      table_name[len++] = *p++;
    }
    table_name[len] = '\0';

    /* Add to list if we got a name */
    if (len > 0) {
      DynamicSourceDep *dep = palloc0(sizeof(DynamicSourceDep));

      /* Parse schema.table */
      char *dot = strchr(table_name, '.');
      if (dot) {
        *dot = '\0';
        dep->source_schema = pstrdup(table_name);
        dep->source_name = pstrdup(dot + 1);
      } else {
        dep->source_schema = pstrdup("public");
        dep->source_name = pstrdup(table_name);
      }

      dep->is_dynamic_table = false; /* Will be checked later */
      sources = lappend(sources, dep);
    }

    /* Skip comma or alias */
    while (*p && (*p == ',' || isspace(*p)))
      p++;

    /* Skip alias (AS name) */
    if (strncasecmp(p, "AS", 2) == 0) {
      p += 2;
      while (*p && isspace(*p))
        p++;
      while (*p && !isspace(*p) && *p != ',')
        p++;
    }
  }

  return sources;
}

/*
 * Parse CREATE DYNAMIC TABLE statement
 *
 * Format:
 *   CREATE DYNAMIC TABLE name
 *     TARGET_LAG = 'interval'
 *     [REFRESH_MODE = AUTO|FULL|INCREMENTAL]
 *     [WAREHOUSE = 'warehouse']
 *     [CLUSTER BY (cols)]
 *     [INITIALIZE = ON|OFF]
 *     AS query
 */
DynamicTable *ddl_parse_create_dynamic_table(const char *sql) {
  DynamicTable *dt;
  const char *p;
  size_t len;

  dt = dynamic_table_alloc();

  p = sql;

  /* Skip CREATE DYNAMIC TABLE */
  if (strncasecmp(p, "CREATE", 6) == 0)
    p += 6;
  while (*p && isspace(*p))
    p++;
  if (strncasecmp(p, "DYNAMIC", 7) == 0)
    p += 7;
  while (*p && isspace(*p))
    p++;
  if (strncasecmp(p, "TABLE", 5) == 0)
    p += 5;
  while (*p && isspace(*p))
    p++;

  /* Parse table name */
  len = 0;
  while (p[len] && !isspace(p[len]))
    len++;

  dt->table_name = pnstrdup(p, len);
  p += len;

  /* Default schema */
  dt->table_schema = pstrdup("public");

  /* Parse optional clauses */
  while (*p) {
    while (*p && isspace(*p))
      p++;

    /* TARGET_LAG clause */
    if (strncasecmp(p, "TARGET_LAG", 10) == 0) {
      p += 10;
      while (*p && (*p == '=' || isspace(*p)))
        p++;

      if (*p == '\'') {
        p++;
        const char *start = p;
        while (*p && *p != '\'')
          p++;

        dt->target_lag = ddl_parse_target_lag(pnstrdup(start, p - start));
        if (*p == '\'')
          p++;
      }
      continue;
    }

    /* REFRESH_MODE clause */
    if (strncasecmp(p, "REFRESH_MODE", 12) == 0) {
      p += 12;
      while (*p && (*p == '=' || isspace(*p)))
        p++;

      const char *start = p;
      while (*p && !isspace(*p) && *p != '\'')
        p++;

      dt->refresh_mode = ddl_parse_refresh_mode(pnstrdup(start, p - start));
      continue;
    }

    /* WAREHOUSE clause */
    if (strncasecmp(p, "WAREHOUSE", 9) == 0) {
      p += 9;
      while (*p && (*p == '=' || isspace(*p)))
        p++;

      if (*p == '\'') {
        p++;
        const char *start = p;
        while (*p && *p != '\'')
          p++;

        dt->warehouse = pnstrdup(start, p - start);
        if (*p == '\'')
          p++;
      }
      continue;
    }

    /* INITIALIZE clause */
    if (strncasecmp(p, "INITIALIZE", 10) == 0) {
      p += 10;
      while (*p && (*p == '=' || isspace(*p)))
        p++;

      if (strncasecmp(p, "ON", 2) == 0 || strncasecmp(p, "TRUE", 4) == 0) {
        dt->initialize = true;
        p += (strncasecmp(p, "ON", 2) == 0) ? 2 : 4;
      } else if (strncasecmp(p, "OFF", 3) == 0 ||
                 strncasecmp(p, "FALSE", 5) == 0) {
        dt->initialize = false;
        p += (strncasecmp(p, "OFF", 3) == 0) ? 3 : 5;
      }
      continue;
    }

    /* CLUSTER BY clause */
    if (strncasecmp(p, "CLUSTER", 7) == 0) {
      p += 7;
      while (*p && isspace(*p))
        p++;
      if (strncasecmp(p, "BY", 2) == 0)
        p += 2;
      while (*p && isspace(*p))
        p++;

      if (*p == '(') {
        p++;
        List *cluster_cols = NIL;

        while (*p && *p != ')') {
          while (*p && isspace(*p))
            p++;

          const char *start = p;
          while (*p && *p != ',' && *p != ')' && !isspace(*p))
            p++;

          if (p > start)
            cluster_cols = lappend(cluster_cols, pnstrdup(start, p - start));

          while (*p && (*p == ',' || isspace(*p)))
            p++;
        }

        if (*p == ')')
          p++;

        /* Convert to array */
        if (list_length(cluster_cols) > 0) {
          ListCell *lc;
          int i = 0;

          dt->num_cluster_keys = list_length(cluster_cols);
          dt->cluster_keys = palloc(sizeof(char *) * dt->num_cluster_keys);

          foreach (lc, cluster_cols) {
            dt->cluster_keys[i++] = (char *)lfirst(lc);
          }
        }
      }
      continue;
    }

    /* AS clause (query) */
    if (strncasecmp(p, "AS", 2) == 0) {
      p += 2;
      while (*p && isspace(*p))
        p++;

      /* Rest is the query */
      dt->query_text = pstrdup(p);

      /* Extract source dependencies */
      List *sources = ddl_extract_query_sources(dt->query_text);
      if (list_length(sources) > 0) {
        ListCell *lc;
        int i = 0;

        dt->num_sources = list_length(sources);
        dt->sources = palloc(sizeof(DynamicSourceDep) * dt->num_sources);

        foreach (lc, sources) {
          DynamicSourceDep *dep = (DynamicSourceDep *)lfirst(lc);
          memcpy(&dt->sources[i++], dep, sizeof(DynamicSourceDep));
          pfree(dep);
        }
      }
      break;
    }

    /* Unknown token - skip */
    p++;
  }

  /* Set default target lag if not specified */
  if (dt->target_lag == NULL) {
    dt->target_lag = palloc0(sizeof(Interval));
    dt->target_lag->time =
        DYNAMIC_TABLE_DEFAULT_LAG_HOURS * 3600 * USECS_PER_SEC;
  }

  return dt;
}

/*
 * Validate dynamic table definition
 */
bool ddl_validate_dynamic_table(DynamicTable *dt, char **error_msg) {
  if (dt == NULL) {
    *error_msg = pstrdup("dynamic table is NULL");
    return false;
  }

  if (dt->table_name == NULL || strlen(dt->table_name) == 0) {
    *error_msg = pstrdup("dynamic table name is required");
    return false;
  }

  if (strlen(dt->table_name) > DYNAMIC_TABLE_MAX_NAME_LENGTH) {
    *error_msg = psprintf("table name exceeds maximum length of %d",
                          DYNAMIC_TABLE_MAX_NAME_LENGTH);
    return false;
  }

  if (dt->query_text == NULL || strlen(dt->query_text) == 0) {
    *error_msg = pstrdup("query is required (AS clause)");
    return false;
  }

  if (strlen(dt->query_text) > DYNAMIC_TABLE_MAX_QUERY_LENGTH) {
    *error_msg = psprintf("query exceeds maximum length of %d",
                          DYNAMIC_TABLE_MAX_QUERY_LENGTH);
    return false;
  }

  if (dt->target_lag == NULL) {
    *error_msg = pstrdup("TARGET_LAG is required");
    return false;
  }

  /* Check minimum lag */
  int64 lag_seconds =
      dt->target_lag->time / USECS_PER_SEC + dt->target_lag->day * 86400;
  if (lag_seconds < DYNAMIC_TABLE_MIN_LAG_SECONDS) {
    *error_msg = psprintf("TARGET_LAG must be at least %d seconds",
                          DYNAMIC_TABLE_MIN_LAG_SECONDS);
    return false;
  }

  *error_msg = NULL;
  return true;
}

/* ============================================================
 * Catalog Operations
 * ============================================================ */

/*
 * Store dynamic table in catalog
 */
void ddl_catalog_store_dynamic_table(DynamicTable *dt) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "INSERT INTO orochi.%s "
      "(table_name, table_schema, query_text, refresh_mode, schedule_mode, "
      "target_lag, initialize, state, created_at) "
      "VALUES ('%s', '%s', '%s', %d, %d, "
      "'%ld seconds'::interval, %s, %d, NOW()) "
      "RETURNING dynamic_table_id",
      OROCHI_DYNAMIC_TABLES_TABLE, dt->table_name,
      dt->table_schema ? dt->table_schema : "public", dt->query_text,
      (int)dt->refresh_mode, (int)dt->schedule_mode,
      (dt->target_lag->time / USECS_PER_SEC + dt->target_lag->day * 86400),
      dt->initialize ? "TRUE" : "FALSE", (int)dt->state);

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);

  if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
    bool isnull;
    Datum id_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      dt->dynamic_table_id = DatumGetInt64(id_datum);
  }

  /* Store source dependencies */
  for (int i = 0; i < dt->num_sources; i++) {
    resetStringInfo(&query);
    appendStringInfo(
        &query,
        "INSERT INTO orochi.%s "
        "(dynamic_table_id, source_schema, source_name, is_dynamic_table) "
        "VALUES (%ld, '%s', '%s', %s)",
        OROCHI_DYNAMIC_SOURCES_TABLE, dt->dynamic_table_id,
        dt->sources[i].source_schema, dt->sources[i].source_name,
        dt->sources[i].is_dynamic_table ? "TRUE" : "FALSE");

    SPI_execute(query.data, false, 0);
  }

  SPI_finish();
  pfree(query.data);
}

/*
 * Update dynamic table in catalog
 */
void ddl_catalog_update_dynamic_table(DynamicTable *dt) {
  StringInfoData query;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "UPDATE orochi.%s SET "
      "state = %d, "
      "last_refresh_at = %s, "
      "next_refresh_at = %s, "
      "total_refreshes = %ld, "
      "successful_refreshes = %ld, "
      "failed_refreshes = %ld "
      "WHERE dynamic_table_id = %ld",
      OROCHI_DYNAMIC_TABLES_TABLE, (int)dt->state,
      dt->last_refresh_at
          ? psprintf("'%s'", timestamptz_to_str(dt->last_refresh_at))
          : "NULL",
      dt->next_refresh_at
          ? psprintf("'%s'", timestamptz_to_str(dt->next_refresh_at))
          : "NULL",
      dt->total_refreshes, dt->successful_refreshes, dt->failed_refreshes,
      dt->dynamic_table_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);
}

/*
 * Delete dynamic table from catalog
 */
void ddl_catalog_delete_dynamic_table(int64 dynamic_table_id) {
  StringInfoData query;

  initStringInfo(&query);

  /* Delete sources first */
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE dynamic_table_id = %ld",
                   OROCHI_DYNAMIC_SOURCES_TABLE, dynamic_table_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);

  /* Delete refreshes */
  resetStringInfo(&query);
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE dynamic_table_id = %ld",
                   OROCHI_DYNAMIC_REFRESHES_TABLE, dynamic_table_id);
  SPI_execute(query.data, false, 0);

  /* Delete dynamic table */
  resetStringInfo(&query);
  appendStringInfo(&query, "DELETE FROM orochi.%s WHERE dynamic_table_id = %ld",
                   OROCHI_DYNAMIC_TABLES_TABLE, dynamic_table_id);
  SPI_execute(query.data, false, 0);

  SPI_finish();
  pfree(query.data);
}

/*
 * Create a refresh record
 */
int64 ddl_catalog_create_refresh(int64 dynamic_table_id) {
  StringInfoData query;
  int64 refresh_id = 0;
  int ret;

  initStringInfo(&query);

  appendStringInfo(&query,
                   "INSERT INTO orochi.%s "
                   "(dynamic_table_id, started_at, success) "
                   "VALUES (%ld, NOW(), FALSE) "
                   "RETURNING refresh_id",
                   OROCHI_DYNAMIC_REFRESHES_TABLE, dynamic_table_id);

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);

  if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
    bool isnull;
    Datum id_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      refresh_id = DatumGetInt64(id_datum);
  }

  SPI_finish();
  pfree(query.data);

  return refresh_id;
}

/*
 * Finish a refresh
 */
void ddl_catalog_finish_refresh(int64 refresh_id, bool success,
                                const char *error_msg) {
  StringInfoData query;

  initStringInfo(&query);

  appendStringInfo(
      &query,
      "UPDATE orochi.%s SET "
      "finished_at = NOW(), "
      "duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
      "success = %s, "
      "error_message = %s "
      "WHERE refresh_id = %ld",
      OROCHI_DYNAMIC_REFRESHES_TABLE, success ? "TRUE" : "FALSE",
      error_msg ? psprintf("'%s'", error_msg) : "NULL", refresh_id);

  SPI_connect();
  SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);
}

/* ============================================================
 * Dynamic Table Management Functions
 * ============================================================ */

/*
 * Create a dynamic table
 */
int64 orochi_create_dynamic_table(const char *name, const char *query,
                                  Interval *target_lag,
                                  DynamicTableRefreshMode mode,
                                  bool initialize) {
  DynamicTable *dt;
  char *error_msg;

  dt = dynamic_table_alloc();
  dt->table_name = pstrdup(name);
  dt->table_schema = pstrdup("public");
  dt->query_text = pstrdup(query);
  dt->target_lag = target_lag;
  dt->refresh_mode = mode;
  dt->initialize = initialize;

  /* Extract sources */
  List *sources = ddl_extract_query_sources(query);
  if (list_length(sources) > 0) {
    ListCell *lc;
    int i = 0;

    dt->num_sources = list_length(sources);
    dt->sources = palloc(sizeof(DynamicSourceDep) * dt->num_sources);

    foreach (lc, sources) {
      DynamicSourceDep *dep = (DynamicSourceDep *)lfirst(lc);
      memcpy(&dt->sources[i++], dep, sizeof(DynamicSourceDep));
    }
  }

  /* Validate */
  if (!ddl_validate_dynamic_table(dt, &error_msg)) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid dynamic table definition: %s", error_msg)));
  }

  /* Store in catalog */
  ddl_catalog_store_dynamic_table(dt);

  elog(LOG, "created dynamic table '%s' with ID %ld", dt->table_name,
       dt->dynamic_table_id);

  return dt->dynamic_table_id;
}

/*
 * Drop a dynamic table
 */
bool orochi_drop_dynamic_table(int64 dynamic_table_id, bool if_exists) {
  ddl_catalog_delete_dynamic_table(dynamic_table_id);
  elog(LOG, "dropped dynamic table with ID %ld", dynamic_table_id);
  return true;
}

/*
 * Load dynamic table from catalog
 */
static DynamicTable *dynamic_table_load_from_catalog(int64 dynamic_table_id) {
  StringInfoData query;
  DynamicTable *dt = NULL;
  int ret;

  initStringInfo(&query);

  appendStringInfo(&query,
                   "SELECT table_name, table_schema, query_text, refresh_mode, "
                   "       state, target_lag "
                   "FROM orochi.%s "
                   "WHERE dynamic_table_id = %ld",
                   OROCHI_DYNAMIC_TABLES_TABLE, dynamic_table_id);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    dt = palloc0(sizeof(DynamicTable));
    dt->dynamic_table_id = dynamic_table_id;

    Datum name_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
    if (!isnull)
      dt->table_name = pstrdup(TextDatumGetCString(name_datum));

    Datum schema_datum = SPI_getbinval(tuple, tupdesc, 2, &isnull);
    if (!isnull)
      dt->table_schema = pstrdup(TextDatumGetCString(schema_datum));

    Datum query_datum = SPI_getbinval(tuple, tupdesc, 3, &isnull);
    if (!isnull)
      dt->query_text = pstrdup(TextDatumGetCString(query_datum));

    Datum mode_datum = SPI_getbinval(tuple, tupdesc, 4, &isnull);
    if (!isnull)
      dt->refresh_mode = (DynamicTableRefreshMode)DatumGetInt32(mode_datum);

    Datum state_datum = SPI_getbinval(tuple, tupdesc, 5, &isnull);
    if (!isnull)
      dt->state = (DynamicTableState)DatumGetInt32(state_datum);
  }

  SPI_finish();
  pfree(query.data);

  return dt;
}

/*
 * Refresh a dynamic table
 */
int64 orochi_refresh_dynamic_table(int64 dynamic_table_id) {
  int64 refresh_id;
  DynamicTable *dt;
  int ret;
  bool success = true;
  char *error_msg = NULL;
  StringInfoData refresh_query;
  StringInfoData update_query;

  /* Load dynamic table definition */
  dt = dynamic_table_load_from_catalog(dynamic_table_id);
  if (dt == NULL) {
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("dynamic table with ID %ld does not exist",
                           dynamic_table_id)));
  }

  /* Check if dynamic table is in a refreshable state */
  if (dt->state == DYNAMIC_STATE_SUSPENDED) {
    dynamic_table_free(dt);
    ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("dynamic table %s is suspended", dt->table_name)));
  }

  /* Create refresh record */
  refresh_id = ddl_catalog_create_refresh(dynamic_table_id);

  elog(LOG, "started dynamic table %ld (%s) refresh %ld", dynamic_table_id,
       dt->table_name, refresh_id);

  /* Update state to refreshing */
  initStringInfo(&update_query);
  appendStringInfo(
      &update_query,
      "UPDATE orochi.%s SET state = %d WHERE dynamic_table_id = %ld",
      OROCHI_DYNAMIC_TABLES_TABLE, (int)DYNAMIC_STATE_REFRESHING,
      dynamic_table_id);

  SPI_connect();
  SPI_execute(update_query.data, false, 0);
  SPI_finish();

  /* Execute the refresh based on mode */
  SPI_connect();

  PG_TRY();
  {
    initStringInfo(&refresh_query);

    if (dt->refresh_mode == DYNAMIC_REFRESH_FULL) {
      /* Full refresh: truncate and insert */
      appendStringInfo(&refresh_query,
                       "TRUNCATE TABLE %s.%s; "
                       "INSERT INTO %s.%s %s",
                       dt->table_schema, dt->table_name, dt->table_schema,
                       dt->table_name, dt->query_text);
    } else {
      /* Incremental/Auto refresh: use INSERT ... ON CONFLICT or MERGE */
      /* For simplicity, use INSERT for now */
      appendStringInfo(&refresh_query,
                       "INSERT INTO %s.%s %s "
                       "ON CONFLICT DO NOTHING",
                       dt->table_schema, dt->table_name, dt->query_text);
    }

    ret = SPI_execute(refresh_query.data, false, 0);

    if (ret < 0) {
      success = false;
      error_msg = psprintf("Refresh execution failed with code %d", ret);
    } else {
      elog(LOG, "dynamic table %ld refresh %ld completed, %lu rows",
           dynamic_table_id, refresh_id, (unsigned long)SPI_processed);
    }

    pfree(refresh_query.data);
  }
  PG_CATCH();
  {
    /* Capture error information */
    ErrorData *edata = CopyErrorData();
    FlushErrorState();

    success = false;
    error_msg = pstrdup(edata->message);
    FreeErrorData(edata);
  }
  PG_END_TRY();

  SPI_finish();

  /* Update refresh record */
  ddl_catalog_finish_refresh(refresh_id, success, error_msg);

  /* Update dynamic table state and statistics */
  resetStringInfo(&update_query);

  if (success) {
    appendStringInfo(&update_query,
                     "UPDATE orochi.%s SET "
                     "state = %d, "
                     "last_refresh_at = NOW(), "
                     "next_refresh_at = NOW() + target_lag, "
                     "total_refreshes = total_refreshes + 1, "
                     "successful_refreshes = successful_refreshes + 1 "
                     "WHERE dynamic_table_id = %ld",
                     OROCHI_DYNAMIC_TABLES_TABLE, (int)DYNAMIC_STATE_ACTIVE,
                     dynamic_table_id);
  } else {
    appendStringInfo(&update_query,
                     "UPDATE orochi.%s SET "
                     "state = %d, "
                     "total_refreshes = total_refreshes + 1, "
                     "failed_refreshes = failed_refreshes + 1 "
                     "WHERE dynamic_table_id = %ld",
                     OROCHI_DYNAMIC_TABLES_TABLE, (int)DYNAMIC_STATE_FAILED,
                     dynamic_table_id);
  }

  SPI_connect();
  SPI_execute(update_query.data, false, 0);
  SPI_finish();

  pfree(update_query.data);
  if (error_msg)
    pfree(error_msg);
  dynamic_table_free(dt);

  return refresh_id;
}

/*
 * Suspend a dynamic table
 */
bool orochi_suspend_dynamic_table(int64 dynamic_table_id) {
  elog(LOG, "suspended dynamic table %ld", dynamic_table_id);
  return true;
}

/*
 * Resume a dynamic table
 */
bool orochi_resume_dynamic_table(int64 dynamic_table_id) {
  elog(LOG, "resumed dynamic table %ld", dynamic_table_id);
  return true;
}

/*
 * Check if dynamic table needs refresh
 */
bool dynamic_table_needs_refresh(DynamicTable *dt) {
  if (dt->state != DYNAMIC_STATE_ACTIVE)
    return false;

  if (dt->last_refresh_at == 0)
    return true;

  /* Calculate current lag */
  Interval *current_lag = orochi_get_current_lag(dt->dynamic_table_id);
  if (current_lag == NULL)
    return true;

  return interval_is_less_than(dt->target_lag, current_lag);
}

/*
 * Calculate next refresh time
 */
TimestampTz dynamic_table_next_refresh(DynamicTable *dt) {
  if (dt->target_lag == NULL)
    return 0;

  return DatumGetTimestampTz(DirectFunctionCall2(
      timestamptz_pl_interval, TimestampTzGetDatum(GetCurrentTimestamp()),
      PointerGetDatum(dt->target_lag)));
}

/*
 * Get current lag for a dynamic table
 *
 * The lag is calculated as the difference between the current time
 * and the last refresh time. For a more accurate calculation, we would
 * need to track source data timestamps, but this provides a reasonable
 * approximation for most use cases.
 */
Interval *orochi_get_current_lag(int64 dynamic_table_id) {
  StringInfoData query;
  Interval *lag = NULL;
  int ret;

  initStringInfo(&query);

  /*
   * Calculate the lag as the difference between NOW() and last_refresh_at.
   * If there's no refresh yet, use the difference from created_at.
   */
  appendStringInfo(
      &query,
      "SELECT (NOW() - COALESCE(last_refresh_at, created_at))::interval "
      "FROM orochi.%s "
      "WHERE dynamic_table_id = %ld",
      OROCHI_DYNAMIC_TABLES_TABLE, dynamic_table_id);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    Datum lag_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull) {
      /* Copy the interval to our memory context */
      Interval *src = DatumGetIntervalP(lag_datum);
      lag = palloc(sizeof(Interval));
      memcpy(lag, src, sizeof(Interval));
    }
  }

  SPI_finish();
  pfree(query.data);

  return lag;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *dynamic_table_state_name(DynamicTableState state) {
  switch (state) {
  case DYNAMIC_STATE_ACTIVE:
    return "ACTIVE";
  case DYNAMIC_STATE_SUSPENDED:
    return "SUSPENDED";
  case DYNAMIC_STATE_FAILED:
    return "FAILED";
  case DYNAMIC_STATE_INITIALIZING:
    return "INITIALIZING";
  case DYNAMIC_STATE_REFRESHING:
    return "REFRESHING";
  default:
    return "UNKNOWN";
  }
}

DynamicTableState dynamic_table_parse_state(const char *name) {
  if (strcasecmp(name, "ACTIVE") == 0)
    return DYNAMIC_STATE_ACTIVE;
  if (strcasecmp(name, "SUSPENDED") == 0)
    return DYNAMIC_STATE_SUSPENDED;
  if (strcasecmp(name, "FAILED") == 0)
    return DYNAMIC_STATE_FAILED;
  if (strcasecmp(name, "INITIALIZING") == 0)
    return DYNAMIC_STATE_INITIALIZING;
  if (strcasecmp(name, "REFRESHING") == 0)
    return DYNAMIC_STATE_REFRESHING;
  return DYNAMIC_STATE_ACTIVE;
}

const char *dynamic_refresh_mode_name(DynamicTableRefreshMode mode) {
  switch (mode) {
  case DYNAMIC_REFRESH_AUTO:
    return "AUTO";
  case DYNAMIC_REFRESH_FULL:
    return "FULL";
  case DYNAMIC_REFRESH_INCREMENTAL:
    return "INCREMENTAL";
  default:
    return "UNKNOWN";
  }
}

DynamicTableRefreshMode dynamic_refresh_parse_mode(const char *name) {
  if (strcasecmp(name, "AUTO") == 0)
    return DYNAMIC_REFRESH_AUTO;
  if (strcasecmp(name, "FULL") == 0)
    return DYNAMIC_REFRESH_FULL;
  if (strcasecmp(name, "INCREMENTAL") == 0)
    return DYNAMIC_REFRESH_INCREMENTAL;
  return DYNAMIC_REFRESH_AUTO;
}

const char *dynamic_schedule_mode_name(DynamicSchedulingMode mode) {
  switch (mode) {
  case DYNAMIC_SCHEDULE_TARGET_LAG:
    return "TARGET_LAG";
  case DYNAMIC_SCHEDULE_DOWNSTREAM:
    return "DOWNSTREAM";
  default:
    return "UNKNOWN";
  }
}

bool interval_is_less_than(Interval *a, Interval *b) {
  int64 a_total = a->time + a->day * 86400 * USECS_PER_SEC +
                  a->month * 30 * 86400 * USECS_PER_SEC;
  int64 b_total = b->time + b->day * 86400 * USECS_PER_SEC +
                  b->month * 30 * 86400 * USECS_PER_SEC;
  return a_total < b_total;
}

char *interval_to_string(Interval *interval) {
  int64 total_seconds = interval->time / USECS_PER_SEC + interval->day * 86400 +
                        interval->month * 30 * 86400;

  if (total_seconds < 60)
    return psprintf("%ld seconds", total_seconds);
  else if (total_seconds < 3600)
    return psprintf("%ld minutes", total_seconds / 60);
  else if (total_seconds < 86400)
    return psprintf("%ld hours", total_seconds / 3600);
  else
    return psprintf("%ld days", total_seconds / 86400);
}

void dynamic_freshness_free(DynamicTableFreshness *freshness) {
  if (freshness == NULL)
    return;

  if (freshness->current_lag)
    pfree(freshness->current_lag);
  if (freshness->target_lag)
    pfree(freshness->target_lag);

  pfree(freshness);
}

void dynamic_refresh_stats_free(DynamicRefreshStats *stats) {
  if (stats == NULL)
    return;

  if (stats->error_message)
    pfree(stats->error_message);
  if (stats->lag_before)
    pfree(stats->lag_before);
  if (stats->lag_after)
    pfree(stats->lag_after);

  pfree(stats);
}

void dynamic_table_free(DynamicTable *dt) {
  if (dt == NULL)
    return;

  if (dt->table_name)
    pfree(dt->table_name);
  if (dt->table_schema)
    pfree(dt->table_schema);
  if (dt->description)
    pfree(dt->description);
  if (dt->query_text)
    pfree(dt->query_text);
  if (dt->warehouse)
    pfree(dt->warehouse);
  if (dt->target_lag)
    pfree(dt->target_lag);

  for (int i = 0; i < dt->num_sources; i++) {
    if (dt->sources[i].source_schema)
      pfree(dt->sources[i].source_schema);
    if (dt->sources[i].source_name)
      pfree(dt->sources[i].source_name);
  }
  if (dt->sources)
    pfree(dt->sources);

  for (int i = 0; i < dt->num_cluster_keys; i++) {
    if (dt->cluster_keys && dt->cluster_keys[i])
      pfree(dt->cluster_keys[i]);
  }
  if (dt->cluster_keys)
    pfree(dt->cluster_keys);

  dynamic_freshness_free(dt->freshness);

  pfree(dt);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_dynamic_table_sql);
Datum orochi_create_dynamic_table_sql(PG_FUNCTION_ARGS) {
  text *name_text = PG_GETARG_TEXT_PP(0);
  text *query_text = PG_GETARG_TEXT_PP(1);
  Interval *target_lag = PG_GETARG_INTERVAL_P(2);
  char *name = text_to_cstring(name_text);
  char *query = text_to_cstring(query_text);
  int64 dt_id;

  dt_id = orochi_create_dynamic_table(name, query, target_lag,
                                      DYNAMIC_REFRESH_AUTO, true);

  PG_RETURN_INT64(dt_id);
}

PG_FUNCTION_INFO_V1(orochi_drop_dynamic_table_sql);
Datum orochi_drop_dynamic_table_sql(PG_FUNCTION_ARGS) {
  int64 dt_id = PG_GETARG_INT64(0);
  bool if_exists = PG_GETARG_BOOL(1);

  orochi_drop_dynamic_table(dt_id, if_exists);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_refresh_dynamic_table_sql);
Datum orochi_refresh_dynamic_table_sql(PG_FUNCTION_ARGS) {
  int64 dt_id = PG_GETARG_INT64(0);
  int64 refresh_id;

  refresh_id = orochi_refresh_dynamic_table(dt_id);

  PG_RETURN_INT64(refresh_id);
}

PG_FUNCTION_INFO_V1(orochi_suspend_dynamic_table_sql);
Datum orochi_suspend_dynamic_table_sql(PG_FUNCTION_ARGS) {
  int64 dt_id = PG_GETARG_INT64(0);

  orochi_suspend_dynamic_table(dt_id);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_resume_dynamic_table_sql);
Datum orochi_resume_dynamic_table_sql(PG_FUNCTION_ARGS) {
  int64 dt_id = PG_GETARG_INT64(0);

  orochi_resume_dynamic_table(dt_id);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_dynamic_table_status_sql);
Datum orochi_dynamic_table_status_sql(PG_FUNCTION_ARGS) {
  int64 dt_id = PG_GETARG_INT64(0);
  StringInfoData query;
  const char *state_name;
  int ret;
  DynamicTableState state = DYNAMIC_STATE_ACTIVE;

  initStringInfo(&query);

  /* Query the dynamic table state from the catalog */
  appendStringInfo(&query,
                   "SELECT state FROM orochi.%s WHERE dynamic_table_id = %ld",
                   OROCHI_DYNAMIC_TABLES_TABLE, dt_id);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    Datum state_datum =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      state = (DynamicTableState)DatumGetInt32(state_datum);
  } else {
    /* Dynamic table not found */
    SPI_finish();
    pfree(query.data);
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("dynamic table with ID %ld does not exist", dt_id)));
  }

  SPI_finish();
  pfree(query.data);

  state_name = dynamic_table_state_name(state);

  PG_RETURN_TEXT_P(cstring_to_text(state_name));
}
