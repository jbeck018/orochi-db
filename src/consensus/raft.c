/*-------------------------------------------------------------------------
 *
 * raft.c
 *    Orochi DB Raft consensus protocol implementation
 *
 * This file contains the core Raft algorithm implementation:
 *   - Leader election with randomized timeouts
 *   - Log replication via AppendEntries RPC
 *   - Heartbeat mechanism for liveness
 *   - State machine application
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "storage/lwlock.h"
#include "miscadmin.h"
#include "libpq-fe.h"
#include "utils/builtins.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "raft.h"

/* ============================================================
 * JSON Escape Helper (Security: prevents JSON injection)
 * ============================================================ */

/*
 * escape_json_string - Escape a string for safe JSON embedding
 *
 * Escapes quotes, backslashes, and control characters to prevent
 * JSON injection attacks when embedding user data in JSON.
 */
static char *
escape_json_string(const char *input)
{
    StringInfoData escaped;
    const char *p;

    if (input == NULL)
        return pstrdup("");

    initStringInfo(&escaped);

    for (p = input; *p != '\0'; p++)
    {
        switch (*p)
        {
            case '"':
                appendStringInfoString(&escaped, "\\\"");
                break;
            case '\\':
                appendStringInfoString(&escaped, "\\\\");
                break;
            case '\n':
                appendStringInfoString(&escaped, "\\n");
                break;
            case '\r':
                appendStringInfoString(&escaped, "\\r");
                break;
            case '\t':
                appendStringInfoString(&escaped, "\\t");
                break;
            case '\b':
                appendStringInfoString(&escaped, "\\b");
                break;
            case '\f':
                appendStringInfoString(&escaped, "\\f");
                break;
            default:
                /* Escape control characters (0x00-0x1F) */
                if ((unsigned char)*p < 0x20)
                {
                    appendStringInfo(&escaped, "\\u%04x", (unsigned char)*p);
                }
                else
                {
                    appendStringInfoChar(&escaped, *p);
                }
                break;
        }
    }

    return escaped.data;
}

#include "../core/catalog.h"

/* ============================================================
 * Private Function Declarations
 * ============================================================ */

static void raft_become_follower(RaftNode *node, uint64 term);
static void raft_become_candidate(RaftNode *node);
static void raft_become_leader(RaftNode *node);
static void raft_reset_election_timeout(RaftNode *node);
static void raft_apply_committed_entries(RaftNode *node);
static bool raft_is_log_up_to_date(RaftNode *node, uint64 last_log_index,
                                   uint64 last_log_term);
static int raft_count_votes(RaftNode *node);
static void raft_initialize_leader_state(RaftNode *node);
static PGconn *raft_get_peer_connection(RaftPeer *peer);

/* ============================================================
 * Raft Node Lifecycle
 * ============================================================ */

/*
 * raft_init - Initialize a new Raft node
 */
RaftNode *
raft_init(int32 node_id, const char *wal_path)
{
    MemoryContext old_context;
    MemoryContext raft_context;
    RaftNode *node;

    /* Create dedicated memory context for Raft */
    raft_context = AllocSetContextCreate(TopMemoryContext,
                                         "RaftContext",
                                         ALLOCSET_DEFAULT_SIZES);

    old_context = MemoryContextSwitchTo(raft_context);

    node = (RaftNode *) palloc0(sizeof(RaftNode));
    node->raft_context = raft_context;

    /* Initialize persistent state */
    node->current_term = 0;
    node->voted_for = -1;
    node->log = raft_log_init(wal_path);

    /* Initialize volatile state */
    node->state = RAFT_STATE_FOLLOWER;
    node->node_id = node_id;
    node->leader_id = -1;
    node->is_leader = false;

    /* Initialize cluster */
    node->cluster = (RaftCluster *) palloc0(sizeof(RaftCluster));
    node->cluster->peers = (RaftPeer *) palloc0(sizeof(RaftPeer) * RAFT_MAX_CLUSTER_SIZE);
    node->cluster->peer_count = 0;
    node->cluster->quorum_size = 1;  /* Just us initially */
    node->cluster->my_node_id = node_id;

    /* Initialize timing */
    raft_reset_election_timeout(node);
    node->last_heartbeat = GetCurrentTimestamp();

    /* Initialize statistics */
    node->elections_started = 0;
    node->elections_won = 0;
    node->append_entries_sent = 0;
    node->votes_received = 0;

    /* Initialize callbacks */
    node->apply_callback = NULL;
    node->apply_context = NULL;

    /* Initialize synchronization */
    node->state_lock = NULL;  /* Will be assigned from shared memory if needed */
    node->initialized = true;
    node->running = false;

    /* Attempt to recover persisted state */
    raft_load_state(node);
    if (node->log->persistence_enabled)
        raft_log_recover(node->log);

    MemoryContextSwitchTo(old_context);

    elog(LOG, "Raft node %d initialized (term=%lu)", node_id, node->current_term);

    return node;
}

/*
 * raft_start - Start Raft protocol processing
 */
void
raft_start(RaftNode *node)
{
    if (!node->initialized)
    {
        elog(ERROR, "Cannot start uninitialized Raft node");
        return;
    }

    node->running = true;
    node->state = RAFT_STATE_FOLLOWER;
    raft_reset_election_timeout(node);

    elog(LOG, "Raft node %d started as follower", node->node_id);
}

/*
 * raft_stop - Stop Raft protocol processing
 */
void
raft_stop(RaftNode *node)
{
    if (!node->running)
        return;

    node->running = false;
    node->is_leader = false;

    /* Persist current state before stopping */
    raft_persist_state(node);
    raft_log_persist(node->log);

    elog(LOG, "Raft node %d stopped", node->node_id);
}

/*
 * raft_shutdown - Clean up and free Raft node
 */
void
raft_shutdown(RaftNode *node)
{
    if (node == NULL)
        return;

    raft_stop(node);
    raft_disconnect_peers(node);

    if (node->log != NULL)
        raft_log_free(node->log);

    if (node->cluster != NULL)
    {
        if (node->cluster->peers != NULL)
            pfree(node->cluster->peers);
        pfree(node->cluster);
    }

    /* Free the entire Raft memory context */
    MemoryContextDelete(node->raft_context);
}

/*
 * raft_tick - Main event loop tick, call periodically
 */
void
raft_tick(RaftNode *node)
{
    TimestampTz now;

    if (!node->running)
        return;

    now = GetCurrentTimestamp();

    switch (node->state)
    {
        case RAFT_STATE_FOLLOWER:
        case RAFT_STATE_CANDIDATE:
            /* Check for election timeout */
            if (now >= node->election_timeout)
            {
                raft_start_election(node);
            }
            break;

        case RAFT_STATE_LEADER:
            /* Send periodic heartbeats */
            {
                int64 heartbeat_interval_us = RAFT_HEARTBEAT_INTERVAL_MS * 1000L;
                if (now - node->last_heartbeat >= heartbeat_interval_us)
                {
                    raft_heartbeat(node);
                    node->last_heartbeat = now;
                }
            }
            break;
    }

    /* Apply any newly committed entries */
    raft_apply_committed_entries(node);
}

/* ============================================================
 * State Transitions
 * ============================================================ */

/*
 * raft_become_follower - Transition to follower state
 */
static void
raft_become_follower(RaftNode *node, uint64 term)
{
    elog(DEBUG1, "Raft node %d becoming follower (term=%lu)",
         node->node_id, term);

    node->state = RAFT_STATE_FOLLOWER;
    node->is_leader = false;
    node->current_term = term;
    node->voted_for = -1;

    raft_reset_election_timeout(node);
    raft_persist_state(node);
}

/*
 * raft_become_candidate - Transition to candidate state
 */
static void
raft_become_candidate(RaftNode *node)
{
    int i;

    node->state = RAFT_STATE_CANDIDATE;
    node->is_leader = false;
    node->current_term++;
    node->voted_for = node->node_id;  /* Vote for self */
    node->votes_received = 1;         /* Count self vote */
    node->leader_id = -1;

    /* Reset vote tracking for all peers */
    for (i = 0; i < node->cluster->peer_count; i++)
    {
        node->cluster->peers[i].vote_granted = false;
    }

    raft_reset_election_timeout(node);
    raft_persist_state(node);

    elog(DEBUG1, "Raft node %d becoming candidate (term=%lu)",
         node->node_id, node->current_term);
}

