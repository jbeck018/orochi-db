/*-------------------------------------------------------------------------
 *
 * test_raft.c
 *    Unit tests for Orochi DB Raft consensus implementation
 *
 * This is a standalone test file that can be compiled and run without
 * PostgreSQL. It mocks the necessary PostgreSQL types and functions.
 *
 * Tests covered:
 *   - State transitions (FOLLOWER -> CANDIDATE -> LEADER)
 *   - Log entry creation and comparison
 *   - Vote request/response handling
 *   - Quorum calculations
 *   - Log operations (append, get, truncate)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L  /* For strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>

/* ============================================================
 * Simple Test Framework
 * ============================================================ */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Running %s... ", #name); \
    fflush(stdout); \
    tests_run++; \
    test_##name(); \
    tests_passed++; \
    printf("PASSED\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAILED\n    Expected %lld, got %lld\n    at %s:%d\n", \
               (long long)(b), (long long)(a), __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAILED\n    Expected '%s', got '%s'\n    at %s:%d\n", \
               (b), (a), __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("FAILED\n    Expected non-NULL pointer\n    at %s:%d\n", \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("FAILED\n    Expected NULL pointer\n    at %s:%d\n", \
               __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ============================================================
 * Mock PostgreSQL Types and Functions
 * ============================================================ */

/* Basic types */
typedef int64_t TimestampTz;
typedef void *MemoryContext;
typedef void *LWLock;
typedef void *PGconn;

/* Mock memory context */
static void *mock_memory_context = (void *)0x12345678;
#define TopMemoryContext mock_memory_context

/* Mock functions */
#define palloc(size) malloc(size)
#define palloc0(size) calloc(1, size)
#define pfree(ptr) free(ptr)
#define repalloc(ptr, size) realloc(ptr, size)
#define pstrdup(str) strdup(str)

#define MemoryContextSwitchTo(ctx) mock_memory_context
#define MemoryContextDelete(ctx) ((void)0)
#define AllocSetContextCreate(parent, name, sizes) mock_memory_context
#define ALLOCSET_DEFAULT_SIZES 0

static TimestampTz mock_timestamp = 1000000;
#define GetCurrentTimestamp() (mock_timestamp++)
#define pg_usleep(usec) ((void)0)

/* Mock logging */
#define LOG 0
#define DEBUG1 1
#define WARNING 2
#define ERROR 3
#define elog(level, ...) ((void)0)

/* Mock DataDir */
static char *DataDir = "/tmp/test_raft";

/* Mock database functions */
#define MyProcPid 12345
#define MyDatabaseId 1
#define get_database_name(dbid) "testdb"

/* StringInfo mock */
typedef struct StringInfoData {
    char *data;
    int len;
    int maxlen;
    int cursor;
} StringInfoData;
typedef StringInfoData *StringInfo;

static void initStringInfo(StringInfo str)
{
    str->maxlen = 256;
    str->data = (char *)malloc(str->maxlen);
    str->data[0] = '\0';
    str->len = 0;
    str->cursor = 0;
}

static void appendStringInfo(StringInfo str, const char *fmt, ...)
{
    va_list args;
    char buf[1024];
    int len;

    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (str->len + len >= str->maxlen) {
        str->maxlen = str->maxlen * 2 + len;
        str->data = (char *)realloc(str->data, str->maxlen);
    }
    strcpy(str->data + str->len, buf);
    str->len += len;
}

static void appendStringInfoChar(StringInfo str, char c)
{
    if (str->len + 1 >= str->maxlen) {
        str->maxlen *= 2;
        str->data = (char *)realloc(str->data, str->maxlen);
    }
    str->data[str->len++] = c;
    str->data[str->len] = '\0';
}

static void appendBinaryStringInfo(StringInfo str, const char *data, int datalen)
{
    if (str->len + datalen >= str->maxlen) {
        str->maxlen = str->maxlen * 2 + datalen;
        str->data = (char *)realloc(str->data, str->maxlen);
    }
    memcpy(str->data + str->len, data, datalen);
    str->len += datalen;
}

/* Min/Max macros */
#ifndef Min
#define Min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef Max
#define Max(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ============================================================
 * Raft Types (copied from raft.h for standalone compilation)
 * ============================================================ */

#define RAFT_ELECTION_TIMEOUT_MIN_MS    150
#define RAFT_ELECTION_TIMEOUT_MAX_MS    300
#define RAFT_HEARTBEAT_INTERVAL_MS      50
#define RAFT_RPC_TIMEOUT_MS             100
#define RAFT_MAX_LOG_ENTRIES            10000
#define RAFT_MAX_ENTRIES_PER_APPEND     100
#define RAFT_SNAPSHOT_THRESHOLD         1000
#define RAFT_MAX_CLUSTER_SIZE           32

typedef enum RaftState
{
    RAFT_STATE_FOLLOWER = 0,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} RaftState;

typedef enum RaftLogEntryType
{
    RAFT_LOG_COMMAND = 0,
    RAFT_LOG_CONFIG_CHANGE,
    RAFT_LOG_NOOP
} RaftLogEntryType;

typedef struct RaftLogEntry
{
    uint64_t            index;
    uint64_t            term;
    RaftLogEntryType    type;
    int32_t             command_size;
    char               *command_data;
    TimestampTz         created_at;
} RaftLogEntry;

typedef struct RaftLog
{
    RaftLogEntry       *entries;
    uint64_t            first_index;
    uint64_t            last_index;
    uint64_t            commit_index;
    uint64_t            last_applied;
    int32_t             capacity;
    LWLock             *lock;
    char               *wal_path;
    bool                persistence_enabled;
} RaftLog;

typedef struct RequestVoteRequest
{
    uint64_t            term;
    int32_t             candidate_id;
    uint64_t            last_log_index;
    uint64_t            last_log_term;
} RequestVoteRequest;

typedef struct RequestVoteResponse
{
    uint64_t            term;
    bool                vote_granted;
    int32_t             voter_id;
} RequestVoteResponse;

typedef struct RaftPeer
{
    int32_t             node_id;
    char               *hostname;
    int                 port;
    bool                is_active;
    uint64_t            next_index;
    uint64_t            match_index;
    TimestampTz         last_contact;
    PGconn             *connection;
    bool                vote_granted;
} RaftPeer;

typedef struct RaftCluster
{
    RaftPeer           *peers;
    int32_t             peer_count;
    int32_t             quorum_size;
    int32_t             my_node_id;
    LWLock             *lock;
} RaftCluster;

typedef struct RaftNode
{
    uint64_t            current_term;
    int32_t             voted_for;
    RaftLog            *log;
    RaftState           state;
    int32_t             node_id;
    int32_t             leader_id;
    RaftCluster        *cluster;
    TimestampTz         election_timeout;
    TimestampTz         last_heartbeat;
    bool                is_leader;
    void              (*apply_callback)(RaftLogEntry *entry, void *context);
    void               *apply_context;
    uint64_t            elections_started;
    uint64_t            elections_won;
    uint64_t            append_entries_sent;
    uint64_t            votes_received;
    MemoryContext       raft_context;
    LWLock             *state_lock;
    bool                initialized;
    bool                running;
} RaftNode;

/* ============================================================
 * Helper Functions for Testing
 * ============================================================ */

/*
 * Create a minimal Raft log for testing
 */
static RaftLog *create_test_log(void)
{
    RaftLog *log = (RaftLog *)calloc(1, sizeof(RaftLog));

    log->first_index = 1;
    log->last_index = 0;
    log->commit_index = 0;
    log->last_applied = 0;
    log->capacity = 1024;
    log->entries = (RaftLogEntry *)calloc(log->capacity, sizeof(RaftLogEntry));
    log->persistence_enabled = false;
    log->wal_path = NULL;
    log->lock = NULL;

    return log;
}

/*
 * Free a test log
 */
static void free_test_log(RaftLog *log)
{
    if (log == NULL)
        return;

    if (log->last_index >= log->first_index) {
        for (uint64_t i = 0; i <= log->last_index - log->first_index; i++) {
            if (log->entries[i].command_data != NULL)
                free(log->entries[i].command_data);
        }
    }

    if (log->entries != NULL)
        free(log->entries);

    free(log);
}

/*
 * Create a minimal Raft node for testing
 */
static RaftNode *create_test_node(int32_t node_id)
{
    RaftNode *node = (RaftNode *)calloc(1, sizeof(RaftNode));

    node->current_term = 0;
    node->voted_for = -1;
    node->log = create_test_log();
    node->state = RAFT_STATE_FOLLOWER;
    node->node_id = node_id;
    node->leader_id = -1;
    node->is_leader = false;

    node->cluster = (RaftCluster *)calloc(1, sizeof(RaftCluster));
    node->cluster->peers = (RaftPeer *)calloc(RAFT_MAX_CLUSTER_SIZE, sizeof(RaftPeer));
    node->cluster->peer_count = 0;
    node->cluster->quorum_size = 1;
    node->cluster->my_node_id = node_id;

    node->elections_started = 0;
    node->elections_won = 0;
    node->votes_received = 0;
    node->initialized = true;
    node->running = true;

    return node;
}

/*
 * Free a test node
 */
static void free_test_node(RaftNode *node)
{
    if (node == NULL)
        return;

    free_test_log(node->log);

    if (node->cluster != NULL) {
        for (int i = 0; i < node->cluster->peer_count; i++) {
            if (node->cluster->peers[i].hostname != NULL)
                free(node->cluster->peers[i].hostname);
        }
        if (node->cluster->peers != NULL)
            free(node->cluster->peers);
        free(node->cluster);
    }

    free(node);
}

/*
 * Append an entry to the test log
 */
static bool test_log_append(RaftLog *log, uint64_t term, RaftLogEntryType type,
                            const char *data, int32_t size)
{
    uint64_t next_index = log->last_index + 1;
    uint64_t array_index = next_index - log->first_index;
    RaftLogEntry *entry;

    if (array_index >= (uint64_t)log->capacity) {
        log->capacity *= 2;
        log->entries = (RaftLogEntry *)realloc(log->entries,
                                                sizeof(RaftLogEntry) * log->capacity);
    }

    entry = &log->entries[array_index];
    entry->index = next_index;
    entry->term = term;
    entry->type = type;
    entry->created_at = GetCurrentTimestamp();

    if (data != NULL && size > 0) {
        entry->command_size = size;
        entry->command_data = (char *)malloc(size);
        memcpy(entry->command_data, data, size);
    } else {
        entry->command_size = 0;
        entry->command_data = NULL;
    }

    log->last_index = next_index;
    return true;
}

/*
 * Get entry at index from test log
 */
static RaftLogEntry *test_log_get(RaftLog *log, uint64_t index)
{
    if (index < log->first_index || index > log->last_index)
        return NULL;

    return &log->entries[index - log->first_index];
}

/*
 * Get term at index from test log
 */
static uint64_t test_log_term_at(RaftLog *log, uint64_t index)
{
    if (index == 0)
        return 0;

    RaftLogEntry *entry = test_log_get(log, index);
    if (entry == NULL)
        return 0;

    return entry->term;
}

/*
 * Truncate log after given index
 */
static void helper_log_truncate(RaftLog *log, uint64_t after_index)
{
    if (after_index >= log->last_index)
        return;

    for (uint64_t i = after_index + 1; i <= log->last_index; i++) {
        uint64_t array_index = i - log->first_index;
        if (log->entries[array_index].command_data != NULL) {
            free(log->entries[array_index].command_data);
            log->entries[array_index].command_data = NULL;
        }
    }

    log->last_index = after_index;

    if (log->commit_index > log->last_index)
        log->commit_index = log->last_index;
    if (log->last_applied > log->last_index)
        log->last_applied = log->last_index;
}

/*
 * Add a peer to the cluster with correct quorum calculation
 */
static void test_add_peer(RaftNode *node, int32_t peer_id,
                          const char *hostname, int port)
{
    RaftPeer *peer = &node->cluster->peers[node->cluster->peer_count];

    peer->node_id = peer_id;
    peer->hostname = strdup(hostname);
    peer->port = port;
    peer->is_active = true;
    peer->next_index = node->log->last_index + 1;
    peer->match_index = 0;
    peer->vote_granted = false;
    peer->connection = NULL;

    node->cluster->peer_count++;

    /* Correct quorum calculation: floor(total_nodes/2) + 1
     * where total_nodes = peer_count + 1 (including self) */
    node->cluster->quorum_size = (node->cluster->peer_count + 1) / 2 + 1;
}

/*
 * Check if candidate's log is at least as up-to-date as ours
 */
static bool test_is_log_up_to_date(RaftNode *node, uint64_t last_log_index,
                                   uint64_t last_log_term)
{
    uint64_t my_last_term = test_log_term_at(node->log, node->log->last_index);
    uint64_t my_last_index = node->log->last_index;

    /* Compare terms first */
    if (last_log_term != my_last_term)
        return last_log_term > my_last_term;

    /* Same term, compare indices */
    return last_log_index >= my_last_index;
}

/*
 * Simulate state transition to candidate
 */
static void test_become_candidate(RaftNode *node)
{
    node->state = RAFT_STATE_CANDIDATE;
    node->is_leader = false;
    node->current_term++;
    node->voted_for = node->node_id;
    node->votes_received = 1;  /* Vote for self */
    node->leader_id = -1;

    for (int i = 0; i < node->cluster->peer_count; i++) {
        node->cluster->peers[i].vote_granted = false;
    }
}

/*
 * Simulate state transition to leader
 */
static void test_become_leader(RaftNode *node)
{
    node->state = RAFT_STATE_LEADER;
    node->is_leader = true;
    node->leader_id = node->node_id;
    node->elections_won++;

    /* Initialize next_index and match_index for all peers */
    for (int i = 0; i < node->cluster->peer_count; i++) {
        node->cluster->peers[i].next_index = node->log->last_index + 1;
        node->cluster->peers[i].match_index = 0;
    }
}

/*
 * Simulate state transition to follower
 */
static void test_become_follower(RaftNode *node, uint64_t term)
{
    node->state = RAFT_STATE_FOLLOWER;
    node->is_leader = false;
    node->current_term = term;
    node->voted_for = -1;
}

/*
 * Handle a vote request
 */
static RequestVoteResponse test_request_vote(RaftNode *node,
                                             RequestVoteRequest *request)
{
    RequestVoteResponse response;

    response.term = node->current_term;
    response.vote_granted = false;
    response.voter_id = node->node_id;

    /* Reject if term is stale */
    if (request->term < node->current_term)
        return response;

    /* Step down if we see a higher term */
    if (request->term > node->current_term)
        test_become_follower(node, request->term);

    response.term = node->current_term;

    /* Check if we can vote for this candidate */
    if ((node->voted_for == -1 || node->voted_for == request->candidate_id) &&
        test_is_log_up_to_date(node, request->last_log_index, request->last_log_term))
    {
        node->voted_for = request->candidate_id;
        response.vote_granted = true;
    }

    return response;
}

/*
 * Get state name as string
 */
static const char *test_state_name(RaftState state)
{
    switch (state) {
        case RAFT_STATE_FOLLOWER:  return "follower";
        case RAFT_STATE_CANDIDATE: return "candidate";
        case RAFT_STATE_LEADER:    return "leader";
        default:                   return "unknown";
    }
}

/* ============================================================
 * Test Cases: State Transitions
 * ============================================================ */

TEST(state_initial_follower)
{
    RaftNode *node = create_test_node(1);

    ASSERT_EQ(node->state, RAFT_STATE_FOLLOWER);
    ASSERT(!node->is_leader);
    ASSERT_EQ(node->voted_for, -1);
    ASSERT_EQ(node->leader_id, -1);

    free_test_node(node);
}

TEST(state_follower_to_candidate)
{
    RaftNode *node = create_test_node(1);

    ASSERT_EQ(node->state, RAFT_STATE_FOLLOWER);
    ASSERT_EQ(node->current_term, 0);

    test_become_candidate(node);

    ASSERT_EQ(node->state, RAFT_STATE_CANDIDATE);
    ASSERT_EQ(node->current_term, 1);
    ASSERT_EQ(node->voted_for, node->node_id);
    ASSERT_EQ(node->votes_received, 1);  /* Self vote */
    ASSERT(!node->is_leader);

    free_test_node(node);
}

TEST(state_candidate_to_leader)
{
    RaftNode *node = create_test_node(1);

    test_become_candidate(node);
    ASSERT_EQ(node->state, RAFT_STATE_CANDIDATE);

    /* Simulate receiving majority votes */
    node->votes_received = node->cluster->quorum_size;
    test_become_leader(node);

    ASSERT_EQ(node->state, RAFT_STATE_LEADER);
    ASSERT(node->is_leader);
    ASSERT_EQ(node->leader_id, node->node_id);
    ASSERT_EQ(node->elections_won, 1);

    free_test_node(node);
}

TEST(state_candidate_to_follower_higher_term)
{
    RaftNode *node = create_test_node(1);

    test_become_candidate(node);
    ASSERT_EQ(node->current_term, 1);

    /* Receive a message with higher term */
    test_become_follower(node, 5);

    ASSERT_EQ(node->state, RAFT_STATE_FOLLOWER);
    ASSERT_EQ(node->current_term, 5);
    ASSERT_EQ(node->voted_for, -1);
    ASSERT(!node->is_leader);

    free_test_node(node);
}

TEST(state_leader_to_follower_higher_term)
{
    RaftNode *node = create_test_node(1);

    test_become_candidate(node);
    test_become_leader(node);
    ASSERT(node->is_leader);

    /* Discover higher term */
    test_become_follower(node, 10);

    ASSERT_EQ(node->state, RAFT_STATE_FOLLOWER);
    ASSERT_EQ(node->current_term, 10);
    ASSERT(!node->is_leader);

    free_test_node(node);
}

TEST(state_name_strings)
{
    ASSERT_STR_EQ(test_state_name(RAFT_STATE_FOLLOWER), "follower");
    ASSERT_STR_EQ(test_state_name(RAFT_STATE_CANDIDATE), "candidate");
    ASSERT_STR_EQ(test_state_name(RAFT_STATE_LEADER), "leader");
}

/* ============================================================
 * Test Cases: Log Operations
 * ============================================================ */

TEST(log_init_empty)
{
    RaftLog *log = create_test_log();

    ASSERT_EQ(log->first_index, 1);
    ASSERT_EQ(log->last_index, 0);
    ASSERT_EQ(log->commit_index, 0);
    ASSERT_EQ(log->last_applied, 0);
    ASSERT_NOT_NULL(log->entries);

    free_test_log(log);
}

TEST(log_append_single_entry)
{
    RaftLog *log = create_test_log();

    bool result = test_log_append(log, 1, RAFT_LOG_COMMAND, "test", 4);

    ASSERT(result);
    ASSERT_EQ(log->last_index, 1);

    RaftLogEntry *entry = test_log_get(log, 1);
    ASSERT_NOT_NULL(entry);
    ASSERT_EQ(entry->index, 1);
    ASSERT_EQ(entry->term, 1);
    ASSERT_EQ(entry->type, RAFT_LOG_COMMAND);
    ASSERT_EQ(entry->command_size, 4);

    free_test_log(log);
}

TEST(log_append_multiple_entries)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd2", 4);
    test_log_append(log, 2, RAFT_LOG_COMMAND, "cmd3", 4);

    ASSERT_EQ(log->last_index, 3);

    ASSERT_EQ(test_log_term_at(log, 1), 1);
    ASSERT_EQ(test_log_term_at(log, 2), 1);
    ASSERT_EQ(test_log_term_at(log, 3), 2);

    free_test_log(log);
}

TEST(log_get_out_of_bounds)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, "test", 4);

    ASSERT_NOT_NULL(test_log_get(log, 1));
    ASSERT_NULL(test_log_get(log, 0));
    ASSERT_NULL(test_log_get(log, 2));

    free_test_log(log);
}

