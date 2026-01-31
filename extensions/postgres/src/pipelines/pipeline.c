/*-------------------------------------------------------------------------
 *
 * pipeline.c
 *    Orochi DB real-time data pipeline core implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/snapmgr.h"
#include "commands/copy.h"
#include "catalog/pg_type.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "pipeline.h"

/* Background worker signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* Shared memory state for pipeline registry */
typedef struct PipelineSharedState
{
    LWLock         *lock;
    int             num_active_pipelines;
    PipelineRegistryEntry entries[64];  /* Max 64 concurrent pipelines */
} PipelineSharedState;

static PipelineSharedState *pipeline_shared_state = NULL;

/* GUC variables */
int orochi_pipeline_max_workers = 8;
int orochi_pipeline_batch_size = PIPELINE_DEFAULT_BATCH_SIZE;
int orochi_pipeline_batch_timeout_ms = PIPELINE_DEFAULT_BATCH_TIMEOUT_MS;

/* Forward declarations */
static void pipeline_sighup_handler(SIGNAL_ARGS);
static void pipeline_sigterm_handler(SIGNAL_ARGS);
static Pipeline *load_pipeline_from_catalog(int64 pipeline_id);
static bool save_pipeline_to_catalog(Pipeline *pipeline);
static void process_pipeline_batch(Pipeline *pipeline);
static void update_pipeline_stats(Pipeline *pipeline, int64 processed, int64 failed);
static void create_pipeline_checkpoint_internal(Pipeline *pipeline);
static bool register_pipeline_worker(int64 pipeline_id, int pid);
static void unregister_pipeline_worker(int64 pipeline_id);
static KafkaSourceConfig *parse_kafka_config(const char *json);
static S3SourceConfig *parse_s3_config(const char *json);

/* ============================================================
 * Shared Memory Initialization
 * ============================================================ */

Size
orochi_pipeline_shmem_size(void)
{
    return sizeof(PipelineSharedState);
}

void
orochi_pipeline_shmem_init(void)
{
    bool found;

    pipeline_shared_state = (PipelineSharedState *)
        ShmemInitStruct("Orochi Pipeline State",
                        sizeof(PipelineSharedState),
                        &found);

    if (!found)
    {
        memset(pipeline_shared_state, 0, sizeof(PipelineSharedState));
        pipeline_shared_state->lock = &(GetNamedLWLockTranche("orochi_pipeline"))->lock;
        pipeline_shared_state->num_active_pipelines = 0;
    }
}

/* ============================================================
 * Pipeline Lifecycle Functions
 * ============================================================ */

/*
 * orochi_create_pipeline
 *    Create a new data pipeline
 */
int64
orochi_create_pipeline(const char *name,
                       PipelineSource source_type,
                       Oid target_table_oid,
                       PipelineFormat format,
                       const char *options_json)
{
    Pipeline *pipeline;
    StringInfoData query;
    int64 pipeline_id = 0;
    int ret;

    /* Validate inputs */
    if (name == NULL || strlen(name) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pipeline name cannot be empty")));

    if (strlen(name) > PIPELINE_MAX_NAME_LENGTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pipeline name too long (max %d characters)",
                        PIPELINE_MAX_NAME_LENGTH)));

    if (!OidIsValid(target_table_oid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid target table")));

    /* Create pipeline record in catalog */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));

    initStringInfo(&query);
    /* Use quote_literal_cstr to prevent SQL injection */
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_pipelines "
        "(name, source_type, target_table_oid, format, options, state, created_at) "
        "VALUES (%s, %d, %u, %d, %s, %d, NOW()) "
        "RETURNING pipeline_id",
        quote_literal_cstr(name), (int)source_type, target_table_oid, (int)format,
        quote_literal_cstr(options_json ? options_json : "{}"),
        (int)PIPELINE_STATE_CREATED);

    ret = SPI_execute(query.data, false, 0);
    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        pipeline_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                   SPI_tuptable->tupdesc, 1, &isnull));
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to create pipeline")));
    }

    pfree(query.data);
    SPI_finish();

    elog(NOTICE, "Created pipeline '%s' with ID %ld", name, pipeline_id);

    return pipeline_id;
}

/*
 * orochi_start_pipeline
 *    Start a pipeline by launching its background worker
 */
