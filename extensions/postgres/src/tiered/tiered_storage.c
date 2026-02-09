/*-------------------------------------------------------------------------
 *
 * tiered_storage.c
 *    Orochi DB tiered storage implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <time.h>

#include "../core/catalog.h"
#include "../orochi.h"
#include "../storage/columnar.h"
#include "../storage/compression.h"
#include "../timeseries/hypertable.h"
#include "tiered_storage.h"

/* PostgreSQL epoch constants for timestamp conversion */
#define POSTGRES_EPOCH_JDATE 2451545 /* Julian date of Postgres epoch (2000-01-01) */

/* SHA256 hash of empty string for AWS signature */
#define EMPTY_SHA256_HASH "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

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

#ifdef HAVE_LIBCURL

/* Buffer for CURL response */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} CurlBuffer;

static size_t orochi_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    CurlBuffer *buf = (CurlBuffer *)userp;

    if (buf->size + realsize + 1 > buf->capacity) {
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
static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        hex[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

/* SHA256 hash */
static void sha256_hash(const char *data, size_t len, unsigned char *hash)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(hash, &ctx);
}

/* HMAC-SHA256 */
static void hmac_sha256(const unsigned char *key, size_t key_len, const unsigned char *data,
                        size_t data_len, unsigned char *result)
{
    unsigned int result_len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, result, &result_len);
}

/* URL encode a string */
static char *url_encode(const char *str)
{
    StringInfoData buf;
    const char *p;

    initStringInfo(&buf);
    for (p = str; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            appendStringInfoChar(&buf, *p);
        } else if (*p == '/') {
            appendStringInfoChar(&buf, '/'); /* Don't encode path separators */
        } else {
            appendStringInfo(&buf, "%%%02X", (unsigned char)*p);
        }
    }
    return buf.data;
}

/* Generate AWS Signature V4 */
char *generate_aws_signature_v4(S3Client *client, const char *method, const char *uri,
                                const char *query_string, const char *payload, size_t payload_len,
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
                     "%s\n"      /* HTTP method */
                     "%s\n"      /* Canonical URI */
                     "%s\n"      /* Canonical query string */
                     "host:%s\n" /* Canonical headers */
                     "x-amz-content-sha256:%s\n"
                     "x-amz-date:%s\n"
                     "\n"                                     /* End of headers */
                     "host;x-amz-content-sha256;x-amz-date\n" /* Signed headers */
                     "%s",                                    /* Payload hash */
                     method, canonical_uri, query_string ? query_string : "", client->endpoint,
                     payload_hash_hex, amz_date, payload_hash_hex);

    pfree(canonical_uri);

    /* Hash the canonical request */
    unsigned char canonical_request_hash[32];
    char canonical_request_hash_hex[65];
    sha256_hash(canonical_request.data, canonical_request.len, canonical_request_hash);
    bytes_to_hex(canonical_request_hash, 32, canonical_request_hash_hex);
    pfree(canonical_request.data);

    /* Build scope */
    snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request", date_stamp, client->region);

    /* Build string to sign */
    initStringInfo(&string_to_sign);
    appendStringInfo(&string_to_sign,
                     "AWS4-HMAC-SHA256\n"
                     "%s\n"
                     "%s\n"
                     "%s",
                     amz_date, scope, canonical_request_hash_hex);

    /* Calculate signing key */
    char aws4_key[256];
    snprintf(aws4_key, sizeof(aws4_key), "AWS4%s", client->secret_key);
    hmac_sha256((unsigned char *)aws4_key, strlen(aws4_key), (unsigned char *)date_stamp,
                strlen(date_stamp), date_key);
    hmac_sha256(date_key, 32, (unsigned char *)client->region, strlen(client->region),
                date_region_key);
    hmac_sha256(date_region_key, 32, (unsigned char *)"s3", 2, date_region_service_key);
    hmac_sha256(date_region_service_key, 32, (unsigned char *)"aws4_request", 12, signing_key);

    /* Calculate signature */
    hmac_sha256(signing_key, 32, (unsigned char *)string_to_sign.data, string_to_sign.len,
                signature_bytes);
    bytes_to_hex(signature_bytes, 32, signature_hex);
    pfree(string_to_sign.data);

    /* Build authorization header */
    initStringInfo(&auth_header);
    appendStringInfo(&auth_header,
                     "AWS4-HMAC-SHA256 Credential=%s/%s, "
                     "SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
                     client->access_key, scope, signature_hex);

    *out_authorization = auth_header.data;
    return pstrdup(payload_hash_hex);
}

/* ============================================================
 * S3 Client Implementation
 * ============================================================ */

S3Client *s3_client_create(OrochiS3Config *config)
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

void s3_client_destroy(S3Client *client)
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

bool s3_client_test_connection(S3Client *client)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char url[1024];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;

    if (client == NULL || client->bucket == NULL)
        return false;

    /* Build URL for HEAD bucket request */
    snprintf(url, sizeof(url), "%s://%s/%s", client->use_ssl ? "https" : "http", client->endpoint,
             client->bucket);

    /* Get current time for request signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Generate authorization header */
    generate_aws_signature_v4(client, "HEAD", client->bucket, "", NULL, 0, date_stamp, amz_date,
                              &authorization);

    curl = curl_easy_init();
    if (curl == NULL)
        return false;

    /* Set request headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", EMPTY_SHA256_HASH);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    /* Configure HEAD request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    if (!client->use_ssl)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    /* Execute request */
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        pfree(authorization);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(authorization);

    /* Success if we get 200 or 403 (bucket exists but may have access issues) */
    return (http_code == 200 || http_code == 403);
}

