/*-------------------------------------------------------------------------
 *
 * s3_source.c
 *    Orochi DB S3 file watcher implementation for data pipelines
 *
 * This module provides S3 bucket monitoring and file ingestion
 * with tracking of processed files to prevent duplicate processing.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include <curl/curl.h>
#include <time.h>

#include "../orochi.h"
#include "../tiered/tiered_storage.h"
#include "pipeline.h"

/* ============================================================
 * S3 File Info Structure
 * ============================================================ */

typedef struct S3FileInfo {
    char *key;                 /* S3 object key */
    int64 size;                /* File size in bytes */
    TimestampTz last_modified; /* Last modification time */
    char *etag;                /* ETag for change detection */
    bool is_processed;         /* Has been processed */
} S3FileInfo;

/* ============================================================
 * S3 Source Context
 * ============================================================ */

typedef struct S3SourceContext {
    int64 pipeline_id;
    S3SourceConfig *config;
    S3Client *client;

    /* State tracking */
    TimestampTz last_poll_time;
    int64 files_discovered;
    int64 files_processed;
    int64 bytes_processed;

    /* Error tracking */
    char last_error[PIPELINE_MAX_ERROR_MESSAGE];
    int consecutive_errors;

    /* Memory context */
    MemoryContext context;
} S3SourceContext;

/* Forward declarations */
static List *parse_s3_list_response(const char *xml_response, int64 size);
static bool matches_pattern(const char *filename, const char *pattern);
static char *extract_filename(const char *key);

/* ============================================================
 * S3 Source Initialization
 * ============================================================ */

/*
 * s3_source_init
 *    Initialize S3 source with given configuration
 */
S3Client *s3_source_init(S3SourceConfig *config)
{
    S3Client *client;
    OrochiS3Config s3_config;

    if (config == NULL) {
        elog(ERROR, "S3 source configuration is required");
        return NULL;
    }

    if (config->bucket == NULL) {
        elog(ERROR, "S3 bucket name is required");
        return NULL;
    }

    /* Convert S3SourceConfig to OrochiS3Config */
    memset(&s3_config, 0, sizeof(s3_config));
    s3_config.bucket = config->bucket;
    s3_config.region = config->region ? config->region : "us-east-1";
    s3_config.access_key = config->access_key;
    s3_config.secret_key = config->secret_key;
    s3_config.use_ssl = config->use_ssl;

    /* Determine endpoint */
    if (config->endpoint) {
        s3_config.endpoint = config->endpoint;
    } else {
        /* Default to AWS S3 endpoint for region */
        char endpoint[256];
        snprintf(endpoint, sizeof(endpoint), "s3.%s.amazonaws.com", s3_config.region);
        s3_config.endpoint = pstrdup(endpoint);
    }

    /* Create S3 client using tiered storage infrastructure */
    client = s3_client_create(&s3_config);

    if (client == NULL) {
        elog(ERROR, "Failed to create S3 client for bucket %s", config->bucket);
        return NULL;
    }

    /* Test connection */
    if (!s3_client_test_connection(client)) {
        elog(WARNING, "S3 connection test failed, but continuing...");
    }

    elog(LOG, "S3 source initialized: bucket=%s, prefix=%s, region=%s", config->bucket,
         config->prefix ? config->prefix : "(none)", s3_config.region);

    return client;
}

/* ============================================================
 * S3 File Discovery
 * ============================================================ */

/*
 * s3_source_list_new_files
 *    List files in S3 bucket that haven't been processed
 */
