# Orochi DB HTAP Implementation Plan

## Executive Summary

This document provides detailed implementation specifications for four critical HTAP features identified in the competitive analysis. These features will position Orochi DB competitively against TiDB, CockroachDB, SingleStore, and ClickHouse.

| Feature | Priority | Complexity | Timeline | Dependencies |
|---------|----------|------------|----------|--------------|
| Raft Consensus Protocol | P1-Critical | High | 12-16 weeks | Existing distributed executor |
| Vectorized Query Execution | P1-Critical | High | 10-14 weeks | Columnar storage |
| Real-time Pipelines | P2-High | Medium | 8-10 weeks | Hypertables, streaming |
| Approximate Algorithms | P3-Medium | Medium | 4-6 weeks | Columnar read path |

---

## 1. Raft Consensus Protocol

### 1.1 Architecture Overview

```
+------------------+     +------------------+     +------------------+
|   Coordinator    |     |    Worker 1      |     |    Worker 2      |
|  (Raft Leader)   |     |  (Raft Follower) |     |  (Raft Follower) |
+--------+---------+     +--------+---------+     +--------+---------+
         |                        |                        |
         |    AppendEntries       |                        |
         +----------------------->|                        |
         |                        |                        |
         +----------------------------------------------->|
         |                        |                        |
         |<--- AppendEntries Response ----+----------------+
         |                        |
+--------+---------+     +--------+---------+
|   Raft Log       |     |   Raft Log       |
|  (WAL + State)   |     |  (WAL + State)   |
+------------------+     +------------------+
```

### 1.2 Design Decisions

**ADR-001: Raft Implementation Strategy**
- **Decision**: Implement Multi-Raft (one Raft group per shard) similar to TiDB
- **Rationale**:
  - Scales better than single-Raft for large clusters
  - Allows independent shard replication
  - Enables follower reads per-shard
- **Trade-offs**: Higher complexity, more state to manage

**ADR-002: Log Storage**
- **Decision**: Use PostgreSQL's existing WAL infrastructure with custom record types
- **Rationale**: Leverages PostgreSQL's crash recovery, fsync, and checkpointing
- **Alternative Rejected**: Separate RocksDB-based log (higher complexity, separate failure domain)

### 1.3 File-by-File Implementation

#### `src/consensus/raft.h` - Core Raft Types and Interface
```c
/*
 * Raft node states
 */
typedef enum RaftState
{
    RAFT_STATE_FOLLOWER = 0,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER,
    RAFT_STATE_LEARNER          /* Non-voting replica (like TiDB TiFlash) */
} RaftState;

/*
 * Raft persistent state (must survive restarts)
 */
typedef struct RaftPersistentState
{
    uint64          current_term;       /* Latest term seen */
    int32           voted_for;          /* Candidate voted for in current term (-1 if none) */
    uint64          commit_index;       /* Highest log entry known to be committed */
    uint64          last_applied;       /* Highest log entry applied to state machine */
} RaftPersistentState;

/*
 * Raft volatile state (leader only)
 */
typedef struct RaftLeaderState
{
    uint64         *next_index;         /* Per-follower: next log entry to send */
    uint64         *match_index;        /* Per-follower: highest replicated entry */
    TimestampTz    *last_contact;       /* Per-follower: last successful RPC */
} RaftLeaderState;

/*
 * Raft log entry
 */
typedef struct RaftLogEntry
{
    uint64          index;              /* Log position (1-indexed) */
    uint64          term;               /* Term when entry was created */
    RaftCommandType command_type;       /* Type of command */
    int32           shard_id;           /* Target shard (for Multi-Raft) */
    int32           data_size;          /* Size of command data */
    char           *data;               /* Serialized command */
} RaftLogEntry;

/*
 * Raft group (one per shard in Multi-Raft)
 */
typedef struct RaftGroup
{
    int64               shard_id;           /* Shard this group manages */
    RaftState           state;              /* Current node state */
    RaftPersistentState persistent;         /* Persistent state */
    RaftLeaderState    *leader_state;       /* NULL if not leader */

    /* Group membership */
    int32               self_id;            /* This node's ID in group */
    int32              *members;            /* Node IDs in Raft group */
    int32               member_count;       /* Number of members */
    int32               quorum_size;        /* Majority threshold */

    /* Timing */
    TimestampTz         election_timeout;   /* When to start election */
    TimestampTz         last_heartbeat;     /* Last leader heartbeat */
    int32               heartbeat_interval_ms;
    int32               election_timeout_ms;

    /* Log */
    List               *log;                /* In-memory log cache */
    uint64              log_start_index;    /* First index in cache */
} RaftGroup;

/* Raft message types */
typedef enum RaftMessageType
{
    RAFT_MSG_REQUEST_VOTE = 1,
    RAFT_MSG_REQUEST_VOTE_RESPONSE,
    RAFT_MSG_APPEND_ENTRIES,
    RAFT_MSG_APPEND_ENTRIES_RESPONSE,
    RAFT_MSG_INSTALL_SNAPSHOT,
    RAFT_MSG_INSTALL_SNAPSHOT_RESPONSE,
    RAFT_MSG_HEARTBEAT,
    RAFT_MSG_HEARTBEAT_RESPONSE
} RaftMessageType;

/*
 * RequestVote RPC (election)
 */
typedef struct RequestVoteRequest
{
    uint64          term;               /* Candidate's term */
    int32           candidate_id;       /* Candidate requesting vote */
    uint64          last_log_index;     /* Index of candidate's last log entry */
    uint64          last_log_term;      /* Term of candidate's last log entry */
} RequestVoteRequest;

typedef struct RequestVoteResponse
{
    uint64          term;               /* Current term, for candidate to update itself */
    bool            vote_granted;       /* True = candidate received vote */
} RequestVoteResponse;

/*
 * AppendEntries RPC (log replication + heartbeat)
 */
typedef struct AppendEntriesRequest
{
    uint64          term;               /* Leader's term */
    int32           leader_id;          /* So follower can redirect clients */
    uint64          prev_log_index;     /* Index of log entry preceding new ones */
    uint64          prev_log_term;      /* Term of prev_log_index entry */
    RaftLogEntry   *entries;            /* Log entries to store (empty for heartbeat) */
    int32           entry_count;        /* Number of entries */
    uint64          leader_commit;      /* Leader's commit_index */
} AppendEntriesRequest;

typedef struct AppendEntriesResponse
{
    uint64          term;               /* Current term, for leader to update itself */
    bool            success;            /* True if follower contained matching entry */
    uint64          match_index;        /* Highest index replicated (optimization) */
    uint64          conflict_index;     /* On failure: first conflicting index */
    uint64          conflict_term;      /* On failure: term at conflict_index */
} AppendEntriesResponse;

/* Core Raft operations */
extern void raft_init(void);
extern RaftGroup *raft_create_group(int64 shard_id, int32 *members, int32 count);
extern void raft_destroy_group(RaftGroup *group);

/* State transitions */
extern void raft_become_follower(RaftGroup *group, uint64 term);
extern void raft_become_candidate(RaftGroup *group);
extern void raft_become_leader(RaftGroup *group);

/* Message handlers */
extern RequestVoteResponse raft_handle_request_vote(RaftGroup *group, RequestVoteRequest *req);
extern AppendEntriesResponse raft_handle_append_entries(RaftGroup *group, AppendEntriesRequest *req);

/* Leader operations */
extern bool raft_propose(RaftGroup *group, void *command, int size, RaftCommandType type);
extern void raft_send_heartbeats(RaftGroup *group);
extern void raft_replicate_log(RaftGroup *group);

/* Election */
extern void raft_start_election(RaftGroup *group);
extern void raft_tick(RaftGroup *group);  /* Timer tick - check timeouts */

/* Log operations */
extern void raft_append_log(RaftGroup *group, RaftLogEntry *entry);
extern RaftLogEntry *raft_get_log_entry(RaftGroup *group, uint64 index);
extern void raft_truncate_log(RaftGroup *group, uint64 from_index);
extern void raft_persist_state(RaftGroup *group);
extern void raft_load_state(RaftGroup *group);

/* Follower reads */
extern bool raft_can_serve_read(RaftGroup *group, bool allow_stale);
extern uint64 raft_read_index(RaftGroup *group);
```

