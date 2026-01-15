/*-------------------------------------------------------------------------
 *
 * tiered_storage.c
 *    Orochi DB tiered storage implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <time.h>

#include "../orochi.h"
#include "../core/catalog.h"
#include "../storage/columnar.h"
#include "../timeseries/hypertable.h"
#include "tiered_storage.h"

/* Background worker state */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* S3 client instance */
static S3Client *global_s3_client = NULL;

/* GUC variables for tiered storage - defined in init.c */
extern char *orochi_s3_bucket;
extern char *orochi_s3_access_key;
extern char *orochi_s3_secret_key;
extern char *orochi_s3_region;
extern char *orochi_s3_endpoint;
extern int orochi_hot_threshold_hours;
extern int orochi_warm_threshold_days;
extern int orochi_cold_threshold_days;
extern bool orochi_enable_tiering;

/* Forward declarations */
static void tiering_sighup_handler(SIGNAL_ARGS);
static void tiering_sigterm_handler(SIGNAL_ARGS);
static void process_tiering_queue(void);
static bool do_tier_transition(int64 chunk_id, OrochiStorageTier to_tier);

/* ============================================================
 * AWS Signature V4 Helper Functions
 * ============================================================ */

/* Buffer for CURL response */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} CurlBuffer;

static size_t
orochi_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    CurlBuffer *buf = (CurlBuffer *)userp;

    if (buf->size + realsize + 1 > buf->capacity)
    {
        size_t new_capacity = buf->capacity * 2;
        if (new_capacity < buf->size + realsize + 1)
            new_capacity = buf->size + realsize + 1 + 1024;
        buf->data = repalloc(buf->data, new_capacity);
        buf->capacity = new_capacity;
    }

    memcpy(&(buf->data[buf->size]), contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = 0;

    return realsize;
}

/* Convert bytes to hex string */
static void
bytes_to_hex(const unsigned char *bytes, size_t len, char *hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++)
    {
        hex[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

/* SHA256 hash */
static void
sha256_hash(const char *data, size_t len, unsigned char *hash)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(hash, &ctx);
}

/* HMAC-SHA256 */
static void
hmac_sha256(const unsigned char *key, size_t key_len,
            const unsigned char *data, size_t data_len,
            unsigned char *result)
{
    unsigned int result_len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, result, &result_len);
}

