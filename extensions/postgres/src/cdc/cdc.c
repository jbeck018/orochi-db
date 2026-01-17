/*-------------------------------------------------------------------------
 *
 * cdc.c
 *    Orochi DB Change Data Capture core implementation
 *
 * This module provides:
 *   - CDC event capture from WAL and Raft log
 *   - Event serialization to JSON format
 *   - Subscriber management (register/unregister)
 *   - Event delivery to subscribers
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
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "replication/logical.h"
#include "replication/origin.h"

#include "../orochi.h"
#include "../consensus/raft.h"
#include "cdc.h"

/* Background worker signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* Shared memory state for CDC registry */
typedef struct CDCSharedState
{
    LWLock         *lock;
    int             num_active_subscriptions;
    CDCRegistryEntry entries[CDC_MAX_SUBSCRIPTIONS];
} CDCSharedState;

static CDCSharedState *cdc_shared_state = NULL;

/* GUC variables */
int orochi_cdc_max_workers = 8;
int orochi_cdc_batch_size = CDC_DEFAULT_BATCH_SIZE;
int orochi_cdc_batch_timeout_ms = CDC_DEFAULT_BATCH_TIMEOUT_MS;

/* Forward declarations */
static void cdc_sighup_handler(SIGNAL_ARGS);
static void cdc_sigterm_handler(SIGNAL_ARGS);
static CDCSubscription *load_subscription_from_catalog(int64 subscription_id);
static bool save_subscription_to_catalog(CDCSubscription *sub);
static void process_subscription_events(CDCSubscription *sub, void *sink_context);
static void update_subscription_stats(CDCSubscription *sub, int64 sent, int64 failed);
static bool register_cdc_worker(int64 subscription_id, int pid);
static void unregister_cdc_worker(int64 subscription_id);
static CDCKafkaSinkConfig *parse_kafka_sink_config(const char *json);
static CDCWebhookSinkConfig *parse_webhook_sink_config(const char *json);
static CDCFileSinkConfig *parse_file_sink_config(const char *json);

/* ============================================================
 * Shared Memory Initialization
 * ============================================================ */

Size
orochi_cdc_shmem_size(void)
{
    return sizeof(CDCSharedState);
}

void
orochi_cdc_shmem_init(void)
{
    bool found;

    cdc_shared_state = (CDCSharedState *)
        ShmemInitStruct("Orochi CDC State",
                        sizeof(CDCSharedState),
                        &found);

    if (!found)
    {
        memset(cdc_shared_state, 0, sizeof(CDCSharedState));
        cdc_shared_state->lock = &(GetNamedLWLockTranche("orochi_cdc"))->lock;
        cdc_shared_state->num_active_subscriptions = 0;
    }
}

/* ============================================================
 * CDC Subscription Lifecycle Functions
 * ============================================================ */

/*
 * orochi_create_cdc_subscription
 *    Create a new CDC subscription
 */
int64
orochi_create_cdc_subscription(const char *name,
                               CDCSinkType sink_type,
                               CDCOutputFormat format,
                               const char *options_json)
{
    StringInfoData query;
    int64 subscription_id = 0;
    int ret;

    /* Validate inputs */
    if (name == NULL || strlen(name) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("CDC subscription name cannot be empty")));

    if (strlen(name) > CDC_MAX_SUBSCRIPTION_NAME)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("CDC subscription name too long (max %d characters)",
                        CDC_MAX_SUBSCRIPTION_NAME)));

    /* Create subscription record in catalog */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_cdc_subscriptions "
        "(name, sink_type, output_format, options, state, created_at) "
        "VALUES (%s, %d, %d, %s, %d, NOW()) "
        "RETURNING subscription_id",
        quote_literal_cstr(name),
        (int)sink_type,
        (int)format,
        quote_literal_cstr(options_json ? options_json : "{}"),
        (int)CDC_SUB_STATE_CREATED);

    ret = SPI_execute(query.data, false, 0);
    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        subscription_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                       SPI_tuptable->tupdesc, 1, &isnull));
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to create CDC subscription")));
    }

    pfree(query.data);
    SPI_finish();

    elog(NOTICE, "Created CDC subscription '%s' with ID %ld", name, subscription_id);

    return subscription_id;
}

/*
 * orochi_start_cdc_subscription
 *    Start a CDC subscription by launching its background worker
 */
bool
orochi_start_cdc_subscription(int64 subscription_id)
{
    CDCSubscription *sub;
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    pid_t pid;
    StringInfoData query;
    int ret;

    /* Load subscription configuration */
    sub = load_subscription_from_catalog(subscription_id);
    if (sub == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("CDC subscription %ld does not exist", subscription_id)));

    if (sub->state == CDC_SUB_STATE_ACTIVE)
    {
        elog(NOTICE, "CDC subscription %ld is already running", subscription_id);
        return true;
    }

    if (sub->state == CDC_SUB_STATE_ERROR)
    {
        elog(WARNING, "CDC subscription %ld is in error state, resetting...", subscription_id);
    }

    /* Configure background worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi_cdc_%ld", subscription_id);
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi CDC worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "cdc_worker_main");
    worker.bgw_main_arg = Int64GetDatum(subscription_id);
    worker.bgw_notify_pid = MyProcPid;

    /* Register and start worker */
    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("could not register CDC worker")));
    }

    /* Wait for worker to start */
    if (WaitForBackgroundWorkerStartup(handle, &pid) != BGWH_STARTED)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("CDC worker failed to start")));
    }

    /* Update subscription state in catalog */
    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_cdc_subscriptions "
        "SET state = %d, started_at = NOW(), worker_pid = %d "
        "WHERE subscription_id = %ld",
        (int)CDC_SUB_STATE_ACTIVE, pid, subscription_id);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    /* Register in shared memory */
    register_cdc_worker(subscription_id, pid);

    elog(NOTICE, "Started CDC subscription %ld with worker PID %d", subscription_id, pid);

    return true;
}

/*
 * orochi_pause_cdc_subscription
 *    Pause an active CDC subscription
 */
bool
orochi_pause_cdc_subscription(int64 subscription_id)
{
    StringInfoData query;
    int ret;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_cdc_subscriptions "
        "SET state = %d "
        "WHERE subscription_id = %ld AND state = %d",
        (int)CDC_SUB_STATE_PAUSED, subscription_id, (int)CDC_SUB_STATE_ACTIVE);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    if (ret == SPI_OK_UPDATE && SPI_processed > 0)
    {
        elog(NOTICE, "Paused CDC subscription %ld", subscription_id);
        return true;
    }

    elog(NOTICE, "CDC subscription %ld is not active", subscription_id);
    return false;
}

/*
 * orochi_resume_cdc_subscription
 *    Resume a paused CDC subscription
 */
bool
orochi_resume_cdc_subscription(int64 subscription_id)
{
    CDCSubscriptionState current_state;

    current_state = orochi_get_cdc_subscription_state(subscription_id);
    if (current_state != CDC_SUB_STATE_PAUSED)
    {
        elog(NOTICE, "CDC subscription %ld is not paused (state: %s)",
             subscription_id, cdc_subscription_state_name(current_state));
        return false;
    }

    return orochi_start_cdc_subscription(subscription_id);
}

/*
 * orochi_stop_cdc_subscription
 *    Stop a running CDC subscription
 */