List *s3_source_list_new_files(S3Client *client, const char *prefix, const char *pattern,
                               TimestampTz since)
{
    List *result = NIL;
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char url[1024];
    char uri[512];
    char query_string[256];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;

    /* Response buffer */
    typedef struct {
        char *data;
        size_t size;
        size_t capacity;
    } ResponseBuffer;

    ResponseBuffer response;

    if (client == NULL)
        return NIL;

    /* Initialize response buffer */
    response.data = palloc(4096);
    response.size = 0;
    response.capacity = 4096;

    /* Build query string */
    if (prefix && strlen(prefix) > 0)
        snprintf(query_string, sizeof(query_string), "list-type=2&prefix=%s", prefix);
    else
        snprintf(query_string, sizeof(query_string), "list-type=2");

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build URI */
    snprintf(uri, sizeof(uri), "/%s", client->bucket);

    /* Build full URL */
    snprintf(url, sizeof(url), "%s://%s%s?%s", client->use_ssl ? "https" : "http", client->endpoint,
             uri, query_string);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "GET", uri, query_string, "", 0, date_stamp,
                                             amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(response.data);
        return NIL;
    }

    /* Build headers */
    snprintf(header_buf, sizeof(header_buf), "Host: %s", client->endpoint);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-date: %s", amz_date);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "x-amz-content-sha256: %s", payload_hash);
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, header_buf);

    /* Write callback */
    static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        size_t realsize = size * nmemb;
        ResponseBuffer *buf = (ResponseBuffer *)userp;

        if (buf->size + realsize + 1 > buf->capacity) {
            size_t new_capacity = buf->capacity * 2;
            if (new_capacity < buf->size + realsize + 1)
                new_capacity = buf->size + realsize + 1 + 4096;
            buf->data = repalloc(buf->data, new_capacity);
            buf->capacity = new_capacity;
        }

        memcpy(&(buf->data[buf->size]), contents, realsize);
        buf->size += realsize;
        buf->data[buf->size] = 0;

        return realsize;
    }

    /* Configure CURL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Execute request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200) {
        /* Parse XML response */
        result = parse_s3_list_response(response.data, response.size);

        /* Filter by pattern and modification time */
        if (result != NIL) {
            List *filtered = NIL;
            ListCell *lc;

            foreach (lc, result) {
                S3FileInfo *file = (S3FileInfo *)lfirst(lc);

                /* Check pattern match */
                if (pattern && strlen(pattern) > 0) {
                    char *filename = extract_filename(file->key);
                    if (!matches_pattern(filename, pattern)) {
                        pfree(file->key);
                        if (file->etag)
                            pfree(file->etag);
                        pfree(file);
                        continue;
                    }
                }

                /* Check modification time */
                if (since > 0 && file->last_modified <= since) {
                    pfree(file->key);
                    if (file->etag)
                        pfree(file->etag);
                    pfree(file);
                    continue;
                }

                /* Check if already processed */
                if (file->is_processed) {
                    pfree(file->key);
                    if (file->etag)
                        pfree(file->etag);
                    pfree(file);
                    continue;
                }

                filtered = lappend(filtered, file);
            }

            list_free(result);
            result = filtered;
        }
    } else {
        elog(WARNING, "S3 list request failed: HTTP %ld, CURL error: %s", http_code,
             curl_easy_strerror(res));
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response.data);
    pfree(authorization);
    pfree(payload_hash);

    return result;
}

/*
 * parse_s3_list_response
 *    Parse S3 ListObjectsV2 XML response
 */
static List *parse_s3_list_response(const char *xml_response, int64 size)
{
    List *files = NIL;
    const char *p = xml_response;
    const char *end = xml_response + size;

    /* Simple XML parsing - look for <Contents> elements */
    while (p < end) {
        const char *contents_start = strstr(p, "<Contents>");
        if (!contents_start || contents_start >= end)
            break;

        const char *contents_end = strstr(contents_start, "</Contents>");
        if (!contents_end || contents_end >= end)
            break;

        /* Extract Key */
        const char *key_start = strstr(contents_start, "<Key>");
        const char *key_end = strstr(contents_start, "</Key>");

        if (key_start && key_end && key_start < contents_end) {
            S3FileInfo *file = palloc0(sizeof(S3FileInfo));

            key_start += 5; /* Skip <Key> */
            int key_len = key_end - key_start;
            file->key = palloc(key_len + 1);
            memcpy(file->key, key_start, key_len);
            file->key[key_len] = '\0';

            /* Extract Size */
            const char *size_start = strstr(contents_start, "<Size>");
            const char *size_end = strstr(contents_start, "</Size>");
            if (size_start && size_end && size_start < contents_end) {
                size_start += 6;
                char size_str[32];
                int size_len = size_end - size_start;
                if (size_len < 32) {
                    memcpy(size_str, size_start, size_len);
                    size_str[size_len] = '\0';
                    file->size = atoll(size_str);
                }
            }

            /* Extract LastModified */
            const char *lm_start = strstr(contents_start, "<LastModified>");
            const char *lm_end = strstr(contents_start, "</LastModified>");
            if (lm_start && lm_end && lm_start < contents_end) {
                lm_start += 14;
                /* Parse ISO8601 timestamp - simplified */
                struct tm tm_time;
                memset(&tm_time, 0, sizeof(tm_time));

                /* Format: 2024-01-15T10:30:00.000Z */
                if (sscanf(lm_start, "%d-%d-%dT%d:%d:%d", &tm_time.tm_year, &tm_time.tm_mon,
                           &tm_time.tm_mday, &tm_time.tm_hour, &tm_time.tm_min,
                           &tm_time.tm_sec) == 6) {
                    tm_time.tm_year -= 1900;
                    tm_time.tm_mon -= 1;
                    time_t epoch = timegm(&tm_time);
                    file->last_modified = (TimestampTz)(epoch * USECS_PER_SEC);
                }
            }

            /* Extract ETag */
            const char *etag_start = strstr(contents_start, "<ETag>");
            const char *etag_end = strstr(contents_start, "</ETag>");
            if (etag_start && etag_end && etag_start < contents_end) {
                etag_start += 6;
                int etag_len = etag_end - etag_start;
                file->etag = palloc(etag_len + 1);
                memcpy(file->etag, etag_start, etag_len);
                file->etag[etag_len] = '\0';

                /* Remove quotes */
                if (file->etag[0] == '"') {
                    memmove(file->etag, file->etag + 1, etag_len - 1);
                    file->etag[etag_len - 2] = '\0';
                }
            }

            file->is_processed = false;
            files = lappend(files, file);
        }

        p = contents_end + 11; /* Move past </Contents> */
    }

    return files;
}

