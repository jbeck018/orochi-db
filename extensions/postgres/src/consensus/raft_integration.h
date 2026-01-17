/*-------------------------------------------------------------------------
 *
 * raft_integration.h
 *    Orochi DB Raft consensus integration with distributed executor
 *
 * This module provides:
 *   - Shared memory initialization for Raft state
 *   - Background worker for Raft consensus protocol
 *   - Integration with distributed executor for consensus-based commits
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_RAFT_INTEGRATION_H
#define OROCHI_RAFT_INTEGRATION_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "fmgr.h"

#include "raft.h"

/* ============================================================
 * Raft Shared Memory State
 * ============================================================ */

/*
 * RaftSharedState - Global Raft state stored in shared memory
 * This allows all backends to access the current Raft state
 */
typedef struct RaftSharedState
{
    LWLock         *lock;               /* Protects shared state access */

    /* Current node state (cached from RaftNode for fast access) */
    int32           my_node_id;         /* This node's identifier */
    int32           current_leader_id;  /* Current Raft leader (-1 if unknown) */
    uint64          current_term;       /* Current Raft term */
    RaftState       current_state;      /* Current node state */

    /* Cluster information */
    int32           cluster_size;       /* Total nodes in cluster */
    int32           quorum_size;        /* Nodes needed for majority */

    /* Worker state */
    int             worker_pid;         /* PID of Raft background worker */
    bool            is_initialized;     /* Has Raft been initialized? */
    bool            is_running;         /* Is Raft protocol running? */

    /* Statistics */
    uint64          commands_submitted; /* Total commands submitted */
    uint64          commands_committed; /* Total commands committed */
    uint64          elections_held;     /* Total elections held */

    /* Pending command queue (for inter-process communication) */
    int32           pending_commands;   /* Number of commands awaiting consensus */
    uint64          last_committed_index; /* Last committed log index */
} RaftSharedState;

/* Global pointer to shared state */
extern RaftSharedState *raft_shared_state;

/* ============================================================
 * Shared Memory Functions
 * ============================================================ */

/*
 * Get size of Raft shared memory segment
 */
extern Size orochi_raft_shmem_size(void);

/*
 * Initialize Raft shared memory structures
 */
extern void orochi_raft_shmem_init(void);

/* ============================================================
 * Background Worker
 * ============================================================ */

/*
 * Entry point for Raft consensus background worker
 */
extern void orochi_raft_worker_main(Datum main_arg);

/* ============================================================
 * Distributed Executor Integration
 * ============================================================ */

/*
 * Submit a distributed transaction for Raft consensus
 * Returns true if successfully submitted (doesn't mean committed)
 */
extern bool orochi_raft_submit_transaction(const char *gid,
                                           List *participant_nodes,
                                           const char *command_data,
                                           int32 command_size);

/*
 * Wait for a transaction to be committed via Raft
 * Returns true if committed within timeout
 */
extern bool orochi_raft_wait_for_commit(const char *gid, int timeout_ms);

/*
 * Check if this node is the current Raft leader
 */
extern bool orochi_raft_is_leader(void);

/*
 * Get the current Raft leader node ID
 */
extern int32 orochi_raft_get_leader(void);

/*
 * Get current Raft term
 */
extern uint64 orochi_raft_get_term(void);

/*
 * Get current Raft state as string
 */
extern const char *orochi_raft_get_state_name(void);

/* ============================================================
 * Cluster Management Integration
 * ============================================================ */

/*
 * Register this node with the Raft cluster
 * Called when Orochi starts on a node
 */
extern void orochi_raft_register_node(int32 node_id,
                                      const char *hostname,
                                      int port);

/*
 * Add a peer node to the Raft cluster
 */
extern void orochi_raft_add_cluster_node(int32 node_id,
                                         const char *hostname,
                                         int port);

/*
 * Remove a node from the Raft cluster
 */
extern void orochi_raft_remove_cluster_node(int32 node_id);

/* ============================================================
 * Transaction Coordination
 * ============================================================ */

/*
 * Prepare a distributed transaction using Raft
 * Phase 1 of 2PC: prepare and replicate to cluster
 */
extern bool orochi_raft_prepare_transaction(const char *gid,
                                            List *shard_ids);

/*
 * Commit a prepared distributed transaction
 * Phase 2 of 2PC: commit after successful prepare
 */
extern bool orochi_raft_commit_transaction(const char *gid);

/*
 * Abort a distributed transaction
 */
extern bool orochi_raft_abort_transaction(const char *gid);

/*
 * Check transaction status
 */
typedef enum RaftTransactionStatus
{
    RAFT_TXN_UNKNOWN = 0,
    RAFT_TXN_PENDING,
    RAFT_TXN_PREPARED,
    RAFT_TXN_COMMITTED,
    RAFT_TXN_ABORTED
} RaftTransactionStatus;

extern RaftTransactionStatus orochi_raft_get_transaction_status(const char *gid);

/* ============================================================
 * Callback Registration for State Machine
 * ============================================================ */

/*
 * Type for transaction apply callback
 */
typedef void (*RaftTransactionApplyCallback)(const char *gid,
                                              const char *command_data,
                                              int32 command_size,
                                              void *context);

/*
 * Register callback for applying committed transactions
 */
extern void orochi_raft_set_apply_callback(RaftTransactionApplyCallback callback,
                                           void *context);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_raft_status(PG_FUNCTION_ARGS);
extern Datum orochi_raft_leader(PG_FUNCTION_ARGS);
extern Datum orochi_raft_cluster_info(PG_FUNCTION_ARGS);

#endif /* OROCHI_RAFT_INTEGRATION_H */
