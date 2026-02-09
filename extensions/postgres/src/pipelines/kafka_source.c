/*-------------------------------------------------------------------------
 *
 * kafka_source.c
 *    Orochi DB Kafka consumer implementation for data pipelines
 *
 * This module provides Kafka message consumption with exactly-once
 * semantics support through manual offset management and integration
 * with PostgreSQL's transaction system.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"

#include "../orochi.h"
#include "pipeline.h"

/*
 * Note: This implementation provides a wrapper around librdkafka.
 * In production, you would link against librdkafka and use its APIs.
 * For now, we provide a stub implementation that can be replaced
 * with actual Kafka integration.
 */

#ifdef HAVE_LIBRDKAFKA
#include <librdkafka/rdkafka.h>
#endif

/* ============================================================
 * Kafka Consumer Context
 * ============================================================ */

typedef struct KafkaConsumer {
    int64 pipeline_id;
    KafkaSourceConfig *config;

#ifdef HAVE_LIBRDKAFKA
    rd_kafka_t *rk; /* Kafka handle */
    rd_kafka_topic_partition_list_t *topics;
    /* Note: conf is consumed by rd_kafka_new, do not store or free it */
#endif

    /* State tracking */
    bool is_initialized;
    bool is_subscribed;
    int64 current_offset;
    int32 current_partition;
    int64 committed_offset;

    /* Statistics */
    int64 messages_received;
    int64 messages_committed;
    int64 errors;
    TimestampTz last_poll_time;
    TimestampTz last_commit_time;

    /* Error handling */
    char last_error[PIPELINE_MAX_ERROR_MESSAGE];
    int consecutive_errors;

    /* Memory context */
    MemoryContext context;
} KafkaConsumer;

/* ============================================================
 * Kafka Consumer Initialization
 * ============================================================ */

/*
 * kafka_source_init
 *    Initialize Kafka consumer with given configuration
 */