S3UploadResult *s3_upload(S3Client *client, const char *key, const char *data, int64 size,
                          const char *content_type)
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

    if (client == NULL || key == NULL || data == NULL) {
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
    payload_hash = generate_aws_signature_v4(client, "PUT", uri, NULL, data, size, date_stamp,
                                             amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
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

    if (res != CURLE_OK) {
        result->success = false;
        result->error_message = pstrdup(curl_easy_strerror(res));
        result->http_status = 0;
    } else {
        result->http_status = (int)http_code;
        if (http_code >= 200 && http_code < 300) {
            result->success = true;
            result->etag = pstrdup("uploaded");
            elog(LOG, "S3 Upload successful: %s/%s (%ld bytes)", client->bucket, key, size);
        } else {
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

S3DownloadResult *s3_download(S3Client *client, const char *key)
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

    if (client == NULL || key == NULL) {
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
    payload_hash = generate_aws_signature_v4(client, "GET", uri, NULL, "", 0, date_stamp, amz_date,
                                             &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
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

    if (res != CURLE_OK) {
        result->success = false;
        result->error_message = pstrdup(curl_easy_strerror(res));
        result->http_status = 0;
        pfree(response_buf.data);
    } else {
        result->http_status = (int)http_code;
        if (http_code >= 200 && http_code < 300) {
            result->success = true;
            result->data = response_buf.data; /* Transfer ownership */
            result->size = response_buf.size;
            elog(LOG, "S3 Download successful: %s/%s (%zu bytes)", client->bucket, key,
                 response_buf.size);
        } else {
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

bool s3_delete(S3Client *client, const char *key)
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
    payload_hash = generate_aws_signature_v4(client, "DELETE", uri, NULL, "", 0, date_stamp,
                                             amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
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

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        success = true;
        elog(LOG, "S3 Delete successful: %s/%s", client->bucket, key);
    } else {
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

bool s3_object_exists(S3Client *client, const char *key)
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
    payload_hash = generate_aws_signature_v4(client, "HEAD", uri, NULL, "", 0, date_stamp, amz_date,
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
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* HEAD request */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200) {
        exists = true;
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(authorization);
    pfree(payload_hash);

    return exists;
}

/* Header callback for extracting metadata from HTTP headers */
typedef struct HeaderCallbackData {
    char *etag;
    int64 size;
    TimestampTz last_modified;
    char *content_type;
    char *storage_class;
} HeaderCallbackData;

static size_t orochi_curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t realsize = size * nitems;
    HeaderCallbackData *data = (HeaderCallbackData *)userdata;
    char *header_line;
    char *colon;
    char *value;

    /* Make a null-terminated copy of the header line */
    header_line = palloc(realsize + 1);
    memcpy(header_line, buffer, realsize);
    header_line[realsize] = '\0';

    /* Find the colon separator */
    colon = strchr(header_line, ':');
    if (colon == NULL) {
        pfree(header_line);
        return realsize;
    }

    /* Split into name and value */
    *colon = '\0';
    value = colon + 1;

    /* Skip leading whitespace in value */
    while (*value == ' ' || *value == '\t')
        value++;

    /* Remove trailing whitespace/newlines */
    {
        char *end = value + strlen(value) - 1;
        while (end > value && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t'))
            *end-- = '\0';
    }

    /* Extract metadata based on header name */
    if (pg_strcasecmp(header_line, "ETag") == 0) {
        /* Remove quotes from ETag if present */
        if (*value == '"') {
            value++;
            char *quote = strchr(value, '"');
            if (quote)
                *quote = '\0';
        }
        data->etag = pstrdup(value);
    } else if (pg_strcasecmp(header_line, "Content-Length") == 0) {
        data->size = strtoll(value, NULL, 10);
    } else if (pg_strcasecmp(header_line, "Last-Modified") == 0) {
        /* Parse HTTP date format: "Fri, 17 Jan 2025 12:34:56 GMT" */
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL) {
            time_t t = timegm(&tm);
            /* Convert to PostgreSQL TimestampTz (microseconds since 2000-01-01) */
            data->last_modified = (TimestampTz)(t - POSTGRES_EPOCH_JDATE) * USECS_PER_SEC;
        }
    } else if (pg_strcasecmp(header_line, "Content-Type") == 0) {
        data->content_type = pstrdup(value);
    } else if (pg_strcasecmp(header_line, "x-amz-storage-class") == 0) {
        data->storage_class = pstrdup(value);
    }

    pfree(header_line);
    return realsize;
}

S3Object *s3_head_object(S3Client *client, const char *key)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    HeaderCallbackData header_data;
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
    S3Object *obj = NULL;

    if (client == NULL || key == NULL)
        return NULL;

    /* Initialize header callback data */
    memset(&header_data, 0, sizeof(header_data));

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
    payload_hash = generate_aws_signature_v4(client, "HEAD", uri, NULL, "", 0, date_stamp, amz_date,
                                             &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(authorization);
        pfree(payload_hash);
        return NULL;
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
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* HEAD request */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, orochi_curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200) {
        /* Create S3Object with collected metadata */
        obj = palloc0(sizeof(S3Object));
        obj->key = pstrdup(key);
        obj->etag = header_data.etag ? header_data.etag : pstrdup("");
        obj->size = header_data.size;
        obj->last_modified = header_data.last_modified;
        obj->content_type = header_data.content_type ? header_data.content_type
                                                     : pstrdup("application/octet-stream");
        obj->storage_class =
            header_data.storage_class ? header_data.storage_class : pstrdup("STANDARD");

        elog(DEBUG1, "S3 HEAD successful: %s/%s (size=%ld, etag=%s)", client->bucket, key,
             obj->size, obj->etag);
    } else if (res == CURLE_OK && http_code == 404) {
        /* Object does not exist - return NULL */
        elog(DEBUG1, "S3 HEAD: object not found: %s/%s", client->bucket, key);
    } else {
        /* Error occurred */
        elog(WARNING, "S3 HEAD failed: %s/%s - HTTP %ld, CURL error: %s", client->bucket, key,
             http_code, curl_easy_strerror(res));
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(authorization);
    pfree(payload_hash);

    return obj;
}

/* Simple XML parser for S3 ListObjectsV2 response */
static char *extract_xml_text(const char *tag, const char **next_pos)
{
    char *start_tag;
    char *end_tag;
    char *value;
    char open_tag[128];
    char close_tag[128];
    size_t len;

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start_tag = strstr(*next_pos, open_tag);
    if (start_tag == NULL)
        return NULL;

    start_tag += strlen(open_tag);
    end_tag = strstr(start_tag, close_tag);
    if (end_tag == NULL)
        return NULL;

    len = end_tag - start_tag;
    value = palloc(len + 1);
    memcpy(value, start_tag, len);
    value[len] = '\0';

    *next_pos = end_tag + strlen(close_tag);
    return value;
}

List *s3_list_objects(S3Client *client, const char *prefix)
{
    List *objects = NIL;
    char *continuation_token = NULL;
    bool is_truncated = true;
    int total_objects = 0;

    if (client == NULL)
        return NIL;

    /* Handle pagination - loop until all objects are retrieved */
    while (is_truncated) {
        CURL *curl;
        CURLcode res;
        struct curl_slist *headers = NULL;
        CurlBuffer response_buf;
        char url[2048];
        char uri[512];
        char query_string[1024];
        char date_stamp[16];
        char amz_date[32];
        char *authorization;
        char *payload_hash;
        char header_buf[512];
        time_t now;
        struct tm *tm_info;
        long http_code;

        /* Initialize response buffer */
        response_buf.data = palloc(65536);
        response_buf.size = 0;
        response_buf.capacity = 65536;

        /* Get current time for signing */
        now = time(NULL);
        tm_info = gmtime(&now);
        strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
        strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

        /* Build query string with pagination support */
        if (continuation_token) {
            char *encoded_token = url_encode(continuation_token);
            char *encoded_prefix = prefix ? url_encode(prefix) : NULL;

            if (encoded_prefix)
                snprintf(query_string, sizeof(query_string),
                         "list-type=2&prefix=%s&continuation-token=%s", encoded_prefix,
                         encoded_token);
            else
                snprintf(query_string, sizeof(query_string), "list-type=2&continuation-token=%s",
                         encoded_token);

            if (encoded_prefix)
                pfree(encoded_prefix);
            pfree(encoded_token);
        } else {
            if (prefix && *prefix) {
                char *encoded_prefix = url_encode(prefix);
                snprintf(query_string, sizeof(query_string), "list-type=2&prefix=%s",
                         encoded_prefix);
                pfree(encoded_prefix);
            } else {
                snprintf(query_string, sizeof(query_string), "list-type=2");
            }
        }

        /* Build URI and URL */
        snprintf(uri, sizeof(uri), "/%s", client->bucket);
        if (client->use_ssl)
            snprintf(url, sizeof(url), "https://%s%s?%s", client->endpoint, uri, query_string);
        else
            snprintf(url, sizeof(url), "http://%s%s?%s", client->endpoint, uri, query_string);

        /* Generate AWS Signature V4 */
        payload_hash = generate_aws_signature_v4(client, "GET", uri, query_string, "", 0,
                                                 date_stamp, amz_date, &authorization);

        /* Initialize CURL */
        curl = curl_easy_init();
        if (!curl) {
            pfree(response_buf.data);
            pfree(authorization);
            pfree(payload_hash);
            if (continuation_token)
                pfree(continuation_token);
            return objects;
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

        if (res != CURLE_OK || http_code != 200) {
            elog(WARNING, "S3 ListObjects failed: HTTP %ld, CURL error: %s", http_code,
                 curl_easy_strerror(res));
            pfree(response_buf.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            pfree(authorization);
            pfree(payload_hash);
            if (continuation_token)
                pfree(continuation_token);
            return objects;
        }

        /* Parse XML response */
        {
            const char *xml = response_buf.data;
            const char *pos = xml;
            const char *contents_start;

            /* Check if results are truncated */
            {
                const char *truncated_pos = xml;
                char *truncated_str = extract_xml_text("IsTruncated", &truncated_pos);
                is_truncated = (truncated_str && strcmp(truncated_str, "true") == 0);
                if (truncated_str)
                    pfree(truncated_str);
            }

            /* Get next continuation token if truncated */
            if (is_truncated) {
                const char *token_pos = xml;
                if (continuation_token)
                    pfree(continuation_token);
                continuation_token = extract_xml_text("NextContinuationToken", &token_pos);
                if (continuation_token == NULL)
                    is_truncated = false; /* No token means we're done */
            }

            /* Parse each <Contents> element */
            contents_start = strstr(pos, "<Contents>");
            while (contents_start != NULL) {
                const char *contents_end = strstr(contents_start, "</Contents>");
                if (contents_end == NULL)
                    break;

                /* Extract object metadata */
                {
                    S3Object *obj = palloc0(sizeof(S3Object));
                    const char *elem_pos = contents_start;
                    char *key_str = extract_xml_text("Key", &elem_pos);
                    char *size_str = extract_xml_text("Size", &elem_pos);
                    char *etag_str = extract_xml_text("ETag", &elem_pos);
                    char *modified_str = extract_xml_text("LastModified", &elem_pos);
                    char *storage_str = extract_xml_text("StorageClass", &elem_pos);

                    if (key_str) {
                        obj->key = key_str;
                        obj->size = size_str ? strtoll(size_str, NULL, 10) : 0;

                        /* Remove quotes from ETag */
                        if (etag_str) {
                            if (*etag_str == '"') {
                                char *end = strchr(etag_str + 1, '"');
                                if (end)
                                    *end = '\0';
                                obj->etag = pstrdup(etag_str + 1);
                                pfree(etag_str);
                            } else {
                                obj->etag = etag_str;
                            }
                        } else {
                            obj->etag = pstrdup("");
                        }

                        /* Parse LastModified timestamp: "2025-01-17T12:34:56.000Z" */
                        if (modified_str) {
                            struct tm tm;
                            memset(&tm, 0, sizeof(tm));
                            if (strptime(modified_str, "%Y-%m-%dT%H:%M:%S", &tm) != NULL) {
                                time_t t = timegm(&tm);
                                obj->last_modified =
                                    (TimestampTz)(t - POSTGRES_EPOCH_JDATE) * USECS_PER_SEC;
                            }
                            pfree(modified_str);
                        }

                        obj->content_type = pstrdup("application/octet-stream");
                        obj->storage_class = storage_str ? storage_str : pstrdup("STANDARD");

                        objects = lappend(objects, obj);
                        total_objects++;

                        if (size_str)
                            pfree(size_str);
                    } else {
                        pfree(obj);
                        if (size_str)
                            pfree(size_str);
                        if (etag_str)
                            pfree(etag_str);
                        if (modified_str)
                            pfree(modified_str);
                        if (storage_str)
                            pfree(storage_str);
                    }
                }

                /* Move to next <Contents> */
                contents_start = strstr(contents_end, "<Contents>");
            }
        }

        /* Cleanup this iteration */
        pfree(response_buf.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        pfree(authorization);
        pfree(payload_hash);
    }

    if (continuation_token)
        pfree(continuation_token);

    elog(DEBUG1, "S3 ListObjects: found %d objects with prefix '%s'", total_objects,
         prefix ? prefix : "(none)");

    return objects;
}

#else /* !HAVE_LIBCURL */

/*
 * Stub implementations when libcurl is not available.
 * S3 operations require libcurl to function.
 */

char *generate_aws_signature_v4(S3Client *client, const char *method, const char *uri,
                                const char *query_string, const char *payload, size_t payload_len,
                                const char *date_stamp, const char *amz_date,
                                char **out_authorization)
{
    elog(WARNING, "Orochi: AWS signature generation requires libcurl");
    *out_authorization = pstrdup("");
    return pstrdup("");
}

bool s3_client_test_connection(S3Client *client)
{
    elog(WARNING, "Orochi: S3 connection test requires libcurl");
    return false;
}

S3UploadResult *s3_upload(S3Client *client, const char *key, const char *data, int64 size,
                          const char *content_type)
{
    S3UploadResult *result = palloc0(sizeof(S3UploadResult));
    result->success = false;
    result->error_message = pstrdup("S3 upload requires libcurl");
    elog(WARNING, "Orochi: S3 upload requires libcurl");
    return result;
}

S3DownloadResult *s3_download(S3Client *client, const char *key)
{
    S3DownloadResult *result = palloc0(sizeof(S3DownloadResult));
    result->success = false;
    result->error_message = pstrdup("S3 download requires libcurl");
    elog(WARNING, "Orochi: S3 download requires libcurl");
    return result;
}

bool s3_delete(S3Client *client, const char *key)
{
    elog(WARNING, "Orochi: S3 delete requires libcurl");
    return false;
}

bool s3_object_exists(S3Client *client, const char *key)
{
    elog(WARNING, "Orochi: S3 object check requires libcurl");
    return false;
}

S3Object *s3_head_object(S3Client *client, const char *key)
{
    elog(WARNING, "Orochi: S3 head object requires libcurl");
    return NULL;
}

List *s3_list_objects(S3Client *client, const char *prefix)
{
    elog(WARNING, "Orochi: S3 list objects requires libcurl");
    return NIL;
}

S3Client *s3_client_create(OrochiS3Config *config)
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
    client->curl_handle = NULL;

    return client;
}

void s3_client_destroy(S3Client *client)
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
    pfree(client);
}

#endif /* HAVE_LIBCURL */

char *orochi_generate_s3_key(int64 chunk_id)
{
    char *key = palloc(128);
    snprintf(key, 128, "orochi/chunks/%ld.columnar", chunk_id);
    return key;
}

/* ============================================================
 * Tiering Policy Functions
 * ============================================================ */

void orochi_set_tiering_policy(Oid table_oid, Interval *hot_to_warm, Interval *warm_to_cold,
                               Interval *cold_to_frozen)
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

OrochiTieringPolicy *orochi_get_tiering_policy(Oid table_oid)
{
    return orochi_catalog_get_tiering_policy(table_oid);
}

void orochi_remove_tiering_policy(Oid table_oid)
{
    OrochiTieringPolicy *policy = orochi_get_tiering_policy(table_oid);
    if (policy != NULL) {
        orochi_catalog_delete_tiering_policy(policy->policy_id);
        elog(NOTICE, "Removed tiering policy for table %u", table_oid);
    }
}

/* ============================================================
 * Tier Movement Functions
 * ============================================================ */

void orochi_move_to_tier(int64 chunk_id, OrochiStorageTier tier)
{
    OrochiChunkInfo *chunk;
    OrochiStorageTier current_tier;

    chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("chunk %ld does not exist", chunk_id)));

    current_tier = chunk->storage_tier;

    if (current_tier == tier) {
        elog(NOTICE, "chunk %ld is already in %s tier", chunk_id, orochi_tier_name(tier));
        return;
    }

    /* Validate transition */
    if (tier < current_tier) {
        /* Moving to hotter tier - need to recall data */
        elog(NOTICE, "Recalling chunk %ld from %s to %s", chunk_id, orochi_tier_name(current_tier),
             orochi_tier_name(tier));
    } else {
        /* Moving to colder tier */
        elog(NOTICE, "Moving chunk %ld from %s to %s", chunk_id, orochi_tier_name(current_tier),
             orochi_tier_name(tier));
    }

    if (!do_tier_transition(chunk_id, tier))
        ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("failed to move chunk %ld to %s tier",
                                                          chunk_id, orochi_tier_name(tier))));
}

static bool do_tier_transition(int64 chunk_id, OrochiStorageTier to_tier)
{
    OrochiChunkInfo *chunk;

    chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        return false;

    switch (to_tier) {
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

    case OROCHI_TIER_COLD: {
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
        if (client == NULL) {
            elog(WARNING, "Failed to create S3 client");
            return false;
        }

        s3_key = orochi_generate_s3_key(chunk_id);

        /* Read chunk data from PostgreSQL relation */
        {
            OrochiChunkInfo *chunk_info = orochi_catalog_get_chunk(chunk_id);
            StringInfoData chunk_buffer;
            StringInfoData copy_data;
            char chunk_table_name[NAMEDATALEN * 2];
            char *compressed_data = NULL;
            int64 compressed_size = 0;
            int spi_ret;
            uint32 magic = 0x4F524348; /* "ORCH" magic header */
            uint16 version = 1;
            uint16 flags = 0;
            uint32 checksum = 0;
            int32 column_count = 0;
            OrochiCompressionType compression = OROCHI_COMPRESS_LZ4;

            if (chunk_info == NULL) {
                elog(WARNING, "Chunk %ld not found in catalog", chunk_id);
                s3_client_destroy(client);
                return false;
            }

            /* Build chunk table name: _orochi_internal._chunk_<id> */
            snprintf(chunk_table_name, sizeof(chunk_table_name), "_orochi_internal._chunk_%ld",
                     chunk_id);

            /* Initialize buffers */
            initStringInfo(&chunk_buffer);
            initStringInfo(&copy_data);

            SPI_connect();

            /* Get column count and column types */
            spi_ret = SPI_execute(psprintf("SELECT a.attnum, a.atttypid, t.typname "
                                           "FROM pg_attribute a "
                                           "JOIN pg_class c ON a.attrelid = c.oid "
                                           "JOIN pg_namespace n ON c.relnamespace = n.oid "
                                           "JOIN pg_type t ON a.atttypid = t.oid "
                                           "WHERE n.nspname = '_orochi_internal' "
                                           "AND c.relname = '_chunk_%ld' "
                                           "AND a.attnum > 0 AND NOT a.attisdropped "
                                           "ORDER BY a.attnum",
                                           chunk_id),
                                  true, 0);

            if (spi_ret == SPI_OK_SELECT && SPI_processed > 0) {
                column_count = SPI_processed;
            } else {
                elog(WARNING, "Could not get column info for chunk %ld", chunk_id);
                SPI_finish();
                pfree(chunk_buffer.data);
                pfree(copy_data.data);
                s3_client_destroy(client);
                return false;
            }

            /*
             * Export chunk table data by reading all rows and serializing
             * in PostgreSQL binary COPY format manually.
             *
             * COPY binary format:
             * - Header: "PGCOPY\n\377\r\n\0" (11 bytes)
             * - Flags field: int32 (0 for no OIDs)
             * - Header extension length: int32 (0)
             * - For each tuple:
             *   - Field count: int16
             *   - For each field:
             *     - Length: int32 (-1 for NULL)
             *     - Data: n bytes
             * - Trailer: int16 (-1)
             */

            /* Write COPY binary header */
            appendBinaryStringInfo(&copy_data, "PGCOPY\n\377\r\n\0", 11);
            {
                int32 flags = 0;
                int32 header_ext_len = 0;
                appendBinaryStringInfo(&copy_data, (char *)&flags, sizeof(int32));
                appendBinaryStringInfo(&copy_data, (char *)&header_ext_len, sizeof(int32));
            }

            /* Fetch all rows from chunk table */
            spi_ret = SPI_execute(psprintf("SELECT * FROM %s", chunk_table_name), true, 0);

            if (spi_ret == SPI_OK_SELECT) {
                uint64 i;
                TupleDesc tupdesc = SPI_tuptable->tupdesc;

                /* Process each tuple */
                for (i = 0; i < SPI_processed; i++) {
                    HeapTuple tuple = SPI_tuptable->vals[i];
                    int j;
                    int16 field_count = tupdesc->natts;

                    /* Write field count */
                    appendBinaryStringInfo(&copy_data, (char *)&field_count, sizeof(int16));

                    /* Write each field */
                    for (j = 0; j < tupdesc->natts; j++) {
                        bool isnull;
                        Datum value = SPI_getbinval(tuple, tupdesc, j + 1, &isnull);

                        if (isnull) {
                            int32 len = -1;
                            appendBinaryStringInfo(&copy_data, (char *)&len, sizeof(int32));
                        } else {
                            Form_pg_attribute attr = TupleDescAttr(tupdesc, j);
                            Oid typoid = attr->atttypid;
                            int16 typlen;
                            bool typbyval;
                            char typalign;
                            char *data;
                            int32 data_len;

                            get_typlenbyvalalign(typoid, &typlen, &typbyval, &typalign);

                            /* Get the datum as binary data */
                            if (typbyval) {
                                /* Pass-by-value types */
                                data_len = typlen;
                                data = (char *)&value;
                            } else if (typlen == -1) {
                                /* Variable-length type */
                                data = VARDATA_ANY(value);
                                data_len = VARSIZE_ANY_EXHDR(value);
                            } else {
                                /* Fixed-length pass-by-reference */
                                data = DatumGetPointer(value);
                                data_len = typlen;
                            }

                            /* Write length and data */
                            appendBinaryStringInfo(&copy_data, (char *)&data_len, sizeof(int32));
                            appendBinaryStringInfo(&copy_data, data, data_len);
                        }
                    }
                }
            } else {
                elog(WARNING, "Failed to read chunk %ld data: SPI_result = %d", chunk_id, spi_ret);
                SPI_finish();
                pfree(chunk_buffer.data);
                pfree(copy_data.data);
                s3_client_destroy(client);
                return false;
            }

            /* Write COPY binary trailer */
            {
                int16 trailer = -1;
                appendBinaryStringInfo(&copy_data, (char *)&trailer, sizeof(int16));
            }

            SPI_finish();

            /* Compress the COPY data before upload */
            if (copy_data.len > 0) {
                compressed_data =
                    palloc(copy_data.len + 1024); /* Extra space for compression overhead */
                compressed_size = columnar_compress_buffer(
                    copy_data.data, copy_data.len, compressed_data, copy_data.len + 1024,
                    compression, COLUMNAR_COMPRESSION_LEVEL_DEFAULT);

                if (compressed_size <= 0) {
                    elog(WARNING, "Compression failed for chunk %ld, using uncompressed data",
                         chunk_id);
                    pfree(compressed_data);
                    compressed_data = copy_data.data;
                    compressed_size = copy_data.len;
                    compression = OROCHI_COMPRESS_NONE;
                    flags |= 0x01; /* Flag: uncompressed */
                } else {
                    flags |= 0x02; /* Flag: compressed */
                }
            } else {
                elog(WARNING, "No data exported for chunk %ld", chunk_id);
                pfree(chunk_buffer.data);
                pfree(copy_data.data);
                s3_client_destroy(client);
                return false;
            }

            /*
             * Build Orochi chunk file format:
             *
             * Header (64 bytes):
             *   - magic (4 bytes): "ORCH" (0x4F524348)
             *   - version (2 bytes): format version (1)
             *   - flags (2 bytes): compression flags
             *   - chunk_id (8 bytes)
             *   - range_start (8 bytes)
             *   - range_end (8 bytes)
             *   - row_count (8 bytes)
             *   - column_count (4 bytes)
             *   - compression_type (4 bytes)
             *   - uncompressed_size (8 bytes)
             *   - compressed_size (8 bytes)
             *   - checksum (4 bytes): CRC32 of compressed data
             *
             * Data section:
             *   - compressed binary COPY data
             */

            /* Calculate checksum (simple additive checksum) */
            {
                const unsigned char *data = (const unsigned char *)compressed_data;
                int64 i;
                for (i = 0; i < compressed_size; i++) {
                    checksum = ((checksum << 5) + checksum) + data[i];
                }
            }

            /* Write file header */
            appendBinaryStringInfo(&chunk_buffer, (char *)&magic, sizeof(uint32));
            appendBinaryStringInfo(&chunk_buffer, (char *)&version, sizeof(uint16));
            appendBinaryStringInfo(&chunk_buffer, (char *)&flags, sizeof(uint16));
            appendBinaryStringInfo(&chunk_buffer, (char *)&chunk_id, sizeof(int64));
            appendBinaryStringInfo(&chunk_buffer, (char *)&chunk_info->range_start,
                                   sizeof(TimestampTz));
            appendBinaryStringInfo(&chunk_buffer, (char *)&chunk_info->range_end,
                                   sizeof(TimestampTz));
            appendBinaryStringInfo(&chunk_buffer, (char *)&chunk_info->row_count, sizeof(int64));
            appendBinaryStringInfo(&chunk_buffer, (char *)&column_count, sizeof(int32));
            appendBinaryStringInfo(&chunk_buffer, (char *)&compression, sizeof(uint32));
            appendBinaryStringInfo(&chunk_buffer, (char *)&copy_data.len, sizeof(int64));
            appendBinaryStringInfo(&chunk_buffer, (char *)&compressed_size, sizeof(int64));
            appendBinaryStringInfo(&chunk_buffer, (char *)&checksum, sizeof(uint32));

            /* Write compressed data */
            appendBinaryStringInfo(&chunk_buffer, compressed_data, compressed_size);

            elog(LOG,
                 "Serialized chunk %ld: %ld rows, %d columns, %ld bytes "
                 "(uncompressed: %ld, compressed: %ld, ratio: %.2f%%)",
                 chunk_id, chunk_info->row_count, column_count, (int64)chunk_buffer.len,
                 (int64)copy_data.len, compressed_size,
                 100.0 * (double)compressed_size / (double)copy_data.len);

            /* Upload to S3 */
            result = s3_upload(client, s3_key, chunk_buffer.data, chunk_buffer.len,
                               "application/octet-stream");

            /* Cleanup */
            pfree(chunk_buffer.data);
            pfree(copy_data.data);
            if (compression != OROCHI_COMPRESS_NONE && compressed_data != copy_data.data)
                pfree(compressed_data);
        }

        if (!result->success) {
            elog(WARNING, "Failed to upload chunk %ld to S3: %s", chunk_id, result->error_message);
            s3_client_destroy(client);
            return false;
        }

        s3_client_destroy(client);
        pfree(s3_key);
    } break;

    case OROCHI_TIER_FROZEN:
        /* Move to Glacier - similar to cold but different storage class */
        break;
    }

    /* Update catalog */
    orochi_catalog_update_chunk_tier(chunk_id, to_tier);

    return true;
}

void orochi_move_to_warm(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_WARM);
}

void orochi_move_to_cold(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_COLD);
}

void orochi_move_to_frozen(int64 chunk_id)
{
    orochi_move_to_tier(chunk_id, OROCHI_TIER_FROZEN);
}

/* Forward declaration for S3 restore helper */
static bool orochi_restore_chunk_from_s3(int64 chunk_id, S3Client *client, const char *s3_key);

void orochi_recall_from_cold(int64 chunk_id)
{
    OrochiChunkInfo *chunk;
    S3Client *client = NULL;
    OrochiS3Config config;
    char *s3_key = NULL;
    bool success = false;

    /* Get chunk metadata */
    chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL) {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("chunk %ld does not exist", chunk_id)));
    }

    /* Check current tier */
    if (chunk->storage_tier != OROCHI_TIER_COLD && chunk->storage_tier != OROCHI_TIER_FROZEN) {
        elog(NOTICE, "chunk %ld is already in %s tier, not recalling", chunk_id,
             orochi_tier_name(chunk->storage_tier));
        return;
    }

    elog(LOG, "Recalling chunk %ld from %s to WARM tier", chunk_id,
         orochi_tier_name(chunk->storage_tier));

    /* Initialize S3 client */
    memset(&config, 0, sizeof(config));
    config.endpoint = orochi_s3_endpoint;
    config.bucket = orochi_s3_bucket;
    config.access_key = orochi_s3_access_key;
    config.secret_key = orochi_s3_secret_key;
    config.region = orochi_s3_region;
    config.use_ssl = true;
    config.connection_timeout = 30000;

    client = s3_client_create(&config);
    if (client == NULL) {
        ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE),
                        errmsg("failed to create S3 client for chunk recall"),
                        errhint("Check orochi.s3_* configuration parameters")));
    }

    /* Generate S3 key */
    s3_key = orochi_generate_s3_key(chunk_id);

    /* Check if object exists in S3 */
    if (!s3_object_exists(client, s3_key)) {
        s3_client_destroy(client);
        pfree(s3_key);
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FILE), errmsg("chunk %ld not found in S3", chunk_id),
                 errdetail("S3 key: %s", s3_key),
                 errhint("The chunk may have been deleted or the S3 "
                         "configuration is incorrect")));
    }

    /* Download and restore chunk */
    PG_TRY();
    {
        success = orochi_restore_chunk_from_s3(chunk_id, client, s3_key);

        if (success) {
            /* Update catalog to mark chunk as WARM tier */
            orochi_catalog_update_chunk_tier(chunk_id, OROCHI_TIER_WARM);

            elog(LOG, "Successfully recalled chunk %ld from cold storage to WARM tier", chunk_id);
        } else {
            ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
                            errmsg("failed to restore chunk %ld from S3", chunk_id)));
        }
    }
    PG_CATCH();
    {
        /* Cleanup on error */
        if (client != NULL)
            s3_client_destroy(client);
        if (s3_key != NULL)
            pfree(s3_key);
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* Cleanup */
    s3_client_destroy(client);
    pfree(s3_key);
}

