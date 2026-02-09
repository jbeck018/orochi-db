/*-------------------------------------------------------------------------
 *
 * cdc_sink.c
 *    Orochi DB CDC sink implementations
 *
 * This module provides sink implementations for delivering CDC events
 * to external systems:
 *   - Kafka sink for Apache Kafka
 *   - Webhook sink for HTTP delivery
 *   - File sink for debugging and local testing
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../orochi.h"
#include "cdc.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Note: This implementation provides wrappers around external libraries.
 * For Kafka: Link against librdkafka
 * For HTTP: Use libcurl
 */

#ifdef HAVE_LIBRDKAFKA
#include <librdkafka/rdkafka.h>
#endif

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

/* ============================================================
 * Kafka Sink Context
 * ============================================================ */

typedef struct KafkaSinkContext {
    CDCKafkaSinkConfig *config;

#ifdef HAVE_LIBRDKAFKA
    rd_kafka_t *producer;
    rd_kafka_topic_t *topic;
#endif

    /* State tracking */
    bool is_initialized;
    int64 messages_sent;
    int64 messages_failed;
    int64 bytes_sent;
    TimestampTz last_send_time;

    /* Error tracking */
    char last_error[CDC_MAX_ERROR_MESSAGE];
    int consecutive_errors;

    /* Memory context */
    MemoryContext context;
} KafkaSinkContext;

/* ============================================================
 * Webhook Sink Context
 * ============================================================ */

typedef struct WebhookSinkContext {
    CDCWebhookSinkConfig *config;

#ifdef HAVE_LIBCURL
    CURL *curl;
    struct curl_slist *headers;
#endif

    /* Batching */
    StringInfo batch_buffer;
    int batch_count;
    TimestampTz batch_start_time;

    /* State tracking */
    bool is_initialized;
    int64 requests_sent;
    int64 requests_failed;
    int64 bytes_sent;
    TimestampTz last_send_time;

    /* Error tracking */
    char last_error[CDC_MAX_ERROR_MESSAGE];
    int consecutive_errors;

    /* Memory context */
    MemoryContext context;
} WebhookSinkContext;

/* ============================================================
 * File Sink Context
 * ============================================================ */

typedef struct FileSinkContext {
    CDCFileSinkConfig *config;

    /* File handling */
    FILE *file;
    char *current_file_path;
    int64 current_file_size;
    int current_file_index;

    /* State tracking */
    bool is_initialized;
    int64 events_written;
    int64 bytes_written;
    TimestampTz last_write_time;

    /* Memory context */
    MemoryContext context;
} FileSinkContext;

/* Forward declarations */
#ifdef HAVE_LIBRDKAFKA
static void kafka_delivery_callback(rd_kafka_t *rk, const rd_kafka_message_t *msg, void *opaque);
#endif
#ifdef HAVE_LIBCURL
static size_t webhook_write_callback(void *contents, size_t size, size_t nmemb, void *userp);
#endif
static bool file_sink_rotate(FileSinkContext *ctx);
static char *file_sink_generate_path(FileSinkContext *ctx, int index);
static bool cdc_webhook_sink_send_internal(WebhookSinkContext *ctx, const char *data, int size);

/* ============================================================
 * Generic Sink Interface
 * ============================================================ */

/*
 * cdc_sink_init
 *    Initialize a sink based on subscription configuration
 */
void *cdc_sink_init(CDCSubscription *sub)
{
    if (sub == NULL)
        return NULL;

    switch (sub->sink_type) {
    case CDC_SINK_KAFKA:
        return cdc_kafka_sink_init(sub->sink_config.kafka);

    case CDC_SINK_WEBHOOK:
        return cdc_webhook_sink_init(sub->sink_config.webhook);

    case CDC_SINK_FILE:
        return cdc_file_sink_init(sub->sink_config.file);

    default:
        elog(ERROR, "Unsupported CDC sink type: %d", sub->sink_type);
        return NULL;
    }
}

/*
 * cdc_sink_close
 *    Close a sink and release resources
 */