TEST(log_term_at_index_zero)
{
    RaftLog *log = create_test_log();

    /* Index 0 should return term 0 by convention */
    ASSERT_EQ(test_log_term_at(log, 0), 0);

    free_test_log(log);
}

TEST(log_truncate)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd2", 4);
    test_log_append(log, 2, RAFT_LOG_COMMAND, "cmd3", 4);
    test_log_append(log, 2, RAFT_LOG_COMMAND, "cmd4", 4);

    ASSERT_EQ(log->last_index, 4);

    helper_log_truncate(log, 2);

    ASSERT_EQ(log->last_index, 2);
    ASSERT_NOT_NULL(test_log_get(log, 1));
    ASSERT_NOT_NULL(test_log_get(log, 2));
    ASSERT_NULL(test_log_get(log, 3));
    ASSERT_NULL(test_log_get(log, 4));

    free_test_log(log);
}

TEST(log_truncate_updates_commit_index)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd2", 4);
    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd3", 4);

    log->commit_index = 3;
    log->last_applied = 3;

    helper_log_truncate(log, 1);

    ASSERT_EQ(log->commit_index, 1);
    ASSERT_EQ(log->last_applied, 1);

    free_test_log(log);
}

TEST(log_noop_entry)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_NOOP, NULL, 0);

    RaftLogEntry *entry = test_log_get(log, 1);
    ASSERT_NOT_NULL(entry);
    ASSERT_EQ(entry->type, RAFT_LOG_NOOP);
    ASSERT_EQ(entry->command_size, 0);
    ASSERT_NULL(entry->command_data);

    free_test_log(log);
}