bool
orochi_stop_cdc_subscription(int64 subscription_id)
{
    StringInfoData query;
    int ret;
    int worker_pid = 0;

    SPI_connect();

    /* Get worker PID */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT worker_pid FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = %ld",
        subscription_id);

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
            elog(WARNING, "failed to send SIGTERM to CDC worker %d", worker_pid);
        }
    }

    /* Update state to disabled */
    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE orochi.orochi_cdc_subscriptions "
        "SET state = %d, stopped_at = NOW(), worker_pid = NULL "
        "WHERE subscription_id = %ld",
        (int)CDC_SUB_STATE_DISABLED, subscription_id);

    SPI_execute(query.data, false, 0);
    pfree(query.data);
    SPI_finish();

    /* Unregister from shared memory */
    unregister_cdc_worker(subscription_id);

    elog(NOTICE, "Stopped CDC subscription %ld", subscription_id);
    return true;
}

/*
 * orochi_drop_cdc_subscription
 *    Drop a CDC subscription and all its data
 */
bool
orochi_drop_cdc_subscription(int64 subscription_id, bool if_exists)
{
    StringInfoData query;
    int ret;
    CDCSubscriptionState state;

    /* Check if subscription exists */
    state = orochi_get_cdc_subscription_state(subscription_id);

    /* Stop subscription if active */
    if (state == CDC_SUB_STATE_ACTIVE || state == CDC_SUB_STATE_PAUSED)
    {
        orochi_stop_cdc_subscription(subscription_id);
    }

    SPI_connect();

    /* Check existence if not already verified */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT 1 FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = %ld",
        subscription_id);
    ret = SPI_execute(query.data, true, 1);
    pfree(query.data);

    if (SPI_processed == 0)
    {
        SPI_finish();
        if (if_exists)
            return true;
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("CDC subscription %ld does not exist", subscription_id)));
    }

    /* Delete table filters */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_cdc_table_filters WHERE subscription_id = %ld",
        subscription_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Delete stats */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_cdc_stats WHERE subscription_id = %ld",
        subscription_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Delete subscription record */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = %ld",
        subscription_id);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(NOTICE, "Dropped CDC subscription %ld", subscription_id);
    return true;
}

/* ============================================================
 * CDC Information Functions
 * ============================================================ */

/*
 * orochi_get_cdc_subscription
 *    Get CDC subscription configuration
 */
CDCSubscription *
orochi_get_cdc_subscription(int64 subscription_id)
{
    return load_subscription_from_catalog(subscription_id);
}

/*
 * orochi_get_cdc_subscription_state
 *    Get current CDC subscription state
 */
CDCSubscriptionState
orochi_get_cdc_subscription_state(int64 subscription_id)
{
    StringInfoData query;
    CDCSubscriptionState state = CDC_SUB_STATE_CREATED;
    int ret;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT state FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = %ld",
        subscription_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        state = (CDCSubscriptionState)DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                                    SPI_tuptable->tupdesc, 1, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return state;
}

/*
 * orochi_get_cdc_stats
 *    Get CDC subscription statistics
 */
CDCStats *
orochi_get_cdc_stats(int64 subscription_id)
{
    CDCStats *stats;
    StringInfoData query;
    int ret;

    stats = palloc0(sizeof(CDCStats));
    stats->subscription_id = subscription_id;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT events_captured, events_sent, events_failed, events_filtered, "
        "bytes_sent, avg_latency_ms, throughput_per_sec, last_capture_time, "
        "last_send_time, current_lsn, confirmed_lsn "
        "FROM orochi.orochi_cdc_stats WHERE subscription_id = %ld",
        subscription_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        stats->events_captured = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        stats->events_sent = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        stats->events_failed = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        stats->events_filtered = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        stats->bytes_sent = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        stats->avg_latency_ms = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        stats->throughput_per_sec = DatumGetFloat8(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        stats->last_capture_time = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        stats->last_send_time = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        stats->current_lsn = DatumGetUInt64(SPI_getbinval(tuple, tupdesc, 10, &isnull));
        stats->confirmed_lsn = DatumGetUInt64(SPI_getbinval(tuple, tupdesc, 11, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return stats;
}

/*
 * orochi_list_cdc_subscriptions
 *    List all CDC subscriptions
 */
List *
orochi_list_cdc_subscriptions(void)
{
    List *result = NIL;
    StringInfoData query;
    int ret;
    int i;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfoString(&query,
        "SELECT subscription_id FROM orochi.orochi_cdc_subscriptions ORDER BY subscription_id");

    ret = SPI_execute(query.data, true, 0);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            bool isnull;
            int64 *sub_id = palloc(sizeof(int64));
            *sub_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[i],
                                                    SPI_tuptable->tupdesc, 1, &isnull));
            result = lappend(result, sub_id);
        }
    }

    pfree(query.data);
    SPI_finish();

    return result;
}

/* ============================================================
 * CDC Event Creation and Serialization
 * ============================================================ */

/*
 * cdc_create_event
 *    Create a CDC event from a table change
 */
CDCEvent *
cdc_create_event(CDCEventType type, Oid table_oid,
                 HeapTuple old_tuple, HeapTuple new_tuple)
{
    CDCEvent *event;
    Relation rel;
    TupleDesc tupdesc;
    char *schema_name;
    char *table_name;
    int natts;
    int i;

    event = palloc0(sizeof(CDCEvent));
    event->event_type = type;
    event->table_oid = table_oid;
    event->event_time = GetCurrentTimestamp();
    event->xid = GetCurrentTransactionId();
    event->lsn = GetXLogWriteRecPtr();

    /* Get table information */
    rel = RelationIdGetRelation(table_oid);
    if (!RelationIsValid(rel))
    {
        pfree(event);
        return NULL;
    }

    tupdesc = RelationGetDescr(rel);
    natts = tupdesc->natts;

    schema_name = get_namespace_name(rel->rd_rel->relnamespace);
    table_name = RelationGetRelationName(rel);

    event->schema_name = pstrdup(schema_name);
    event->table_name = pstrdup(table_name);
    event->num_columns = natts;

    /* Extract column values for before image */
    if (old_tuple != NULL && (type == CDC_EVENT_UPDATE || type == CDC_EVENT_DELETE))
    {
        event->before = palloc0(sizeof(CDCColumnValue) * natts);
        for (i = 0; i < natts; i++)
        {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            bool isnull;
            Datum value;

            if (attr->attisdropped)
                continue;

            event->before[i].column_name = pstrdup(NameStr(attr->attname));
            event->before[i].column_type = attr->atttypid;
            event->before[i].type_name = pstrdup(format_type_be(attr->atttypid));

            value = heap_getattr(old_tuple, i + 1, tupdesc, &isnull);
            event->before[i].is_null = isnull;

            if (!isnull)
            {
                Oid typoutput;
                bool typIsVarlena;

                getTypeOutputInfo(attr->atttypid, &typoutput, &typIsVarlena);
                event->before[i].value = OidOutputFunctionCall(typoutput, value);
            }
        }
    }

    /* Extract column values for after image */
    if (new_tuple != NULL && (type == CDC_EVENT_INSERT || type == CDC_EVENT_UPDATE))
    {
        event->after = palloc0(sizeof(CDCColumnValue) * natts);
        for (i = 0; i < natts; i++)
        {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            bool isnull;
            Datum value;

            if (attr->attisdropped)
                continue;

            event->after[i].column_name = pstrdup(NameStr(attr->attname));
            event->after[i].column_type = attr->atttypid;
            event->after[i].type_name = pstrdup(format_type_be(attr->atttypid));

            value = heap_getattr(new_tuple, i + 1, tupdesc, &isnull);
            event->after[i].is_null = isnull;

            if (!isnull)
            {
                Oid typoutput;
                bool typIsVarlena;

                getTypeOutputInfo(attr->atttypid, &typoutput, &typIsVarlena);
                event->after[i].value = OidOutputFunctionCall(typoutput, value);
            }
        }
    }

    RelationClose(rel);

    return event;
}

