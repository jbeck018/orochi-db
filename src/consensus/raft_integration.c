/*-------------------------------------------------------------------------
 *
 * raft_integration.c
 *    Orochi DB Raft consensus integration with distributed executor
 *
 * This file implements:
 *   - Shared memory management for Raft state
 *   - Background worker for running Raft protocol
 *   - Integration APIs for distributed executor
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "access/xact.h"
#include "executor/spi.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "raft.h"
#include "raft_integration.h"

/* Global shared state pointer */
RaftSharedState *raft_shared_state = NULL;

/* Local Raft node instance (only used by background worker) */
static RaftNode *local_raft_node = NULL;

/* Signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* Transaction apply callback */
static RaftTransactionApplyCallback apply_callback = NULL;
static void *apply_callback_context = NULL;

/* GUC variables */
int orochi_raft_node_id = 1;
char *orochi_raft_wal_path = NULL;
bool orochi_raft_enabled = true;

/* Forward declarations */
static void raft_sighup_handler(SIGNAL_ARGS);
static void raft_sigterm_handler(SIGNAL_ARGS);
static void raft_update_shared_state(RaftNode *node);
static void raft_apply_entry_callback(RaftLogEntry *entry, void *context);

/* ============================================================
 * Shared Memory Functions
 * ============================================================ */

/*
 * orochi_raft_shmem_size
 *    Calculate shared memory requirements for Raft
 */
Size
orochi_raft_shmem_size(void)
{
    return sizeof(RaftSharedState);
}

/*
 * orochi_raft_shmem_init
 *    Initialize Raft shared memory structures
 */
void
orochi_raft_shmem_init(void)
{
    bool found;

    raft_shared_state = (RaftSharedState *)
        ShmemInitStruct("Orochi Raft State",
                        sizeof(RaftSharedState),
                        &found);

    if (!found)
    {
        /* First time initialization */
        memset(raft_shared_state, 0, sizeof(RaftSharedState));
        raft_shared_state->lock = &(GetNamedLWLockTranche("orochi_raft"))->lock;
        raft_shared_state->my_node_id = orochi_raft_node_id;
        raft_shared_state->current_leader_id = -1;
        raft_shared_state->current_term = 0;
        raft_shared_state->current_state = RAFT_STATE_FOLLOWER;
        raft_shared_state->cluster_size = 1;
        raft_shared_state->quorum_size = 1;
        raft_shared_state->worker_pid = 0;
        raft_shared_state->is_initialized = false;
        raft_shared_state->is_running = false;
        raft_shared_state->commands_submitted = 0;
        raft_shared_state->commands_committed = 0;
        raft_shared_state->elections_held = 0;
        raft_shared_state->pending_commands = 0;
        raft_shared_state->last_committed_index = 0;
    }

    elog(LOG, "Raft shared memory initialized");
}

/* ============================================================
 * Signal Handlers
 * ============================================================ */

static void
raft_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
raft_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/* ============================================================
 * Background Worker Implementation
 * ============================================================ */

/*
 * orochi_raft_worker_main
 *    Entry point for Raft consensus background worker
 */
