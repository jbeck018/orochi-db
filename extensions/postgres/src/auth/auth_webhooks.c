/*-------------------------------------------------------------------------
 *
 * auth_webhooks.c
 *    Orochi DB Auth webhook event system
 *
 * This module provides webhook notifications for authentication events:
 *   - user.created - When a new user is created
 *   - user.updated - When user profile is updated
 *   - user.deleted - When a user is deleted
 *   - user.signed_in - When a user signs in
 *   - user.signed_out - When a user signs out
 *
 * Features:
 *   - HMAC-SHA256 signature verification
 *   - Async event delivery with retries
 *   - Event filtering and subscription management
 *   - Audit logging of all webhook deliveries
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postgres.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>

#include "gotrue.h"

/* ============================================================
 * Webhook Constants and Configuration
 * ============================================================ */

#define AUTH_WEBHOOK_MAX_ENDPOINTS     16
#define AUTH_WEBHOOK_MAX_URL_LENGTH    2048
#define AUTH_WEBHOOK_MAX_SECRET_LENGTH 256
#define AUTH_WEBHOOK_MAX_RETRIES       3
#define AUTH_WEBHOOK_RETRY_DELAY_MS    1000
#define AUTH_WEBHOOK_TIMEOUT_MS        30000
#define AUTH_WEBHOOK_MAX_PAYLOAD_SIZE  (1024 * 1024) /* 1MB */

/* Webhook event types */
#define AUTH_WEBHOOK_EVENT_USER_CREATED    "user.created"
#define AUTH_WEBHOOK_EVENT_USER_UPDATED    "user.updated"
#define AUTH_WEBHOOK_EVENT_USER_DELETED    "user.deleted"
#define AUTH_WEBHOOK_EVENT_USER_SIGNED_IN  "user.signed_in"
#define AUTH_WEBHOOK_EVENT_USER_SIGNED_OUT "user.signed_out"

/* ============================================================
 * Webhook Data Structures
 * ============================================================ */

/*
 * AuthWebhookEvent - Webhook event types enum
 */
typedef enum AuthWebhookEventType {
    AUTH_WEBHOOK_USER_CREATED = 0,
    AUTH_WEBHOOK_USER_UPDATED,
    AUTH_WEBHOOK_USER_DELETED,
    AUTH_WEBHOOK_USER_SIGNED_IN,
    AUTH_WEBHOOK_USER_SIGNED_OUT
} AuthWebhookEventType;

/*
 * AuthWebhookEndpoint - Webhook endpoint configuration
 */
typedef struct AuthWebhookEndpoint {
    int64 endpoint_id;
    char *url;
    char *secret;   /* HMAC secret */
    bool events[5]; /* Which events to receive */
    int timeout_ms;
    int max_retries;
    bool enabled;
    TimestampTz created_at;
    TimestampTz updated_at;
} AuthWebhookEndpoint;

/*
 * AuthWebhookPayload - Webhook payload structure
 */
typedef struct AuthWebhookPayload {
    pg_uuid_t event_id; /* Unique event ID */
    AuthWebhookEventType event_type;
    TimestampTz timestamp;
    pg_uuid_t user_id;
    Jsonb *old_record; /* Previous state (for updates) */
    Jsonb *record;     /* Current state */
    Jsonb *claims;     /* JWT claims context */
    char *schema;      /* Always "auth" */
    char *table;       /* "users" */
} AuthWebhookPayload;

/*
 * AuthWebhookDelivery - Delivery attempt record
 */
typedef struct AuthWebhookDelivery {
    pg_uuid_t delivery_id;
    int64 endpoint_id;
    pg_uuid_t event_id;
    int attempt_number;
    int http_status;
    char *response_body;
    bool success;
    TimestampTz attempted_at;
    int duration_ms;
    char *error_message;
} AuthWebhookDelivery;

/*
 * CurlWriteData - Helper for CURL response
 */
typedef struct CurlWriteData {
    char *data;
    size_t size;
    size_t capacity;
} CurlWriteData;

/* Background worker state */
static volatile sig_atomic_t webhook_got_sigterm = false;
static volatile sig_atomic_t webhook_got_sighup = false;

/* Forward declarations */
static char *compute_hmac_signature(const char *payload, const char *secret, TimestampTz timestamp);
static bool deliver_webhook(AuthWebhookEndpoint *endpoint, AuthWebhookPayload *payload,
                            AuthWebhookDelivery *delivery);
