/*-------------------------------------------------------------------------
 *
 * raft.h
 *    Orochi DB Raft consensus protocol implementation
 *
 * This module implements the Raft consensus algorithm for distributed
 * coordination in Orochi DB. It provides:
 *   - Leader election with term-based voting
 *   - Log replication across cluster nodes
 *   - Heartbeat mechanism for failure detection
 *   - State machine replication for consistency
 *
 * Based on the Raft paper by Ongaro and Ousterhout (2014).
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_RAFT_H
#define OROCHI_RAFT_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"
#include "libpq-fe.h"

#include "../orochi.h"

/* ============================================================
 * Raft Configuration
 * ============================================================ */

#define RAFT_ELECTION_TIMEOUT_MIN_MS    150
#define RAFT_ELECTION_TIMEOUT_MAX_MS    300
#define RAFT_HEARTBEAT_INTERVAL_MS      50
#define RAFT_RPC_TIMEOUT_MS             100
#define RAFT_MAX_LOG_ENTRIES            10000
#define RAFT_MAX_ENTRIES_PER_APPEND     100
#define RAFT_SNAPSHOT_THRESHOLD         1000
#define RAFT_MAX_CLUSTER_SIZE           32

/* ============================================================
 * Raft State Definitions
 * ============================================================ */

/*
 * RaftState - Node state in the Raft protocol
 */
typedef enum RaftState
{
    RAFT_STATE_FOLLOWER = 0,      /* Passive, responds to RPCs */
    RAFT_STATE_CANDIDATE,          /* Requesting votes for election */
    RAFT_STATE_LEADER              /* Active leader, handles client requests */
} RaftState;

/*
 * RaftLogEntryType - Types of log entries
 */
typedef enum RaftLogEntryType
{
    RAFT_LOG_COMMAND = 0,         /* Normal state machine command */
    RAFT_LOG_CONFIG_CHANGE,       /* Cluster configuration change */
    RAFT_LOG_NOOP                 /* No-op entry (used on leader election) */
} RaftLogEntryType;

/* ============================================================
 * Raft Log Structures
 * ============================================================ */

/*
 * RaftLogEntry - Single entry in the Raft log
 */
typedef struct RaftLogEntry
{
    uint64              index;              /* Log index (1-based) */
    uint64              term;               /* Term when entry was created */
    RaftLogEntryType    type;               /* Entry type */
    int32               command_size;       /* Size of command data */
    char               *command_data;       /* Serialized command */
    TimestampTz         created_at;         /* Creation timestamp */
} RaftLogEntry;

/*
 * RaftLog - The Raft log containing all entries
 */
typedef struct RaftLog
{
    RaftLogEntry       *entries;            /* Array of log entries */
    uint64              first_index;        /* First log index (after compaction) */
    uint64              last_index;         /* Last log index */
    uint64              commit_index;       /* Highest committed index */
    uint64              last_applied;       /* Highest applied index */
    int32               capacity;           /* Allocated capacity */
    LWLock             *lock;               /* Concurrency control */
    char               *wal_path;           /* Path to persistent WAL file */
    bool                persistence_enabled;/* Enable WAL persistence */
} RaftLog;

/* ============================================================
 * Raft RPC Message Types
 * ============================================================ */

/*
 * RequestVoteRequest - Sent by candidates to request votes
 */
typedef struct RequestVoteRequest
{
    uint64              term;               /* Candidate's term */
    int32               candidate_id;       /* Candidate requesting vote */
    uint64              last_log_index;     /* Index of candidate's last log entry */
    uint64              last_log_term;      /* Term of candidate's last log entry */
} RequestVoteRequest;

/*
 * RequestVoteResponse - Response to RequestVote
 */
typedef struct RequestVoteResponse
{
    uint64              term;               /* Current term, for candidate to update */
    bool                vote_granted;       /* True if vote was granted */
    int32               voter_id;           /* ID of responding node */
} RequestVoteResponse;