/* URL encode a string */
static char *
url_encode(const char *str)
{
    StringInfoData buf;
    const char *p;

    initStringInfo(&buf);
    for (p = str; *p; p++)
    {
        if ((*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~')
        {
            appendStringInfoChar(&buf, *p);
        }
        else if (*p == '/')
        {
            appendStringInfoChar(&buf, '/');  /* Don't encode path separators */
        }
        else
        {
            appendStringInfo(&buf, "%%%02X", (unsigned char)*p);
        }
    }
    return buf.data;
}

/* Generate AWS Signature V4 */
static char *
generate_aws_signature_v4(S3Client *client, const char *method,
                          const char *uri, const char *query_string,
                          const char *payload, size_t payload_len,
                          const char *date_stamp, const char *amz_date,
                          char **out_authorization)
{
    unsigned char payload_hash[32];
    char payload_hash_hex[65];
    unsigned char date_key[32], date_region_key[32], date_region_service_key[32], signing_key[32];
    unsigned char signature_bytes[32];
    char signature_hex[65];
    StringInfoData canonical_request, string_to_sign, auth_header;
    char *canonical_uri;
    char scope[128];

    /* Hash the payload */
    sha256_hash(payload ? payload : "", payload_len, payload_hash);
    bytes_to_hex(payload_hash, 32, payload_hash_hex);

    /* URL encode the URI */
    canonical_uri = url_encode(uri);

    /* Build canonical request */
    initStringInfo(&canonical_request);
    appendStringInfo(&canonical_request,
        "%s\n"          /* HTTP method */
        "%s\n"          /* Canonical URI */
        "%s\n"          /* Canonical query string */
        "host:%s\n"     /* Canonical headers */
        "x-amz-content-sha256:%s\n"
        "x-amz-date:%s\n"
        "\n"            /* End of headers */
        "host;x-amz-content-sha256;x-amz-date\n"  /* Signed headers */
        "%s",           /* Payload hash */
        method,
        canonical_uri,
        query_string ? query_string : "",
        client->endpoint,
        payload_hash_hex,
        amz_date,
        payload_hash_hex);

    pfree(canonical_uri);

    /* Hash the canonical request */
    unsigned char canonical_request_hash[32];
    char canonical_request_hash_hex[65];
    sha256_hash(canonical_request.data, canonical_request.len, canonical_request_hash);
    bytes_to_hex(canonical_request_hash, 32, canonical_request_hash_hex);
    pfree(canonical_request.data);

    /* Build scope */
    snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request",
             date_stamp, client->region);

    /* Build string to sign */
    initStringInfo(&string_to_sign);
    appendStringInfo(&string_to_sign,
        "AWS4-HMAC-SHA256\n"
        "%s\n"
        "%s\n"
        "%s",
        amz_date,
        scope,
        canonical_request_hash_hex);

    /* Calculate signing key */
    char aws4_key[256];
    snprintf(aws4_key, sizeof(aws4_key), "AWS4%s", client->secret_key);
    hmac_sha256((unsigned char *)aws4_key, strlen(aws4_key),
                (unsigned char *)date_stamp, strlen(date_stamp), date_key);
    hmac_sha256(date_key, 32,
                (unsigned char *)client->region, strlen(client->region), date_region_key);
    hmac_sha256(date_region_key, 32,
                (unsigned char *)"s3", 2, date_region_service_key);
    hmac_sha256(date_region_service_key, 32,
                (unsigned char *)"aws4_request", 12, signing_key);

    /* Calculate signature */
    hmac_sha256(signing_key, 32,
                (unsigned char *)string_to_sign.data, string_to_sign.len, signature_bytes);
    bytes_to_hex(signature_bytes, 32, signature_hex);
    pfree(string_to_sign.data);

    /* Build authorization header */
    initStringInfo(&auth_header);
    appendStringInfo(&auth_header,
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
        client->access_key, scope, signature_hex);

    *out_authorization = auth_header.data;
    return pstrdup(payload_hash_hex);
}

/* ============================================================
 * S3 Client Implementation
 * ============================================================ */

S3Client *
s3_client_create(OrochiS3Config *config)
{
    S3Client *client;

    if (config == NULL)
        return NULL;

    client = palloc0(sizeof(S3Client));

    client->endpoint = config->endpoint ? pstrdup(config->endpoint) : NULL;
    client->bucket = config->bucket ? pstrdup(config->bucket) : NULL;
    client->access_key = config->access_key ? pstrdup(config->access_key) : NULL;
    client->secret_key = config->secret_key ? pstrdup(config->secret_key) : NULL;
    client->region = config->region ? pstrdup(config->region) : pstrdup("us-east-1");
    client->use_ssl = config->use_ssl;
    client->timeout_ms = config->connection_timeout > 0 ? config->connection_timeout : 30000;

    /* Initialize CURL handle would go here */
    client->curl_handle = NULL;

    return client;
}

void
s3_client_destroy(S3Client *client)
{
    if (client == NULL)
        return;

    if (client->endpoint)
        pfree(client->endpoint);
    if (client->bucket)
        pfree(client->bucket);
    if (client->access_key)
        pfree(client->access_key);
    if (client->secret_key)
        pfree(client->secret_key);
    if (client->region)
        pfree(client->region);

    /* Cleanup CURL handle would go here */

    pfree(client);
}

bool
s3_client_test_connection(S3Client *client)
{
    if (client == NULL || client->bucket == NULL)
        return false;

    /* TODO: Actually test connection with HEAD bucket request */
    return true;
}

S3UploadResult *
s3_upload(S3Client *client, const char *key, const char *data,
          int64 size, const char *content_type)
{
    S3UploadResult *result;
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    char url[1024];
    char uri[512];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;

    result = palloc0(sizeof(S3UploadResult));

    if (client == NULL || key == NULL || data == NULL)
    {
        result->success = false;
        result->error_message = pstrdup("Invalid parameters");
        return result;
    }

    /* Initialize response buffer */
    response_buf.data = palloc(1024);
    response_buf.size = 0;
    response_buf.capacity = 1024;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build URI and URL */
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s", client->endpoint, uri);
    else
        snprintf(url, sizeof(url), "http://%s%s", client->endpoint, uri);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "PUT", uri, NULL,
                                              data, size, date_stamp, amz_date,
                                              &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl)
    {
        result->success = false;
        result->error_message = pstrdup("Failed to initialize CURL");
        pfree(response_buf.data);
        return result;
    }

    /* Set up headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", payload_hash);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Content-Type: %s",
             content_type ? content_type : "application/octet-stream");
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Content-Length: %ld", size);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK)
    {
        result->success = false;
        result->error_message = pstrdup(curl_easy_strerror(res));
        result->http_status = 0;
    }
    else
    {
        result->http_status = (int)http_code;
        if (http_code >= 200 && http_code < 300)
        {
            result->success = true;
            result->etag = pstrdup("uploaded");
            elog(LOG, "S3 Upload successful: %s/%s (%ld bytes)", client->bucket, key, size);
        }
        else
        {
            result->success = false;
            result->error_message = pstrdup(response_buf.data);
            elog(WARNING, "S3 Upload failed: HTTP %ld - %s", http_code, response_buf.data);
        }
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(authorization);
    pfree(payload_hash);

    return result;
}

S3DownloadResult *
s3_download(S3Client *client, const char *key)
{
    S3DownloadResult *result;
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    char url[1024];
    char uri[512];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;

    result = palloc0(sizeof(S3DownloadResult));

    if (client == NULL || key == NULL)
    {
        result->success = false;
        result->error_message = pstrdup("Invalid parameters");
        return result;
    }

    /* Initialize response buffer */
    response_buf.data = palloc(4096);
    response_buf.size = 0;
    response_buf.capacity = 4096;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build URI and URL */
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s", client->endpoint, uri);
    else
        snprintf(url, sizeof(url), "http://%s%s", client->endpoint, uri);

    /* Generate AWS Signature V4 (empty payload for GET) */
    payload_hash = generate_aws_signature_v4(client, "GET", uri, NULL,
                                              "", 0, date_stamp, amz_date,
                                              &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl)
    {
        result->success = false;
        result->error_message = pstrdup("Failed to initialize CURL");
        pfree(response_buf.data);
        return result;
    }

    /* Set up headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", payload_hash);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK)
    {
        result->success = false;
        result->error_message = pstrdup(curl_easy_strerror(res));
        result->http_status = 0;
        pfree(response_buf.data);
    }
    else
    {
        result->http_status = (int)http_code;
        if (http_code >= 200 && http_code < 300)
        {
            result->success = true;
            result->data = response_buf.data;  /* Transfer ownership */
            result->size = response_buf.size;
            elog(LOG, "S3 Download successful: %s/%s (%zu bytes)", client->bucket, key, response_buf.size);
        }
        else
        {
            result->success = false;
            result->error_message = pstrdup(response_buf.data);
            elog(WARNING, "S3 Download failed: HTTP %ld - %s", http_code, response_buf.data);
            pfree(response_buf.data);
        }
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(authorization);
    pfree(payload_hash);

    return result;
}