bool
orochi_start_pipeline(int64 pipeline_id)
{
    Pipeline *pipeline;
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    pid_t pid;
    StringInfoData query;
    int ret;

    /* Load pipeline configuration */
    pipeline = load_pipeline_from_catalog(pipeline_id);
    if (pipeline == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pipeline %ld does not exist", pipeline_id)));

    if (pipeline->state == PIPELINE_STATE_RUNNING)
    {
        elog(NOTICE, "pipeline %ld is already running", pipeline_id);
        return true;
    }

    if (pipeline->state == PIPELINE_STATE_ERROR)
    {
        elog(WARNING, "pipeline %ld is in error state, resetting...", pipeline_id);
    }

    /* Configure background worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi_pipeline_%ld", pipeline_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi pipeline worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;  /* We handle restarts manually */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pipeline_worker_main");
    worker.bgw_main_arg = Int64GetDatum(pipeline_id);
    worker.bgw_notify_pid = MyProcPid;

    /* Register and start worker */
    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("could not register pipeline worker")));
    }

    /* Wait for worker to start */
    if (WaitForBackgroundWorkerStartup(handle, &pid) != BGWH_STARTED)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pipeline worker failed to start")));
    }

    /* Update pipeline state in catalog */
    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_pipelines "
        "SET state = %d, started_at = NOW(), worker_pid = %d "
        "WHERE pipeline_id = %ld",
        (int)PIPELINE_STATE_RUNNING, pid, pipeline_id);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    /* Register in shared memory */
    register_pipeline_worker(pipeline_id, pid);

    elog(NOTICE, "Started pipeline %ld with worker PID %d", pipeline_id, pid);

    return true;
}

/*
 * orochi_pause_pipeline
 *    Pause a running pipeline
 */
bool
orochi_pause_pipeline(int64 pipeline_id)
{
    StringInfoData query;
    int ret;
    int worker_pid;

    /* Get worker PID */
    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT worker_pid FROM orochi.orochi_pipelines "
        "WHERE pipeline_id = %ld AND state = %d",
        pipeline_id, (int)PIPELINE_STATE_RUNNING);

    ret = SPI_execute(query.data, true, 1);
    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        pfree(query.data);
        SPI_finish();
        elog(NOTICE, "pipeline %ld is not running", pipeline_id);
        return false;
    }

    worker_pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                              SPI_tuptable->tupdesc, 1, NULL));
    pfree(query.data);

    /* Update state to paused */
    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_pipelines "
        "SET state = %d "
        "WHERE pipeline_id = %ld",
        (int)PIPELINE_STATE_PAUSED, pipeline_id);

    SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    elog(NOTICE, "Paused pipeline %ld", pipeline_id);
    return true;
}

/*
 * orochi_resume_pipeline
 *    Resume a paused pipeline
 */
bool
orochi_resume_pipeline(int64 pipeline_id)
{
    PipelineState current_state;

    current_state = orochi_get_pipeline_state(pipeline_id);
    if (current_state != PIPELINE_STATE_PAUSED)
    {
        elog(NOTICE, "pipeline %ld is not paused (state: %s)",
             pipeline_id, pipeline_state_name(current_state));
        return false;
    }

    /* Simply restart the pipeline */
    return orochi_start_pipeline(pipeline_id);
}

/*
 * orochi_stop_pipeline
 *    Stop a running pipeline
 */
bool
orochi_stop_pipeline(int64 pipeline_id)
{
    StringInfoData query;
    int ret;
    int worker_pid = 0;

    SPI_connect();

    /* Get worker PID */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT worker_pid FROM orochi.orochi_pipelines WHERE pipeline_id = %ld",
        pipeline_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        worker_pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                  SPI_tuptable->tupdesc, 1, &isnull));
    }
    pfree(query.data);

    /* Send SIGTERM to worker if running */
    if (worker_pid > 0)
    {
        if (kill(worker_pid, SIGTERM) != 0 && errno != ESRCH)
        {
            elog(WARNING, "failed to send SIGTERM to pipeline worker %d", worker_pid);
        }
    }

    /* Update state to stopped */
    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_pipelines "
        "SET state = %d, stopped_at = NOW(), worker_pid = NULL "
        "WHERE pipeline_id = %ld",
        (int)PIPELINE_STATE_STOPPED, pipeline_id);

    SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    /* Unregister from shared memory */
    unregister_pipeline_worker(pipeline_id);

    elog(NOTICE, "Stopped pipeline %ld", pipeline_id);
    return true;
}

/*
 * orochi_drop_pipeline
 *    Drop a pipeline and all its data
 */
bool
orochi_drop_pipeline(int64 pipeline_id, bool if_exists)
{
    StringInfoData query;
    int ret;
    PipelineState state;

    /* Check if pipeline exists */
    state = orochi_get_pipeline_state(pipeline_id);
    if (state == PIPELINE_STATE_CREATED && !if_exists)
    {
        /* Pipeline doesn't exist - check more thoroughly */
        SPI_connect();
        initStringInfo(&query);
        appendStringInfo(&query,
            "SELECT 1 FROM orochi.orochi_pipelines WHERE pipeline_id = %ld",
            pipeline_id);
        ret = SPI_execute(query.data, true, 1);
        pfree(query.data);
        SPI_finish();

        if (SPI_processed == 0)
        {
            if (if_exists)
                return true;
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("pipeline %ld does not exist", pipeline_id)));
        }
    }

    /* Stop pipeline if running */
    if (state == PIPELINE_STATE_RUNNING || state == PIPELINE_STATE_PAUSED)
    {
        orochi_stop_pipeline(pipeline_id);
    }

    SPI_connect();

    /* Delete checkpoints */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_pipeline_checkpoints WHERE pipeline_id = %ld",
        pipeline_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Delete stats */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_pipeline_stats WHERE pipeline_id = %ld",
        pipeline_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Delete pipeline record */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_pipelines WHERE pipeline_id = %ld",
        pipeline_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(NOTICE, "Dropped pipeline %ld", pipeline_id);
    return true;
}