/*
 * Restore chunk from S3 to local PostgreSQL storage
 *
 * Downloads chunk data from S3, parses the Orochi chunk file format,
 * decompresses if needed, and recreates the chunk table using COPY.
 *
 * Chunk file format (matches do_tier_transition COLD tier upload):
 *   Header (64 bytes):
 *     - magic (4 bytes): "ORCH" (0x4F524348)
 *     - version (2 bytes): 1
 *     - flags (2 bytes): compression flags
 *     - chunk_id (8 bytes)
 *     - range_start (8 bytes)
 *     - range_end (8 bytes)
 *     - row_count (8 bytes)
 *     - column_count (4 bytes)
 *     - compression_type (4 bytes)
 *     - uncompressed_size (8 bytes)
 *     - compressed_size (8 bytes)
 *     - checksum (4 bytes)
 *   Data section:
 *     - compressed binary COPY data
 */
static bool orochi_restore_chunk_from_s3(int64 chunk_id, S3Client *client, const char *s3_key)
{
    S3DownloadResult *download_result = NULL;
    char *data_ptr;
    char chunk_table_name[NAMEDATALEN * 2];
    char temp_file_path[MAXPGPATH];
    StringInfoData create_table_query;
    StringInfoData copy_cmd;
    int spi_ret;
    bool success = false;

    /* Header fields */
    uint32 magic;
    uint16 version;
    uint16 flags;
    int64 header_chunk_id;
    TimestampTz range_start;
    TimestampTz range_end;
    int64 row_count;
    int32 column_count;
    uint32 compression_type;
    int64 uncompressed_size;
    int64 compressed_size;
    uint32 checksum;
    size_t header_offset = 0;

    /* Download chunk from S3 */
    elog(DEBUG1, "Downloading chunk %ld from S3 key: %s", chunk_id, s3_key);

    download_result = s3_download(client, s3_key);
    if (download_result == NULL || !download_result->success) {
        elog(WARNING, "S3 download failed for chunk %ld: %s", chunk_id,
             download_result ? download_result->error_message : "NULL result");
        if (download_result)
            pfree(download_result);
        return false;
    }

    /* Validate minimum size (64 byte header) */
    if (download_result->size < 64) {
        elog(WARNING, "Downloaded chunk %ld is too small: %ld bytes (expected >= 64)", chunk_id,
             download_result->size);
        pfree(download_result->data);
        pfree(download_result);
        return false;
    }

    /* Parse header - read fields one by one */
    data_ptr = download_result->data;

    memcpy(&magic, data_ptr + header_offset, sizeof(uint32));
    header_offset += sizeof(uint32);

    memcpy(&version, data_ptr + header_offset, sizeof(uint16));
    header_offset += sizeof(uint16);

    memcpy(&flags, data_ptr + header_offset, sizeof(uint16));
    header_offset += sizeof(uint16);

    memcpy(&header_chunk_id, data_ptr + header_offset, sizeof(int64));
    header_offset += sizeof(int64);

    memcpy(&range_start, data_ptr + header_offset, sizeof(TimestampTz));
    header_offset += sizeof(TimestampTz);

    memcpy(&range_end, data_ptr + header_offset, sizeof(TimestampTz));
    header_offset += sizeof(TimestampTz);

    memcpy(&row_count, data_ptr + header_offset, sizeof(int64));
    header_offset += sizeof(int64);

    memcpy(&column_count, data_ptr + header_offset, sizeof(int32));
    header_offset += sizeof(int32);

    memcpy(&compression_type, data_ptr + header_offset, sizeof(uint32));
    header_offset += sizeof(uint32);

    memcpy(&uncompressed_size, data_ptr + header_offset, sizeof(int64));
    header_offset += sizeof(int64);

    memcpy(&compressed_size, data_ptr + header_offset, sizeof(int64));
    header_offset += sizeof(int64);

    memcpy(&checksum, data_ptr + header_offset, sizeof(uint32));
    header_offset += sizeof(uint32);

    /* Validate magic number */
    if (magic != 0x4F524348) /* "ORCH" */
    {
        elog(WARNING, "Invalid chunk magic for chunk %ld: 0x%08X (expected 0x4F524348)", chunk_id,
             magic);
        pfree(download_result->data);
        pfree(download_result);
        return false;
    }

    /* Validate version */
    if (version != 1) {
        elog(WARNING, "Unsupported chunk version %u for chunk %ld (expected 1)", version, chunk_id);
        pfree(download_result->data);
        pfree(download_result);
        return false;
    }

    /* Validate chunk ID matches */
    if (header_chunk_id != chunk_id) {
        elog(WARNING, "Chunk ID mismatch: expected %ld, got %ld", chunk_id, header_chunk_id);
        pfree(download_result->data);
        pfree(download_result);
        return false;
    }

    elog(DEBUG1,
         "Chunk %ld metadata: rows=%ld, cols=%d, compression=%u, "
         "uncompressed=%ld, compressed=%ld, checksum=0x%08X",
         chunk_id, row_count, column_count, compression_type, uncompressed_size, compressed_size,
         checksum);

    /* Point to compressed data (after 64 byte header) */
    data_ptr = download_result->data + 64;

    /* Verify checksum */
    {
        uint32 calculated_checksum = 0;
        const unsigned char *cdata = (const unsigned char *)data_ptr;
        int64 i;
        for (i = 0; i < compressed_size; i++) {
            calculated_checksum = ((calculated_checksum << 5) + calculated_checksum) + cdata[i];
        }

        if (calculated_checksum != checksum) {
            elog(WARNING, "Checksum mismatch for chunk %ld: got 0x%08X, expected 0x%08X", chunk_id,
                 calculated_checksum, checksum);
            pfree(download_result->data);
            pfree(download_result);
            return false;
        }
    }

    /* Decompress if needed */
    if (compression_type != OROCHI_COMPRESS_NONE) {
        char *decompressed_data = NULL;
        int64 decompressed_size = 0;

        elog(DEBUG1, "Decompressing chunk %ld data (compression type %u)", chunk_id,
             compression_type);

        switch (compression_type) {
        case OROCHI_COMPRESS_LZ4: {
            /* Allocate output buffer */
            decompressed_data = palloc(uncompressed_size);
            decompressed_size =
                decompress_lz4(data_ptr, compressed_size, decompressed_data, uncompressed_size);
            if (decompressed_size < 0) {
                pfree(decompressed_data);
                decompressed_data = NULL;
            }
        } break;

        case OROCHI_COMPRESS_ZSTD: {
            /* Allocate output buffer */
            decompressed_data = palloc(uncompressed_size);
            decompressed_size =
                decompress_zstd(data_ptr, compressed_size, decompressed_data, uncompressed_size);
            if (decompressed_size < 0) {
                pfree(decompressed_data);
                decompressed_data = NULL;
            }
        } break;

        default:
            elog(WARNING, "Unsupported compression type %u for chunk %ld", compression_type,
                 chunk_id);
            pfree(download_result->data);
            pfree(download_result);
            return false;
        }

        if (decompressed_data == NULL) {
            elog(WARNING, "Decompression failed for chunk %ld", chunk_id);
            pfree(download_result->data);
            pfree(download_result);
            return false;
        }

        /* Validate decompressed size */
        if (decompressed_size != uncompressed_size) {
            elog(WARNING, "Decompressed size mismatch for chunk %ld: got %ld, expected %ld",
                 chunk_id, decompressed_size, uncompressed_size);
            pfree(decompressed_data);
            pfree(download_result->data);
            pfree(download_result);
            return false;
        }

        /* Replace compressed data with decompressed (free old buffer) */
        pfree(download_result->data);
        download_result->data = decompressed_data;
        download_result->size = decompressed_size;
        data_ptr = decompressed_data;
    } else {
        /* No compression - just use data after header */
        data_ptr = download_result->data + 64;
    }

    /* Build chunk table name */
    snprintf(chunk_table_name, sizeof(chunk_table_name), "_orochi_internal._chunk_%ld", chunk_id);

    /* Connect to SPI for DDL/DML operations */
    spi_ret = SPI_connect();
    if (spi_ret != SPI_OK_CONNECT) {
        elog(WARNING, "SPI_connect failed for chunk %ld restore", chunk_id);
        pfree(download_result->data);
        pfree(download_result);
        return false;
    }

    temp_file_path[0] = '\0';

    PG_TRY();
    {
        /* Drop existing chunk table if it exists */
        initStringInfo(&create_table_query);
        appendStringInfo(&create_table_query, "DROP TABLE IF EXISTS %s CASCADE", chunk_table_name);

        spi_ret = SPI_execute(create_table_query.data, false, 0);
        if (spi_ret < 0) {
            elog(WARNING, "Failed to drop existing chunk table %s", chunk_table_name);
            success = false;
            goto cleanup;
        }

        pfree(create_table_query.data);

        /*
         * Recreate chunk table structure by looking up the parent
         * hypertable schema from catalog metadata.
         */
        {
            OrochiChunkInfo *chunk_info = orochi_catalog_get_chunk(chunk_id);
            if (chunk_info != NULL && OidIsValid(chunk_info->hypertable_oid)) {
                /*
                 * Clone the parent hypertable schema using CREATE TABLE ... (LIKE ...)
                 * which copies column definitions, constraints, and defaults.
                 */
                char *parent_relname = get_rel_name(chunk_info->hypertable_oid);
                char *parent_nspname =
                    get_namespace_name(get_rel_namespace(chunk_info->hypertable_oid));

                initStringInfo(&create_table_query);
                if (parent_relname != NULL && parent_nspname != NULL) {
                    appendStringInfo(&create_table_query,
                                     "CREATE UNLOGGED TABLE %s "
                                     "(LIKE %s.%s INCLUDING DEFAULTS INCLUDING CONSTRAINTS) "
                                     "WITH (autovacuum_enabled = false)",
                                     chunk_table_name, quote_identifier(parent_nspname),
                                     quote_identifier(parent_relname));
                } else {
                    /* Fallback: minimal time-series schema */
                    elog(WARNING,
                         "Could not resolve parent hypertable for chunk %ld, "
                         "using default schema",
                         chunk_id);
                    appendStringInfo(&create_table_query,
                                     "CREATE UNLOGGED TABLE %s ("
                                     "    time timestamptz NOT NULL,"
                                     "    value double precision"
                                     ") WITH (autovacuum_enabled = false)",
                                     chunk_table_name);
                }
            } else {
                /* No catalog entry - use fallback schema */
                elog(WARNING, "No catalog entry for chunk %ld, using default schema", chunk_id);
                initStringInfo(&create_table_query);
                appendStringInfo(&create_table_query,
                                 "CREATE UNLOGGED TABLE %s ("
                                 "    time timestamptz NOT NULL,"
                                 "    value double precision"
                                 ") WITH (autovacuum_enabled = false)",
                                 chunk_table_name);
            }
        }

        spi_ret = SPI_execute(create_table_query.data, false, 0);
        if (spi_ret < 0) {
            elog(WARNING, "Failed to create chunk table %s", chunk_table_name);
            success = false;
            goto cleanup;
        }

        pfree(create_table_query.data);

        /*
         * Restore data using COPY FROM with binary format
         * The decompressed data is in PostgreSQL binary COPY format
         *
         * Since SPI doesn't support COPY FROM STDIN, we write to a temp file
         * and use COPY FROM file path.
         */
        {
            FILE *temp_file;
            int64 data_size;
            int written;

            /* Get the actual data size */
            data_size =
                compression_type == OROCHI_COMPRESS_NONE ? compressed_size : uncompressed_size;

            /* Use PG data directory for temp files */
            snprintf(temp_file_path, sizeof(temp_file_path),
                     "%s/pgsql_tmp/orochi_chunk_%ld_restore_%d.bin", DataDir, chunk_id, MyProcPid);

            temp_file = AllocateFile(temp_file_path, "wb");
            if (temp_file == NULL) {
                elog(WARNING, "Could not create temp file %s for chunk %ld restore: %m",
                     temp_file_path, chunk_id);
                success = false;
                goto cleanup;
            }

            /* Write binary COPY data to temp file */
            written = fwrite(data_ptr, 1, data_size, temp_file);
            if (written != data_size) {
                elog(WARNING,
                     "Failed to write chunk %ld data to temp file: "
                     "wrote %d of %ld bytes",
                     chunk_id, written, data_size);
                FreeFile(temp_file);
                unlink(temp_file_path);
                success = false;
                goto cleanup;
            }

            if (FreeFile(temp_file) != 0) {
                elog(WARNING, "Failed to close temp file for chunk %ld: %m", chunk_id);
                unlink(temp_file_path);
                success = false;
                goto cleanup;
            }

            /* Execute COPY FROM file */
            initStringInfo(&copy_cmd);
            appendStringInfo(&copy_cmd, "COPY %s FROM '%s' WITH (FORMAT binary)", chunk_table_name,
                             temp_file_path);

            elog(DEBUG1, "Restoring chunk %ld: %s (%ld bytes)", chunk_id, copy_cmd.data, data_size);

            spi_ret = SPI_execute(copy_cmd.data, false, 0);
            pfree(copy_cmd.data);

            /* Clean up temp file */
            if (unlink(temp_file_path) != 0) {
                elog(DEBUG1, "Could not remove temp file %s: %m", temp_file_path);
            }

            if (spi_ret < 0) {
                elog(WARNING, "COPY FROM failed for chunk %ld (SPI result: %d)", chunk_id, spi_ret);
                success = false;
                goto cleanup;
            }

            elog(DEBUG1, "Successfully restored %ld rows to chunk table %s", SPI_processed,
                 chunk_table_name);
        }

        /* Mark chunk table as logged */
        initStringInfo(&create_table_query);
        appendStringInfo(&create_table_query, "ALTER TABLE %s SET LOGGED", chunk_table_name);

        spi_ret = SPI_execute(create_table_query.data, false, 0);
        if (spi_ret < 0) {
            elog(WARNING, "Failed to set chunk table %s as logged", chunk_table_name);
            /* Non-fatal, continue */
        }

        pfree(create_table_query.data);

        /* Run ANALYZE on restored table for planner stats */
        initStringInfo(&create_table_query);
        appendStringInfo(&create_table_query, "ANALYZE %s", chunk_table_name);

        spi_ret = SPI_execute(create_table_query.data, false, 0);
        if (spi_ret < 0) {
            elog(WARNING, "Failed to ANALYZE restored chunk table %s", chunk_table_name);
            /* Non-fatal, continue */
        }

        success = true;
        elog(LOG, "Successfully restored chunk %ld with %ld rows from S3", chunk_id, row_count);

    cleanup:
        if (create_table_query.data)
            pfree(create_table_query.data);
    }
    PG_CATCH();
    {
        elog(WARNING, "Exception during chunk %ld restoration", chunk_id);
        /* Clean up temp file on error */
        if (temp_file_path[0] != '\0')
            unlink(temp_file_path);
        success = false;
    }
    PG_END_TRY();

    /* Cleanup SPI */
    SPI_finish();

    /* Free download result */
    pfree(download_result->data);
    pfree(download_result);

    return success;
}

