/*-------------------------------------------------------------------------
 *
 * pipeline.h
 *    Orochi DB real-time data pipeline definitions
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_PIPELINE_H
#define OROCHI_PIPELINE_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/timestamp.h"

/* ============================================================
 * Pipeline Source Types
 * ============================================================ */

typedef enum PipelineSource
{
    PIPELINE_SOURCE_KAFKA = 0,      /* Apache Kafka consumer */
    PIPELINE_SOURCE_S3 = 1,         /* S3 bucket file watcher */
    PIPELINE_SOURCE_FILESYSTEM = 2, /* Local filesystem watcher */
    PIPELINE_SOURCE_HTTP = 3,       /* HTTP/webhook endpoint */
    PIPELINE_SOURCE_KINESIS = 4     /* AWS Kinesis stream */
} PipelineSource;

/* ============================================================
 * Pipeline States
 * ============================================================ */

typedef enum PipelineState
{
    PIPELINE_STATE_CREATED = 0,     /* Pipeline created but not started */
    PIPELINE_STATE_RUNNING = 1,     /* Actively ingesting data */
    PIPELINE_STATE_PAUSED = 2,      /* Temporarily stopped */
    PIPELINE_STATE_STOPPED = 3,     /* Permanently stopped */
    PIPELINE_STATE_ERROR = 4,       /* Error state, needs attention */
    PIPELINE_STATE_DRAINING = 5     /* Processing remaining messages before stop */
} PipelineState;

/* ============================================================
 * Data Formats
 * ============================================================ */

typedef enum PipelineFormat
{
    PIPELINE_FORMAT_JSON = 0,       /* JSON format */
    PIPELINE_FORMAT_CSV = 1,        /* CSV format */
    PIPELINE_FORMAT_AVRO = 2,       /* Apache Avro format */
    PIPELINE_FORMAT_PARQUET = 3,    /* Apache Parquet format */
    PIPELINE_FORMAT_LINE = 4        /* Line protocol (InfluxDB-style) */
} PipelineFormat;

/* ============================================================
 * Delivery Semantics
 * ============================================================ */

typedef enum PipelineDelivery
{
    PIPELINE_DELIVERY_AT_LEAST_ONCE = 0,  /* May have duplicates */
    PIPELINE_DELIVERY_AT_MOST_ONCE = 1,   /* May lose messages */
    PIPELINE_DELIVERY_EXACTLY_ONCE = 2    /* Exactly-once semantics */
} PipelineDelivery;

/* ============================================================
 * Pipeline Configuration Structures
 * ============================================================ */

/* Kafka source configuration */
typedef struct KafkaSourceConfig
{
    char           *bootstrap_servers;    /* Kafka broker list */
    char           *topic;                /* Topic to consume */
    char           *consumer_group;       /* Consumer group ID */
    int             partition;            /* Specific partition (-1 for all) */
    int64           start_offset;         /* Starting offset (-1 for latest, -2 for earliest) */
    int             fetch_min_bytes;      /* Minimum bytes to fetch */
    int             fetch_max_wait_ms;    /* Max wait time for fetch */
    int             session_timeout_ms;   /* Session timeout */
    int             heartbeat_interval_ms;/* Heartbeat interval */
    char           *security_protocol;    /* PLAINTEXT, SSL, SASL_SSL, etc. */
    char           *sasl_mechanism;       /* PLAIN, SCRAM-SHA-256, etc. */
    char           *sasl_username;        /* SASL username */
    char           *sasl_password;        /* SASL password */
    bool            enable_auto_commit;   /* Auto-commit offsets */
    int             auto_commit_interval; /* Auto-commit interval ms */
} KafkaSourceConfig;

/* S3 source configuration */
typedef struct S3SourceConfig
{
    char           *bucket;               /* S3 bucket name */
    char           *prefix;               /* Object key prefix (folder) */
    char           *region;               /* AWS region */
    char           *endpoint;             /* S3 endpoint (for MinIO, etc.) */
    char           *access_key;           /* AWS access key */
    char           *secret_key;           /* AWS secret key */
    int             poll_interval_sec;    /* How often to check for new files */
    bool            delete_after_process; /* Delete files after processing */
    char           *file_pattern;         /* Glob pattern for files (e.g., "*.json") */
    int64           max_file_size;        /* Maximum file size to process */
    bool            use_ssl;              /* Use HTTPS */
} S3SourceConfig;

/* Filesystem source configuration */
typedef struct FilesystemSourceConfig
{
    char           *watch_path;           /* Directory path to watch */
    char           *file_pattern;         /* Glob pattern for files */
    bool            recursive;            /* Watch subdirectories */
    bool            delete_after_process; /* Delete files after processing */
    int             poll_interval_sec;    /* Poll interval for changes */
} FilesystemSourceConfig;