void
orochi_raft_worker_main(Datum main_arg)
{
    StringInfoData wal_path;

    /* Set up signal handlers */
    pqsignal(SIGHUP, raft_sighup_handler);
    pqsignal(SIGTERM, raft_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Raft consensus worker started");

    /* Determine WAL path */
    initStringInfo(&wal_path);
    if (orochi_raft_wal_path != NULL)
        appendStringInfoString(&wal_path, orochi_raft_wal_path);
    else
        appendStringInfo(&wal_path, "%s/orochi_raft", DataDir);

    /* Initialize Raft node */
    local_raft_node = raft_init(orochi_raft_node_id, wal_path.data);
    pfree(wal_path.data);

    if (local_raft_node == NULL)
    {
        elog(ERROR, "Failed to initialize Raft node");
        proc_exit(1);
    }

    /* Register apply callback */
    raft_set_apply_callback(local_raft_node, raft_apply_entry_callback, NULL);

    /* Load cluster configuration from catalog */
    StartTransactionCommand();
    {
        List *nodes = orochi_catalog_list_nodes();
        ListCell *lc;

        foreach(lc, nodes)
        {
            OrochiNodeInfo *node = (OrochiNodeInfo *) lfirst(lc);

            if (node->node_id != orochi_raft_node_id)
            {
                raft_add_peer(local_raft_node, node->node_id,
                              node->hostname, node->port);
            }
        }
    }
    CommitTransactionCommand();

    /* Connect to peers */
    raft_connect_peers(local_raft_node);

    /* Start Raft protocol */
    raft_start(local_raft_node);

    /* Update shared state */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->worker_pid = MyProcPid;
    raft_shared_state->is_initialized = true;
    raft_shared_state->is_running = true;
    LWLockRelease(raft_shared_state->lock);

    raft_update_shared_state(local_raft_node);

    elog(LOG, "Raft node %d initialized and running", orochi_raft_node_id);

    /* Main event loop */
    while (!got_sigterm)
    {
        int rc;
        int wait_time_ms;

        /* Determine wait time based on state */
        if (local_raft_node->is_leader)
            wait_time_ms = RAFT_HEARTBEAT_INTERVAL_MS;
        else
            wait_time_ms = RAFT_ELECTION_TIMEOUT_MIN_MS / 2;

        /* Wait for events or timeout */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       wait_time_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Handle SIGHUP - reload config */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
            elog(LOG, "Raft worker reloaded configuration");
        }

        if (got_sigterm)
            break;

        /* Drive Raft state machine */
        raft_tick(local_raft_node);

        /* Update shared state periodically */
        raft_update_shared_state(local_raft_node);
    }

    /* Cleanup */
    elog(LOG, "Raft consensus worker shutting down");

    /* Update shared state */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->is_running = false;
    raft_shared_state->worker_pid = 0;
    LWLockRelease(raft_shared_state->lock);

    /* Shutdown Raft node */
    if (local_raft_node != NULL)
    {
        raft_shutdown(local_raft_node);
        local_raft_node = NULL;
    }

    proc_exit(0);
}

/*
 * raft_update_shared_state
 *    Update shared memory state from Raft node
 */
static void
raft_update_shared_state(RaftNode *node)
{
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);

    raft_shared_state->current_leader_id = node->leader_id;
    raft_shared_state->current_term = node->current_term;
    raft_shared_state->current_state = node->state;
    raft_shared_state->cluster_size = node->cluster->peer_count + 1;
    raft_shared_state->quorum_size = node->cluster->quorum_size;
    raft_shared_state->last_committed_index = node->log->commit_index;
    raft_shared_state->elections_held = node->elections_started;

    LWLockRelease(raft_shared_state->lock);
}

/*
 * raft_apply_entry_callback
 *    Called when a log entry is committed and should be applied
 */
static void
raft_apply_entry_callback(RaftLogEntry *entry, void *context)
{
    elog(DEBUG1, "Raft: applying committed entry at index %lu (type=%d)",
         entry->index, entry->type);

    /* Increment committed commands counter */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->commands_committed++;
    raft_shared_state->last_committed_index = entry->index;
    LWLockRelease(raft_shared_state->lock);

    /* Call user-registered callback if present */
    if (apply_callback != NULL && entry->command_data != NULL)
    {
        /* Extract GID from command data (first 64 bytes or null-terminated) */
        char gid[65];
        int gid_len = Min(entry->command_size, 64);
        memcpy(gid, entry->command_data, gid_len);
        gid[gid_len] = '\0';

        apply_callback(gid,
                       entry->command_data,
                       entry->command_size,
                       apply_callback_context);
    }
}

/* ============================================================
 * Public API Functions
 * ============================================================ */

/*
 * orochi_raft_is_leader
 *    Check if this node is the current Raft leader
 */
bool
orochi_raft_is_leader(void)
{
    bool is_leader;

    if (raft_shared_state == NULL)
        return false;

    LWLockAcquire(raft_shared_state->lock, LW_SHARED);
    is_leader = (raft_shared_state->current_state == RAFT_STATE_LEADER);
    LWLockRelease(raft_shared_state->lock);

    return is_leader;
}

/*
 * orochi_raft_get_leader
 *    Get the current Raft leader node ID
 */
int32
orochi_raft_get_leader(void)
{
    int32 leader_id;

    if (raft_shared_state == NULL)
        return -1;

    LWLockAcquire(raft_shared_state->lock, LW_SHARED);
    leader_id = raft_shared_state->current_leader_id;
    LWLockRelease(raft_shared_state->lock);

    return leader_id;
}

/*
 * orochi_raft_get_term
 *    Get current Raft term
 */
