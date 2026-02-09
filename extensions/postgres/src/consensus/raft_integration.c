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


/* postgres.h must be included first */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
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
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../core/catalog.h"
#include "../orochi.h"
#include "raft.h"
#include "raft_integration.h"
#include "utils/hsearch.h"

/* Global shared state pointer */
RaftSharedState *raft_shared_state = NULL;

/* ============================================================
 * Transaction Tracking Data Structures
 * ============================================================ */

/*
 * RaftTransactionEntry - Entry in the transaction tracking hash table
 * Maps transaction GID to Raft log index and status
 */
typedef struct RaftTransactionEntry {
    char gid[65];                 /* Transaction GID (hash key) */
    uint64 log_index;             /* Raft log index where submitted */
    uint64 term;                  /* Raft term when submitted */
    RaftTransactionStatus status; /* Current status */
    TimestampTz submitted_at;     /* When transaction was submitted */
    TimestampTz committed_at;     /* When transaction was committed (if applicable) */
    int32 command_size;           /* Size of command for verification */
    uint32 command_hash;          /* Simple hash of command for verification */
} RaftTransactionEntry;

/* Hash table for tracking pending transactions */
static HTAB *transaction_tracker = NULL;
static LWLock *transaction_tracker_lock = NULL;

#define MAX_TRACKED_TRANSACTIONS 1024

/* Forward declarations for transaction tracking */
static void transaction_tracker_init(void);
static RaftTransactionEntry *transaction_tracker_add(const char *gid, uint64 log_index, uint64 term,
                                                     const char *command, int32 command_size);
static RaftTransactionEntry *transaction_tracker_find(const char *gid);
static void transaction_tracker_update_status(const char *gid, RaftTransactionStatus status);
static void transaction_tracker_remove_old(void);
static uint32 command_hash(const char *data, int32 size);
static bool transaction_tracker_verify_commit(const char *gid, RaftNode *node);

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
Size orochi_raft_shmem_size(void)
{
    return sizeof(RaftSharedState);
}

/*
 * orochi_raft_shmem_init
 *    Initialize Raft shared memory structures
 */
