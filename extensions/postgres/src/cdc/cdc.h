/*-------------------------------------------------------------------------
 *
 * cdc.h
 *    Orochi DB Change Data Capture (CDC) definitions
 *
 * This module provides Change Data Capture functionality for Orochi DB,
 * enabling real-time streaming of database changes to external systems.
 * Features:
 *   - Event capture from WAL and Raft log
 *   - JSON serialization of change events
 *   - Multiple sink types (Kafka, Webhook, File)
 *   - Subscriber management with filtering
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_CDC_H
#define OROCHI_CDC_H

#include "access/xlogdefs.h"
#include "fmgr.h"
#include "postgres.h"
#include "replication/reorderbuffer.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

/* ============================================================
 * CDC Event Types
 * ============================================================ */

typedef enum CDCEventType {
    CDC_EVENT_INSERT = 0,   /* Row inserted */
    CDC_EVENT_UPDATE = 1,   /* Row updated */
    CDC_EVENT_DELETE = 2,   /* Row deleted */
    CDC_EVENT_TRUNCATE = 3, /* Table truncated */
    CDC_EVENT_BEGIN = 4,    /* Transaction begin */
    CDC_EVENT_COMMIT = 5,   /* Transaction commit */
    CDC_EVENT_DDL = 6       /* DDL statement (schema change) */
} CDCEventType;

/* ============================================================
 * CDC Sink Types
 * ============================================================ */

typedef enum CDCSinkType {
    CDC_SINK_KAFKA = 0,   /* Apache Kafka */
    CDC_SINK_WEBHOOK = 1, /* HTTP webhook */
    CDC_SINK_FILE = 2,    /* Local file (debugging) */
    CDC_SINK_KINESIS = 3, /* AWS Kinesis */
    CDC_SINK_PUBSUB = 4   /* Google Cloud Pub/Sub */
} CDCSinkType;

/* ============================================================
 * CDC Subscription State
 * ============================================================ */

typedef enum CDCSubscriptionState {
    CDC_SUB_STATE_CREATED = 0, /* Subscription created but not active */
    CDC_SUB_STATE_ACTIVE = 1,  /* Subscription is active */
    CDC_SUB_STATE_PAUSED = 2,  /* Subscription paused */
    CDC_SUB_STATE_ERROR = 3,   /* Subscription in error state */
    CDC_SUB_STATE_DISABLED = 4 /* Subscription disabled */
} CDCSubscriptionState;

/* ============================================================
 * CDC Output Format
 * ============================================================ */

typedef enum CDCOutputFormat {
    CDC_FORMAT_JSON = 0,     /* Standard JSON */
    CDC_FORMAT_AVRO = 1,     /* Apache Avro */
    CDC_FORMAT_DEBEZIUM = 2, /* Debezium-compatible JSON */
    CDC_FORMAT_PROTOBUF = 3  /* Protocol Buffers */
} CDCOutputFormat;

/* ============================================================
 * CDC Event Structure
 * ============================================================ */

/*
 * CDCColumnValue - Single column value in a CDC event
 */
typedef struct CDCColumnValue {
    char *column_name; /* Column name */
    Oid column_type;   /* PostgreSQL type OID */
    char *type_name;   /* Type name as string */
    bool is_null;      /* Is value NULL? */
    char *value;       /* Value as string (JSON-escaped) */
    bool is_key;       /* Is this column part of primary key? */
} CDCColumnValue;

/*
 * CDCEvent - Single change data capture event
 */
typedef struct CDCEvent {
    int64 event_id;          /* Unique event identifier */
    CDCEventType event_type; /* Type of change */

    /* Source information */
    char *schema_name; /* Schema name */
    char *table_name;  /* Table name */
    Oid table_oid;     /* Table OID */

    /* Transaction information */
    TransactionId xid;      /* Transaction ID */
    XLogRecPtr lsn;         /* Log sequence number */
    TimestampTz event_time; /* When event occurred */

    /* Row data */
    int num_columns;        /* Number of columns */
    CDCColumnValue *before; /* Before image (for UPDATE/DELETE) */
    CDCColumnValue *after;  /* After image (for INSERT/UPDATE) */

    /* Primary key */
    int num_key_columns; /* Number of key columns */
    char **key_columns;  /* Key column names */
    char **key_values;   /* Key column values */

    /* Additional metadata */
    char *origin;        /* Origin database/node */
    int64 sequence;      /* Sequence number within transaction */
    bool is_last_in_txn; /* Last event in transaction? */
} CDCEvent;