#### `src/consensus/raft.c` - Core Raft Implementation
```c
/* Key implementation functions */

/*
 * raft_tick - Called periodically to handle timeouts
 *
 * For followers: Check election timeout, start election if expired
 * For candidates: Check election timeout, restart election if expired
 * For leaders: Send heartbeats, check follower timeouts
 */
void
raft_tick(RaftGroup *group)
{
    TimestampTz now = GetCurrentTimestamp();

    switch (group->state)
    {
        case RAFT_STATE_FOLLOWER:
        case RAFT_STATE_CANDIDATE:
            if (now >= group->election_timeout)
            {
                raft_start_election(group);
            }
            break;

        case RAFT_STATE_LEADER:
            if (now - group->last_heartbeat >= group->heartbeat_interval_ms * 1000)
            {
                raft_send_heartbeats(group);
                group->last_heartbeat = now;
            }
            break;

        case RAFT_STATE_LEARNER:
            /* Learners don't participate in elections */
            break;
    }
}

/*
 * raft_handle_request_vote - Process RequestVote RPC
 *
 * Grant vote if:
 * 1. Candidate's term >= our term
 * 2. We haven't voted for someone else this term
 * 3. Candidate's log is at least as up-to-date as ours
 */
RequestVoteResponse
raft_handle_request_vote(RaftGroup *group, RequestVoteRequest *req)
{
    RequestVoteResponse response;

    response.term = group->persistent.current_term;
    response.vote_granted = false;

    /* Reject if candidate's term is stale */
    if (req->term < group->persistent.current_term)
        return response;

    /* Update term if candidate has higher term */
    if (req->term > group->persistent.current_term)
    {
        raft_become_follower(group, req->term);
    }

    /* Check if we can vote for this candidate */
    bool can_vote = (group->persistent.voted_for == -1 ||
                     group->persistent.voted_for == req->candidate_id);

    /* Check log is up-to-date (Section 5.4.1 of Raft paper) */
    uint64 last_term = raft_get_last_log_term(group);
    uint64 last_index = raft_get_last_log_index(group);

    bool log_ok = (req->last_log_term > last_term) ||
                  (req->last_log_term == last_term && req->last_log_index >= last_index);

    if (can_vote && log_ok)
    {
        group->persistent.voted_for = req->candidate_id;
        response.vote_granted = true;
        raft_reset_election_timeout(group);
        raft_persist_state(group);
    }

    response.term = group->persistent.current_term;
    return response;
}

/*
 * raft_handle_append_entries - Process AppendEntries RPC
 *
 * This is the core of Raft's log replication.
 */
AppendEntriesResponse
raft_handle_append_entries(RaftGroup *group, AppendEntriesRequest *req)
{
    AppendEntriesResponse response;

    response.term = group->persistent.current_term;
    response.success = false;

    /* Reply false if term < currentTerm (Section 5.1) */
    if (req->term < group->persistent.current_term)
        return response;

    /* Recognize leader, reset election timeout */
    if (req->term > group->persistent.current_term)
    {
        raft_become_follower(group, req->term);
    }
    else if (group->state == RAFT_STATE_CANDIDATE)
    {
        raft_become_follower(group, req->term);
    }

    raft_reset_election_timeout(group);

    /* Check log consistency (Section 5.3) */
    if (req->prev_log_index > 0)
    {
        RaftLogEntry *prev_entry = raft_get_log_entry(group, req->prev_log_index);

        if (prev_entry == NULL || prev_entry->term != req->prev_log_term)
        {
            /* Log doesn't match - return conflict info for fast backup */
            response.conflict_index = req->prev_log_index;
            if (prev_entry)
                response.conflict_term = prev_entry->term;
            return response;
        }
    }

    /* Append new entries, removing conflicting ones */
    for (int i = 0; i < req->entry_count; i++)
    {
        uint64 idx = req->prev_log_index + 1 + i;
        RaftLogEntry *existing = raft_get_log_entry(group, idx);

        if (existing && existing->term != req->entries[i].term)
        {
            /* Conflict - truncate log from here */
            raft_truncate_log(group, idx);
            existing = NULL;
        }

        if (!existing)
        {
            raft_append_log(group, &req->entries[i]);
        }
    }

    /* Update commit index */
    if (req->leader_commit > group->persistent.commit_index)
    {
        uint64 last_new_index = req->prev_log_index + req->entry_count;
        group->persistent.commit_index = Min(req->leader_commit, last_new_index);
        raft_apply_committed_entries(group);
    }

    response.success = true;
    response.match_index = req->prev_log_index + req->entry_count;
    raft_persist_state(group);

    return response;
}
```

#### `src/consensus/raft_log.h` - Raft Log Storage
```c
/*
 * Raft log storage using PostgreSQL WAL
 */

/* WAL record types for Raft */
#define XLOG_RAFT_LOG_ENTRY     0x00    /* Log entry append */
#define XLOG_RAFT_VOTE          0x10    /* Vote record */
#define XLOG_RAFT_TERM          0x20    /* Term change */
#define XLOG_RAFT_SNAPSHOT      0x30    /* Snapshot marker */

/*
 * WAL record for Raft log entry
 */
typedef struct xl_raft_log_entry
{
    int64           shard_id;
    uint64          index;
    uint64          term;
    RaftCommandType command_type;
    int32           data_size;
    /* command data follows */
} xl_raft_log_entry;

/*
 * On-disk Raft state (persisted in pg_control or separate file)
 */
typedef struct RaftDiskState
{
    uint64          current_term;
    int32           voted_for;
    uint64          last_log_index;
    uint64          snapshot_index;
    uint64          snapshot_term;
} RaftDiskState;

/* Log persistence functions */
extern void raft_log_init(void);
extern void raft_log_write_entry(int64 shard_id, RaftLogEntry *entry);
extern void raft_log_write_vote(int64 shard_id, uint64 term, int32 voted_for);
extern List *raft_log_read_entries(int64 shard_id, uint64 from_index, uint64 to_index);
extern void raft_log_truncate(int64 shard_id, uint64 from_index);

/* State persistence */
extern void raft_state_persist(int64 shard_id, RaftPersistentState *state);
extern RaftPersistentState *raft_state_load(int64 shard_id);

/* Snapshot operations */
extern void raft_log_install_snapshot(int64 shard_id, uint64 index, uint64 term, void *data, int size);
extern void *raft_log_get_snapshot(int64 shard_id, uint64 *index, uint64 *term);
```

#### `src/consensus/raft_transport.h` - Network Transport
```c
/*
 * Raft message transport layer
 * Uses PostgreSQL's libpq for node-to-node communication
 */

typedef struct RaftTransport
{
    int32           self_id;            /* This node's ID */
    HTAB           *connections;        /* Node ID -> PGconn* */
    int             listen_fd;          /* Listening socket for Raft RPCs */
    int             listen_port;        /* Raft communication port */
} RaftTransport;

/*
 * Serialized Raft message envelope
 */
typedef struct RaftMessageEnvelope
{
    RaftMessageType type;
    int64           shard_id;           /* Target Raft group */
    int32           from_node;          /* Sender node ID */
    int32           to_node;            /* Receiver node ID */
    int32           payload_size;       /* Size of message payload */
    /* payload follows */
} RaftMessageEnvelope;

/* Transport initialization */
extern RaftTransport *raft_transport_init(int32 self_id, int port);
extern void raft_transport_shutdown(RaftTransport *transport);

/* Sending messages */
extern bool raft_transport_send(RaftTransport *transport, int32 target_node,
                                 RaftMessageEnvelope *envelope);
extern void raft_transport_broadcast(RaftTransport *transport,
                                      RaftMessageEnvelope *envelope);

/* Receiving messages */
extern RaftMessageEnvelope *raft_transport_receive(RaftTransport *transport,
                                                    int timeout_ms);
extern void raft_transport_process_incoming(RaftTransport *transport);

/* Connection management */
extern void raft_transport_connect(RaftTransport *transport, int32 node_id,
                                    const char *hostname, int port);
extern void raft_transport_disconnect(RaftTransport *transport, int32 node_id);
```

#### `src/consensus/multi_raft.h` - Multi-Raft Coordination
```c
/*
 * Multi-Raft manager - coordinates multiple Raft groups (one per shard)
 */

typedef struct MultiRaftManager
{
    HTAB               *groups;             /* shard_id -> RaftGroup* */
    RaftTransport      *transport;          /* Shared transport layer */
    int32               self_id;            /* This node's ID */

    /* Background worker state */
    bool                running;
    pthread_t           ticker_thread;      /* Handles Raft ticks */
    pthread_t           receiver_thread;    /* Handles incoming messages */

    /* Statistics */
    uint64              elections_started;
    uint64              elections_won;
    uint64              entries_replicated;
    uint64              snapshots_sent;
} MultiRaftManager;

/* Manager lifecycle */
extern MultiRaftManager *multi_raft_init(int32 self_id, int port);
extern void multi_raft_shutdown(MultiRaftManager *mgr);
extern void multi_raft_start(MultiRaftManager *mgr);
extern void multi_raft_stop(MultiRaftManager *mgr);

/* Group management */
extern RaftGroup *multi_raft_get_group(MultiRaftManager *mgr, int64 shard_id);
extern RaftGroup *multi_raft_create_group(MultiRaftManager *mgr, int64 shard_id,
                                           int32 *members, int32 count);
extern void multi_raft_remove_group(MultiRaftManager *mgr, int64 shard_id);

/* Proposal interface (used by distributed executor) */
extern bool multi_raft_propose(MultiRaftManager *mgr, int64 shard_id,
                                void *command, int size, RaftCommandType type);

/* Leader detection */
extern int32 multi_raft_get_leader(MultiRaftManager *mgr, int64 shard_id);
extern bool multi_raft_is_leader(MultiRaftManager *mgr, int64 shard_id);

/* Membership changes */
extern bool multi_raft_add_member(MultiRaftManager *mgr, int64 shard_id, int32 node_id);
extern bool multi_raft_remove_member(MultiRaftManager *mgr, int64 shard_id, int32 node_id);
extern bool multi_raft_add_learner(MultiRaftManager *mgr, int64 shard_id, int32 node_id);
extern bool multi_raft_promote_learner(MultiRaftManager *mgr, int64 shard_id, int32 node_id);
```

### 1.4 Integration with Existing Components

#### Modifications to `src/executor/distributed_executor.c`
```c
/*
 * New functions for Raft-integrated distributed transactions
 */

/*
 * orochi_execute_with_consensus - Execute command through Raft
 *
 * For write operations, this ensures the command is replicated
 * to a quorum before returning success.
 */
bool
orochi_execute_with_consensus(DistributedExecutorState *state, int64 shard_id,
                               const char *command)
{
    MultiRaftManager *mgr = get_multi_raft_manager();
    RaftGroup *group = multi_raft_get_group(mgr, shard_id);

    if (!multi_raft_is_leader(mgr, shard_id))
    {
        /* Redirect to leader */
        int32 leader = multi_raft_get_leader(mgr, shard_id);
        if (leader >= 0)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_CONNECTION_FAILURE),
                     errmsg("not the leader for shard %ld, redirect to node %d",
                            shard_id, leader)));
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_CONNECTION_FAILURE),
                     errmsg("no leader available for shard %ld", shard_id)));
        }
    }

    /* Propose command through Raft */
    return multi_raft_propose(mgr, shard_id, (void *)command,
                               strlen(command) + 1, RAFT_CMD_SQL);
}

/*
 * orochi_wait_for_commit - Wait for Raft commit
 *
 * Returns when the command has been committed (replicated to quorum)
 * or timeout occurs.
 */
bool
orochi_wait_for_commit(int64 shard_id, uint64 log_index, int timeout_ms)
{
    MultiRaftManager *mgr = get_multi_raft_manager();
    RaftGroup *group = multi_raft_get_group(mgr, shard_id);
    TimestampTz deadline = GetCurrentTimestamp() + timeout_ms * 1000;

    while (GetCurrentTimestamp() < deadline)
    {
        if (group->persistent.commit_index >= log_index)
            return true;

        pg_usleep(1000);  /* 1ms */
    }

    return false;
}
```