static size_t curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp);
static void log_delivery_attempt(AuthWebhookDelivery *delivery);
static const char *webhook_event_type_name(AuthWebhookEventType type);
static AuthWebhookEventType parse_webhook_event_type(const char *name);

/* ============================================================
 * HMAC-SHA256 Signature Generation
 * ============================================================ */

/*
 * compute_hmac_signature
 *    Compute HMAC-SHA256 signature for webhook payload
 *
 * The signature format follows Supabase/Stripe conventions:
 *   signature = HMAC-SHA256(timestamp.payload, secret)
 *
 * Returns: hex-encoded signature string
 */
static char *compute_hmac_signature(const char *payload, const char *secret, TimestampTz timestamp)
{
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;
    char *signature;
    char *signing_input;
    int64 unix_timestamp;
    int i;

    if (payload == NULL || secret == NULL)
        return NULL;

    /* Convert PostgreSQL timestamp to Unix timestamp */
    unix_timestamp =
        (timestamp / USECS_PER_SEC) + ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

    /* Build signing input: "timestamp.payload" */
    signing_input = psprintf("%ld.%s", unix_timestamp, payload);

    /* Compute HMAC-SHA256 */
    if (!HMAC(EVP_sha256(), secret, strlen(secret), (unsigned char *)signing_input,
              strlen(signing_input), hmac_result, &hmac_len)) {
        pfree(signing_input);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to compute HMAC signature")));
    }

    pfree(signing_input);

    /* Convert to hex string */
    signature = palloc(hmac_len * 2 + 1);
    for (i = 0; i < hmac_len; i++) {
        snprintf(signature + i * 2, 3, "%02x", hmac_result[i]);
    }
    signature[hmac_len * 2] = '\0';

    return signature;
}

/*
 * orochi_auth_verify_webhook_signature
 *    Verify an incoming webhook signature
 *
 * Used when Orochi receives webhooks from external services.
 */
bool orochi_auth_verify_webhook_signature(const char *payload, const char *signature,
                                          const char *secret, int64 timestamp)
{
    char *expected_signature;
    bool valid;
    TimestampTz pg_timestamp;

    if (payload == NULL || signature == NULL || secret == NULL)
        return false;

    /* Convert Unix timestamp to PostgreSQL timestamp */
    pg_timestamp =
        (timestamp - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) * USECS_PER_SEC;

    expected_signature = compute_hmac_signature(payload, secret, pg_timestamp);

    /* Use constant-time comparison to prevent timing attacks */
    valid = (strlen(expected_signature) == strlen(signature)) &&
            (CRYPTO_memcmp(expected_signature, signature, strlen(signature)) == 0);

    pfree(expected_signature);

    return valid;
}

/* ============================================================
 * Webhook Event Creation
 * ============================================================ */

/*
 * orochi_auth_create_webhook_event
 *    Create a webhook event payload for a user action
 */
