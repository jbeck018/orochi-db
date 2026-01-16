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

#include "raft.h"
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
 */
InstallSnapshotResponse
raft_install_snapshot(RaftNode *node, InstallSnapshotRequest *request)
{
    InstallSnapshotResponse response;

    response.term = node->current_term;
    response.node_id = node->node_id;

    /* Reject if term is stale */
    if (request->term < node->current_term)
        return response;

    /* Update term if needed */
    if (request->term > node->current_term)
        raft_become_follower(node, request->term);

    /* Reset election timer */
    raft_reset_election_timeout(node);
    node->leader_id = request->leader_id;

    /* TODO: Implement full snapshot handling:
     * 1. Create new snapshot file
     * 2. Write chunk data at offset
     * 3. If done, discard log up to last_included_index
     * 4. Reset state machine using snapshot
     */

    elog(DEBUG1, "Raft node %d received snapshot chunk (offset=%d, done=%d)",
         node->node_id, request->offset, request->done);

    if (request->done)
    {
        /* Compact log after snapshot */
        raft_log_compact(node->log, request->last_included_index);
    }

    return response;
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

    /* Update quorum size (majority) */
    node->cluster->quorum_size = (node->cluster->peer_count + 2) / 2;

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
            node->cluster->quorum_size = (node->cluster->peer_count + 2) / 2;

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
 */
RequestVoteResponse
raft_send_request_vote(RaftPeer *peer, RequestVoteRequest *request)
{
    RequestVoteResponse response;
    StringInfoData query;
    PGresult *res;

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

    /* Build SQL query to invoke Raft handler function */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT term, vote_granted FROM orochi.raft_request_vote(%lu, %d, %lu, %lu)",
        request->term, request->candidate_id,
        request->last_log_index, request->last_log_term);

    res = PQexec(peer->connection, query.data);
    pfree(query.data);

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
 */
AppendEntriesResponse
raft_send_append_entries(RaftPeer *peer, AppendEntriesRequest *request)
{
    AppendEntriesResponse response;
    StringInfoData query;
    PGresult *res;

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

    /* Build SQL query - for heartbeats (no entries), use simpler form */
    initStringInfo(&query);

    if (request->entries_count == 0)
    {
        /* Heartbeat - no entries */
        appendStringInfo(&query,
            "SELECT term, success, match_index FROM orochi.raft_append_entries("
            "%lu, %d, %lu, %lu, NULL, %lu)",
            request->term, request->leader_id,
            request->prev_log_index, request->prev_log_term,
            request->leader_commit);
    }
    else
    {
        /* With entries - serialize as JSON array */
        StringInfoData entries_json;
        int i;

        initStringInfo(&entries_json);
        appendStringInfoChar(&entries_json, '[');

        for (i = 0; i < request->entries_count; i++)
        {
            RaftLogEntry *e = &request->entries[i];

            if (i > 0)
                appendStringInfoChar(&entries_json, ',');

            appendStringInfo(&entries_json,
                "{\"term\":%lu,\"type\":%d,\"data\":\"%s\"}",
                e->term, e->type,
                e->command_data ? e->command_data : "");
        }

        appendStringInfoChar(&entries_json, ']');

        appendStringInfo(&query,
            "SELECT term, success, match_index FROM orochi.raft_append_entries("
            "%lu, %d, %lu, %lu, '%s'::jsonb, %lu)",
            request->term, request->leader_id,
            request->prev_log_index, request->prev_log_term,
            entries_json.data, request->leader_commit);

        pfree(entries_json.data);
    }

    res = PQexec(peer->connection, query.data);
    pfree(query.data);

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

    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_state_%d.dat",
                     DataDir, node->node_id);

    fp = fopen(path.data, "wb");
    if (fp != NULL)
    {
        fwrite(&node->current_term, sizeof(uint64), 1, fp);
        fwrite(&node->voted_for, sizeof(int32), 1, fp);
        fclose(fp);

        elog(DEBUG1, "Raft node %d persisted state (term=%lu, voted_for=%d)",
             node->node_id, node->current_term, node->voted_for);
    }
    else
    {
        elog(WARNING, "Failed to persist Raft state: %s", path.data);
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