/* ============================================================
 * Pipeline Transform Configuration
 * ============================================================ */

typedef struct PipelineTransform
{
    char           *source_field;         /* Source field name in data */
    char           *target_column;        /* Target column in table */
    char           *transform_expr;       /* Optional SQL expression */
    char           *default_value;        /* Default value if field missing */
    bool            required;             /* Is this field required? */
} PipelineTransform;

/* ============================================================
 * Pipeline Statistics
 * ============================================================ */

typedef struct PipelineStats
{
    int64           messages_received;    /* Total messages received */
    int64           messages_processed;   /* Successfully processed */
    int64           messages_failed;      /* Failed to process */
    int64           bytes_received;       /* Total bytes received */
    int64           rows_inserted;        /* Rows inserted into target */
    TimestampTz     last_message_time;    /* Time of last message */
    TimestampTz     last_error_time;      /* Time of last error */
    char           *last_error_message;   /* Last error message */
    double          avg_latency_ms;       /* Average processing latency */
    double          throughput_per_sec;   /* Messages per second */
} PipelineStats;

/* ============================================================
 * Pipeline Checkpoint
 * ============================================================ */

typedef struct PipelineCheckpoint
{
    int64           pipeline_id;          /* Pipeline ID */
    int64           checkpoint_id;        /* Checkpoint sequence number */
    TimestampTz     checkpoint_time;      /* When checkpoint was created */

    /* Kafka-specific checkpoint */
    int64           kafka_offset;         /* Kafka consumer offset */
    int32           kafka_partition;      /* Kafka partition */

    /* S3-specific checkpoint */
    char           *last_processed_key;   /* Last processed S3 object key */
    TimestampTz     last_modified;        /* Last modified time of object */

    /* Transaction info for exactly-once */
    TransactionId   last_committed_xid;   /* Last committed transaction */
    bool            is_valid;             /* Is checkpoint valid? */
} PipelineCheckpoint;

/* ============================================================
 * Main Pipeline Structure
 * ============================================================ */

typedef struct Pipeline
{
    int64           pipeline_id;          /* Unique pipeline identifier */
    char           *pipeline_name;        /* Human-readable name */

    /* Source configuration */
    PipelineSource  source_type;          /* Type of data source */
    union {
        KafkaSourceConfig      *kafka;
        S3SourceConfig         *s3;
        FilesystemSourceConfig *filesystem;
    } source_config;

    /* Target configuration */
    Oid             target_table_oid;     /* Target table OID */
    char           *target_schema;        /* Target schema name */
    char           *target_table;         /* Target table name */

    /* Data format and processing */
    PipelineFormat  format;               /* Input data format */
    PipelineDelivery delivery_semantics;  /* Delivery guarantee */
    char           *timestamp_column;     /* Column for event timestamps */
    char           *timestamp_format;     /* Format string for timestamps */

    /* Field mappings */
    int             num_transforms;       /* Number of field transforms */
    PipelineTransform *transforms;        /* Array of field mappings */

    /* Operational state */
    PipelineState   state;                /* Current pipeline state */
    TimestampTz     created_at;           /* Creation timestamp */
    TimestampTz     started_at;           /* Last start timestamp */
    TimestampTz     stopped_at;           /* Last stop timestamp */

    /* Worker info */
    int             worker_pid;           /* Background worker PID */
    int             batch_size;           /* Messages per batch */
    int             batch_timeout_ms;     /* Batch timeout in milliseconds */
    int             error_threshold;      /* Errors before stopping */
    int             retry_count;          /* Number of retries on error */
    int             retry_delay_ms;       /* Delay between retries */

    /* Statistics and checkpointing */
    PipelineStats  *stats;                /* Runtime statistics */
    PipelineCheckpoint *checkpoint;       /* Current checkpoint */

    /* Memory context */
    MemoryContext   pipeline_context;     /* Memory context for pipeline */
} Pipeline;

/* ============================================================
 * Pipeline Registry Entry (stored in shared memory)
 * ============================================================ */

typedef struct PipelineRegistryEntry
{
    int64           pipeline_id;
    PipelineState   state;
    int             worker_pid;
    int64           messages_processed;
    int64           last_checkpoint_id;
    TimestampTz     last_activity;
    bool            is_active;
} PipelineRegistryEntry;

/* ============================================================
 * Function Prototypes - Core Pipeline Management
 * ============================================================ */

/* Pipeline lifecycle */
extern int64 orochi_create_pipeline(const char *name,
                                    PipelineSource source_type,
                                    Oid target_table_oid,
                                    PipelineFormat format,
                                    const char *options_json);