AuthWebhookPayload *orochi_auth_create_webhook_event(AuthWebhookEventType event_type,
                                                     OrochiAuthUser *user, OrochiAuthUser *old_user)
{
    AuthWebhookPayload *payload;
    StringInfoData record_json;
    StringInfoData old_record_json;
    char uuid_str[37];
    unsigned char random_bytes[16];

    payload = palloc0(sizeof(AuthWebhookPayload));
    payload->event_type = event_type;
    payload->timestamp = GetCurrentTimestamp();
    payload->schema = pstrdup("auth");
    payload->table = pstrdup("users");

    /* Generate unique event ID */
    RAND_bytes(random_bytes, 16);
    memcpy(&payload->event_id, random_bytes, sizeof(pg_uuid_t));

    /* Copy user ID */
    if (user)
        memcpy(&payload->user_id, &user->id, sizeof(pg_uuid_t));

    /* Build current record JSON */
    if (user) {
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
                 user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
                 user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
                 user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

        initStringInfo(&record_json);
        appendStringInfo(&record_json, "{\"id\": \"%s\"", uuid_str);

        if (user->email)
            appendStringInfo(&record_json, ", \"email\": \"%s\"", user->email);
        else
            appendStringInfoString(&record_json, ", \"email\": null");

        if (user->phone)
            appendStringInfo(&record_json, ", \"phone\": \"%s\"", user->phone);
        else
            appendStringInfoString(&record_json, ", \"phone\": null");

        appendStringInfo(&record_json, ", \"role\": \"%s\"",
                         user->role ? user->role : "authenticated");

        appendStringInfo(&record_json, ", \"is_anonymous\": %s",
                         user->is_anonymous ? "true" : "false");

        appendStringInfo(&record_json, ", \"email_confirmed_at\": %s",
                         user->email_confirmed_at > 0
                             ? psprintf("\"%s\"", timestamptz_to_str(user->email_confirmed_at))
                             : "null");

        appendStringInfo(&record_json, ", \"phone_confirmed_at\": %s",
                         user->phone_confirmed_at > 0
                             ? psprintf("\"%s\"", timestamptz_to_str(user->phone_confirmed_at))
                             : "null");

        appendStringInfo(&record_json, ", \"created_at\": \"%s\"",
                         timestamptz_to_str(user->created_at));

        appendStringInfo(&record_json, ", \"updated_at\": \"%s\"",
                         timestamptz_to_str(user->updated_at));

        appendStringInfoChar(&record_json, '}');

        payload->record =
            DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(record_json.data)));
        pfree(record_json.data);
    }

    /* Build old record JSON for updates */
    if (old_user && event_type == AUTH_WEBHOOK_USER_UPDATED) {
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 old_user->id.data[0], old_user->id.data[1], old_user->id.data[2],
                 old_user->id.data[3], old_user->id.data[4], old_user->id.data[5],
                 old_user->id.data[6], old_user->id.data[7], old_user->id.data[8],
                 old_user->id.data[9], old_user->id.data[10], old_user->id.data[11],
                 old_user->id.data[12], old_user->id.data[13], old_user->id.data[14],
                 old_user->id.data[15]);

        initStringInfo(&old_record_json);
        appendStringInfo(&old_record_json, "{\"id\": \"%s\"", uuid_str);

        if (old_user->email)
            appendStringInfo(&old_record_json, ", \"email\": \"%s\"", old_user->email);
        else
            appendStringInfoString(&old_record_json, ", \"email\": null");

        if (old_user->phone)
            appendStringInfo(&old_record_json, ", \"phone\": \"%s\"", old_user->phone);
        else
            appendStringInfoString(&old_record_json, ", \"phone\": null");

        appendStringInfo(&old_record_json, ", \"role\": \"%s\"",
                         old_user->role ? old_user->role : "authenticated");

        appendStringInfo(&old_record_json, ", \"is_anonymous\": %s",
                         old_user->is_anonymous ? "true" : "false");

        appendStringInfoChar(&old_record_json, '}');

        payload->old_record =
            DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(old_record_json.data)));
        pfree(old_record_json.data);
    }

    return payload;
}

/*
 * orochi_auth_serialize_webhook_payload
 *    Serialize a webhook payload to JSON string
 */