/* ============================================================
 * Test Cases: Vote Request/Response
 * ============================================================ */

TEST(vote_grant_to_first_candidate)
{
    RaftNode *node = create_test_node(1);

    RequestVoteRequest request = {
        .term = 1,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response = test_request_vote(node, &request);

    ASSERT(response.vote_granted);
    ASSERT_EQ(response.term, 1);
    ASSERT_EQ(node->voted_for, 2);
    ASSERT_EQ(node->current_term, 1);

    free_test_node(node);
}

TEST(vote_reject_stale_term)
{
    RaftNode *node = create_test_node(1);
    node->current_term = 5;

    RequestVoteRequest request = {
        .term = 3,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response = test_request_vote(node, &request);

    ASSERT(!response.vote_granted);
    ASSERT_EQ(response.term, 5);
    ASSERT_EQ(node->voted_for, -1);  /* Did not vote */

    free_test_node(node);
}

TEST(vote_reject_already_voted)
{
    RaftNode *node = create_test_node(1);

    /* First vote request */
    RequestVoteRequest request1 = {
        .term = 1,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response1 = test_request_vote(node, &request1);
    ASSERT(response1.vote_granted);

    /* Second vote request from different candidate */
    RequestVoteRequest request2 = {
        .term = 1,
        .candidate_id = 3,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response2 = test_request_vote(node, &request2);
    ASSERT(!response2.vote_granted);
    ASSERT_EQ(node->voted_for, 2);  /* Still voted for first candidate */

    free_test_node(node);
}

TEST(vote_allow_revote_for_same_candidate)
{
    RaftNode *node = create_test_node(1);

    /* First vote request */
    RequestVoteRequest request1 = {
        .term = 1,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    test_request_vote(node, &request1);
    ASSERT_EQ(node->voted_for, 2);

    /* Same candidate asks again */
    RequestVoteRequest request2 = {
        .term = 1,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response2 = test_request_vote(node, &request2);
    ASSERT(response2.vote_granted);

    free_test_node(node);
}

TEST(vote_reject_outdated_log_by_term)
{
    RaftNode *node = create_test_node(1);

    /* Add some entries to our log with term 5 */
    test_log_append(node->log, 5, RAFT_LOG_COMMAND, "cmd", 3);

    /* Candidate has older term in log */
    RequestVoteRequest request = {
        .term = 6,
        .candidate_id = 2,
        .last_log_index = 1,
        .last_log_term = 3  /* Older term than our log */
    };

    RequestVoteResponse response = test_request_vote(node, &request);
    ASSERT(!response.vote_granted);

    free_test_node(node);
}

TEST(vote_reject_outdated_log_by_index)
{
    RaftNode *node = create_test_node(1);

    /* Add multiple entries */
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd2", 4);
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd3", 4);

    /* Candidate has same term but shorter log */
    RequestVoteRequest request = {
        .term = 2,
        .candidate_id = 2,
        .last_log_index = 1,  /* We have 3 entries */
        .last_log_term = 1
    };

    RequestVoteResponse response = test_request_vote(node, &request);
    ASSERT(!response.vote_granted);

    free_test_node(node);
}

TEST(vote_accept_more_uptodate_log)
{
    RaftNode *node = create_test_node(1);

    /* Add entry with term 1 */
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd", 3);

    /* Candidate has higher term in log */
    RequestVoteRequest request = {
        .term = 2,
        .candidate_id = 2,
        .last_log_index = 1,
        .last_log_term = 2  /* Higher term than our log */
    };

    RequestVoteResponse response = test_request_vote(node, &request);
    ASSERT(response.vote_granted);

    free_test_node(node);
}

TEST(vote_updates_term_on_higher_term)
{
    RaftNode *node = create_test_node(1);
    node->current_term = 3;

    RequestVoteRequest request = {
        .term = 10,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response = test_request_vote(node, &request);

    ASSERT_EQ(node->current_term, 10);
    ASSERT_EQ(node->state, RAFT_STATE_FOLLOWER);

    free_test_node(node);
}

/* ============================================================
 * Test Cases: Quorum Calculations
 * ============================================================ */

TEST(quorum_single_node)
{
    RaftNode *node = create_test_node(1);

    /* Single node cluster needs quorum of 1 */
    ASSERT_EQ(node->cluster->quorum_size, 1);

    free_test_node(node);
}

TEST(quorum_two_nodes)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);

    /* 2 nodes: need majority of 2 */
    ASSERT_EQ(node->cluster->peer_count, 1);
    ASSERT_EQ(node->cluster->quorum_size, 2);

    free_test_node(node);
}

TEST(quorum_three_nodes)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);

    /* 3 nodes: need majority of 2 */
    ASSERT_EQ(node->cluster->peer_count, 2);
    ASSERT_EQ(node->cluster->quorum_size, 2);

    free_test_node(node);
}

TEST(quorum_four_nodes)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);
    test_add_peer(node, 4, "peer4", 5434);

    /* 4 nodes: need majority of 3 */
    ASSERT_EQ(node->cluster->peer_count, 3);
    ASSERT_EQ(node->cluster->quorum_size, 3);

    free_test_node(node);
}

TEST(quorum_five_nodes)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);
    test_add_peer(node, 4, "peer4", 5434);
    test_add_peer(node, 5, "peer5", 5435);

    /* 5 nodes: need majority of 3 */
    ASSERT_EQ(node->cluster->peer_count, 4);
    ASSERT_EQ(node->cluster->quorum_size, 3);

    free_test_node(node);
}