/*
 * cdc_free_event
 *    Free a CDC event and all its memory
 */
void
cdc_free_event(CDCEvent *event)
{
    int i;

    if (event == NULL)
        return;

    if (event->schema_name)
        pfree(event->schema_name);
    if (event->table_name)
        pfree(event->table_name);

    /* Free before image */
    if (event->before)
    {
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->before[i].column_name)
                pfree(event->before[i].column_name);
            if (event->before[i].type_name)
                pfree(event->before[i].type_name);
            if (event->before[i].value)
                pfree(event->before[i].value);
        }
        pfree(event->before);
    }

    /* Free after image */
    if (event->after)
    {
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->after[i].column_name)
                pfree(event->after[i].column_name);
            if (event->after[i].type_name)
                pfree(event->after[i].type_name);
            if (event->after[i].value)
                pfree(event->after[i].value);
        }
        pfree(event->after);
    }

    /* Free key columns */
    if (event->key_columns)
    {
        for (i = 0; i < event->num_key_columns; i++)
        {
            if (event->key_columns[i])
                pfree(event->key_columns[i]);
        }
        pfree(event->key_columns);
    }

    if (event->key_values)
    {
        for (i = 0; i < event->num_key_columns; i++)
        {
            if (event->key_values[i])
                pfree(event->key_values[i]);
        }
        pfree(event->key_values);
    }

    if (event->origin)
        pfree(event->origin);

    pfree(event);
}

/*
 * cdc_serialize_event_json
 *    Serialize a CDC event to JSON format
 */
char *
cdc_serialize_event_json(CDCEvent *event, CDCSubscription *sub)
{
    StringInfoData buf;
    int i;
    bool first;

    initStringInfo(&buf);

    appendStringInfoChar(&buf, '{');

    /* Event metadata */
    appendStringInfo(&buf, "\"event_type\": \"%s\"", cdc_event_type_name(event->event_type));
    appendStringInfo(&buf, ", \"timestamp\": \"%s\"",
                     timestamptz_to_str(event->event_time));
    appendStringInfo(&buf, ", \"lsn\": \"%X/%X\"",
                     LSN_FORMAT_ARGS(event->lsn));

    if (sub->include_transaction)
    {
        appendStringInfo(&buf, ", \"xid\": %u", event->xid);
        appendStringInfo(&buf, ", \"sequence\": %ld", event->sequence);
        if (event->is_last_in_txn)
            appendStringInfo(&buf, ", \"is_last_in_txn\": true");
    }

    /* Source information */
    if (sub->include_schema)
    {
        appendStringInfo(&buf, ", \"schema\": \"%s\"", event->schema_name);
    }
    appendStringInfo(&buf, ", \"table\": \"%s\"", event->table_name);

    /* Before image */
    if (sub->include_before && event->before != NULL)
    {
        appendStringInfo(&buf, ", \"before\": {");
        first = true;
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->before[i].column_name == NULL)
                continue;

            if (!first)
                appendStringInfoChar(&buf, ',');
            first = false;

            appendStringInfo(&buf, "\"%s\": ", event->before[i].column_name);
            if (event->before[i].is_null)
                appendStringInfo(&buf, "null");
            else
            {
                /* Escape JSON string */
                appendStringInfoChar(&buf, '"');
                escape_json(&buf, event->before[i].value);
                appendStringInfoChar(&buf, '"');
            }
        }
        appendStringInfoChar(&buf, '}');
    }

    /* After image */
    if (event->after != NULL)
    {
        appendStringInfo(&buf, ", \"after\": {");
        first = true;
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->after[i].column_name == NULL)
                continue;

            if (!first)
                appendStringInfoChar(&buf, ',');
            first = false;

            appendStringInfo(&buf, "\"%s\": ", event->after[i].column_name);
            if (event->after[i].is_null)
                appendStringInfo(&buf, "null");
            else
            {
                appendStringInfoChar(&buf, '"');
                escape_json(&buf, event->after[i].value);
                appendStringInfoChar(&buf, '"');
            }
        }
        appendStringInfoChar(&buf, '}');
    }

    appendStringInfoChar(&buf, '}');

    return buf.data;
}

/*
 * cdc_serialize_event_debezium
 *    Serialize a CDC event to Debezium-compatible JSON format
 */
char *
cdc_serialize_event_debezium(CDCEvent *event, CDCSubscription *sub)
{
    StringInfoData buf;
    int i;
    bool first;
    const char *op;

    initStringInfo(&buf);

    /* Map event type to Debezium operation */
    switch (event->event_type)
    {
        case CDC_EVENT_INSERT:
            op = "c";  /* create */
            break;
        case CDC_EVENT_UPDATE:
            op = "u";  /* update */
            break;
        case CDC_EVENT_DELETE:
            op = "d";  /* delete */
            break;
        case CDC_EVENT_TRUNCATE:
            op = "t";  /* truncate */
            break;
        default:
            op = "r";  /* read (snapshot) */
            break;
    }

    appendStringInfoChar(&buf, '{');

    /* Schema section */
    appendStringInfo(&buf, "\"schema\": {\"type\": \"struct\", \"name\": \"%s.%s.Envelope\"}",
                     event->schema_name, event->table_name);

    /* Payload section */
    appendStringInfo(&buf, ", \"payload\": {");

    /* Before image */
    if (event->before != NULL && (event->event_type == CDC_EVENT_UPDATE ||
                                   event->event_type == CDC_EVENT_DELETE))
    {
        appendStringInfo(&buf, "\"before\": {");
        first = true;
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->before[i].column_name == NULL)
                continue;

            if (!first)
                appendStringInfoChar(&buf, ',');
            first = false;

            appendStringInfo(&buf, "\"%s\": ", event->before[i].column_name);
            if (event->before[i].is_null)
                appendStringInfo(&buf, "null");
            else
            {
                appendStringInfoChar(&buf, '"');
                escape_json(&buf, event->before[i].value);
                appendStringInfoChar(&buf, '"');
            }
        }
        appendStringInfo(&buf, "}, ");
    }
    else
    {
        appendStringInfo(&buf, "\"before\": null, ");
    }

    /* After image */
    if (event->after != NULL && (event->event_type == CDC_EVENT_INSERT ||
                                  event->event_type == CDC_EVENT_UPDATE))
    {
        appendStringInfo(&buf, "\"after\": {");
        first = true;
        for (i = 0; i < event->num_columns; i++)
        {
            if (event->after[i].column_name == NULL)
                continue;

            if (!first)
                appendStringInfoChar(&buf, ',');
            first = false;

            appendStringInfo(&buf, "\"%s\": ", event->after[i].column_name);
            if (event->after[i].is_null)
                appendStringInfo(&buf, "null");
            else
            {
                appendStringInfoChar(&buf, '"');
                escape_json(&buf, event->after[i].value);
                appendStringInfoChar(&buf, '"');
            }
        }
        appendStringInfo(&buf, "}, ");
    }
    else
    {
        appendStringInfo(&buf, "\"after\": null, ");
    }

    /* Source metadata */
    appendStringInfo(&buf, "\"source\": {");
    appendStringInfo(&buf, "\"version\": \"1.0.0\"");
    appendStringInfo(&buf, ", \"connector\": \"orochi\"");
    appendStringInfo(&buf, ", \"name\": \"orochi-cdc\"");
    appendStringInfo(&buf, ", \"ts_ms\": %ld",
                     timestamptz_to_time_t(event->event_time) * 1000);
    appendStringInfo(&buf, ", \"schema\": \"%s\"", event->schema_name);
    appendStringInfo(&buf, ", \"table\": \"%s\"", event->table_name);
    appendStringInfo(&buf, ", \"lsn\": %lu", (unsigned long)event->lsn);
    appendStringInfo(&buf, ", \"xid\": %u", event->xid);
    appendStringInfo(&buf, "}, ");

    /* Operation */
    appendStringInfo(&buf, "\"op\": \"%s\"", op);
    appendStringInfo(&buf, ", \"ts_ms\": %ld",
                     timestamptz_to_time_t(GetCurrentTimestamp()) * 1000);

    appendStringInfo(&buf, "}}");

    return buf.data;
}