char *orochi_auth_serialize_webhook_payload(AuthWebhookPayload *payload)
{
    StringInfoData json;
    StringInfoData record_str;
    StringInfoData old_record_str;
    char event_uuid_str[37];
    char user_uuid_str[37];

    if (payload == NULL)
        return NULL;

    snprintf(event_uuid_str, sizeof(event_uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             payload->event_id.data[0], payload->event_id.data[1], payload->event_id.data[2],
             payload->event_id.data[3], payload->event_id.data[4], payload->event_id.data[5],
             payload->event_id.data[6], payload->event_id.data[7], payload->event_id.data[8],
             payload->event_id.data[9], payload->event_id.data[10], payload->event_id.data[11],
             payload->event_id.data[12], payload->event_id.data[13], payload->event_id.data[14],
             payload->event_id.data[15]);

    snprintf(user_uuid_str, sizeof(user_uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             payload->user_id.data[0], payload->user_id.data[1], payload->user_id.data[2],
             payload->user_id.data[3], payload->user_id.data[4], payload->user_id.data[5],
             payload->user_id.data[6], payload->user_id.data[7], payload->user_id.data[8],
             payload->user_id.data[9], payload->user_id.data[10], payload->user_id.data[11],
             payload->user_id.data[12], payload->user_id.data[13], payload->user_id.data[14],
             payload->user_id.data[15]);

    initStringInfo(&json);
    appendStringInfoChar(&json, '{');

    /* Event metadata */
    appendStringInfo(&json, "\"type\": \"%s\"", webhook_event_type_name(payload->event_type));
    appendStringInfo(&json, ", \"event_id\": \"%s\"", event_uuid_str);
    appendStringInfo(&json, ", \"user_id\": \"%s\"", user_uuid_str);
    appendStringInfo(&json, ", \"timestamp\": \"%s\"", timestamptz_to_str(payload->timestamp));

    /* Schema and table */
    appendStringInfo(&json, ", \"schema\": \"%s\"", payload->schema ? payload->schema : "auth");
    appendStringInfo(&json, ", \"table\": \"%s\"", payload->table ? payload->table : "users");

    /* Record data */
    if (payload->record) {
        initStringInfo(&record_str);
        (void)JsonbToCString(&record_str, &payload->record->root, VARSIZE(payload->record));
        appendStringInfo(&json, ", \"record\": %s", record_str.data);
        pfree(record_str.data);
    } else {
        appendStringInfoString(&json, ", \"record\": null");
    }

    /* Old record (for updates) */
    if (payload->old_record) {
        initStringInfo(&old_record_str);
        (void)JsonbToCString(&old_record_str, &payload->old_record->root,
                             VARSIZE(payload->old_record));
        appendStringInfo(&json, ", \"old_record\": %s", old_record_str.data);
        pfree(old_record_str.data);
    } else {
        appendStringInfoString(&json, ", \"old_record\": null");
    }

    appendStringInfoChar(&json, '}');

    return json.data;
}

/* ============================================================
 * Webhook Delivery
 * ============================================================ */

/*
 * curl_write_callback
 *    Callback for CURL to write response data
 */
static size_t curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    CurlWriteData *data = (CurlWriteData *)userp;

    if (data->size + realsize >= data->capacity) {
        size_t new_capacity = data->capacity * 2;
        if (new_capacity < data->size + realsize + 1)
            new_capacity = data->size + realsize + 1;

        data->data = repalloc(data->data, new_capacity);
        data->capacity = new_capacity;
    }

    memcpy(data->data + data->size, contents, realsize);
    data->size += realsize;
    data->data[data->size] = '\0';

    return realsize;
}

/*
 * deliver_webhook
 *    Deliver a webhook to an endpoint using CURL
 */
static bool deliver_webhook(AuthWebhookEndpoint *endpoint, AuthWebhookPayload *payload,
                            AuthWebhookDelivery *delivery)
{
#ifdef HAVE_LIBCURL
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlWriteData response_data;
    char *payload_json;
    char *signature;
    char signature_header[512];
    char timestamp_header[64];
    int64 unix_timestamp;
    long http_status;
    double total_time;

    if (endpoint == NULL || payload == NULL)
        return false;

    /* Serialize payload */
    payload_json = orochi_auth_serialize_webhook_payload(payload);
    if (payload_json == NULL)
        return false;

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(payload_json);
        return false;
    }

    /* Calculate Unix timestamp */
    unix_timestamp = (payload->timestamp / USECS_PER_SEC) +
                     ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

    /* Compute HMAC signature */
    signature = compute_hmac_signature(payload_json, endpoint->secret, payload->timestamp);

    /* Build headers */
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Orochi-Auth-Webhook/1.0");

    snprintf(signature_header, sizeof(signature_header), "X-Webhook-Signature: sha256=%s",
             signature);
    headers = curl_slist_append(headers, signature_header);

    snprintf(timestamp_header, sizeof(timestamp_header), "X-Webhook-Timestamp: %ld",
             unix_timestamp);
    headers = curl_slist_append(headers, timestamp_header);

    /* Initialize response buffer */
    response_data.data = palloc(1024);
    response_data.size = 0;
    response_data.capacity = 1024;

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, endpoint->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, endpoint->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    /* Perform request */
    res = curl_easy_perform(curl);

    /* Get timing info */
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    /* Populate delivery record */
    if (delivery) {
        delivery->http_status = (int)http_status;
        delivery->duration_ms = (int)(total_time * 1000);
        delivery->attempted_at = GetCurrentTimestamp();

        if (res == CURLE_OK) {
            delivery->success = (http_status >= 200 && http_status < 300);
            delivery->response_body = pstrdup(response_data.data);
        } else {
            delivery->success = false;
            delivery->error_message = pstrdup(curl_easy_strerror(res));
        }
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(signature);
    pfree(payload_json);
    pfree(response_data.data);

    return (res == CURLE_OK && http_status >= 200 && http_status < 300);
#else
    /* Stub without libcurl */
    elog(WARNING, "Webhook delivery requires libcurl (not compiled in)");
    if (delivery) {
        delivery->success = false;
        delivery->error_message = pstrdup("libcurl not available");
        delivery->attempted_at = GetCurrentTimestamp();
    }
    return false;
#endif
}

/*
 * orochi_auth_deliver_webhook_event
 *    Deliver a webhook event to all configured endpoints
 */
bool orochi_auth_deliver_webhook_event(AuthWebhookPayload *payload)
{
    StringInfoData query;
    int ret;
    int i;
    bool any_success = false;

    if (payload == NULL)
        return false;

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    /* Get all enabled webhook endpoints */
    initStringInfo(&query);
    appendStringInfoString(&query, "SELECT endpoint_id, url, secret, timeout_ms, max_retries, "
                                   "user_created, user_updated, user_deleted, user_signed_in, "
                                   "user_signed_out "
                                   "FROM auth.webhook_endpoints WHERE enabled = TRUE");

    ret = SPI_execute(query.data, true, AUTH_WEBHOOK_MAX_ENDPOINTS);
    pfree(query.data);

    if (ret != SPI_OK_SELECT || SPI_processed == 0) {
        SPI_finish();
        return true; /* No endpoints configured is not an error */
    }

    /* Deliver to each endpoint */
    for (i = 0; i < SPI_processed; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        AuthWebhookEndpoint endpoint;
        AuthWebhookDelivery delivery;
        bool isnull;
        int event_idx;
        int attempt;
        bool delivered = false;

        memset(&endpoint, 0, sizeof(AuthWebhookEndpoint));
        memset(&delivery, 0, sizeof(AuthWebhookDelivery));

        endpoint.endpoint_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
        endpoint.url = SPI_getvalue(tuple, tupdesc, 2);
        endpoint.secret = SPI_getvalue(tuple, tupdesc, 3);
        endpoint.timeout_ms = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        if (isnull)
            endpoint.timeout_ms = AUTH_WEBHOOK_TIMEOUT_MS;
        endpoint.max_retries = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        if (isnull)
            endpoint.max_retries = AUTH_WEBHOOK_MAX_RETRIES;

        /* Check event filter */
        endpoint.events[AUTH_WEBHOOK_USER_CREATED] =
            DatumGetBool(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        endpoint.events[AUTH_WEBHOOK_USER_UPDATED] =
            DatumGetBool(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        endpoint.events[AUTH_WEBHOOK_USER_DELETED] =
            DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));
        endpoint.events[AUTH_WEBHOOK_USER_SIGNED_IN] =
            DatumGetBool(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        endpoint.events[AUTH_WEBHOOK_USER_SIGNED_OUT] =
            DatumGetBool(SPI_getbinval(tuple, tupdesc, 10, &isnull));

        /* Skip if endpoint doesn't want this event type */
        event_idx = (int)payload->event_type;
        if (event_idx >= 0 && event_idx < 5 && !endpoint.events[event_idx])
            continue;

        /* Attempt delivery with retries */
        delivery.endpoint_id = endpoint.endpoint_id;
        memcpy(&delivery.event_id, &payload->event_id, sizeof(pg_uuid_t));

        for (attempt = 1; attempt <= endpoint.max_retries && !delivered; attempt++) {
            delivery.attempt_number = attempt;

            delivered = deliver_webhook(&endpoint, payload, &delivery);

            if (!delivered && attempt < endpoint.max_retries) {
                pg_usleep(AUTH_WEBHOOK_RETRY_DELAY_MS * 1000 * attempt);
            }

            /* Log the attempt */
            log_delivery_attempt(&delivery);
        }

        if (delivered)
            any_success = true;
    }

    SPI_finish();

    return any_success;
}

/*
 * log_delivery_attempt
 *    Log a webhook delivery attempt to the database
 */
static void log_delivery_attempt(AuthWebhookDelivery *delivery)
{
    StringInfoData query;
    char uuid_str[37];

    if (delivery == NULL)
        return;

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             delivery->event_id.data[0], delivery->event_id.data[1], delivery->event_id.data[2],
             delivery->event_id.data[3], delivery->event_id.data[4], delivery->event_id.data[5],
             delivery->event_id.data[6], delivery->event_id.data[7], delivery->event_id.data[8],
             delivery->event_id.data[9], delivery->event_id.data[10], delivery->event_id.data[11],
             delivery->event_id.data[12], delivery->event_id.data[13], delivery->event_id.data[14],
             delivery->event_id.data[15]);

    initStringInfo(&query);
    appendStringInfo(
        &query,
        "INSERT INTO auth.webhook_deliveries "
        "(endpoint_id, event_id, attempt_number, http_status, success, "
        "duration_ms, error_message, attempted_at) "
        "VALUES (%ld, '%s', %d, %d, %s, %d, %s, NOW())",
        delivery->endpoint_id, uuid_str, delivery->attempt_number, delivery->http_status,
        delivery->success ? "TRUE" : "FALSE", delivery->duration_ms,
        delivery->error_message ? quote_literal_cstr(delivery->error_message) : "NULL");

    /* Need to connect to SPI if not already connected */
    if (SPI_connect() == SPI_OK_CONNECT) {
        SPI_execute(query.data, false, 0);
        SPI_finish();
    }

    pfree(query.data);
}

/* ============================================================
 * Webhook Endpoint Management
 * ============================================================ */

/*
 * orochi_auth_create_webhook_endpoint
 *    Create a new webhook endpoint
 */
int64 orochi_auth_create_webhook_endpoint(const char *url, const char *secret, bool *events,
                                          int timeout_ms, int max_retries)
{
    StringInfoData query;
    int64 endpoint_id = 0;
    int ret;

    if (url == NULL || strlen(url) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("webhook URL is required")));

    if (strlen(url) > AUTH_WEBHOOK_MAX_URL_LENGTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("webhook URL too long (max %d characters)", AUTH_WEBHOOK_MAX_URL_LENGTH)));

    if (secret == NULL || strlen(secret) == 0) {
        /* Generate a random secret */
        secret = orochi_auth_generate_token(32);
    }

    if (timeout_ms <= 0)
        timeout_ms = AUTH_WEBHOOK_TIMEOUT_MS;

    if (max_retries <= 0)
        max_retries = AUTH_WEBHOOK_MAX_RETRIES;

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to connect to SPI")));

    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.webhook_endpoints "
                     "(url, secret, timeout_ms, max_retries, enabled, "
                     "user_created, user_updated, user_deleted, user_signed_in, "
                     "user_signed_out, "
                     "created_at, updated_at) "
                     "VALUES (%s, %s, %d, %d, TRUE, %s, %s, %s, %s, %s, NOW(), NOW()) "
                     "RETURNING endpoint_id",
                     quote_literal_cstr(url), quote_literal_cstr(secret), timeout_ms, max_retries,
                     events ? (events[0] ? "TRUE" : "FALSE") : "TRUE",
                     events ? (events[1] ? "TRUE" : "FALSE") : "TRUE",
                     events ? (events[2] ? "TRUE" : "FALSE") : "TRUE",
                     events ? (events[3] ? "TRUE" : "FALSE") : "TRUE",
                     events ? (events[4] ? "TRUE" : "FALSE") : "TRUE");

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
        bool isnull;
        endpoint_id =
            DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    elog(NOTICE, "Created webhook endpoint %ld: %s", endpoint_id, url);

    return endpoint_id;
}

