/*-------------------------------------------------------------------------
 *
 * ddl_workflow.c
 *    Implementation of Workflow DDL for Orochi DB
 *
 * This module handles:
 *   - Parsing CREATE WORKFLOW statements
 *   - Storing workflow definitions in catalog
 *   - Workflow execution orchestration (stub)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "ddl_workflow.h"
#include "../core/catalog.h"

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

/*
 * Allocate a new WorkflowStep
 */
static WorkflowStep *
workflow_step_alloc(void)
{
    WorkflowStep *step = palloc0(sizeof(WorkflowStep));
    step->timeout_seconds = WORKFLOW_DEFAULT_TIMEOUT;
    step->retry_count = WORKFLOW_DEFAULT_RETRY_COUNT;
    step->retry_delay_ms = WORKFLOW_DEFAULT_RETRY_DELAY;
    step->continue_on_error = false;
    return step;
}

/*
 * Allocate a new Workflow
 */
static Workflow *
workflow_alloc(void)
{
    Workflow *wf = palloc0(sizeof(Workflow));
    wf->state = WORKFLOW_STATE_CREATED;
    wf->enabled = true;
    wf->max_concurrent = 1;
    wf->auto_resume = false;
    wf->created_at = GetCurrentTimestamp();
    return wf;
}

/*
 * Parse source type from string
 */
static StageSourceType
parse_source_type_from_url(const char *url)
{
    if (strncmp(url, "s3://", 5) == 0)
        return STAGE_SOURCE_S3;
    else if (strncmp(url, "gs://", 5) == 0)
        return STAGE_SOURCE_GCS;
    else if (strncmp(url, "azure://", 8) == 0 ||
             strncmp(url, "wasb://", 7) == 0)
        return STAGE_SOURCE_AZURE;
    else if (strncmp(url, "http://", 7) == 0 ||
             strncmp(url, "https://", 8) == 0)
        return STAGE_SOURCE_HTTP;
    else if (strncmp(url, "file://", 7) == 0 ||
             url[0] == '/')
        return STAGE_SOURCE_LOCAL;
    else
        return STAGE_SOURCE_TABLE;
}

/*
 * Parse file format from extension
 */
static StageFileFormat
parse_format_from_pattern(const char *pattern)
{
    const char *ext;

    if (pattern == NULL)
        return STAGE_FORMAT_PARQUET;

    ext = strrchr(pattern, '.');
    if (ext == NULL)
        return STAGE_FORMAT_PARQUET;

    if (strcasecmp(ext, ".parquet") == 0)
        return STAGE_FORMAT_PARQUET;
    else if (strcasecmp(ext, ".csv") == 0)
        return STAGE_FORMAT_CSV;
    else if (strcasecmp(ext, ".json") == 0 ||
             strcasecmp(ext, ".ndjson") == 0)
        return STAGE_FORMAT_JSON;
    else if (strcasecmp(ext, ".avro") == 0)
        return STAGE_FORMAT_AVRO;
    else if (strcasecmp(ext, ".orc") == 0)
        return STAGE_FORMAT_ORC;

    return STAGE_FORMAT_PARQUET;
}

/* ============================================================
 * DDL Parsing Functions
 * ============================================================ */

/*
 * Parse a STAGE step definition
 *
 * Format: STAGE stage_name FROM 'url/pattern' [WITH (options)]
 */
WorkflowStep *
ddl_parse_stage_step(const char *definition)
{
    WorkflowStep   *step;
    StageConfig    *config;
    const char     *p;
    char           *stage_name;
    char           *source_url;
    size_t          len;

    step = workflow_step_alloc();
    step->step_type = WORKFLOW_STEP_STAGE;

    config = palloc0(sizeof(StageConfig));
    step->config.stage = config;

    /* Skip "STAGE " prefix if present */
    p = definition;
    if (strncasecmp(p, "STAGE ", 6) == 0)
        p += 6;

    /* Parse stage name (until FROM or whitespace) */
    while (*p && isspace(*p)) p++;

    len = 0;
    while (p[len] && !isspace(p[len]) && strncasecmp(p + len, "FROM", 4) != 0)
        len++;

    stage_name = palloc(len + 1);
    memcpy(stage_name, p, len);
    stage_name[len] = '\0';

    config->stage_name = stage_name;
    step->step_name = pstrdup(stage_name);
    p += len;

    /* Skip to FROM */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "FROM", 4) == 0)
        p += 4;
    while (*p && isspace(*p)) p++;

    /* Parse source URL (quoted string) */
    if (*p == '\'')
    {
        p++;
        len = 0;
        while (p[len] && p[len] != '\'')
            len++;

        source_url = palloc(len + 1);
        memcpy(source_url, p, len);
        source_url[len] = '\0';

        config->source_url = source_url;
        config->source_type = parse_source_type_from_url(source_url);
        config->file_format = parse_format_from_pattern(source_url);

        p += len;
        if (*p == '\'')
            p++;
    }

    /* Set defaults */
    config->recursive = false;
    config->max_files = 0;  /* unlimited */
    config->max_file_size = 0;  /* unlimited */

    return step;
}