/* ============================================================
 * Pipeline Information Functions
 * ============================================================ */

/*
 * orochi_get_pipeline
 *    Get pipeline configuration
 */
Pipeline *
orochi_get_pipeline(int64 pipeline_id)
{
    return load_pipeline_from_catalog(pipeline_id);
}

/*
 * orochi_get_pipeline_state
 *    Get current pipeline state
 */
PipelineState
orochi_get_pipeline_state(int64 pipeline_id)
{
    StringInfoData query;
    PipelineState state = PIPELINE_STATE_CREATED;
    int ret;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT state FROM orochi.orochi_pipelines WHERE pipeline_id = %ld",
        pipeline_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        state = (PipelineState)DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                            SPI_tuptable->tupdesc, 1, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return state;
}

/*
 * orochi_get_pipeline_stats
 *    Get pipeline statistics
 */
PipelineStats *
orochi_get_pipeline_stats(int64 pipeline_id)
{
    PipelineStats *stats;
    StringInfoData query;
    int ret;

    stats = palloc0(sizeof(PipelineStats));

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT messages_received, messages_processed, messages_failed, "
        "bytes_received, rows_inserted, last_message_time, last_error_time, "
        "last_error_message, avg_latency_ms, throughput_per_sec "
        "FROM orochi.orochi_pipeline_stats WHERE pipeline_id = %ld",
        pipeline_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        stats->messages_received = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        stats->messages_processed = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        stats->messages_failed = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        stats->bytes_received = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        stats->rows_inserted = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        stats->last_message_time = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        if (!isnull)
            stats->last_error_time = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        stats->last_error_message = SPI_getvalue(tuple, tupdesc, 8);
        stats->avg_latency_ms = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        stats->throughput_per_sec = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 10, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return stats;
}

/*
 * orochi_list_pipelines
 *    List all pipelines
 */
List *
orochi_list_pipelines(void)
{
    List *result = NIL;
    StringInfoData query;
    int ret;
    int i;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfoString(&query,
        "SELECT pipeline_id FROM orochi.orochi_pipelines ORDER BY pipeline_id");

    ret = SPI_execute(query.data, true, 0);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            bool isnull;
            int64 *pipeline_id = palloc(sizeof(int64));
            *pipeline_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[i],
                                                        SPI_tuptable->tupdesc, 1, &isnull));
            result = lappend(result, pipeline_id);
        }
    }

    pfree(query.data);
    SPI_finish();

    return result;
}

/* ============================================================
 * Background Worker Implementation
 * ============================================================ */

static void
pipeline_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
pipeline_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/*
 * pipeline_worker_main
 *    Main entry point for pipeline background worker
 */