TEST(quorum_large_cluster)
{
    RaftNode *node = create_test_node(1);

    /* Add 10 peers (11 total nodes) */
    for (int i = 2; i <= 11; i++) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "peer%d", i);
        test_add_peer(node, i, hostname, 5430 + i);
    }

    /* 11 nodes: need majority of 6 */
    ASSERT_EQ(node->cluster->peer_count, 10);
    ASSERT_EQ(node->cluster->quorum_size, 6);

    free_test_node(node);
}

/* ============================================================
 * Test Cases: Leader Election Simulation
 * ============================================================ */

TEST(election_self_vote)
{
    RaftNode *node = create_test_node(1);

    test_become_candidate(node);

    /* Candidate always votes for itself */
    ASSERT_EQ(node->voted_for, node->node_id);
    ASSERT_EQ(node->votes_received, 1);

    free_test_node(node);
}

TEST(election_win_single_node)
{
    RaftNode *node = create_test_node(1);

    test_become_candidate(node);

    /* Single node wins immediately with self vote */
    ASSERT_EQ(node->votes_received, 1);
    ASSERT(node->votes_received >= node->cluster->quorum_size);

    test_become_leader(node);
    ASSERT(node->is_leader);

    free_test_node(node);
}

TEST(election_win_three_node_cluster)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);

    ASSERT_EQ(node->cluster->quorum_size, 2);

    test_become_candidate(node);
    ASSERT_EQ(node->votes_received, 1);  /* Self vote */

    /* Simulate receiving one more vote */
    node->cluster->peers[0].vote_granted = true;
    node->votes_received = 2;

    ASSERT(node->votes_received >= node->cluster->quorum_size);

    test_become_leader(node);
    ASSERT(node->is_leader);

    free_test_node(node);
}

