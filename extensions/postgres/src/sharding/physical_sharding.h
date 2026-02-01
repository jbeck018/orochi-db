/*-------------------------------------------------------------------------
 *
 * physical_sharding.h
 *    Orochi DB physical shard table management
 *
 * This module handles:
 *   - Creating physical shard tables (child tables)
 *   - INSERT/UPDATE/DELETE routing via triggers
 *   - Query rewriting for transparent access
 *   - Moving data to physical shards
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_PHYSICAL_SHARDING_H
#define OROCHI_PHYSICAL_SHARDING_H

#include "../orochi.h"
#include "postgres.h"

/* ============================================================
 * Physical Shard Table Management
 * ============================================================ */

/*
 * Create physical shard tables for a distributed table.
 * Creates tables named: schema.tablename_shard_N
 */
extern void orochi_create_physical_shards(Oid table_oid, int shard_count, const char *dist_column);

/*
 * Get the physical shard table name for a given shard index
 */
extern char *orochi_get_shard_table_name(Oid table_oid, int shard_index);

/*
 * Get the OID of a physical shard table
 */
extern Oid orochi_get_shard_table_oid(Oid parent_table_oid, int shard_index);

/*
 * Drop all physical shard tables for a table
 */
extern void orochi_drop_physical_shards(Oid table_oid);

/*
 * Move existing data from parent table to physical shards
 */
extern int64 orochi_redistribute_data(Oid table_oid);

/* ============================================================
 * Routing Functions
 * ============================================================ */

/*
 * Install INSERT/UPDATE/DELETE routing trigger on parent table
 */
extern void orochi_install_routing_trigger(Oid table_oid, const char *dist_column,
                                           int32 shard_count);

/*
 * Remove routing triggers from parent table
 */
extern void orochi_remove_routing_trigger(Oid table_oid);

/*
 * Compute which shard a value belongs to
 * Returns shard index (0-based)
 */
extern int32 orochi_compute_shard_for_value(Datum value, Oid type_oid, int32 shard_count);

/* ============================================================
 * View Management for Transparent Access
 * ============================================================ */

/*
 * Create a view that unions all shard tables
 * (Used if parent table is renamed/hidden)
 */
extern void orochi_create_union_view(Oid table_oid, const char *dist_column, int32 shard_count);

/*
 * Drop the union view
 */
extern void orochi_drop_union_view(Oid table_oid);

/* ============================================================
 * Query Rewriting
 * ============================================================ */

/*
 * Check if we should rewrite a query for this table
 */
extern bool orochi_should_rewrite_query(Oid table_oid);

/*
 * Get list of shard table OIDs for a query with shard pruning applied
 */
extern List *orochi_get_target_shards_for_query(Oid table_oid, Node *quals);

/*
 * Rewrite a range table entry to point to shard tables
 */
extern List *orochi_expand_rte_to_shards(RangeTblEntry *rte, Node *quals);

#endif /* OROCHI_PHYSICAL_SHARDING_H */