void
pipeline_worker_main(Datum main_arg)
{
    int64 pipeline_id = DatumGetInt64(main_arg);
    Pipeline *pipeline;
    int64 messages_since_checkpoint = 0;

    /* Set up signal handlers */
    pqsignal(SIGHUP, pipeline_sighup_handler);
    pqsignal(SIGTERM, pipeline_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Pipeline worker started for pipeline %ld", pipeline_id);

    /* Load pipeline configuration */
    StartTransactionCommand();
    pipeline = load_pipeline_from_catalog(pipeline_id);
    CommitTransactionCommand();

    if (pipeline == NULL)
    {
        elog(ERROR, "Failed to load pipeline %ld configuration", pipeline_id);
        proc_exit(1);
    }

    /* Initialize source based on type */
    switch (pipeline->source_type)
    {
        case PIPELINE_SOURCE_KAFKA:
            /* Initialize Kafka consumer - implementation in kafka_source.c */
            elog(LOG, "Initializing Kafka consumer for pipeline %ld", pipeline_id);
            break;

        case PIPELINE_SOURCE_S3:
            /* Initialize S3 watcher - implementation in s3_source.c */
            elog(LOG, "Initializing S3 source for pipeline %ld", pipeline_id);
            break;

        case PIPELINE_SOURCE_FILESYSTEM:
            elog(LOG, "Initializing filesystem source for pipeline %ld", pipeline_id);
            break;

        default:
            elog(ERROR, "Unsupported pipeline source type: %d", pipeline->source_type);
            proc_exit(1);
    }

    /* Main processing loop */
    while (!got_sigterm)
    {
        int rc;
        PipelineState current_state;

        /* Wait for work or timeout */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       pipeline->batch_timeout_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Handle SIGHUP - reload config */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);

            /* Reload pipeline config */
            StartTransactionCommand();
            Pipeline *new_config = load_pipeline_from_catalog(pipeline_id);
            if (new_config != NULL)
            {
                /* Update relevant configuration */
                pipeline->batch_size = new_config->batch_size;
                pipeline->batch_timeout_ms = new_config->batch_timeout_ms;
            }
            CommitTransactionCommand();
        }

        if (got_sigterm)
            break;

        /* Check if pipeline is paused */
        StartTransactionCommand();
        current_state = orochi_get_pipeline_state(pipeline_id);
        CommitTransactionCommand();

        if (current_state == PIPELINE_STATE_PAUSED)
        {
            /* Sleep longer when paused */
            pg_usleep(1000000);  /* 1 second */
            continue;
        }

        if (current_state == PIPELINE_STATE_STOPPED)
        {
            elog(LOG, "Pipeline %ld stopped", pipeline_id);
            break;
        }

        /* Process a batch of messages */
        StartTransactionCommand();
        PG_TRY();
        {
            process_pipeline_batch(pipeline);

            messages_since_checkpoint += pipeline->batch_size;

            /* Create checkpoint periodically */
            if (messages_since_checkpoint >= PIPELINE_CHECKPOINT_INTERVAL)
            {
                create_pipeline_checkpoint_internal(pipeline);
                messages_since_checkpoint = 0;
            }

            CommitTransactionCommand();
        }
        PG_CATCH();
        {
            /* Handle error */
            ErrorData *edata;
            MemoryContext ecxt;

            /* Safely get error data - use TopMemoryContext if pipeline_context is NULL */
            ecxt = MemoryContextSwitchTo(pipeline->pipeline_context ?
                                         pipeline->pipeline_context : TopMemoryContext);
            edata = CopyErrorData();
            FlushErrorState();
            MemoryContextSwitchTo(ecxt);

            elog(WARNING, "Pipeline %ld error: %s", pipeline_id, edata->message);

            /* Update error stats - check stats is not NULL */
            if (pipeline->stats != NULL)
            {
                pipeline->stats->messages_failed++;
                pipeline->stats->last_error_time = GetCurrentTimestamp();
                if (edata->message)
                    pipeline->stats->last_error_message = pstrdup(edata->message);
            }

            AbortCurrentTransaction();
            StartTransactionCommand();

            /* Check error threshold */
            if (pipeline->stats &&
                pipeline->stats->messages_failed > pipeline->error_threshold)
            {
                StringInfoData query;
                initStringInfo(&query);
                appendStringInfo(&query,
                    "UPDATE orochi.orochi_pipelines SET state = %d "
                    "WHERE pipeline_id = %ld",
                    (int)PIPELINE_STATE_ERROR, pipeline_id);
                SPI_connect();
                SPI_execute(query.data, false, 0);
                SPI_finish();
                pfree(query.data);

                elog(LOG, "Pipeline %ld exceeded error threshold, stopping", pipeline_id);
                got_sigterm = true;
            }

            CommitTransactionCommand();

            FreeErrorData(edata);
        }
        PG_END_TRY();
    }

    /* Cleanup */
    elog(LOG, "Pipeline worker %ld shutting down", pipeline_id);

    /* Final checkpoint */
    StartTransactionCommand();
    create_pipeline_checkpoint_internal(pipeline);
    CommitTransactionCommand();

    /* Update pipeline state */
    StartTransactionCommand();
    SPI_connect();
    {
        StringInfoData query;
        initStringInfo(&query);
        appendStringInfo(&query,
            "UPDATE orochi.orochi_pipelines "
            "SET state = %d, stopped_at = NOW(), worker_pid = NULL "
            "WHERE pipeline_id = %ld AND state = %d",
            (int)PIPELINE_STATE_STOPPED, pipeline_id, (int)PIPELINE_STATE_RUNNING);
        SPI_execute(query.data, false, 0);
        pfree(query.data);
    }
    SPI_finish();
    CommitTransactionCommand();

    unregister_pipeline_worker(pipeline_id);

    proc_exit(0);
}

/* ============================================================
 * Internal Helper Functions
 * ============================================================ */