/*
 * orochi_auth_delete_webhook_endpoint
 *    Delete a webhook endpoint
 */
bool orochi_auth_delete_webhook_endpoint(int64 endpoint_id)
{
    StringInfoData query;
    int ret;
    bool deleted = false;

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);
    appendStringInfo(&query, "DELETE FROM auth.webhook_endpoints WHERE endpoint_id = %ld",
                     endpoint_id);

    ret = SPI_execute(query.data, false, 0);
    deleted = (ret == SPI_OK_DELETE && SPI_processed > 0);

    pfree(query.data);
    SPI_finish();

    return deleted;
}

/*
 * orochi_auth_enable_webhook_endpoint
 *    Enable or disable a webhook endpoint
 */
bool orochi_auth_enable_webhook_endpoint(int64 endpoint_id, bool enabled)
{
    StringInfoData query;
    int ret;
    bool updated = false;

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webhook_endpoints SET enabled = %s, updated_at = NOW() "
                     "WHERE endpoint_id = %ld",
                     enabled ? "TRUE" : "FALSE", endpoint_id);

    ret = SPI_execute(query.data, false, 0);
    updated = (ret == SPI_OK_UPDATE && SPI_processed > 0);

    pfree(query.data);
    SPI_finish();

    return updated;
}

/* ============================================================
 * Event Emission Helpers
 * ============================================================ */

