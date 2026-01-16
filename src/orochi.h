/*-------------------------------------------------------------------------
 *
 * orochi.h
 *    Main header for Orochi DB - A Modern HTAP PostgreSQL Extension
 *
 * Orochi DB combines:
 *   - Automatic sharding (inspired by Citus)
 *   - Time-series optimization (inspired by TimescaleDB)
 *   - Columnar storage (inspired by Hydra)
 *   - Tiered storage (hot/cold with S3)
 *   - AI/Vector workload support
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_H
#define OROCHI_H

#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "storage/lwlock.h"
#include "utils/relcache.h"
#include "utils/timestamp.h"

/* Version information */
#define OROCHI_VERSION_MAJOR    1
#define OROCHI_VERSION_MINOR    0
#define OROCHI_VERSION_PATCH    0
#define OROCHI_VERSION_STRING   "1.0.0"

/* PostgreSQL version requirements */
#define OROCHI_MIN_PG_MAJOR     16
#define OROCHI_MAX_PG_MAJOR     18

/* Extension name for identification */
#define OROCHI_EXTENSION_NAME   "orochi"
#define OROCHI_SCHEMA_NAME      "orochi"

/*
 * Storage Engine Types
 * Orochi supports multiple storage formats for different workloads
 */
typedef enum OrochiStorageType
{
    OROCHI_STORAGE_ROW = 0,      /* Standard row-based (PostgreSQL heap) */
    OROCHI_STORAGE_COLUMNAR,      /* Columnar storage for analytics */
    OROCHI_STORAGE_HYBRID,        /* Hybrid row-columnar (hot/cold) */
    OROCHI_STORAGE_VECTOR         /* Optimized for vector/embedding data */
} OrochiStorageType;

/*
 * Compression algorithms supported
 */
typedef enum OrochiCompressionType
{
    OROCHI_COMPRESS_NONE = 0,
    OROCHI_COMPRESS_LZ4,          /* Fast compression */
    OROCHI_COMPRESS_ZSTD,         /* Best ratio */
    OROCHI_COMPRESS_PGLZ,         /* PostgreSQL native */
    OROCHI_COMPRESS_DELTA,        /* Delta encoding for integers */
    OROCHI_COMPRESS_GORILLA,      /* Float compression (Facebook) */
    OROCHI_COMPRESS_DICTIONARY,   /* Dictionary encoding */
    OROCHI_COMPRESS_RLE           /* Run-length encoding */
} OrochiCompressionType;

/*
 * Sharding strategies
 */
typedef enum OrochiShardStrategy
{
    OROCHI_SHARD_HASH = 0,        /* Hash-based distribution */
    OROCHI_SHARD_RANGE,           /* Range-based partitioning */
    OROCHI_SHARD_LIST,            /* List-based partitioning */
    OROCHI_SHARD_COMPOSITE,       /* Multi-dimensional */
    OROCHI_SHARD_REFERENCE        /* Replicated reference table */
} OrochiShardStrategy;

/*
 * Storage tier levels for data lifecycle
 */
typedef enum OrochiStorageTier
{
    OROCHI_TIER_HOT = 0,          /* Active data - local SSD */
    OROCHI_TIER_WARM,             /* Recent data - local disk */
    OROCHI_TIER_COLD,             /* Archived data - S3/object storage */
    OROCHI_TIER_FROZEN            /* Long-term archive - S3 Glacier */
} OrochiStorageTier;

/*
 * Node roles in distributed cluster
 */
typedef enum OrochiNodeRole
{
    OROCHI_NODE_COORDINATOR = 0,  /* Query coordinator */
    OROCHI_NODE_WORKER,           /* Data worker node */
    OROCHI_NODE_REPLICA           /* Read replica */
} OrochiNodeRole;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiTable - Extended table metadata
 */
typedef struct OrochiTableInfo
{
    Oid                 relid;              /* PostgreSQL relation OID */
    char               *schema_name;         /* Schema name */
    char               *table_name;          /* Table name */
    OrochiStorageType   storage_type;        /* Storage engine type */
    OrochiShardStrategy shard_strategy;      /* Sharding strategy */
    int                 shard_count;         /* Number of shards */
    char               *distribution_column; /* Distribution key column */
    bool                is_distributed;      /* Is table distributed? */
    bool                is_timeseries;       /* Is time-series table? */
    char               *time_column;         /* Time partitioning column */
    Interval           *chunk_interval;      /* Time chunk size */
    OrochiCompressionType compression;       /* Compression algorithm */
    int                 compression_level;   /* Compression level (1-19) */
} OrochiTableInfo;

/*
 * OrochiShard - Individual shard information
 */