void cdc_sink_close(void *sink_context, CDCSinkType sink_type)
{
    if (sink_context == NULL)
        return;

    switch (sink_type) {
    case CDC_SINK_KAFKA:
        cdc_kafka_sink_close(sink_context);
        break;

    case CDC_SINK_WEBHOOK:
        cdc_webhook_sink_close(sink_context);
        break;

    case CDC_SINK_FILE:
        cdc_file_sink_close(sink_context);
        break;

    default:
        elog(WARNING, "Unknown sink type for close: %d", sink_type);
        break;
    }
}

/*
 * cdc_sink_send_event
 *    Send a single event to the sink
 */
bool cdc_sink_send_event(void *sink_context, CDCSinkType sink_type, const char *event_data,
                         int event_size, const char *key, int key_size)
{
    if (sink_context == NULL || event_data == NULL)
        return false;

    switch (sink_type) {
    case CDC_SINK_KAFKA: {
        KafkaSinkContext *ctx = (KafkaSinkContext *)sink_context;
        return cdc_kafka_sink_send(ctx, ctx->config->topic, key, key_size, event_data, event_size);
    }

    case CDC_SINK_WEBHOOK:
        return cdc_webhook_sink_send(sink_context, event_data, event_size);

    case CDC_SINK_FILE:
        return cdc_file_sink_write(sink_context, event_data, event_size);

    default:
        elog(WARNING, "Unknown sink type for send: %d", sink_type);
        return false;
    }
}

/*
 * cdc_sink_send_batch
 *    Send a batch of events to the sink
 */
bool cdc_sink_send_batch(void *sink_context, CDCSinkType sink_type, char **events, int *event_sizes,
                         int count)
{
    int i;
    bool success = true;

    for (i = 0; i < count; i++) {
        if (!cdc_sink_send_event(sink_context, sink_type, events[i], event_sizes[i], NULL, 0)) {
            success = false;
        }
    }

    return success;
}

/*
 * cdc_sink_flush
 *    Flush any buffered events
 */
bool cdc_sink_flush(void *sink_context, CDCSinkType sink_type)
{
    if (sink_context == NULL)
        return false;

    switch (sink_type) {
    case CDC_SINK_KAFKA: {
#ifdef HAVE_LIBRDKAFKA
        KafkaSinkContext *ctx = (KafkaSinkContext *)sink_context;
        if (ctx->producer)
            rd_kafka_flush(ctx->producer, 5000); /* 5 second timeout */
#else
        /* Table writes are already durable, just log */
        elog(DEBUG1, "Orochi CDC: table-based sink flush (no-op, writes already durable)");
#endif
        return true;
    }

    case CDC_SINK_WEBHOOK: {
        WebhookSinkContext *ctx = (WebhookSinkContext *)sink_context;
        if (ctx->batch_count > 0 && ctx->batch_buffer != NULL) {
            /* Send any remaining batched events */
            return cdc_webhook_sink_send(ctx, ctx->batch_buffer->data, ctx->batch_buffer->len);
        }
        return true;
    }

    case CDC_SINK_FILE: {
        FileSinkContext *ctx = (FileSinkContext *)sink_context;
        if (ctx->file)
            fflush(ctx->file);
        return true;
    }

    default:
        return true;
    }
}

/* ============================================================
 * Kafka Sink Implementation
 * ============================================================ */

#ifdef HAVE_LIBRDKAFKA
static void kafka_delivery_callback(rd_kafka_t *rk, const rd_kafka_message_t *msg, void *opaque)
{
    KafkaSinkContext *ctx = (KafkaSinkContext *)opaque;

    if (msg->err) {
        snprintf(ctx->last_error, sizeof(ctx->last_error), "Kafka delivery failed: %s",
                 rd_kafka_err2str(msg->err));
        ctx->messages_failed++;
        ctx->consecutive_errors++;
        elog(WARNING, "%s", ctx->last_error);
    } else {
        ctx->messages_sent++;
        ctx->bytes_sent += msg->len;
        ctx->consecutive_errors = 0;
        ctx->last_send_time = GetCurrentTimestamp();
    }
}
#endif

/*
 * cdc_kafka_sink_init
 *    Initialize Kafka producer for CDC events
 */