/* ============================================================
 * CDC Event Filtering
 * ============================================================ */

/*
 * cdc_event_matches_subscription
 *    Check if an event matches a subscription's filters
 */
bool
cdc_event_matches_subscription(CDCEvent *event, CDCSubscription *sub)
{
    /* Check event type filter */
    if (sub->event_filter != NULL)
    {
        switch (event->event_type)
        {
            case CDC_EVENT_INSERT:
                if (!sub->event_filter->include_insert)
                    return false;
                break;
            case CDC_EVENT_UPDATE:
                if (!sub->event_filter->include_update)
                    return false;
                break;
            case CDC_EVENT_DELETE:
                if (!sub->event_filter->include_delete)
                    return false;
                break;
            case CDC_EVENT_TRUNCATE:
                if (!sub->event_filter->include_truncate)
                    return false;
                break;
            case CDC_EVENT_DDL:
                if (!sub->event_filter->include_ddl)
                    return false;
                break;
            case CDC_EVENT_BEGIN:
                if (!sub->event_filter->include_begin)
                    return false;
                break;
            case CDC_EVENT_COMMIT:
                if (!sub->event_filter->include_commit)
                    return false;
                break;
        }
    }

    /* Check table filters */
    if (sub->num_table_filters > 0)
    {
        return cdc_table_matches_filter(event->schema_name, event->table_name,
                                        sub->table_filters, sub->num_table_filters);
    }

    return true;
}

/*
 * cdc_table_matches_filter
 *    Check if a table matches the filter patterns
 */
bool
cdc_table_matches_filter(const char *schema, const char *table,
                         CDCTableFilter *filters, int num_filters)
{
    int i;
    bool has_include = false;
    bool matched_include = false;

    for (i = 0; i < num_filters; i++)
    {
        bool schema_match = true;
        bool table_match = true;

        /* Check schema pattern */
        if (filters[i].schema_pattern != NULL)
        {
            if (strcmp(filters[i].schema_pattern, "*") != 0)
            {
                /* Simple wildcard matching */
                if (strchr(filters[i].schema_pattern, '*') != NULL)
                {
                    /* Pattern with wildcard - simple prefix match */
                    int len = strchr(filters[i].schema_pattern, '*') - filters[i].schema_pattern;
                    schema_match = (strncmp(schema, filters[i].schema_pattern, len) == 0);
                }
                else
                {
                    schema_match = (strcmp(schema, filters[i].schema_pattern) == 0);
                }
            }
        }

        /* Check table pattern */
        if (filters[i].table_pattern != NULL)
        {
            if (strcmp(filters[i].table_pattern, "*") != 0)
            {
                if (strchr(filters[i].table_pattern, '*') != NULL)
                {
                    int len = strchr(filters[i].table_pattern, '*') - filters[i].table_pattern;
                    table_match = (strncmp(table, filters[i].table_pattern, len) == 0);
                }
                else
                {
                    table_match = (strcmp(table, filters[i].table_pattern) == 0);
                }
            }
        }

        if (schema_match && table_match)
        {
            if (filters[i].include)
            {
                has_include = true;
                matched_include = true;
            }
            else
            {
                /* Exclude filter matched */
                return false;
            }
        }
        else if (filters[i].include)
        {
            has_include = true;
        }
    }

    /* If there are include filters, we must match at least one */
    if (has_include)
        return matched_include;

    /* No include filters means include everything not excluded */
    return true;
}

/* ============================================================
 * Background Worker Implementation
 * ============================================================ */

static void
cdc_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
cdc_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/*
 * cdc_worker_main
 *    Main entry point for CDC background worker
 */
void
cdc_worker_main(Datum main_arg)
{
    int64 subscription_id = DatumGetInt64(main_arg);
    CDCSubscription *sub;
    void *sink_context = NULL;

    /* Set up signal handlers */
    pqsignal(SIGHUP, cdc_sighup_handler);
    pqsignal(SIGTERM, cdc_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "CDC worker started for subscription %ld", subscription_id);

    /* Load subscription configuration */
    StartTransactionCommand();
    sub = load_subscription_from_catalog(subscription_id);
    CommitTransactionCommand();

    if (sub == NULL)
    {
        elog(ERROR, "Failed to load CDC subscription %ld configuration", subscription_id);
        proc_exit(1);
    }

    /* Initialize sink */
    sink_context = cdc_sink_init(sub);
    if (sink_context == NULL)
    {
        elog(ERROR, "Failed to initialize CDC sink for subscription %ld", subscription_id);
        proc_exit(1);
    }

    /* Main processing loop */
    while (!got_sigterm)
    {
        int rc;
        CDCSubscriptionState current_state;

        /* Wait for work or timeout */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       orochi_cdc_batch_timeout_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Handle SIGHUP - reload config */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            break;

        /* Check if subscription is paused */
        StartTransactionCommand();
        current_state = orochi_get_cdc_subscription_state(subscription_id);
        CommitTransactionCommand();

        if (current_state == CDC_SUB_STATE_PAUSED)
        {
            pg_usleep(1000000);  /* 1 second */
            continue;
        }

        if (current_state == CDC_SUB_STATE_DISABLED)
        {
            elog(LOG, "CDC subscription %ld disabled", subscription_id);
            break;
        }

        /* Process events */
        StartTransactionCommand();
        PG_TRY();
        {
            process_subscription_events(sub, sink_context);
            CommitTransactionCommand();
        }
        PG_CATCH();
        {
            ErrorData *edata;
            MemoryContext ecxt;

            ecxt = MemoryContextSwitchTo(sub->cdc_context ?
                                         sub->cdc_context : TopMemoryContext);
            edata = CopyErrorData();
            FlushErrorState();
            MemoryContextSwitchTo(ecxt);

            elog(WARNING, "CDC subscription %ld error: %s", subscription_id, edata->message);

            sub->events_failed++;
            sub->last_error_time = GetCurrentTimestamp();
            if (edata->message)
                sub->last_error_message = pstrdup(edata->message);

            AbortCurrentTransaction();

            FreeErrorData(edata);
        }
        PG_END_TRY();
    }

    /* Cleanup */
    elog(LOG, "CDC worker %ld shutting down", subscription_id);

    if (sink_context)
    {
        cdc_sink_flush(sink_context, sub->sink_type);
        cdc_sink_close(sink_context, sub->sink_type);
    }

    /* Update subscription state */
    StartTransactionCommand();
    SPI_connect();
    {
        StringInfoData query;
        initStringInfo(&query);
        appendStringInfo(&query,
            "UPDATE orochi.orochi_cdc_subscriptions "
            "SET state = %d, stopped_at = NOW(), worker_pid = NULL "
            "WHERE subscription_id = %ld AND state = %d",
            (int)CDC_SUB_STATE_DISABLED, subscription_id, (int)CDC_SUB_STATE_ACTIVE);
        SPI_execute(query.data, false, 0);
        pfree(query.data);
    }
    SPI_finish();
    CommitTransactionCommand();

    unregister_cdc_worker(subscription_id);

    proc_exit(0);
}