typedef struct OrochiShardInfo
{
    int64               shard_id;           /* Unique shard identifier */
    Oid                 table_oid;          /* Parent table OID */
    int32               shard_index;        /* Shard number (0-based) */
    int32               hash_min;           /* Minimum hash value */
    int32               hash_max;           /* Maximum hash value */
    int32               node_id;            /* Worker node ID */
    char               *node_host;          /* Worker hostname */
    int                 node_port;          /* Worker port */
    int64               row_count;          /* Estimated row count */
    int64               size_bytes;         /* Shard size in bytes */
    OrochiStorageTier   storage_tier;       /* Current storage tier */
    TimestampTz         created_at;         /* Creation timestamp */
    TimestampTz         last_accessed;      /* Last access timestamp */
} OrochiShardInfo;

/*
 * OrochiChunk - Time-series chunk information
 */
typedef struct OrochiChunkInfo
{
    int64               chunk_id;           /* Unique chunk identifier */
    Oid                 hypertable_oid;     /* Parent hypertable OID */
    int32               dimension_id;       /* Dimension identifier */
    TimestampTz         range_start;        /* Time range start */
    TimestampTz         range_end;          /* Time range end */
    int64               row_count;          /* Row count in chunk */
    int64               size_bytes;         /* Chunk size */
    bool                is_compressed;      /* Compression status */
    OrochiStorageTier   storage_tier;       /* Current storage tier */
    char               *tablespace;         /* Tablespace name */
} OrochiChunkInfo;

/*
 * OrochiNode - Cluster node information
 */
typedef struct OrochiNodeInfo
{
    int32               node_id;            /* Unique node identifier */
    char               *hostname;           /* Node hostname */
    int                 port;               /* PostgreSQL port */
    OrochiNodeRole      role;               /* Node role */
    bool                is_active;          /* Is node active? */
    int64               shard_count;        /* Shards on this node */
    int64               total_size;         /* Total data size */
    double              cpu_usage;          /* CPU utilization */
    double              memory_usage;       /* Memory utilization */
    TimestampTz         last_heartbeat;     /* Last health check */
} OrochiNodeInfo;

/*
 * OrochiStripe - Columnar storage stripe
 */
typedef struct OrochiStripeInfo
{
    int64               stripe_id;          /* Unique stripe identifier */
    Oid                 table_oid;          /* Parent table OID */
    int64               first_row;          /* First row number */
    int64               row_count;          /* Rows in stripe */
    int32               column_count;       /* Number of columns */
    int64               data_size;          /* Compressed data size */
    int64               metadata_size;      /* Metadata size */
    OrochiCompressionType compression;      /* Compression used */
    bool                is_flushed;         /* Is stripe flushed? */
} OrochiStripeInfo;

/*
 * OrochiColumnChunk - Column data within a stripe
 */
typedef struct OrochiColumnChunk
{
    int64               chunk_id;           /* Chunk identifier */
    int64               stripe_id;          /* Parent stripe ID */
    int32               chunk_group_index;  /* Chunk group within stripe */
    int16               column_index;       /* Column position */
    Oid                 data_type;          /* PostgreSQL data type OID */
    int64               value_count;        /* Number of values */
    int64               null_count;         /* Number of nulls */
    int64               compressed_size;    /* Compressed size */
    int64               decompressed_size;  /* Original size */
    int64               uncompressed_size;  /* Alias for decompressed_size */
    OrochiCompressionType compression;      /* Compression type */
    OrochiCompressionType compression_type; /* Alias for compression */
    Datum               min_value;          /* Minimum value */
    Datum               max_value;          /* Maximum value */
    bool                has_nulls;          /* Contains nulls? */
} OrochiColumnChunk;

/*
 * OrochiVectorIndex - Vector/embedding index info
 */
typedef struct OrochiVectorIndex
{
    Oid                 index_oid;          /* PostgreSQL index OID */
    Oid                 table_oid;          /* Table OID */
    int16               vector_column;      /* Vector column index */
    int32               dimensions;         /* Vector dimensions */
    int32               lists;              /* IVF lists (if applicable) */
    int32               probes;             /* Search probes */
    char               *distance_type;      /* l2, cosine, inner_product */
    char               *index_type;         /* hnsw, ivfflat, etc. */
} OrochiVectorIndex;

/*
 * S3/Object Storage configuration
 */
typedef struct OrochiS3Config
{
    char               *endpoint;           /* S3 endpoint URL */
    char               *bucket;             /* Bucket name */
    char               *prefix;             /* Key prefix */
    char               *access_key;         /* AWS access key */
    char               *secret_key;         /* AWS secret key */
    char               *region;             /* AWS region */
    bool                use_ssl;            /* Use HTTPS */
    int                 connection_timeout; /* Timeout in ms */
} OrochiS3Config;

/*
 * Storage tiering policy
 */