/* ============================================================
 * S3 File Fetching
 * ============================================================ */

/*
 * s3_source_fetch_file
 *    Download a file from S3
 */
char *s3_source_fetch_file(S3Client *client, const char *key, int64 *size)
{
    S3DownloadResult *result;
    char *data = NULL;

    if (client == NULL || key == NULL)
        return NULL;

    result = s3_download(client, key);

    if (result->success) {
        data = result->data;
        if (size)
            *size = result->size;

        elog(DEBUG1, "Fetched S3 file %s (%ld bytes)", key, result->size);
    } else {
        elog(WARNING, "Failed to fetch S3 file %s: %s", key,
             result->error_message ? result->error_message : "unknown error");
    }

    /* Note: Don't free result->data as we're returning it */
    if (!result->success && result->error_message)
        pfree(result->error_message);
    pfree(result);

    return data;
}

/* ============================================================
 * Processed File Tracking
 * ============================================================ */

/*
 * s3_source_mark_processed
 *    Mark a file as processed to prevent reprocessing
 */
bool s3_source_mark_processed(int64 pipeline_id, const char *key)
{
    StringInfoData query;
    int ret;

    if (key == NULL)
        return false;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO orochi.orochi_pipeline_s3_files "
                     "(pipeline_id, s3_key, processed_at, status) "
                     "VALUES (%ld, '%s', NOW(), 'processed') "
                     "ON CONFLICT (pipeline_id, s3_key) DO UPDATE SET "
                     "processed_at = NOW(), status = 'processed'",
                     pipeline_id, key);

    ret = SPI_execute(query.data, false, 0);

    pfree(query.data);
    SPI_finish();

    return (ret == SPI_OK_INSERT);
}

/*
 * s3_source_get_processed_files
 *    Get list of already processed files for a pipeline
 */
List *s3_source_get_processed_files(int64 pipeline_id)
{
    List *files = NIL;
    StringInfoData query;
    int ret;
    int i;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT s3_key FROM orochi.orochi_pipeline_s3_files "
                     "WHERE pipeline_id = %ld AND status = 'processed'",
                     pipeline_id);

    ret = SPI_execute(query.data, true, 0);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        for (i = 0; i < SPI_processed; i++) {
            char *key = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            if (key)
                files = lappend(files, pstrdup(key));
        }
    }

    pfree(query.data);
    SPI_finish();

    return files;
}

/*
 * s3_source_is_processed
 *    Check if a specific file has been processed
 */
bool s3_source_is_processed(int64 pipeline_id, const char *key)
{
    StringInfoData query;
    int ret;
    bool processed = false;

    if (key == NULL)
        return false;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT 1 FROM orochi.orochi_pipeline_s3_files "
                     "WHERE pipeline_id = %ld AND s3_key = '%s' AND status = 'processed'",
                     pipeline_id, key);

    ret = SPI_execute(query.data, true, 1);
    processed = (ret == SPI_OK_SELECT && SPI_processed > 0);

    pfree(query.data);
    SPI_finish();

    return processed;
}

/* ============================================================
 * S3 Batch Processing
 * ============================================================ */

/*
 * s3_process_files
 *    Process new S3 files for a pipeline
 *
 * Returns number of files successfully processed.
 */