/* ============================================================
 * Internal Helper Functions
 * ============================================================ */

static CDCSubscription *
load_subscription_from_catalog(int64 subscription_id)
{
    CDCSubscription *sub;
    StringInfoData query;
    int ret;

    sub = palloc0(sizeof(CDCSubscription));
    sub->cdc_context = AllocSetContextCreate(TopMemoryContext,
                                             "CDC Subscription Context",
                                             ALLOCSET_DEFAULT_SIZES);

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT subscription_id, name, sink_type, output_format, options, "
        "state, created_at, started_at, stopped_at, worker_pid, "
        "include_schema, include_before, include_transaction, "
        "start_lsn, confirmed_lsn "
        "FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = %ld",
        subscription_id);

    ret = SPI_execute(query.data, true, 1);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        char *options_json;

        sub->subscription_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        sub->subscription_name = SPI_getvalue(tuple, tupdesc, 2);
        sub->sink_type = (CDCSinkType)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        sub->output_format = (CDCOutputFormat)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));

        options_json = SPI_getvalue(tuple, tupdesc, 5);

        sub->state = (CDCSubscriptionState)DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        sub->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        sub->started_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        sub->stopped_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        sub->worker_pid = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 10, &isnull));

        sub->include_schema = DatumGetBool(SPI_getbinval(tuple, tupdesc, 11, &isnull));
        if (isnull) sub->include_schema = true;

        sub->include_before = DatumGetBool(SPI_getbinval(tuple, tupdesc, 12, &isnull));
        if (isnull) sub->include_before = true;

        sub->include_transaction = DatumGetBool(SPI_getbinval(tuple, tupdesc, 13, &isnull));
        if (isnull) sub->include_transaction = false;

        sub->start_lsn = DatumGetUInt64(SPI_getbinval(tuple, tupdesc, 14, &isnull));
        sub->confirmed_lsn = DatumGetUInt64(SPI_getbinval(tuple, tupdesc, 15, &isnull));

        /* Parse sink-specific configuration */
        if (options_json != NULL)
        {
            switch (sub->sink_type)
            {
                case CDC_SINK_KAFKA:
                    sub->sink_config.kafka = parse_kafka_sink_config(options_json);
                    break;
                case CDC_SINK_WEBHOOK:
                    sub->sink_config.webhook = parse_webhook_sink_config(options_json);
                    break;
                case CDC_SINK_FILE:
                    sub->sink_config.file = parse_file_sink_config(options_json);
                    break;
                default:
                    break;
            }
        }

        /* Initialize event filter with defaults */
        sub->event_filter = palloc0(sizeof(CDCEventFilter));
        sub->event_filter->include_insert = true;
        sub->event_filter->include_update = true;
        sub->event_filter->include_delete = true;
        sub->event_filter->include_truncate = true;
        sub->event_filter->include_ddl = false;
        sub->event_filter->include_begin = false;
        sub->event_filter->include_commit = false;
    }
    else
    {
        if (sub->cdc_context)
            MemoryContextDelete(sub->cdc_context);
        pfree(sub);
        sub = NULL;
    }

    pfree(query.data);
    SPI_finish();

    return sub;
}

static void
process_subscription_events(CDCSubscription *sub, void *sink_context)
{
    /*
     * In a real implementation, this would:
     * 1. Read from logical replication slot
     * 2. Decode WAL records into CDC events
     * 3. Filter and serialize events
     * 4. Send to the configured sink
     */
    elog(DEBUG1, "Processing CDC events for subscription %ld",
         sub->subscription_id);
}

static void
update_subscription_stats(CDCSubscription *sub, int64 sent, int64 failed)
{
    StringInfoData query;

    if (sub == NULL)
        return;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO orochi.orochi_cdc_stats "
        "(subscription_id, events_captured, events_sent, events_failed, "
        "last_send_time, current_lsn, confirmed_lsn) "
        "VALUES (%ld, %ld, %ld, %ld, NOW(), %lu, %lu) "
        "ON CONFLICT (subscription_id) DO UPDATE SET "
        "events_captured = orochi.orochi_cdc_stats.events_captured + EXCLUDED.events_captured, "
        "events_sent = orochi.orochi_cdc_stats.events_sent + EXCLUDED.events_sent, "
        "events_failed = orochi.orochi_cdc_stats.events_failed + EXCLUDED.events_failed, "
        "last_send_time = NOW(), "
        "current_lsn = EXCLUDED.current_lsn, "
        "confirmed_lsn = EXCLUDED.confirmed_lsn",
        sub->subscription_id, sent + failed, sent, failed,
        (unsigned long)sub->start_lsn, (unsigned long)sub->confirmed_lsn);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();
}

static bool
register_cdc_worker(int64 subscription_id, int pid)
{
    int i;

    if (cdc_shared_state == NULL)
        return false;

    LWLockAcquire(cdc_shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < CDC_MAX_SUBSCRIPTIONS; i++)
    {
        if (!cdc_shared_state->entries[i].is_active)
        {
            cdc_shared_state->entries[i].subscription_id = subscription_id;
            cdc_shared_state->entries[i].worker_pid = pid;
            cdc_shared_state->entries[i].state = CDC_SUB_STATE_ACTIVE;
            cdc_shared_state->entries[i].is_active = true;
            cdc_shared_state->entries[i].last_activity = GetCurrentTimestamp();
            cdc_shared_state->num_active_subscriptions++;
            break;
        }
    }

    LWLockRelease(cdc_shared_state->lock);

    return (i < CDC_MAX_SUBSCRIPTIONS);
}

static void
unregister_cdc_worker(int64 subscription_id)
{
    int i;

    if (cdc_shared_state == NULL)
        return;

    LWLockAcquire(cdc_shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < CDC_MAX_SUBSCRIPTIONS; i++)
    {
        if (cdc_shared_state->entries[i].is_active &&
            cdc_shared_state->entries[i].subscription_id == subscription_id)
        {
            cdc_shared_state->entries[i].is_active = false;
            cdc_shared_state->num_active_subscriptions--;
            break;
        }
    }

    LWLockRelease(cdc_shared_state->lock);
}