### 1.5 SQL Interface Extensions

```sql
-- Add to sql/orochi--1.1.sql (upgrade script)

-- Raft cluster management functions
CREATE FUNCTION set_replication_factor(
    relation regclass,
    replication_factor integer DEFAULT 3
) RETURNS void AS $$
BEGIN
    PERFORM orochi._set_replication_factor(relation, replication_factor);
    RAISE NOTICE 'Set replication factor for % to %', relation, replication_factor;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._set_replication_factor(table_oid oid, factor integer)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_set_replication_factor_sql'
    LANGUAGE C STRICT;

-- Enable follower reads for a table
CREATE FUNCTION enable_follower_reads(
    relation regclass,
    max_staleness interval DEFAULT INTERVAL '10 seconds'
) RETURNS void AS $$
BEGIN
    PERFORM orochi._enable_follower_reads(relation, max_staleness);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._enable_follower_reads(table_oid oid, max_staleness interval)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_enable_follower_reads_sql'
    LANGUAGE C STRICT;

-- Raft group status view
CREATE VIEW orochi.raft_groups AS
SELECT
    g.shard_id,
    t.schema_name || '.' || t.table_name as table_name,
    g.state,
    g.current_term,
    g.commit_index,
    g.last_applied,
    g.leader_id,
    g.member_count,
    g.last_heartbeat
FROM orochi.orochi_raft_groups g
JOIN orochi.orochi_shards s ON g.shard_id = s.shard_id
JOIN orochi.orochi_tables t ON s.table_oid = t.table_oid;

-- Raft group catalog table
CREATE TABLE IF NOT EXISTS orochi.orochi_raft_groups (
    shard_id BIGINT PRIMARY KEY,
    state INTEGER NOT NULL DEFAULT 0,
    current_term BIGINT NOT NULL DEFAULT 0,
    voted_for INTEGER,
    commit_index BIGINT NOT NULL DEFAULT 0,
    last_applied BIGINT NOT NULL DEFAULT 0,
    leader_id INTEGER,
    member_count INTEGER NOT NULL DEFAULT 1,
    last_heartbeat TIMESTAMPTZ,
    FOREIGN KEY (shard_id) REFERENCES orochi.orochi_shards(shard_id) ON DELETE CASCADE
);
```

### 1.6 Testing Strategy

```
tests/
  consensus/
    test_raft_election.c        # Leader election tests
    test_raft_replication.c     # Log replication tests
    test_raft_membership.c      # Membership change tests
    test_raft_failover.c        # Failover scenarios
    test_raft_split_brain.c     # Network partition handling
    test_multi_raft.c           # Multi-Raft coordination
```

---

## 2. Vectorized Query Execution

### 2.1 Architecture Overview

```
                    Query Executor
                          |
                          v
            +-------------+-------------+
            |   Vectorized Executor     |
            |  (Batch-oriented)         |
            +-------------+-------------+
                          |
          +---------------+---------------+
          |               |               |
          v               v               v
    +-----------+   +-----------+   +-----------+
    | Filter    |   | Project   |   | Aggregate |
    | Operator  |   | Operator  |   | Operator  |
    | (SIMD)    |   | (SIMD)    |   | (SIMD)    |
    +-----------+   +-----------+   +-----------+
          |               |               |
          v               v               v
    +-----------+   +-----------+   +-----------+
    | Column    |   | Column    |   | Hash      |
    | Batch     |   | Batch     |   | Table     |
    +-----------+   +-----------+   +-----------+
```

### 2.2 Design Decisions

**ADR-003: Vectorization Scope**
- **Decision**: Implement volcano-style vectorized execution with SIMD primitives
- **Rationale**:
  - Compatible with PostgreSQL's executor model
  - Gradual adoption path (can mix vectorized and row-at-a-time)
  - Similar approach to DuckDB and Velox

**ADR-004: Batch Size**
- **Decision**: Default batch size of 1024 vectors, configurable
- **Rationale**: Fits in L1 cache on modern CPUs, good SIMD utilization

### 2.3 File-by-File Implementation

#### `src/vectorized/vector_batch.h` - Core Batch Types
```c
/*
 * Vector batch - columnar representation of tuple batch
 */

#define VECTOR_BATCH_SIZE       1024    /* Default vectors per batch */
#define VECTOR_NULL_DENSITY     0.01    /* Assume 1% null for sizing */

/*
 * Selection vector - tracks which rows are active
 * After a filter, only selected indices contain valid data
 */
typedef struct SelectionVector
{
    int32          *indices;            /* Indices of selected rows */
    int32           count;              /* Number of selected rows */
    int32           capacity;           /* Capacity (usually VECTOR_BATCH_SIZE) */
} SelectionVector;

/*
 * Single column vector within a batch
 */
typedef struct ColumnVector
{
    Oid             type_oid;           /* PostgreSQL type */
    int16           typlen;             /* Type length (-1 for varlena) */
    bool            typbyval;           /* Pass by value? */

    /* Data storage */
    void           *data;               /* Column data (aligned) */
    uint64         *nullmask;           /* Bit vector for nulls (1 = null) */
    int32           count;              /* Number of values */
    int32           capacity;           /* Allocated capacity */

    /* For variable-length types */
    int32          *offsets;            /* String/varlena offsets */
    char           *vardata;            /* Variable data buffer */
    int32           vardata_size;       /* Size of vardata buffer */

    /* Dictionary encoding (optional) */
    bool            is_dictionary;
    void           *dictionary;         /* Unique values */
    int32          *dict_indices;       /* Indices into dictionary */
    int32           dict_size;          /* Dictionary size */

    /* Statistics */
    bool            has_stats;
    Datum           min_value;
    Datum           max_value;
    int32           null_count;
} ColumnVector;

/*
 * Vector batch - represents multiple tuples in columnar format
 */
typedef struct VectorBatch
{
    int32           column_count;       /* Number of columns */
    ColumnVector  **columns;            /* Column vectors */
    SelectionVector *selection;         /* Active rows (NULL = all active) */
    int32           row_count;          /* Total rows in batch */
    MemoryContext   batch_context;      /* Memory context for allocations */
} VectorBatch;

/* Batch lifecycle */
extern VectorBatch *vector_batch_create(TupleDesc tupdesc, int capacity);
extern void vector_batch_destroy(VectorBatch *batch);
extern void vector_batch_reset(VectorBatch *batch);
extern VectorBatch *vector_batch_copy(VectorBatch *src);

/* Column vector operations */
extern ColumnVector *column_vector_create(Oid type_oid, int capacity);
extern void column_vector_destroy(ColumnVector *vec);
extern void column_vector_append(ColumnVector *vec, Datum value, bool isnull);
extern Datum column_vector_get(ColumnVector *vec, int idx, bool *isnull);

/* Selection vector */
extern SelectionVector *selection_vector_create(int capacity);
extern void selection_vector_set_all(SelectionVector *sel, int count);
extern void selection_vector_intersect(SelectionVector *a, SelectionVector *b);

/* Conversion */
extern void vector_batch_from_tuplestore(VectorBatch *batch, Tuplestorestate *store, int count);
extern void vector_batch_to_slots(VectorBatch *batch, TupleTableSlot **slots, int *count);
```