static Pipeline *
load_pipeline_from_catalog(int64 pipeline_id)
{
    Pipeline *pipeline;
    StringInfoData query;
    int ret;

    pipeline = palloc0(sizeof(Pipeline));
    pipeline->pipeline_context = AllocSetContextCreate(TopMemoryContext,
                                                       "Pipeline Context",
                                                       ALLOCSET_DEFAULT_SIZES);

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT pipeline_id, name, source_type, target_table_oid, format, "
        "options, state, created_at, started_at, stopped_at, worker_pid, "
        "batch_size, batch_timeout_ms, error_threshold "
        "FROM orochi.orochi_pipelines WHERE pipeline_id = %ld",
        pipeline_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        char *options_json;

        pipeline->pipeline_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        pipeline->pipeline_name = SPI_getvalue(tuple, tupdesc, 2);
        pipeline->source_type = (PipelineSource)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        pipeline->target_table_oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        pipeline->format = (PipelineFormat)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));

        options_json = SPI_getvalue(tuple, tupdesc, 6);

        pipeline->state = (PipelineState)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        pipeline->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));

        /* Get started_at if not null */
        pipeline->started_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        if (isnull)
            pipeline->started_at = 0;

        /* Get stopped_at if not null */
        pipeline->stopped_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 10, &isnull));
        if (isnull)
            pipeline->stopped_at = 0;

        pipeline->worker_pid = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 11, &isnull));

        pipeline->batch_size = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 12, &isnull));
        if (isnull || pipeline->batch_size <= 0)
            pipeline->batch_size = PIPELINE_DEFAULT_BATCH_SIZE;

        pipeline->batch_timeout_ms = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 13, &isnull));
        if (isnull || pipeline->batch_timeout_ms <= 0)
            pipeline->batch_timeout_ms = PIPELINE_DEFAULT_BATCH_TIMEOUT_MS;

        pipeline->error_threshold = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 14, &isnull));
        if (isnull || pipeline->error_threshold <= 0)
            pipeline->error_threshold = PIPELINE_DEFAULT_ERROR_THRESHOLD;

        /* Parse source-specific configuration */
        if (options_json != NULL)
        {
            switch (pipeline->source_type)
            {
                case PIPELINE_SOURCE_KAFKA:
                    pipeline->source_config.kafka = parse_kafka_config(options_json);
                    break;
                case PIPELINE_SOURCE_S3:
                    pipeline->source_config.s3 = parse_s3_config(options_json);
                    break;
                default:
                    break;
            }
        }

        /* Initialize stats */
        pipeline->stats = palloc0(sizeof(PipelineStats));
    }
    else
    {
        /* Clean up memory context before returning NULL */
        if (pipeline->pipeline_context)
            MemoryContextDelete(pipeline->pipeline_context);
        pfree(pipeline);
        pipeline = NULL;
    }

    pfree(query.data);
    SPI_finish();

    return pipeline;
}

static void
process_pipeline_batch(Pipeline *pipeline)
{
    /* Implementation depends on source type */
    switch (pipeline->source_type)
    {
        case PIPELINE_SOURCE_KAFKA:
            /* Call kafka_source_poll and process messages */
            elog(DEBUG1, "Processing Kafka batch for pipeline %ld",
                 pipeline->pipeline_id);
            break;

        case PIPELINE_SOURCE_S3:
            /* Call s3_source_list_new_files and process */
            elog(DEBUG1, "Processing S3 files for pipeline %ld",
                 pipeline->pipeline_id);
            break;

        case PIPELINE_SOURCE_FILESYSTEM:
            elog(DEBUG1, "Processing filesystem for pipeline %ld",
                 pipeline->pipeline_id);
            break;

        default:
            elog(WARNING, "Unsupported source type for pipeline %ld",
                 pipeline->pipeline_id);
            break;
    }
}

static void
create_pipeline_checkpoint_internal(Pipeline *pipeline)
{
    StringInfoData query;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_pipeline_checkpoints "
        "(pipeline_id, checkpoint_time, messages_processed) "
        "VALUES (%ld, NOW(), %ld)",
        pipeline->pipeline_id,
        pipeline->stats ? pipeline->stats->messages_processed : 0);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(DEBUG1, "Created checkpoint for pipeline %ld", pipeline->pipeline_id);
}

static void
update_pipeline_stats(Pipeline *pipeline, int64 processed, int64 failed)
{
    StringInfoData query;

    if (pipeline == NULL)
        return;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_pipeline_stats "
        "(pipeline_id, messages_received, messages_processed, messages_failed, "
        "rows_inserted, last_message_time) "
        "VALUES (%ld, %ld, %ld, %ld, %ld, NOW()) "
        "ON CONFLICT (pipeline_id) DO UPDATE SET "
        "messages_received = orochi.orochi_pipeline_stats.messages_received + EXCLUDED.messages_received, "
        "messages_processed = orochi.orochi_pipeline_stats.messages_processed + EXCLUDED.messages_processed, "
        "messages_failed = orochi.orochi_pipeline_stats.messages_failed + EXCLUDED.messages_failed, "
        "rows_inserted = orochi.orochi_pipeline_stats.rows_inserted + EXCLUDED.rows_inserted, "
        "last_message_time = NOW()",
        pipeline->pipeline_id, processed, processed, failed, processed);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();
}

static bool
register_pipeline_worker(int64 pipeline_id, int pid)
{
    int i;

    if (pipeline_shared_state == NULL)
        return false;

    LWLockAcquire(pipeline_shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < 64; i++)
    {
        if (!pipeline_shared_state->entries[i].is_active)
        {
            pipeline_shared_state->entries[i].pipeline_id = pipeline_id;
            pipeline_shared_state->entries[i].worker_pid = pid;
            pipeline_shared_state->entries[i].state = PIPELINE_STATE_RUNNING;
            pipeline_shared_state->entries[i].is_active = true;
            pipeline_shared_state->entries[i].last_activity = GetCurrentTimestamp();
            pipeline_shared_state->num_active_pipelines++;
            break;
        }
    }

    LWLockRelease(pipeline_shared_state->lock);

    return (i < 64);
}

static void
unregister_pipeline_worker(int64 pipeline_id)
{
    int i;

    if (pipeline_shared_state == NULL)
        return;

    LWLockAcquire(pipeline_shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < 64; i++)
    {
        if (pipeline_shared_state->entries[i].is_active &&
            pipeline_shared_state->entries[i].pipeline_id == pipeline_id)
        {
            pipeline_shared_state->entries[i].is_active = false;
            pipeline_shared_state->num_active_pipelines--;
            break;
        }
    }

    LWLockRelease(pipeline_shared_state->lock);
}