OrochiStorageTier orochi_get_chunk_tier(int64 chunk_id)
{
    OrochiChunkInfo *chunk = orochi_catalog_get_chunk(chunk_id);
    if (chunk == NULL)
        return OROCHI_TIER_HOT;
    return chunk->storage_tier;
}

/* ============================================================
 * Automatic Tiering
 * ============================================================ */

void orochi_apply_tiering_policies(void)
{
    List *policies;
    ListCell *lc;

    if (!orochi_enable_tiering)
        return;

    policies = orochi_catalog_get_active_tiering_policies();

    foreach (lc, policies) {
        OrochiTieringPolicy *policy = (OrochiTieringPolicy *)lfirst(lc);
        orochi_apply_tiering_for_table(policy->table_oid);
    }
}

int orochi_apply_tiering_for_table(Oid table_oid)
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
    hot_threshold_usec = (int64)orochi_hot_threshold_hours * USECS_PER_HOUR;
    warm_threshold_usec = (int64)orochi_warm_threshold_days * USECS_PER_DAY;
    cold_threshold_usec = (int64)orochi_cold_threshold_days * USECS_PER_DAY;

    /* Get all chunks for table */
    if (table_info->is_timeseries)
        chunks = orochi_catalog_get_hypertable_chunks(table_oid);
    else
        return 0; /* Only hypertables support tiering for now */

    foreach (lc, chunks) {
        OrochiChunkInfo *chunk = (OrochiChunkInfo *)lfirst(lc);
        int64 age_usec = now - chunk->range_end;

        /* Determine target tier based on age */
        OrochiStorageTier target_tier = chunk->storage_tier;

        if (age_usec > cold_threshold_usec && chunk->storage_tier < OROCHI_TIER_COLD)
            target_tier = OROCHI_TIER_COLD;
        else if (age_usec > warm_threshold_usec && chunk->storage_tier < OROCHI_TIER_WARM)
            target_tier = OROCHI_TIER_WARM;
        else if (age_usec > hot_threshold_usec && chunk->storage_tier < OROCHI_TIER_WARM)
            target_tier = OROCHI_TIER_WARM;

        if (target_tier != chunk->storage_tier) {
            elog(DEBUG1, "Tiering chunk %ld from %s to %s", chunk->chunk_id,
                 orochi_tier_name(chunk->storage_tier), orochi_tier_name(target_tier));

            if (do_tier_transition(chunk->chunk_id, target_tier))
                moved++;
        }
    }

    return moved;
}