void *kafka_source_init(KafkaSourceConfig *config)
{
    KafkaConsumer *consumer;
    MemoryContext oldcontext;
    MemoryContext kafka_context;

    if (config == NULL) {
        elog(ERROR, "Kafka configuration is required");
        return NULL;
    }

    if (config->bootstrap_servers == NULL || config->topic == NULL) {
        elog(ERROR, "Kafka bootstrap_servers and topic are required");
        return NULL;
    }

    /* Create memory context for Kafka consumer */
    kafka_context =
        AllocSetContextCreate(TopMemoryContext, "KafkaConsumer", ALLOCSET_DEFAULT_SIZES);

    oldcontext = MemoryContextSwitchTo(kafka_context);

    consumer = palloc0(sizeof(KafkaConsumer));
    consumer->context = kafka_context;
    consumer->config = config;
    consumer->is_initialized = false;
    consumer->is_subscribed = false;
    consumer->current_offset = config->start_offset;
    consumer->current_partition = config->partition;
    consumer->committed_offset = -1;

#ifdef HAVE_LIBRDKAFKA
    {
        char errstr[512];
        rd_kafka_conf_t *conf;
        rd_kafka_resp_err_t err;

        /* Create Kafka configuration */
        conf = rd_kafka_conf_new();

        /* Set bootstrap servers */
        if (rd_kafka_conf_set(conf, "bootstrap.servers", config->bootstrap_servers, errstr,
                              sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            elog(ERROR, "Failed to set Kafka bootstrap.servers: %s", errstr);
            rd_kafka_conf_destroy(conf);
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        /* Set consumer group */
        if (config->consumer_group) {
            if (rd_kafka_conf_set(conf, "group.id", config->consumer_group, errstr,
                                  sizeof(errstr)) != RD_KAFKA_CONF_OK) {
                elog(ERROR, "Failed to set Kafka group.id: %s", errstr);
                rd_kafka_conf_destroy(conf);
                MemoryContextSwitchTo(oldcontext);
                return NULL;
            }
        }

        /* Set auto offset reset */
        if (rd_kafka_conf_set(conf, "auto.offset.reset",
                              config->start_offset == -2 ? "earliest" : "latest", errstr,
                              sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            elog(WARNING, "Failed to set auto.offset.reset: %s", errstr);
        }

        /* Disable auto commit - we handle commits manually for exactly-once */
        if (rd_kafka_conf_set(conf, "enable.auto.commit", "false", errstr, sizeof(errstr)) !=
            RD_KAFKA_CONF_OK) {
            elog(WARNING, "Failed to disable auto.commit: %s", errstr);
        }

        /* Set session timeout */
        {
            char timeout_str[32];
            snprintf(timeout_str, sizeof(timeout_str), "%d", config->session_timeout_ms);
            rd_kafka_conf_set(conf, "session.timeout.ms", timeout_str, errstr, sizeof(errstr));
        }

        /* Set heartbeat interval */
        {
            char heartbeat_str[32];
            snprintf(heartbeat_str, sizeof(heartbeat_str), "%d", config->heartbeat_interval_ms);
            rd_kafka_conf_set(conf, "heartbeat.interval.ms", heartbeat_str, errstr, sizeof(errstr));
        }

        /* Configure security if specified */
        if (config->security_protocol) {
            rd_kafka_conf_set(conf, "security.protocol", config->security_protocol, errstr,
                              sizeof(errstr));
        }

        if (config->sasl_mechanism) {
            rd_kafka_conf_set(conf, "sasl.mechanism", config->sasl_mechanism, errstr,
                              sizeof(errstr));
        }

        if (config->sasl_username) {
            rd_kafka_conf_set(conf, "sasl.username", config->sasl_username, errstr, sizeof(errstr));
        }

        if (config->sasl_password) {
            rd_kafka_conf_set(conf, "sasl.password", config->sasl_password, errstr, sizeof(errstr));
        }

        /* Create Kafka consumer - note: rd_kafka_new takes ownership of conf */
        consumer->rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
        if (!consumer->rk) {
            elog(ERROR, "Failed to create Kafka consumer: %s", errstr);
            /* Note: rd_kafka_new destroys conf on failure, so no need to destroy it
             * here */
            MemoryContextSwitchTo(oldcontext);
            MemoryContextDelete(kafka_context);
            return NULL;
        }
        /* conf is now owned by consumer->rk, do not store or destroy it */

        /* Redirect all messages to consumer_poll() */
        rd_kafka_poll_set_consumer(consumer->rk);

        /* Subscribe to topic */
        consumer->topics = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(consumer->topics, config->topic,
                                          config->partition >= 0 ? config->partition
                                                                 : RD_KAFKA_PARTITION_UA);

        err = rd_kafka_subscribe(consumer->rk, consumer->topics);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            snprintf(consumer->last_error, sizeof(consumer->last_error),
                     "Failed to subscribe to topic %s: %s", config->topic, rd_kafka_err2str(err));
            elog(ERROR, "%s", consumer->last_error);
            rd_kafka_destroy(consumer->rk);
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        consumer->is_subscribed = true;
        consumer->is_initialized = true;

        elog(LOG, "Kafka consumer initialized: brokers=%s, topic=%s, group=%s",
             config->bootstrap_servers, config->topic,
             config->consumer_group ? config->consumer_group : "(none)");
    }
#else
    /* Table-based queue fallback when librdkafka is not available */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT) {
            elog(ERROR, "Orochi: failed to connect to SPI for pipeline queue init");
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        PG_TRY();
        {
            char *quoted_topic;

            SPI_execute("CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_queue ("
                        "  message_id BIGSERIAL PRIMARY KEY,"
                        "  pipeline_id INTEGER NOT NULL,"
                        "  topic TEXT NOT NULL,"
                        "  partition_id INTEGER DEFAULT 0,"
                        "  payload BYTEA NOT NULL,"
                        "  created_at TIMESTAMPTZ DEFAULT now()"
                        ")",
                        false, 0);

            SPI_execute("CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_queue_cursors ("
                        "  pipeline_id INTEGER NOT NULL,"
                        "  topic TEXT NOT NULL,"
                        "  partition_id INTEGER DEFAULT 0,"
                        "  committed_offset BIGINT DEFAULT 0,"
                        "  current_offset BIGINT DEFAULT 0,"
                        "  PRIMARY KEY (pipeline_id, topic, partition_id)"
                        ")",
                        false, 0);

            /* Initialize cursor row for this pipeline */
            quoted_topic = quote_literal_cstr(config->topic);
            {
                char query[512];
                snprintf(query, sizeof(query),
                         "INSERT INTO orochi.orochi_pipeline_queue_cursors "
                         "(pipeline_id, topic, partition_id, committed_offset, current_offset) "
                         "VALUES (%d, %s, 0, 0, 0) "
                         "ON CONFLICT DO NOTHING",
                         (int)consumer->pipeline_id, quoted_topic);
                SPI_execute(query, false, 0);
            }
            pfree(quoted_topic);

            SPI_finish();

            consumer->is_initialized = true;
            consumer->is_subscribed = true;

            elog(LOG, "Orochi: pipeline %d using table-based queue (no Kafka)",
                 (int)consumer->pipeline_id);
        }
        PG_CATCH();
        {
            SPI_finish();
            PG_RE_THROW();
        }
        PG_END_TRY();
    }
#endif

    MemoryContextSwitchTo(oldcontext);

    return consumer;
}

/* ============================================================
 * Kafka Message Polling
 * ============================================================ */

/*
 * kafka_source_poll
 *    Poll for messages from Kafka topic
 *
 * Returns number of messages retrieved, -1 on error
 */
int kafka_source_poll(void *consumer_ptr, char **messages, int max_messages, int timeout_ms)
{
    KafkaConsumer *consumer = (KafkaConsumer *)consumer_ptr;
    int count = 0;

    if (consumer == NULL || !consumer->is_initialized) {
        elog(WARNING, "Kafka consumer not initialized");
        return -1;
    }

    if (messages == NULL || max_messages <= 0)
        return 0;

#ifdef HAVE_LIBRDKAFKA
    {
        int i;
        int remaining_timeout = timeout_ms;

        for (i = 0; i < max_messages && remaining_timeout > 0; i++) {
            rd_kafka_message_t *msg;
            TimestampTz start_time = GetCurrentTimestamp();

            msg = rd_kafka_consumer_poll(consumer->rk, remaining_timeout);

            if (msg == NULL)
                break;

            if (msg->err) {
                if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                    /* End of partition, not an error */
                    rd_kafka_message_destroy(msg);
                    break;
                } else if (msg->err == RD_KAFKA_RESP_ERR__TIMED_OUT) {
                    rd_kafka_message_destroy(msg);
                    break;
                } else {
                    snprintf(consumer->last_error, sizeof(consumer->last_error),
                             "Kafka poll error: %s", rd_kafka_message_errstr(msg));
                    elog(WARNING, "%s", consumer->last_error);
                    consumer->errors++;
                    consumer->consecutive_errors++;
                    rd_kafka_message_destroy(msg);

                    if (consumer->consecutive_errors > 10)
                        return -1;

                    continue;
                }
            }

            /* Copy message payload with size validation */
            if (msg->payload && msg->len > 0) {
/* Limit maximum message size to 64MB to prevent DoS */
#define KAFKA_MAX_MESSAGE_SIZE (64 * 1024 * 1024)
                if (msg->len > KAFKA_MAX_MESSAGE_SIZE) {
                    elog(WARNING, "Kafka message too large: %zu bytes (max %d)", msg->len,
                         KAFKA_MAX_MESSAGE_SIZE);
                    rd_kafka_message_destroy(msg);
                    continue;
                }

                messages[count] = palloc(msg->len + 1);
                memcpy(messages[count], msg->payload, msg->len);
                messages[count][msg->len] = '\0';
                count++;

                /* Update tracking */
                consumer->current_offset = msg->offset;
                consumer->current_partition = msg->partition;
                consumer->messages_received++;
                consumer->consecutive_errors = 0;
            }

            rd_kafka_message_destroy(msg);

            /* Update remaining timeout */
            remaining_timeout -= (GetCurrentTimestamp() - start_time) / 1000;
        }
    }
#else
    /* Table-based queue fallback: poll messages from orochi_pipeline_queue */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi: failed to connect to SPI for pipeline queue poll");
            return -1;
        }

        PG_TRY();
        {
            char *quoted_topic;
            char query[1024];
            int ret;

            quoted_topic = quote_literal_cstr(consumer->config->topic);

            snprintf(query, sizeof(query),
                     "SELECT message_id, payload FROM orochi.orochi_pipeline_queue "
                     "WHERE pipeline_id = %d AND message_id > "
                     "(SELECT current_offset FROM orochi.orochi_pipeline_queue_cursors "
                     " WHERE pipeline_id = %d AND topic = %s AND partition_id = 0) "
                     "ORDER BY message_id LIMIT %d",
                     (int)consumer->pipeline_id, (int)consumer->pipeline_id, quoted_topic,
                     max_messages);

            ret = SPI_execute(query, true, 0);
            if (ret == SPI_OK_SELECT && SPI_processed > 0) {
                uint64 i;
                for (i = 0; i < SPI_processed; i++) {
                    bool isnull;
                    Datum msg_id_datum;
                    Datum payload_datum;
                    bytea *payload_bytea;
                    int payload_len;
                    char *payload_data;

                    msg_id_datum =
                        SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
                    if (isnull)
                        continue;

                    payload_datum =
                        SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull);
                    if (isnull)
                        continue;

                    payload_bytea = DatumGetByteaPP(payload_datum);
                    payload_len = VARSIZE_ANY_EXHDR(payload_bytea);
                    payload_data = VARDATA_ANY(payload_bytea);

                    messages[count] = palloc(payload_len + 1);
                    memcpy(messages[count], payload_data, payload_len);
                    messages[count][payload_len] = '\0';
                    count++;

                    /* Update tracking with the latest message_id */
                    consumer->current_offset = DatumGetInt64(msg_id_datum);
                    consumer->messages_received++;
                    consumer->consecutive_errors = 0;
                }

                /* Update current_offset in the cursors table */
                if (count > 0) {
                    snprintf(query, sizeof(query),
                             "UPDATE orochi.orochi_pipeline_queue_cursors "
                             "SET current_offset = %ld "
                             "WHERE pipeline_id = %d AND topic = %s AND partition_id = 0",
                             (long)consumer->current_offset, (int)consumer->pipeline_id,
                             quoted_topic);
                    SPI_execute(query, false, 0);
                }
            }

            pfree(quoted_topic);
            SPI_finish();
        }
        PG_CATCH();
        {
            SPI_finish();
            PG_RE_THROW();
        }
        PG_END_TRY();
    }
