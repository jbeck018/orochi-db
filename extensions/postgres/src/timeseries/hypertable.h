/*-------------------------------------------------------------------------
 *
 * hypertable.h
 *    Orochi DB time-series hypertable implementation
 *
 * Provides:
 *   - Automatic time-based partitioning
 *   - Chunk creation and management
 *   - Time bucketing functions
 *   - Continuous aggregates
 *   - Retention policies
 *   - Compression policies
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_HYPERTABLE_H
#define OROCHI_HYPERTABLE_H

#include "../orochi.h"
#include "postgres.h"
#include "utils/timestamp.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define DEFAULT_CHUNK_INTERVAL_USEC (USECS_PER_DAY)
#define MIN_CHUNK_INTERVAL_USEC     (USECS_PER_HOUR)
#define MAX_CHUNK_INTERVAL_USEC     (USECS_PER_DAY * 365)
#define CHUNK_PREFIX                "_orochi_chunk_"
#define MAX_CHUNK_NAME_LEN          64

/* ============================================================
 * Structures
 * ============================================================ */

typedef enum DimensionType { DIMENSION_TIME = 0, DIMENSION_SPACE } DimensionType;

typedef struct HypertableDimension {
    int32 dimension_id;
    Oid hypertable_oid;
    char *column_name;
    Oid column_type;
    DimensionType dim_type;
    int64 interval_length;
    int32 num_slices;
} HypertableDimension;

typedef struct ChunkConstraint {
    int64 chunk_id;
    int32 dimension_id;
    int64 range_start;
    int64 range_end;
} ChunkConstraint;

typedef struct HypertableInfo {
    Oid hypertable_oid;
    char *schema_name;
    char *table_name;
    int32 num_dimensions;
    HypertableDimension *dimensions;
    int64 chunk_count;
    bool compression_enabled;
    Interval *compress_after;
    bool is_distributed;
} HypertableInfo;

typedef struct ChunkInfo {
    int64 chunk_id;
    Oid hypertable_oid;
    Oid chunk_table_oid;
    char *chunk_schema;
    char *chunk_name;
    int32 num_constraints;
    ChunkConstraint *constraints;
    bool is_compressed;
    OrochiStorageTier storage_tier;
    int64 row_count;
    int64 size_bytes;
    TimestampTz created_at;
} ChunkInfo;

typedef struct ContinuousAggregate {
    int64 agg_id;
    Oid view_oid;
    Oid source_hypertable;
    Oid materialization_table;
    char *view_schema;
    char *view_name;
    char *query_text;
    char *bucket_column;
    int64 bucket_width;
    Interval *refresh_interval;
    TimestampTz last_refresh;
    TimestampTz invalidation_threshold;
    bool enabled;
} ContinuousAggregate;

typedef struct RetentionPolicy {
    int64 policy_id;
    Oid hypertable_oid;
    Interval *drop_after;
    bool enabled;
    TimestampTz last_run;
} RetentionPolicy;

typedef struct CompressionPolicy {
    int64 policy_id;
    Oid hypertable_oid;
    Interval *compress_after;
    char *segment_by;
    char *order_by;
    bool enabled;
    TimestampTz last_run;
} CompressionPolicy;

/* ============================================================
 * Hypertable Management
 * ============================================================ */

extern void orochi_create_hypertable(Oid table_oid, const char *time_column,
                                     Interval *chunk_interval, bool if_not_exists);
extern void orochi_create_hypertable_with_space(Oid table_oid, const char *time_column,
                                                Interval *chunk_interval, const char *space_column,
                                                int num_partitions);
extern void orochi_add_dimension(Oid hypertable_oid, const char *column_name, int num_partitions,
                                 Interval *interval);
extern void orochi_register_dimension(Oid hypertable_oid, const char *column_name,
                                      int dimension_type, int num_partitions,
                                      int64 interval_length);
extern HypertableInfo *orochi_get_hypertable_info(Oid hypertable_oid);
extern bool orochi_is_hypertable(Oid table_oid);
extern void orochi_drop_hypertable(Oid hypertable_oid, bool cascade);

/* ============================================================
 * Chunk Management
 * ============================================================ */

extern ChunkInfo *orochi_get_chunk_for_timestamp(Oid hypertable_oid, TimestampTz ts);
extern ChunkInfo *orochi_get_chunk_for_point(Oid hypertable_oid, Datum *dimension_values,
                                             int num_dimensions);
extern List *orochi_get_hypertable_chunks(Oid hypertable_oid);
extern List *orochi_get_chunks_in_time_range(Oid hypertable_oid, TimestampTz start,
                                             TimestampTz end);
extern void orochi_compress_chunk(int64 chunk_id);
extern void orochi_decompress_chunk(int64 chunk_id);
extern int orochi_drop_chunks_older_than(Oid hypertable_oid, Interval *older_than);
extern void orochi_drop_chunk(int64 chunk_id);
extern void orochi_reorder_chunk(int64 chunk_id, const char *index_name);
extern void orochi_move_chunk(int64 chunk_id, const char *tablespace);

/* ============================================================
 * Time Bucketing
 * ============================================================ */

extern TimestampTz orochi_time_bucket(Interval *bucket_width, TimestampTz ts);
extern TimestampTz orochi_time_bucket_offset(Interval *bucket_width, TimestampTz ts,
                                             Interval *offset);
extern TimestampTz orochi_time_bucket_origin(Interval *bucket_width, TimestampTz ts,
                                             TimestampTz origin);
extern int64 orochi_time_bucket_int(int64 bucket_width, int64 value);

/* ============================================================
 * Continuous Aggregates
 * ============================================================ */

extern void orochi_create_continuous_aggregate(const char *view_name, const char *query,
                                               Interval *refresh_interval, bool with_data);
extern void orochi_refresh_continuous_aggregate(Oid view_oid, TimestampTz start, TimestampTz end);
extern void orochi_set_continuous_aggregate_policy(Oid view_oid, Interval *start_offset,
                                                   Interval *end_offset,
                                                   Interval *schedule_interval);
extern ContinuousAggregate *orochi_get_continuous_aggregate(Oid view_oid);
extern void orochi_drop_continuous_aggregate(Oid view_oid);
extern void orochi_log_invalidation(Oid hypertable_oid, TimestampTz start, TimestampTz end);
extern List *orochi_get_invalidations(Oid agg_id);

/* ============================================================
 * Policies
 * ============================================================ */

extern void orochi_add_retention_policy(Oid hypertable_oid, Interval *drop_after);
extern void orochi_remove_retention_policy(Oid hypertable_oid);
extern void orochi_add_compression_policy(Oid hypertable_oid, Interval *compress_after);
extern void orochi_remove_compression_policy(Oid hypertable_oid);
extern void orochi_enable_compression(Oid hypertable_oid, const char *segment_by,
                                      const char *order_by);
extern void orochi_run_maintenance(void);

/* ============================================================
 * Query Optimization
 * ============================================================ */

extern List *orochi_get_chunk_exclusion_info(Oid hypertable_oid, List *predicates);
extern double orochi_estimate_chunk_rows(int64 chunk_id);
extern void orochi_get_chunk_stats(int64 chunk_id, int64 *row_count, int64 *size_bytes);

/* ============================================================
 * Distributed Hypertables
 * ============================================================ */

extern void orochi_create_distributed_hypertable(Oid table_oid, const char *time_column,
                                                 const char *partition_column,
                                                 int replication_factor);
extern void orochi_attach_data_node(Oid hypertable_oid, int node_id);
extern void orochi_detach_data_node(Oid hypertable_oid, int node_id);

#endif /* OROCHI_HYPERTABLE_H */