void *cdc_kafka_sink_init(CDCKafkaSinkConfig *config)
{
    KafkaSinkContext *ctx;
    MemoryContext oldcontext;
    MemoryContext kafka_context;

    if (config == NULL) {
        elog(ERROR, "Kafka sink configuration is required");
        return NULL;
    }

    if (config->bootstrap_servers == NULL || config->topic == NULL) {
        elog(ERROR, "Kafka bootstrap_servers and topic are required");
        return NULL;
    }

    /* Create memory context */
    kafka_context =
        AllocSetContextCreate(TopMemoryContext, "CDC Kafka Sink", ALLOCSET_DEFAULT_SIZES);

    oldcontext = MemoryContextSwitchTo(kafka_context);

    ctx = palloc0(sizeof(KafkaSinkContext));
    ctx->context = kafka_context;
    ctx->config = config;
    ctx->is_initialized = false;

#ifdef HAVE_LIBRDKAFKA
    {
        char errstr[512];
        rd_kafka_conf_t *conf;

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

        /* Set delivery callback */
        rd_kafka_conf_set_dr_msg_cb(conf, kafka_delivery_callback);
        rd_kafka_conf_set_opaque(conf, ctx);

        /* Configure batching */
        {
            char batch_str[32];
            snprintf(batch_str, sizeof(batch_str), "%d", config->batch_size);
            rd_kafka_conf_set(conf, "batch.num.messages", batch_str, errstr, sizeof(errstr));

            snprintf(batch_str, sizeof(batch_str), "%d", config->linger_ms);
            rd_kafka_conf_set(conf, "linger.ms", batch_str, errstr, sizeof(errstr));
        }

        /* Configure compression */
        if (config->compression_type) {
            rd_kafka_conf_set(conf, "compression.type", config->compression_type, errstr,
                              sizeof(errstr));
        }

        /* Configure acks */
        {
            char acks_str[8];
            snprintf(acks_str, sizeof(acks_str), "%d", config->acks);
            rd_kafka_conf_set(conf, "acks", acks_str, errstr, sizeof(errstr));
        }

        /* Configure retries */
        {
            char retries_str[16];
            snprintf(retries_str, sizeof(retries_str), "%d", config->retries);
            rd_kafka_conf_set(conf, "retries", retries_str, errstr, sizeof(errstr));

            snprintf(retries_str, sizeof(retries_str), "%d", config->retry_backoff_ms);
            rd_kafka_conf_set(conf, "retry.backoff.ms", retries_str, errstr, sizeof(errstr));
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

        /* Create producer - rd_kafka_new takes ownership of conf */
        ctx->producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
        if (!ctx->producer) {
            elog(ERROR, "Failed to create Kafka producer: %s", errstr);
            MemoryContextSwitchTo(oldcontext);
            MemoryContextDelete(kafka_context);
            return NULL;
        }

        /* Create topic handle */
        ctx->topic = rd_kafka_topic_new(ctx->producer, config->topic, NULL);
        if (!ctx->topic) {
            elog(ERROR, "Failed to create Kafka topic handle: %s",
                 rd_kafka_err2str(rd_kafka_last_error()));
            rd_kafka_destroy(ctx->producer);
            MemoryContextSwitchTo(oldcontext);
            MemoryContextDelete(kafka_context);
            return NULL;
        }

        ctx->is_initialized = true;

        elog(LOG, "CDC Kafka sink initialized: brokers=%s, topic=%s", config->bootstrap_servers,
             config->topic);
    }
#else
    /* Table-based events fallback when librdkafka is not available */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT)
        {
            elog(ERROR, "Orochi CDC: failed to connect to SPI for table-based events init");
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        PG_TRY();
        {
            SPI_execute(
                "CREATE TABLE IF NOT EXISTS orochi.orochi_cdc_events ("
                "  event_id BIGSERIAL PRIMARY KEY,"
                "  subscription_id INTEGER,"
                "  topic TEXT NOT NULL,"
                "  message_key TEXT,"
                "  payload BYTEA,"
                "  payload_size INTEGER,"
                "  created_at TIMESTAMPTZ DEFAULT now()"
                ")", false, 0);

            SPI_finish();

            ctx->is_initialized = true;

            elog(LOG, "Orochi CDC: using table-based events (no Kafka)");
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

    return ctx;
}

/*
 * cdc_kafka_sink_send
 *    Send a message to Kafka topic
 */
bool cdc_kafka_sink_send(void *producer, const char *topic, const char *key, int key_size,
                         const char *value, int value_size)
{
    KafkaSinkContext *ctx = (KafkaSinkContext *)producer;

    if (ctx == NULL || !ctx->is_initialized)
        return false;

#ifdef HAVE_LIBRDKAFKA
    {
        int partition =
            ctx->config->partition >= 0 ? ctx->config->partition : RD_KAFKA_PARTITION_UA;
        int err;

        err = rd_kafka_produce(ctx->topic, partition, RD_KAFKA_MSG_F_COPY, (void *)value,
                               value_size, key, key_size, ctx);

        if (err == -1) {
            snprintf(ctx->last_error, sizeof(ctx->last_error), "Failed to produce to Kafka: %s",
                     rd_kafka_err2str(rd_kafka_last_error()));
            elog(WARNING, "%s", ctx->last_error);
            ctx->messages_failed++;
            return false;
        }

        /* Poll for delivery callbacks */
        rd_kafka_poll(ctx->producer, 0);

        return true;
    }
#else
    /* Table-based events fallback: insert event into orochi_cdc_events */
    {
        int spi_ret;
        bool success = false;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT)
        {
            elog(WARNING, "Orochi CDC: failed to connect to SPI for table-based event send");
            return false;
        }

        PG_TRY();
        {
            Oid argtypes[4] = { TEXTOID, TEXTOID, BYTEAOID, INT4OID };
            Datum argvals[4];
            char nulls[4] = { ' ', ' ', ' ', ' ' };
            bytea *payload_bytea;

            /* Build topic datum */
            argvals[0] = CStringGetTextDatum(topic ? topic : ctx->config->topic);

            /* Build key datum (may be NULL) */
            if (key != NULL && key_size > 0)
            {
                char *key_copy = palloc(key_size + 1);
                memcpy(key_copy, key, key_size);
                key_copy[key_size] = '\0';
                argvals[1] = CStringGetTextDatum(key_copy);
            }
            else
            {
                argvals[1] = (Datum)0;
                nulls[1] = 'n';
            }

            /* Build payload datum */
            payload_bytea = (bytea *)palloc(VARHDRSZ + value_size);
            SET_VARSIZE(payload_bytea, VARHDRSZ + value_size);
            memcpy(VARDATA(payload_bytea), value, value_size);
            argvals[2] = PointerGetDatum(payload_bytea);

            /* payload_size */
            argvals[3] = Int32GetDatum(value_size);

            SPI_execute_with_args(
                "INSERT INTO orochi.orochi_cdc_events "
                "(topic, message_key, payload, payload_size) "
                "VALUES ($1, $2, $3, $4)",
                4, argtypes, argvals, nulls, false, 0);

            SPI_finish();

            ctx->messages_sent++;
            ctx->bytes_sent += value_size;
            ctx->last_send_time = GetCurrentTimestamp();
            success = true;
        }
        PG_CATCH();
        {
            SPI_finish();
            ctx->messages_failed++;
            ctx->consecutive_errors++;
            PG_RE_THROW();
        }
        PG_END_TRY();

        return success;
    }
#endif
}

/*
 * cdc_kafka_sink_close
 *    Close Kafka producer and release resources
 */
void cdc_kafka_sink_close(void *producer)
{
    KafkaSinkContext *ctx = (KafkaSinkContext *)producer;

    if (ctx == NULL)
        return;

#ifdef HAVE_LIBRDKAFKA
    if (ctx->producer) {
        /* Wait for outstanding messages */
        rd_kafka_flush(ctx->producer, 10000); /* 10 second timeout */

        if (ctx->topic)
            rd_kafka_topic_destroy(ctx->topic);

        rd_kafka_destroy(ctx->producer);

        elog(LOG, "CDC Kafka sink closed: sent=%ld, failed=%ld, bytes=%ld", ctx->messages_sent,
             ctx->messages_failed, ctx->bytes_sent);
    }
#endif

    if (ctx->context)
        MemoryContextDelete(ctx->context);
}

/* ============================================================
 * Webhook Sink Implementation
 * ============================================================ */

#ifdef HAVE_LIBCURL
static size_t webhook_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    /* Discard response body */
    return size * nmemb;
}
#endif