/*
 * AppendEntriesRequest - Sent by leader for log replication
 */
typedef struct AppendEntriesRequest
{
    uint64              term;               /* Leader's term */
    int32               leader_id;          /* Leader's node ID */
    uint64              prev_log_index;     /* Index of entry before new ones */
    uint64              prev_log_term;      /* Term of prev_log_index entry */
    RaftLogEntry       *entries;            /* Log entries to store (may be empty) */
    int32               entries_count;      /* Number of entries */
    uint64              leader_commit;      /* Leader's commit index */
} AppendEntriesRequest;

/*
 * AppendEntriesResponse - Response to AppendEntries
 */
typedef struct AppendEntriesResponse
{
    uint64              term;               /* Current term, for leader to update */
    bool                success;            /* True if follower matched prev_log */
    uint64              match_index;        /* Highest log index known to match */
    int32               node_id;            /* ID of responding node */
} AppendEntriesResponse;

/*
 * InstallSnapshotRequest - For transferring snapshots to lagging followers
 */
typedef struct InstallSnapshotRequest
{
    uint64              term;               /* Leader's term */
    int32               leader_id;          /* Leader's node ID */
    uint64              last_included_index;/* Index of last entry in snapshot */
    uint64              last_included_term; /* Term of last entry in snapshot */
    int32               offset;             /* Byte offset for chunk */
    char               *data;               /* Raw snapshot data chunk */
    int32               data_size;          /* Size of this chunk */
    bool                done;               /* True if this is the last chunk */
} InstallSnapshotRequest;

/*
 * InstallSnapshotResponse - Response to InstallSnapshot
 */
typedef struct InstallSnapshotResponse
{
    uint64              term;               /* Current term, for leader to update */
    int32               node_id;            /* ID of responding node */
} InstallSnapshotResponse;

/* ============================================================
 * Raft Cluster Structures
 * ============================================================ */

/*
 * RaftPeer - Information about a peer node in the cluster
 */
typedef struct RaftPeer
{
    int32               node_id;            /* Peer's unique node ID */
    char               *hostname;           /* Peer hostname */
    int                 port;               /* Peer PostgreSQL port */
    bool                is_active;          /* Is peer responsive? */
    uint64              next_index;         /* Next log index to send */
    uint64              match_index;        /* Highest log index known to replicate */
    TimestampTz         last_contact;       /* Last successful RPC time */
    PGconn             *connection;         /* Persistent connection to peer */
    bool                vote_granted;       /* Did peer vote for us in current term? */
} RaftPeer;

/*
 * RaftCluster - Cluster membership and configuration
 */
typedef struct RaftCluster
{
    RaftPeer           *peers;              /* Array of peer nodes */
    int32               peer_count;         /* Number of peers */
    int32               quorum_size;        /* Votes needed for majority */
    int32               my_node_id;         /* This node's ID */
    LWLock             *lock;               /* Concurrency control */
} RaftCluster;

/* ============================================================
 * Main Raft Node Structure
 * ============================================================ */

/*
 * RaftNode - Complete state of a Raft node
 */
typedef struct RaftNode
{
    /* Persistent state (saved to disk) */
    uint64              current_term;       /* Latest term server has seen */
    int32               voted_for;          /* Candidate voted for in current term (-1 = none) */
    RaftLog            *log;                /* The Raft log */

    /* Volatile state */
    RaftState           state;              /* Current role: follower/candidate/leader */
    int32               node_id;            /* This node's unique identifier */
    int32               leader_id;          /* Current known leader (-1 = unknown) */

    /* Cluster information */
    RaftCluster        *cluster;            /* Cluster membership */

    /* Timing state */
    TimestampTz         election_timeout;   /* When to start election */
    TimestampTz         last_heartbeat;     /* Last heartbeat received */

    /* Leader-only volatile state */
    bool                is_leader;          /* Shortcut for state == LEADER */

    /* Callbacks */
    void              (*apply_callback)(RaftLogEntry *entry, void *context);
    void               *apply_context;      /* Context for apply callback */

    /* Statistics */
    uint64              elections_started;  /* Number of elections started */
    uint64              elections_won;      /* Number of elections won */
    uint64              append_entries_sent;/* Total AppendEntries sent */
    uint64              votes_received;     /* Votes in current election */

    /* Memory context */
    MemoryContext       raft_context;       /* Memory context for Raft allocations */

    /* Synchronization */
    LWLock             *state_lock;         /* Protects state changes */
    bool                initialized;        /* Is node initialized? */
    bool                running;            /* Is Raft protocol running? */
} RaftNode;