/*
 * raft_become_leader - Transition to leader state
 */
static void
raft_become_leader(RaftNode *node)
{
    RaftLogEntry noop_entry;

    elog(LOG, "Raft node %d becoming leader (term=%lu)",
         node->node_id, node->current_term);

    node->state = RAFT_STATE_LEADER;
    node->is_leader = true;
    node->leader_id = node->node_id;
    node->elections_won++;

    /* Initialize leader volatile state */
    raft_initialize_leader_state(node);

    /* Append a no-op entry to establish leadership */
    memset(&noop_entry, 0, sizeof(RaftLogEntry));
    noop_entry.term = node->current_term;
    noop_entry.type = RAFT_LOG_NOOP;
    noop_entry.command_size = 0;
    noop_entry.command_data = NULL;
    noop_entry.created_at = GetCurrentTimestamp();

    raft_log_append(node->log, &noop_entry, 1);

    /* Send initial heartbeats to establish authority */
    raft_heartbeat(node);
}

/*
 * raft_initialize_leader_state - Initialize next_index and match_index for all peers
 */
static void
raft_initialize_leader_state(RaftNode *node)
{
    int i;
    uint64 last_log_index = node->log->last_index;

    for (i = 0; i < node->cluster->peer_count; i++)
    {
        node->cluster->peers[i].next_index = last_log_index + 1;
        node->cluster->peers[i].match_index = 0;
    }
}

/*
 * raft_reset_election_timeout - Set new randomized election timeout
 */
static void
raft_reset_election_timeout(RaftNode *node)
{
    int timeout_ms = raft_random_election_timeout();
    node->election_timeout = GetCurrentTimestamp() + (timeout_ms * 1000L);
}

/* ============================================================
 * Leader Election
 * ============================================================ */

/*
 * raft_start_election - Begin a new election
 */
void
raft_start_election(RaftNode *node)
{
    int i;
    int votes_needed;
    RequestVoteRequest request;

    if (!node->running)
        return;

    node->elections_started++;
    raft_become_candidate(node);

    elog(DEBUG1, "Raft node %d starting election for term %lu",
         node->node_id, node->current_term);

    /* Build RequestVote request */
    request.term = node->current_term;
    request.candidate_id = node->node_id;
    request.last_log_index = node->log->last_index;
    request.last_log_term = raft_log_term_at(node->log, node->log->last_index);

    /* Send RequestVote to all peers */
    for (i = 0; i < node->cluster->peer_count; i++)
    {
        RaftPeer *peer = &node->cluster->peers[i];
        RequestVoteResponse response;

        if (!peer->is_active)
            continue;

        response = raft_send_request_vote(peer, &request);

        /* Check if we discovered a higher term */
        if (response.term > node->current_term)
        {
            raft_become_follower(node, response.term);
            return;
        }

        if (response.vote_granted)
        {
            peer->vote_granted = true;
            node->votes_received++;

            elog(DEBUG1, "Raft node %d received vote from node %d",
                 node->node_id, peer->node_id);
        }
    }

    /* Check if we won the election */
    votes_needed = node->cluster->quorum_size;
    if (node->votes_received >= votes_needed)
    {
        raft_become_leader(node);
    }
    else
    {
        elog(DEBUG1, "Raft node %d election failed: got %lu votes, needed %d",
             node->node_id, node->votes_received, votes_needed);
    }
}

/*
 * raft_request_vote - Handle incoming RequestVote RPC
 */
RequestVoteResponse
raft_request_vote(RaftNode *node, RequestVoteRequest *request)
{
    RequestVoteResponse response;

    response.term = node->current_term;
    response.vote_granted = false;
    response.voter_id = node->node_id;

    /* If request term is older, reject */
    if (request->term < node->current_term)
    {
        elog(DEBUG1, "Raft node %d rejecting vote for node %d: stale term %lu < %lu",
             node->node_id, request->candidate_id,
             request->term, node->current_term);
        return response;
    }

    /* If we see a higher term, become follower */
    if (request->term > node->current_term)
    {
        raft_become_follower(node, request->term);
    }

    response.term = node->current_term;

    /* Check if we can vote for this candidate */
    if ((node->voted_for == -1 || node->voted_for == request->candidate_id) &&
        raft_is_log_up_to_date(node, request->last_log_index, request->last_log_term))
    {
        /* Grant vote */
        node->voted_for = request->candidate_id;
        response.vote_granted = true;
        raft_reset_election_timeout(node);
        raft_persist_state(node);

        elog(DEBUG1, "Raft node %d granting vote to node %d for term %lu",
             node->node_id, request->candidate_id, request->term);
    }
    else
    {
        elog(DEBUG1, "Raft node %d rejecting vote for node %d: "
             "already voted for %d or log not up-to-date",
             node->node_id, request->candidate_id, node->voted_for);
    }

    return response;
}

/*
 * raft_is_log_up_to_date - Check if candidate's log is at least as up-to-date as ours
 */
static bool
raft_is_log_up_to_date(RaftNode *node, uint64 last_log_index, uint64 last_log_term)
{
    uint64 my_last_term = raft_log_term_at(node->log, node->log->last_index);
    uint64 my_last_index = node->log->last_index;

    /* Compare terms first */
    if (last_log_term != my_last_term)
        return last_log_term > my_last_term;

    /* Same term, compare indices */
    return last_log_index >= my_last_index;
}

/* ============================================================
 * Log Replication
 * ============================================================ */

/*
 * raft_heartbeat - Send heartbeat (empty AppendEntries) to all followers
 */
void
raft_heartbeat(RaftNode *node)
{
    if (!node->is_leader)
        return;

    raft_replicate(node);
}

/*
 * raft_replicate - Replicate log entries to all followers
 */
void
raft_replicate(RaftNode *node)
{
    int i;

    if (!node->is_leader)
        return;

    for (i = 0; i < node->cluster->peer_count; i++)
    {
        RaftPeer *peer = &node->cluster->peers[i];
        AppendEntriesRequest request;
        AppendEntriesResponse response;
        int32 entries_count = 0;
        RaftLogEntry *entries = NULL;

        if (!peer->is_active)
            continue;

        /* Build AppendEntries request */
        request.term = node->current_term;
        request.leader_id = node->node_id;
        request.prev_log_index = peer->next_index - 1;
        request.prev_log_term = raft_log_term_at(node->log, request.prev_log_index);
        request.leader_commit = node->log->commit_index;

        /* Get entries to send */
        if (peer->next_index <= node->log->last_index)
        {
            uint64 end_index = Min(peer->next_index + RAFT_MAX_ENTRIES_PER_APPEND - 1,
                                   node->log->last_index);
            entries = raft_log_get_range(node->log, peer->next_index,
                                         end_index, &entries_count);
        }

        request.entries = entries;
        request.entries_count = entries_count;

        /* Send request */
        node->append_entries_sent++;
        response = raft_send_append_entries(peer, &request);

        /* Handle response */
        if (response.term > node->current_term)
        {
            /* We're outdated, step down */
            raft_become_follower(node, response.term);
            return;
        }

        if (response.success)
        {
            /* Update peer indices */
            if (entries_count > 0)
            {
                peer->next_index = response.match_index + 1;
                peer->match_index = response.match_index;
            }
            peer->last_contact = GetCurrentTimestamp();
        }
        else
        {
            /* Decrement next_index and retry */
            if (peer->next_index > 1)
                peer->next_index--;
        }
    }

    /* Update commit index based on replication status */
    raft_update_commit_index(node);
}

/*
 * raft_append_entries - Handle incoming AppendEntries RPC
 */