/*
 * Parse a TRANSFORM step definition
 *
 * Format: TRANSFORM WITH query [AS stage_name]
 */
WorkflowStep *
ddl_parse_transform_step(const char *definition)
{
    WorkflowStep    *step;
    TransformConfig *config;
    const char      *p;
    size_t           len;

    step = workflow_step_alloc();
    step->step_type = WORKFLOW_STEP_TRANSFORM;

    config = palloc0(sizeof(TransformConfig));
    step->config.transform = config;

    p = definition;
    if (strncasecmp(p, "TRANSFORM ", 10) == 0)
        p += 10;

    /* Skip "WITH " if present */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "WITH", 4) == 0)
        p += 4;
    while (*p && isspace(*p)) p++;

    /* Rest is the query (until end or AS clause) */
    len = strlen(p);

    /* Look for trailing AS clause */
    for (size_t i = len; i > 0; i--)
    {
        if (strncasecmp(p + i - 2, "AS", 2) == 0 &&
            (i < 3 || isspace(p[i - 3])))
        {
            len = i - 3;
            break;
        }
    }

    config->query_text = pnstrdup(p, len);
    config->materialize = true;
    config->parallelism = 4;  /* default parallelism */

    step->step_name = pstrdup("transform");

    return step;
}

/*
 * Parse a LOAD step definition
 *
 * Format: LOAD INTO target_table [FROM stage_name] [OPTIONS (...)]
 */
WorkflowStep *
ddl_parse_load_step(const char *definition)
{
    WorkflowStep *step;
    LoadConfig   *config;
    const char   *p;
    size_t        len;

    step = workflow_step_alloc();
    step->step_type = WORKFLOW_STEP_LOAD;

    config = palloc0(sizeof(LoadConfig));
    step->config.load = config;

    p = definition;
    if (strncasecmp(p, "LOAD ", 5) == 0)
        p += 5;

    /* Skip "INTO " */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "INTO", 4) == 0)
        p += 4;
    while (*p && isspace(*p)) p++;

    /* Parse target table name */
    len = 0;
    while (p[len] && !isspace(p[len]))
        len++;

    config->target_table = pnstrdup(p, len);
    step->step_name = psprintf("load_%s", config->target_table);
    p += len;

    /* Parse optional FROM clause */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "FROM", 4) == 0)
    {
        p += 4;
        while (*p && isspace(*p)) p++;

        len = 0;
        while (p[len] && !isspace(p[len]))
            len++;

        config->source_stage = pnstrdup(p, len);
    }

    /* Set defaults */
    config->truncate_target = false;
    config->use_copy = true;
    config->on_error = pstrdup("ABORT");
    config->error_limit = 0;

    return step;
}

/*
 * Parse a MERGE step definition
 *
 * Format: MERGE INTO target ON (keys) ...
 */
WorkflowStep *
ddl_parse_merge_step(const char *definition)
{
    WorkflowStep *step;
    MergeConfig  *config;
    const char   *p;
    size_t        len;

    step = workflow_step_alloc();
    step->step_type = WORKFLOW_STEP_MERGE;

    config = palloc0(sizeof(MergeConfig));
    step->config.merge = config;

    p = definition;
    if (strncasecmp(p, "MERGE ", 6) == 0)
        p += 6;

    /* Skip "INTO " */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "INTO", 4) == 0)
        p += 4;
    while (*p && isspace(*p)) p++;

    /* Parse target table */
    len = 0;
    while (p[len] && !isspace(p[len]))
        len++;

    config->target_table = pnstrdup(p, len);
    step->step_name = psprintf("merge_%s", config->target_table);

    config->delete_unmatched = false;

    return step;
}