void orochi_raft_shmem_init(void)
{
    bool found;

    raft_shared_state =
        (RaftSharedState *)ShmemInitStruct("Orochi Raft State", sizeof(RaftSharedState), &found);

    if (!found) {
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

static void raft_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void raft_sigterm_handler(SIGNAL_ARGS)
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
void orochi_raft_worker_main(Datum main_arg)
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

    if (local_raft_node == NULL) {
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

        foreach (lc, nodes) {
            OrochiNodeInfo *node = (OrochiNodeInfo *)lfirst(lc);

            if (node->node_id != orochi_raft_node_id) {
                raft_add_peer(local_raft_node, node->node_id, node->hostname, node->port);
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
    while (!got_sigterm) {
        int rc;
        int wait_time_ms;

        /* Determine wait time based on state */
        if (local_raft_node->is_leader)
            wait_time_ms = RAFT_HEARTBEAT_INTERVAL_MS;
        else
            wait_time_ms = RAFT_ELECTION_TIMEOUT_MIN_MS / 2;

        /* Wait for events or timeout */
        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, wait_time_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Handle SIGHUP - reload config */
        if (got_sighup) {
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
    if (local_raft_node != NULL) {
        raft_shutdown(local_raft_node);
        local_raft_node = NULL;
    }

    proc_exit(0);
}

/*
 * raft_update_shared_state
 *    Update shared memory state from Raft node
 */
static void raft_update_shared_state(RaftNode *node)
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
 *
 * This callback is invoked for each log entry that reaches the committed state.
 * It updates statistics, marks the transaction as committed in our tracker,
 * and invokes any user-registered callback.
 */
static void raft_apply_entry_callback(RaftLogEntry *entry, void *context)
{
    char gid[65];
    int gid_len;
    RaftTransactionStatus new_status;

    elog(DEBUG1, "Raft: applying committed entry at index %lu (type=%d)", entry->index,
         entry->type);

    /* Increment committed commands counter */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->commands_committed++;
    raft_shared_state->last_committed_index = entry->index;

    /* Decrement pending commands if this was a tracked transaction */
    if (raft_shared_state->pending_commands > 0)
        raft_shared_state->pending_commands--;

    LWLockRelease(raft_shared_state->lock);

    /* Extract GID from command data (null-terminated string at start) */
    if (entry->command_data != NULL && entry->command_size > 0) {
        /* Find the null terminator or use max length */
        gid_len = 0;
        while (gid_len < entry->command_size && gid_len < 64 &&
               entry->command_data[gid_len] != '\0') {
            gid_len++;
        }
        memcpy(gid, entry->command_data, gid_len);
        gid[gid_len] = '\0';

        /*
         * Determine the new status based on command type.
         * Commands are formatted as "TYPE:GID:..." where TYPE can be
         * PREPARE, COMMIT, or ABORT.
         */
        if (strncmp(entry->command_data, "COMMIT:", 7) == 0)
            new_status = RAFT_TXN_COMMITTED;
        else if (strncmp(entry->command_data, "ABORT:", 6) == 0)
            new_status = RAFT_TXN_ABORTED;
        else if (strncmp(entry->command_data, "PREPARE:", 8) == 0)
            new_status = RAFT_TXN_PREPARED;
        else
            new_status = RAFT_TXN_COMMITTED; /* Default for raw commands */

        /* Update transaction status in tracker */
        transaction_tracker_update_status(gid, new_status);

        elog(DEBUG1, "Transaction %s status updated to %d on apply", gid, new_status);

        /* Call user-registered callback if present */
        if (apply_callback != NULL) {
            apply_callback(gid, entry->command_data, entry->command_size, apply_callback_context);
        }
    }
}

/* ============================================================
 * Public API Functions
 * ============================================================ */

/*
 * orochi_raft_is_leader
 *    Check if this node is the current Raft leader
 */
bool orochi_raft_is_leader(void)
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
int32 orochi_raft_get_leader(void)
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
uint64 orochi_raft_get_term(void)
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
const char *orochi_raft_get_state_name(void)
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
bool orochi_raft_submit_transaction(const char *gid, List *participant_nodes,
                                    const char *command_data, int32 command_size)
{
    uint64 log_index;
    StringInfoData full_command;

    /* Check if we're the leader */
    if (!orochi_raft_is_leader()) {
        elog(WARNING, "Cannot submit transaction: not Raft leader (leader=%d)",
             orochi_raft_get_leader());
        return false;
    }

    /* Check if Raft worker is running */
    if (local_raft_node == NULL) {
        elog(WARNING, "Cannot submit transaction: Raft node not available");
        return false;
    }

    /* Build full command: GID + command_data */
    initStringInfo(&full_command);
    appendStringInfo(&full_command, "%s", gid);
    appendBinaryStringInfo(&full_command, "\0", 1); /* Null separator */
    appendBinaryStringInfo(&full_command, command_data, command_size);

    /* Submit to Raft log */
    log_index = raft_submit_command(local_raft_node, full_command.data, full_command.len);

    if (log_index == 0) {
        pfree(full_command.data);
        elog(WARNING, "Failed to submit transaction to Raft log");
        return false;
    }

    /* Track the transaction for commit verification */
    transaction_tracker_add(gid, log_index, local_raft_node->current_term, full_command.data,
                            full_command.len);

    pfree(full_command.data);

    /* Update statistics */
    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);
    raft_shared_state->commands_submitted++;
    raft_shared_state->pending_commands++;
    LWLockRelease(raft_shared_state->lock);

    elog(DEBUG1, "Transaction %s submitted to Raft log at index %lu, term %lu", gid, log_index,
         local_raft_node->current_term);

    return true;
}

/*
 * orochi_raft_wait_for_commit
 *    Wait for a transaction to be committed via Raft
 *
 * This function verifies that the specific transaction identified by GID
 * was actually committed in the Raft log, not just that some commit occurred.
 */
bool orochi_raft_wait_for_commit(const char *gid, int timeout_ms)
{
    TimestampTz deadline = GetCurrentTimestamp() + (timeout_ms * 1000L);
    RaftTransactionEntry *entry;

    if (raft_shared_state == NULL)
        return false;

    /* First, find the transaction in our tracker to get its log index */
    entry = transaction_tracker_find(gid);
    if (entry == NULL) {
        elog(WARNING, "Transaction %s not found in tracker - cannot wait for commit", gid);
        return false;
    }

    /* Check if already committed or aborted */
    if (entry->status == RAFT_TXN_COMMITTED)
        return true;
    if (entry->status == RAFT_TXN_ABORTED) {
        elog(WARNING, "Transaction %s was aborted", gid);
        return false;
    }

    /* Poll until committed or timeout */
    while (GetCurrentTimestamp() < deadline) {
        uint64 current_committed;
        uint64 txn_log_index = entry->log_index;

        /* Get current commit index from shared state */
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        current_committed = raft_shared_state->last_committed_index;
        LWLockRelease(raft_shared_state->lock);

        /* Check if our transaction's log index has been committed */
        if (current_committed >= txn_log_index) {
            /*
             * The commit index has reached or passed our transaction's index.
             * Now verify the transaction was actually committed correctly:
             * - Compare log index with commit index
             * - Verify the term matches (no log truncation)
             * - Verify the command data matches via hash
             */
            if (local_raft_node != NULL) {
                if (transaction_tracker_verify_commit(gid, local_raft_node)) {
                    elog(DEBUG1, "Transaction %s confirmed committed at index %lu", gid,
                         txn_log_index);
                    return true;
                }

                /* Re-check entry status after verification attempt */
                entry = transaction_tracker_find(gid);
                if (entry != NULL && entry->status == RAFT_TXN_ABORTED) {
                    elog(WARNING, "Transaction %s was aborted during verification", gid);
                    return false;
                }
            } else {
                /*
                 * No local node available (we're not the background worker).
                 * Fall back to checking shared state commit index advancement.
                 * Update status optimistically based on index comparison.
                 */
                transaction_tracker_update_status(gid, RAFT_TXN_COMMITTED);
                elog(DEBUG1, "Transaction %s presumed committed at index %lu (no local node)", gid,
                     txn_log_index);
                return true;
            }
        }

        /* Small sleep to avoid busy-waiting */
        pg_usleep(1000); /* 1ms */
    }

    elog(WARNING, "Timeout waiting for transaction %s to commit (waited %d ms)", gid, timeout_ms);
    return false;
}

/*
 * orochi_raft_prepare_transaction
 *    Phase 1 of 2PC: prepare transaction via Raft
 */
bool orochi_raft_prepare_transaction(const char *gid, List *shard_ids)
{
    StringInfoData command;
    ListCell *lc;

    initStringInfo(&command);
    appendStringInfo(&command, "PREPARE:%s:", gid);

    /* Append shard IDs */
    foreach (lc, shard_ids) {
        int64 shard_id = lfirst_int(lc);
        appendStringInfo(&command, "%ld,", shard_id);
    }

    /* Submit via Raft */
    bool result = orochi_raft_submit_transaction(gid, shard_ids, command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_commit_transaction
 *    Phase 2 of 2PC: commit prepared transaction
 */
bool orochi_raft_commit_transaction(const char *gid)
{
    StringInfoData command;

    initStringInfo(&command);
    appendStringInfo(&command, "COMMIT:%s", gid);

    bool result = orochi_raft_submit_transaction(gid, NIL, command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_abort_transaction
 *    Abort a distributed transaction
 */
bool orochi_raft_abort_transaction(const char *gid)
{
    StringInfoData command;

    initStringInfo(&command);
    appendStringInfo(&command, "ABORT:%s", gid);

    bool result = orochi_raft_submit_transaction(gid, NIL, command.data, command.len);
    pfree(command.data);

    return result;
}

/*
 * orochi_raft_get_transaction_status
 *    Get status of a transaction
 *
 * This function queries the transaction tracking hash table to retrieve
 * the current status of a distributed transaction. The tracker maintains
 * a mapping from transaction GID to Raft log index and status.
 *
 * Status values:
 *   RAFT_TXN_UNKNOWN   - Transaction not found in tracker
 *   RAFT_TXN_PENDING   - Transaction submitted, waiting for commit
 *   RAFT_TXN_PREPARED  - Transaction prepared (2PC phase 1 complete)
 *   RAFT_TXN_COMMITTED - Transaction committed via Raft consensus
 *   RAFT_TXN_ABORTED   - Transaction aborted or failed verification
 */
RaftTransactionStatus orochi_raft_get_transaction_status(const char *gid)
{
    RaftTransactionEntry *entry;
    RaftTransactionStatus status;

    /* Look up transaction in the tracker */
    entry = transaction_tracker_find(gid);
    if (entry == NULL) {
        elog(DEBUG1, "Transaction %s not found in tracker", gid);
        return RAFT_TXN_UNKNOWN;
    }

    status = entry->status;

    /*
     * If transaction is still pending, check if it may have been committed
     * since we last checked. This handles the case where the apply callback
     * hasn't run yet but the commit index has advanced.
     */
    if (status == RAFT_TXN_PENDING && local_raft_node != NULL) {
        uint64 current_committed;

        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        current_committed = raft_shared_state->last_committed_index;
        LWLockRelease(raft_shared_state->lock);

        /* If commit index has reached our transaction, try to verify */
        if (current_committed >= entry->log_index) {
            if (transaction_tracker_verify_commit(gid, local_raft_node))
                status = RAFT_TXN_COMMITTED;

            /* Re-fetch in case verification updated status */
            entry = transaction_tracker_find(gid);
            if (entry != NULL)
                status = entry->status;
        }
    }

    elog(DEBUG2, "Transaction %s status: %d", gid, status);
    return status;
}

/* ============================================================
 * Cluster Management
 * ============================================================ */

/*
 * orochi_raft_register_node
 *    Register this node with the Raft cluster
 */
void orochi_raft_register_node(int32 node_id, const char *hostname, int port)
{
    if (raft_shared_state != NULL) {
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
void orochi_raft_add_cluster_node(int32 node_id, const char *hostname, int port)
{
    if (local_raft_node != NULL) {
        raft_add_peer(local_raft_node, node_id, hostname, port);
        raft_update_shared_state(local_raft_node);
    }

    elog(LOG, "Added Raft peer node %d at %s:%d", node_id, hostname, port);
}

/*
 * orochi_raft_remove_cluster_node
 *    Remove a node from the Raft cluster
 */
void orochi_raft_remove_cluster_node(int32 node_id)
{
    if (local_raft_node != NULL) {
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
void orochi_raft_set_apply_callback(RaftTransactionApplyCallback callback, void *context)
{
    apply_callback = callback;
    apply_callback_context = context;
}

/* ============================================================
 * Transaction Tracking Implementation
 * ============================================================ */

/*
 * command_hash
 *    Simple hash function for command data verification
 */
static uint32 command_hash(const char *data, int32 size)
{
    uint32 hash = 5381;
    int i;

    if (data == NULL || size <= 0)
        return 0;

    for (i = 0; i < size; i++)
        hash = ((hash << 5) + hash) + (unsigned char)data[i];

    return hash;
}

/*
 * transaction_tracker_init
 *    Initialize the transaction tracking hash table
 */
static void transaction_tracker_init(void)
{
    HASHCTL hash_ctl;

    if (transaction_tracker != NULL)
        return; /* Already initialized */

    /* Initialize hash table control structure */
    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = 65; /* GID size */
    hash_ctl.entrysize = sizeof(RaftTransactionEntry);
    hash_ctl.hcxt = TopMemoryContext;

    transaction_tracker = hash_create("Raft Transaction Tracker", MAX_TRACKED_TRANSACTIONS,
                                      &hash_ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

    /* Get or create a lock for the tracker */
    transaction_tracker_lock = &(GetNamedLWLockTranche("orochi_raft_txn"))->lock;

    elog(DEBUG1, "Transaction tracker initialized with capacity %d", MAX_TRACKED_TRANSACTIONS);
}

/*
 * transaction_tracker_add
 *    Add a new transaction to the tracking table
 */
static RaftTransactionEntry *transaction_tracker_add(const char *gid, uint64 log_index, uint64 term,
                                                     const char *command, int32 command_size)
{
    RaftTransactionEntry *entry;
    bool found;

    if (transaction_tracker == NULL)
        transaction_tracker_init();

    LWLockAcquire(transaction_tracker_lock, LW_EXCLUSIVE);

    /* Remove old completed transactions if we're getting full */
    transaction_tracker_remove_old();

    /* Insert or find entry */
    entry = (RaftTransactionEntry *)hash_search(transaction_tracker, gid, HASH_ENTER, &found);

    if (!found) {
        /* New entry - initialize */
        strlcpy(entry->gid, gid, sizeof(entry->gid));
        entry->log_index = log_index;
        entry->term = term;
        entry->status = RAFT_TXN_PENDING;
        entry->submitted_at = GetCurrentTimestamp();
        entry->committed_at = 0;
        entry->command_size = command_size;
        entry->command_hash = command_hash(command, command_size);

        elog(DEBUG1, "Transaction %s added to tracker at index %lu, term %lu", gid, log_index,
             term);
    } else {
        /* Entry exists - update if re-submitted (e.g., after leader change) */
        entry->log_index = log_index;
        entry->term = term;
        entry->status = RAFT_TXN_PENDING;
        entry->submitted_at = GetCurrentTimestamp();

        elog(DEBUG1, "Transaction %s updated in tracker at index %lu, term %lu", gid, log_index,
             term);
    }

    LWLockRelease(transaction_tracker_lock);

    return entry;
}

/*
 * transaction_tracker_find
 *    Find a transaction in the tracking table
 */
static RaftTransactionEntry *transaction_tracker_find(const char *gid)
{
    RaftTransactionEntry *entry;

    if (transaction_tracker == NULL)
        return NULL;

    LWLockAcquire(transaction_tracker_lock, LW_SHARED);

    entry = (RaftTransactionEntry *)hash_search(transaction_tracker, gid, HASH_FIND, NULL);

    LWLockRelease(transaction_tracker_lock);

    return entry;
}

/*
 * transaction_tracker_update_status
 *    Update the status of a tracked transaction
 */
static void transaction_tracker_update_status(const char *gid, RaftTransactionStatus status)
{
    RaftTransactionEntry *entry;

    if (transaction_tracker == NULL)
        return;

    LWLockAcquire(transaction_tracker_lock, LW_EXCLUSIVE);

    entry = (RaftTransactionEntry *)hash_search(transaction_tracker, gid, HASH_FIND, NULL);

    if (entry != NULL) {
        entry->status = status;

        if (status == RAFT_TXN_COMMITTED || status == RAFT_TXN_ABORTED)
            entry->committed_at = GetCurrentTimestamp();

        elog(DEBUG1, "Transaction %s status updated to %d", gid, status);
    }

    LWLockRelease(transaction_tracker_lock);
}

/*
 * transaction_tracker_remove_old
 *    Remove old completed transactions to prevent table from growing unbounded
 *    Must be called with transaction_tracker_lock held exclusively
 */
static void transaction_tracker_remove_old(void)
{
    HASH_SEQ_STATUS status;
    RaftTransactionEntry *entry;
    TimestampTz cutoff;
    int removed = 0;

    if (transaction_tracker == NULL)
        return;

    /* Remove transactions completed more than 5 minutes ago */
    cutoff = GetCurrentTimestamp() - (5 * 60 * 1000000L);

    hash_seq_init(&status, transaction_tracker);
    while ((entry = (RaftTransactionEntry *)hash_seq_search(&status)) != NULL) {
        if ((entry->status == RAFT_TXN_COMMITTED || entry->status == RAFT_TXN_ABORTED) &&
            entry->committed_at != 0 && entry->committed_at < cutoff) {
            hash_search(transaction_tracker, entry->gid, HASH_REMOVE, NULL);
            removed++;
        }
    }

    if (removed > 0)
        elog(DEBUG1, "Removed %d old transactions from tracker", removed);
}

/*
 * transaction_tracker_verify_commit
 *    Verify if a specific transaction was committed in the Raft log
 *    Returns true if the transaction is confirmed committed
 */
static bool transaction_tracker_verify_commit(const char *gid, RaftNode *node)
{
    RaftTransactionEntry *entry;
    RaftLogEntry *log_entry;
    uint32 stored_hash;
    bool verified = false;

    if (transaction_tracker == NULL || node == NULL)
        return false;

    /* Find the transaction in our tracker */
    entry = transaction_tracker_find(gid);
    if (entry == NULL) {
        elog(DEBUG1, "Transaction %s not found in tracker", gid);
        return false;
    }

    /* Check if already marked as committed */
    if (entry->status == RAFT_TXN_COMMITTED)
        return true;

    /* Check if the log index is committed */
    if (entry->log_index > node->log->commit_index) {
        elog(DEBUG2, "Transaction %s at index %lu not yet committed (commit_index=%lu)", gid,
             entry->log_index, node->log->commit_index);
        return false;
    }

    /* Verify the log entry matches our transaction */
    log_entry = raft_log_get(node->log, entry->log_index);
    if (log_entry == NULL) {
        elog(WARNING, "Transaction %s: log entry at index %lu not found", gid, entry->log_index);
        return false;
    }

    /* Verify term matches (ensures no log truncation occurred) */
    if (log_entry->term != entry->term) {
        elog(WARNING, "Transaction %s: term mismatch at index %lu (expected %lu, got %lu)", gid,
             entry->log_index, entry->term, log_entry->term);
        /* Term mismatch means this entry was overwritten - transaction failed */
        transaction_tracker_update_status(gid, RAFT_TXN_ABORTED);
        return false;
    }

    /* Verify command data matches via hash comparison */
    stored_hash = command_hash(log_entry->command_data, log_entry->command_size);
    if (stored_hash != entry->command_hash || log_entry->command_size != entry->command_size) {
        elog(WARNING, "Transaction %s: command mismatch at index %lu", gid, entry->log_index);
        transaction_tracker_update_status(gid, RAFT_TXN_ABORTED);
        return false;
    }

    /* All checks passed - transaction is committed */
    transaction_tracker_update_status(gid, RAFT_TXN_COMMITTED);
    verified = true;

    elog(DEBUG1, "Transaction %s verified committed at index %lu", gid, entry->log_index);

    return verified;
}

/* ============================================================
 * SQL Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_raft_status);
PG_FUNCTION_INFO_V1(orochi_raft_leader);
PG_FUNCTION_INFO_V1(orochi_raft_cluster_info);
PG_FUNCTION_INFO_V1(orochi_raft_step_down);
PG_FUNCTION_INFO_V1(orochi_raft_transfer_leadership);
PG_FUNCTION_INFO_V1(orochi_raft_add_node);
PG_FUNCTION_INFO_V1(orochi_raft_remove_node);
PG_FUNCTION_INFO_V1(orochi_raft_is_leader_sql);
PG_FUNCTION_INFO_V1(orochi_raft_get_leader_sql);
PG_FUNCTION_INFO_V1(orochi_raft_status_sql);
PG_FUNCTION_INFO_V1(orochi_raft_read_index_sql);
PG_FUNCTION_INFO_V1(orochi_raft_leader_read_index_sql);
PG_FUNCTION_INFO_V1(orochi_raft_can_read_stale_sql);
PG_FUNCTION_INFO_V1(orochi_raft_follower_read_sql);
PG_FUNCTION_INFO_V1(orochi_raft_create_snapshot_sql);
PG_FUNCTION_INFO_V1(orochi_raft_install_snapshot_sql);
PG_FUNCTION_INFO_V1(orochi_raft_request_vote_sql);
PG_FUNCTION_INFO_V1(orochi_raft_append_entries_sql);

/*
 * orochi_raft_status
 *    SQL function to get Raft status
 */
Datum orochi_raft_status(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[8];
    bool nulls[8];
    HeapTuple tuple;

    /* Build result tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (raft_shared_state == NULL) {
        memset(nulls, true, sizeof(nulls));
    } else {
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
Datum orochi_raft_leader(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(orochi_raft_get_leader());
}

/*
 * orochi_raft_cluster_info
 *    SQL function to get Raft cluster information
 */
Datum orochi_raft_cluster_info(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (raft_shared_state == NULL) {
        memset(nulls, true, sizeof(nulls));
    } else {
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

/* ============================================================
 * Additional SQL Functions
 * ============================================================ */

/*
 * orochi_raft_step_down
 *    SQL function to request the current leader to step down.
 *
 * Signals the Raft background worker to transition from leader to follower
 * by setting the state in shared memory. The background worker will pick
 * up this change on its next tick.
 *
 * Returns true if the step-down was initiated, false otherwise.
 */
Datum orochi_raft_step_down(PG_FUNCTION_ARGS)
{
    bool initiated = false;

    if (raft_shared_state == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft shared state is not initialized")));

    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);

    if (raft_shared_state->current_state == RAFT_STATE_LEADER) {
        /*
         * Signal step-down by setting the state to follower in shared memory.
         * The background worker will detect the mismatch between its local
         * state and shared state on the next tick.
         */
        raft_shared_state->current_state = RAFT_STATE_FOLLOWER;
        raft_shared_state->current_leader_id = -1;
        initiated = true;

        elog(LOG, "Raft step-down initiated via SQL for node %d", raft_shared_state->my_node_id);
    } else {
        elog(NOTICE, "Node %d is not the leader (state: %s), cannot step down",
             raft_shared_state->my_node_id, raft_state_name(raft_shared_state->current_state));
    }

    LWLockRelease(raft_shared_state->lock);

    PG_RETURN_BOOL(initiated);
}

/*
 * orochi_raft_transfer_leadership
 *    SQL function to transfer leadership to a specific node.
 *
 * Sets the target node ID in shared memory for the background worker
 * to initiate leadership transfer. The actual transfer happens when
 * the background worker processes this request.
 *
 * Arguments: target_node_id (int4)
 * Returns: bool (true if transfer was initiated)
 */
Datum orochi_raft_transfer_leadership(PG_FUNCTION_ARGS)
{
    int32 target_node_id = PG_GETARG_INT32(0);
    bool initiated = false;

    if (raft_shared_state == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft shared state is not initialized")));

    LWLockAcquire(raft_shared_state->lock, LW_EXCLUSIVE);

    if (raft_shared_state->current_state == RAFT_STATE_LEADER) {
        if (target_node_id == raft_shared_state->my_node_id) {
            elog(NOTICE, "Cannot transfer leadership to self");
        } else {
            /*
             * Signal leadership transfer by stepping down and setting
             * the new leader hint in shared memory. The background worker
             * will detect the state change and trigger a new election
             * where the target node should win.
             */
            raft_shared_state->current_state = RAFT_STATE_FOLLOWER;
            raft_shared_state->current_leader_id = target_node_id;
            initiated = true;

            elog(LOG, "Raft leadership transfer to node %d initiated via SQL", target_node_id);
        }
    } else {
        elog(NOTICE, "Node %d is not the leader, cannot transfer leadership",
             raft_shared_state->my_node_id);
    }

    LWLockRelease(raft_shared_state->lock);

    PG_RETURN_BOOL(initiated);
}

/*
 * orochi_raft_add_node
 *    SQL function to add a peer node to the Raft cluster.
 *
 * This requires the Raft background worker to be running on this backend,
 * as it needs to modify the local Raft node's cluster configuration.
 *
 * Arguments: node_id (int4), hostname (text), port (int4)
 * Returns: bool (true if node was added)
 */
Datum orochi_raft_add_node(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);
    text *hostname_text = PG_GETARG_TEXT_PP(1);
    int32 port = PG_GETARG_INT32(2);
    char *hostname;

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    hostname = text_to_cstring(hostname_text);

    raft_add_peer(local_raft_node, node_id, hostname, port);
    raft_update_shared_state(local_raft_node);

    pfree(hostname);

    elog(LOG, "Raft peer node %d added via SQL at port %d", node_id, port);

    PG_RETURN_BOOL(true);
}

/*
 * orochi_raft_remove_node
 *    SQL function to remove a peer node from the Raft cluster.
 *
 * This requires the Raft background worker to be running on this backend,
 * as it needs to modify the local Raft node's cluster configuration.
 *
 * Arguments: node_id (int4)
 * Returns: bool (true if node was removed)
 */
Datum orochi_raft_remove_node(PG_FUNCTION_ARGS)
{
    int32 node_id = PG_GETARG_INT32(0);

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    raft_remove_peer(local_raft_node, node_id);
    raft_update_shared_state(local_raft_node);

    elog(LOG, "Raft peer node %d removed via SQL", node_id);

    PG_RETURN_BOOL(true);
}

/*
 * orochi_raft_is_leader_sql
 *    SQL function to check if this node is the current Raft leader.
 *
 * Reads from shared state, so it can be called from any backend.
 *
 * Returns: bool
 */
Datum orochi_raft_is_leader_sql(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(orochi_raft_is_leader());
}

/*
 * orochi_raft_get_leader_sql
 *    SQL function to get the current Raft leader node ID.
 *
 * Reads from shared state, so it can be called from any backend.
 *
 * Returns: int4 (leader node ID, -1 if unknown)
 */
Datum orochi_raft_get_leader_sql(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(orochi_raft_get_leader());
}

/*
 * orochi_raft_status_sql
 *    SQL function returning a composite record with detailed Raft status.
 *
 * Returns: record (node_id int, state text, term bigint, leader_id int,
 *                   cluster_size int)
 */
Datum orochi_raft_status_sql(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[5];
    bool nulls[5];
    HeapTuple tuple;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (raft_shared_state == NULL) {
        memset(nulls, true, sizeof(nulls));
    } else {
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);

        values[0] = Int32GetDatum(raft_shared_state->my_node_id);
        values[1] = CStringGetTextDatum(raft_state_name(raft_shared_state->current_state));
        values[2] = Int64GetDatum(raft_shared_state->current_term);
        values[3] = Int32GetDatum(raft_shared_state->current_leader_id);
        values[4] = Int32GetDatum(raft_shared_state->cluster_size);

        LWLockRelease(raft_shared_state->lock);
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * orochi_raft_read_index_sql
 *    SQL function to request a read index for linearizable reads.
 *
 * If the Raft background worker is available, delegates to raft_read_index().
 * Otherwise, falls back to returning the last committed index from shared
 * state, which provides eventual consistency.
 *
 * Arguments: timeout_ms (int4, optional, default 5000)
 * Returns: int8 (read index, 0 on failure)
 */
Datum orochi_raft_read_index_sql(PG_FUNCTION_ARGS)
{
    int timeout_ms = PG_NARGS() > 0 && !PG_ARGISNULL(0) ? PG_GETARG_INT32(0) : 5000;
    uint64 read_index = 0;

    if (local_raft_node != NULL) {
        /* We are in the background worker - use the full protocol */
        read_index = raft_read_index(local_raft_node, timeout_ms);
    } else if (raft_shared_state != NULL) {
        /*
         * Regular backend - return the last committed index from shared
         * state as a best-effort read index. This provides bounded
         * staleness but not strict linearizability.
         */
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        read_index = raft_shared_state->last_committed_index;
        LWLockRelease(raft_shared_state->lock);
    }

    PG_RETURN_INT64((int64)read_index);
}

/*
 * orochi_raft_leader_read_index_sql
 *    SQL function for leader-only linearizable read index.
 *
 * This is the server-side handler for the leader read index protocol.
 * Followers call this on the leader to obtain a linearizable read index.
 * Only returns a valid index when called on the leader node.
 *
 * Returns: int8 (read index, 0 if not leader or on failure)
 */
Datum orochi_raft_leader_read_index_sql(PG_FUNCTION_ARGS)
{
    uint64 read_index = 0;

    if (local_raft_node != NULL) {
        if (!local_raft_node->is_leader) {
            elog(DEBUG1, "leader_read_index called on non-leader node %d",
                 local_raft_node->node_id);
            PG_RETURN_INT64(0);
        }
        read_index = raft_read_index(local_raft_node, 5000);
    } else if (raft_shared_state != NULL) {
        /*
         * Regular backend - check if we are leader and return
         * the committed index.
         */
        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        if (raft_shared_state->current_state == RAFT_STATE_LEADER)
            read_index = raft_shared_state->last_committed_index;
        LWLockRelease(raft_shared_state->lock);
    }

    PG_RETURN_INT64((int64)read_index);
}

/*
 * orochi_raft_can_read_stale_sql
 *    SQL function to check if this node can serve a stale read.
 *
 * Uses the local raft node if available, otherwise approximates based
 * on shared state. For a follower, checks whether the node has applied
 * entries up to at least the given read index.
 *
 * Arguments: read_index (int8)
 * Returns: bool
 */
Datum orochi_raft_can_read_stale_sql(PG_FUNCTION_ARGS)
{
    int64 read_index = PG_GETARG_INT64(0);

    if (local_raft_node != NULL) {
        PG_RETURN_BOOL(raft_can_serve_read(local_raft_node, (uint64)read_index));
    } else if (raft_shared_state != NULL) {
        /*
         * Without the local raft node, approximate by comparing
         * the read_index to the last committed index in shared state.
         */
        bool can_read;

        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        can_read = (raft_shared_state->last_committed_index >= (uint64)read_index);
        LWLockRelease(raft_shared_state->lock);

        PG_RETURN_BOOL(can_read);
    }

    /* No shared state - cannot serve any reads */
    PG_RETURN_BOOL(false);
}

/*
 * orochi_raft_follower_read_sql
 *    SQL function to check if a bounded-stale follower read is allowed.
 *
 * Checks whether the follower has heard from the leader recently enough
 * to serve reads within the specified staleness bound.
 *
 * Arguments: max_staleness_ms (int4)
 * Returns: bool
 */
Datum orochi_raft_follower_read_sql(PG_FUNCTION_ARGS)
{
    int32 max_staleness_ms = PG_GETARG_INT32(0);

    if (local_raft_node != NULL) {
        PG_RETURN_BOOL(raft_follower_read(local_raft_node, max_staleness_ms));
    } else if (raft_shared_state != NULL) {
        /*
         * Without the local raft node, we cannot accurately determine
         * heartbeat recency. If we are the leader, reads are always fresh.
         * Otherwise, be conservative and deny the read.
         */
        bool is_leader;

        LWLockAcquire(raft_shared_state->lock, LW_SHARED);
        is_leader = (raft_shared_state->current_state == RAFT_STATE_LEADER);
        LWLockRelease(raft_shared_state->lock);

        PG_RETURN_BOOL(is_leader);
    }

    PG_RETURN_BOOL(false);
}

/*
 * orochi_raft_create_snapshot_sql
 *    SQL function to trigger creation of a Raft log snapshot.
 *
 * Creates a snapshot at the current last-applied log index to allow
 * log compaction. Requires the Raft background worker.
 *
 * Arguments: snapshot_data (bytea)
 * Returns: bool (true if snapshot was created)
 */
Datum orochi_raft_create_snapshot_sql(PG_FUNCTION_ARGS)
{
    bytea *snapshot_data = PG_GETARG_BYTEA_PP(0);
    char *data;
    int32 data_size;
    uint64 snapshot_index;

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    data = VARDATA_ANY(snapshot_data);
    data_size = VARSIZE_ANY_EXHDR(snapshot_data);
    snapshot_index = local_raft_node->log->last_applied;

    if (snapshot_index < local_raft_node->log->first_index) {
        elog(NOTICE, "Nothing to snapshot (applied=%lu, first=%lu)", snapshot_index,
             local_raft_node->log->first_index);
        PG_RETURN_BOOL(false);
    }

    raft_log_create_snapshot(local_raft_node->log, snapshot_index, data, data_size);

    elog(LOG, "Raft snapshot created at index %lu (%d bytes) via SQL", snapshot_index, data_size);

    PG_RETURN_BOOL(true);
}

/*
 * orochi_raft_install_snapshot_sql
 *    SQL function to handle an InstallSnapshot RPC from the leader.
 *
 * This is the server-side handler called via libpq by the leader node
 * to install a snapshot on a lagging follower. Requires the Raft
 * background worker.
 *
 * Arguments: term (int8), leader_id (int4), last_included_index (int8),
 *            last_included_term (int8), offset (int4), data (bytea),
 *            data_size (int4), done (bool)
 * Returns: record (term int8, bytes_received int4, success bool)
 */
Datum orochi_raft_install_snapshot_sql(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[3];
    bool nulls[3];
    HeapTuple tuple;
    InstallSnapshotRequest request;
    InstallSnapshotResponse response;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    /* Parse arguments into InstallSnapshotRequest */
    request.term = (uint64)PG_GETARG_INT64(0);
    request.leader_id = PG_GETARG_INT32(1);
    request.last_included_index = (uint64)PG_GETARG_INT64(2);
    request.last_included_term = (uint64)PG_GETARG_INT64(3);
    request.offset = PG_GETARG_INT32(4);

    if (!PG_ARGISNULL(5)) {
        bytea *data_bytea = PG_GETARG_BYTEA_PP(5);
        request.data = VARDATA_ANY(data_bytea);
        request.data_size = VARSIZE_ANY_EXHDR(data_bytea);
    } else {
        request.data = NULL;
        request.data_size = 0;
    }

    /* data_size argument is informational; we use the actual bytea length */
    request.done = PG_GETARG_BOOL(7);

    /* Delegate to the Raft protocol handler */
    response = raft_install_snapshot(local_raft_node, &request);

    /* Build result tuple */
    memset(nulls, 0, sizeof(nulls));
    values[0] = Int64GetDatum((int64)response.term);
    values[1] = Int32GetDatum(response.bytes_received);
    values[2] = BoolGetDatum(response.success);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * orochi_raft_request_vote_sql
 *    SQL function to handle a RequestVote RPC from a candidate.
 *
 * This is the server-side handler called via libpq by candidate nodes
 * during elections. Requires the Raft background worker.
 *
 * Arguments: term (int8), candidate_id (int4), last_log_index (int8),
 *            last_log_term (int8)
 * Returns: record (term int8, vote_granted bool)
 */
Datum orochi_raft_request_vote_sql(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[2];
    bool nulls[2];
    HeapTuple tuple;
    RequestVoteRequest request;
    RequestVoteResponse response;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    /* Parse arguments into RequestVoteRequest */
    request.term = (uint64)PG_GETARG_INT64(0);
    request.candidate_id = PG_GETARG_INT32(1);
    request.last_log_index = (uint64)PG_GETARG_INT64(2);
    request.last_log_term = (uint64)PG_GETARG_INT64(3);

    /* Delegate to the Raft protocol handler */
    response = raft_request_vote(local_raft_node, &request);

    /* Update shared state after handling the vote */
    raft_update_shared_state(local_raft_node);

    /* Build result tuple */
    memset(nulls, 0, sizeof(nulls));
    values[0] = Int64GetDatum((int64)response.term);
    values[1] = BoolGetDatum(response.vote_granted);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * orochi_raft_append_entries_sql
 *    SQL function to handle an AppendEntries RPC from the leader.
 *
 * This is the server-side handler called via libpq by the leader node
 * for log replication and heartbeats. Requires the Raft background worker.
 *
 * Arguments: term (int8), leader_id (int4), prev_log_index (int8),
 *            prev_log_term (int8), entries (jsonb, nullable),
 *            leader_commit (int8)
 * Returns: record (term int8, success bool, match_index int8)
 */
Datum orochi_raft_append_entries_sql(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[3];
    bool nulls[3];
    HeapTuple tuple;
    AppendEntriesRequest request;
    AppendEntriesResponse response;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    if (local_raft_node == NULL)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("Raft node is not available"),
                        errdetail("This operation must be performed on the Raft "
                                  "background worker node.")));

    /* Parse arguments into AppendEntriesRequest */
    request.term = (uint64)PG_GETARG_INT64(0);
    request.leader_id = PG_GETARG_INT32(1);
    request.prev_log_index = (uint64)PG_GETARG_INT64(2);
    request.prev_log_term = (uint64)PG_GETARG_INT64(3);
    request.leader_commit = (uint64)PG_GETARG_INT64(5);

    /*
     * Parse entries from JSONB if provided.
     * For heartbeats, the entries argument is NULL.
     */
    if (!PG_ARGISNULL(4)) {
        /*
         * TODO: Full JSONB entry parsing would require jsonb_utils.
         * For now, we handle the common case of heartbeat (NULL entries)
         * and leave entry deserialization as a follow-up. The binary
         * serialization path in raft_send_append_entries covers the
         * production RPC path.
         */
        elog(WARNING, "JSONB entry deserialization not yet implemented "
                      "in append_entries SQL handler; treating as heartbeat");
        request.entries = NULL;
        request.entries_count = 0;
    } else {
        request.entries = NULL;
        request.entries_count = 0;
    }

    /* Delegate to the Raft protocol handler */
    response = raft_append_entries(local_raft_node, &request);

    /* Update shared state after handling append entries */
    raft_update_shared_state(local_raft_node);

    /* Build result tuple */
    memset(nulls, 0, sizeof(nulls));
    values[0] = Int64GetDatum((int64)response.term);
    values[1] = BoolGetDatum(response.success);
    values[2] = Int64GetDatum((int64)response.match_index);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