/* ============================================================
 * CDC Sink Configuration Structures
 * ============================================================ */

/*
 * CDCKafkaSinkConfig - Kafka sink configuration
 */
typedef struct CDCKafkaSinkConfig {
    char *bootstrap_servers; /* Kafka broker list */
    char *topic;             /* Target topic */
    char *key_field;         /* Field to use as message key */
    int partition;           /* Specific partition (-1 for auto) */
    int batch_size;          /* Messages per batch */
    int linger_ms;           /* Batch linger time */
    char *compression_type;  /* none, gzip, snappy, lz4, zstd */
    char *security_protocol; /* PLAINTEXT, SSL, SASL_SSL */
    char *sasl_mechanism;    /* PLAIN, SCRAM-SHA-256, etc. */
    char *sasl_username;     /* SASL username */
    char *sasl_password;     /* SASL password */
    int acks;                /* Producer acks (0, 1, -1) */
    int retries;             /* Number of retries */
    int retry_backoff_ms;    /* Retry backoff time */
} CDCKafkaSinkConfig;

/*
 * CDCWebhookSinkConfig - HTTP webhook sink configuration
 */
typedef struct CDCWebhookSinkConfig {
    char *url;              /* Webhook URL */
    char *method;           /* HTTP method (POST, PUT) */
    char *content_type;     /* Content-Type header */
    char **headers;         /* Additional headers (key=value) */
    int num_headers;        /* Number of additional headers */
    char *auth_type;        /* none, basic, bearer, api_key */
    char *auth_credentials; /* Authentication credentials */
    int timeout_ms;         /* Request timeout */
    int retry_count;        /* Number of retries */
    int retry_delay_ms;     /* Delay between retries */
    bool batch_mode;        /* Send events in batches? */
    int batch_size;         /* Events per batch */
    int batch_timeout_ms;   /* Batch timeout */
} CDCWebhookSinkConfig;

/*
 * CDCFileSinkConfig - File sink configuration (debugging)
 */
typedef struct CDCFileSinkConfig {
    char *file_path;        /* Output file path */
    char *rotation_pattern; /* Log rotation pattern */
    int64 max_file_size;    /* Max file size before rotation */
    int max_files;          /* Max rotated files to keep */
    bool append_mode;       /* Append or overwrite */
    bool flush_immediate;   /* Flush after each event */
    bool pretty_print;      /* Pretty-print JSON */
} CDCFileSinkConfig;

/* ============================================================
 * CDC Subscription Structures
 * ============================================================ */

/*
 * CDCTableFilter - Filter for specific tables
 */
typedef struct CDCTableFilter {
    char *schema_pattern; /* Schema pattern (supports wildcards) */
    char *table_pattern;  /* Table pattern (supports wildcards) */
    bool include;         /* Include (true) or exclude (false) */
} CDCTableFilter;

/*
 * CDCEventFilter - Filter for event types
 */
typedef struct CDCEventFilter {
    bool include_insert;   /* Include INSERT events */
    bool include_update;   /* Include UPDATE events */
    bool include_delete;   /* Include DELETE events */
    bool include_truncate; /* Include TRUNCATE events */
    bool include_ddl;      /* Include DDL events */
    bool include_begin;    /* Include BEGIN events */
    bool include_commit;   /* Include COMMIT events */
} CDCEventFilter;

/*
 * CDCSubscription - Complete subscription configuration
 */