/*
 * cdc_webhook_sink_init
 *    Initialize HTTP webhook sink
 */
void *cdc_webhook_sink_init(CDCWebhookSinkConfig *config)
{
    WebhookSinkContext *ctx;
    MemoryContext oldcontext;
    MemoryContext webhook_context;

    if (config == NULL) {
        elog(ERROR, "Webhook sink configuration is required");
        return NULL;
    }

    if (config->url == NULL) {
        elog(ERROR, "Webhook URL is required");
        return NULL;
    }

    /* Create memory context */
    webhook_context =
        AllocSetContextCreate(TopMemoryContext, "CDC Webhook Sink", ALLOCSET_DEFAULT_SIZES);

    oldcontext = MemoryContextSwitchTo(webhook_context);

    ctx = palloc0(sizeof(WebhookSinkContext));
    ctx->context = webhook_context;
    ctx->config = config;
    ctx->is_initialized = false;

    /* Initialize batch buffer if batching enabled */
    if (config->batch_mode) {
        ctx->batch_buffer = makeStringInfo();
        appendStringInfoChar(ctx->batch_buffer, '['); /* Start JSON array */
        ctx->batch_count = 0;
        ctx->batch_start_time = GetCurrentTimestamp();
    }

#ifdef HAVE_LIBCURL
    {
        CURLcode res;

        /* Initialize curl */
        ctx->curl = curl_easy_init();
        if (!ctx->curl) {
            elog(ERROR, "Failed to initialize CURL");
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        /* Set URL */
        curl_easy_setopt(ctx->curl, CURLOPT_URL, config->url);

        /* Set HTTP method */
        if (pg_strcasecmp(config->method, "PUT") == 0)
            curl_easy_setopt(ctx->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        else
            curl_easy_setopt(ctx->curl, CURLOPT_POST, 1L);

        /* Set timeout */
        curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT_MS, config->timeout_ms);

        /* Set content type header */
        ctx->headers =
            curl_slist_append(ctx->headers, psprintf("Content-Type: %s", config->content_type));

        /* Add custom headers */
        if (config->headers && config->num_headers > 0) {
            int i;
            for (i = 0; i < config->num_headers; i++) {
                ctx->headers = curl_slist_append(ctx->headers, config->headers[i]);
            }
        }

        /* Set authentication */
        if (config->auth_type && config->auth_credentials) {
            if (pg_strcasecmp(config->auth_type, "basic") == 0) {
                curl_easy_setopt(ctx->curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
                curl_easy_setopt(ctx->curl, CURLOPT_USERPWD, config->auth_credentials);
            } else if (pg_strcasecmp(config->auth_type, "bearer") == 0) {
                ctx->headers = curl_slist_append(
                    ctx->headers, psprintf("Authorization: Bearer %s", config->auth_credentials));
            } else if (pg_strcasecmp(config->auth_type, "api_key") == 0) {
                ctx->headers = curl_slist_append(
                    ctx->headers, psprintf("X-API-Key: %s", config->auth_credentials));
            }
        }

        curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);

        /* Set write callback to discard response */
        curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, webhook_write_callback);

        ctx->is_initialized = true;

        elog(LOG, "CDC Webhook sink initialized: url=%s", config->url);
    }
#else
    /* Table-based webhook queue fallback when libcurl is not available */
    {
        int spi_ret;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT)
        {
            elog(ERROR, "Orochi CDC: failed to connect to SPI for webhook queue init");
            MemoryContextSwitchTo(oldcontext);
            return NULL;
        }

        PG_TRY();
        {
            SPI_execute(
                "CREATE TABLE IF NOT EXISTS orochi.orochi_webhook_queue ("
                "  delivery_id BIGSERIAL PRIMARY KEY,"
                "  subscription_id INTEGER,"
                "  url TEXT NOT NULL,"
                "  method TEXT DEFAULT 'POST',"
                "  content_type TEXT DEFAULT 'application/json',"
                "  headers JSONB,"
                "  payload BYTEA,"
                "  payload_size INTEGER,"
                "  status TEXT DEFAULT 'pending',"
                "  created_at TIMESTAMPTZ DEFAULT now(),"
                "  processed_at TIMESTAMPTZ"
                ")", false, 0);

            SPI_finish();

            ctx->is_initialized = true;

            elog(LOG, "Orochi CDC: using table-based webhook queue (no libcurl)");
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

    return ctx;
}

/*
 * cdc_webhook_sink_send
 *    Send data to webhook endpoint
 */
bool cdc_webhook_sink_send(void *context, const char *data, int size)
{
    WebhookSinkContext *ctx = (WebhookSinkContext *)context;

    if (ctx == NULL || !ctx->is_initialized)
        return false;

    /* Handle batching */
    if (ctx->config->batch_mode && ctx->batch_buffer != NULL) {
        TimestampTz now = GetCurrentTimestamp();

        if (ctx->batch_count > 0)
            appendStringInfoChar(ctx->batch_buffer, ',');

        appendBinaryStringInfo(ctx->batch_buffer, data, size);
        ctx->batch_count++;

        /* Check if batch should be flushed */
        if (ctx->batch_count >= ctx->config->batch_size ||
            (now - ctx->batch_start_time) >= ctx->config->batch_timeout_ms * 1000) {
            /* Close JSON array and send */
            appendStringInfoChar(ctx->batch_buffer, ']');

            bool result = cdc_webhook_sink_send_internal(ctx, ctx->batch_buffer->data,
                                                         ctx->batch_buffer->len);

            /* Reset batch buffer */
            resetStringInfo(ctx->batch_buffer);
            appendStringInfoChar(ctx->batch_buffer, '[');
            ctx->batch_count = 0;
            ctx->batch_start_time = now;

            return result;
        }

        return true;
    }

    return cdc_webhook_sink_send_internal(ctx, data, size);
}

/*
 * Internal function to actually send HTTP request
 */
static bool cdc_webhook_sink_send_internal(WebhookSinkContext *ctx, const char *data, int size)
{
#ifdef HAVE_LIBCURL
    CURLcode res;
    int retry;

    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, (long)size);

    for (retry = 0; retry <= ctx->config->retry_count; retry++) {
        if (retry > 0) {
            elog(DEBUG1, "Webhook retry %d/%d", retry, ctx->config->retry_count);
            pg_usleep(ctx->config->retry_delay_ms * 1000);
        }

        res = curl_easy_perform(ctx->curl);

        if (res == CURLE_OK) {
            long http_code;
            curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code >= 200 && http_code < 300) {
                ctx->requests_sent++;
                ctx->bytes_sent += size;
                ctx->consecutive_errors = 0;
                ctx->last_send_time = GetCurrentTimestamp();
                return true;
            } else {
                snprintf(ctx->last_error, sizeof(ctx->last_error), "Webhook returned HTTP %ld",
                         http_code);
            }
        } else {
            snprintf(ctx->last_error, sizeof(ctx->last_error), "CURL error: %s",
                     curl_easy_strerror(res));
        }
    }

    elog(WARNING, "Webhook send failed after %d retries: %s", ctx->config->retry_count,
         ctx->last_error);
    ctx->requests_failed++;
    ctx->consecutive_errors++;
    return false;