#endif

    consumer->last_poll_time = GetCurrentTimestamp();

    return count;
}

/* ============================================================
 * Offset Management (for exactly-once semantics)
 * ============================================================ */

/*
 * kafka_source_commit
 *    Commit offset after successful processing
 *
 * This should be called after the data has been successfully
 * committed to PostgreSQL to ensure exactly-once semantics.
 */
bool kafka_source_commit(void *consumer_ptr, int64 offset)
{
    KafkaConsumer *consumer = (KafkaConsumer *)consumer_ptr;

    if (consumer == NULL || !consumer->is_initialized)
        return false;

#ifdef HAVE_LIBRDKAFKA
    {
        rd_kafka_resp_err_t err;
        rd_kafka_topic_partition_list_t *offsets;

        offsets = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(offsets, consumer->config->topic,
                                          consumer->current_partition);
        offsets->elems[0].offset = offset + 1; /* Commit next offset */

        err = rd_kafka_commit(consumer->rk, offsets, 0); /* Synchronous commit */

        rd_kafka_topic_partition_list_destroy(offsets);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            snprintf(consumer->last_error, sizeof(consumer->last_error),
                     "Failed to commit offset %ld: %s", offset, rd_kafka_err2str(err));
            elog(WARNING, "%s", consumer->last_error);
            return false;
        }

        consumer->committed_offset = offset;
        consumer->messages_committed++;
        consumer->last_commit_time = GetCurrentTimestamp();

        elog(DEBUG1, "Committed Kafka offset %ld for partition %d", offset,
             consumer->current_partition);
    }