/* ============================================================
 * Raft Node Lifecycle Functions
 * ============================================================ */

/*
 * Initialize Raft node with given configuration
 */
extern RaftNode *raft_init(int32 node_id, const char *wal_path);

/*
 * Start Raft protocol (background processing)
 */
extern void raft_start(RaftNode *node);

/*
 * Stop Raft protocol
 */
extern void raft_stop(RaftNode *node);

/*
 * Cleanup and free Raft node
 */
extern void raft_shutdown(RaftNode *node);

/*
 * Periodic tick - call this regularly to drive Raft state machine
 */
extern void raft_tick(RaftNode *node);

/* ============================================================
 * Raft Cluster Management
 * ============================================================ */

/*
 * Add a peer to the cluster
 */
extern void raft_add_peer(RaftNode *node, int32 peer_id,
                          const char *hostname, int port);

/*
 * Remove a peer from the cluster
 */
extern void raft_remove_peer(RaftNode *node, int32 peer_id);

/*
 * Connect to all peers in the cluster
 */
extern void raft_connect_peers(RaftNode *node);

/*
 * Disconnect from all peers
 */
extern void raft_disconnect_peers(RaftNode *node);

/* ============================================================
 * Raft RPC Handlers (called when receiving RPCs)
 * ============================================================ */

/*
 * Handle RequestVote RPC from a candidate
 */
extern RequestVoteResponse raft_request_vote(RaftNode *node,
                                             RequestVoteRequest *request);

/*
 * Handle AppendEntries RPC from leader
 */
extern AppendEntriesResponse raft_append_entries(RaftNode *node,
                                                 AppendEntriesRequest *request);

/*
 * Handle InstallSnapshot RPC from leader
 */
extern InstallSnapshotResponse raft_install_snapshot(RaftNode *node,
                                                     InstallSnapshotRequest *request);

/* ============================================================
 * Raft Leader Functions
 * ============================================================ */

/*
 * Start an election (called when election timeout expires)
 */
extern void raft_start_election(RaftNode *node);

/*
 * Send heartbeat to all followers (as leader)
 */
extern void raft_heartbeat(RaftNode *node);

/*
 * Replicate log entries to followers (as leader)
 */
extern void raft_replicate(RaftNode *node);

/*
 * Update commit index based on peer acknowledgments
 */
extern void raft_update_commit_index(RaftNode *node);

/* ============================================================
 * Raft Client Interface
 * ============================================================ */

/*
 * Submit a command to the replicated state machine
 * Returns the log index if successful, 0 if not leader
 */
extern uint64 raft_submit_command(RaftNode *node, const char *command,
                                  int32 command_size);

/*
 * Check if a log entry has been committed
 */
extern bool raft_is_committed(RaftNode *node, uint64 log_index);

/*
 * Wait for a log entry to be committed (with timeout)
 */
extern bool raft_wait_for_commit(RaftNode *node, uint64 log_index,
                                 int timeout_ms);

/*
 * Register callback for applying committed entries
 */
extern void raft_set_apply_callback(RaftNode *node,
                                    void (*callback)(RaftLogEntry *entry, void *context),
                                    void *context);

/* ============================================================
 * Raft Log Management (see raft_log.c)
 * ============================================================ */

/*
 * Initialize the Raft log
 */
extern RaftLog *raft_log_init(const char *wal_path);