#else
    /* Table-based webhook queue fallback: enqueue webhook delivery */
    {
        int spi_ret;
        bool success = false;

        spi_ret = SPI_connect();
        if (spi_ret != SPI_OK_CONNECT)
        {
            elog(WARNING, "Orochi CDC: failed to connect to SPI for webhook queue send");
            return false;
        }

        PG_TRY();
        {
            Oid argtypes[5] = { TEXTOID, TEXTOID, TEXTOID, BYTEAOID, INT4OID };
            Datum argvals[5];
            bytea *payload_bytea;

            /* URL */
            argvals[0] = CStringGetTextDatum(ctx->config->url);

            /* Method */
            argvals[1] = CStringGetTextDatum(
                ctx->config->method ? ctx->config->method : "POST");

            /* Content-Type */
            argvals[2] = CStringGetTextDatum(
                ctx->config->content_type ? ctx->config->content_type : "application/json");

            /* Payload */
            payload_bytea = (bytea *)palloc(VARHDRSZ + size);
            SET_VARSIZE(payload_bytea, VARHDRSZ + size);
            memcpy(VARDATA(payload_bytea), data, size);
            argvals[3] = PointerGetDatum(payload_bytea);

            /* payload_size */
            argvals[4] = Int32GetDatum(size);

            SPI_execute_with_args(
                "INSERT INTO orochi.orochi_webhook_queue "
                "(url, method, content_type, payload, payload_size) "
                "VALUES ($1, $2, $3, $4, $5)",
                5, argtypes, argvals, NULL, false, 0);

            SPI_finish();

            ctx->requests_sent++;
            ctx->bytes_sent += size;
            ctx->last_send_time = GetCurrentTimestamp();
            ctx->consecutive_errors = 0;
            success = true;
        }
        PG_CATCH();
        {
            SPI_finish();
            ctx->requests_failed++;
            ctx->consecutive_errors++;
            PG_RE_THROW();
        }
        PG_END_TRY();

        return success;
    }