static CDCKafkaSinkConfig *
parse_kafka_sink_config(const char *json)
{
    CDCKafkaSinkConfig *config;
    Jsonb      *jb;
    JsonbIterator *it;
    JsonbValue  key, val;
    JsonbIteratorToken r;

    config = palloc0(sizeof(CDCKafkaSinkConfig));

    /* Default values */
    config->batch_size = CDC_KAFKA_DEFAULT_BATCH_SIZE;
    config->linger_ms = CDC_KAFKA_DEFAULT_LINGER_MS;
    config->acks = CDC_KAFKA_DEFAULT_ACKS;
    config->retries = CDC_KAFKA_DEFAULT_RETRIES;
    config->retry_backoff_ms = CDC_KAFKA_DEFAULT_RETRY_BACKOFF_MS;
    config->partition = -1;

    if (json == NULL || strlen(json) == 0)
        return config;

    /* Parse JSON string to Jsonb */
    PG_TRY();
    {
        Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json));
        jb = DatumGetJsonbP(jsonb_datum);
    }
    PG_CATCH();
    {
        FlushErrorState();
        elog(WARNING, "CDC Kafka config: invalid JSON, using defaults");
        return config;
    }
    PG_END_TRY();

    /* Iterate through JSON object */
    it = JsonbIteratorInit(&jb->root);
    r = JsonbIteratorNext(&it, &val, false);

    if (r != WJB_BEGIN_OBJECT)
        return config;

    while ((r = JsonbIteratorNext(&it, &key, true)) == WJB_KEY)
    {
        char *keystr;

        /* Get key string */
        if (key.type != jbvString)
            continue;
        keystr = pnstrdup(key.val.string.val, key.val.string.len);

        /* Get value */
        r = JsonbIteratorNext(&it, &val, true);

        /* Extract fields based on key name */
        if (strcmp(keystr, "brokers") == 0 || strcmp(keystr, "bootstrap_servers") == 0)
        {
            if (val.type == jbvString)
                config->bootstrap_servers = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "topic") == 0)
        {
            if (val.type == jbvString)
                config->topic = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "client_id") == 0 || strcmp(keystr, "key_field") == 0)
        {
            if (val.type == jbvString)
                config->key_field = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "partition") == 0)
        {
            if (val.type == jbvNumeric)
                config->partition = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                  NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "batch_size") == 0)
        {
            if (val.type == jbvNumeric)
                config->batch_size = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                   NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "linger_ms") == 0)
        {
            if (val.type == jbvNumeric)
                config->linger_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                  NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "compression_type") == 0 || strcmp(keystr, "compression") == 0)
        {
            if (val.type == jbvString)
                config->compression_type = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "security_protocol") == 0 || strcmp(keystr, "security") == 0)
        {
            if (val.type == jbvString)
                config->security_protocol = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "sasl_mechanism") == 0)
        {
            if (val.type == jbvString)
                config->sasl_mechanism = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "sasl_username") == 0 || strcmp(keystr, "username") == 0)
        {
            if (val.type == jbvString)
                config->sasl_username = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "sasl_password") == 0 || strcmp(keystr, "password") == 0)
        {
            if (val.type == jbvString)
                config->sasl_password = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "acks") == 0)
        {
            if (val.type == jbvNumeric)
                config->acks = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                             NumericGetDatum(val.val.numeric)));
            else if (val.type == jbvString)
            {
                /* Handle "all" as -1 */
                char *acks_str = pnstrdup(val.val.string.val, val.val.string.len);
                if (strcmp(acks_str, "all") == 0)
                    config->acks = -1;
                else
                    config->acks = atoi(acks_str);
                pfree(acks_str);
            }
        }
        else if (strcmp(keystr, "retries") == 0)
        {
            if (val.type == jbvNumeric)
                config->retries = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "retry_backoff_ms") == 0)
        {
            if (val.type == jbvNumeric)
                config->retry_backoff_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                         NumericGetDatum(val.val.numeric)));
        }

        pfree(keystr);
    }

    return config;
}

static CDCWebhookSinkConfig *
parse_webhook_sink_config(const char *json)
{
    CDCWebhookSinkConfig *config;
    Jsonb      *jb;
    JsonbIterator *it;
    JsonbValue  key, val;
    JsonbIteratorToken r;

    config = palloc0(sizeof(CDCWebhookSinkConfig));

    /* Default values */
    config->method = pstrdup("POST");
    config->content_type = pstrdup("application/json");
    config->timeout_ms = CDC_DEFAULT_WEBHOOK_TIMEOUT_MS;
    config->retry_count = CDC_DEFAULT_RETRY_COUNT;
    config->retry_delay_ms = CDC_DEFAULT_RETRY_DELAY_MS;
    config->batch_mode = false;
    config->batch_size = CDC_DEFAULT_BATCH_SIZE;
    config->batch_timeout_ms = CDC_DEFAULT_BATCH_TIMEOUT_MS;

    if (json == NULL || strlen(json) == 0)
        return config;

    /* Parse JSON string to Jsonb */
    PG_TRY();
    {
        Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json));
        jb = DatumGetJsonbP(jsonb_datum);
    }
    PG_CATCH();
    {
        FlushErrorState();
        elog(WARNING, "CDC Webhook config: invalid JSON, using defaults");
        return config;
    }
    PG_END_TRY();

    /* Iterate through JSON object */
    it = JsonbIteratorInit(&jb->root);
    r = JsonbIteratorNext(&it, &val, false);

    if (r != WJB_BEGIN_OBJECT)
        return config;

    while ((r = JsonbIteratorNext(&it, &key, true)) == WJB_KEY)
    {
        char *keystr;

        /* Get key string */
        if (key.type != jbvString)
            continue;
        keystr = pnstrdup(key.val.string.val, key.val.string.len);

        /* Get value */
        r = JsonbIteratorNext(&it, &val, true);

        /* Extract fields based on key name */
        if (strcmp(keystr, "url") == 0 || strcmp(keystr, "endpoint") == 0)
        {
            if (val.type == jbvString)
                config->url = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "method") == 0)
        {
            if (val.type == jbvString)
            {
                pfree(config->method);
                config->method = pnstrdup(val.val.string.val, val.val.string.len);
            }
        }
        else if (strcmp(keystr, "content_type") == 0)
        {
            if (val.type == jbvString)
            {
                pfree(config->content_type);
                config->content_type = pnstrdup(val.val.string.val, val.val.string.len);
            }
        }
        else if (strcmp(keystr, "headers") == 0)
        {
            /* Headers can be an object or array */
            if (val.type == jbvBinary)
            {
                JsonbIterator *headers_it;
                JsonbValue header_key, header_val;
                JsonbIteratorToken hr;
                List *header_list = NIL;
                ListCell *lc;
                int i;

                headers_it = JsonbIteratorInit((JsonbContainer *)val.val.binary.data);
                hr = JsonbIteratorNext(&headers_it, &header_val, false);

                if (hr == WJB_BEGIN_OBJECT)
                {
                    /* Object format: {"key": "value", ...} */
                    while ((hr = JsonbIteratorNext(&headers_it, &header_key, true)) == WJB_KEY)
                    {
                        hr = JsonbIteratorNext(&headers_it, &header_val, true);
                        if (header_key.type == jbvString && header_val.type == jbvString)
                        {
                            StringInfoData hdr;
                            initStringInfo(&hdr);
                            appendStringInfo(&hdr, "%.*s=%.*s",
                                           header_key.val.string.len, header_key.val.string.val,
                                           header_val.val.string.len, header_val.val.string.val);
                            header_list = lappend(header_list, hdr.data);
                        }
                    }
                }
                else if (hr == WJB_BEGIN_ARRAY)
                {
                    /* Array format: ["Header: Value", ...] */
                    while ((hr = JsonbIteratorNext(&headers_it, &header_val, true)) != WJB_END_ARRAY)
                    {
                        if (hr == WJB_ELEM && header_val.type == jbvString)
                        {
                            header_list = lappend(header_list,
                                                  pnstrdup(header_val.val.string.val,
                                                          header_val.val.string.len));
                        }
                    }
                }

                /* Convert list to array */
                config->num_headers = list_length(header_list);
                if (config->num_headers > 0)
                {
                    config->headers = palloc(sizeof(char *) * config->num_headers);
                    i = 0;
                    foreach(lc, header_list)
                    {
                        config->headers[i++] = (char *)lfirst(lc);
                    }
                }
                list_free(header_list);
            }
        }
        else if (strcmp(keystr, "auth_type") == 0 || strcmp(keystr, "auth") == 0)
        {
            if (val.type == jbvString)
                config->auth_type = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "auth_credentials") == 0 || strcmp(keystr, "credentials") == 0 ||
                 strcmp(keystr, "token") == 0 || strcmp(keystr, "api_key") == 0)
        {
            if (val.type == jbvString)
                config->auth_credentials = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "timeout_ms") == 0 || strcmp(keystr, "timeout") == 0)
        {
            if (val.type == jbvNumeric)
                config->timeout_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                   NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "retry_count") == 0 || strcmp(keystr, "retries") == 0)
        {
            if (val.type == jbvNumeric)
                config->retry_count = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                    NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "retry_delay_ms") == 0)
        {
            if (val.type == jbvNumeric)
                config->retry_delay_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                       NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "batch_mode") == 0 || strcmp(keystr, "batch") == 0)
        {
            if (val.type == jbvBool)
                config->batch_mode = val.val.boolean;
        }
        else if (strcmp(keystr, "batch_size") == 0)
        {
            if (val.type == jbvNumeric)
                config->batch_size = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                   NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "batch_timeout_ms") == 0)
        {
            if (val.type == jbvNumeric)
                config->batch_timeout_ms = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                         NumericGetDatum(val.val.numeric)));
        }

        pfree(keystr);
    }

    return config;
}