#else
    /* Table-based queue fallback: commit offset and purge consumed messages */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi: failed to connect to SPI for pipeline queue commit");
            return false;
        }

        PG_TRY();
        {
            char *quoted_topic;
            char query[512];

            quoted_topic = quote_literal_cstr(consumer->config->topic);

            /* Update committed offset */
            snprintf(query, sizeof(query),
                     "UPDATE orochi.orochi_pipeline_queue_cursors "
                     "SET committed_offset = %ld "
                     "WHERE pipeline_id = %d AND topic = %s AND partition_id = 0",
                     (long)offset, (int)consumer->pipeline_id, quoted_topic);
            SPI_execute(query, false, 0);

            /* Purge consumed messages up to the committed offset */
            snprintf(query, sizeof(query),
                     "DELETE FROM orochi.orochi_pipeline_queue "
                     "WHERE pipeline_id = %d AND message_id <= %ld",
                     (int)consumer->pipeline_id, (long)offset);
            SPI_execute(query, false, 0);

            pfree(quoted_topic);
            SPI_finish();

            consumer->committed_offset = offset;
            consumer->messages_committed++;
            consumer->last_commit_time = GetCurrentTimestamp();

            elog(DEBUG1, "Orochi: pipeline %d committed offset %ld (table-based)",
                 (int)consumer->pipeline_id, (long)offset);
        }
        PG_CATCH();
        {
            SPI_finish();
            PG_RE_THROW();
        }
        PG_END_TRY();
    }