/*
 * orochi_auth_emit_user_created
 *    Emit a user.created webhook event
 */
void orochi_auth_emit_user_created(OrochiAuthUser *user)
{
    AuthWebhookPayload *payload;

    if (user == NULL)
        return;

    payload = orochi_auth_create_webhook_event(AUTH_WEBHOOK_USER_CREATED, user, NULL);
    orochi_auth_deliver_webhook_event(payload);
}

/*
 * orochi_auth_emit_user_updated
 *    Emit a user.updated webhook event
 */
void orochi_auth_emit_user_updated(OrochiAuthUser *user, OrochiAuthUser *old_user)
{
    AuthWebhookPayload *payload;

    if (user == NULL)
        return;

    payload = orochi_auth_create_webhook_event(AUTH_WEBHOOK_USER_UPDATED, user, old_user);
    orochi_auth_deliver_webhook_event(payload);
}

/*
 * orochi_auth_emit_user_deleted
 *    Emit a user.deleted webhook event
 */
void orochi_auth_emit_user_deleted(OrochiAuthUser *user)
{
    AuthWebhookPayload *payload;

    if (user == NULL)
        return;

    payload = orochi_auth_create_webhook_event(AUTH_WEBHOOK_USER_DELETED, user, NULL);
    orochi_auth_deliver_webhook_event(payload);
}