AppendEntriesResponse
raft_append_entries(RaftNode *node, AppendEntriesRequest *request)
{
    AppendEntriesResponse response;
    int i;

    response.term = node->current_term;
    response.success = false;
    response.match_index = 0;
    response.node_id = node->node_id;

    /* Reply false if term < currentTerm */
    if (request->term < node->current_term)
    {
        elog(DEBUG1, "Raft node %d rejecting AppendEntries: stale term %lu < %lu",
             node->node_id, request->term, node->current_term);
        return response;
    }

    /* If we see a higher or equal term, become/stay follower */
    if (request->term > node->current_term ||
        (request->term == node->current_term && node->state == RAFT_STATE_CANDIDATE))
    {
        raft_become_follower(node, request->term);
    }

    /* Reset election timeout - we heard from a leader */
    raft_reset_election_timeout(node);
    node->leader_id = request->leader_id;
    response.term = node->current_term;

    /* Check log consistency */
    if (request->prev_log_index > 0)
    {
        uint64 term_at_prev = raft_log_term_at(node->log, request->prev_log_index);

        if (request->prev_log_index > node->log->last_index ||
            term_at_prev != request->prev_log_term)
        {
            elog(DEBUG1, "Raft node %d rejecting AppendEntries: log inconsistency "
                 "at index %lu (expected term %lu, have %lu)",
                 node->node_id, request->prev_log_index,
                 request->prev_log_term, term_at_prev);
            response.match_index = Min(node->log->last_index, request->prev_log_index - 1);
            return response;
        }
    }

    /* Process new entries */
    if (request->entries_count > 0)
    {
        for (i = 0; i < request->entries_count; i++)
        {
            RaftLogEntry *entry = &request->entries[i];
            uint64 entry_index = request->prev_log_index + 1 + i;

            /* Check for conflicting entry */
            if (entry_index <= node->log->last_index)
            {
                uint64 existing_term = raft_log_term_at(node->log, entry_index);
                if (existing_term != entry->term)
                {
                    /* Conflict - truncate log from here */
                    raft_log_truncate(node->log, entry_index - 1);
                }
                else
                {
                    /* Entry already exists and matches */
                    continue;
                }
            }

            /* Append the entry */
            entry->index = entry_index;
            raft_log_append(node->log, entry, 1);
        }

        elog(DEBUG1, "Raft node %d appended %d entries (last_index now %lu)",
             node->node_id, request->entries_count, node->log->last_index);
    }

    /* Update commit index */
    if (request->leader_commit > node->log->commit_index)
    {
        node->log->commit_index = Min(request->leader_commit, node->log->last_index);
    }

    response.success = true;
    response.match_index = node->log->last_index;

    return response;
}

/*
 * raft_update_commit_index - Update commit index based on peer acknowledgments
 */
void
raft_update_commit_index(RaftNode *node)
{
    uint64 new_commit;
    int i;

    if (!node->is_leader)
        return;

    /* Find the highest index replicated to a majority */
    for (new_commit = node->log->last_index;
         new_commit > node->log->commit_index;
         new_commit--)
    {
        int replicated_count = 1;  /* Count self */

        /* Only commit entries from current term */
        if (raft_log_term_at(node->log, new_commit) != node->current_term)
            continue;

        /* Count how many peers have this entry */
        for (i = 0; i < node->cluster->peer_count; i++)
        {
            if (node->cluster->peers[i].match_index >= new_commit)
                replicated_count++;
        }

        /* Check if majority has the entry */
        if (replicated_count >= node->cluster->quorum_size)
        {
            node->log->commit_index = new_commit;
            elog(DEBUG1, "Raft node %d advanced commit_index to %lu",
                 node->node_id, new_commit);
            break;
        }
    }
}

/*
 * raft_apply_committed_entries - Apply newly committed entries to state machine
 */
static void
raft_apply_committed_entries(RaftNode *node)
{
    while (node->log->last_applied < node->log->commit_index)
    {
        RaftLogEntry *entry;

        node->log->last_applied++;
        entry = raft_log_get(node->log, node->log->last_applied);

        if (entry == NULL)
        {
            elog(WARNING, "Raft node %d: missing log entry at index %lu",
                 node->node_id, node->log->last_applied);
            continue;
        }

        /* Skip no-op entries */
        if (entry->type == RAFT_LOG_NOOP)
            continue;

        /* Apply to state machine via callback */
        if (node->apply_callback != NULL)
        {
            node->apply_callback(entry, node->apply_context);
        }

        elog(DEBUG1, "Raft node %d applied entry at index %lu",
             node->node_id, node->log->last_applied);
    }
}

/*
 * raft_install_snapshot - Handle InstallSnapshot RPC
 *
 * This implements the Raft InstallSnapshot RPC handler. When a follower is
 * too far behind, the leader sends a snapshot instead of individual log entries.
 */
InstallSnapshotResponse
raft_install_snapshot(RaftNode *node, InstallSnapshotRequest *request)
{
    InstallSnapshotResponse response;
    StringInfoData snapshot_path;
    int fd;

    /* Static variables to accumulate snapshot chunks */
    static char *snapshot_buffer = NULL;
    static int32 snapshot_buffer_size = 0;
    static int32 snapshot_offset = 0;
    static uint64 snapshot_last_index = 0;

    response.term = node->current_term;
    response.node_id = node->node_id;
    response.bytes_received = 0;
    response.success = false;

    /* Reject if term is stale */
    if (request->term < node->current_term)
    {
        elog(DEBUG1, "Raft node %d rejecting snapshot: stale term %lu < %lu",
             node->node_id, request->term, node->current_term);
        return response;
    }

    /* Update term if needed */
    if (request->term > node->current_term)
        raft_become_follower(node, request->term);

    /* Reset election timer - we heard from leader */
    raft_reset_election_timeout(node);
    node->leader_id = request->leader_id;
    response.term = node->current_term;

    /* If this is the first chunk (offset == 0), create new buffer */
    if (request->offset == 0)
    {
        /* Free any previous incomplete snapshot */
        if (snapshot_buffer != NULL)
        {
            pfree(snapshot_buffer);
            snapshot_buffer = NULL;
        }
        snapshot_buffer_size = 0;
        snapshot_offset = 0;
        snapshot_last_index = request->last_included_index;

        elog(DEBUG1, "Raft node %d starting new snapshot reception "
             "(last_included_index=%lu, last_included_term=%lu)",
             node->node_id, request->last_included_index,
             request->last_included_term);
    }

    /* Validate offset continuity */
    if (request->offset != snapshot_offset)
    {
        elog(WARNING, "Raft node %d: snapshot chunk out of order "
             "(expected offset=%d, got=%d)",
             node->node_id, snapshot_offset, request->offset);
        /* Reset and request retransmission from start */
        if (snapshot_buffer != NULL)
        {
            pfree(snapshot_buffer);
            snapshot_buffer = NULL;
        }
        snapshot_buffer_size = 0;
        snapshot_offset = 0;
        return response;
    }

    /* Grow buffer if needed and copy chunk data */
    if (request->data_size > 0 && request->data != NULL)
    {
        int32 new_size = snapshot_offset + request->data_size;
        int32 chunk_alloc_size = 64 * 1024;  /* 64KB allocation chunks */

        if (new_size > snapshot_buffer_size)
        {
            /* Grow buffer in chunks to avoid frequent reallocation */
            int32 alloc_size = ((new_size / chunk_alloc_size) + 1) * chunk_alloc_size;

            if (snapshot_buffer == NULL)
                snapshot_buffer = MemoryContextAlloc(node->raft_context, alloc_size);
            else
                snapshot_buffer = repalloc(snapshot_buffer, alloc_size);

            snapshot_buffer_size = alloc_size;
        }

        memcpy(snapshot_buffer + snapshot_offset, request->data, request->data_size);
        snapshot_offset += request->data_size;
    }

    response.bytes_received = snapshot_offset;
    response.success = true;

    elog(DEBUG1, "Raft node %d received snapshot chunk "
         "(offset=%d, size=%d, total=%d, done=%d)",
         node->node_id, request->offset, request->data_size,
         snapshot_offset, request->done);

    /* If this is the final chunk, apply the snapshot */
    if (request->done)
    {
        bool snapshot_applied = false;

        /* Save snapshot to disk for durability */
        if (node->log->persistence_enabled && node->log->wal_path != NULL)
        {
            initStringInfo(&snapshot_path);
            appendStringInfo(&snapshot_path, "%s/raft_snapshot_%lu.snap",
                             node->log->wal_path, request->last_included_index);

            fd = open(snapshot_path.data, O_WRONLY | O_CREAT | O_TRUNC,
                      S_IRUSR | S_IWUSR);
            if (fd >= 0)
            {
                TimestampTz now = GetCurrentTimestamp();

                /* Write snapshot header */
                write(fd, &request->last_included_index, sizeof(uint64));
                write(fd, &request->last_included_term, sizeof(uint64));
                write(fd, &snapshot_offset, sizeof(int32));
                write(fd, &now, sizeof(TimestampTz));

                /* Write snapshot data */
                if (snapshot_offset > 0 && snapshot_buffer != NULL)
                    write(fd, snapshot_buffer, snapshot_offset);

                fsync(fd);
                close(fd);

                elog(LOG, "Raft node %d saved snapshot to %s (%d bytes)",
                     node->node_id, snapshot_path.data, snapshot_offset);
            }
            else
            {
                elog(WARNING, "Raft node %d failed to save snapshot: %s",
                     node->node_id, strerror(errno));
            }

            pfree(snapshot_path.data);
        }

        /* Apply snapshot to state machine via callback */
        if (node->snapshot_apply_callback != NULL)
        {
            snapshot_applied = node->snapshot_apply_callback(
                request->last_included_index,
                request->last_included_term,
                snapshot_buffer,
                snapshot_offset,
                node->snapshot_apply_context);

            if (!snapshot_applied)
            {
                elog(WARNING, "Raft node %d: snapshot apply callback failed",
                     node->node_id);
            }
        }
        else
        {
            /* No callback, assume snapshot is applied */
            snapshot_applied = true;
        }

        if (snapshot_applied)
        {
            /* Discard log entries covered by snapshot */
            raft_log_compact(node->log, request->last_included_index);

            /* Update last_applied to reflect snapshot */
            if (request->last_included_index > node->log->last_applied)
                node->log->last_applied = request->last_included_index;

            /* Update commit_index if needed */
            if (request->last_included_index > node->log->commit_index)
                node->log->commit_index = request->last_included_index;

            elog(LOG, "Raft node %d applied snapshot at index %lu (term %lu)",
                 node->node_id, request->last_included_index,
                 request->last_included_term);
        }

        /* Clean up buffer */
        if (snapshot_buffer != NULL)
        {
            pfree(snapshot_buffer);
            snapshot_buffer = NULL;
        }
        snapshot_buffer_size = 0;
        snapshot_offset = 0;
    }

    return response;
}