#endif
}

/*
 * cdc_webhook_sink_close
 *    Close webhook sink and release resources
 */
void cdc_webhook_sink_close(void *context)
{
    WebhookSinkContext *ctx = (WebhookSinkContext *)context;

    if (ctx == NULL)
        return;

#ifdef HAVE_LIBCURL
    if (ctx->curl) {
        if (ctx->headers)
            curl_slist_free_all(ctx->headers);

        curl_easy_cleanup(ctx->curl);

        elog(LOG, "CDC Webhook sink closed: sent=%ld, failed=%ld, bytes=%ld", ctx->requests_sent,
             ctx->requests_failed, ctx->bytes_sent);
    }
#endif

    if (ctx->batch_buffer)
        pfree(ctx->batch_buffer->data);

    if (ctx->context)
        MemoryContextDelete(ctx->context);
}

/* ============================================================
 * File Sink Implementation
 * ============================================================ */

/*
 * cdc_file_sink_init
 *    Initialize file sink for CDC events
 */
void *cdc_file_sink_init(CDCFileSinkConfig *config)
{
    FileSinkContext *ctx;
    MemoryContext oldcontext;
    MemoryContext file_context;

    if (config == NULL) {
        elog(ERROR, "File sink configuration is required");
        return NULL;
    }

    if (config->file_path == NULL) {
        elog(ERROR, "File sink path is required");
        return NULL;
    }

    /* Create memory context */
    file_context = AllocSetContextCreate(TopMemoryContext, "CDC File Sink", ALLOCSET_DEFAULT_SIZES);

    oldcontext = MemoryContextSwitchTo(file_context);

    ctx = palloc0(sizeof(FileSinkContext));
    ctx->context = file_context;
    ctx->config = config;
    ctx->current_file_index = 0;

    /* Generate initial file path */
    ctx->current_file_path = file_sink_generate_path(ctx, ctx->current_file_index);

    /* Open file */
    ctx->file = fopen(ctx->current_file_path, config->append_mode ? "a" : "w");
    if (ctx->file == NULL) {
        int saved_errno = errno;
        elog(ERROR, "Failed to open CDC file sink: %s (error: %s)", ctx->current_file_path,
             strerror(saved_errno));
        MemoryContextSwitchTo(oldcontext);
        MemoryContextDelete(file_context);
        return NULL;
    }

    /* Get current file size */
    fseek(ctx->file, 0, SEEK_END);
    ctx->current_file_size = ftell(ctx->file);

    ctx->is_initialized = true;

    elog(LOG, "CDC File sink initialized: path=%s", ctx->current_file_path);

    MemoryContextSwitchTo(oldcontext);

    return ctx;
}

