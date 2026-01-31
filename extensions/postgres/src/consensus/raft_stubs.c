/*-------------------------------------------------------------------------
 *
 * raft_stubs.c
 *    Stub implementations for Raft integration functions
 *
 * These stubs allow the extension to load without full Raft support.
 * The full implementation is in raft_integration.c (not currently built).
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "storage/lwlock.h"

#include "raft.h"
#include "raft_integration.h"

/* Global shared state pointer (NULL when stubs are used) */
RaftSharedState *raft_shared_state = NULL;

/* ============================================================
 * Shared Memory Functions (stubs)
 * ============================================================ */

Size
orochi_raft_shmem_size(void)
{
    return sizeof(RaftSharedState);
}

void
orochi_raft_shmem_init(void)
{
    /* No-op stub - Raft not fully implemented */
    elog(DEBUG1, "Raft consensus integration not enabled (stub)");
}

/* ============================================================
 * Background Worker (stub)
 * ============================================================ */

void
orochi_raft_worker_main(Datum main_arg)
{
    elog(WARNING, "Raft background worker not implemented (stub)");
}

/* ============================================================
 * Distributed Executor Integration (stubs)
 * ============================================================ */

bool
orochi_raft_submit_transaction(const char *gid,
                                List *participant_nodes,
                                const char *command_data,
                                int32 command_size)
{
    /* Stub: return true to indicate "success" without actual consensus */
    elog(DEBUG1, "Raft submit_transaction stub called for GID: %s", gid);
    return true;
}

bool
orochi_raft_wait_for_commit(const char *gid, int timeout_ms)
{
    /* Stub: return true immediately */
    elog(DEBUG1, "Raft wait_for_commit stub called for GID: %s", gid);
    return true;
}

bool
orochi_raft_is_leader(void)
{
    /* Stub: always return true (single-node mode) */
    return true;
}

int32
orochi_raft_get_leader_id(void)
{
    /* Stub: return 1 as default leader */
    return 1;
}

int32
orochi_raft_get_leader(void)
{
    /* Stub: return 1 as default leader */
    return 1;
}

uint64
orochi_raft_get_term(void)
{
    /* Stub: return 1 as default term */
    return 1;
}

const char *
orochi_raft_get_state_name(void)
{
    /* Stub: return "leader" since we're always leader in single-node mode */
    return "leader";
}

void
orochi_raft_register_node(int32 node_id, const char *hostname, int port)
{
    /* Stub: no-op */
    elog(DEBUG1, "Raft register_node stub called for node %d", node_id);
}

void
orochi_raft_add_cluster_node(int32 node_id, const char *hostname, int port)
{
    /* Stub: no-op */
    elog(DEBUG1, "Raft add_cluster_node stub called for node %d", node_id);
}

void
orochi_raft_remove_cluster_node(int32 node_id)
{
    /* Stub: no-op */
    elog(DEBUG1, "Raft remove_cluster_node stub called for node %d", node_id);
}

/* ============================================================
 * Transaction Coordination (stubs)
 * ============================================================ */

bool
orochi_raft_prepare_transaction(const char *gid, List *shard_ids)
{
    elog(DEBUG1, "Raft prepare_transaction stub called for GID: %s", gid);
    return true;
}

bool
orochi_raft_commit_transaction(const char *gid)
{
    elog(DEBUG1, "Raft commit_transaction stub called for GID: %s", gid);
    return true;
}

bool
orochi_raft_abort_transaction(const char *gid)
{
    elog(DEBUG1, "Raft abort_transaction stub called for GID: %s", gid);
    return true;
}

RaftTransactionStatus
orochi_raft_get_transaction_status(const char *gid)
{
    /* Stub: return COMMITTED */
    return RAFT_TXN_COMMITTED;
}

/* ============================================================
 * Callback Registration (stubs)
 * ============================================================ */

void
orochi_raft_register_apply_callback(RaftTransactionApplyCallback callback,
                                     void *context)
{
    /* Stub: no-op */
    elog(DEBUG1, "Raft register_apply_callback stub called");
}

void
orochi_raft_set_apply_callback(RaftTransactionApplyCallback callback,
                                void *context)
{
    /* Stub: no-op */
    elog(DEBUG1, "Raft set_apply_callback stub called");
}

/* ============================================================
 * SQL-Callable Functions (stubs)
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_raft_status);
Datum
orochi_raft_status(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(orochi_raft_step_down);
Datum
orochi_raft_step_down(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_raft_transfer_leadership);
Datum
orochi_raft_transfer_leadership(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(orochi_raft_add_node);
Datum
orochi_raft_add_node(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(orochi_raft_remove_node);
Datum
orochi_raft_remove_node(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(orochi_raft_leader);
Datum
orochi_raft_leader(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_INT32(1);
}

PG_FUNCTION_INFO_V1(orochi_raft_cluster_info);
Datum
orochi_raft_cluster_info(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

/* Additional SQL-callable stubs for raft functions */

PG_FUNCTION_INFO_V1(orochi_raft_request_vote_sql);
Datum
orochi_raft_request_vote_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(orochi_raft_append_entries_sql);
Datum
orochi_raft_append_entries_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(orochi_raft_install_snapshot_sql);
Datum
orochi_raft_install_snapshot_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(orochi_raft_leader_read_index_sql);
Datum
orochi_raft_leader_read_index_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_INT64(0);
}

PG_FUNCTION_INFO_V1(orochi_raft_read_index_sql);
Datum
orochi_raft_read_index_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_INT64(0);
}

PG_FUNCTION_INFO_V1(orochi_raft_can_read_stale_sql);
Datum
orochi_raft_can_read_stale_sql(PG_FUNCTION_ARGS)
{
    /* Return true - in single-node mode, reads are always fresh */
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(orochi_raft_status_sql);
Datum
orochi_raft_status_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(orochi_raft_is_leader_sql);
Datum
orochi_raft_is_leader_sql(PG_FUNCTION_ARGS)
{
    /* Return true - single node is always leader */
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(orochi_raft_get_leader_sql);
Datum
orochi_raft_get_leader_sql(PG_FUNCTION_ARGS)
{
    /* Return 1 as default leader ID */
    PG_RETURN_INT32(1);
}

PG_FUNCTION_INFO_V1(orochi_raft_follower_read_sql);
Datum
orochi_raft_follower_read_sql(PG_FUNCTION_ARGS)
{
    /* Return true - in single-node mode, data is always fresh */
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(orochi_raft_create_snapshot_sql);
Datum
orochi_raft_create_snapshot_sql(PG_FUNCTION_ARGS)
{
    ereport(NOTICE,
            (errmsg("Raft consensus not enabled (stub implementation)")));
    PG_RETURN_BOOL(false);
}