TEST(election_lose_three_node_cluster)
{
    RaftNode *node = create_test_node(1);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);

    test_become_candidate(node);

    /* Only have self vote, not enough for quorum */
    ASSERT_EQ(node->votes_received, 1);
    ASSERT(node->votes_received < node->cluster->quorum_size);

    /* Should still be candidate, not leader */
    ASSERT_EQ(node->state, RAFT_STATE_CANDIDATE);
    ASSERT(!node->is_leader);

    free_test_node(node);
}

TEST(election_leader_initializes_peer_indices)
{
    RaftNode *node = create_test_node(1);

    /* Add some log entries */
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd2", 4);

    test_add_peer(node, 2, "peer2", 5432);
    test_add_peer(node, 3, "peer3", 5433);

    test_become_candidate(node);
    node->votes_received = 2;
    test_become_leader(node);

    /* Leader should initialize next_index to last_log_index + 1 */
    for (int i = 0; i < node->cluster->peer_count; i++) {
        ASSERT_EQ(node->cluster->peers[i].next_index, node->log->last_index + 1);
        ASSERT_EQ(node->cluster->peers[i].match_index, 0);
    }

    free_test_node(node);
}

/* ============================================================
 * Test Cases: Log Entry Comparison
 * ============================================================ */

TEST(log_comparison_empty_logs)
{
    RaftNode *node = create_test_node(1);

    /* Both logs empty - candidate should be up-to-date */
    ASSERT(test_is_log_up_to_date(node, 0, 0));

    free_test_node(node);
}