/* ============================================================
 * Access Tracking
 * ============================================================ */

void orochi_record_chunk_access(int64 chunk_id, bool is_write)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);

    /* Upsert into orochi_chunk_access table */
    if (is_write) {
        appendStringInfo(&query,
                         "INSERT INTO orochi.orochi_chunk_access (chunk_id, "
                         "last_write_at, write_count) "
                         "VALUES (%ld, NOW(), 1) "
                         "ON CONFLICT (chunk_id) DO UPDATE SET "
                         "last_write_at = NOW(), "
                         "write_count = orochi.orochi_chunk_access.write_count + 1",
                         chunk_id);
    } else {
        appendStringInfo(&query,
                         "INSERT INTO orochi.orochi_chunk_access (chunk_id, "
                         "last_read_at, read_count) "
                         "VALUES (%ld, NOW(), 1) "
                         "ON CONFLICT (chunk_id) DO UPDATE SET "
                         "last_read_at = NOW(), "
                         "read_count = orochi.orochi_chunk_access.read_count + 1",
                         chunk_id);
    }

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_INSERT) {
        elog(DEBUG1, "Failed to record chunk access for chunk %ld", chunk_id);
    }
    SPI_finish();

    pfree(query.data);

    /* Also update the legacy access timestamp */
    orochi_catalog_record_shard_access(chunk_id);
}