/*
 * raft_send_install_snapshot - Send InstallSnapshot RPC to a lagging peer
 *
 * This is called when a follower is too far behind and needs a snapshot
 * instead of individual log entries. Returns true if the entire snapshot
 * was successfully transferred.
 */
bool
raft_send_install_snapshot(RaftNode *node, RaftPeer *peer)
{
    InstallSnapshotRequest request;
    InstallSnapshotResponse response;
    char *snapshot_data = NULL;
    int32 snapshot_size = 0;
    uint64 last_included_index = 0;
    uint64 last_included_term = 0;
    int32 offset = 0;
    int32 chunk_size = 64 * 1024;  /* 64KB chunks */
    bool success = false;

    if (!node->is_leader)
        return false;

    /* Load the latest snapshot */
    snapshot_data = raft_log_load_snapshot(node->log,
                                           &last_included_index,
                                           &last_included_term,
                                           &snapshot_size);

    if (snapshot_data == NULL && snapshot_size == 0 && last_included_index == 0)
    {
        /* No snapshot available */
        elog(WARNING, "Raft node %d: no snapshot available for peer %d",
             node->node_id, peer->node_id);
        return false;
    }

    elog(LOG, "Raft node %d sending snapshot to peer %d "
         "(index=%lu, term=%lu, size=%d bytes)",
         node->node_id, peer->node_id,
         last_included_index, last_included_term, snapshot_size);

    /* Send snapshot in chunks */
    while (offset < snapshot_size || (offset == 0 && snapshot_size == 0))
    {
        int32 this_chunk = Min(chunk_size, snapshot_size - offset);

        request.term = node->current_term;
        request.leader_id = node->node_id;
        request.last_included_index = last_included_index;
        request.last_included_term = last_included_term;
        request.offset = offset;
        request.data = (this_chunk > 0) ? (snapshot_data + offset) : NULL;
        request.data_size = this_chunk;
        request.done = (offset + this_chunk >= snapshot_size);

        /* Send chunk via RPC */
        response = raft_send_install_snapshot_rpc(peer, &request);

        /* Check for term mismatch */
        if (response.term > node->current_term)
        {
            raft_become_follower(node, response.term);
            success = false;
            break;
        }

        if (!response.success)
        {
            elog(WARNING, "Raft node %d: snapshot chunk to peer %d failed at offset %d",
                 node->node_id, peer->node_id, offset);
            success = false;
            break;
        }

        offset += this_chunk;
        peer->last_contact = GetCurrentTimestamp();

        /* Handle empty snapshot case */
        if (snapshot_size == 0)
            break;
    }

    /* If successful, update peer indices */
    if (offset >= snapshot_size)
    {
        peer->next_index = last_included_index + 1;
        peer->match_index = last_included_index;
        success = true;

        elog(LOG, "Raft node %d completed snapshot transfer to peer %d",
             node->node_id, peer->node_id);
    }

    if (snapshot_data != NULL)
        pfree(snapshot_data);

    return success;
}

/*
 * raft_send_install_snapshot_rpc - Send InstallSnapshot RPC via libpq
 */
InstallSnapshotResponse
raft_send_install_snapshot_rpc(RaftPeer *peer, InstallSnapshotRequest *request)
{
    InstallSnapshotResponse response;
    PGresult *res;
    char *escaped_data = NULL;
    char param_term[32];
    char param_leader_id[16];
    char param_last_idx[32];
    char param_last_term[32];
    char param_offset[16];
    char param_data_size[16];
    const char *paramValues[8];

    /* Initialize response with defaults */
    response.term = 0;
    response.node_id = peer->node_id;
    response.bytes_received = 0;
    response.success = false;

    if (peer->connection == NULL || PQstatus(peer->connection) != CONNECTION_OK)
    {
        peer->connection = raft_get_peer_connection(peer);
        if (peer->connection == NULL)
            return response;
    }

    /* Prepare parameters */
    snprintf(param_term, sizeof(param_term), "%lu", (unsigned long)request->term);
    snprintf(param_leader_id, sizeof(param_leader_id), "%d", request->leader_id);
    snprintf(param_last_idx, sizeof(param_last_idx), "%lu", (unsigned long)request->last_included_index);
    snprintf(param_last_term, sizeof(param_last_term), "%lu", (unsigned long)request->last_included_term);
    snprintf(param_offset, sizeof(param_offset), "%d", request->offset);
    snprintf(param_data_size, sizeof(param_data_size), "%d", request->data_size);

    if (request->data_size > 0 && request->data != NULL)
    {
        /* Escape binary data for SQL */
        size_t escaped_len;
        escaped_data = (char *) PQescapeByteaConn(peer->connection,
                                                   (unsigned char *) request->data,
                                                   request->data_size,
                                                   &escaped_len);

        paramValues[0] = param_term;
        paramValues[1] = param_leader_id;
        paramValues[2] = param_last_idx;
        paramValues[3] = param_last_term;
        paramValues[4] = param_offset;
        paramValues[5] = escaped_data;
        paramValues[6] = param_data_size;
        paramValues[7] = request->done ? "t" : "f";

        res = PQexecParams(peer->connection,
            "SELECT term, bytes_received, success "
            "FROM orochi.raft_install_snapshot("
            "$1::bigint, $2::integer, $3::bigint, $4::bigint, "
            "$5::integer, $6::bytea, $7::integer, $8::boolean)",
            8, NULL, paramValues, NULL, NULL, 0);

        PQfreemem(escaped_data);
    }
    else
    {
        paramValues[0] = param_term;
        paramValues[1] = param_leader_id;
        paramValues[2] = param_last_idx;
        paramValues[3] = param_last_term;
        paramValues[4] = param_offset;
        paramValues[5] = NULL;  /* NULL data */
        paramValues[6] = "0";
        paramValues[7] = request->done ? "t" : "f";

        res = PQexecParams(peer->connection,
            "SELECT term, bytes_received, success "
            "FROM orochi.raft_install_snapshot("
            "$1::bigint, $2::integer, $3::bigint, $4::bigint, "
            "$5::integer, $6::bytea, $7::integer, $8::boolean)",
            8, NULL, paramValues, NULL, NULL, 0);
    }

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        response.term = atoll(PQgetvalue(res, 0, 0));
        response.bytes_received = atoi(PQgetvalue(res, 0, 1));
        response.success = (PQgetvalue(res, 0, 2)[0] == 't');
        peer->last_contact = GetCurrentTimestamp();
    }
    else
    {
        elog(WARNING, "InstallSnapshot RPC to peer %d failed: %s",
             peer->node_id, PQerrorMessage(peer->connection));
        peer->is_active = false;
    }

    PQclear(res);
    return response;
}

