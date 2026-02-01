/*-------------------------------------------------------------------------
 *
 * distribution.h
 *    Orochi DB distributed table management
 *
 * This module handles:
 *   - Hash-based data distribution
 *   - Shard assignment and lookup
 *   - Co-location management
 *   - Reference table replication
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_DISTRIBUTION_H
#define OROCHI_DISTRIBUTION_H

#include "../orochi.h"
#include "postgres.h"

/* ============================================================
 * Hash Distribution Constants
 * ============================================================ */

#define OROCHI_HASH_SEED 0x9747b28c
#define OROCHI_HASH_MODULUS INT32_MAX

/* ============================================================
 * Co-location Group
 * ============================================================ */

typedef struct OrochiColocationGroup {
  int32 group_id;               /* Unique group identifier */
  int32 shard_count;            /* Number of shards */
  OrochiShardStrategy strategy; /* Distribution strategy */
  char *distribution_type;      /* Column type name */
} OrochiColocationGroup;

/* ============================================================
 * Distribution Functions
 * ============================================================ */

/*
 * Check if a table is distributed by Orochi
 */
extern bool orochi_is_distributed_table(Oid table_oid);

/*
 * Check if a table is a reference table (replicated)
 */
extern bool orochi_is_reference_table(Oid table_oid);

/*
 * Check if two tables are co-located (same shard placement)
 */
extern bool orochi_tables_are_colocated(Oid table1_oid, Oid table2_oid);

/*
 * Get the co-location group for a table
 */
extern int32 orochi_get_colocation_group(Oid table_oid);

/*
 * Create a new co-location group
 */
extern int32 orochi_create_colocation_group(int32 shard_count,
                                            OrochiShardStrategy strategy,
                                            const char *dist_type);

/* ============================================================
 * Hash Functions
 * ============================================================ */

/*
 * Compute hash value for a datum
 */
extern int32 orochi_hash_datum(Datum value, Oid type_oid);

/*
 * Compute CRC32-based hash
 */
extern uint32 orochi_crc32_hash(const void *data, size_t length);

/*
 * Get shard index for a hash value
 */
extern int32 orochi_get_shard_index(int32 hash_value, int32 shard_count);

/* ============================================================
 * Shard Routing Functions
 * ============================================================ */

/*
 * Get the shard for a given value
 */
extern OrochiShardInfo *orochi_route_to_shard(Oid table_oid, Datum value,
                                              Oid type_oid);

/*
 * Get all shards for a table
 */
extern List *orochi_get_all_shards(Oid table_oid);

/*
 * Get shards that match a hash range
 */
extern List *orochi_get_shards_in_range(Oid table_oid, int32 min_hash,
                                        int32 max_hash);

/* ============================================================
 * Distribution Column Functions
 * ============================================================ */

/*
 * Get the distribution column for a table
 */
extern char *orochi_get_distribution_column(Oid table_oid);

/*
 * Get the distribution column attribute number
 */
extern AttrNumber orochi_get_distribution_attnum(Oid table_oid);

/*
 * Get the distribution column type
 */
extern Oid orochi_get_distribution_type(Oid table_oid);

/* ============================================================
 * Reference Table Functions
 * ============================================================ */

/*
 * Create a reference table (replicated on all nodes)
 */
extern void orochi_create_reference_table_internal(Oid table_oid);

/*
 * Check if table can be converted to reference table
 */
extern bool orochi_can_be_reference_table(Oid table_oid);

/* ============================================================
 * Shard Placement Functions
 * ============================================================ */

/*
 * Get the node for a shard
 */
extern OrochiNodeInfo *orochi_get_shard_node(int64 shard_id);

/*
 * Move a shard to a different node
 */
extern void orochi_move_shard(int64 shard_id, int32 target_node);

/*
 * Get the best node for a new shard
 */
extern int32 orochi_select_node_for_shard(void);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Validate distribution column exists and is hashable
 */
extern bool orochi_validate_distribution_column(Oid table_oid,
                                                const char *column_name);

/*
 * Get the number of shards for a table
 */
extern int32 orochi_get_shard_count(Oid table_oid);

/* ============================================================
 * Shard Rebalancing Structures
 * ============================================================ */

/*
 * Statistics about shard distribution across nodes
 */
typedef struct ShardBalanceStats {
  int total_nodes;            /* Number of active nodes */
  int64 total_shards;         /* Total shards across all nodes */
  int64 min_shards;           /* Minimum shards on any node */
  int64 max_shards;           /* Maximum shards on any node */
  double avg_shards_per_node; /* Average shards per node */
  double std_deviation;       /* Standard deviation */
  double imbalance_ratio;     /* max/min ratio */
  bool needs_rebalancing;     /* True if imbalance > 20% */
} ShardBalanceStats;

/*
 * Shard move operation
 */
typedef struct ShardMove {
  int64 shard_id;    /* Shard to move */
  int32 source_node; /* Source node ID */
  int32 target_node; /* Target node ID */
} ShardMove;

/*
 * Detailed shard placement info
 */
typedef struct ShardPlacementInfo {
  int64 shard_id;
  Oid table_oid;
  int32 node_id;
  int32 shard_index;
  int32 hash_min;
  int32 hash_max;
  int64 row_count;
  int64 size_bytes;
} ShardPlacementInfo;

/* ============================================================
 * Shard Rebalancing Functions
 * ============================================================ */

/*
 * Get statistics about shard distribution across nodes
 */
extern ShardBalanceStats *orochi_get_shard_balance_stats(void);

/*
 * Rebalance shards for a distributed table
 */
extern void orochi_rebalance_table_shards(Oid table_oid);

/*
 * Move a shard with data to a different node
 */
extern void orochi_move_shard_with_data(int64 shard_id, int32 target_node);

/*
 * Split a shard into two
 */
extern void orochi_split_shard_impl(int64 shard_id);

/*
 * Merge two adjacent shards
 */
extern void orochi_merge_shards_impl(int64 shard1_id, int64 shard2_id);

/*
 * Get detailed shard placement information
 */
extern List *orochi_get_shard_placements(Oid table_oid);

#endif /* OROCHI_DISTRIBUTION_H */