static KafkaSourceConfig *
parse_kafka_config(const char *json)
{
    KafkaSourceConfig *config;
    Datum json_datum;
    Jsonb *jb;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;

    config = palloc0(sizeof(KafkaSourceConfig));

    /* Default values */
    config->fetch_min_bytes = KAFKA_DEFAULT_FETCH_MIN_BYTES;
    config->fetch_max_wait_ms = KAFKA_DEFAULT_FETCH_MAX_WAIT_MS;
    config->session_timeout_ms = KAFKA_DEFAULT_SESSION_TIMEOUT_MS;
    config->heartbeat_interval_ms = KAFKA_DEFAULT_HEARTBEAT_MS;
    config->enable_auto_commit = false;  /* We handle commits manually */
    config->partition = -1;  /* All partitions */
    config->start_offset = -1;  /* Latest */

    if (json == NULL || strlen(json) == 0 || strcmp(json, "{}") == 0)
        return config;

    /* Parse JSON using JSONB functions */
    PG_TRY();
    {
        jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));

        it = JsonbIteratorInit(&jb->root);

        while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
        {
            if (r == WJB_KEY)
            {
                char *key = pnstrdup(v.val.string.val, v.val.string.len);

                /* Get the value */
                r = JsonbIteratorNext(&it, &v, false);

                if (strcmp(key, "bootstrap_servers") == 0 || strcmp(key, "brokers") == 0)
                {
                    if (v.type == jbvString)
                        config->bootstrap_servers = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "topic") == 0)
                {
                    if (v.type == jbvString)
                        config->topic = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "consumer_group") == 0 || strcmp(key, "group_id") == 0)
                {
                    if (v.type == jbvString)
                        config->consumer_group = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "partition") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->partition = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                            NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "start_offset") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->start_offset = DatumGetInt64(DirectFunctionCall1(numeric_int8,
                                               NumericGetDatum(v.val.numeric)));
                    else if (v.type == jbvString)
                    {
                        char *offset_str = pnstrdup(v.val.string.val, v.val.string.len);
                        if (strcmp(offset_str, "earliest") == 0)
                            config->start_offset = -2;
                        else if (strcmp(offset_str, "latest") == 0)
                            config->start_offset = -1;
                        pfree(offset_str);
                    }
                }
                else if (strcmp(key, "fetch_min_bytes") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->fetch_min_bytes = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                  NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "fetch_max_wait_ms") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->fetch_max_wait_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                    NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "session_timeout_ms") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->session_timeout_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                     NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "heartbeat_interval_ms") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->heartbeat_interval_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                        NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "security_protocol") == 0)
                {
                    if (v.type == jbvString)
                        config->security_protocol = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "sasl_mechanism") == 0)
                {
                    if (v.type == jbvString)
                        config->sasl_mechanism = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "sasl_username") == 0)
                {
                    if (v.type == jbvString)
                        config->sasl_username = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "sasl_password") == 0)
                {
                    if (v.type == jbvString)
                        config->sasl_password = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "enable_auto_commit") == 0)
                {
                    if (v.type == jbvBool)
                        config->enable_auto_commit = v.val.boolean;
                }
                else if (strcmp(key, "auto_commit_interval") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->auto_commit_interval = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                       NumericGetDatum(v.val.numeric)));
                }

                pfree(key);
            }
        }
    }
    PG_CATCH();
    {
        elog(WARNING, "Failed to parse Kafka JSON configuration, using defaults");
        FlushErrorState();
    }
    PG_END_TRY();

    return config;
}