TEST(log_comparison_candidate_has_higher_term)
{
    RaftNode *node = create_test_node(1);

    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd", 3);

    /* Candidate has higher term */
    ASSERT(test_is_log_up_to_date(node, 1, 5));

    free_test_node(node);
}

TEST(log_comparison_candidate_has_lower_term)
{
    RaftNode *node = create_test_node(1);

    test_log_append(node->log, 5, RAFT_LOG_COMMAND, "cmd", 3);

    /* Candidate has lower term */
    ASSERT(!test_is_log_up_to_date(node, 1, 3));

    free_test_node(node);
}

TEST(log_comparison_same_term_longer_log)
{
    RaftNode *node = create_test_node(1);

    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd", 3);

    /* Same term, candidate has longer log */
    ASSERT(test_is_log_up_to_date(node, 5, 1));

    free_test_node(node);
}

TEST(log_comparison_same_term_shorter_log)
{
    RaftNode *node = create_test_node(1);

    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd1", 4);
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd2", 4);
    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd3", 4);

    /* Same term, candidate has shorter log */
    ASSERT(!test_is_log_up_to_date(node, 1, 1));

    free_test_node(node);
}

TEST(log_comparison_same_term_equal_length)
{
    RaftNode *node = create_test_node(1);

    test_log_append(node->log, 1, RAFT_LOG_COMMAND, "cmd", 3);

    /* Same term, same length - candidate is up-to-date */
    ASSERT(test_is_log_up_to_date(node, 1, 1));

    free_test_node(node);
}