#### `src/vectorized/vector_primitives.h` - SIMD Primitives
```c
/*
 * SIMD-accelerated primitive operations
 * These operate on raw column data with selection vectors
 */

/* Comparison operations - return selection vector of matching rows */

/*
 * vector_filter_eq_int32 - Filter int32 column for equality
 *
 * @vec: Column vector of int32 values
 * @sel: Input selection (NULL = all rows)
 * @value: Value to compare against
 * @result: Output selection vector (matching rows)
 */
extern void vector_filter_eq_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 value, SelectionVector *result);
extern void vector_filter_eq_int64(ColumnVector *vec, SelectionVector *sel,
                                    int64 value, SelectionVector *result);
extern void vector_filter_eq_float64(ColumnVector *vec, SelectionVector *sel,
                                      double value, SelectionVector *result);

/* Range comparisons */
extern void vector_filter_lt_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 value, SelectionVector *result);
extern void vector_filter_le_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 value, SelectionVector *result);
extern void vector_filter_gt_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 value, SelectionVector *result);
extern void vector_filter_ge_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 value, SelectionVector *result);

/* BETWEEN operation (compound) */
extern void vector_filter_between_int32(ColumnVector *vec, SelectionVector *sel,
                                         int32 low, int32 high, SelectionVector *result);

/* IN list operation */
extern void vector_filter_in_int32(ColumnVector *vec, SelectionVector *sel,
                                    int32 *values, int count, SelectionVector *result);

/* Null handling */
extern void vector_filter_is_null(ColumnVector *vec, SelectionVector *sel,
                                   SelectionVector *result);
extern void vector_filter_is_not_null(ColumnVector *vec, SelectionVector *sel,
                                       SelectionVector *result);

/* String operations (with SIMD where possible) */
extern void vector_filter_eq_string(ColumnVector *vec, SelectionVector *sel,
                                     const char *value, int len, SelectionVector *result);
extern void vector_filter_like_prefix(ColumnVector *vec, SelectionVector *sel,
                                       const char *prefix, int len, SelectionVector *result);

/* Aggregation primitives */

/*
 * vector_sum_int64 - Sum int64 column with selection
 */
extern int64 vector_sum_int64(ColumnVector *vec, SelectionVector *sel);
extern double vector_sum_float64(ColumnVector *vec, SelectionVector *sel);

/*
 * vector_count - Count non-null values
 */
extern int64 vector_count(ColumnVector *vec, SelectionVector *sel);
extern int64 vector_count_all(SelectionVector *sel);  /* Count selected rows */

/*
 * vector_min/max - Find minimum/maximum value
 */
extern int64 vector_min_int64(ColumnVector *vec, SelectionVector *sel, bool *found);
extern int64 vector_max_int64(ColumnVector *vec, SelectionVector *sel, bool *found);
extern double vector_min_float64(ColumnVector *vec, SelectionVector *sel, bool *found);
extern double vector_max_float64(ColumnVector *vec, SelectionVector *sel, bool *found);

/* Arithmetic operations (column op column) */
extern void vector_add_int64(ColumnVector *a, ColumnVector *b, ColumnVector *result,
                              SelectionVector *sel);
extern void vector_sub_int64(ColumnVector *a, ColumnVector *b, ColumnVector *result,
                              SelectionVector *sel);
extern void vector_mul_int64(ColumnVector *a, ColumnVector *b, ColumnVector *result,
                              SelectionVector *sel);
extern void vector_div_float64(ColumnVector *a, ColumnVector *b, ColumnVector *result,
                                SelectionVector *sel);

/* Scalar operations (column op scalar) */
extern void vector_add_scalar_int64(ColumnVector *vec, int64 scalar, ColumnVector *result,
                                     SelectionVector *sel);
extern void vector_mul_scalar_float64(ColumnVector *vec, double scalar, ColumnVector *result,
                                       SelectionVector *sel);

/* Hash operations (for hash joins and grouping) */
extern void vector_hash_int64(ColumnVector *vec, SelectionVector *sel,
                               uint64 *hashes);
extern void vector_hash_string(ColumnVector *vec, SelectionVector *sel,
                                uint64 *hashes);
extern void vector_hash_combine(uint64 *hashes1, uint64 *hashes2, int count);
```

#### `src/vectorized/vector_primitives_simd.c` - SIMD Implementations
```c
/*
 * AVX2/AVX-512 implementations of vector primitives
 */

#ifdef __AVX2__
#include <immintrin.h>

void
vector_filter_eq_int32_avx2(const int32 *data, const uint64 *nullmask,
                             int32 value, int count,
                             int32 *result_indices, int32 *result_count)
{
    __m256i val_vec = _mm256_set1_epi32(value);
    int32 *out = result_indices;
    int32 idx = 0;

    /* Process 8 values at a time */
    for (int i = 0; i < count - 7; i += 8)
    {
        /* Load 8 int32 values */
        __m256i data_vec = _mm256_loadu_si256((__m256i*)(data + i));

        /* Compare for equality */
        __m256i cmp = _mm256_cmpeq_epi32(data_vec, val_vec);

        /* Extract comparison mask */
        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));

        /* Handle nulls - clear bits where nullmask is set */
        if (nullmask)
        {
            uint8_t null_byte = (nullmask[i / 64] >> (i % 64)) & 0xFF;
            mask &= ~null_byte;
        }

        /* Store matching indices */
        while (mask)
        {
            int bit = __builtin_ctz(mask);
            *out++ = i + bit;
            mask &= mask - 1;  /* Clear lowest bit */
        }
    }

    /* Handle remaining elements with scalar code */
    for (int i = (count / 8) * 8; i < count; i++)
    {
        bool is_null = nullmask && (nullmask[i / 64] & (1ULL << (i % 64)));
        if (!is_null && data[i] == value)
            *out++ = i;
    }

    *result_count = out - result_indices;
}

int64
vector_sum_int64_avx2(const int64 *data, const uint64 *nullmask,
                       const int32 *selection, int sel_count)
{
    __m256i sum_vec = _mm256_setzero_si256();
    int64 total = 0;

    if (selection)
    {
        /* Sum selected elements only */
        for (int i = 0; i < sel_count - 3; i += 4)
        {
            /* Gather 4 values at selected indices */
            __m128i indices = _mm_loadu_si128((__m128i*)(selection + i));
            __m256i vals = _mm256_i32gather_epi64(data, indices, 8);

            /* Check nulls for selected indices */
            bool any_null = false;
            for (int j = 0; j < 4; j++)
            {
                int idx = selection[i + j];
                if (nullmask && (nullmask[idx / 64] & (1ULL << (idx % 64))))
                    any_null = true;
            }

            if (!any_null)
                sum_vec = _mm256_add_epi64(sum_vec, vals);
            else
            {
                /* Handle nulls with scalar fallback */
                for (int j = 0; j < 4; j++)
                {
                    int idx = selection[i + j];
                    bool is_null = nullmask && (nullmask[idx / 64] & (1ULL << (idx % 64)));
                    if (!is_null)
                        total += data[idx];
                }
            }
        }
    }
    else
    {
        /* Sum all elements */
        int count = sel_count;  /* In this case, sel_count is total count */
        for (int i = 0; i < count - 3; i += 4)
        {
            __m256i vals = _mm256_loadu_si256((__m256i*)(data + i));
            sum_vec = _mm256_add_epi64(sum_vec, vals);
        }
    }

    /* Horizontal sum of vector */
    int64 sums[4];
    _mm256_storeu_si256((__m256i*)sums, sum_vec);
    total += sums[0] + sums[1] + sums[2] + sums[3];

    return total;
}

#endif /* __AVX2__ */

#ifdef __AVX512F__
/* AVX-512 implementations for even better performance */

void
vector_filter_eq_int32_avx512(const int32 *data, const uint64 *nullmask,
                               int32 value, int count,
                               int32 *result_indices, int32 *result_count)
{
    __m512i val_vec = _mm512_set1_epi32(value);
    int32 *out = result_indices;

    /* Process 16 values at a time */
    for (int i = 0; i < count - 15; i += 16)
    {
        __m512i data_vec = _mm512_loadu_si512(data + i);
        __mmask16 cmp = _mm512_cmpeq_epi32_mask(data_vec, val_vec);

        /* Handle nulls */
        if (nullmask)
        {
            uint16_t null_bits = (nullmask[i / 64] >> (i % 64)) & 0xFFFF;
            cmp &= ~null_bits;
        }

        /* Compress store matching indices */
        __m512i indices = _mm512_add_epi32(
            _mm512_set1_epi32(i),
            _mm512_setr_epi32(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15)
        );
        _mm512_mask_compressstoreu_epi32(out, cmp, indices);
        out += __builtin_popcount(cmp);
    }

    *result_count = out - result_indices;
}

#endif /* __AVX512F__ */
```

#### `src/vectorized/vector_executor.h` - Vectorized Executor Nodes
```c
/*
 * Vectorized executor nodes - replacements for standard executor nodes
 */

/*
 * VectorScanState - Base for vectorized scans
 */
typedef struct VectorScanState
{
    ScanState       ss;                 /* Standard scan state */
    VectorBatch    *batch;              /* Current output batch */
    int             batch_position;     /* Position within batch */
    bool            is_vectorized;      /* True if actually vectorized */
} VectorScanState;

/*
 * VectorSeqScanState - Vectorized sequential scan
 */
typedef struct VectorSeqScanState
{
    VectorScanState base;
    HeapScanDesc    scan;               /* Heap scan descriptor */
    int             prefetch_pages;     /* Pages to prefetch */
} VectorSeqScanState;

/*
 * VectorColumnarScanState - Vectorized columnar scan
 */
typedef struct VectorColumnarScanState
{
    VectorScanState base;
    ColumnarReadState *columnar_state;  /* Columnar reader */
    Bitmapset      *columns_needed;     /* Required columns */
    List           *qual_list;          /* Pushed-down predicates */
} VectorColumnarScanState;

/*
 * VectorFilterState - Vectorized filter (WHERE clause)
 */
typedef struct VectorFilterState
{
    PlanState       ps;
    PlanState      *subplan;            /* Child plan */
    List           *qual_ops;           /* Compiled qual operations */
    VectorBatch    *input_batch;        /* Input from child */
    SelectionVector *filter_result;     /* Filter output selection */
} VectorFilterState;

/*
 * VectorAggState - Vectorized aggregation
 */
typedef struct VectorAggState
{
    AggState        as;                 /* Standard agg state */
    VectorBatch    *input_batch;        /* Input batch */

    /* Hash aggregation state */
    void           *hash_table;         /* Hash table for groups */
    int             num_groups;         /* Number of groups */

    /* Per-aggregate state */
    struct VectorAggInfo {
        int         agg_type;           /* Type of aggregate */
        int         input_col;          /* Input column index */
        int64       sum;                /* Running sum (for SUM/AVG) */
        int64       count;              /* Running count */
        Datum       min_val;            /* Running min */
        Datum       max_val;            /* Running max */
        bool        has_value;          /* Has seen non-null value */
    } *agg_info;
    int             num_aggs;           /* Number of aggregates */
} VectorAggState;

/*
 * VectorHashJoinState - Vectorized hash join
 */
typedef struct VectorHashJoinState
{
    HashJoinState   hjs;                /* Standard hash join state */

    /* Build side */
    void           *hash_table;         /* Hash table */
    VectorBatch    *build_batch;        /* Current build batch */

    /* Probe side */
    VectorBatch    *probe_batch;        /* Current probe batch */
    uint64         *probe_hashes;       /* Hash values for probe */
    int            *probe_matches;      /* Match indices */
    int             probe_position;     /* Position in probe batch */
} VectorHashJoinState;

/* Executor functions */
extern TupleTableSlot *ExecVectorSeqScan(PlanState *pstate);
extern TupleTableSlot *ExecVectorColumnarScan(PlanState *pstate);
extern TupleTableSlot *ExecVectorFilter(PlanState *pstate);
extern TupleTableSlot *ExecVectorAgg(PlanState *pstate);
extern TupleTableSlot *ExecVectorHashJoin(PlanState *pstate);

/* Batch-returning functions (internal) */
extern VectorBatch *ExecVectorScanBatch(VectorScanState *node);
extern VectorBatch *ExecVectorFilterBatch(VectorFilterState *node);
extern VectorBatch *ExecVectorAggBatch(VectorAggState *node);
```