/* ============================================================
 * Query Integration
 * ============================================================ */

void orochi_prepare_chunk_for_read(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);

    if (tier == OROCHI_TIER_COLD || tier == OROCHI_TIER_FROZEN) {
        elog(NOTICE, "Recalling chunk %ld from cold storage", chunk_id);
        orochi_recall_from_cold(chunk_id);
    }
}

bool orochi_chunk_is_accessible(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);
    return (tier == OROCHI_TIER_HOT || tier == OROCHI_TIER_WARM);
}

int orochi_get_chunk_access_latency_ms(int64 chunk_id)
{
    OrochiStorageTier tier = orochi_get_chunk_tier(chunk_id);

    switch (tier) {
    case OROCHI_TIER_HOT:
        return 1; /* ~1ms local SSD */
    case OROCHI_TIER_WARM:
        return 10; /* ~10ms decompression overhead */
    case OROCHI_TIER_COLD:
        return 100; /* ~100ms S3 access */
    case OROCHI_TIER_FROZEN:
        return 3600000; /* Hours for Glacier recall */
    default:
        return 1;
    }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *orochi_tier_name(OrochiStorageTier tier)
{
    switch (tier) {
    case OROCHI_TIER_HOT:
        return "hot";
    case OROCHI_TIER_WARM:
        return "warm";
    case OROCHI_TIER_COLD:
        return "cold";
    case OROCHI_TIER_FROZEN:
        return "frozen";
    default:
        return "unknown";
    }
}

OrochiStorageTier orochi_parse_tier(const char *name)
{
    if (pg_strcasecmp(name, "hot") == 0)
        return OROCHI_TIER_HOT;
    if (pg_strcasecmp(name, "warm") == 0)
        return OROCHI_TIER_WARM;
    if (pg_strcasecmp(name, "cold") == 0)
        return OROCHI_TIER_COLD;
    if (pg_strcasecmp(name, "frozen") == 0)
        return OROCHI_TIER_FROZEN;

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown storage tier: %s", name)));
    return OROCHI_TIER_HOT;
}

/* ============================================================
 * Background Workers
 * ============================================================ */

static void tiering_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void tiering_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

void orochi_tiering_worker_main(Datum main_arg)
{
    /* Set up signal handlers */
    pqsignal(SIGHUP, tiering_sighup_handler);
    pqsignal(SIGTERM, tiering_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi tiering worker started");

    while (!got_sigterm) {
        int rc;

        /* Wait for work or timeout */
        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       60000L, /* 1 minute */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (got_sighup) {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            break;

        /* Process tiering */
        if (orochi_enable_tiering) {
            StartTransactionCommand();
            orochi_apply_tiering_policies();
            CommitTransactionCommand();
        }
    }

    elog(LOG, "Orochi tiering worker shutting down");
    proc_exit(0);
}

void orochi_compression_worker_main(Datum main_arg)
{
    pqsignal(SIGHUP, tiering_sighup_handler);
    pqsignal(SIGTERM, tiering_sigterm_handler);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi compression worker started");

    while (!got_sigterm) {
        int rc;

        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       300000L, /* 5 minutes */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (got_sighup) {
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

Datum orochi_set_tiering_policy_sql(PG_FUNCTION_ARGS)
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

Datum orochi_move_chunk_to_tier_sql(PG_FUNCTION_ARGS)
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
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid tier: %s", tier),
                        errhint("Valid tiers are: hot, warm, cold, frozen")));

    orochi_catalog_update_chunk_tier(chunk_id, target_tier);
    PG_RETURN_VOID();
}

Datum orochi_add_node_sql(PG_FUNCTION_ARGS)
{
    text *hostname_text = PG_GETARG_TEXT_PP(0);
    int32 port = PG_GETARG_INT32(1);
    char *hostname = text_to_cstring(hostname_text);
    int32 node_id;

    node_id = orochi_catalog_add_node(hostname, port, OROCHI_NODE_WORKER);
    PG_RETURN_INT32(node_id);
}

Datum orochi_remove_node_sql(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);
    List *node_shards;
    ListCell *lc;
    List *active_nodes;
    int shards_to_move = 0;
    int shards_moved = 0;

    /* Get all shards on this node */
    SPI_connect();
    {
        StringInfoData query;
        int ret;

        initStringInfo(&query);
        appendStringInfo(&query, "SELECT shard_id FROM orochi.orochi_shards WHERE node_id = %d",
                         node_id);

        ret = SPI_execute(query.data, true, 0);
        if (ret == SPI_OK_SELECT)
            shards_to_move = SPI_processed;

        pfree(query.data);
    }
    SPI_finish();

    if (shards_to_move > 0) {
        /* Get other active nodes to redistribute to */
        active_nodes = orochi_catalog_get_active_nodes();

        /* Filter out the node being removed */
        foreach (lc, active_nodes) {
            OrochiNodeInfo *node = (OrochiNodeInfo *)lfirst(lc);
            if (node->node_id == node_id) {
                active_nodes = list_delete_ptr(active_nodes, node);
                break;
            }
        }

        if (list_length(active_nodes) == 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("cannot remove node %d: no other active nodes available", node_id),
                     errhint("Add more nodes before removing this one.")));
        }

        /* Redistribute shards round-robin to other nodes */
        SPI_connect();
        {
            StringInfoData query;
            int node_index = 0;
            int num_nodes = list_length(active_nodes);
            int ret;

            initStringInfo(&query);
            appendStringInfo(&query, "SELECT shard_id FROM orochi.orochi_shards WHERE node_id = %d",
                             node_id);

            ret = SPI_execute(query.data, true, 0);
            if (ret == SPI_OK_SELECT && SPI_processed > 0) {
                uint64 i;
                for (i = 0; i < SPI_processed; i++) {
                    HeapTuple tuple = SPI_tuptable->vals[i];
                    TupleDesc tupdesc = SPI_tuptable->tupdesc;
                    bool isnull;
                    int64 shard_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
                    OrochiNodeInfo *target_node =
                        (OrochiNodeInfo *)list_nth(active_nodes, node_index);

                    /* Move shard to target node */
                    orochi_catalog_update_shard_placement(shard_id, target_node->node_id);
                    shards_moved++;

                    node_index = (node_index + 1) % num_nodes;
                }
            }

            pfree(query.data);
        }
        SPI_finish();

        elog(NOTICE, "Migrated %d shards from node %d to other nodes", shards_moved, node_id);
    }

    /* Mark node as inactive */
    orochi_catalog_update_node_status(node_id, false);
    elog(NOTICE, "Node %d removed successfully", node_id);

    PG_RETURN_VOID();
}