/* ============================================================
 * Test Cases: Edge Cases and Boundary Conditions
 * ============================================================ */

TEST(edge_empty_command_data)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, NULL, 0);

    RaftLogEntry *entry = test_log_get(log, 1);
    ASSERT_NOT_NULL(entry);
    ASSERT_EQ(entry->command_size, 0);
    ASSERT_NULL(entry->command_data);

    free_test_log(log);
}

TEST(edge_truncate_empty_log)
{
    RaftLog *log = create_test_log();

    /* Truncating empty log should be safe */
    helper_log_truncate(log, 0);

    ASSERT_EQ(log->last_index, 0);

    free_test_log(log);
}

TEST(edge_truncate_beyond_end)
{
    RaftLog *log = create_test_log();

    test_log_append(log, 1, RAFT_LOG_COMMAND, "cmd", 3);

    /* Truncating beyond end should be no-op */
    helper_log_truncate(log, 100);

    ASSERT_EQ(log->last_index, 1);

    free_test_log(log);
}

TEST(edge_term_zero)
{
    RaftNode *node = create_test_node(1);

    /* Term 0 is valid initial state */
    ASSERT_EQ(node->current_term, 0);

    /* Vote request with term 0 should be rejected if we're at higher term */
    node->current_term = 1;

    RequestVoteRequest request = {
        .term = 0,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0
    };

    RequestVoteResponse response = test_request_vote(node, &request);
    ASSERT(!response.vote_granted);

    free_test_node(node);
}