### 2.4 Integration with Columnar Storage

```c
/* Modifications to src/storage/columnar.c */

/*
 * columnar_read_next_vector_batch - Read batch of rows in columnar format
 *
 * This is the primary interface for vectorized execution over columnar tables.
 * Returns data already in columnar (VectorBatch) format, avoiding row-by-row
 * materialization.
 */
VectorBatch *
columnar_read_next_vector_batch(ColumnarReadState *state, int max_rows)
{
    VectorBatch *batch;
    int rows_read = 0;

    if (state->is_complete)
        return NULL;

    batch = vector_batch_create(state->tupdesc, max_rows);

    while (rows_read < max_rows && !state->is_complete)
    {
        /* Try to read from current chunk group */
        if (state->current_chunk_group != NULL)
        {
            int chunk_rows = columnar_read_chunk_to_batch(
                state, batch, max_rows - rows_read);
            rows_read += chunk_rows;

            if (chunk_rows == 0)
            {
                /* Move to next chunk group */
                state->current_chunk_group_index++;
                state->current_row_in_chunk = 0;
                columnar_load_next_chunk_group(state);
            }
        }
        else
        {
            /* Move to next stripe */
            state->current_stripe_index++;
            if (!columnar_load_next_stripe(state))
            {
                state->is_complete = true;
                break;
            }
        }
    }

    batch->row_count = rows_read;
    return batch;
}

/*
 * columnar_read_chunk_to_batch - Read column chunk directly into batch
 *
 * Zero-copy path: decompresses directly into VectorBatch column buffers
 */
static int
columnar_read_chunk_to_batch(ColumnarReadState *state, VectorBatch *batch,
                              int max_rows)
{
    ColumnarChunkGroup *group = state->current_chunk_group;
    int start_row = state->current_row_in_chunk;
    int rows_to_read = Min(max_rows, group->row_count - start_row);

    /* For each column needed */
    int col_idx = -1;
    while ((col_idx = bms_next_member(state->columns_needed, col_idx)) >= 0)
    {
        ColumnarColumnChunk *chunk = group->columns[col_idx];
        ColumnVector *vec = batch->columns[col_idx];

        /* Decompress directly into column vector buffer */
        columnar_decompress_to_vector(chunk, vec, start_row, rows_to_read);
    }

    state->current_row_in_chunk += rows_to_read;
    return rows_to_read;
}
```

### 2.5 SQL Interface Extensions

```sql
-- GUC parameters for vectorization
-- Add to postgresql.conf

-- Enable/disable vectorized execution
-- orochi.enable_vectorized_execution = on

-- Batch size for vectorized operations
-- orochi.vector_batch_size = 1024

-- Minimum table size to use vectorization (bytes)
-- orochi.vectorized_min_table_size = 1000000

-- Force vectorization even for small tables (for testing)
-- orochi.force_vectorization = off

-- SQL functions to control vectorization
CREATE FUNCTION set_vectorization(
    relation regclass,
    enabled boolean DEFAULT true
) RETURNS void AS $$
BEGIN
    UPDATE orochi.orochi_tables
    SET vectorization_enabled = enabled
    WHERE table_oid = relation;
END;
$$ LANGUAGE plpgsql;

-- View vectorization stats
CREATE VIEW orochi.vectorization_stats AS
SELECT
    t.schema_name || '.' || t.table_name as table_name,
    s.batches_processed,
    s.rows_processed,
    s.filter_selectivity,
    s.simd_operations,
    s.avg_batch_time_us
FROM orochi.orochi_vectorization_stats s
JOIN orochi.orochi_tables t ON s.table_oid = t.table_oid;
```

---

## 3. Real-time Pipelines

### 3.1 Architecture Overview

```
External Sources                    Orochi Pipeline System
+-------------+                   +-------------------------+
|   Kafka     |  ---- events ---> | Pipeline Consumer       |
|   Broker    |                   | (Background Worker)     |
+-------------+                   +------------+------------+
                                               |
+-------------+                                v
|    S3       |  ---- files ----> +-----------+-----------+
|   Bucket    |                   | Transform Engine      |
+-------------+                   | (SQL expressions)     |
                                  +-----------+-----------+
                                               |
                                               v
                                  +-----------+-----------+
                                  | Batch Insert          |
                                  | (into Hypertable)     |
                                  +-----------------------+
```

### 3.2 Design Decisions

**ADR-005: Pipeline Architecture**
- **Decision**: Background worker model with configurable parallelism
- **Rationale**:
  - Non-blocking for regular queries
  - Scalable by adding workers
  - Crash isolation from main PostgreSQL process

**ADR-006: Exactly-Once Semantics**
- **Decision**: Transactional offsets stored in PostgreSQL
- **Rationale**:
  - Atomic commit with data insertion
  - Simple recovery model
  - No external coordination needed

### 3.3 File-by-File Implementation

#### `src/pipeline/pipeline.h` - Core Pipeline Types
```c
/*
 * Pipeline - streaming data ingestion system
 */

typedef enum PipelineSourceType
{
    PIPELINE_SOURCE_KAFKA = 1,
    PIPELINE_SOURCE_S3,
    PIPELINE_SOURCE_HTTP,
    PIPELINE_SOURCE_FILE
} PipelineSourceType;

typedef enum PipelineFormat
{
    PIPELINE_FORMAT_JSON = 1,
    PIPELINE_FORMAT_CSV,
    PIPELINE_FORMAT_AVRO,
    PIPELINE_FORMAT_PARQUET,
    PIPELINE_FORMAT_MSGPACK
} PipelineFormat;

typedef enum PipelineState
{
    PIPELINE_STATE_CREATED = 0,
    PIPELINE_STATE_RUNNING,
    PIPELINE_STATE_PAUSED,
    PIPELINE_STATE_ERROR,
    PIPELINE_STATE_STOPPED
} PipelineState;

/*
 * Pipeline definition
 */
typedef struct Pipeline
{
    int64               pipeline_id;
    char               *name;
    PipelineSourceType  source_type;
    Oid                 target_table;
    PipelineState       state;

    /* Source configuration */
    char               *source_uri;         /* Kafka: broker:port/topic, S3: bucket/prefix */
    PipelineFormat      format;
    char               *schema_hint;        /* Optional schema for parsing */

    /* Processing options */
    int32               batch_size;         /* Rows per batch */
    int32               batch_timeout_ms;   /* Max wait for batch */
    int32               parallelism;        /* Consumer threads */
    bool                exactly_once;       /* Exactly-once semantics */

    /* Transform */
    char               *transform_sql;      /* SQL expression for transform */
    List               *column_mappings;    /* Source -> Target column mappings */

    /* Error handling */
    char               *error_table;        /* Table for failed rows */
    int32               max_errors;         /* Max errors before pause */

    /* Statistics */
    int64               rows_ingested;
    int64               bytes_ingested;
    int64               errors;
    TimestampTz         last_activity;
} Pipeline;

/*
 * Kafka-specific configuration
 */
typedef struct KafkaConfig
{
    char               *broker_list;        /* Comma-separated broker:port */
    char               *topic;              /* Topic name */
    char               *consumer_group;     /* Consumer group ID */
    int32               partition;          /* Specific partition (-1 for all) */
    int64               start_offset;       /* Starting offset (-1 for latest) */
    char               *security_protocol;  /* PLAINTEXT, SSL, SASL_PLAINTEXT, SASL_SSL */
    char               *sasl_mechanism;     /* PLAIN, SCRAM-SHA-256, etc. */
    char               *sasl_username;
    char               *sasl_password;
} KafkaConfig;

/*
 * S3-specific configuration
 */
typedef struct S3Config
{
    char               *bucket;
    char               *prefix;             /* Object prefix (folder) */
    char               *region;
    char               *access_key_id;
    char               *secret_access_key;
    char               *endpoint_url;       /* For S3-compatible stores */
    int32               poll_interval_sec;  /* How often to check for new files */
    char               *pattern;            /* Glob pattern for files */
    bool                delete_after;       /* Delete files after processing */
} S3Config;

/*
 * Pipeline offset tracking (for exactly-once)
 */
typedef struct PipelineOffset
{
    int64               pipeline_id;
    int32               partition;          /* Kafka partition or S3 shard */
    int64               offset;             /* Kafka offset or S3 file position */
    char               *file_path;          /* S3: current file being processed */
    TimestampTz         updated_at;
} PipelineOffset;

/* Pipeline lifecycle */
extern Pipeline *pipeline_create(const char *name, const char *source_uri,
                                  Oid target_table, PipelineFormat format);
extern void pipeline_start(int64 pipeline_id);
extern void pipeline_pause(int64 pipeline_id);
extern void pipeline_stop(int64 pipeline_id);
extern void pipeline_drop(int64 pipeline_id);
extern Pipeline *pipeline_get(int64 pipeline_id);
extern List *pipeline_list_all(void);

/* Pipeline configuration */
extern void pipeline_set_transform(int64 pipeline_id, const char *transform_sql);
extern void pipeline_set_parallelism(int64 pipeline_id, int parallelism);
extern void pipeline_set_batch_size(int64 pipeline_id, int batch_size);
extern void pipeline_enable_exactly_once(int64 pipeline_id, bool enable);

/* Offset management */
extern void pipeline_commit_offset(int64 pipeline_id, int32 partition, int64 offset);
extern PipelineOffset *pipeline_get_offset(int64 pipeline_id, int32 partition);
extern void pipeline_reset_offset(int64 pipeline_id, int64 offset);
```