/*
 * Append entries to the log
 */
extern bool raft_log_append(RaftLog *log, RaftLogEntry *entries, int32 count);

/*
 * Get log entry at index
 */
extern RaftLogEntry *raft_log_get(RaftLog *log, uint64 index);

/*
 * Get entries from start_index to end_index (inclusive)
 */
extern RaftLogEntry *raft_log_get_range(RaftLog *log, uint64 start_index,
                                        uint64 end_index, int32 *count);

/*
 * Truncate log after given index (exclusive)
 */
extern void raft_log_truncate(RaftLog *log, uint64 after_index);

/*
 * Get term of entry at index (0 if not found)
 */
extern uint64 raft_log_term_at(RaftLog *log, uint64 index);

/*
 * Persist log entries to WAL
 */
extern void raft_log_persist(RaftLog *log);

/*
 * Recover log from WAL on startup
 */
extern void raft_log_recover(RaftLog *log);

/*
 * Compact log up to given index (create snapshot)
 */
extern void raft_log_compact(RaftLog *log, uint64 up_to_index);

/*
 * Free log resources
 */
extern void raft_log_free(RaftLog *log);

/*
 * Create a snapshot of the state machine
 */
extern void raft_log_create_snapshot(RaftLog *log, uint64 last_included_index,
                                     const char *snapshot_data, int32 snapshot_size);

/*
 * Load the most recent snapshot
 * Returns snapshot data that caller must free, or NULL if no snapshot.
 */
extern char *raft_log_load_snapshot(RaftLog *log, uint64 *last_included_index,
                                    uint64 *last_included_term, int32 *data_size);

/*
 * Get log statistics
 */
extern void raft_log_get_stats(RaftLog *log, uint64 *first_index, uint64 *last_index,
                               uint64 *commit_index, uint64 *last_applied,
                               int32 *entry_count, int32 *capacity);

/*
 * Dump log contents for debugging
 */
extern void raft_log_dump(RaftLog *log, StringInfo buf);

/* ============================================================
 * Raft State Persistence
 * ============================================================ */

/*
 * Save persistent state (current_term, voted_for) to disk
 */
extern void raft_persist_state(RaftNode *node);

/*
 * Load persistent state from disk
 */
extern void raft_load_state(RaftNode *node);

/* ============================================================
 * Raft Utility Functions
 * ============================================================ */

/*
 * Get current leader node ID (-1 if unknown)
 */
extern int32 raft_get_leader(RaftNode *node);

/*
 * Check if this node is the leader
 */
extern bool raft_is_leader(RaftNode *node);

/*
 * Get current term
 */
extern uint64 raft_get_term(RaftNode *node);

/*
 * Get current state as string
 */
extern const char *raft_state_name(RaftState state);

/*
 * Get node statistics
 */
extern void raft_get_stats(RaftNode *node, uint64 *term, RaftState *state,
                           int32 *leader_id, uint64 *commit_index,
                           uint64 *last_applied);

/*
 * Generate random election timeout
 */
extern int raft_random_election_timeout(void);

/* ============================================================
 * Raft RPC Serialization/Deserialization
 * ============================================================ */

/*
 * Serialize RPC requests/responses for network transmission
 */
extern char *raft_serialize_request_vote(RequestVoteRequest *req, int32 *size);
extern char *raft_serialize_append_entries(AppendEntriesRequest *req, int32 *size);
extern RequestVoteRequest *raft_deserialize_request_vote(const char *data, int32 size);
extern AppendEntriesRequest *raft_deserialize_append_entries(const char *data, int32 size);

/*
 * Send RPC to peer node via libpq
 */
extern RequestVoteResponse raft_send_request_vote(RaftPeer *peer,
                                                  RequestVoteRequest *request);
extern AppendEntriesResponse raft_send_append_entries(RaftPeer *peer,
                                                      AppendEntriesRequest *request);

#endif /* OROCHI_RAFT_H */