static CDCFileSinkConfig *
parse_file_sink_config(const char *json)
{
    CDCFileSinkConfig *config;
    Jsonb      *jb;
    JsonbIterator *it;
    JsonbValue  key, val;
    JsonbIteratorToken r;

    config = palloc0(sizeof(CDCFileSinkConfig));

    /* Default values */
    config->max_file_size = CDC_FILE_DEFAULT_MAX_SIZE;
    config->max_files = CDC_FILE_DEFAULT_MAX_FILES;
    config->append_mode = true;
    config->flush_immediate = false;
    config->pretty_print = false;

    if (json == NULL || strlen(json) == 0)
        return config;

    /* Parse JSON string to Jsonb */
    PG_TRY();
    {
        Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json));
        jb = DatumGetJsonbP(jsonb_datum);
    }
    PG_CATCH();
    {
        FlushErrorState();
        elog(WARNING, "CDC File config: invalid JSON, using defaults");
        return config;
    }
    PG_END_TRY();

    /* Iterate through JSON object */
    it = JsonbIteratorInit(&jb->root);
    r = JsonbIteratorNext(&it, &val, false);

    if (r != WJB_BEGIN_OBJECT)
        return config;

    while ((r = JsonbIteratorNext(&it, &key, true)) == WJB_KEY)
    {
        char *keystr;

        /* Get key string */
        if (key.type != jbvString)
            continue;
        keystr = pnstrdup(key.val.string.val, key.val.string.len);

        /* Get value */
        r = JsonbIteratorNext(&it, &val, true);

        /* Extract fields based on key name */
        if (strcmp(keystr, "path") == 0 || strcmp(keystr, "file_path") == 0 ||
            strcmp(keystr, "file") == 0)
        {
            if (val.type == jbvString)
                config->file_path = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "format") == 0 || strcmp(keystr, "rotation_pattern") == 0 ||
                 strcmp(keystr, "pattern") == 0)
        {
            if (val.type == jbvString)
                config->rotation_pattern = pnstrdup(val.val.string.val, val.val.string.len);
        }
        else if (strcmp(keystr, "max_file_size") == 0 || strcmp(keystr, "max_size") == 0)
        {
            if (val.type == jbvNumeric)
                config->max_file_size = DatumGetInt64(DirectFunctionCall1(numeric_int8,
                                                      NumericGetDatum(val.val.numeric)));
            else if (val.type == jbvString)
            {
                /* Parse size strings like "100MB", "1GB" */
                char *size_str = pnstrdup(val.val.string.val, val.val.string.len);
                char *endptr;
                int64 size = strtoll(size_str, &endptr, 10);

                if (endptr != size_str)
                {
                    /* Check for size suffix */
                    if (*endptr == 'K' || *endptr == 'k')
                        size *= 1024;
                    else if (*endptr == 'M' || *endptr == 'm')
                        size *= 1024 * 1024;
                    else if (*endptr == 'G' || *endptr == 'g')
                        size *= 1024 * 1024 * 1024;
                }
                config->max_file_size = size;
                pfree(size_str);
            }
        }
        else if (strcmp(keystr, "max_files") == 0 || strcmp(keystr, "rotation_count") == 0)
        {
            if (val.type == jbvNumeric)
                config->max_files = DatumGetInt32(DirectFunctionCall1(numeric_int4,
                                                  NumericGetDatum(val.val.numeric)));
        }
        else if (strcmp(keystr, "append_mode") == 0 || strcmp(keystr, "append") == 0)
        {
            if (val.type == jbvBool)
                config->append_mode = val.val.boolean;
        }
        else if (strcmp(keystr, "flush_immediate") == 0 || strcmp(keystr, "flush") == 0 ||
                 strcmp(keystr, "sync") == 0)
        {
            if (val.type == jbvBool)
                config->flush_immediate = val.val.boolean;
        }
        else if (strcmp(keystr, "pretty_print") == 0 || strcmp(keystr, "pretty") == 0)
        {
            if (val.type == jbvBool)
                config->pretty_print = val.val.boolean;
        }

        pfree(keystr);
    }

    return config;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *
cdc_event_type_name(CDCEventType type)
{
    switch (type)
    {
        case CDC_EVENT_INSERT:   return "insert";
        case CDC_EVENT_UPDATE:   return "update";
        case CDC_EVENT_DELETE:   return "delete";
        case CDC_EVENT_TRUNCATE: return "truncate";
        case CDC_EVENT_BEGIN:    return "begin";
        case CDC_EVENT_COMMIT:   return "commit";
        case CDC_EVENT_DDL:      return "ddl";
        default:                 return "unknown";
    }
}

CDCEventType
cdc_parse_event_type(const char *name)
{
    if (pg_strcasecmp(name, "insert") == 0)   return CDC_EVENT_INSERT;
    if (pg_strcasecmp(name, "update") == 0)   return CDC_EVENT_UPDATE;
    if (pg_strcasecmp(name, "delete") == 0)   return CDC_EVENT_DELETE;
    if (pg_strcasecmp(name, "truncate") == 0) return CDC_EVENT_TRUNCATE;
    if (pg_strcasecmp(name, "begin") == 0)    return CDC_EVENT_BEGIN;
    if (pg_strcasecmp(name, "commit") == 0)   return CDC_EVENT_COMMIT;
    if (pg_strcasecmp(name, "ddl") == 0)      return CDC_EVENT_DDL;

    return CDC_EVENT_INSERT;
}