uint64
orochi_raft_get_term(void)
{
    uint64 term;

    if (raft_shared_state == NULL)
        return 0;

    LWLockAcquire(raft_shared_state->lock, LW_SHARED);
    term = raft_shared_state->current_term;
    LWLockRelease(raft_shared_state->lock);

    return term;
}

/*
 * orochi_raft_get_state_name
 *    Get current Raft state as string
 */
const char *
orochi_raft_get_state_name(void)
{
    RaftState state;

    if (raft_shared_state == NULL)
        return "not initialized";

    LWLockAcquire(raft_shared_state->lock, LW_SHARED);
    state = raft_shared_state->current_state;
    LWLockRelease(raft_shared_state->lock);

    return raft_state_name(state);
}

/* ============================================================
 * Transaction Integration
 * ============================================================ */

/*
 * orochi_raft_submit_transaction
 *    Submit a distributed transaction for Raft consensus
 */
bool
orochi_raft_submit_transaction(const char *gid,
                               List *participant_nodes,
                               const char *command_data,
                               int32 command_size)
{
    uint64 log_index;
    StringInfoData full_command;

    /* Check if we're the leader */
    if (!orochi_raft_is_leader())
    {
        elog(WARNING, "Cannot submit transaction: not Raft leader (leader=%d)",
             orochi_raft_get_leader());
        return false;
    }

    /* Check if Raft worker is running */
    if (local_raft_node == NULL)
    {
        elog(WARNING, "Cannot submit transaction: Raft node not available");
        return false;
    }

    /* Build full command: GID + command_data */
    initStringInfo(&full_command);
    appendStringInfo(&full_command, "%s", gid);
    appendBinaryStringInfo(&full_command, "\0", 1);  /* Null separator */
    appendBinaryStringInfo(&full_command, command_data, command_size);

    /* Submit to Raft log */
    log_index = raft_submit_command(local_raft_node,
                                    full_command.data,
                                    full_command.len);

    pfree(full_command.data);

    if (log_index == 0)
    {
        elog(WARNING, "Failed to submit transaction to Raft log");
        return false;
    }

    /* Update statistics */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->commands_submitted++;
    raft_shared_state->pending_commands++;
    LWLockRelease(raft_shared_state->lock);

    elog(DEBUG1, "Transaction %s submitted to Raft log at index %lu",
         gid, log_index);

    return true;
}

/*
 * orochi_raft_wait_for_commit
 *    Wait for a transaction to be committed via Raft
 */
bool
orochi_raft_wait_for_commit(const char *gid, int timeout_ms)
{
    TimestampTz deadline = GetCurrentTimestamp() + (timeout_ms * 1000L);
    uint64 initial_committed;

    if (raft_shared_state == NULL)
        return false;

    /* Get initial committed index */
    LWLockAcquire(raft_shared_state->lock, LW_SHARED);
    initial_committed = raft_shared_state->last_committed_index;
    LWLockRelease(raft_shared_state->lock);

    /* Poll until committed or timeout */
    while (GetCurrentTimestamp() < deadline)
    {
        uint64 current_committed;

        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        current_committed = raft_shared_state->last_committed_index;
        LWLockRelease(raft_shared_state->lock);

        /* Check if commit index advanced */
        if (current_committed > initial_committed)
        {
            /* TODO: Actually check if our specific transaction was committed
             * For now, assume any advancement means success */
            return true;
        }

        /* Small sleep to avoid busy-waiting */
        pg_usleep(1000);  /* 1ms */
    }

    elog(WARNING, "Timeout waiting for transaction %s to commit", gid);
    return false;
}

/*
 * orochi_raft_prepare_transaction
 *    Phase 1 of 2PC: prepare transaction via Raft
 */