static S3SourceConfig *
parse_s3_config(const char *json)
{
    S3SourceConfig *config;
    Jsonb *jb;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;

    config = palloc0(sizeof(S3SourceConfig));

    /* Default values */
    config->poll_interval_sec = S3_DEFAULT_POLL_INTERVAL_SEC;
    config->delete_after_process = false;
    config->max_file_size = S3_DEFAULT_MAX_FILE_SIZE;
    config->use_ssl = true;

    if (json == NULL || strlen(json) == 0 || strcmp(json, "{}") == 0)
        return config;

    /* Parse JSON using JSONB functions */
    PG_TRY();
    {
        jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));

        it = JsonbIteratorInit(&jb->root);

        while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
        {
            if (r == WJB_KEY)
            {
                char *key = pnstrdup(v.val.string.val, v.val.string.len);

                /* Get the value */
                r = JsonbIteratorNext(&it, &v, false);

                if (strcmp(key, "bucket") == 0)
                {
                    if (v.type == jbvString)
                        config->bucket = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "prefix") == 0)
                {
                    if (v.type == jbvString)
                        config->prefix = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "region") == 0)
                {
                    if (v.type == jbvString)
                        config->region = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "endpoint") == 0)
                {
                    if (v.type == jbvString)
                        config->endpoint = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "access_key") == 0 || strcmp(key, "access_key_id") == 0)
                {
                    if (v.type == jbvString)
                        config->access_key = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "secret_key") == 0 || strcmp(key, "secret_access_key") == 0)
                {
                    if (v.type == jbvString)
                        config->secret_key = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "poll_interval_sec") == 0 || strcmp(key, "poll_interval") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->poll_interval_sec = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                    NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "delete_after_process") == 0)
                {
                    if (v.type == jbvBool)
                        config->delete_after_process = v.val.boolean;
                }
                else if (strcmp(key, "file_pattern") == 0 || strcmp(key, "pattern") == 0)
                {
                    if (v.type == jbvString)
                        config->file_pattern = pnstrdup(v.val.string.val, v.val.string.len);
                }
                else if (strcmp(key, "max_file_size") == 0)
                {
                    if (v.type == jbvNumeric)
                        config->max_file_size = DatumGetInt64(DirectFunctionCall1(numeric_int8,
                                                NumericGetDatum(v.val.numeric)));
                }
                else if (strcmp(key, "use_ssl") == 0)
                {
                    if (v.type == jbvBool)
                        config->use_ssl = v.val.boolean;
                }

                pfree(key);
            }
        }
    }
    PG_CATCH();
    {
        elog(WARNING, "Failed to parse S3 JSON configuration, using defaults");
        FlushErrorState();
    }
    PG_END_TRY();

    return config;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *
pipeline_source_name(PipelineSource source)
{
    switch (source)
    {
        case PIPELINE_SOURCE_KAFKA:      return "kafka";
        case PIPELINE_SOURCE_S3:         return "s3";
        case PIPELINE_SOURCE_FILESYSTEM: return "filesystem";
        case PIPELINE_SOURCE_HTTP:       return "http";
        case PIPELINE_SOURCE_KINESIS:    return "kinesis";
        default:                         return "unknown";
    }
}

PipelineSource
pipeline_parse_source(const char *name)
{
    if (pg_strcasecmp(name, "kafka") == 0)      return PIPELINE_SOURCE_KAFKA;
    if (pg_strcasecmp(name, "s3") == 0)         return PIPELINE_SOURCE_S3;
    if (pg_strcasecmp(name, "filesystem") == 0) return PIPELINE_SOURCE_FILESYSTEM;
    if (pg_strcasecmp(name, "http") == 0)       return PIPELINE_SOURCE_HTTP;
    if (pg_strcasecmp(name, "kinesis") == 0)    return PIPELINE_SOURCE_KINESIS;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown pipeline source: %s", name)));
    return PIPELINE_SOURCE_KAFKA;
}

const char *
pipeline_state_name(PipelineState state)
{
    switch (state)
    {
        case PIPELINE_STATE_CREATED:  return "created";
        case PIPELINE_STATE_RUNNING:  return "running";
        case PIPELINE_STATE_PAUSED:   return "paused";
        case PIPELINE_STATE_STOPPED:  return "stopped";
        case PIPELINE_STATE_ERROR:    return "error";
        case PIPELINE_STATE_DRAINING: return "draining";
        default:                      return "unknown";
    }
}

PipelineState
pipeline_parse_state(const char *name)
{
    if (pg_strcasecmp(name, "created") == 0)  return PIPELINE_STATE_CREATED;
    if (pg_strcasecmp(name, "running") == 0)  return PIPELINE_STATE_RUNNING;
    if (pg_strcasecmp(name, "paused") == 0)   return PIPELINE_STATE_PAUSED;
    if (pg_strcasecmp(name, "stopped") == 0)  return PIPELINE_STATE_STOPPED;
    if (pg_strcasecmp(name, "error") == 0)    return PIPELINE_STATE_ERROR;
    if (pg_strcasecmp(name, "draining") == 0) return PIPELINE_STATE_DRAINING;

    return PIPELINE_STATE_CREATED;
}

const char *
pipeline_format_name(PipelineFormat format)
{
    switch (format)
    {
        case PIPELINE_FORMAT_JSON:    return "json";
        case PIPELINE_FORMAT_CSV:     return "csv";
        case PIPELINE_FORMAT_AVRO:    return "avro";
        case PIPELINE_FORMAT_PARQUET: return "parquet";
        case PIPELINE_FORMAT_LINE:    return "line";
        default:                      return "unknown";
    }
}

PipelineFormat
pipeline_parse_format(const char *name)
{
    if (pg_strcasecmp(name, "json") == 0)    return PIPELINE_FORMAT_JSON;
    if (pg_strcasecmp(name, "csv") == 0)     return PIPELINE_FORMAT_CSV;
    if (pg_strcasecmp(name, "avro") == 0)    return PIPELINE_FORMAT_AVRO;
    if (pg_strcasecmp(name, "parquet") == 0) return PIPELINE_FORMAT_PARQUET;
    if (pg_strcasecmp(name, "line") == 0)    return PIPELINE_FORMAT_LINE;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown pipeline format: %s", name)));
    return PIPELINE_FORMAT_JSON;
}

/* ============================================================
 * Batch Insert Functions
 * ============================================================ */