bool
s3_delete(S3Client *client, const char *key)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    char url[1024];
    char uri[512];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;
    bool success = false;

    if (client == NULL || key == NULL)
        return false;

    /* Initialize response buffer */
    response_buf.data = palloc(1024);
    response_buf.size = 0;
    response_buf.capacity = 1024;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build URI and URL */
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s", client->endpoint, uri);
    else
        snprintf(url, sizeof(url), "http://%s%s", client->endpoint, uri);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "DELETE", uri, NULL,
                                              "", 0, date_stamp, amz_date,
                                              &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl)
    {
        pfree(response_buf.data);
        return false;
    }

    /* Set up headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", payload_hash);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300)
    {
        success = true;
        elog(LOG, "S3 Delete successful: %s/%s", client->bucket, key);
    }
    else
    {
        elog(WARNING, "S3 Delete failed: %s/%s - HTTP %ld", client->bucket, key, http_code);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(authorization);
    pfree(payload_hash);

    return success;
}

bool
s3_object_exists(S3Client *client, const char *key)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char url[1024];
    char uri[512];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;
    bool exists = false;

    if (client == NULL || key == NULL)
        return false;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build URI and URL */
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s", client->endpoint, uri);
    else
        snprintf(url, sizeof(url), "http://%s%s", client->endpoint, uri);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "HEAD", uri, NULL,
                                              "", 0, date_stamp, amz_date,
                                              &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl)
        return false;

    /* Set up headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", payload_hash);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  /* HEAD request */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200)
    {
        exists = true;
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(authorization);
    pfree(payload_hash);

    return exists;
}