const char *
cdc_sink_type_name(CDCSinkType type)
{
    switch (type)
    {
        case CDC_SINK_KAFKA:   return "kafka";
        case CDC_SINK_WEBHOOK: return "webhook";
        case CDC_SINK_FILE:    return "file";
        case CDC_SINK_KINESIS: return "kinesis";
        case CDC_SINK_PUBSUB:  return "pubsub";
        default:               return "unknown";
    }
}

CDCSinkType
cdc_parse_sink_type(const char *name)
{
    if (pg_strcasecmp(name, "kafka") == 0)   return CDC_SINK_KAFKA;
    if (pg_strcasecmp(name, "webhook") == 0) return CDC_SINK_WEBHOOK;
    if (pg_strcasecmp(name, "file") == 0)    return CDC_SINK_FILE;
    if (pg_strcasecmp(name, "kinesis") == 0) return CDC_SINK_KINESIS;
    if (pg_strcasecmp(name, "pubsub") == 0)  return CDC_SINK_PUBSUB;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown CDC sink type: %s", name)));
    return CDC_SINK_KAFKA;
}

const char *
cdc_subscription_state_name(CDCSubscriptionState state)
{
    switch (state)
    {
        case CDC_SUB_STATE_CREATED:  return "created";
        case CDC_SUB_STATE_ACTIVE:   return "active";
        case CDC_SUB_STATE_PAUSED:   return "paused";
        case CDC_SUB_STATE_ERROR:    return "error";
        case CDC_SUB_STATE_DISABLED: return "disabled";
        default:                     return "unknown";
    }
}

const char *
cdc_output_format_name(CDCOutputFormat format)
{
    switch (format)
    {
        case CDC_FORMAT_JSON:     return "json";
        case CDC_FORMAT_AVRO:     return "avro";
        case CDC_FORMAT_DEBEZIUM: return "debezium";
        case CDC_FORMAT_PROTOBUF: return "protobuf";
        default:                  return "unknown";
    }
}

CDCOutputFormat
cdc_parse_output_format(const char *name)
{
    if (pg_strcasecmp(name, "json") == 0)     return CDC_FORMAT_JSON;
    if (pg_strcasecmp(name, "avro") == 0)     return CDC_FORMAT_AVRO;
    if (pg_strcasecmp(name, "debezium") == 0) return CDC_FORMAT_DEBEZIUM;
    if (pg_strcasecmp(name, "protobuf") == 0) return CDC_FORMAT_PROTOBUF;

    return CDC_FORMAT_JSON;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_create_cdc_subscription_sql);
PG_FUNCTION_INFO_V1(orochi_drop_cdc_subscription_sql);
PG_FUNCTION_INFO_V1(orochi_start_cdc_subscription_sql);
PG_FUNCTION_INFO_V1(orochi_stop_cdc_subscription_sql);
PG_FUNCTION_INFO_V1(orochi_list_cdc_subscriptions_sql);
PG_FUNCTION_INFO_V1(orochi_cdc_subscription_stats_sql);

Datum
orochi_create_cdc_subscription_sql(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *sink_text = PG_GETARG_TEXT_PP(1);
    text *format_text = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);
    text *options_text = PG_ARGISNULL(3) ? NULL : PG_GETARG_TEXT_PP(3);

    char *name = text_to_cstring(name_text);
    char *sink = text_to_cstring(sink_text);
    char *format = format_text ? text_to_cstring(format_text) : "json";
    char *options = options_text ? text_to_cstring(options_text) : NULL;

    CDCSinkType sink_type = cdc_parse_sink_type(sink);
    CDCOutputFormat output_format = cdc_parse_output_format(format);

    int64 subscription_id = orochi_create_cdc_subscription(name, sink_type,
                                                            output_format, options);

    PG_RETURN_INT64(subscription_id);
}

Datum
orochi_drop_cdc_subscription_sql(PG_FUNCTION_ARGS)
{
    int64 subscription_id = PG_GETARG_INT64(0);
    bool if_exists = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
    bool result = orochi_drop_cdc_subscription(subscription_id, if_exists);
    PG_RETURN_BOOL(result);
}

Datum
orochi_start_cdc_subscription_sql(PG_FUNCTION_ARGS)
{
    int64 subscription_id = PG_GETARG_INT64(0);
    bool result = orochi_start_cdc_subscription(subscription_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_stop_cdc_subscription_sql(PG_FUNCTION_ARGS)
{
    int64 subscription_id = PG_GETARG_INT64(0);
    bool result = orochi_stop_cdc_subscription(subscription_id);
    PG_RETURN_BOOL(result);
}

Datum
orochi_list_cdc_subscriptions_sql(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        TupleDesc tupdesc;
        List *subscriptions;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Get list of subscriptions */
        subscriptions = orochi_list_cdc_subscriptions();
        funcctx->max_calls = list_length(subscriptions);
        funcctx->user_fctx = subscriptions;

        /* Build tuple descriptor */
        tupdesc = CreateTemplateTupleDesc(6);
        TupleDescInitEntry(tupdesc, 1, "subscription_id", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "name", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "sink_type", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "state", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "events_sent", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 6, "created_at", TIMESTAMPTZOID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if (call_cntr < max_calls)
    {
        List *subscriptions = (List *)funcctx->user_fctx;
        int64 *sub_id = (int64 *)list_nth(subscriptions, call_cntr);
        CDCSubscription *sub;
        Datum values[6];
        bool nulls[6];
        HeapTuple tuple;

        memset(nulls, 0, sizeof(nulls));

        sub = orochi_get_cdc_subscription(*sub_id);
        if (sub != NULL)
        {
            values[0] = Int64GetDatum(sub->subscription_id);
            values[1] = CStringGetTextDatum(sub->subscription_name);
            values[2] = CStringGetTextDatum(cdc_sink_type_name(sub->sink_type));
            values[3] = CStringGetTextDatum(cdc_subscription_state_name(sub->state));
            values[4] = Int64GetDatum(sub->events_sent);
            values[5] = TimestampTzGetDatum(sub->created_at);

            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
            SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
        }
    }

    SRF_RETURN_DONE(funcctx);
}

Datum
orochi_cdc_subscription_stats_sql(PG_FUNCTION_ARGS)
{
    int64 subscription_id = PG_GETARG_INT64(0);
    CDCStats *stats;
    TupleDesc tupdesc;
    Datum values[11];
    bool nulls[11];
    HeapTuple tuple;

    /* Build result tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    stats = orochi_get_cdc_stats(subscription_id);

    memset(nulls, 0, sizeof(nulls));

    values[0] = Int64GetDatum(stats->events_captured);
    values[1] = Int64GetDatum(stats->events_sent);
    values[2] = Int64GetDatum(stats->events_failed);
    values[3] = Int64GetDatum(stats->events_filtered);
    values[4] = Int64GetDatum(stats->bytes_sent);
    values[5] = Float8GetDatum(stats->avg_latency_ms);
    values[6] = Float8GetDatum(stats->throughput_per_sec);

    if (stats->last_capture_time > 0)
        values[7] = TimestampTzGetDatum(stats->last_capture_time);
    else
        nulls[7] = true;

    if (stats->last_send_time > 0)
        values[8] = TimestampTzGetDatum(stats->last_send_time);
    else
        nulls[8] = true;

    values[9] = UInt64GetDatum(stats->current_lsn);
    values[10] = UInt64GetDatum(stats->confirmed_lsn);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