int s3_process_files(Pipeline *pipeline, S3Client *client, int max_files)
{
    S3SourceConfig *config;
    List *new_files;
    ListCell *lc;
    int processed = 0;
    int64 total_bytes = 0;

    if (pipeline == NULL || client == NULL)
        return 0;

    config = pipeline->source_config.s3;
    if (config == NULL)
        return 0;

    /* Get list of new files */
    new_files = s3_source_list_new_files(client, config->prefix, config->file_pattern, 0);

    if (new_files == NIL)
        return 0;

    /* Process each file */
    foreach (lc, new_files) {
        S3FileInfo *file = (S3FileInfo *)lfirst(lc);
        char *data;
        int64 size = 0;

        if (processed >= max_files)
            break;

        /* Skip if already processed */
        if (s3_source_is_processed(pipeline->pipeline_id, file->key)) {
            elog(DEBUG1, "Skipping already processed file: %s", file->key);
            continue;
        }

        /* Check file size limit */
        if (config->max_file_size > 0 && file->size > config->max_file_size) {
            elog(WARNING, "Skipping file %s: size %ld exceeds limit %ld", file->key, file->size,
                 config->max_file_size);
            continue;
        }

        /* Fetch file */
        data = s3_source_fetch_file(client, file->key, &size);
        if (data == NULL) {
            elog(WARNING, "Failed to fetch file: %s", file->key);
            continue;
        }

        /* Parse and insert data based on format */
        PG_TRY();
        {
            List *records = NIL;

            switch (pipeline->format) {
            case PIPELINE_FORMAT_JSON:
                records = parse_json_records(data, size);
                break;

            case PIPELINE_FORMAT_CSV:
                records = parse_csv_records(data, size, ',', true);
                break;

            case PIPELINE_FORMAT_LINE:
                records = parse_line_protocol(data, size);
                break;

            default:
                elog(WARNING, "Unsupported format for S3 file processing");
                break;
            }

            if (records != NIL) {
                int64 inserted = pipeline_insert_batch(pipeline, records, true);

                elog(DEBUG1, "Processed S3 file %s: %d records, %ld bytes", file->key,
                     list_length(records), size);

                list_free_deep(records);
            }

            /* Mark as processed */
            s3_source_mark_processed(pipeline->pipeline_id, file->key);
            processed++;
            total_bytes += size;

            /* Optionally delete file */
            if (config->delete_after_process) {
                if (s3_delete(client, file->key)) {
                    elog(DEBUG1, "Deleted processed S3 file: %s", file->key);
                }
            }
        }
        PG_CATCH();
        {
            elog(WARNING, "Error processing S3 file %s", file->key);
            FlushErrorState();
        }
        PG_END_TRY();

        pfree(data);
    }

    /* Free file list */
    foreach (lc, new_files) {
        S3FileInfo *file = (S3FileInfo *)lfirst(lc);
        pfree(file->key);
        if (file->etag)
            pfree(file->etag);
        pfree(file);
    }
    list_free(new_files);

    elog(LOG, "S3 source processed %d files (%ld bytes) for pipeline %ld", processed, total_bytes,
         pipeline->pipeline_id);

    return processed;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * matches_pattern
 *    Simple glob pattern matching (supports * and ?)
 */
static bool matches_pattern(const char *filename, const char *pattern)
{
    if (filename == NULL || pattern == NULL)
        return true;

    while (*pattern) {
        if (*pattern == '*') {
            /* Match zero or more characters */
            pattern++;
            if (*pattern == '\0')
                return true;

            while (*filename) {
                if (matches_pattern(filename, pattern))
                    return true;
                filename++;
            }
            return false;
        } else if (*pattern == '?') {
            /* Match exactly one character */
            if (*filename == '\0')
                return false;
            filename++;
            pattern++;
        } else {
            /* Match literal character */
            if (*filename != *pattern)
                return false;
            filename++;
            pattern++;
        }
    }

    return (*filename == '\0');
}

/*
 * extract_filename
 *    Extract filename from S3 key (path)
 */
static char *extract_filename(const char *key)
{
    const char *last_slash;

    if (key == NULL)
        return NULL;

    last_slash = strrchr(key, '/');
    if (last_slash)
        return (char *)(last_slash + 1);

    return (char *)key;
}

/* ============================================================
 * S3 Source Cleanup
 * ============================================================ */

/*
 * s3_source_close
 *    Close S3 source and release resources
 */
void s3_source_close(S3Client *client)
{
    if (client) {
        s3_client_destroy(client);
        elog(LOG, "S3 source closed");
    }
}