typedef struct CDCSubscription {
    int64 subscription_id;   /* Unique subscription identifier */
    char *subscription_name; /* Human-readable name */

    /* Sink configuration */
    CDCSinkType sink_type; /* Type of sink */
    union {
        CDCKafkaSinkConfig *kafka;
        CDCWebhookSinkConfig *webhook;
        CDCFileSinkConfig *file;
    } sink_config;

    /* Output configuration */
    CDCOutputFormat output_format; /* Event output format */
    bool include_schema;           /* Include schema in events? */
    bool include_before;           /* Include before image? */
    bool include_transaction;      /* Include transaction info? */

    /* Filtering */
    int num_table_filters;         /* Number of table filters */
    CDCTableFilter *table_filters; /* Table filters */
    CDCEventFilter *event_filter;  /* Event type filter */

    /* State tracking */
    CDCSubscriptionState state; /* Current state */
    XLogRecPtr start_lsn;       /* Starting LSN */
    XLogRecPtr confirmed_lsn;   /* Last confirmed LSN */
    TransactionId restart_xid;  /* Restart transaction ID */

    /* Timestamps */
    TimestampTz created_at; /* Creation timestamp */
    TimestampTz started_at; /* Last start timestamp */
    TimestampTz stopped_at; /* Last stop timestamp */

    /* Statistics */
    int64 events_sent;           /* Total events sent */
    int64 events_failed;         /* Failed events */
    int64 bytes_sent;            /* Total bytes sent */
    TimestampTz last_event_time; /* Time of last event */
    TimestampTz last_error_time; /* Time of last error */
    char *last_error_message;    /* Last error message */

    /* Worker info */
    int worker_pid; /* Background worker PID */

    /* Memory context */
    MemoryContext cdc_context; /* Memory context for CDC */
} CDCSubscription;

/* ============================================================
 * CDC Statistics Structure
 * ============================================================ */

typedef struct CDCStats {
    int64 subscription_id;
    int64 events_captured;         /* Events captured from WAL */
    int64 events_sent;             /* Events sent to sink */
    int64 events_failed;           /* Failed to send */
    int64 events_filtered;         /* Events filtered out */
    int64 bytes_sent;              /* Total bytes sent */
    double avg_latency_ms;         /* Average delivery latency */
    double throughput_per_sec;     /* Events per second */
    TimestampTz last_capture_time; /* Last WAL capture time */
    TimestampTz last_send_time;    /* Last successful send time */
    XLogRecPtr current_lsn;        /* Current processing LSN */
    XLogRecPtr confirmed_lsn;      /* Confirmed delivery LSN */
} CDCStats;

/* ============================================================
 * CDC Shared Memory State
 * ============================================================ */

typedef struct CDCRegistryEntry {
    int64 subscription_id;
    CDCSubscriptionState state;
    int worker_pid;
    int64 events_sent;
    XLogRecPtr current_lsn;
    XLogRecPtr confirmed_lsn;
    TimestampTz last_activity;
    bool is_active;
} CDCRegistryEntry;

/* ============================================================
 * Function Prototypes - CDC Subscription Management
 * ============================================================ */

/* Subscription lifecycle */
extern int64 orochi_create_cdc_subscription(const char *name, CDCSinkType sink_type,
                                            CDCOutputFormat format, const char *options_json);

extern bool orochi_start_cdc_subscription(int64 subscription_id);
extern bool orochi_pause_cdc_subscription(int64 subscription_id);
extern bool orochi_resume_cdc_subscription(int64 subscription_id);
extern bool orochi_stop_cdc_subscription(int64 subscription_id);
extern bool orochi_drop_cdc_subscription(int64 subscription_id, bool if_exists);

/* Subscription configuration */
extern CDCSubscription *orochi_get_cdc_subscription(int64 subscription_id);
extern List *orochi_list_cdc_subscriptions(void);
extern bool orochi_update_cdc_subscription(int64 subscription_id, const char *options_json);

/* Subscription filtering */
extern bool orochi_add_cdc_table_filter(int64 subscription_id, const char *schema_pattern,
                                        const char *table_pattern, bool include);
extern bool orochi_remove_cdc_table_filter(int64 subscription_id, int filter_index);

/* Subscription status */
extern CDCSubscriptionState orochi_get_cdc_subscription_state(int64 subscription_id);
extern CDCStats *orochi_get_cdc_stats(int64 subscription_id);
extern void orochi_reset_cdc_stats(int64 subscription_id);

/* ============================================================
 * Function Prototypes - CDC Event Processing
 * ============================================================ */

/* Event capture */
extern void cdc_process_wal_record(XLogReaderState *record);
extern void cdc_process_raft_entry(struct RaftLogEntry *entry);
extern CDCEvent *cdc_create_event(CDCEventType type, Oid table_oid, HeapTuple old_tuple,
                                  HeapTuple new_tuple);
extern void cdc_free_event(CDCEvent *event);