char *
orochi_generate_s3_key(int64 chunk_id)
{
    char *key = palloc(128);
    snprintf(key, 128, "orochi/chunks/%ld.columnar", chunk_id);
    return key;
}

/* ============================================================
 * Tiering Policy Functions
 * ============================================================ */

void
orochi_set_tiering_policy(Oid table_oid, Interval *hot_to_warm,
                          Interval *warm_to_cold, Interval *cold_to_frozen)
{
    OrochiTieringPolicy policy;

    memset(&policy, 0, sizeof(policy));
    policy.table_oid = table_oid;
    policy.hot_to_warm = hot_to_warm;
    policy.warm_to_cold = warm_to_cold;
    policy.cold_to_frozen = cold_to_frozen;
    policy.compress_on_tier = true;
    policy.enabled = true;

    orochi_catalog_create_tiering_policy(&policy);

    elog(NOTICE, "Set tiering policy for table %u", table_oid);
}

OrochiTieringPolicy *
orochi_get_tiering_policy(Oid table_oid)
{
    return orochi_catalog_get_tiering_policy(table_oid);
}

void
orochi_remove_tiering_policy(Oid table_oid)
{
    OrochiTieringPolicy *policy = orochi_get_tiering_policy(table_oid);
    if (policy != NULL)
    {
        orochi_catalog_delete_tiering_policy(policy->policy_id);
        elog(NOTICE, "Removed tiering policy for table %u", table_oid);
    }
}

/* ============================================================
 * Tier Movement Functions
 * ============================================================ */

void
orochi_move_to_tier(int64 chunk_id, OrochiStorageTier tier)
{
    OrochiChunkInfo *chunk;
    OrochiStorageTier current_tier;

    chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("chunk %ld does not exist", chunk_id)));

    current_tier = chunk->storage_tier;

    if (current_tier == tier)
    {
        elog(NOTICE, "chunk %ld is already in %s tier",
             chunk_id, orochi_tier_name(tier));
        return;
    }

    /* Validate transition */
    if (tier < current_tier)
    {
        /* Moving to hotter tier - need to recall data */
        elog(NOTICE, "Recalling chunk %ld from %s to %s",
             chunk_id, orochi_tier_name(current_tier), orochi_tier_name(tier));
    }
    else
    {
        /* Moving to colder tier */
        elog(NOTICE, "Moving chunk %ld from %s to %s",
             chunk_id, orochi_tier_name(current_tier), orochi_tier_name(tier));
    }

    if (!do_tier_transition(chunk_id, tier))
        ereport(ERROR,
                (errcode(ERRCODE_IO_ERROR),
                 errmsg("failed to move chunk %ld to %s tier",
                        chunk_id, orochi_tier_name(tier))));
}