#endif

    return true;
}

/*
 * kafka_source_get_offset
 *    Get current consumer offset
 */
int64 kafka_source_get_offset(void *consumer_ptr)
{
    KafkaConsumer *consumer = (KafkaConsumer *)consumer_ptr;

    if (consumer == NULL)
        return -1;

    return consumer->current_offset;
}

/*
 * kafka_source_seek
 *    Seek to a specific offset
 *
 * Used for replay or recovery scenarios.
 */
bool kafka_source_seek(void *consumer_ptr, int64 offset)
{
    KafkaConsumer *consumer = (KafkaConsumer *)consumer_ptr;

    if (consumer == NULL || !consumer->is_initialized)
        return false;

#ifdef HAVE_LIBRDKAFKA
    {
        rd_kafka_error_t *error;
        rd_kafka_topic_partition_list_t *offsets;

        offsets = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(offsets, consumer->config->topic,
                                          consumer->current_partition);
        offsets->elems[0].offset = offset;

        error = rd_kafka_seek_partitions(consumer->rk, offsets, 5000);

        rd_kafka_topic_partition_list_destroy(offsets);

        if (error) {
            snprintf(consumer->last_error, sizeof(consumer->last_error),
                     "Failed to seek to offset %ld: %s", offset, rd_kafka_error_string(error));
            elog(WARNING, "%s", consumer->last_error);
            rd_kafka_error_destroy(error);
            return false;
        }

        consumer->current_offset = offset;
        elog(LOG, "Seeked Kafka consumer to offset %ld", offset);
    }
#else
    /* Table-based queue fallback: update current_offset in cursors table */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi: failed to connect to SPI for pipeline queue seek");
            return false;
        }

        PG_TRY();
        {
            char *quoted_topic;
            char query[512];

            quoted_topic = quote_literal_cstr(consumer->config->topic);

            snprintf(query, sizeof(query),
                     "UPDATE orochi.orochi_pipeline_queue_cursors "
                     "SET current_offset = %ld "
                     "WHERE pipeline_id = %d AND topic = %s AND partition_id = 0",
                     (long)offset, (int)consumer->pipeline_id, quoted_topic);
            SPI_execute(query, false, 0);

            pfree(quoted_topic);
            SPI_finish();

            consumer->current_offset = offset;

            elog(LOG, "Orochi: pipeline %d seeked to offset %ld (table-based)",
                 (int)consumer->pipeline_id, (long)offset);
        }
        PG_CATCH();
        {
            SPI_finish();
            PG_RE_THROW();
        }
        PG_END_TRY();
    }