/* Event filtering */
extern bool cdc_event_matches_subscription(CDCEvent *event, CDCSubscription *sub);
extern bool cdc_table_matches_filter(const char *schema, const char *table, CDCTableFilter *filters,
                                     int num_filters);

/* Event serialization */
extern char *cdc_serialize_event_json(CDCEvent *event, CDCSubscription *sub);
extern char *cdc_serialize_event_debezium(CDCEvent *event, CDCSubscription *sub);
extern char *cdc_serialize_event_avro(CDCEvent *event, CDCSubscription *sub);

/* ============================================================
 * Function Prototypes - CDC Sink Operations
 * ============================================================ */

/* Sink initialization */
extern void *cdc_sink_init(CDCSubscription *sub);
extern void cdc_sink_close(void *sink_context, CDCSinkType sink_type);

/* Event delivery */
extern bool cdc_sink_send_event(void *sink_context, CDCSinkType sink_type, const char *event_data,
                                int event_size, const char *key, int key_size);
extern bool cdc_sink_send_batch(void *sink_context, CDCSinkType sink_type, char **events,
                                int *event_sizes, int count);
extern bool cdc_sink_flush(void *sink_context, CDCSinkType sink_type);

/* Kafka sink */
extern void *cdc_kafka_sink_init(CDCKafkaSinkConfig *config);
extern bool cdc_kafka_sink_send(void *producer, const char *topic, const char *key, int key_size,
                                const char *value, int value_size);
extern void cdc_kafka_sink_close(void *producer);

/* Webhook sink */
extern void *cdc_webhook_sink_init(CDCWebhookSinkConfig *config);
extern bool cdc_webhook_sink_send(void *context, const char *data, int size);
extern void cdc_webhook_sink_close(void *context);

/* File sink */
extern void *cdc_file_sink_init(CDCFileSinkConfig *config);
extern bool cdc_file_sink_write(void *context, const char *data, int size);
extern void cdc_file_sink_close(void *context);

/* ============================================================
 * Function Prototypes - Background Worker
 * ============================================================ */

extern void cdc_worker_main(Datum main_arg);
extern void orochi_cdc_shmem_init(void);
extern Size orochi_cdc_shmem_size(void);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_cdc_subscription_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_cdc_subscription_sql(PG_FUNCTION_ARGS);
extern Datum orochi_start_cdc_subscription_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stop_cdc_subscription_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_cdc_subscriptions_sql(PG_FUNCTION_ARGS);
extern Datum orochi_cdc_subscription_stats_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *cdc_event_type_name(CDCEventType type);
extern CDCEventType cdc_parse_event_type(const char *name);
extern const char *cdc_sink_type_name(CDCSinkType type);
extern CDCSinkType cdc_parse_sink_type(const char *name);
extern const char *cdc_subscription_state_name(CDCSubscriptionState state);
extern const char *cdc_output_format_name(CDCOutputFormat format);
extern CDCOutputFormat cdc_parse_output_format(const char *name);

/* ============================================================
 * Constants
 * ============================================================ */

#define CDC_DEFAULT_BATCH_SIZE         1000
#define CDC_DEFAULT_BATCH_TIMEOUT_MS   100
#define CDC_DEFAULT_RETRY_COUNT        3
#define CDC_DEFAULT_RETRY_DELAY_MS     1000
#define CDC_DEFAULT_WEBHOOK_TIMEOUT_MS 30000
#define CDC_MAX_SUBSCRIPTION_NAME      128
#define CDC_MAX_ERROR_MESSAGE          1024
#define CDC_MAX_SUBSCRIPTIONS          64

/* Kafka defaults */
#define CDC_KAFKA_DEFAULT_BATCH_SIZE       1000
#define CDC_KAFKA_DEFAULT_LINGER_MS        5
#define CDC_KAFKA_DEFAULT_ACKS             -1
#define CDC_KAFKA_DEFAULT_RETRIES          3
#define CDC_KAFKA_DEFAULT_RETRY_BACKOFF_MS 100

/* File sink defaults */
#define CDC_FILE_DEFAULT_MAX_SIZE  (100 * 1024 * 1024) /* 100MB */
#define CDC_FILE_DEFAULT_MAX_FILES 10

#endif /* OROCHI_CDC_H */