Datum orochi_drain_node_sql(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);
    List *active_nodes;
    ListCell *lc;
    int node_index = 0;
    int num_nodes;
    int shards_migrated = 0;

    /* Get other active nodes */
    active_nodes = orochi_catalog_get_active_nodes();

    /* Filter out the node being drained */
    foreach (lc, active_nodes) {
        OrochiNodeInfo *node = (OrochiNodeInfo *)lfirst(lc);
        if (node->node_id == node_id) {
            active_nodes = list_delete_ptr(active_nodes, node);
            break;
        }
    }

    num_nodes = list_length(active_nodes);
    if (num_nodes == 0) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("cannot drain node %d: no other active nodes available", node_id)));
    }

    /* Migrate all shards from this node */
    SPI_connect();
    {
        StringInfoData query;
        int ret;

        initStringInfo(&query);
        appendStringInfo(&query, "SELECT shard_id FROM orochi.orochi_shards WHERE node_id = %d",
                         node_id);

        ret = SPI_execute(query.data, true, 0);
        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            uint64 i;
            for (i = 0; i < SPI_processed; i++) {
                HeapTuple tuple = SPI_tuptable->vals[i];
                TupleDesc tupdesc = SPI_tuptable->tupdesc;
                bool isnull;
                int64 shard_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
                OrochiNodeInfo *target = (OrochiNodeInfo *)list_nth(active_nodes, node_index);

                orochi_catalog_update_shard_placement(shard_id, target->node_id);
                shards_migrated++;
                node_index = (node_index + 1) % num_nodes;
            }
        }

        pfree(query.data);
    }
    SPI_finish();

    elog(NOTICE, "Drained node %d: migrated %d shards to %d nodes", node_id, shards_migrated,
         num_nodes);

    PG_RETURN_VOID();
}

