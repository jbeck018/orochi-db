/*-------------------------------------------------------------------------
 *
 * catalog.h
 *    Orochi DB catalog management - metadata tables and operations
 *
 * The Orochi catalog stores:
 *   - Distributed table metadata
 *   - Shard placement information
 *   - Hypertable/chunk information
 *   - Node registry
 *   - Tiering policies
 *   - Vector index metadata
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_CATALOG_H
#define OROCHI_CATALOG_H

#include "postgres.h"
#include "../orochi.h"

/* ============================================================
 * Catalog Table Names
 * ============================================================ */

#define OROCHI_TABLES_TABLE           "orochi_tables"
#define OROCHI_SHARDS_TABLE           "orochi_shards"
#define OROCHI_SHARD_PLACEMENTS_TABLE "orochi_shard_placements"
#define OROCHI_CHUNKS_TABLE           "orochi_chunks"
#define OROCHI_NODES_TABLE            "orochi_nodes"
#define OROCHI_STRIPES_TABLE          "orochi_stripes"
#define OROCHI_COLUMN_CHUNKS_TABLE    "orochi_column_chunks"
#define OROCHI_TIERING_POLICIES_TABLE "orochi_tiering_policies"
#define OROCHI_VECTOR_INDEXES_TABLE   "orochi_vector_indexes"
#define OROCHI_CONTINUOUS_AGGS_TABLE  "orochi_continuous_aggregates"
#define OROCHI_INVALIDATION_LOG_TABLE "orochi_invalidation_log"

/* ============================================================
 * Catalog Initialization
 * ============================================================ */

/*
 * Initialize the Orochi catalog - create schema and metadata tables
 */
extern void orochi_catalog_init(void);

/*
 * Check if catalog is initialized
 */
extern bool orochi_catalog_exists(void);

/*
 * Get the Orochi schema OID
 */
extern Oid orochi_get_schema_oid(void);

/* ============================================================
 * Table Metadata Operations
 * ============================================================ */

/*
 * Register a table with Orochi
 */
extern void orochi_catalog_register_table(OrochiTableInfo *table_info);

/*
 * Get table metadata by OID
 */
extern OrochiTableInfo *orochi_catalog_get_table(Oid relid);

/*
 * Get table metadata by name
 */
extern OrochiTableInfo *orochi_catalog_get_table_by_name(const char *schema,
                                                          const char *table);

/*
 * Update table metadata
 */
extern void orochi_catalog_update_table(OrochiTableInfo *table_info);

/*
 * Remove table from Orochi catalog
 */
extern void orochi_catalog_remove_table(Oid relid);

/*
 * List all Orochi tables
 */
extern List *orochi_catalog_list_tables(void);

/*
 * Check if table is managed by Orochi
 */
extern bool orochi_catalog_is_orochi_table(Oid relid);

/* ============================================================
 * Shard Operations
 * ============================================================ */

/*
 * Create shard metadata entries
 */
extern void orochi_catalog_create_shards(Oid table_oid, int shard_count,
                                         OrochiShardStrategy strategy);

/*
 * Get shard information by ID
 */
extern OrochiShardInfo *orochi_catalog_get_shard(int64 shard_id);

/*
 * Get all shards for a table
 */
extern List *orochi_catalog_get_table_shards(Oid table_oid);

/*
 * Get shard for a hash value
 */
extern OrochiShardInfo *orochi_catalog_get_shard_for_hash(Oid table_oid, int32 hash_value);

/*
 * Update shard placement
 */
extern void orochi_catalog_update_shard_placement(int64 shard_id, int32 node_id);

/*
 * Update shard statistics
 */
extern void orochi_catalog_update_shard_stats(int64 shard_id, int64 row_count,
                                              int64 size_bytes);

/*
 * Record shard access for tiering decisions
 */
extern void orochi_catalog_record_shard_access(int64 shard_id);

/*
 * Create a single shard
 */
extern int64 orochi_catalog_create_shard(Oid table_oid, int32 hash_min,
                                         int32 hash_max, int32 node_id);

/*
 * Update shard hash range
 */
extern void orochi_catalog_update_shard_range(int64 shard_id, int32 hash_min,
                                              int32 hash_max);

/*
 * Delete a shard
 */
extern void orochi_catalog_delete_shard(int64 shard_id);

/* ============================================================
 * Chunk Operations (Time-series)
 * ============================================================ */

/*
 * Create a new chunk
 */
extern int64 orochi_catalog_create_chunk(Oid hypertable_oid,
                                         TimestampTz range_start,
                                         TimestampTz range_end);

/*
 * Get chunk by ID
 */
extern OrochiChunkInfo *orochi_catalog_get_chunk(int64 chunk_id);

/*
 * Get chunks for a hypertable
 */
extern List *orochi_catalog_get_hypertable_chunks(Oid hypertable_oid);

/*
 * Get chunks in time range
 */
extern List *orochi_catalog_get_chunks_in_range(Oid hypertable_oid,
                                                TimestampTz start,
                                                TimestampTz end);

/*
 * Update chunk compression status
 */
extern void orochi_catalog_update_chunk_compression(int64 chunk_id, bool is_compressed);

/*
 * Update chunk storage tier
 */
extern void orochi_catalog_update_chunk_tier(int64 chunk_id, OrochiStorageTier tier);

/*
 * Delete chunk metadata
 */
extern void orochi_catalog_delete_chunk(int64 chunk_id);

/* ============================================================
 * Node Operations
 * ============================================================ */

/*
 * Register a new node
 */
extern int32 orochi_catalog_add_node(const char *hostname, int port,
                                     OrochiNodeRole role);