/*
 * raft_set_snapshot_callback - Register callback for applying snapshots
 */
void
raft_set_snapshot_callback(RaftNode *node,
                           bool (*callback)(uint64 last_index, uint64 last_term,
                                            const char *data, int32 size,
                                            void *context),
                           void *context)
{
    node->snapshot_apply_callback = callback;
    node->snapshot_apply_context = context;
}

/*
 * raft_create_snapshot - Create a snapshot of the current state
 *
 * This should be called periodically by the application when it has
 * persisted its state. The callback provides the state machine data.
 */
bool
raft_create_snapshot(RaftNode *node,
                     char *(*get_state)(void *context, int32 *size),
                     void *context)
{
    char *snapshot_data;
    int32 snapshot_size;
    uint64 snapshot_index;

    /* Can only snapshot committed entries */
    snapshot_index = node->log->last_applied;

    if (snapshot_index < node->log->first_index)
    {
        elog(DEBUG1, "Raft node %d: nothing to snapshot (applied=%lu, first=%lu)",
             node->node_id, snapshot_index, node->log->first_index);
        return false;
    }

    /* Get state machine data from callback */
    snapshot_data = get_state(context, &snapshot_size);

    if (snapshot_data == NULL && snapshot_size > 0)
    {
        elog(WARNING, "Raft node %d: failed to get state for snapshot",
             node->node_id);
        return false;
    }

    /* Create the snapshot */
    raft_log_create_snapshot(node->log, snapshot_index,
                             snapshot_data, snapshot_size);

    if (snapshot_data != NULL)
        pfree(snapshot_data);

    elog(LOG, "Raft node %d created snapshot at index %lu (%d bytes)",
         node->node_id, snapshot_index, snapshot_size);

    return true;
}

/*
 * raft_should_snapshot - Check if we should create a snapshot
 */
bool
raft_should_snapshot(RaftNode *node)
{
    uint64 entries_since_first;

    if (node->log->last_applied < node->log->first_index)
        return false;

    entries_since_first = node->log->last_applied - node->log->first_index + 1;

    return entries_since_first >= RAFT_SNAPSHOT_THRESHOLD;
}

/* ============================================================
 * Follower Reads (Linearizable)
 * ============================================================ */

/*
 * raft_read_index - Request a read index for linearizable reads
 *
 * This implements the Raft read index optimization for linearizable reads.
 * For leaders: confirms leadership via heartbeat round, returns commit_index.
 * For followers: forwards request to leader and waits for response.
 *
 * Returns the read index if successful, 0 if failed.
 */
uint64
raft_read_index(RaftNode *node, int timeout_ms)
{
    uint64 read_index = 0;
    TimestampTz deadline;
    int i;
    int acks_needed;
    int acks_received;

    if (!node->running)
        return 0;

    deadline = GetCurrentTimestamp() + (timeout_ms * 1000L);

    if (node->is_leader)
    {
        /*
         * Leader path: To ensure linearizability, we need to confirm our
         * leadership is still valid by getting acknowledgment from a
         * majority of the cluster.
         */
        read_index = node->log->commit_index;

        /* If we're the only node, we're automatically confirmed */
        if (node->cluster->peer_count == 0)
            return read_index;

        /* Send heartbeats and count acknowledgments */
        acks_needed = node->cluster->quorum_size - 1;  /* Minus self */
        acks_received = 0;

        for (i = 0; i < node->cluster->peer_count; i++)
        {
            RaftPeer *peer = &node->cluster->peers[i];
            AppendEntriesRequest request;
            AppendEntriesResponse response;

            if (!peer->is_active)
                continue;

            /* Send empty AppendEntries (heartbeat) */
            request.term = node->current_term;
            request.leader_id = node->node_id;
            request.prev_log_index = peer->next_index - 1;
            request.prev_log_term = raft_log_term_at(node->log, request.prev_log_index);
            request.entries = NULL;
            request.entries_count = 0;
            request.leader_commit = node->log->commit_index;

            response = raft_send_append_entries(peer, &request);

            /* Check if we got demoted */
            if (response.term > node->current_term)
            {
                raft_become_follower(node, response.term);
                return 0;  /* No longer leader */
            }

            if (response.success)
            {
                acks_received++;
                peer->last_contact = GetCurrentTimestamp();
            }
        }

        /* Check if we got majority acknowledgment */
        if (acks_received >= acks_needed)
        {
            return read_index;
        }

        elog(DEBUG1, "Raft node %d: read_index failed, only %d/%d acks",
             node->node_id, acks_received, acks_needed);
        return 0;
    }
    else
    {
        /*
         * Follower path: Forward read request to leader and wait for
         * the leader to confirm its commit index. Then wait for our
         * state machine to catch up to that index.
         */
        if (node->leader_id < 0)
        {
            elog(DEBUG1, "Raft node %d: cannot perform read - no known leader",
                 node->node_id);
            return 0;
        }

        /* Find leader peer */
        for (i = 0; i < node->cluster->peer_count; i++)
        {
            if (node->cluster->peers[i].node_id == node->leader_id)
            {
                RaftPeer *leader = &node->cluster->peers[i];

                /* Request read index from leader */
                read_index = raft_request_read_index_from_leader(leader, node);

                if (read_index == 0)
                {
                    elog(DEBUG1, "Raft node %d: read_index request to leader failed",
                         node->node_id);
                    return 0;
                }

                break;
            }
        }

        /* Wait for our state machine to catch up */
        while (node->log->last_applied < read_index)
        {
            if (GetCurrentTimestamp() >= deadline)
            {
                elog(DEBUG1, "Raft node %d: read_index timeout waiting for apply",
                     node->node_id);
                return 0;
            }

            /* Drive state machine */
            raft_tick(node);
            pg_usleep(1000);  /* 1ms */
        }

        return read_index;
    }
}

/*
 * raft_request_read_index_from_leader - Send read index request to leader
 */
uint64
raft_request_read_index_from_leader(RaftPeer *leader, RaftNode *node)
{
    PGresult *res;
    uint64 read_index = 0;
    char param_node_id[16];
    const char *paramValues[1];

    if (leader->connection == NULL || PQstatus(leader->connection) != CONNECTION_OK)
    {
        leader->connection = raft_get_peer_connection(leader);
        if (leader->connection == NULL)
            return 0;
    }

    snprintf(param_node_id, sizeof(param_node_id), "%d", node->node_id);
    paramValues[0] = param_node_id;

    res = PQexecParams(leader->connection,
        "SELECT orochi.raft_leader_read_index($1::integer)",
        1, NULL, paramValues, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        const char *val = PQgetvalue(res, 0, 0);
        if (val != NULL && val[0] != '\0')
            read_index = atoll(val);
        leader->last_contact = GetCurrentTimestamp();
    }
    else
    {
        elog(WARNING, "Read index request to leader %d failed: %s",
             leader->node_id, PQerrorMessage(leader->connection));
    }

    PQclear(res);
    return read_index;
}