/*
 * cdc_file_sink_write
 *    Write CDC event to file
 */
bool cdc_file_sink_write(void *context, const char *data, int size)
{
    FileSinkContext *ctx = (FileSinkContext *)context;
    size_t written;

    if (ctx == NULL || !ctx->is_initialized || ctx->file == NULL)
        return false;

    /* Check if rotation is needed */
    if (ctx->config->max_file_size > 0 &&
        ctx->current_file_size + size > ctx->config->max_file_size) {
        if (!file_sink_rotate(ctx)) {
            elog(WARNING, "File sink rotation failed");
            return false;
        }
    }

    /* Write data */
    written = fwrite(data, 1, size, ctx->file);
    if (written != size) {
        elog(WARNING, "File sink write failed: wrote %zu of %d bytes", written, size);
        return false;
    }

    /* Add newline for readability */
    fputc('\n', ctx->file);
    written++;

    ctx->current_file_size += written;
    ctx->events_written++;
    ctx->bytes_written += written;
    ctx->last_write_time = GetCurrentTimestamp();

    /* Flush immediately if configured */
    if (ctx->config->flush_immediate)
        fflush(ctx->file);

    return true;
}

/*
 * file_sink_rotate
 *    Rotate log file
 */
static bool file_sink_rotate(FileSinkContext *ctx)
{
    char *new_path;
    int i;

    /* Close current file */
    if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
    }

    /* Remove oldest file if we've hit the limit */
    if (ctx->config->max_files > 0 && ctx->current_file_index >= ctx->config->max_files - 1) {
        char *oldest_path = file_sink_generate_path(ctx, 0);
        unlink(oldest_path);
        pfree(oldest_path);

        /* Rename existing files */
        for (i = 1; i <= ctx->current_file_index; i++) {
            char *old_path = file_sink_generate_path(ctx, i);
            char *new_name = file_sink_generate_path(ctx, i - 1);
            rename(old_path, new_name);
            pfree(old_path);
            pfree(new_name);
        }
    } else {
        ctx->current_file_index++;
    }

    /* Generate new file path */
    new_path = file_sink_generate_path(ctx, ctx->current_file_index);

    if (ctx->current_file_path)
        pfree(ctx->current_file_path);
    ctx->current_file_path = new_path;

    /* Open new file */
    ctx->file = fopen(ctx->current_file_path, "w");
    if (ctx->file == NULL) {
        elog(WARNING, "Failed to open rotated file: %s", ctx->current_file_path);
        return false;
    }

    ctx->current_file_size = 0;

    elog(LOG, "CDC File sink rotated to: %s", ctx->current_file_path);

    return true;
}

