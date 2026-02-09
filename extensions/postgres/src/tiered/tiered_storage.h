/*-------------------------------------------------------------------------
 *
 * tiered_storage.h
 *    Orochi DB tiered storage - hot/warm/cold data lifecycle
 *
 * Provides:
 *   - Automatic data tiering based on access patterns
 *   - S3/object storage integration for cold data
 *   - Transparent query access across tiers
 *   - Background data movement
 *
 * Storage Tiers:
 *   HOT    - Local SSD, fully indexed, row format
 *   WARM   - Local disk, compressed columnar
 *   COLD   - S3, heavily compressed columnar
 *   FROZEN - S3 Glacier, archived
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_TIERED_STORAGE_H
#define OROCHI_TIERED_STORAGE_H

#include "../orochi.h"
#include "postgres.h"

/* ============================================================
 * S3 Client Structures
 * ============================================================ */

typedef struct S3Client {
    char *endpoint;
    char *bucket;
    char *access_key;
    char *secret_key;
    char *region;
    bool use_ssl;
    int timeout_ms;
    void *curl_handle; /* CURL handle */
} S3Client;

typedef struct S3Object {
    char *key;
    char *etag;
    int64 size;
    TimestampTz last_modified;
    char *content_type;
    char *storage_class;
} S3Object;

typedef struct S3UploadResult {
    bool success;
    char *etag;
    char *error_message;
    int http_status;
} S3UploadResult;

typedef struct S3DownloadResult {
    bool success;
    char *data;
    int64 size;
    char *error_message;
    int http_status;
} S3DownloadResult;

/* ============================================================
 * Tiering Structures
 * ============================================================ */

/*
 * Data location in tiered storage
 */
typedef struct TieredLocation {
    OrochiStorageTier tier;
    char *local_path; /* For HOT/WARM */
    char *s3_key;     /* For COLD/FROZEN */
    int64 size_bytes;
    bool is_compressed;
    OrochiCompressionType compression;
} TieredLocation;

/*
 * Tier transition record
 */
typedef struct TierTransition {
    int64 transition_id;
    int64 chunk_id;
    OrochiStorageTier from_tier;
    OrochiStorageTier to_tier;
    TimestampTz started_at;
    TimestampTz completed_at;
    int64 bytes_moved;
    bool success;
    char *error_message;
} TierTransition;

/*
 * Access statistics for tiering decisions
 */
typedef struct AccessStats {
    int64 chunk_id;
    int64 read_count;
    int64 write_count;
    TimestampTz last_read;
    TimestampTz last_write;
    TimestampTz created_at;
} AccessStats;

/* ============================================================
 * S3 Client Functions
 * ============================================================ */

/*
 * Generate AWS Signature V4 for S3 requests
 * Used internally by S3 upload/download functions
 */
extern char *generate_aws_signature_v4(S3Client *client, const char *method, const char *uri,
                                       const char *query_string, const char *payload,
                                       size_t payload_len, const char *date_stamp,
                                       const char *amz_date, char **out_authorization);

/*
 * Create S3 client from configuration
 */
extern S3Client *s3_client_create(OrochiS3Config *config);

/*
 * Destroy S3 client
 */
extern void s3_client_destroy(S3Client *client);

/*
 * Test S3 connection
 */
extern bool s3_client_test_connection(S3Client *client);

/*
 * Upload data to S3
 */
extern S3UploadResult *s3_upload(S3Client *client, const char *key, const char *data, int64 size,
                                 const char *content_type);

/*
 * Download data from S3
 */
extern S3DownloadResult *s3_download(S3Client *client, const char *key);

/*
 * Delete object from S3
 */
extern bool s3_delete(S3Client *client, const char *key);

/*
 * List objects with prefix
 */
extern List *s3_list_objects(S3Client *client, const char *prefix);

/*
 * Check if object exists
 */
extern bool s3_object_exists(S3Client *client, const char *key);

/*
 * Get object metadata
 */
extern S3Object *s3_head_object(S3Client *client, const char *key);

/*
 * Multipart upload part tracking
 */
typedef struct S3MultipartPart {
    int part_number;
    char *etag;
} S3MultipartPart;

/*
 * Multipart upload for large objects
 */
extern char *s3_multipart_upload_start(S3Client *client, const char *key);