#endif

    return true;
}

/* ============================================================
 * Kafka Consumer Cleanup
 * ============================================================ */

/*
 * kafka_source_close
 *    Close Kafka consumer and release resources
 */
void kafka_source_close(void *consumer_ptr)
{
    KafkaConsumer *consumer = (KafkaConsumer *)consumer_ptr;

    if (consumer == NULL)
        return;

#ifdef HAVE_LIBRDKAFKA
    if (consumer->rk) {
        /* Unsubscribe and close consumer */
        rd_kafka_consumer_close(consumer->rk);

        if (consumer->topics)
            rd_kafka_topic_partition_list_destroy(consumer->topics);

        rd_kafka_destroy(consumer->rk);

        elog(LOG, "Kafka consumer closed: received=%ld, committed=%ld, errors=%ld",
             consumer->messages_received, consumer->messages_committed, consumer->errors);
    }
#endif

    /* Free memory context */
    if (consumer->context)
        MemoryContextDelete(consumer->context);
}

/* ============================================================
 * Kafka Message Batch Processing
 * ============================================================ */

/* kafka_process_batch is available for external callers via pipeline.c */

/* ============================================================
 * JSON Record Parsing
 * ============================================================ */

/*
 * parse_json_records
 *    Parse JSON data into list of records
 *
 * Handles both single JSON objects and JSON arrays.
 */
List *parse_json_records(const char *data, int64 size)
{
    List *records = NIL;

    if (data == NULL || size <= 0)
        return NIL;

    /* Trim whitespace */
    while (size > 0 && (data[0] == ' ' || data[0] == '\n' || data[0] == '\r' || data[0] == '\t')) {
        data++;
        size--;
    }

    if (size == 0)
        return NIL;

    /* Check if it's an array or single object */
    if (data[0] == '[') {
        /* JSON array - parse each element using parameterized query */
        StringInfoData query;
        int ret;
        Oid argtypes[1] = { JSONBOID };
        Datum argvals[1];
        Jsonb *jb;

        if (SPI_connect() != SPI_OK_CONNECT) {
            elog(WARNING, "Failed to connect to SPI for JSON parsing");
            return NIL;
        }

        /* Convert text to jsonb for safe parameterized query */
        initStringInfo(&query);
        appendStringInfoString(&query, "SELECT value FROM jsonb_array_elements($1)");

        /* Create jsonb datum from text - use DirectFunctionCall for safe conversion
         */
        jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(data)));
        argvals[0] = JsonbPGetDatum(jb);

        ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 0);
        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            uint64 i;
            for (i = 0; i < SPI_processed; i++) {
                char *record_str = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
                if (record_str)
                    records = lappend(records, pstrdup(record_str));
            }
        }

        pfree(query.data);
        SPI_finish();
    } else if (data[0] == '{') {
        /* Single JSON object */
        records = lappend(records, pstrdup(data));
    } else {
        /* Try NDJSON (newline-delimited JSON) */
        const char *line_start = data;
        const char *p = data;
        const char *end = data + size;

        while (p < end) {
            if (*p == '\n' || p == end - 1) {
                int line_len = p - line_start;
                if (p == end - 1 && *p != '\n')
                    line_len++;

                if (line_len > 0) {
                    char *line;
                    char *trimmed;

                    line = palloc(line_len + 1);
                    memcpy(line, line_start, line_len);
                    line[line_len] = '\0';

                    /* Trim and check if valid JSON */
                    trimmed = line;
                    while (*trimmed == ' ' || *trimmed == '\t')
                        trimmed++;

                    if (*trimmed == '{')
                        records = lappend(records, pstrdup(trimmed));

                    pfree(line);
                }
                line_start = p + 1;
            }
            p++;
        }
    }

    return records;
}