extern bool orochi_start_pipeline(int64 pipeline_id);
extern bool orochi_pause_pipeline(int64 pipeline_id);
extern bool orochi_resume_pipeline(int64 pipeline_id);
extern bool orochi_stop_pipeline(int64 pipeline_id);
extern bool orochi_drop_pipeline(int64 pipeline_id, bool if_exists);

/* Pipeline configuration */
extern Pipeline *orochi_get_pipeline(int64 pipeline_id);
extern List *orochi_list_pipelines(void);
extern bool orochi_update_pipeline_config(int64 pipeline_id, const char *options_json);

/* Pipeline status and statistics */
extern PipelineState orochi_get_pipeline_state(int64 pipeline_id);
extern PipelineStats *orochi_get_pipeline_stats(int64 pipeline_id);
extern void orochi_reset_pipeline_stats(int64 pipeline_id);

/* Checkpointing */
extern bool orochi_create_checkpoint(int64 pipeline_id);
extern bool orochi_restore_checkpoint(int64 pipeline_id, int64 checkpoint_id);
extern List *orochi_list_checkpoints(int64 pipeline_id);

/* Background worker entry point */
extern void pipeline_worker_main(Datum main_arg);

/* ============================================================
 * Function Prototypes - Kafka Source
 * ============================================================ */

extern void *kafka_source_init(KafkaSourceConfig *config);
extern int kafka_source_poll(void *consumer, char **messages, int max_messages, int timeout_ms);
extern bool kafka_source_commit(void *consumer, int64 offset);
extern void kafka_source_close(void *consumer);
extern int64 kafka_source_get_offset(void *consumer);
extern bool kafka_source_seek(void *consumer, int64 offset);

/* ============================================================
 * Function Prototypes - S3 Source
 * ============================================================ */

/* Forward declaration for S3Client from tiered_storage.h */
struct S3Client;

extern struct S3Client *s3_source_init(S3SourceConfig *config);
extern List *s3_source_list_new_files(struct S3Client *client,
                                      const char *prefix,
                                      const char *pattern,
                                      TimestampTz since);
extern char *s3_source_fetch_file(struct S3Client *client,
                                  const char *key,
                                  int64 *size);
extern bool s3_source_mark_processed(int64 pipeline_id, const char *key);
extern List *s3_source_get_processed_files(int64 pipeline_id);

/* ============================================================
 * Function Prototypes - Data Parsing
 * ============================================================ */

extern List *parse_json_records(const char *data, int64 size);
extern List *parse_csv_records(const char *data, int64 size,
                               char delimiter, bool has_header);
extern List *parse_line_protocol(const char *data, int64 size);

/* ============================================================
 * Function Prototypes - Data Insertion
 * ============================================================ */

extern int64 pipeline_insert_batch(Pipeline *pipeline,
                                   List *records,
                                   bool use_copy);
extern bool pipeline_apply_transforms(Pipeline *pipeline,
                                      void *record,
                                      Datum *values,
                                      bool *nulls);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *pipeline_source_name(PipelineSource source);
extern PipelineSource pipeline_parse_source(const char *name);
extern const char *pipeline_state_name(PipelineState state);
extern PipelineState pipeline_parse_state(const char *name);
extern const char *pipeline_format_name(PipelineFormat format);
extern PipelineFormat pipeline_parse_format(const char *name);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_start_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_pause_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_resume_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stop_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_pipeline_sql(PG_FUNCTION_ARGS);
extern Datum orochi_pipeline_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_pipeline_stats_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Constants
 * ============================================================ */

#define PIPELINE_DEFAULT_BATCH_SIZE         1000
#define PIPELINE_DEFAULT_BATCH_TIMEOUT_MS   100
#define PIPELINE_DEFAULT_ERROR_THRESHOLD    100
#define PIPELINE_DEFAULT_RETRY_COUNT        3
#define PIPELINE_DEFAULT_RETRY_DELAY_MS     1000
#define PIPELINE_DEFAULT_POLL_INTERVAL_SEC  10
#define PIPELINE_MAX_NAME_LENGTH            128
#define PIPELINE_MAX_ERROR_MESSAGE          1024
#define PIPELINE_CHECKPOINT_INTERVAL        10000  /* Every 10k messages */

/* Kafka defaults */
#define KAFKA_DEFAULT_FETCH_MIN_BYTES       1
#define KAFKA_DEFAULT_FETCH_MAX_WAIT_MS     500
#define KAFKA_DEFAULT_SESSION_TIMEOUT_MS    30000
#define KAFKA_DEFAULT_HEARTBEAT_MS          10000
#define KAFKA_DEFAULT_AUTO_COMMIT_MS        5000

/* S3 defaults */
#define S3_DEFAULT_POLL_INTERVAL_SEC        30
#define S3_DEFAULT_MAX_FILE_SIZE            (100 * 1024 * 1024)  /* 100MB */

#endif /* OROCHI_PIPELINE_H */
