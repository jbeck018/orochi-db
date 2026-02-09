/*-------------------------------------------------------------------------
 *
 * s3_multipart.c
 *    S3 Multipart Upload Implementation
 *
 * Implements AWS S3 multipart upload API for large object uploads:
 * - InitiateMultipartUpload
 * - UploadPart
 * - CompleteMultipartUpload
 * - AbortMultipartUpload
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "tiered_storage.h"

#ifdef HAVE_LIBCURL

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <string.h>
#include <time.h>

/* External functions from tiered_storage.c */
extern char *generate_aws_signature_v4(S3Client *client, const char *method, const char *uri,
                                       const char *query_string, const char *payload,
                                       size_t payload_len, const char *date_stamp,
                                       const char *amz_date, char **out_authorization);

/* S3MultipartPart is defined in tiered_storage.h */

/* Forward declarations */
static char *extract_upload_id_from_xml(const char *xml_data);
static char *extract_etag_from_xml(const char *xml_data);
static char *extract_etag_from_headers(const char *headers);

/*
 * CURL buffer structure (also defined in tiered_storage.c)
 */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} CurlBuffer;

/*
 * Structure to capture headers during CURL request
 */
typedef struct {
    char *etag;
} HeaderCapture;

/* CURL write callback (also defined in tiered_storage.c) */
static size_t orochi_curl_write_cb_mp(void *contents, size_t size, size_t nmemb, void *userp)
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

/*
 * CURL header callback to capture ETag from response headers
 * S3 returns ETag in header like: ETag: "d41d8cd98f00b204e9800998ecf8427e"
 */
static size_t orochi_curl_header_cb_mp(char *buffer, size_t size, size_t nitems, void *userp)
{
    size_t realsize = size * nitems;
    HeaderCapture *capture = (HeaderCapture *)userp;
    const char *etag_prefix = "ETag:";
    size_t prefix_len = strlen(etag_prefix);

    /* Check if this header starts with "ETag:" (case-insensitive) */
    if (realsize > prefix_len && strncasecmp(buffer, etag_prefix, prefix_len) == 0) {
        char *etag_start = buffer + prefix_len;
        char *etag_end;
        size_t etag_len;

        /* Skip leading whitespace */
        while (*etag_start == ' ' || *etag_start == '\t')
            etag_start++;

        /* Find end of ETag (before CRLF) */
        etag_end = etag_start;
        while (*etag_end && *etag_end != '\r' && *etag_end != '\n')
            etag_end++;

        /* Remove surrounding quotes if present */
        if (*etag_start == '"')
            etag_start++;
        if (etag_end > etag_start && *(etag_end - 1) == '"')
            etag_end--;

        etag_len = etag_end - etag_start;
        if (etag_len > 0) {
            capture->etag = palloc(etag_len + 1);
            memcpy(capture->etag, etag_start, etag_len);
            capture->etag[etag_len] = '\0';
        }
    }

    return realsize;
}

/*
 * Parse UploadId from InitiateMultipartUpload XML response
 */
static char *extract_upload_id_from_xml(const char *xml_data)
{
    char *id_start;
    char *id_end;
    char *upload_id;
    size_t id_len;

    if (xml_data == NULL)
        return NULL;

    /* Look for <UploadId>...</UploadId> */
    id_start = strstr(xml_data, "<UploadId>");
    if (id_start == NULL)
        return NULL;

    id_start += 10; /* Skip "<UploadId>" */
    id_end = strstr(id_start, "</UploadId>");
    if (id_end == NULL)
        return NULL;

    id_len = id_end - id_start;
    upload_id = palloc(id_len + 1);
    memcpy(upload_id, id_start, id_len);
    upload_id[id_len] = '\0';

    return upload_id;
}

/*
 * Parse ETag from XML response body
 */