/* ============================================================
 * CSV Record Parsing
 * ============================================================ */

/*
 * parse_csv_records
 *    Parse CSV data into list of records
 */
List *parse_csv_records(const char *data, int64 size, char delimiter, bool has_header)
{
    List *records = NIL;
    List *headers = NIL;
    const char *line_start;
    const char *p;
    const char *end;
    bool first_line = true;

    if (data == NULL || size <= 0)
        return NIL;

    line_start = data;
    p = data;
    end = data + size;

    while (p <= end) {
        if (*p == '\n' || *p == '\r' || p == end) {
            int line_len = p - line_start;

            if (line_len > 0) {
                char *line = palloc(line_len + 1);
                memcpy(line, line_start, line_len);
                line[line_len] = '\0';

                if (first_line && has_header) {
                    /* Parse header line */
                    char *token;
                    char *saveptr;
                    char delim_str[2] = { delimiter, '\0' };

                    token = strtok_r(line, delim_str, &saveptr);
                    while (token != NULL) {
                        headers = lappend(headers, pstrdup(token));
                        token = strtok_r(NULL, delim_str, &saveptr);
                    }
                    first_line = false;
                } else {
                    /* Parse data line - convert to JSON */
                    StringInfoData json;
                    char *token;
                    char *saveptr;
                    char delim_str[2] = { delimiter, '\0' };
                    ListCell *header_cell;
                    int col_idx = 0;

                    initStringInfo(&json);
                    appendStringInfoChar(&json, '{');

                    token = strtok_r(line, delim_str, &saveptr);
                    header_cell = list_head(headers);

                    while (token != NULL) {
                        if (col_idx > 0)
                            appendStringInfoString(&json, ", ");

                        if (header_cell != NULL) {
                            appendStringInfo(&json, "\"%s\": ", (char *)lfirst(header_cell));
                            header_cell = lnext(headers, header_cell);
                        } else {
                            appendStringInfo(&json, "\"col%d\": ", col_idx);
                        }

                        /* Check if numeric */
                        {
                            bool is_numeric = true;
                            const char *tp = token;
                            while (*tp) {
                                if (!isdigit(*tp) && *tp != '.' && *tp != '-' && *tp != '+') {
                                    is_numeric = false;
                                    break;
                                }
                                tp++;
                            }

                            if (is_numeric && strlen(token) > 0)
                                appendStringInfo(&json, "%s", token);
                            else
                                appendStringInfo(&json, "\"%s\"", token);
                        }

                        token = strtok_r(NULL, delim_str, &saveptr);
                        col_idx++;
                    }

                    appendStringInfoChar(&json, '}');
                    records = lappend(records, json.data);
                }

                pfree(line);
            }

            /* Skip \r\n sequences */
            if (*p == '\r' && p + 1 < end && *(p + 1) == '\n')
                p++;

            line_start = p + 1;
        }
        p++;
    }

    /* Free headers */
    if (headers != NIL)
        list_free_deep(headers);

    return records;
}

/* ============================================================
 * Line Protocol Parsing (InfluxDB-style)
 * ============================================================ */

/*
 * parse_line_protocol
 *    Parse InfluxDB line protocol data
 *
 * Format: <measurement>,<tag_key>=<tag_value> <field_key>=<field_value>
 * <timestamp>
 */