/*
 * Parse a complete CREATE WORKFLOW statement
 */
Workflow *
ddl_parse_create_workflow(const char *sql)
{
    Workflow       *workflow;
    const char     *p;
    char           *name;
    size_t          len;
    List           *steps = NIL;
    int             step_count = 0;
    int             i;
    ListCell       *lc;

    workflow = workflow_alloc();

    p = sql;

    /* Skip CREATE WORKFLOW */
    if (strncasecmp(p, "CREATE", 6) == 0)
        p += 6;
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "WORKFLOW", 8) == 0)
        p += 8;
    while (*p && isspace(*p)) p++;

    /* Parse workflow name */
    len = 0;
    while (p[len] && !isspace(p[len]) && p[len] != '(')
        len++;

    name = pnstrdup(p, len);
    workflow->workflow_name = name;
    p += len;

    /* Skip to AS ( */
    while (*p && isspace(*p)) p++;
    if (strncasecmp(p, "AS", 2) == 0)
        p += 2;
    while (*p && isspace(*p)) p++;
    if (*p == '(')
        p++;

    /* Parse steps (comma-separated) */
    while (*p)
    {
        const char *step_start;
        const char *step_end;
        char       *step_text;
        WorkflowStep *step = NULL;

        while (*p && isspace(*p)) p++;
        if (*p == ')' || *p == '\0')
            break;

        step_start = p;

        /* Find end of step (comma or closing paren) */
        step_end = p;
        int paren_depth = 0;
        while (*step_end)
        {
            if (*step_end == '(')
                paren_depth++;
            else if (*step_end == ')')
            {
                if (paren_depth == 0)
                    break;
                paren_depth--;
            }
            else if (*step_end == ',' && paren_depth == 0)
                break;
            step_end++;
        }

        len = step_end - step_start;
        step_text = pnstrdup(step_start, len);

        /* Parse based on step type */
        if (strncasecmp(step_text, "STAGE", 5) == 0)
            step = ddl_parse_stage_step(step_text);
        else if (strncasecmp(step_text, "TRANSFORM", 9) == 0)
            step = ddl_parse_transform_step(step_text);
        else if (strncasecmp(step_text, "LOAD", 4) == 0)
            step = ddl_parse_load_step(step_text);
        else if (strncasecmp(step_text, "MERGE", 5) == 0)
            step = ddl_parse_merge_step(step_text);

        if (step != NULL)
        {
            step->step_id = step_count++;
            steps = lappend(steps, step);
        }

        pfree(step_text);
        p = step_end;
        if (*p == ',')
            p++;
    }

    /* Convert list to array */
    workflow->num_steps = step_count;
    workflow->steps = palloc(sizeof(WorkflowStep *) * step_count);
    i = 0;
    foreach(lc, steps)
    {
        workflow->steps[i++] = (WorkflowStep *) lfirst(lc);
    }
    list_free(steps);

    return workflow;
}

/*
 * Validate a workflow definition
 */
bool
ddl_validate_workflow(Workflow *workflow, char **error_msg)
{
    if (workflow == NULL)
    {
        *error_msg = pstrdup("workflow is NULL");
        return false;
    }

    if (workflow->workflow_name == NULL ||
        strlen(workflow->workflow_name) == 0)
    {
        *error_msg = pstrdup("workflow name is required");
        return false;
    }

    if (strlen(workflow->workflow_name) > WORKFLOW_MAX_NAME_LENGTH)
    {
        *error_msg = psprintf("workflow name exceeds maximum length of %d",
                              WORKFLOW_MAX_NAME_LENGTH);
        return false;
    }

    if (workflow->num_steps == 0)
    {
        *error_msg = pstrdup("workflow must have at least one step");
        return false;
    }

    if (workflow->num_steps > WORKFLOW_MAX_STEPS)
    {
        *error_msg = psprintf("workflow exceeds maximum of %d steps",
                              WORKFLOW_MAX_STEPS);
        return false;
    }

    *error_msg = NULL;
    return true;
}

/* ============================================================
 * Catalog Operations
 * ============================================================ */

/*
 * Store workflow in catalog
 */