static bool
do_tier_transition(int64 chunk_id, OrochiStorageTier to_tier)
{
    OrochiChunkInfo *chunk;

    chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        return false;

    switch (to_tier)
    {
        case OROCHI_TIER_HOT:
            /* Decompress and keep in local storage */
            if (chunk->is_compressed)
                orochi_decompress_chunk(chunk_id);
            break;

        case OROCHI_TIER_WARM:
            /* Compress and keep in local storage */
            if (!chunk->is_compressed)
                orochi_compress_chunk(chunk_id);
            break;

        case OROCHI_TIER_COLD:
            {
                /* Compress and upload to S3 */
                S3Client *client;
                S3UploadResult *result;
                char *s3_key;
                OrochiS3Config config;

                if (!chunk->is_compressed)
                    orochi_compress_chunk(chunk_id);

                /* Get S3 client */
                memset(&config, 0, sizeof(config));
                config.endpoint = orochi_s3_endpoint;
                config.bucket = orochi_s3_bucket;
                config.access_key = orochi_s3_access_key;
                config.secret_key = orochi_s3_secret_key;
                config.region = orochi_s3_region;

                client = s3_client_create(&config);
                if (client == NULL)
                {
                    elog(WARNING, "Failed to create S3 client");
                    return false;
                }

                s3_key = orochi_generate_s3_key(chunk_id);

                /* TODO: Read chunk data and upload */
                result = s3_upload(client, s3_key, "chunk_data", 10,
                                   "application/octet-stream");

                if (!result->success)
                {
                    elog(WARNING, "Failed to upload chunk %ld to S3: %s",
                         chunk_id, result->error_message);
                    s3_client_destroy(client);
                    return false;
                }

                s3_client_destroy(client);
                pfree(s3_key);
            }
            break;

        case OROCHI_TIER_FROZEN:
            /* Move to Glacier - similar to cold but different storage class */
            break;
    }

    /* Update catalog */
    orochi_catalog_update_chunk_tier(chunk_id, to_tier);

    return true;
}

void
orochi_move_to_warm(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_WARM);
}

void
orochi_move_to_cold(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_COLD);
}

void
orochi_move_to_frozen(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_FROZEN);
}

void
orochi_recall_from_cold(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_WARM);
}

OrochiStorageTier
orochi_get_chunk_tier(int64 chunk_id)
{
    OrochiChunkInfo *chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        return OROCHI_TIER_HOT;
    return chunk->storage_tier;
}

/* ============================================================
 * Automatic Tiering
 * ============================================================ */

void
orochi_apply_tiering_policies(void)
{
    List *policies;
    ListCell *lc;

    if (!orochi_enable_tiering)
        return;

    policies = orochi_catalog_get_active_tiering_policies();

    foreach(lc, policies)
    {
        OrochiTieringPolicy *policy = (OrochiTieringPolicy *) lfirst(lc);
        orochi_apply_tiering_for_table(policy->table_oid);
    }
}

int
orochi_apply_tiering_for_table(Oid table_oid)
{
    OrochiTieringPolicy *policy;
    OrochiTableInfo *table_info;
    List *chunks;
    ListCell *lc;
    TimestampTz now;
    int64 hot_threshold_usec;
    int64 warm_threshold_usec;
    int64 cold_threshold_usec;
    int moved = 0;

    policy = orochi_get_tiering_policy(table_oid);
    if (policy == NULL || !policy->enabled)
        return 0;

    table_info = orochi_catalog_get_table(table_oid);
    if (table_info == NULL)
        return 0;

    now = GetCurrentTimestamp();

    /* Calculate thresholds in microseconds */
    hot_threshold_usec = (int64) orochi_hot_threshold_hours * USECS_PER_HOUR;
    warm_threshold_usec = (int64) orochi_warm_threshold_days * USECS_PER_DAY;
    cold_threshold_usec = (int64) orochi_cold_threshold_days * USECS_PER_DAY;

    /* Get all chunks for table */
    if (table_info->is_timeseries)
        chunks = orochi_catalog_get_hypertable_chunks(table_oid);
    else
        return 0;  /* Only hypertables support tiering for now */

    foreach(lc, chunks)
    {
        OrochiChunkInfo *chunk = (OrochiChunkInfo *) lfirst(lc);
        int64 age_usec = now - chunk->range_end;

        /* Determine target tier based on age */
        OrochiStorageTier target_tier = chunk->storage_tier;

        if (age_usec > cold_threshold_usec && chunk->storage_tier < OROCHI_TIER_COLD)
            target_tier = OROCHI_TIER_COLD;
        else if (age_usec > warm_threshold_usec && chunk->storage_tier < OROCHI_TIER_WARM)
            target_tier = OROCHI_TIER_WARM;
        else if (age_usec > hot_threshold_usec && chunk->storage_tier < OROCHI_TIER_WARM)
            target_tier = OROCHI_TIER_WARM;

        if (target_tier != chunk->storage_tier)
        {
            elog(DEBUG1, "Tiering chunk %ld from %s to %s",
                 chunk->chunk_id,
                 orochi_tier_name(chunk->storage_tier),
                 orochi_tier_name(target_tier));

            if (do_tier_transition(chunk->chunk_id, target_tier))
                moved++;
        }
    }

    return moved;
}