#### `src/pipeline/kafka_consumer.h` - Kafka Integration
```c
/*
 * Kafka consumer using librdkafka
 */

#include <librdkafka/rdkafka.h>

typedef struct KafkaConsumer
{
    rd_kafka_t         *rk;                 /* Kafka handle */
    rd_kafka_topic_t   *rkt;                /* Topic handle */
    KafkaConfig        *config;
    int64               pipeline_id;

    /* Batch accumulator */
    List               *batch;              /* Current batch of messages */
    int                 batch_count;
    TimestampTz         batch_start_time;

    /* Statistics */
    int64               messages_consumed;
    int64               bytes_consumed;
    int64               errors;
} KafkaConsumer;

/* Consumer lifecycle */
extern KafkaConsumer *kafka_consumer_create(int64 pipeline_id, KafkaConfig *config);
extern void kafka_consumer_destroy(KafkaConsumer *consumer);
extern void kafka_consumer_start(KafkaConsumer *consumer);
extern void kafka_consumer_stop(KafkaConsumer *consumer);

/* Message consumption */
extern rd_kafka_message_t *kafka_consumer_poll(KafkaConsumer *consumer, int timeout_ms);
extern List *kafka_consumer_poll_batch(KafkaConsumer *consumer, int max_messages, int timeout_ms);

/* Offset management */
extern void kafka_consumer_commit(KafkaConsumer *consumer);
extern void kafka_consumer_seek(KafkaConsumer *consumer, int64 offset);
extern int64 kafka_consumer_get_offset(KafkaConsumer *consumer, int32 partition);

/* Error handling */
extern const char *kafka_consumer_last_error(KafkaConsumer *consumer);
```

#### `src/pipeline/s3_watcher.h` - S3 Integration
```c
/*
 * S3 file watcher using AWS SDK or libcurl
 */

typedef struct S3Object
{
    char               *key;                /* Object key (path) */
    int64               size;               /* Object size in bytes */
    TimestampTz         last_modified;      /* Last modification time */
    char               *etag;               /* Entity tag */
} S3Object;

typedef struct S3Watcher
{
    S3Config           *config;
    int64               pipeline_id;

    /* State */
    char               *last_processed_key; /* Last fully processed object */
    List               *pending_objects;    /* Objects to process */

    /* Current file state */
    char               *current_file;       /* Currently processing */
    int64               current_offset;     /* Byte offset in file */
    void               *file_handle;        /* File reader handle */

    /* Statistics */
    int64               files_processed;
    int64               bytes_processed;
} S3Watcher;

/* Watcher lifecycle */
extern S3Watcher *s3_watcher_create(int64 pipeline_id, S3Config *config);
extern void s3_watcher_destroy(S3Watcher *watcher);

/* File discovery */
extern List *s3_watcher_list_new_files(S3Watcher *watcher);
extern S3Object *s3_watcher_get_next_file(S3Watcher *watcher);

/* File reading */
extern void *s3_watcher_open_file(S3Watcher *watcher, const char *key);
extern int s3_watcher_read_batch(S3Watcher *watcher, char *buffer, int size);
extern bool s3_watcher_eof(S3Watcher *watcher);
extern void s3_watcher_close_file(S3Watcher *watcher);

/* File management */
extern void s3_watcher_mark_processed(S3Watcher *watcher, const char *key);
extern void s3_watcher_delete_file(S3Watcher *watcher, const char *key);
```

#### `src/pipeline/pipeline_worker.c` - Background Worker
```c
/*
 * Pipeline background worker
 */

#include "postgres.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "access/xact.h"

/*
 * Pipeline worker main loop
 */
void
pipeline_worker_main(Datum main_arg)
{
    int64 pipeline_id = DatumGetInt64(main_arg);
    Pipeline *pipeline;

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    /* Load pipeline configuration */
    StartTransactionCommand();
    pipeline = pipeline_get(pipeline_id);
    CommitTransactionCommand();

    if (pipeline == NULL)
    {
        elog(ERROR, "Pipeline %ld not found", pipeline_id);
        return;
    }

    /* Main processing loop */
    while (!got_sigterm)
    {
        int rc;
        List *batch = NIL;

        /* Wait for events with timeout */
        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       pipeline->batch_timeout_ms, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_TIMEOUT)
        {
            /* Check for new data */
            switch (pipeline->source_type)
            {
                case PIPELINE_SOURCE_KAFKA:
                    batch = pipeline_consume_kafka(pipeline);
                    break;
                case PIPELINE_SOURCE_S3:
                    batch = pipeline_consume_s3(pipeline);
                    break;
                default:
                    break;
            }

            if (list_length(batch) > 0)
            {
                pipeline_process_batch(pipeline, batch);
            }
        }

        /* Check for control signals */
        if (pipeline_check_control(pipeline_id))
        {
            /* Reload configuration or stop */
            pipeline = pipeline_get(pipeline_id);
            if (pipeline->state == PIPELINE_STATE_STOPPED)
                break;
        }
    }

    proc_exit(0);
}

/*
 * Process a batch of records
 */
static void
pipeline_process_batch(Pipeline *pipeline, List *batch)
{
    ListCell *lc;
    int success_count = 0;
    int error_count = 0;

    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    PG_TRY();
    {
        /* Parse and transform records */
        List *tuples = NIL;

        foreach(lc, batch)
        {
            char *record = (char *) lfirst(lc);
            HeapTuple tuple = NULL;

            /* Parse based on format */
            switch (pipeline->format)
            {
                case PIPELINE_FORMAT_JSON:
                    tuple = pipeline_parse_json(pipeline, record);
                    break;
                case PIPELINE_FORMAT_CSV:
                    tuple = pipeline_parse_csv(pipeline, record);
                    break;
                default:
                    break;
            }

            if (tuple != NULL)
            {
                /* Apply transform if specified */
                if (pipeline->transform_sql != NULL)
                    tuple = pipeline_apply_transform(pipeline, tuple);

                if (tuple != NULL)
                    tuples = lappend(tuples, tuple);
                else
                    error_count++;
            }
            else
            {
                error_count++;
            }
        }

        /* Bulk insert tuples */
        if (list_length(tuples) > 0)
        {
            pipeline_bulk_insert(pipeline, tuples);
            success_count = list_length(tuples);
        }

        /* Commit offset (for exactly-once) */
        if (pipeline->exactly_once)
        {
            pipeline_commit_current_offset(pipeline);
        }

        PopActiveSnapshot();
        CommitTransactionCommand();

        /* Update statistics */
        pipeline->rows_ingested += success_count;
        pipeline->errors += error_count;
    }
    PG_CATCH();
    {
        PopActiveSnapshot();
        AbortCurrentTransaction();

        /* Log error and continue */
        elog(WARNING, "Pipeline %s batch failed: %s",
             pipeline->name, PG_exception_stack->message);

        pipeline->errors += list_length(batch);

        /* Pause if too many errors */
        if (pipeline->errors >= pipeline->max_errors)
        {
            pipeline_pause(pipeline->pipeline_id);
        }
    }
    PG_END_TRY();
}
```

### 3.4 SQL Interface

```sql
-- Pipeline management functions

-- Create a Kafka pipeline
CREATE FUNCTION create_pipeline(
    name text,
    source_uri text,
    target_table regclass,
    format text DEFAULT 'json',
    transform_sql text DEFAULT NULL,
    batch_size integer DEFAULT 10000,
    exactly_once boolean DEFAULT true
) RETURNS bigint AS $$
DECLARE
    pipeline_id bigint;
BEGIN
    INSERT INTO orochi.orochi_pipelines
    (name, source_uri, target_table_oid, format, transform_sql,
     batch_size, exactly_once, state, created_at)
    VALUES
    (name, source_uri, target_table, format, transform_sql,
     batch_size, exactly_once, 0, NOW())
    RETURNING orochi.orochi_pipelines.pipeline_id INTO pipeline_id;

    RAISE NOTICE 'Created pipeline % with ID %', name, pipeline_id;
    RETURN pipeline_id;
END;
$$ LANGUAGE plpgsql;

-- Example usage
-- Kafka pipeline
SELECT create_pipeline(
    'sensor_events',
    'kafka://broker1:9092,broker2:9092/sensor_topic',
    'sensor_data'::regclass,
    'json',
    $$
        SELECT
            (data->>'device_id')::text as device_id,
            (data->>'timestamp')::timestamptz as time,
            (data->>'temperature')::float as temperature,
            (data->>'humidity')::float as humidity
        FROM json_records
    $$
);

-- S3 pipeline
SELECT create_pipeline(
    'log_import',
    's3://my-bucket/logs/',
    'access_logs'::regclass,
    'csv'
);

-- Pipeline control
CREATE FUNCTION start_pipeline(pipeline_id bigint) RETURNS void AS $$
BEGIN
    PERFORM orochi._start_pipeline(pipeline_id);
    RAISE NOTICE 'Started pipeline %', pipeline_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION pause_pipeline(pipeline_id bigint) RETURNS void AS $$
BEGIN
    PERFORM orochi._pause_pipeline(pipeline_id);
    RAISE NOTICE 'Paused pipeline %', pipeline_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION drop_pipeline(pipeline_id bigint) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_pipeline(pipeline_id);
    RAISE NOTICE 'Dropped pipeline %', pipeline_id;
END;
$$ LANGUAGE plpgsql;

-- Pipeline catalog table
CREATE TABLE IF NOT EXISTS orochi.orochi_pipelines (
    pipeline_id BIGSERIAL PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    source_type INTEGER NOT NULL DEFAULT 1,  -- 1=kafka, 2=s3
    source_uri TEXT NOT NULL,
    target_table_oid OID NOT NULL,
    format TEXT NOT NULL DEFAULT 'json',
    transform_sql TEXT,
    batch_size INTEGER NOT NULL DEFAULT 10000,
    batch_timeout_ms INTEGER NOT NULL DEFAULT 1000,
    parallelism INTEGER NOT NULL DEFAULT 1,
    exactly_once BOOLEAN NOT NULL DEFAULT true,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=created, 1=running, 2=paused, 3=error, 4=stopped
    error_table TEXT,
    max_errors INTEGER NOT NULL DEFAULT 1000,
    rows_ingested BIGINT NOT NULL DEFAULT 0,
    bytes_ingested BIGINT NOT NULL DEFAULT 0,
    errors BIGINT NOT NULL DEFAULT 0,
    last_activity TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Pipeline offset tracking
CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_offsets (
    pipeline_id BIGINT NOT NULL REFERENCES orochi.orochi_pipelines(pipeline_id) ON DELETE CASCADE,
    partition INTEGER NOT NULL DEFAULT 0,
    offset_value BIGINT NOT NULL DEFAULT 0,
    file_path TEXT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (pipeline_id, partition)
);

-- Pipeline monitoring view
CREATE VIEW orochi.pipelines AS
SELECT
    p.pipeline_id,
    p.name,
    CASE p.source_type
        WHEN 1 THEN 'kafka'
        WHEN 2 THEN 's3'
        WHEN 3 THEN 'http'
        WHEN 4 THEN 'file'
    END as source_type,
    p.source_uri,
    t.schema_name || '.' || t.table_name as target_table,
    p.format,
    CASE p.state
        WHEN 0 THEN 'created'
        WHEN 1 THEN 'running'
        WHEN 2 THEN 'paused'
        WHEN 3 THEN 'error'
        WHEN 4 THEN 'stopped'
    END as state,
    p.rows_ingested,
    pg_size_pretty(p.bytes_ingested) as bytes_ingested,
    p.errors,
    p.last_activity
FROM orochi.orochi_pipelines p
JOIN orochi.orochi_tables t ON p.target_table_oid = t.table_oid;
```