/*
 * Get node information
 */
extern OrochiNodeInfo *orochi_catalog_get_node(int32 node_id);

/*
 * Get all active nodes
 */
extern List *orochi_catalog_get_active_nodes(void);

/*
 * Get nodes by role
 */
extern List *orochi_catalog_get_nodes_by_role(OrochiNodeRole role);

/*
 * Update node status
 */
extern void orochi_catalog_update_node_status(int32 node_id, bool is_active);

/*
 * Update node statistics
 */
extern void orochi_catalog_update_node_stats(int32 node_id, int64 shard_count,
                                             int64 total_size, double cpu_usage,
                                             double memory_usage);

/*
 * Record node heartbeat
 */
extern void orochi_catalog_record_heartbeat(int32 node_id);

/*
 * Remove node
 */
extern void orochi_catalog_remove_node(int32 node_id);

/* ============================================================
 * Columnar Storage Operations
 * ============================================================ */

/*
 * Create stripe metadata
 */
extern int64 orochi_catalog_create_stripe(Oid table_oid, int64 first_row,
                                          int64 row_count, int32 column_count);

/*
 * Get stripe information
 */
extern OrochiStripeInfo *orochi_catalog_get_stripe(int64 stripe_id);

/*
 * Get stripes for a table
 */
extern List *orochi_catalog_get_table_stripes(Oid table_oid);

/*
 * Update stripe status
 */
extern void orochi_catalog_update_stripe_status(int64 stripe_id, bool is_flushed,
                                                int64 data_size, int64 metadata_size);

/*
 * Create column chunk metadata
 */
extern void orochi_catalog_create_column_chunk(OrochiColumnChunk *chunk);

/*
 * Update column chunk min/max statistics
 */
extern void orochi_catalog_update_column_stats(int64 stripe_id, int16 column_index,
                                               Datum min_value, Datum max_value,
                                               Oid type_oid);

/*
 * Get column chunks for a stripe
 */
extern List *orochi_catalog_get_stripe_columns(int64 stripe_id);

/*
 * Read compressed chunk data from storage
 * Returns the compressed data buffer and sets size.
 * Caller is responsible for freeing the returned buffer.
 */
extern char *orochi_catalog_read_chunk_data(int64 stripe_id, int32 chunk_group_index,
                                            int16 column_index, int64 *data_size);

/*
 * Extended version of create_column_chunk that stores the data
 */
extern void orochi_catalog_create_column_chunk_with_data(int64 stripe_id,
                                                          int32 chunk_group_index,
                                                          int16 column_index,
                                                          OrochiColumnChunk *chunk,
                                                          const char *data,
                                                          int64 data_size);

/* ============================================================
 * Tiering Policy Operations
 * ============================================================ */

/*
 * Create tiering policy
 */
extern int64 orochi_catalog_create_tiering_policy(OrochiTieringPolicy *policy);

/*
 * Get tiering policy for table
 */
extern OrochiTieringPolicy *orochi_catalog_get_tiering_policy(Oid table_oid);

/*
 * Update tiering policy
 */
extern void orochi_catalog_update_tiering_policy(OrochiTieringPolicy *policy);

/*
 * Delete tiering policy
 */
extern void orochi_catalog_delete_tiering_policy(int64 policy_id);

/*
 * Get all active tiering policies
 */
extern List *orochi_catalog_get_active_tiering_policies(void);

/* ============================================================
 * Vector Index Operations
 * ============================================================ */

/*
 * Register vector index
 */
extern void orochi_catalog_register_vector_index(OrochiVectorIndex *index_info);

/*
 * Get vector index info
 */
extern OrochiVectorIndex *orochi_catalog_get_vector_index(Oid index_oid);

/*
 * Get vector indexes for table
 */
extern List *orochi_catalog_get_table_vector_indexes(Oid table_oid);

/* ============================================================
 * Continuous Aggregate Operations
 * ============================================================ */

/*
 * Register continuous aggregate
 */
extern int64 orochi_catalog_register_continuous_agg(Oid view_oid, Oid source_table,
                                                    Oid mat_table, const char *query_text);

/*
 * Get continuous aggregate info by view OID
 */
extern OrochiContinuousAggInfo *orochi_catalog_get_continuous_agg(Oid view_oid);

/*
 * Get continuous aggregates for source table
 */
extern List *orochi_catalog_get_continuous_aggs(Oid source_table);

/*
 * Update continuous aggregate refresh timestamp
 */
extern void orochi_catalog_update_continuous_agg_refresh(int64 agg_id, TimestampTz last_refresh);

/*
 * Delete continuous aggregate
 */
extern void orochi_catalog_delete_continuous_agg(Oid view_oid);

/*
 * Log invalidation for continuous aggregates
 */
extern void orochi_catalog_log_invalidation(Oid table_oid, TimestampTz start,
                                            TimestampTz end);

/*
 * Get pending invalidations for an aggregate
 */
extern List *orochi_catalog_get_pending_invalidations(Oid agg_oid);

/*
 * Clear invalidations up to a timestamp
 */
extern void orochi_catalog_clear_invalidations(Oid agg_oid, TimestampTz up_to);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Execute catalog query
 */
extern void orochi_catalog_execute(const char *query);

/*
 * Begin catalog transaction
 */
extern void orochi_catalog_begin_transaction(void);

/*
 * Commit catalog transaction
 */
extern void orochi_catalog_commit_transaction(void);

/*
 * Rollback catalog transaction
 */
extern void orochi_catalog_rollback_transaction(void);

#endif /* OROCHI_CATALOG_H */