static char *extract_etag_from_xml(const char *xml_data)
{
    char *etag_start;
    char *etag_end;
    char *etag;
    size_t etag_len;

    if (xml_data == NULL)
        return NULL;

    /* Look for <ETag>...</ETag> in XML response */
    etag_start = strstr(xml_data, "<ETag>");
    if (etag_start == NULL)
        return NULL;

    etag_start += 6; /* Skip "<ETag>" */
    etag_end = strstr(etag_start, "</ETag>");
    if (etag_end == NULL)
        return NULL;

    /* Remove quotes if present */
    if (*etag_start == '"')
        etag_start++;
    if (*(etag_end - 1) == '"')
        etag_end--;

    etag_len = etag_end - etag_start;
    etag = palloc(etag_len + 1);
    memcpy(etag, etag_start, etag_len);
    etag[etag_len] = '\0';

    return etag;
}

/*
 * Parse ETag from HTTP response headers (fallback for non-XML responses)
 * Header format: "ETag: \"d41d8cd98f00b204e9800998ecf8427e\"\r\n"
 */
static char *extract_etag_from_headers(const char *headers)
{
    const char *etag_prefix = "ETag:";
    char *found;
    char *etag_start;
    char *etag_end;
    char *etag;
    size_t etag_len;

    if (headers == NULL)
        return NULL;

    /* Case-insensitive search for "ETag:" header */
    found = strcasestr(headers, etag_prefix);
    if (found == NULL)
        return NULL;

    etag_start = found + strlen(etag_prefix);

    /* Skip whitespace */
    while (*etag_start == ' ' || *etag_start == '\t')
        etag_start++;

    /* Find end of value (CRLF or LF) */
    etag_end = etag_start;
    while (*etag_end && *etag_end != '\r' && *etag_end != '\n')
        etag_end++;

    /* Remove surrounding quotes */
    if (*etag_start == '"')
        etag_start++;
    if (etag_end > etag_start && *(etag_end - 1) == '"')
        etag_end--;

    etag_len = etag_end - etag_start;
    if (etag_len == 0)
        return NULL;

    etag = palloc(etag_len + 1);
    memcpy(etag, etag_start, etag_len);
    etag[etag_len] = '\0';

    return etag;
}

/*
 * Initiate multipart upload
 * POST /{bucket}/{key}?uploads
 * Returns upload_id to be used in subsequent part uploads
 */
char *s3_multipart_upload_start(S3Client *client, const char *key)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    char url[1024];
    char uri[512];
    char query_string[] = "uploads";
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    time_t now;
    struct tm *tm_info;
    long http_code;
    char *upload_id = NULL;

    if (client == NULL || key == NULL)
        return NULL;

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
        snprintf(url, sizeof(url), "https://%s%s?uploads", client->endpoint, uri);
    else
        snprintf(url, sizeof(url), "http://%s%s?uploads", client->endpoint, uri);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "POST", uri, query_string, "", 0, date_stamp,
                                             amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(response_buf.data);
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

    /* Configure CURL for POST request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb_mp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        /* Parse UploadId from XML response */
        upload_id = extract_upload_id_from_xml(response_buf.data);
        if (upload_id) {
            elog(LOG, "S3 Multipart upload initiated: %s/%s (UploadId: %s)", client->bucket, key,
                 upload_id);
        } else {
            elog(WARNING, "Failed to parse UploadId from response: %s", response_buf.data);
        }
    } else {
        elog(WARNING, "S3 Multipart upload initiation failed: HTTP %ld - %s", http_code,
             response_buf.data);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(authorization);
    pfree(payload_hash);

    return upload_id;
}

/*
 * Upload a single part in multipart upload
 * PUT /{bucket}/{key}?partNumber={n}&uploadId={id}
 * Returns the ETag on success, NULL on failure
 * Minimum part size is 5MB except for the last part
 *
 * IMPORTANT: The returned ETag must be stored and passed to
 * s3_multipart_upload_complete_with_parts() to finalize the upload.
 */