/*
 * raft_can_serve_read - Check if this node can serve a read at given index
 *
 * For followers to serve stale reads, they need to have applied up to
 * the requested index.
 */
bool
raft_can_serve_read(RaftNode *node, uint64 read_index)
{
    return node->log->last_applied >= read_index;
}

/*
 * raft_follower_read - Perform a bounded-stale read from a follower
 *
 * This allows reading from a follower with a bounded staleness guarantee.
 * The max_staleness_ms parameter specifies the maximum age of data.
 * Returns true if the read can proceed, false if too stale.
 */
bool
raft_follower_read(RaftNode *node, int max_staleness_ms)
{
    TimestampTz now = GetCurrentTimestamp();
    int64 staleness_us;

    /* Can always read from leader */
    if (node->is_leader)
        return true;

    /* Check how long since we last heard from leader */
    staleness_us = now - node->last_heartbeat;

    if (staleness_us > (int64)max_staleness_ms * 1000L)
    {
        elog(DEBUG1, "Raft node %d: follower read denied - too stale (%ld ms)",
             node->node_id, staleness_us / 1000);
        return false;
    }

    return true;
}

/*
 * raft_lease_read - Perform a lease-based read (for leader only)
 *
 * If the leader holds a valid lease (recently confirmed leadership),
 * it can serve reads without additional network round trips.
 * Returns the commit index if lease is valid, 0 otherwise.
 */
uint64
raft_lease_read(RaftNode *node)
{
    TimestampTz now;
    int64 lease_remaining_us;

    if (!node->is_leader)
        return 0;

    now = GetCurrentTimestamp();

    /* Lease is based on minimum election timeout */
    lease_remaining_us = node->election_timeout - now;

    /*
     * If we still have more than half the minimum election timeout remaining,
     * our lease is valid and we can serve reads without confirmation.
     */
    if (lease_remaining_us > (RAFT_ELECTION_TIMEOUT_MIN_MS * 500L))  /* Half of min */
    {
        return node->log->commit_index;
    }

    /* Lease expired, need to confirm leadership */
    return 0;
}

/* ============================================================
 * Client Interface
 * ============================================================ */

/*
 * raft_submit_command - Submit a command to be replicated
 */
uint64
raft_submit_command(RaftNode *node, const char *command, int32 command_size)
{
    RaftLogEntry entry;
    MemoryContext old_context;

    if (!node->is_leader)
    {
        elog(DEBUG1, "Raft node %d: cannot submit command - not leader",
             node->node_id);
        return 0;
    }

    old_context = MemoryContextSwitchTo(node->raft_context);

    /* Create new log entry */
    memset(&entry, 0, sizeof(RaftLogEntry));
    entry.term = node->current_term;
    entry.type = RAFT_LOG_COMMAND;
    entry.command_size = command_size;
    entry.command_data = palloc(command_size);
    memcpy(entry.command_data, command, command_size);
    entry.created_at = GetCurrentTimestamp();

    /* Append to local log */
    raft_log_append(node->log, &entry, 1);

    MemoryContextSwitchTo(old_context);

    elog(DEBUG1, "Raft node %d accepted command at index %lu",
         node->node_id, node->log->last_index);

    /* Trigger immediate replication */
    raft_replicate(node);

    return node->log->last_index;
}

/*
 * raft_is_committed - Check if a log entry has been committed
 */
bool
raft_is_committed(RaftNode *node, uint64 log_index)
{
    return log_index <= node->log->commit_index;
}

/*
 * raft_wait_for_commit - Wait for entry to be committed with timeout
 */
bool
raft_wait_for_commit(RaftNode *node, uint64 log_index, int timeout_ms)
{
    TimestampTz deadline = GetCurrentTimestamp() + (timeout_ms * 1000L);

    while (GetCurrentTimestamp() < deadline)
    {
        if (raft_is_committed(node, log_index))
            return true;

        /* Drive the state machine */
        raft_tick(node);

        /* Small sleep to avoid busy-waiting */
        pg_usleep(1000);  /* 1ms */
    }

    return false;
}

/*
 * raft_set_apply_callback - Register callback for state machine application
 */
void
raft_set_apply_callback(RaftNode *node,
                        void (*callback)(RaftLogEntry *entry, void *context),
                        void *context)
{
    node->apply_callback = callback;
    node->apply_context = context;
}

/* ============================================================
 * Cluster Management
 * ============================================================ */

/*
 * raft_add_peer - Add a peer to the cluster
 */
void
raft_add_peer(RaftNode *node, int32 peer_id, const char *hostname, int port)
{
    RaftPeer *peer;
    MemoryContext old_context;

    if (node->cluster->peer_count >= RAFT_MAX_CLUSTER_SIZE)
    {
        elog(ERROR, "Raft cluster at maximum size (%d)", RAFT_MAX_CLUSTER_SIZE);
        return;
    }

    old_context = MemoryContextSwitchTo(node->raft_context);

    peer = &node->cluster->peers[node->cluster->peer_count];
    peer->node_id = peer_id;
    peer->hostname = pstrdup(hostname);
    peer->port = port;
    peer->is_active = true;
    peer->next_index = node->log->last_index + 1;
    peer->match_index = 0;
    peer->last_contact = 0;
    peer->connection = NULL;
    peer->vote_granted = false;

    node->cluster->peer_count++;

    /* Update quorum size (majority): floor(total_nodes/2) + 1
     * where total_nodes = peer_count + 1 (including self) */
    node->cluster->quorum_size = (node->cluster->peer_count + 1) / 2 + 1;

    MemoryContextSwitchTo(old_context);

    elog(LOG, "Raft node %d added peer %d at %s:%d (quorum=%d)",
         node->node_id, peer_id, hostname, port, node->cluster->quorum_size);
}

/*
 * raft_remove_peer - Remove a peer from the cluster
 */
void
raft_remove_peer(RaftNode *node, int32 peer_id)
{
    int i, j;

    for (i = 0; i < node->cluster->peer_count; i++)
    {
        if (node->cluster->peers[i].node_id == peer_id)
        {
            /* Close connection if open */
            if (node->cluster->peers[i].connection != NULL)
            {
                PQfinish(node->cluster->peers[i].connection);
                node->cluster->peers[i].connection = NULL;
            }

            /* Free hostname */
            if (node->cluster->peers[i].hostname != NULL)
                pfree(node->cluster->peers[i].hostname);

            /* Shift remaining peers */
            for (j = i; j < node->cluster->peer_count - 1; j++)
            {
                node->cluster->peers[j] = node->cluster->peers[j + 1];
            }

            node->cluster->peer_count--;
            /* Update quorum size (majority): floor(total_nodes/2) + 1 */
            node->cluster->quorum_size = (node->cluster->peer_count + 1) / 2 + 1;

            elog(LOG, "Raft node %d removed peer %d (quorum=%d)",
                 node->node_id, peer_id, node->cluster->quorum_size);
            return;
        }
    }

    elog(WARNING, "Raft node %d: peer %d not found", node->node_id, peer_id);
}

/*
 * raft_connect_peers - Establish connections to all peers
 */
void
raft_connect_peers(RaftNode *node)
{
    int i;

    for (i = 0; i < node->cluster->peer_count; i++)
    {
        RaftPeer *peer = &node->cluster->peers[i];

        if (peer->connection == NULL || PQstatus(peer->connection) != CONNECTION_OK)
        {
            peer->connection = raft_get_peer_connection(peer);
            peer->is_active = (peer->connection != NULL);

            if (peer->is_active)
            {
                elog(DEBUG1, "Raft node %d connected to peer %d",
                     node->node_id, peer->node_id);
            }
            else
            {
                elog(WARNING, "Raft node %d failed to connect to peer %d",
                     node->node_id, peer->node_id);
            }
        }
    }
}

/*
 * raft_disconnect_peers - Close all peer connections
 */
void
raft_disconnect_peers(RaftNode *node)
{
    int i;

    for (i = 0; i < node->cluster->peer_count; i++)
    {
        RaftPeer *peer = &node->cluster->peers[i];

        if (peer->connection != NULL)
        {
            PQfinish(peer->connection);
            peer->connection = NULL;
        }
        peer->is_active = false;
    }
}