/*
 * pipeline_insert_batch
 *    Insert a batch of records into the target table
 *    Records is a List of char* (JSON strings)
 *    Returns the number of rows successfully inserted
 */
int64
pipeline_insert_batch(Pipeline *pipeline, List *records, bool use_copy)
{
    ListCell *lc;
    int64 inserted = 0;
    char *relname;
    Oid argtypes[1] = { JSONBOID };
    StringInfoData query;

    if (pipeline == NULL || records == NIL)
        return 0;

    /*
     * For now, use simple INSERT statements with parameterized queries.
     * TODO: Implement COPY protocol for better performance when use_copy is true
     */
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(WARNING, "pipeline_insert_batch: failed to connect to SPI");
        return 0;
    }

    relname = get_rel_name(pipeline->target_table_oid);
    if (relname == NULL)
    {
        elog(WARNING, "pipeline_insert_batch: target table not found");
        SPI_finish();
        return 0;
    }

    /* Build parameterized INSERT query */
    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO %s SELECT * FROM jsonb_populate_record(NULL::%s, $1)",
        quote_identifier(relname),
        quote_identifier(relname));

    foreach(lc, records)
    {
        char *record_json = (char *) lfirst(lc);
        Datum argvals[1];
        Jsonb *jb;
        int ret;

        if (record_json == NULL || record_json[0] == '\0')
            continue;

        /* Convert JSON string to jsonb datum */
        PG_TRY();
        {
            jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
                                                     CStringGetDatum(record_json)));
            argvals[0] = JsonbPGetDatum(jb);

            ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);
            if (ret == SPI_OK_INSERT)
                inserted++;
            else
                elog(DEBUG1, "pipeline_insert_batch: insert failed for record");
        }
        PG_CATCH();
        {
            /* Log and continue on error */
            elog(DEBUG1, "pipeline_insert_batch: failed to parse/insert record");
            FlushErrorState();
        }
        PG_END_TRY();
    }

    pfree(query.data);
    SPI_finish();

    return inserted;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_start_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_pause_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_resume_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_stop_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_drop_pipeline_sql);
PG_FUNCTION_INFO_V1(orochi_pipeline_status_sql);
PG_FUNCTION_INFO_V1(orochi_pipeline_stats_sql);

Datum
orochi_create_pipeline_sql(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *source_text = PG_GETARG_TEXT_PP(1);
    Oid target_table = PG_GETARG_OID(2);
    text *format_text = PG_GETARG_TEXT_PP(3);
    text *options_text = PG_ARGISNULL(4) ? NULL : PG_GETARG_TEXT_PP(4);

    char *name = text_to_cstring(name_text);
    char *source = text_to_cstring(source_text);
    char *format = text_to_cstring(format_text);
    char *options = options_text ? text_to_cstring(options_text) : NULL;

    PipelineSource source_type = pipeline_parse_source(source);
    PipelineFormat format_type = pipeline_parse_format(format);

    int64 pipeline_id = orochi_create_pipeline(name, source_type, target_table,
                                                format_type, options);

    PG_RETURN_INT64(pipeline_id);
}

Datum
orochi_start_pipeline_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    bool result = orochi_start_pipeline(pipeline_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_pause_pipeline_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    bool result = orochi_pause_pipeline(pipeline_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_resume_pipeline_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    bool result = orochi_resume_pipeline(pipeline_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_stop_pipeline_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    bool result = orochi_stop_pipeline(pipeline_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_drop_pipeline_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    bool if_exists = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
    bool result = orochi_drop_pipeline(pipeline_id, if_exists);
    PG_RETURN_BOOL(result);
}

Datum
orochi_pipeline_status_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    PipelineState state = orochi_get_pipeline_state(pipeline_id);
    PG_RETURN_TEXT_P(cstring_to_text(pipeline_state_name(state)));
}

Datum
orochi_pipeline_stats_sql(PG_FUNCTION_ARGS)
{
    int64 pipeline_id = PG_GETARG_INT64(0);
    PipelineStats *stats;
    TupleDesc tupdesc;
    Datum values[10];
    bool nulls[10];
    HeapTuple tuple;

    /* Build result tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    stats = orochi_get_pipeline_stats(pipeline_id);

    memset(nulls, 0, sizeof(nulls));

    values[0] = Int64GetDatum(stats->messages_received);
    values[1] = Int64GetDatum(stats->messages_processed);
    values[2] = Int64GetDatum(stats->messages_failed);
    values[3] = Int64GetDatum(stats->bytes_received);
    values[4] = Int64GetDatum(stats->rows_inserted);
    values[5] = TimestampTzGetDatum(stats->last_message_time);

    if (stats->last_error_time > 0)
        values[6] = TimestampTzGetDatum(stats->last_error_time);
    else
        nulls[6] = true;

    if (stats->last_error_message)
        values[7] = CStringGetTextDatum(stats->last_error_message);
    else
        nulls[7] = true;

    values[8] = Float8GetDatum(stats->avg_latency_ms);
    values[9] = Float8GetDatum(stats->throughput_per_sec);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