/* Returns ETag on success, NULL on failure - use this for proper uploads */
extern char *s3_multipart_upload_part_ex(S3Client *client, const char *key, const char *upload_id,
                                         int part_number, const char *data, int64 size);

/* Complete upload with parts array containing ETags */
extern bool s3_multipart_upload_complete_with_parts(S3Client *client, const char *key,
                                                    const char *upload_id, S3MultipartPart *parts,
                                                    int num_parts);

extern void s3_multipart_upload_abort(S3Client *client, const char *key, const char *upload_id);

/* ============================================================
 * Tiering Policy Functions
 * ============================================================ */

/*
 * Set tiering policy for table
 */
extern void orochi_set_tiering_policy(Oid table_oid, Interval *hot_to_warm, Interval *warm_to_cold,
                                      Interval *cold_to_frozen);

/*
 * Get tiering policy
 */
extern OrochiTieringPolicy *orochi_get_tiering_policy(Oid table_oid);

/*
 * Remove tiering policy
 */
extern void orochi_remove_tiering_policy(Oid table_oid);

/*
 * Enable/disable tiering for a specific table
 */
extern void orochi_enable_table_tiering(Oid table_oid);
extern void orochi_disable_table_tiering(Oid table_oid);

/* ============================================================
 * Tier Movement Functions
 * ============================================================ */

/*
 * Move chunk to specified tier
 */
extern void orochi_move_to_tier(int64 chunk_id, OrochiStorageTier tier);

/*
 * Move chunk to warm tier (compress locally)
 */
extern void orochi_move_to_warm(int64 chunk_id);

/*
 * Move chunk to cold tier (upload to S3)
 */
extern void orochi_move_to_cold(int64 chunk_id);

/*
 * Move chunk to frozen tier (S3 Glacier)
 */
extern void orochi_move_to_frozen(int64 chunk_id);

/*
 * Recall chunk from cold storage
 */
extern void orochi_recall_from_cold(int64 chunk_id);

/*
 * Get current tier for chunk
 */
extern OrochiStorageTier orochi_get_chunk_tier(int64 chunk_id);

/*
 * Get location info for chunk
 */
extern TieredLocation *orochi_get_chunk_location(int64 chunk_id);

/* ============================================================
 * Automatic Tiering
 * ============================================================ */

/*
 * Apply all tiering policies
 */
extern void orochi_apply_tiering_policies(void);

/*
 * Check and tier single table
 */
extern int orochi_apply_tiering_for_table(Oid table_oid);

/*
 * Get chunks eligible for tiering
 */
extern List *orochi_get_chunks_for_tiering(Oid table_oid, OrochiStorageTier from_tier,
                                           OrochiStorageTier to_tier);

/* ============================================================
 * Access Tracking
 * ============================================================ */

/*
 * Record chunk access
 */
extern void orochi_record_chunk_access(int64 chunk_id, bool is_write);

/*
 * Get access statistics
 */
extern AccessStats *orochi_get_access_stats(int64 chunk_id);

/*
 * Reset access statistics
 */
extern void orochi_reset_access_stats(int64 chunk_id);

/* ============================================================
 * Query Integration
 * ============================================================ */

/*
 * Prepare chunk for reading (recall if needed)
 */
extern void orochi_prepare_chunk_for_read(int64 chunk_id);

/*
 * Check if chunk is accessible
 */
extern bool orochi_chunk_is_accessible(int64 chunk_id);

/*
 * Get estimated access latency for chunk
 */
extern int orochi_get_chunk_access_latency_ms(int64 chunk_id);

/* ============================================================
 * Background Worker
 * ============================================================ */

/*
 * Tiering worker main function
 */
extern void orochi_tiering_worker_main(Datum main_arg);

/*
 * Compression worker main function
 */
extern void orochi_compression_worker_main(Datum main_arg);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Get tier name
 */
extern const char *orochi_tier_name(OrochiStorageTier tier);

/*
 * Parse tier name
 */
extern OrochiStorageTier orochi_parse_tier(const char *name);

/*
 * Generate S3 key for chunk
 */
extern char *orochi_generate_s3_key(int64 chunk_id);

/*
 * Estimate tier transition time
 */
extern int orochi_estimate_transition_time_ms(int64 chunk_id, OrochiStorageTier from_tier,
                                              OrochiStorageTier to_tier);

#endif /* OROCHI_TIERED_STORAGE_H */