/*
 * raft_get_peer_connection - Get or create connection to peer
 */
static PGconn *
raft_get_peer_connection(RaftPeer *peer)
{
    StringInfoData connstr;
    PGconn *conn;

    initStringInfo(&connstr);
    appendStringInfo(&connstr,
        "host=%s port=%d dbname=%s connect_timeout=5",
        peer->hostname,
        peer->port,
        get_database_name(MyDatabaseId));

    conn = PQconnectdb(connstr.data);
    pfree(connstr.data);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        elog(WARNING, "Failed to connect to Raft peer %d at %s:%d: %s",
             peer->node_id, peer->hostname, peer->port, PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    return conn;
}

/* ============================================================
 * RPC Communication
 * ============================================================ */

/*
 * raft_send_request_vote - Send RequestVote RPC to peer
 *
 * Security: Uses parameterized queries (PQexecParams) to prevent SQL injection
 */
RequestVoteResponse
raft_send_request_vote(RaftPeer *peer, RequestVoteRequest *request)
{
    RequestVoteResponse response;
    PGresult *res;
    /* Parameter buffers for parameterized query */
    char param_term[32];
    char param_candidate_id[16];
    char param_last_log_index[32];
    char param_last_log_term[32];
    const char *paramValues[4];

    /* Initialize response with defaults (rejected) */
    response.term = 0;
    response.vote_granted = false;
    response.voter_id = peer->node_id;

    if (peer->connection == NULL || PQstatus(peer->connection) != CONNECTION_OK)
    {
        peer->connection = raft_get_peer_connection(peer);
        if (peer->connection == NULL)
            return response;
    }

    /* Convert numeric parameters to strings for PQexecParams */
    snprintf(param_term, sizeof(param_term), "%lu", (unsigned long)request->term);
    snprintf(param_candidate_id, sizeof(param_candidate_id), "%d", request->candidate_id);
    snprintf(param_last_log_index, sizeof(param_last_log_index), "%lu", (unsigned long)request->last_log_index);
    snprintf(param_last_log_term, sizeof(param_last_log_term), "%lu", (unsigned long)request->last_log_term);

    paramValues[0] = param_term;
    paramValues[1] = param_candidate_id;
    paramValues[2] = param_last_log_index;
    paramValues[3] = param_last_log_term;

    /* Security: Use parameterized query to prevent SQL injection */
    res = PQexecParams(peer->connection,
        "SELECT term, vote_granted FROM orochi.raft_request_vote("
        "$1::bigint, $2::integer, $3::bigint, $4::bigint)",
        4,          /* number of parameters */
        NULL,       /* let backend deduce param types */
        paramValues,
        NULL,       /* param lengths (not needed for text) */
        NULL,       /* param formats (0 = text) */
        0);         /* result format (0 = text) */

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        response.term = atoll(PQgetvalue(res, 0, 0));
        response.vote_granted = (PQgetvalue(res, 0, 1)[0] == 't');
    }
    else
    {
        elog(WARNING, "RequestVote RPC to peer %d failed: %s",
             peer->node_id, PQerrorMessage(peer->connection));
        peer->is_active = false;
    }

    PQclear(res);
    return response;
}

/*
 * raft_send_append_entries - Send AppendEntries RPC to peer
 *
 * Security: Uses parameterized queries (PQexecParams) to prevent SQL injection
 */
AppendEntriesResponse
raft_send_append_entries(RaftPeer *peer, AppendEntriesRequest *request)
{
    AppendEntriesResponse response;
    PGresult *res;
    /* Parameter buffers for parameterized query */
    char param_term[32];
    char param_leader_id[16];
    char param_prev_log_index[32];
    char param_prev_log_term[32];
    char param_leader_commit[32];

    /* Initialize response with defaults (failed) */
    response.term = 0;
    response.success = false;
    response.match_index = 0;
    response.node_id = peer->node_id;

    if (peer->connection == NULL || PQstatus(peer->connection) != CONNECTION_OK)
    {
        peer->connection = raft_get_peer_connection(peer);
        if (peer->connection == NULL)
            return response;
    }

    /* Convert numeric parameters to strings for PQexecParams */
    snprintf(param_term, sizeof(param_term), "%lu", (unsigned long)request->term);
    snprintf(param_leader_id, sizeof(param_leader_id), "%d", request->leader_id);
    snprintf(param_prev_log_index, sizeof(param_prev_log_index), "%lu", (unsigned long)request->prev_log_index);
    snprintf(param_prev_log_term, sizeof(param_prev_log_term), "%lu", (unsigned long)request->prev_log_term);
    snprintf(param_leader_commit, sizeof(param_leader_commit), "%lu", (unsigned long)request->leader_commit);

    if (request->entries_count == 0)
    {
        /* Heartbeat - no entries, use parameterized query with NULL */
        const char *paramValues[5] = {
            param_term, param_leader_id, param_prev_log_index,
            param_prev_log_term, param_leader_commit
        };

        res = PQexecParams(peer->connection,
            "SELECT term, success, match_index FROM orochi.raft_append_entries("
            "$1::bigint, $2::integer, $3::bigint, $4::bigint, NULL, $5::bigint)",
            5,          /* number of parameters */
            NULL,       /* let backend deduce param types */
            paramValues,
            NULL,       /* param lengths (not needed for text) */
            NULL,       /* param formats (0 = text) */
            0);         /* result format (0 = text) */
    }
    else
    {
        /* With entries - serialize as JSON array */
        StringInfoData entries_json;
        int i;
        const char *paramValues[6];

        initStringInfo(&entries_json);
        appendStringInfoChar(&entries_json, '[');

        for (i = 0; i < request->entries_count; i++)
        {
            RaftLogEntry *e = &request->entries[i];
            char *escaped_data;

            if (i > 0)
                appendStringInfoChar(&entries_json, ',');

            /* Security: Escape command_data to prevent JSON injection */
            escaped_data = escape_json_string(e->command_data);

            appendStringInfo(&entries_json,
                "{\"term\":%lu,\"type\":%d,\"data\":\"%s\"}",
                (unsigned long)e->term, e->type, escaped_data);

            pfree(escaped_data);
        }

        appendStringInfoChar(&entries_json, ']');

        /* Security: Use parameterized query to prevent SQL injection */
        paramValues[0] = param_term;
        paramValues[1] = param_leader_id;
        paramValues[2] = param_prev_log_index;
        paramValues[3] = param_prev_log_term;
        paramValues[4] = entries_json.data;
        paramValues[5] = param_leader_commit;

        res = PQexecParams(peer->connection,
            "SELECT term, success, match_index FROM orochi.raft_append_entries("
            "$1::bigint, $2::integer, $3::bigint, $4::bigint, $5::jsonb, $6::bigint)",
            6,          /* number of parameters */
            NULL,       /* let backend deduce param types */
            paramValues,
            NULL,       /* param lengths (not needed for text) */
            NULL,       /* param formats (0 = text) */
            0);         /* result format (0 = text) */

        pfree(entries_json.data);
    }

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        response.term = atoll(PQgetvalue(res, 0, 0));
        response.success = (PQgetvalue(res, 0, 1)[0] == 't');
        response.match_index = atoll(PQgetvalue(res, 0, 2));
        peer->last_contact = GetCurrentTimestamp();
    }
    else
    {
        elog(WARNING, "AppendEntries RPC to peer %d failed: %s",
             peer->node_id, PQerrorMessage(peer->connection));
        peer->is_active = false;
    }

    PQclear(res);
    return response;
}

/* ============================================================
 * State Persistence
 * ============================================================ */

/*
 * raft_persist_state - Save persistent state to disk
 */