void
ddl_catalog_store_workflow(Workflow *workflow)
{
    StringInfoData query;
    int            ret;
    int            i;

    initStringInfo(&query);

    /* Insert workflow metadata */
    appendStringInfo(&query,
        "INSERT INTO orochi.%s "
        "(workflow_name, description, schedule, enabled, auto_resume, "
        "max_concurrent, state, created_at) "
        "VALUES ('%s', %s, %s, %s, %s, %d, %d, NOW()) "
        "RETURNING workflow_id",
        OROCHI_WORKFLOWS_TABLE,
        workflow->workflow_name,
        workflow->description ? psprintf("'%s'", workflow->description) : "NULL",
        workflow->schedule ? psprintf("'%s'", workflow->schedule) : "NULL",
        workflow->enabled ? "TRUE" : "FALSE",
        workflow->auto_resume ? "TRUE" : "FALSE",
        workflow->max_concurrent,
        (int) workflow->state);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        Datum id_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                       SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            workflow->workflow_id = DatumGetInt64(id_datum);
    }

    /* Insert workflow steps */
    for (i = 0; i < workflow->num_steps; i++)
    {
        WorkflowStep *step = workflow->steps[i];

        resetStringInfo(&query);
        appendStringInfo(&query,
            "INSERT INTO orochi.%s "
            "(workflow_id, step_id, step_name, step_type, "
            "timeout_seconds, retry_count, continue_on_error) "
            "VALUES (%ld, %d, '%s', %d, %d, %d, %s)",
            OROCHI_WORKFLOW_STEPS_TABLE,
            workflow->workflow_id,
            step->step_id,
            step->step_name ? step->step_name : "unnamed",
            (int) step->step_type,
            step->timeout_seconds,
            step->retry_count,
            step->continue_on_error ? "TRUE" : "FALSE");

        SPI_execute(query.data, false, 0);
    }

    SPI_finish();
    pfree(query.data);
}

/*
 * Update workflow in catalog
 */