---

## 4. Approximate Algorithms

### 4.1 Architecture Overview

```
                 Approximate Aggregates
                        |
        +---------------+---------------+
        |               |               |
        v               v               v
  +-----------+   +-----------+   +-----------+
  | HyperLog  |   | T-Digest  |   | Sampling  |
  | Log       |   |           |   | Operators |
  +-----------+   +-----------+   +-----------+
        |               |               |
        v               v               v
  Cardinality      Percentiles      Approximate
  Estimation       p50, p99, etc.   Query Results
```

### 4.2 File-by-File Implementation

#### `src/approximate/hyperloglog.h` - HyperLogLog Cardinality Estimation
```c
/*
 * HyperLogLog - Probabilistic cardinality estimation
 *
 * Based on the HyperLogLog++ algorithm from Google.
 * Provides ~1.04/sqrt(m) standard error where m is number of registers.
 */

/* Precision (p) determines number of registers (m = 2^p) */
#define HLL_MIN_PRECISION       4       /* 16 registers, ~26% error */
#define HLL_DEFAULT_PRECISION   14      /* 16384 registers, ~0.81% error */
#define HLL_MAX_PRECISION       18      /* 262144 registers, ~0.20% error */

/*
 * HyperLogLog state
 */
typedef struct HyperLogLog
{
    int             precision;          /* Precision parameter (p) */
    int             num_registers;      /* 2^precision */
    uint8          *registers;          /* Register array (max leading zeros) */
    bool            sparse;             /* Using sparse representation */
    List           *sparse_list;        /* Sparse register list */
    int             sparse_threshold;   /* When to switch to dense */
} HyperLogLog;

/* HLL lifecycle */
extern HyperLogLog *hll_create(int precision);
extern HyperLogLog *hll_create_default(void);
extern void hll_destroy(HyperLogLog *hll);
extern HyperLogLog *hll_copy(HyperLogLog *hll);

/* Add elements */
extern void hll_add_hash(HyperLogLog *hll, uint64 hash);
extern void hll_add_int64(HyperLogLog *hll, int64 value);
extern void hll_add_text(HyperLogLog *hll, const char *value, int len);
extern void hll_add_datum(HyperLogLog *hll, Datum value, Oid type_oid);

/* Cardinality estimation */
extern double hll_estimate(HyperLogLog *hll);
extern double hll_estimate_bias_corrected(HyperLogLog *hll);

/* Merging (for distributed aggregation) */
extern void hll_merge(HyperLogLog *dest, HyperLogLog *src);
extern HyperLogLog *hll_union(HyperLogLog *a, HyperLogLog *b);

/* Serialization (for storage/transfer) */
extern bytea *hll_serialize(HyperLogLog *hll);
extern HyperLogLog *hll_deserialize(bytea *data);

/* SQL aggregate state functions */
extern Datum hll_add_trans(PG_FUNCTION_ARGS);
extern Datum hll_merge_trans(PG_FUNCTION_ARGS);
extern Datum hll_final(PG_FUNCTION_ARGS);
```

#### `src/approximate/hyperloglog.c` - HyperLogLog Implementation
```c
/*
 * Core HyperLogLog algorithm implementation
 */

/* Hash function - MurmurHash3 */
static uint64
hll_hash(const void *data, int len)
{
    /* MurmurHash3 finalization */
    uint64 h = 0x9747b28c;
    const uint8 *bytes = (const uint8 *)data;

    for (int i = 0; i < len; i++)
    {
        h ^= bytes[i];
        h *= 0x5bd1e995;
        h ^= h >> 15;
    }

    return h;
}

/* Count leading zeros (for register value) */
static int
count_leading_zeros(uint64 x, int precision)
{
    /* Get the bits after the index bits */
    uint64 w = x >> precision;
    if (w == 0)
        return 64 - precision;
    return __builtin_clzll(w) + 1;
}

void
hll_add_hash(HyperLogLog *hll, uint64 hash)
{
    /* Extract register index from first p bits */
    int idx = hash & ((1 << hll->precision) - 1);

    /* Count leading zeros in remaining bits */
    int zeros = count_leading_zeros(hash, hll->precision);

    /* Update register if new value is larger */
    if (zeros > hll->registers[idx])
        hll->registers[idx] = zeros;
}

double
hll_estimate(HyperLogLog *hll)
{
    double alpha;
    double sum = 0.0;
    int zeros = 0;
    int m = hll->num_registers;

    /* Alpha constant based on register count */
    switch (hll->precision)
    {
        case 4:  alpha = 0.673; break;
        case 5:  alpha = 0.697; break;
        case 6:  alpha = 0.709; break;
        default: alpha = 0.7213 / (1.0 + 1.079 / m); break;
    }

    /* Harmonic mean of register values */
    for (int i = 0; i < m; i++)
    {
        sum += pow(2.0, -hll->registers[i]);
        if (hll->registers[i] == 0)
            zeros++;
    }

    double estimate = alpha * m * m / sum;

    /* Small range correction (linear counting) */
    if (estimate <= 2.5 * m && zeros > 0)
    {
        estimate = m * log((double)m / zeros);
    }

    /* Large range correction (not needed for 64-bit hashes) */

    return estimate;
}

void
hll_merge(HyperLogLog *dest, HyperLogLog *src)
{
    Assert(dest->precision == src->precision);

    /* Take maximum of each register */
    for (int i = 0; i < dest->num_registers; i++)
    {
        if (src->registers[i] > dest->registers[i])
            dest->registers[i] = src->registers[i];
    }
}
```

#### `src/approximate/tdigest.h` - T-Digest Percentile Estimation
```c
/*
 * T-Digest - Accurate percentile estimation for streaming data
 *
 * Based on the paper: "Computing Extremely Accurate Quantiles Using t-Digests"
 * by Ted Dunning and Otmar Ertl.
 */

#define TDIGEST_DEFAULT_COMPRESSION     100.0
#define TDIGEST_MAX_CENTROIDS           1000

/*
 * Centroid - weighted mean point
 */
typedef struct TDigestCentroid
{
    double          mean;               /* Centroid mean value */
    double          weight;             /* Number of points (weight) */
} TDigestCentroid;

/*
 * T-Digest state
 */
typedef struct TDigest
{
    double          compression;        /* Compression parameter (delta) */
    TDigestCentroid *centroids;        /* Array of centroids */
    int             num_centroids;      /* Current centroid count */
    int             max_centroids;      /* Maximum centroids */
    double          total_weight;       /* Total weight (count) */
    double          min;                /* Minimum value seen */
    double          max;                /* Maximum value seen */

    /* Buffer for batch processing */
    double         *buffer;             /* Unsorted values */
    int             buffer_count;       /* Values in buffer */
    int             buffer_capacity;    /* Buffer size */
} TDigest;

/* T-Digest lifecycle */
extern TDigest *tdigest_create(double compression);
extern TDigest *tdigest_create_default(void);
extern void tdigest_destroy(TDigest *td);
extern TDigest *tdigest_copy(TDigest *td);

/* Add values */
extern void tdigest_add(TDigest *td, double value);
extern void tdigest_add_weighted(TDigest *td, double value, double weight);
extern void tdigest_add_batch(TDigest *td, double *values, int count);

/* Quantile queries */
extern double tdigest_quantile(TDigest *td, double q);
extern double tdigest_percentile(TDigest *td, double p);
extern double tdigest_median(TDigest *td);
extern double tdigest_cdf(TDigest *td, double value);

/* Merging (for distributed aggregation) */
extern void tdigest_merge(TDigest *dest, TDigest *src);
extern TDigest *tdigest_merge_all(TDigest **digests, int count);

/* Serialization */
extern bytea *tdigest_serialize(TDigest *td);
extern TDigest *tdigest_deserialize(bytea *data);

/* SQL aggregate state functions */
extern Datum tdigest_add_trans(PG_FUNCTION_ARGS);
extern Datum tdigest_merge_trans(PG_FUNCTION_ARGS);
extern Datum tdigest_percentile_final(PG_FUNCTION_ARGS);
```