char *s3_multipart_upload_part_ex(S3Client *client, const char *key, const char *upload_id,
                                  int part_number, const char *data, int64 size)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    HeaderCapture header_capture;
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
    char *result_etag = NULL;

    if (client == NULL || key == NULL || upload_id == NULL || data == NULL)
        return NULL;

    /* Part numbers must be 1-10000 */
    if (part_number < 1 || part_number > 10000) {
        elog(WARNING, "Invalid part number %d (must be 1-10000)", part_number);
        return NULL;
    }

    /* Minimum part size is 5MB except for last part */
    if (size < 5 * 1024 * 1024) {
        elog(DEBUG1, "Part %d is smaller than 5MB (%ld bytes), assuming last part", part_number,
             size);
    }

    /* Initialize response buffer */
    response_buf.data = palloc(1024);
    response_buf.size = 0;
    response_buf.capacity = 1024;

    /* Initialize header capture */
    header_capture.etag = NULL;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build query string and URI */
    snprintf(query_string, sizeof(query_string), "partNumber=%d&uploadId=%s", part_number,
             upload_id);
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);

    /* Build URL */
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s?%s", client->endpoint, uri, query_string);
    else
        snprintf(url, sizeof(url), "http://%s%s?%s", client->endpoint, uri, query_string);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "PUT", uri, query_string, data, size,
                                             date_stamp, amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(response_buf.data);
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

    snprintf(header_buf, sizeof(header_buf), "Content-Length: %ld", size);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL for PUT request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb_mp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, orochi_curl_header_cb_mp);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_capture);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        if (header_capture.etag != NULL) {
            result_etag = header_capture.etag;
            elog(LOG, "S3 Multipart part %d uploaded: %s/%s (%ld bytes, ETag: %s)", part_number,
                 client->bucket, key, size, result_etag);
        } else {
            elog(WARNING, "S3 Multipart part %d uploaded but no ETag received", part_number);
        }
    } else {
        elog(WARNING, "S3 Multipart part %d upload failed: HTTP %ld - %s", part_number, http_code,
             response_buf.data);
        if (header_capture.etag != NULL)
            pfree(header_capture.etag);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(authorization);
    pfree(payload_hash);

    return result_etag;
}

/*
 * Complete multipart upload with parts array
 * POST /{bucket}/{key}?uploadId={id}
 * Finalizes the upload by combining all parts
 *
 * This function properly builds the CompleteMultipartUpload XML
 * with all part numbers and ETags as required by the S3 API.
 */
