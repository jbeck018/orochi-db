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
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "../storage/columnar.h"
#include "tiered_storage.h"

/* Background worker state */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* S3 client instance */
static S3Client *global_s3_client = NULL;

/* Forward declarations */
static void tiering_sighup_handler(SIGNAL_ARGS);
static void tiering_sigterm_handler(SIGNAL_ARGS);
static void process_tiering_queue(void);
static bool do_tier_transition(int64 chunk_id, OrochiStorageTier to_tier);

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

    result = palloc0(sizeof(S3UploadResult));

    if (client == NULL || key == NULL || data == NULL)
    {
        result->success = false;
        result->error_message = pstrdup("Invalid parameters");
        return result;
    }

    /* TODO: Implement actual S3 upload using CURL
     *
     * 1. Build AWS Signature V4
     * 2. Create PUT request to s3://bucket/key
     * 3. Set Content-Type and Content-Length headers
     * 4. Send data
     * 5. Handle response
     */

    elog(DEBUG1, "S3 Upload: %s/%s (%ld bytes)", client->bucket, key, size);

    result->success = true;
    result->etag = pstrdup("mock-etag");
    result->http_status = 200;

    return result;
}

S3DownloadResult *
s3_download(S3Client *client, const char *key)
{
    S3DownloadResult *result;

    result = palloc0(sizeof(S3DownloadResult));

    if (client == NULL || key == NULL)
    {
        result->success = false;
        result->error_message = pstrdup("Invalid parameters");
        return result;
    }

    /* TODO: Implement actual S3 download using CURL
     *
     * 1. Build AWS Signature V4
     * 2. Create GET request to s3://bucket/key
     * 3. Download data
     * 4. Return buffer
     */

    elog(DEBUG1, "S3 Download: %s/%s", client->bucket, key);

    result->success = false;
    result->error_message = pstrdup("Not implemented");

    return result;
}

bool
s3_delete(S3Client *client, const char *key)
{
    if (client == NULL || key == NULL)
        return false;

    /* TODO: Implement S3 DELETE */
    elog(DEBUG1, "S3 Delete: %s/%s", client->bucket, key);

    return true;
}

bool
s3_object_exists(S3Client *client, const char *key)
{
    if (client == NULL || key == NULL)
        return false;

    /* TODO: Implement S3 HEAD */
    return false;
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