#### `src/approximate/sampling.h` - Sampling Operators
```c
/*
 * Table sampling for approximate queries
 */

typedef enum SamplingMethod
{
    SAMPLE_BERNOULLI = 1,       /* Random with probability p */
    SAMPLE_SYSTEM,              /* Page-level sampling */
    SAMPLE_RESERVOIR            /* Reservoir sampling (fixed size) */
} SamplingMethod;

/*
 * Sampling state
 */
typedef struct SamplingState
{
    SamplingMethod  method;
    double          sample_rate;        /* For Bernoulli/System */
    int64           sample_size;        /* For Reservoir */
    int64           rows_seen;          /* Total rows examined */
    int64           rows_sampled;       /* Rows in sample */

    /* Reservoir state */
    HeapTuple      *reservoir;          /* Fixed-size reservoir */
    int             reservoir_count;

    /* Statistics */
    double          actual_rate;        /* Actual sampling rate */
} SamplingState;

/* Sampling initialization */
extern SamplingState *sampling_init_bernoulli(double rate);
extern SamplingState *sampling_init_system(double rate);
extern SamplingState *sampling_init_reservoir(int64 sample_size);
extern void sampling_destroy(SamplingState *state);

/* Sampling operations */
extern bool sampling_should_include(SamplingState *state, HeapTuple tuple);
extern void sampling_add_to_reservoir(SamplingState *state, HeapTuple tuple);

/* SQL-level sampling */
extern Datum table_sample_bernoulli(PG_FUNCTION_ARGS);
extern Datum table_sample_system(PG_FUNCTION_ARGS);
```

### 4.3 SQL Interface

```sql
-- Approximate count distinct (HyperLogLog)
CREATE FUNCTION approx_count_distinct(anyelement)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_approx_count_distinct'
    LANGUAGE C IMMUTABLE;

CREATE AGGREGATE approx_count_distinct(anyelement) (
    SFUNC = hll_add_trans,
    STYPE = internal,
    FINALFUNC = hll_count_final,
    COMBINEFUNC = hll_merge_trans,
    SERIALFUNC = hll_serialize_trans,
    DESERIALFUNC = hll_deserialize_trans,
    PARALLEL = SAFE
);

-- Approximate percentile (T-Digest)
CREATE FUNCTION approx_percentile(double precision, double precision)
RETURNS double precision
    AS 'MODULE_PATHNAME', 'orochi_approx_percentile'
    LANGUAGE C IMMUTABLE;

CREATE AGGREGATE approx_percentile(double precision, double precision) (
    SFUNC = tdigest_add_trans,
    STYPE = internal,
    FINALFUNC = tdigest_percentile_final,
    FINALFUNC_EXTRA,
    COMBINEFUNC = tdigest_merge_trans,
    SERIALFUNC = tdigest_serialize_trans,
    DESERIALFUNC = tdigest_deserialize_trans,
    PARALLEL = SAFE
);

-- Convenience functions for common percentiles
CREATE FUNCTION approx_median(double precision)
RETURNS double precision AS $$
    SELECT approx_percentile($1, 0.5);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION approx_p99(double precision)
RETURNS double precision AS $$
    SELECT approx_percentile($1, 0.99);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION approx_p95(double precision)
RETURNS double precision AS $$
    SELECT approx_percentile($1, 0.95);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

-- Table sampling
-- Already supported by PostgreSQL: TABLESAMPLE BERNOULLI(10)

-- Approximate aggregates view
CREATE VIEW orochi.approximate_functions AS
SELECT
    'approx_count_distinct' as function_name,
    'HyperLogLog' as algorithm,
    '~0.81%' as error_rate,
    'Cardinality estimation' as use_case
UNION ALL
SELECT
    'approx_percentile',
    'T-Digest',
    '~1%',
    'Quantile estimation'
UNION ALL
SELECT
    'approx_median',
    'T-Digest',
    '~1%',
    'Median estimation'
UNION ALL
SELECT
    'TABLESAMPLE BERNOULLI',
    'Bernoulli sampling',
    'Varies with sample rate',
    'Random row sampling';

-- Example queries
/*
-- Count distinct users (approximate)
SELECT approx_count_distinct(user_id) FROM events;

-- P99 latency (approximate)
SELECT approx_percentile(latency_ms, 0.99) FROM requests;

-- Multiple percentiles
SELECT
    approx_percentile(latency_ms, 0.50) as p50,
    approx_percentile(latency_ms, 0.95) as p95,
    approx_percentile(latency_ms, 0.99) as p99
FROM requests
WHERE time > NOW() - INTERVAL '1 hour';

-- Sampled query (10% of rows)
SELECT AVG(value) * 10 as estimated_sum
FROM metrics TABLESAMPLE BERNOULLI(10);
*/
```

---

## 5. Implementation Timeline

### Phase 1: Foundation (Weeks 1-6)
- **Week 1-2**: Raft core implementation (state machine, log)
- **Week 3-4**: Raft transport and message handling
- **Week 5-6**: Multi-Raft coordination, basic integration tests

### Phase 2: Vectorization (Weeks 7-12)
- **Week 7-8**: Vector batch types and primitives
- **Week 9-10**: SIMD implementations (AVX2/AVX-512)
- **Week 11-12**: Vectorized executor nodes, columnar integration

### Phase 3: Pipelines (Weeks 13-18)
- **Week 13-14**: Pipeline core infrastructure
- **Week 15-16**: Kafka consumer integration
- **Week 17-18**: S3 watcher, exactly-once semantics

### Phase 4: Approximate Algorithms (Weeks 19-22)
- **Week 19-20**: HyperLogLog implementation
- **Week 21-22**: T-Digest, sampling operators

### Phase 5: Integration and Testing (Weeks 23-26)
- **Week 23-24**: Full integration testing
- **Week 25-26**: Performance benchmarking, documentation

---

## 6. Testing Strategy

### Unit Tests
```
tests/
  consensus/
    test_raft_state_machine.c
    test_raft_log.c
    test_raft_election.c
    test_multi_raft.c
  vectorized/
    test_vector_batch.c
    test_vector_primitives.c
    test_simd_operations.c
    test_vector_executor.c
  pipeline/
    test_kafka_consumer.c
    test_s3_watcher.c
    test_pipeline_worker.c
  approximate/
    test_hyperloglog.c
    test_tdigest.c
    test_sampling.c
```

### Integration Tests
```sql
-- tests/sql/test_raft_consensus.sql
-- tests/sql/test_vectorized_queries.sql
-- tests/sql/test_pipelines.sql
-- tests/sql/test_approximate_aggregates.sql
```

### Performance Benchmarks
```
benchmarks/
  raft_throughput.c           -- Raft commit rate
  vectorized_scan.c           -- Vectorized vs row scan
  pipeline_throughput.c       -- Kafka ingestion rate
  approximate_accuracy.c      -- Error rate vs exact
```

---

## 7. Dependencies

### External Libraries

| Library | Version | Purpose | License |
|---------|---------|---------|---------|
| librdkafka | 2.3+ | Kafka consumer | BSD-2 |
| aws-sdk-cpp | 1.11+ | S3 integration | Apache 2.0 |
| xxhash | 0.8+ | Fast hashing | BSD-2 |

### Build Requirements
```makefile
# Makefile additions
SHLIB_LINK += -lrdkafka
SHLIB_LINK += -laws-cpp-sdk-s3

# Compiler flags for SIMD
PG_CFLAGS += -mavx2 -mavx512f
```

---

## 8. Risk Assessment

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| Raft correctness bugs | High | Medium | Jepsen testing, formal verification |
| SIMD portability | Medium | Low | Runtime detection, scalar fallbacks |
| Kafka API changes | Medium | Low | Abstraction layer, version pinning |
| Performance regression | High | Medium | Continuous benchmarking |

---

## 9. Success Criteria

### Raft Consensus
- Leader election within 500ms
- Log replication latency < 10ms (single region)
- Zero data loss during planned failover
- Pass Jepsen tests for linearizability

### Vectorized Execution
- 3-10x speedup for analytical queries
- < 5% overhead for OLTP queries
- AVX-512 utilization > 80% where available

### Real-time Pipelines
- 100K+ events/second per pipeline
- End-to-end latency < 1 second
- Zero data loss with exactly-once enabled

### Approximate Algorithms
- HyperLogLog error < 1% with default settings
- T-Digest error < 2% for P50-P99
- Memory usage < 10KB per aggregate state

---

## Appendix A: Configuration Parameters

```sql
-- GUC parameters added by these features

-- Raft
orochi.raft_heartbeat_interval = '150ms'
orochi.raft_election_timeout_min = '1s'
orochi.raft_election_timeout_max = '2s'
orochi.raft_snapshot_interval = '10000'

-- Vectorization
orochi.enable_vectorized_execution = on
orochi.vector_batch_size = 1024
orochi.vectorized_min_table_size = '1MB'
orochi.force_vectorization = off

-- Pipelines
orochi.max_pipeline_workers = 8
orochi.pipeline_batch_size = 10000
orochi.pipeline_batch_timeout = '1s'

-- Approximate
orochi.hll_default_precision = 14
orochi.tdigest_compression = 100
```

---

## Appendix B: Migration Notes

### Upgrading from 1.0 to 1.1

1. **Raft Migration**: Existing single-coordinator clusters require manual conversion
   ```sql
   SELECT orochi.enable_raft_consensus();
   SELECT orochi.add_raft_member(node_id) FROM orochi.nodes WHERE role = 'worker';
   ```

2. **Vectorization**: Automatic; toggle with GUC parameter

3. **Pipelines**: New feature; no migration needed

4. **Approximate Functions**: New feature; no migration needed

---

*Document Version: 1.0*
*Last Updated: 2026-01-16*
*Author: System Architecture Team*