void
raft_persist_state(RaftNode *node)
{
    StringInfoData path;
    FILE *fp;
    bool write_success = false;

    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_state_%d.dat",
                     DataDir, node->node_id);

    fp = fopen(path.data, "wb");
    if (fp != NULL)
    {
        /* Write both fields and check for errors */
        if (fwrite(&node->current_term, sizeof(uint64), 1, fp) == 1 &&
            fwrite(&node->voted_for, sizeof(int32), 1, fp) == 1)
        {
            /* Flush to ensure data is written */
            if (fflush(fp) == 0)
            {
                /* fsync the file descriptor for durability */
                if (fsync(fileno(fp)) == 0)
                {
                    write_success = true;
                }
                else
                {
                    elog(WARNING, "Failed to fsync Raft state file: %m");
                }
            }
            else
            {
                elog(WARNING, "Failed to flush Raft state file: %m");
            }
        }
        else
        {
            elog(WARNING, "Failed to write Raft state data: %m");
        }

        fclose(fp);

        if (write_success)
        {
            elog(DEBUG1, "Raft node %d persisted state (term=%lu, voted_for=%d)",
                 node->node_id, node->current_term, node->voted_for);
        }
    }
    else
    {
        elog(WARNING, "Failed to open Raft state file for writing: %s (%m)", path.data);
    }

    pfree(path.data);
}

/*
 * raft_load_state - Load persistent state from disk
 */
void
raft_load_state(RaftNode *node)
{
    StringInfoData path;
    FILE *fp;

    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_state_%d.dat",
                     DataDir, node->node_id);

    fp = fopen(path.data, "rb");
    if (fp != NULL)
    {
        if (fread(&node->current_term, sizeof(uint64), 1, fp) == 1 &&
            fread(&node->voted_for, sizeof(int32), 1, fp) == 1)
        {
            elog(LOG, "Raft node %d recovered state (term=%lu, voted_for=%d)",
                 node->node_id, node->current_term, node->voted_for);
        }
        fclose(fp);
    }

    pfree(path.data);
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * raft_get_leader - Get current leader node ID
 */
int32
raft_get_leader(RaftNode *node)
{
    return node->leader_id;
}

/*
 * raft_is_leader - Check if this node is the leader
 */
bool
raft_is_leader(RaftNode *node)
{
    return node->is_leader;
}

/*
 * raft_get_term - Get current term
 */
uint64
raft_get_term(RaftNode *node)
{
    return node->current_term;
}

/*
 * raft_state_name - Get string name for Raft state
 */
const char *
raft_state_name(RaftState state)
{
    switch (state)
    {
        case RAFT_STATE_FOLLOWER:
            return "follower";
        case RAFT_STATE_CANDIDATE:
            return "candidate";
        case RAFT_STATE_LEADER:
            return "leader";
        default:
            return "unknown";
    }
}

/*
 * raft_get_stats - Get node statistics
 */
void
raft_get_stats(RaftNode *node, uint64 *term, RaftState *state,
               int32 *leader_id, uint64 *commit_index, uint64 *last_applied)
{
    if (term != NULL)
        *term = node->current_term;
    if (state != NULL)
        *state = node->state;
    if (leader_id != NULL)
        *leader_id = node->leader_id;
    if (commit_index != NULL)
        *commit_index = node->log->commit_index;
    if (last_applied != NULL)
        *last_applied = node->log->last_applied;
}

/*
 * raft_random_election_timeout - Generate random election timeout
 */
int
raft_random_election_timeout(void)
{
    static bool seeded = false;

    if (!seeded)
    {
        srand((unsigned int) (GetCurrentTimestamp() ^ MyProcPid));
        seeded = true;
    }

    return RAFT_ELECTION_TIMEOUT_MIN_MS +
           (rand() % (RAFT_ELECTION_TIMEOUT_MAX_MS - RAFT_ELECTION_TIMEOUT_MIN_MS));
}

/* ============================================================
 * RPC Serialization (for binary protocol if needed)
 * ============================================================ */

/*
 * raft_serialize_request_vote - Serialize RequestVote for transmission
 */
char *
raft_serialize_request_vote(RequestVoteRequest *req, int32 *size)
{
    char *buffer;
    char *ptr;

    *size = sizeof(uint64) * 3 + sizeof(int32);
    buffer = palloc(*size);
    ptr = buffer;

    memcpy(ptr, &req->term, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(ptr, &req->candidate_id, sizeof(int32));
    ptr += sizeof(int32);
    memcpy(ptr, &req->last_log_index, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(ptr, &req->last_log_term, sizeof(uint64));

    return buffer;
}

/*
 * raft_deserialize_request_vote - Deserialize RequestVote
 */
RequestVoteRequest *
raft_deserialize_request_vote(const char *data, int32 size)
{
    RequestVoteRequest *req;
    const char *ptr = data;

    if (size < (int32)(sizeof(uint64) * 3 + sizeof(int32)))
        return NULL;

    req = palloc(sizeof(RequestVoteRequest));

    memcpy(&req->term, ptr, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(&req->candidate_id, ptr, sizeof(int32));
    ptr += sizeof(int32);
    memcpy(&req->last_log_index, ptr, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(&req->last_log_term, ptr, sizeof(uint64));

    return req;
}

/*
 * raft_serialize_append_entries - Serialize AppendEntries for transmission
 */
char *
raft_serialize_append_entries(AppendEntriesRequest *req, int32 *size)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);

    /* Fixed fields */
    appendBinaryStringInfo(&buf, (char *) &req->term, sizeof(uint64));
    appendBinaryStringInfo(&buf, (char *) &req->leader_id, sizeof(int32));
    appendBinaryStringInfo(&buf, (char *) &req->prev_log_index, sizeof(uint64));
    appendBinaryStringInfo(&buf, (char *) &req->prev_log_term, sizeof(uint64));
    appendBinaryStringInfo(&buf, (char *) &req->entries_count, sizeof(int32));
    appendBinaryStringInfo(&buf, (char *) &req->leader_commit, sizeof(uint64));

    /* Entries */
    for (i = 0; i < req->entries_count; i++)
    {
        RaftLogEntry *e = &req->entries[i];

        appendBinaryStringInfo(&buf, (char *) &e->index, sizeof(uint64));
        appendBinaryStringInfo(&buf, (char *) &e->term, sizeof(uint64));
        appendBinaryStringInfo(&buf, (char *) &e->type, sizeof(int32));
        appendBinaryStringInfo(&buf, (char *) &e->command_size, sizeof(int32));

        if (e->command_size > 0 && e->command_data != NULL)
            appendBinaryStringInfo(&buf, e->command_data, e->command_size);
    }

    *size = buf.len;
    return buf.data;
}

/*
 * raft_deserialize_append_entries - Deserialize AppendEntries
 */
AppendEntriesRequest *
raft_deserialize_append_entries(const char *data, int32 size)
{
    AppendEntriesRequest *req;
    const char *ptr = data;
    int i;

    req = palloc0(sizeof(AppendEntriesRequest));

    /* Fixed fields */
    memcpy(&req->term, ptr, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(&req->leader_id, ptr, sizeof(int32));
    ptr += sizeof(int32);
    memcpy(&req->prev_log_index, ptr, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(&req->prev_log_term, ptr, sizeof(uint64));
    ptr += sizeof(uint64);
    memcpy(&req->entries_count, ptr, sizeof(int32));
    ptr += sizeof(int32);
    memcpy(&req->leader_commit, ptr, sizeof(uint64));
    ptr += sizeof(uint64);

    /* Entries */
    if (req->entries_count > 0)
    {
        req->entries = palloc(sizeof(RaftLogEntry) * req->entries_count);

        for (i = 0; i < req->entries_count; i++)
        {
            RaftLogEntry *e = &req->entries[i];

            memcpy(&e->index, ptr, sizeof(uint64));
            ptr += sizeof(uint64);
            memcpy(&e->term, ptr, sizeof(uint64));
            ptr += sizeof(uint64);
            memcpy(&e->type, ptr, sizeof(int32));
            ptr += sizeof(int32);
            memcpy(&e->command_size, ptr, sizeof(int32));
            ptr += sizeof(int32);

            if (e->command_size > 0)
            {
                e->command_data = palloc(e->command_size);
                memcpy(e->command_data, ptr, e->command_size);
                ptr += e->command_size;
            }
            else
            {
                e->command_data = NULL;
            }
        }
    }

    return req;
}