bool
orochi_raft_prepare_transaction(const char *gid, List *shard_ids)
{
    StringInfoData command;
    ListCell *lc;

    initStringInfo(&command);
    appendStringInfo(&command, "PREPARE:%s:", gid);

    /* Append shard IDs */
    foreach(lc, shard_ids)
    {
        int64 shard_id = lfirst_int(lc);
        appendStringInfo(&command, "%ld,", shard_id);
    }

    /* Submit via Raft */
    bool result = orochi_raft_submit_transaction(gid, shard_ids,
                                                  command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_commit_transaction
 *    Phase 2 of 2PC: commit prepared transaction
 */
bool
orochi_raft_commit_transaction(const char *gid)
{
    StringInfoData command;

    initStringInfo(&command);
    appendStringInfo(&command, "COMMIT:%s", gid);

    bool result = orochi_raft_submit_transaction(gid, NIL,
                                                  command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_abort_transaction
 *    Abort a distributed transaction
 */
bool
orochi_raft_abort_transaction(const char *gid)
{
    StringInfoData command;

    initStringInfo(&command);
    appendStringInfo(&command, "ABORT:%s", gid);

    bool result = orochi_raft_submit_transaction(gid, NIL,
                                                  command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_get_transaction_status
 *    Get status of a transaction
 */
RaftTransactionStatus
orochi_raft_get_transaction_status(const char *gid)
{
    /* TODO: Implement transaction status tracking
     * This would require maintaining a map of GID -> status */
    return RAFT_TXN_UNKNOWN;
}

/* ============================================================
 * Cluster Management
 * ============================================================ */

/*
 * orochi_raft_register_node
 *    Register this node with the Raft cluster
 */
void
orochi_raft_register_node(int32 node_id, const char *hostname, int port)
{
    if (raft_shared_state != NULL)
    {
        LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
        raft_shared_state->my_node_id = node_id;
        LWLockRelease(raft_shared_state->lock);
    }

    /* Update GUC */
    orochi_raft_node_id = node_id;

    elog(LOG, "Registered Raft node %d at %s:%d", node_id, hostname, port);
}

/*
 * orochi_raft_add_cluster_node
 *    Add a peer node to the Raft cluster
 */
void
orochi_raft_add_cluster_node(int32 node_id, const char *hostname, int port)
{
    if (local_raft_node != NULL)
    {
        raft_add_peer(local_raft_node, node_id, hostname, port);
        raft_update_shared_state(local_raft_node);
    }

    elog(LOG, "Added Raft peer node %d at %s:%d", node_id, hostname, port);
}

/*
 * orochi_raft_remove_cluster_node
 *    Remove a node from the Raft cluster
 */
void
orochi_raft_remove_cluster_node(int32 node_id)
{
    if (local_raft_node != NULL)
    {
        raft_remove_peer(local_raft_node, node_id);
        raft_update_shared_state(local_raft_node);
    }

    elog(LOG, "Removed Raft peer node %d", node_id);
}

/* ============================================================
 * Callback Registration
 * ============================================================ */

/*
 * orochi_raft_set_apply_callback
 *    Register callback for applying committed transactions
 */
void
orochi_raft_set_apply_callback(RaftTransactionApplyCallback callback,
                               void *context)
{
    apply_callback = callback;
    apply_callback_context = context;
}

/* ============================================================
 * SQL Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_raft_status);
PG_FUNCTION_INFO_V1(orochi_raft_leader);
PG_FUNCTION_INFO_V1(orochi_raft_cluster_info);

/*
 * orochi_raft_status
 *    SQL function to get Raft status
 */
Datum
orochi_raft_status(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[8];
    bool nulls[8];
    HeapTuple tuple;

    /* Build result tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (raft_shared_state == NULL)
    {
        memset(nulls, true, sizeof(nulls));
    }
    else
    {
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);

        values[0] = Int32GetDatum(raft_shared_state->my_node_id);
        values[1] = CStringGetTextDatum(raft_state_name(raft_shared_state->current_state));
        values[2] = Int64GetDatum(raft_shared_state->current_term);
        values[3] = Int32GetDatum(raft_shared_state->current_leader_id);
        values[4] = Int32GetDatum(raft_shared_state->cluster_size);
        values[5] = Int64GetDatum(raft_shared_state->commands_committed);
        values[6] = Int64GetDatum(raft_shared_state->last_committed_index);
        values[7] = BoolGetDatum(raft_shared_state->is_running);

        LWLockRelease(raft_shared_state->lock);
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * orochi_raft_leader
 *    SQL function to get current Raft leader
 */
Datum
orochi_raft_leader(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(orochi_raft_get_leader());
}

/*
 * orochi_raft_cluster_info
 *    SQL function to get Raft cluster information
 */
Datum
orochi_raft_cluster_info(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (raft_shared_state == NULL)
    {
        memset(nulls, true, sizeof(nulls));
    }
    else
    {
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);

        values[0] = Int32GetDatum(raft_shared_state->cluster_size);
        values[1] = Int32GetDatum(raft_shared_state->quorum_size);
        values[2] = Int64GetDatum(raft_shared_state->elections_held);
        values[3] = BoolGetDatum(raft_shared_state->current_state == RAFT_STATE_LEADER);

        LWLockRelease(raft_shared_state->lock);
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