TEST(edge_multiple_term_increments)
{
    RaftNode *node = create_test_node(1);

    for (int i = 0; i < 100; i++) {
        test_become_candidate(node);
        test_become_follower(node, node->current_term);
    }

    ASSERT_EQ(node->current_term, 100);

    free_test_node(node);
}

/* ============================================================
 * Main Test Runner
 * ============================================================ */

int main(int argc, char *argv[])
{
    printf("\n========================================\n");
    printf("Orochi DB Raft Consensus Unit Tests\n");
    printf("========================================\n\n");

    /* State Transition Tests */
    printf("State Transition Tests:\n");
    RUN_TEST(state_initial_follower);
    RUN_TEST(state_follower_to_candidate);
    RUN_TEST(state_candidate_to_leader);
    RUN_TEST(state_candidate_to_follower_higher_term);
    RUN_TEST(state_leader_to_follower_higher_term);
    RUN_TEST(state_name_strings);
    printf("\n");

    /* Log Operation Tests */
    printf("Log Operation Tests:\n");
    RUN_TEST(log_init_empty);
    RUN_TEST(log_append_single_entry);
    RUN_TEST(log_append_multiple_entries);
    RUN_TEST(log_get_out_of_bounds);
    RUN_TEST(log_term_at_index_zero);
    RUN_TEST(log_truncate);
    RUN_TEST(log_truncate_updates_commit_index);
    RUN_TEST(log_noop_entry);
    printf("\n");

    /* Vote Request/Response Tests */
    printf("Vote Request/Response Tests:\n");
    RUN_TEST(vote_grant_to_first_candidate);
    RUN_TEST(vote_reject_stale_term);
    RUN_TEST(vote_reject_already_voted);
    RUN_TEST(vote_allow_revote_for_same_candidate);
    RUN_TEST(vote_reject_outdated_log_by_term);
    RUN_TEST(vote_reject_outdated_log_by_index);
    RUN_TEST(vote_accept_more_uptodate_log);
    RUN_TEST(vote_updates_term_on_higher_term);
    printf("\n");

    /* Quorum Calculation Tests */
    printf("Quorum Calculation Tests:\n");
    RUN_TEST(quorum_single_node);
    RUN_TEST(quorum_two_nodes);
    RUN_TEST(quorum_three_nodes);
    RUN_TEST(quorum_four_nodes);
    RUN_TEST(quorum_five_nodes);
    RUN_TEST(quorum_large_cluster);
    printf("\n");

    /* Leader Election Tests */
    printf("Leader Election Tests:\n");
    RUN_TEST(election_self_vote);
    RUN_TEST(election_win_single_node);
    RUN_TEST(election_win_three_node_cluster);
    RUN_TEST(election_lose_three_node_cluster);
    RUN_TEST(election_leader_initializes_peer_indices);
    printf("\n");

    /* Log Comparison Tests */
    printf("Log Comparison Tests:\n");
    RUN_TEST(log_comparison_empty_logs);
    RUN_TEST(log_comparison_candidate_has_higher_term);
    RUN_TEST(log_comparison_candidate_has_lower_term);
    RUN_TEST(log_comparison_same_term_longer_log);
    RUN_TEST(log_comparison_same_term_shorter_log);
    RUN_TEST(log_comparison_same_term_equal_length);
    printf("\n");

    /* Edge Case Tests */
    printf("Edge Case Tests:\n");
    RUN_TEST(edge_empty_command_data);
    RUN_TEST(edge_truncate_empty_log);
    RUN_TEST(edge_truncate_beyond_end);
    RUN_TEST(edge_term_zero);
    RUN_TEST(edge_multiple_term_increments);
    printf("\n");

    /* Summary */
    printf("========================================\n");
    printf("Test Results:\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