/*
 * file_sink_generate_path
 *    Generate file path with index suffix
 */
static char *file_sink_generate_path(FileSinkContext *ctx, int index)
{
    StringInfoData path;
    char *base_path = ctx->config->file_path;
    char *dot_pos;

    initStringInfo(&path);

    /* Find extension */
    dot_pos = strrchr(base_path, '.');

    if (dot_pos && index > 0) {
        /* Insert index before extension */
        appendBinaryStringInfo(&path, base_path, dot_pos - base_path);
        appendStringInfo(&path, ".%d%s", index, dot_pos);
    } else if (index > 0) {
        appendStringInfo(&path, "%s.%d", base_path, index);
    } else {
        appendStringInfoString(&path, base_path);
    }

    return path.data;
}

/*
 * cdc_file_sink_close
 *    Close file sink and release resources
 */
void cdc_file_sink_close(void *context)
{
    FileSinkContext *ctx = (FileSinkContext *)context;

    if (ctx == NULL)
        return;

    if (ctx->file) {
        fflush(ctx->file);
        fclose(ctx->file);

        elog(LOG, "CDC File sink closed: events=%ld, bytes=%ld, path=%s", ctx->events_written,
             ctx->bytes_written, ctx->current_file_path);
    }

    if (ctx->current_file_path)
        pfree(ctx->current_file_path);

    if (ctx->context)
        MemoryContextDelete(ctx->context);
}