/*
 * orochi_auth_emit_user_signed_in
 *    Emit a user.signed_in webhook event
 */
void orochi_auth_emit_user_signed_in(OrochiAuthUser *user)
{
    AuthWebhookPayload *payload;

    if (user == NULL)
        return;

    payload = orochi_auth_create_webhook_event(AUTH_WEBHOOK_USER_SIGNED_IN, user, NULL);
    orochi_auth_deliver_webhook_event(payload);
}

/*
 * orochi_auth_emit_user_signed_out
 *    Emit a user.signed_out webhook event
 */
void orochi_auth_emit_user_signed_out(OrochiAuthUser *user)
{
    AuthWebhookPayload *payload;

    if (user == NULL)
        return;

    payload = orochi_auth_create_webhook_event(AUTH_WEBHOOK_USER_SIGNED_OUT, user, NULL);
    orochi_auth_deliver_webhook_event(payload);
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

static const char *webhook_event_type_name(AuthWebhookEventType type)
{
    switch (type) {
    case AUTH_WEBHOOK_USER_CREATED:
        return AUTH_WEBHOOK_EVENT_USER_CREATED;
    case AUTH_WEBHOOK_USER_UPDATED:
        return AUTH_WEBHOOK_EVENT_USER_UPDATED;
    case AUTH_WEBHOOK_USER_DELETED:
        return AUTH_WEBHOOK_EVENT_USER_DELETED;
    case AUTH_WEBHOOK_USER_SIGNED_IN:
        return AUTH_WEBHOOK_EVENT_USER_SIGNED_IN;
    case AUTH_WEBHOOK_USER_SIGNED_OUT:
        return AUTH_WEBHOOK_EVENT_USER_SIGNED_OUT;
    default:
        return "unknown";
    }
}

static AuthWebhookEventType parse_webhook_event_type(const char *name)
{
    if (strcmp(name, AUTH_WEBHOOK_EVENT_USER_CREATED) == 0)
        return AUTH_WEBHOOK_USER_CREATED;
    if (strcmp(name, AUTH_WEBHOOK_EVENT_USER_UPDATED) == 0)
        return AUTH_WEBHOOK_USER_UPDATED;
    if (strcmp(name, AUTH_WEBHOOK_EVENT_USER_DELETED) == 0)
        return AUTH_WEBHOOK_USER_DELETED;
    if (strcmp(name, AUTH_WEBHOOK_EVENT_USER_SIGNED_IN) == 0)
        return AUTH_WEBHOOK_USER_SIGNED_IN;
    if (strcmp(name, AUTH_WEBHOOK_EVENT_USER_SIGNED_OUT) == 0)
        return AUTH_WEBHOOK_USER_SIGNED_OUT;

    return AUTH_WEBHOOK_USER_CREATED;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_create_webhook_endpoint_sql);
PG_FUNCTION_INFO_V1(orochi_auth_delete_webhook_endpoint_sql);
PG_FUNCTION_INFO_V1(orochi_auth_verify_webhook_signature_sql);
PG_FUNCTION_INFO_V1(orochi_auth_list_webhook_endpoints_sql);

Datum orochi_auth_create_webhook_endpoint_sql(PG_FUNCTION_ARGS)
{
    text *url_text = PG_GETARG_TEXT_PP(0);
    text *secret_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);
    int timeout_ms = PG_ARGISNULL(2) ? AUTH_WEBHOOK_TIMEOUT_MS : PG_GETARG_INT32(2);
    int max_retries = PG_ARGISNULL(3) ? AUTH_WEBHOOK_MAX_RETRIES : PG_GETARG_INT32(3);

    char *url = text_to_cstring(url_text);
    char *secret = secret_text ? text_to_cstring(secret_text) : NULL;

    int64 endpoint_id =
        orochi_auth_create_webhook_endpoint(url, secret, NULL, timeout_ms, max_retries);

    PG_RETURN_INT64(endpoint_id);
}