typedef struct OrochiTieringPolicy
{
    int64               policy_id;          /* Policy identifier */
    Oid                 table_oid;          /* Target table */
    Interval           *hot_to_warm;        /* Hot->Warm threshold */
    Interval           *warm_to_cold;       /* Warm->Cold threshold */
    Interval           *cold_to_frozen;     /* Cold->Frozen threshold */
    bool                compress_on_tier;   /* Compress when tiering */
    bool                enabled;            /* Policy active? */
} OrochiTieringPolicy;

/*
 * Continuous aggregate metadata
 */
typedef struct OrochiContinuousAggInfo
{
    int64               agg_id;             /* Unique aggregate ID */
    Oid                 view_oid;           /* View OID */
    Oid                 source_table_oid;   /* Source hypertable OID */
    Oid                 materialization_table_oid; /* Materialization table */
    char               *query_text;         /* Original query text */
    Interval           *refresh_interval;   /* Auto-refresh interval */
    TimestampTz         last_refresh;       /* Last refresh timestamp */
    bool                enabled;            /* Is aggregate active? */
} OrochiContinuousAggInfo;

/*
 * Invalidation log entry for continuous aggregates
 */
typedef struct OrochiInvalidationEntry
{
    int64               log_id;             /* Log entry ID */
    int64               agg_id;             /* Continuous aggregate ID */
    TimestampTz         range_start;        /* Invalidated range start */
    TimestampTz         range_end;          /* Invalidated range end */
    TimestampTz         created_at;         /* When logged */
} OrochiInvalidationEntry;

/* ============================================================
 * Function Declarations
 * ============================================================ */

/* Initialization */
extern void _PG_init(void);
extern void _PG_fini(void);
extern void orochi_init(void);

/* Table operations */
extern OrochiTableInfo *orochi_get_table_info(Oid relid);
extern void orochi_create_distributed_table(Oid relid, const char *distribution_column,
                                            OrochiShardStrategy strategy, int shard_count);
extern void orochi_create_hypertable(Oid relid, const char *time_column, Interval *chunk_interval, bool if_not_exists);
extern void orochi_set_storage_type(Oid relid, OrochiStorageType storage_type);

/* Shard management */
extern List *orochi_get_shards(Oid table_oid);
extern OrochiShardInfo *orochi_get_shard_for_value(Oid table_oid, Datum distribution_value);
extern void orochi_rebalance_shards(Oid table_oid);
extern void orochi_split_shard(int64 shard_id);
extern void orochi_merge_shards(int64 shard1_id, int64 shard2_id);

/* Chunk management */
extern List *orochi_get_chunks(Oid hypertable_oid);
extern void orochi_compress_chunk(int64 chunk_id);
extern void orochi_decompress_chunk(int64 chunk_id);
extern void orochi_drop_chunks(Oid hypertable_oid, TimestampTz older_than);

/* Columnar storage */
extern void orochi_convert_to_columnar(Oid relid);
extern void orochi_flush_columnar_writes(Oid relid);
extern OrochiStripeInfo *orochi_create_stripe(Oid relid);

/* Tiered storage */
extern void orochi_move_to_tier(int64 chunk_id, OrochiStorageTier tier);
extern void orochi_apply_tiering_policies(void);
extern OrochiS3Config *orochi_get_s3_config(void);

/* Vector operations */
extern void orochi_create_vector_index(Oid relid, const char *column,
                                       int dimensions, const char *index_type,
                                       const char *distance_type);

/* Cluster management */
extern List *orochi_get_nodes(void);
extern void orochi_add_node(const char *hostname, int port, OrochiNodeRole role);
extern void orochi_remove_node(int node_id);

/* Query execution */
extern void orochi_planner_hook_init(void);
extern void orochi_executor_hook_init(void);

/* Utility functions */
extern int32 orochi_hash_value(Datum value, Oid type_oid);
extern char *orochi_compression_type_name(OrochiCompressionType type);
extern char *orochi_storage_tier_name(OrochiStorageTier tier);

/* GUC variables */
extern int orochi_default_shard_count;
extern int orochi_stripe_row_limit;
extern int orochi_chunk_time_interval_hours;
extern int orochi_compression_level;
extern char *orochi_s3_endpoint;
extern char *orochi_s3_bucket;
extern bool orochi_enable_vectorized_execution;
extern bool orochi_enable_parallel_query;
extern int orochi_parallel_workers;

/* ============================================================
 * JSON Semi-Structured Data Support
 * ============================================================ */

/*
 * JSON Index Types
 */