List *parse_line_protocol(const char *data, int64 size)
{
    List *records = NIL;
    const char *line_start = data;
    const char *p = data;
    const char *end = data + size;

    while (p <= end) {
        if (*p == '\n' || p == end) {
            int line_len = p - line_start;

            if (line_len > 0 && line_start[0] != '#') /* Skip comments */
            {
                StringInfoData json;
                char *line = palloc(line_len + 1);
                char *measurement = NULL;
                char *tags_str = NULL;
                char *fields_str = NULL;
                char *timestamp_str = NULL;
                char *space1, *space2;

                memcpy(line, line_start, line_len);
                line[line_len] = '\0';

                /* Parse line protocol */
                /* Format: measurement,tags fields timestamp */
                space1 = strchr(line, ' ');
                if (space1) {
                    *space1 = '\0';

                    /* Split measurement and tags */
                    {
                        char *comma = strchr(line, ',');
                        if (comma) {
                            *comma = '\0';
                            measurement = line;
                            tags_str = comma + 1;
                        } else {
                            measurement = line;
                        }
                    }

                    /* Split fields and timestamp */
                    fields_str = space1 + 1;
                    space2 = strchr(fields_str, ' ');
                    if (space2) {
                        *space2 = '\0';
                        timestamp_str = space2 + 1;
                    }
                }

                /* Build JSON object */
                initStringInfo(&json);
                appendStringInfoString(&json, "{");

                if (measurement)
                    appendStringInfo(&json, "\"_measurement\": \"%s\"", measurement);

                /* Parse tags */
                if (tags_str && strlen(tags_str) > 0) {
                    char *tag_token;
                    char *tag_saveptr;

                    tag_token = strtok_r(tags_str, ",", &tag_saveptr);
                    while (tag_token) {
                        char *eq = strchr(tag_token, '=');
                        if (eq) {
                            *eq = '\0';
                            appendStringInfo(&json, ", \"%s\": \"%s\"", tag_token, eq + 1);
                        }
                        tag_token = strtok_r(NULL, ",", &tag_saveptr);
                    }
                }

                /* Parse fields */
                if (fields_str && strlen(fields_str) > 0) {
                    char *field_token;
                    char *field_saveptr;

                    field_token = strtok_r(fields_str, ",", &field_saveptr);
                    while (field_token) {
                        char *eq = strchr(field_token, '=');
                        if (eq) {
                            char *value = eq + 1;
                            *eq = '\0';

                            /* Check value type */
                            {
                                int val_len = strlen(value);
                                if (val_len > 0) {
                                    if (value[val_len - 1] == 'i') {
                                        /* Integer */
                                        value[val_len - 1] = '\0';
                                        appendStringInfo(&json, ", \"%s\": %s", field_token, value);
                                    } else if (value[0] == '"' && value[val_len - 1] == '"') {
                                        /* String */
                                        appendStringInfo(&json, ", \"%s\": %s", field_token, value);
                                    } else if (strcmp(value, "true") == 0 ||
                                               strcmp(value, "false") == 0 ||
                                               strcmp(value, "t") == 0 || strcmp(value, "f") == 0) {
                                        /* Boolean */
                                        bool bval = (value[0] == 't');
                                        appendStringInfo(&json, ", \"%s\": %s", field_token,
                                                         bval ? "true" : "false");
                                    } else {
                                        /* Float */
                                        appendStringInfo(&json, ", \"%s\": %s", field_token, value);
                                    }
                                }
                            }
                        }
                        field_token = strtok_r(NULL, ",", &field_saveptr);
                    }
                }

                /* Add timestamp */
                if (timestamp_str && strlen(timestamp_str) > 0) {
                    appendStringInfo(&json, ", \"_time\": %s", timestamp_str);
                }

                appendStringInfoChar(&json, '}');
                records = lappend(records, json.data);

                pfree(line);
            }

            line_start = p + 1;
        }
        p++;
    }

    return records;
}