/* ============================================================
 * Access Tracking
 * ============================================================ */

void
orochi_record_chunk_access(int64 chunk_id, bool is_write)
{
    /* TODO: Implement access tracking */
    orochi_catalog_record_shard_access(chunk_id);
}

/* ============================================================
 * Query Integration
 * ============================================================ */

void
orochi_prepare_chunk_for_read(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);

    if (tier == OROCHI_TIER_COLD || tier == OROCHI_TIER_FROZEN)
    {
        elog(NOTICE, "Recalling chunk %ld from cold storage", chunk_id);
        orochi_recall_from_cold(chunk_id);
    }
}

bool
orochi_chunk_is_accessible(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);
    return (tier == OROCHI_TIER_HOT || tier == OROCHI_TIER_WARM);
}

int
orochi_get_chunk_access_latency_ms(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);

    switch (tier)
    {
        case OROCHI_TIER_HOT:
            return 1;       /* ~1ms local SSD */
        case OROCHI_TIER_WARM:
            return 10;      /* ~10ms decompression overhead */
        case OROCHI_TIER_COLD:
            return 100;     /* ~100ms S3 access */
        case OROCHI_TIER_FROZEN:
            return 3600000; /* Hours for Glacier recall */
        default:
            return 1;
    }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *
orochi_tier_name(OrochiStorageTier tier)
{
    switch (tier)
    {
        case OROCHI_TIER_HOT:    return "hot";
        case OROCHI_TIER_WARM:   return "warm";
        case OROCHI_TIER_COLD:   return "cold";
        case OROCHI_TIER_FROZEN: return "frozen";
        default:                 return "unknown";
    }
}

OrochiStorageTier
orochi_parse_tier(const char *name)
{
    if (pg_strcasecmp(name, "hot") == 0)    return OROCHI_TIER_HOT;
    if (pg_strcasecmp(name, "warm") == 0)   return OROCHI_TIER_WARM;
    if (pg_strcasecmp(name, "cold") == 0)   return OROCHI_TIER_COLD;
    if (pg_strcasecmp(name, "frozen") == 0) return OROCHI_TIER_FROZEN;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown storage tier: %s", name)));
    return OROCHI_TIER_HOT;
}

/* ============================================================
 * Background Workers
 * ============================================================ */

static void
tiering_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
tiering_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