typedef enum OrochiJsonIndexType
{
    OROCHI_JSON_INDEX_GIN = 0,        /* GIN index for containment */
    OROCHI_JSON_INDEX_BTREE,          /* B-tree for scalar paths */
    OROCHI_JSON_INDEX_HASH,           /* Hash for equality */
    OROCHI_JSON_INDEX_EXPRESSION,     /* Expression index */
    OROCHI_JSON_INDEX_PARTIAL         /* Partial index */
} OrochiJsonIndexType;

/*
 * JSON Path Value Types
 */
typedef enum OrochiJsonPathType
{
    OROCHI_JSON_PATH_NULL = 0,
    OROCHI_JSON_PATH_STRING,
    OROCHI_JSON_PATH_NUMBER,
    OROCHI_JSON_PATH_BOOLEAN,
    OROCHI_JSON_PATH_ARRAY,
    OROCHI_JSON_PATH_OBJECT,
    OROCHI_JSON_PATH_MIXED
} OrochiJsonPathType;

/*
 * JSON Columnar Storage Type
 */
typedef enum OrochiJsonColumnarType
{
    OROCHI_JSON_COLUMNAR_INLINE = 0,  /* Small values inline */
    OROCHI_JSON_COLUMNAR_DICTIONARY,  /* Dictionary encoded */
    OROCHI_JSON_COLUMNAR_DELTA,       /* Delta encoded */
    OROCHI_JSON_COLUMNAR_RLE,         /* Run-length encoded */
    OROCHI_JSON_COLUMNAR_RAW          /* Raw uncompressed */
} OrochiJsonColumnarType;

/*
 * OrochiJsonIndexInfo - JSON index metadata
 */
typedef struct OrochiJsonIndexInfo
{
    Oid                     index_oid;          /* PostgreSQL index OID */
    Oid                     table_oid;          /* Parent table OID */
    int16                   column_attnum;      /* JSONB column */
    char                   *index_name;         /* Index name */
    OrochiJsonIndexType     index_type;         /* Type of index */
    char                  **paths;              /* Indexed paths */
    int32                   path_count;         /* Number of paths */
    bool                    is_unique;          /* Unique index */
    int64                   size_bytes;         /* Index size */
    TimestampTz             created_at;         /* Creation time */
} OrochiJsonIndexInfo;

/*
 * OrochiJsonPathStats - JSON path statistics
 */
typedef struct OrochiJsonPathStats
{
    char                   *path;               /* JSON path */
    OrochiJsonPathType      value_type;         /* Value type */
    int64                   access_count;       /* Access count */
    int64                   null_count;         /* Null values */
    int64                   distinct_count;     /* Distinct values */
    double                  selectivity;        /* Selectivity estimate */
    double                  avg_value_size;     /* Average value size */
    bool                    is_indexed;         /* Has index */
    OrochiJsonIndexType     recommended_index;  /* Recommended index */
    TimestampTz             last_accessed;      /* Last access time */
} OrochiJsonPathStats;

/*
 * OrochiJsonExtractedColumn - Extracted JSON column metadata
 */
typedef struct OrochiJsonExtractedColumn
{
    int32                   column_id;          /* Column ID */
    Oid                     table_oid;          /* Table OID */
    int16                   source_attnum;      /* Source JSONB column */
    char                   *path;               /* Extraction path */
    Oid                     target_type;        /* Target type */
    OrochiJsonColumnarType  storage_type;       /* Storage type */
    int64                   row_count;          /* Row count */
    int64                   null_count;         /* Null count */
    double                  compression_ratio;  /* Compression ratio */
    TimestampTz             created_at;         /* Creation time */
} OrochiJsonExtractedColumn;

/* JSON Index Operations */
extern Oid orochi_json_create_index(Oid table_oid, int16 column_attnum,
                                    const char **paths, int path_count,
                                    OrochiJsonIndexType index_type);
extern void orochi_json_drop_index(Oid index_oid);
extern bool orochi_json_path_is_indexed(Oid table_oid, int16 column_attnum,
                                        const char *path);
extern List *orochi_json_get_indexes(Oid table_oid, int16 column_attnum);

/* JSON Path Statistics */
extern OrochiJsonPathStats *orochi_json_analyze_path(Oid table_oid,
                                                     int16 column_attnum,
                                                     const char *path);
extern List *orochi_json_analyze_all_paths(Oid table_oid, int16 column_attnum);
extern void orochi_json_record_path_access(Oid table_oid, int16 column_attnum,
                                           const char *path);

/* JSON Columnar Extraction */
extern List *orochi_json_extract_columns(Oid table_oid, int16 column_attnum,
                                         const char **paths, int path_count);
extern void orochi_json_refresh_columns(Oid table_oid, int16 column_attnum);
extern List *orochi_json_get_extracted_columns(Oid table_oid, int16 column_attnum);

/* JSON Query Optimization */
extern void orochi_json_planner_init(void);
extern void orochi_json_executor_init(void);

#endif /* OROCHI_H */