bool s3_multipart_upload_complete_with_parts(S3Client *client, const char *key,
                                             const char *upload_id, S3MultipartPart *parts,
                                             int num_parts)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
    char url[1024];
    char uri[512];
    char query_string[256];
    char date_stamp[16];
    char amz_date[32];
    char *authorization;
    char *payload_hash;
    char header_buf[512];
    StringInfoData complete_xml;
    time_t now;
    struct tm *tm_info;
    long http_code;
    bool success = false;
    int i;

    if (client == NULL || key == NULL || upload_id == NULL)
        return false;

    if (parts == NULL || num_parts < 1) {
        elog(WARNING, "Cannot complete multipart upload without parts");
        return false;
    }

    /* Initialize response buffer */
    response_buf.data = palloc(4096);
    response_buf.size = 0;
    response_buf.capacity = 4096;

    /*
     * Build CompleteMultipartUpload XML payload
     * S3 requires all parts to be listed with their part numbers and ETags
     * in ascending order by part number.
     *
     * Format:
     * <CompleteMultipartUpload>
     *   <Part>
     *     <PartNumber>1</PartNumber>
     *     <ETag>"etag-value"</ETag>
     *   </Part>
     *   ...
     * </CompleteMultipartUpload>
     */
    initStringInfo(&complete_xml);
    appendStringInfo(&complete_xml, "<CompleteMultipartUpload>");

    for (i = 0; i < num_parts; i++) {
        if (parts[i].etag == NULL) {
            elog(WARNING, "Part %d missing ETag, cannot complete upload", parts[i].part_number);
            pfree(complete_xml.data);
            pfree(response_buf.data);
            return false;
        }
        appendStringInfo(&complete_xml,
                         "<Part>"
                         "<PartNumber>%d</PartNumber>"
                         "<ETag>\"%s\"</ETag>"
                         "</Part>",
                         parts[i].part_number, parts[i].etag);
    }

    appendStringInfo(&complete_xml, "</CompleteMultipartUpload>");

    elog(DEBUG1, "CompleteMultipartUpload XML (%d parts): %s", num_parts, complete_xml.data);

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build query string and URI */
    snprintf(query_string, sizeof(query_string), "uploadId=%s", upload_id);
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);

    /* Build URL */
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s?%s", client->endpoint, uri, query_string);
    else
        snprintf(url, sizeof(url), "http://%s%s?%s", client->endpoint, uri, query_string);

    /* Generate AWS Signature V4 */
    payload_hash =
        generate_aws_signature_v4(client, "POST", uri, query_string, complete_xml.data,
                                  complete_xml.len, date_stamp, amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(response_buf.data);
        pfree(complete_xml.data);
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

    snprintf(header_buf, sizeof(header_buf), "Content-Type: application/xml");
    headers = curl_slist_append(headers, header_buf);

    snprintf(header_buf, sizeof(header_buf), "Content-Length: %d", complete_xml.len);
    headers = curl_slist_append(headers, header_buf);

    /* Configure CURL for POST request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, complete_xml.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, complete_xml.len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb_mp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        success = true;
        elog(LOG, "S3 Multipart upload completed: %s/%s (UploadId: %s, %d parts)", client->bucket,
             key, upload_id, num_parts);
    } else {
        elog(WARNING, "S3 Multipart upload completion failed: HTTP %ld - %s", http_code,
             response_buf.data);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(complete_xml.data);
    pfree(authorization);
    pfree(payload_hash);

    return success;
}

/*
 * Abort multipart upload
 * DELETE /{bucket}/{key}?uploadId={id}
 * Cleans up all uploaded parts
 */
void s3_multipart_upload_abort(S3Client *client, const char *key, const char *upload_id)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    CurlBuffer response_buf;
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

    if (client == NULL || key == NULL || upload_id == NULL)
        return;

    /* Initialize response buffer */
    response_buf.data = palloc(1024);
    response_buf.size = 0;
    response_buf.capacity = 1024;

    /* Get current time for signing */
    now = time(NULL);
    tm_info = gmtime(&now);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    /* Build query string and URI */
    snprintf(query_string, sizeof(query_string), "uploadId=%s", upload_id);
    snprintf(uri, sizeof(uri), "/%s/%s", client->bucket, key);

    /* Build URL */
    if (client->use_ssl)
        snprintf(url, sizeof(url), "https://%s%s?%s", client->endpoint, uri, query_string);
    else
        snprintf(url, sizeof(url), "http://%s%s?%s", client->endpoint, uri, query_string);

    /* Generate AWS Signature V4 */
    payload_hash = generate_aws_signature_v4(client, "DELETE", uri, query_string, "", 0, date_stamp,
                                             amz_date, &authorization);

    /* Initialize CURL */
    curl = curl_easy_init();
    if (!curl) {
        pfree(response_buf.data);
        return;
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

    /* Configure CURL for DELETE request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, orochi_curl_write_cb_mp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, client->timeout_ms / 1000);

    /* Perform request */
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        elog(LOG, "S3 Multipart upload aborted: %s/%s (UploadId: %s)", client->bucket, key,
             upload_id);
    } else {
        elog(WARNING, "S3 Multipart upload abort failed: HTTP %ld - %s", http_code,
             response_buf.data);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    pfree(response_buf.data);
    pfree(authorization);
    pfree(payload_hash);
}

/*
 * Helper function to upload chunk to S3
 * Automatically chooses between:
 * - Regular upload for files < 100MB
 * - Multipart upload for files >= 100MB (10MB parts)
 *
 * This implementation properly tracks ETags for multipart uploads
 * as required by the S3 CompleteMultipartUpload API.
 */
S3UploadResult *orochi_upload_chunk_to_s3(S3Client *client, const char *key, const char *data,
                                          int64 size)
{
    const int64 MULTIPART_THRESHOLD = 100 * 1024 * 1024; /* 100MB */
    const int64 PART_SIZE = 10 * 1024 * 1024;            /* 10MB parts */

    if (size < MULTIPART_THRESHOLD) {
        /* Use regular upload for small files */
        extern S3UploadResult *s3_upload(S3Client * client, const char *key, const char *data,
                                         int64 size, const char *content_type);
        return s3_upload(client, key, data, size, "application/octet-stream");
    } else {
        /* Use multipart upload for large files */
        S3UploadResult *result;
        char *upload_id;
        S3MultipartPart *parts;
        int max_parts;
        int part_count;
        int part_number;
        int64 offset;
        bool success = true;
        char *final_etag;
        int i;

        result = palloc0(sizeof(S3UploadResult));

        /* Calculate maximum number of parts needed */
        max_parts = (int)((size + PART_SIZE - 1) / PART_SIZE);
        if (max_parts > 10000) {
            result->success = false;
            result->error_message = pstrdup("File too large: would exceed 10000 parts limit");
            return result;
        }

        /* Allocate parts array to track ETags */
        parts = palloc0(sizeof(S3MultipartPart) * max_parts);
        part_count = 0;

        /* Start multipart upload */
        upload_id = s3_multipart_upload_start(client, key);
        if (upload_id == NULL) {
            pfree(parts);
            result->success = false;
            result->error_message = pstrdup("Failed to initiate multipart upload");
            return result;
        }

        /* Upload parts and collect ETags */
        part_number = 1;
        offset = 0;

        while (offset < size) {
            int64 part_size = PART_SIZE;
            char *etag;

            if (offset + part_size > size)
                part_size = size - offset;

            /* Upload part and get ETag */
            etag = s3_multipart_upload_part_ex(client, key, upload_id, part_number, data + offset,
                                               part_size);

            if (etag == NULL) {
                success = false;
                elog(WARNING, "Failed to upload part %d of multipart upload", part_number);
                break;
            }

            /* Store part info for completion */
            parts[part_count].part_number = part_number;
            parts[part_count].etag = etag;
            part_count++;

            offset += part_size;
            part_number++;
        }

        if (success) {
            /* Complete multipart upload with all parts and ETags */
            if (s3_multipart_upload_complete_with_parts(client, key, upload_id, parts,
                                                        part_count)) {
                result->success = true;

                /* Build composite ETag (S3 format: "etag-N" where N is part count) */
                final_etag = palloc(128);
                if (part_count > 0 && parts[0].etag != NULL) {
                    snprintf(final_etag, 128, "%s-%d", parts[part_count - 1].etag, part_count);
                } else {
                    snprintf(final_etag, 128, "multipart-%d", part_count);
                }
                result->etag = final_etag;
                result->http_status = 200;

                elog(LOG, "Successfully uploaded %ld bytes in %d parts to %s/%s", size, part_count,
                     client->bucket, key);
            } else {
                result->success = false;
                result->error_message = pstrdup("Failed to complete multipart upload");
                s3_multipart_upload_abort(client, key, upload_id);
            }
        } else {
            /* Abort on failure */
            result->success = false;
            result->error_message = pstrdup("Failed to upload one or more parts");
            s3_multipart_upload_abort(client, key, upload_id);
        }

        /* Cleanup parts array */
        for (i = 0; i < part_count; i++) {
            if (parts[i].etag != NULL)
                pfree(parts[i].etag);
        }
        pfree(parts);
        pfree(upload_id);

        return result;
    }
}

#else /* !HAVE_LIBCURL */

/*
 * Stub implementations when libcurl is not available.
 * All S3 multipart operations require libcurl to function.
 */

char *
s3_multipart_upload_start(S3Client *client, const char *key)
{
    elog(WARNING, "Orochi: S3 multipart upload requires libcurl");
    return NULL;
}

char *
s3_multipart_upload_part_ex(S3Client *client, const char *key, const char *upload_id,
                            int part_number, const char *data, int64 size)
{
    elog(WARNING, "Orochi: S3 multipart upload requires libcurl");
    return NULL;
}

bool
s3_multipart_upload_complete_with_parts(S3Client *client, const char *key, const char *upload_id,
                                        S3MultipartPart *parts, int num_parts)
{
    elog(WARNING, "Orochi: S3 multipart upload requires libcurl");
    return false;
}

void
s3_multipart_upload_abort(S3Client *client, const char *key, const char *upload_id)
{
    elog(WARNING, "Orochi: S3 multipart upload requires libcurl");
}

S3UploadResult *
orochi_upload_chunk_to_s3(S3Client *client, const char *key, const char *data, int64 size)
{
    S3UploadResult *result = palloc0(sizeof(S3UploadResult));
    result->success = false;
    result->error_message = pstrdup("S3 multipart upload requires libcurl");
    elog(WARNING, "Orochi: S3 multipart upload requires libcurl");
    return result;
}

#endif /* HAVE_LIBCURL */