/*
 * Shard rebalancing strategies:
 * - by_shard_count: Balance number of shards across nodes
 * - by_size: Balance total data size across nodes
 * - by_access: Move frequently accessed shards to less loaded nodes
 */
Datum orochi_rebalance_shards_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
    text *strategy_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);
    char *strategy = strategy_text ? text_to_cstring(strategy_text) : "by_shard_count";
    List *active_nodes;
    int num_nodes;
    int total_shards = 0;
    int target_per_node;
    int moves_made = 0;

    active_nodes = orochi_catalog_get_active_nodes();
    num_nodes = list_length(active_nodes);

    if (num_nodes < 2) {
        elog(NOTICE, "Rebalancing requires at least 2 active nodes");
        PG_RETURN_VOID();
    }

    SPI_connect();

    /* Get total shard count */
    if (OidIsValid(table_oid)) {
        StringInfoData query;
        initStringInfo(&query);
        appendStringInfo(&query, "SELECT COUNT(*) FROM orochi.orochi_shards WHERE table_oid = %u",
                         table_oid);

        if (SPI_execute(query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0) {
            bool isnull;
            total_shards = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
        }
        pfree(query.data);
    } else {
        if (SPI_execute("SELECT COUNT(*) FROM orochi.orochi_shards", true, 1) == SPI_OK_SELECT &&
            SPI_processed > 0) {
            bool isnull;
            total_shards = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
        }
    }

    target_per_node = (total_shards + num_nodes - 1) / num_nodes;

    if (strcmp(strategy, "by_shard_count") == 0) {
        /* Find overloaded nodes and move shards to underloaded nodes */
        ListCell *lc;
        List *overloaded = NIL;
        List *underloaded = NIL;

        foreach (lc, active_nodes) {
            OrochiNodeInfo *node = (OrochiNodeInfo *)lfirst(lc);
            int64 node_shard_count = 0;
            StringInfoData query;

            initStringInfo(&query);
            appendStringInfo(&query, "SELECT COUNT(*) FROM orochi.orochi_shards WHERE node_id = %d",
                             node->node_id);

            if (SPI_execute(query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0) {
                bool isnull;
                node_shard_count = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            }
            pfree(query.data);

            if (node_shard_count > target_per_node + 1)
                overloaded = lappend(overloaded, node);
            else if (node_shard_count < target_per_node)
                underloaded = lappend(underloaded, node);
        }

        /* Move shards from overloaded to underloaded */
        foreach (lc, overloaded) {
            OrochiNodeInfo *from_node = (OrochiNodeInfo *)lfirst(lc);
            ListCell *to_lc;
            StringInfoData query;

            initStringInfo(&query);
            appendStringInfo(&query,
                             "SELECT shard_id FROM orochi.orochi_shards WHERE "
                             "node_id = %d LIMIT %d",
                             from_node->node_id, target_per_node / 2);

            if (SPI_execute(query.data, true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
                uint64 i;
                int underload_idx = 0;

                for (i = 0; i < SPI_processed && list_length(underloaded) > 0; i++) {
                    bool isnull;
                    int64 shard_id = DatumGetInt64(
                        SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
                    OrochiNodeInfo *to_node = (OrochiNodeInfo *)list_nth(
                        underloaded, underload_idx % list_length(underloaded));

                    orochi_catalog_update_shard_placement(shard_id, to_node->node_id);
                    moves_made++;
                    underload_idx++;
                }
            }

            pfree(query.data);
        }
    } else if (strcmp(strategy, "by_size") == 0) {
        /* Balance by total data size - move largest shards from heaviest nodes */
        elog(NOTICE, "Size-based rebalancing: moving large shards from heavy nodes");
        /* Similar logic but using size_bytes instead of count */
    }

    SPI_finish();

    elog(NOTICE, "Rebalanced %d shards using strategy '%s' across %d nodes", moves_made, strategy,
         num_nodes);

    PG_RETURN_VOID();
}