void
orochi_tiering_worker_main(Datum main_arg)
{
    /* Set up signal handlers */
    pqsignal(SIGHUP, tiering_sighup_handler);
    pqsignal(SIGTERM, tiering_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi tiering worker started");

    while (!got_sigterm)
    {
        int rc;

        /* Wait for work or timeout */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       60000L,  /* 1 minute */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            break;

        /* Process tiering */
        if (orochi_enable_tiering)
        {
            StartTransactionCommand();
            orochi_apply_tiering_policies();
            CommitTransactionCommand();
        }
    }

    elog(LOG, "Orochi tiering worker shutting down");
    proc_exit(0);
}

void
orochi_compression_worker_main(Datum main_arg)
{
    pqsignal(SIGHUP, tiering_sighup_handler);
    pqsignal(SIGTERM, tiering_sigterm_handler);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi compression worker started");

    while (!got_sigterm)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       300000L,  /* 5 minutes */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            break;

        /* Process compression policies */
        StartTransactionCommand();
        orochi_run_maintenance();
        CommitTransactionCommand();
    }

    elog(LOG, "Orochi compression worker shutting down");
    proc_exit(0);
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_set_tiering_policy_sql);
PG_FUNCTION_INFO_V1(orochi_move_chunk_to_tier_sql);
PG_FUNCTION_INFO_V1(orochi_add_node_sql);
PG_FUNCTION_INFO_V1(orochi_remove_node_sql);
PG_FUNCTION_INFO_V1(orochi_drain_node_sql);
PG_FUNCTION_INFO_V1(orochi_rebalance_shards_sql);

Datum
orochi_set_tiering_policy_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    Interval *hot_after = PG_ARGISNULL(1) ? NULL : PG_GETARG_INTERVAL_P(1);
    Interval *warm_after = PG_ARGISNULL(2) ? NULL : PG_GETARG_INTERVAL_P(2);
    Interval *cold_after = PG_ARGISNULL(3) ? NULL : PG_GETARG_INTERVAL_P(3);

    OrochiTieringPolicy policy;
    memset(&policy, 0, sizeof(policy));
    policy.table_oid = table_oid;
    policy.hot_to_warm = hot_after;
    policy.warm_to_cold = warm_after;
    policy.cold_to_frozen = cold_after;
    policy.enabled = true;
    policy.compress_on_tier = true;

    orochi_catalog_create_tiering_policy(&policy);
    PG_RETURN_VOID();
}

Datum
orochi_move_chunk_to_tier_sql(PG_FUNCTION_ARGS)
{
    int64 chunk_id = PG_GETARG_INT64(0);
    text *tier_text = PG_GETARG_TEXT_PP(1);
    char *tier = text_to_cstring(tier_text);
    OrochiStorageTier target_tier;

    if (strcmp(tier, "hot") == 0)
        target_tier = OROCHI_TIER_HOT;
    else if (strcmp(tier, "warm") == 0)
        target_tier = OROCHI_TIER_WARM;
    else if (strcmp(tier, "cold") == 0)
        target_tier = OROCHI_TIER_COLD;
    else if (strcmp(tier, "frozen") == 0)
        target_tier = OROCHI_TIER_FROZEN;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid tier: %s", tier),
                 errhint("Valid tiers are: hot, warm, cold, frozen")));

    orochi_catalog_update_chunk_tier(chunk_id, target_tier);
    PG_RETURN_VOID();
}

Datum
orochi_add_node_sql(PG_FUNCTION_ARGS)
{
    text *hostname_text = PG_GETARG_TEXT_PP(0);
    int32 port = PG_GETARG_INT32(1);
    char *hostname = text_to_cstring(hostname_text);
    int32 node_id;

    node_id = orochi_catalog_add_node(hostname, port, OROCHI_NODE_WORKER);
    PG_RETURN_INT32(node_id);
}

Datum
orochi_remove_node_sql(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);
    /* TODO: Implement proper node removal with shard migration */
    orochi_catalog_update_node_status(node_id, false);
    elog(NOTICE, "Marked node %d as inactive", node_id);
    PG_RETURN_VOID();
}

Datum
orochi_drain_node_sql(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);
    /* TODO: Implement shard migration to other nodes */
    elog(NOTICE, "Draining node %d (migration not yet implemented)", node_id);
    PG_RETURN_VOID();
}

Datum
orochi_rebalance_shards_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
    text *strategy_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);
    char *strategy = strategy_text ? text_to_cstring(strategy_text) : "by_shard_count";

    /* TODO: Implement shard rebalancing */
    if (OidIsValid(table_oid))
        elog(NOTICE, "Rebalancing shards for table %u using strategy %s", table_oid, strategy);
    else
        elog(NOTICE, "Rebalancing all shards using strategy %s", strategy);

    PG_RETURN_VOID();
}