void
ddl_catalog_update_workflow(Workflow *workflow)
{
    StringInfoData query;

    initStringInfo(&query);

    appendStringInfo(&query,
        "UPDATE orochi.%s SET "
        "description = %s, "
        "schedule = %s, "
        "enabled = %s, "
        "auto_resume = %s, "
        "max_concurrent = %d, "
        "state = %d, "
        "last_run_at = %s "
        "WHERE workflow_id = %ld",
        OROCHI_WORKFLOWS_TABLE,
        workflow->description ? psprintf("'%s'", workflow->description) : "NULL",
        workflow->schedule ? psprintf("'%s'", workflow->schedule) : "NULL",
        workflow->enabled ? "TRUE" : "FALSE",
        workflow->auto_resume ? "TRUE" : "FALSE",
        workflow->max_concurrent,
        (int) workflow->state,
        workflow->last_run_at ?
            psprintf("'%s'", timestamptz_to_str(workflow->last_run_at)) : "NULL",
        workflow->workflow_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/*
 * Delete workflow from catalog
 */
void
ddl_catalog_delete_workflow(int64 workflow_id)
{
    StringInfoData query;

    initStringInfo(&query);

    /* Delete steps first (foreign key) */
    appendStringInfo(&query,
        "DELETE FROM orochi.%s WHERE workflow_id = %ld",
        OROCHI_WORKFLOW_STEPS_TABLE, workflow_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);

    /* Delete workflow */
    resetStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.%s WHERE workflow_id = %ld",
        OROCHI_WORKFLOWS_TABLE, workflow_id);

    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/*
 * Create a workflow run record
 */
int64
ddl_catalog_create_run(int64 workflow_id)
{
    StringInfoData query;
    int64          run_id = 0;
    int            ret;

    initStringInfo(&query);

    appendStringInfo(&query,
        "INSERT INTO orochi.%s "
        "(workflow_id, state, started_at) "
        "VALUES (%ld, %d, NOW()) "
        "RETURNING run_id",
        OROCHI_WORKFLOW_RUNS_TABLE,
        workflow_id,
        (int) WORKFLOW_STATE_RUNNING);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        Datum id_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                       SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            run_id = DatumGetInt64(id_datum);
    }

    SPI_finish();
    pfree(query.data);

    return run_id;
}

/*
 * Finish a workflow run
 */
void
ddl_catalog_finish_run(int64 run_id, WorkflowState state, const char *error_msg)
{
    StringInfoData query;

    initStringInfo(&query);

    appendStringInfo(&query,
        "UPDATE orochi.%s SET "
        "state = %d, "
        "finished_at = NOW(), "
        "duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
        "error_message = %s "
        "WHERE run_id = %ld",
        OROCHI_WORKFLOW_RUNS_TABLE,
        (int) state,
        error_msg ? psprintf("'%s'", error_msg) : "NULL",
        run_id);

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/* ============================================================
 * Workflow Management Functions
 * ============================================================ */

/*
 * Create a workflow from definition string
 */
int64
orochi_create_workflow(const char *name, const char *definition)
{
    Workflow   *workflow;
    char       *error_msg;

    /* Parse the definition */
    workflow = ddl_parse_create_workflow(definition);

    if (workflow->workflow_name == NULL)
        workflow->workflow_name = pstrdup(name);

    /* Validate */
    if (!ddl_validate_workflow(workflow, &error_msg))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid workflow definition: %s", error_msg)));
    }

    /* Store in catalog */
    ddl_catalog_store_workflow(workflow);

    elog(LOG, "created workflow '%s' with ID %ld",
         workflow->workflow_name, workflow->workflow_id);

    return workflow->workflow_id;
}

/*
 * Drop a workflow
 */
bool
orochi_drop_workflow(int64 workflow_id, bool if_exists)
{
    ddl_catalog_delete_workflow(workflow_id);
    elog(LOG, "dropped workflow with ID %ld", workflow_id);
    return true;
}

/*
 * Run a workflow (stub - actual execution would be more complex)
 */
int64
orochi_run_workflow(int64 workflow_id)
{
    int64 run_id;

    /* Create run record */
    run_id = ddl_catalog_create_run(workflow_id);

    elog(LOG, "started workflow %ld run %ld", workflow_id, run_id);

    /*
     * TODO: Actual execution would:
     * 1. Load workflow definition
     * 2. Execute each step in order
     * 3. Handle errors and retries
     * 4. Update run status
     */

    /* For now, mark as succeeded */
    ddl_catalog_finish_run(run_id, WORKFLOW_STATE_SUCCEEDED, NULL);

    return run_id;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *
workflow_state_name(WorkflowState state)
{
    switch (state)
    {
        case WORKFLOW_STATE_CREATED:    return "CREATED";
        case WORKFLOW_STATE_RUNNING:    return "RUNNING";
        case WORKFLOW_STATE_SUCCEEDED:  return "SUCCEEDED";
        case WORKFLOW_STATE_FAILED:     return "FAILED";
        case WORKFLOW_STATE_PAUSED:     return "PAUSED";
        case WORKFLOW_STATE_SUSPENDED:  return "SUSPENDED";
        default:                        return "UNKNOWN";
    }
}

WorkflowState
workflow_parse_state(const char *name)
{
    if (strcasecmp(name, "CREATED") == 0)   return WORKFLOW_STATE_CREATED;
    if (strcasecmp(name, "RUNNING") == 0)   return WORKFLOW_STATE_RUNNING;
    if (strcasecmp(name, "SUCCEEDED") == 0) return WORKFLOW_STATE_SUCCEEDED;
    if (strcasecmp(name, "FAILED") == 0)    return WORKFLOW_STATE_FAILED;
    if (strcasecmp(name, "PAUSED") == 0)    return WORKFLOW_STATE_PAUSED;
    if (strcasecmp(name, "SUSPENDED") == 0) return WORKFLOW_STATE_SUSPENDED;
    return WORKFLOW_STATE_CREATED;
}

const char *
workflow_step_type_name(WorkflowStepType type)
{
    switch (type)
    {
        case WORKFLOW_STEP_STAGE:       return "STAGE";
        case WORKFLOW_STEP_TRANSFORM:   return "TRANSFORM";
        case WORKFLOW_STEP_LOAD:        return "LOAD";
        case WORKFLOW_STEP_MERGE:       return "MERGE";
        case WORKFLOW_STEP_VALIDATE:    return "VALIDATE";
        case WORKFLOW_STEP_NOTIFY:      return "NOTIFY";
        case WORKFLOW_STEP_CUSTOM:      return "CUSTOM";
        default:                        return "UNKNOWN";
    }
}

const char *
stage_source_type_name(StageSourceType type)
{
    switch (type)
    {
        case STAGE_SOURCE_S3:       return "S3";
        case STAGE_SOURCE_GCS:      return "GCS";
        case STAGE_SOURCE_AZURE:    return "AZURE";
        case STAGE_SOURCE_LOCAL:    return "LOCAL";
        case STAGE_SOURCE_HTTP:     return "HTTP";
        case STAGE_SOURCE_TABLE:    return "TABLE";
        default:                    return "UNKNOWN";
    }
}

const char *
stage_format_name(StageFileFormat format)
{
    switch (format)
    {
        case STAGE_FORMAT_PARQUET:  return "PARQUET";
        case STAGE_FORMAT_CSV:      return "CSV";
        case STAGE_FORMAT_JSON:     return "JSON";
        case STAGE_FORMAT_AVRO:     return "AVRO";
        case STAGE_FORMAT_ORC:      return "ORC";
        default:                    return "UNKNOWN";
    }
}

void
workflow_step_free(WorkflowStep *step)
{
    if (step == NULL)
        return;

    if (step->step_name)
        pfree(step->step_name);

    switch (step->step_type)
    {
        case WORKFLOW_STEP_STAGE:
            if (step->config.stage)
            {
                if (step->config.stage->stage_name)
                    pfree(step->config.stage->stage_name);
                if (step->config.stage->source_url)
                    pfree(step->config.stage->source_url);
                pfree(step->config.stage);
            }
            break;
        case WORKFLOW_STEP_TRANSFORM:
            if (step->config.transform)
            {
                if (step->config.transform->query_text)
                    pfree(step->config.transform->query_text);
                pfree(step->config.transform);
            }
            break;
        case WORKFLOW_STEP_LOAD:
            if (step->config.load)
            {
                if (step->config.load->target_table)
                    pfree(step->config.load->target_table);
                pfree(step->config.load);
            }
            break;
        case WORKFLOW_STEP_MERGE:
            if (step->config.merge)
            {
                if (step->config.merge->target_table)
                    pfree(step->config.merge->target_table);
                pfree(step->config.merge);
            }
            break;
        default:
            break;
    }

    if (step->depends_on)
        pfree(step->depends_on);
    if (step->condition)
        pfree(step->condition);

    pfree(step);
}

void
workflow_free(Workflow *workflow)
{
    int i;

    if (workflow == NULL)
        return;

    if (workflow->workflow_name)
        pfree(workflow->workflow_name);
    if (workflow->description)
        pfree(workflow->description);
    if (workflow->schedule)
        pfree(workflow->schedule);

    for (i = 0; i < workflow->num_steps; i++)
        workflow_step_free(workflow->steps[i]);

    if (workflow->steps)
        pfree(workflow->steps);

    pfree(workflow);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_workflow_sql);
Datum
orochi_create_workflow_sql(PG_FUNCTION_ARGS)
{
    text   *name_text = PG_GETARG_TEXT_PP(0);
    text   *def_text = PG_GETARG_TEXT_PP(1);
    char   *name = text_to_cstring(name_text);
    char   *definition = text_to_cstring(def_text);
    int64   workflow_id;

    workflow_id = orochi_create_workflow(name, definition);

    PG_RETURN_INT64(workflow_id);
}

PG_FUNCTION_INFO_V1(orochi_drop_workflow_sql);
Datum
orochi_drop_workflow_sql(PG_FUNCTION_ARGS)
{
    int64 workflow_id = PG_GETARG_INT64(0);
    bool  if_exists = PG_GETARG_BOOL(1);

    orochi_drop_workflow(workflow_id, if_exists);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_run_workflow_sql);
Datum
orochi_run_workflow_sql(PG_FUNCTION_ARGS)
{
    int64 workflow_id = PG_GETARG_INT64(0);
    int64 run_id;

    run_id = orochi_run_workflow(workflow_id);

    PG_RETURN_INT64(run_id);
}

PG_FUNCTION_INFO_V1(orochi_workflow_status_sql);
Datum
orochi_workflow_status_sql(PG_FUNCTION_ARGS)
{
    int64       workflow_id = PG_GETARG_INT64(0);
    const char *state_name;

    /* TODO: Actually query the workflow state from catalog */
    state_name = workflow_state_name(WORKFLOW_STATE_CREATED);

    PG_RETURN_TEXT_P(cstring_to_text(state_name));
}