Datum orochi_auth_delete_webhook_endpoint_sql(PG_FUNCTION_ARGS)
{
    int64 endpoint_id = PG_GETARG_INT64(0);
    bool result = orochi_auth_delete_webhook_endpoint(endpoint_id);
    PG_RETURN_BOOL(result);
}

Datum orochi_auth_verify_webhook_signature_sql(PG_FUNCTION_ARGS)
{
    text *payload_text = PG_GETARG_TEXT_PP(0);
    text *signature_text = PG_GETARG_TEXT_PP(1);
    text *secret_text = PG_GETARG_TEXT_PP(2);
    int64 timestamp = PG_GETARG_INT64(3);

    char *payload = text_to_cstring(payload_text);
    char *signature = text_to_cstring(signature_text);
    char *secret = text_to_cstring(secret_text);

    bool valid = orochi_auth_verify_webhook_signature(payload, signature, secret, timestamp);

    PG_RETURN_BOOL(valid);
}

Datum orochi_auth_list_webhook_endpoints_sql(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext oldcontext;
        TupleDesc tupdesc;
        StringInfoData query;
        int ret;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Query endpoints */
        if (SPI_connect() != SPI_OK_CONNECT) {
            MemoryContextSwitchTo(oldcontext);
            SRF_RETURN_DONE(funcctx);
        }

        initStringInfo(&query);
        appendStringInfoString(&query,
                               "SELECT endpoint_id, url, enabled, timeout_ms, max_retries, "
                               "created_at FROM auth.webhook_endpoints ORDER BY endpoint_id");

        ret = SPI_execute(query.data, true, AUTH_WEBHOOK_MAX_ENDPOINTS);

        if (ret == SPI_OK_SELECT) {
            funcctx->max_calls = SPI_processed;
            funcctx->user_fctx = SPI_tuptable;
        } else {
            funcctx->max_calls = 0;
        }

        pfree(query.data);

        /* Build tuple descriptor */
        tupdesc = CreateTemplateTupleDesc(6);
        TupleDescInitEntry(tupdesc, 1, "endpoint_id", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "url", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "enabled", BOOLOID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "timeout_ms", INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "max_retries", INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, 6, "created_at", TIMESTAMPTZOID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if (call_cntr < max_calls) {
        SPITupleTable *tuptable = (SPITupleTable *)funcctx->user_fctx;
        HeapTuple spi_tuple = tuptable->vals[call_cntr];
        TupleDesc spi_tupdesc = tuptable->tupdesc;
        Datum values[6];
        bool nulls[6];
        HeapTuple tuple;
        bool isnull;

        memset(nulls, 0, sizeof(nulls));

        values[0] = SPI_getbinval(spi_tuple, spi_tupdesc, 1, &isnull);
        values[1] = CStringGetTextDatum(SPI_getvalue(spi_tuple, spi_tupdesc, 2));
        values[2] = SPI_getbinval(spi_tuple, spi_tupdesc, 3, &isnull);
        values[3] = SPI_getbinval(spi_tuple, spi_tupdesc, 4, &isnull);
        values[4] = SPI_getbinval(spi_tuple, spi_tupdesc, 5, &isnull);
        values[5] = SPI_getbinval(spi_tuple, spi_tupdesc, 6, &isnull);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SPI_finish();
    SRF_RETURN_DONE(funcctx);
}
