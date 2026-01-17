/*-------------------------------------------------------------------------
 *
 * test_pipelines.c
 *    Unit tests for Orochi DB data pipeline implementation
 *
 * This test file provides standalone testing without requiring an actual
 * Kafka broker. It uses mock implementations to test:
 *   - Pipeline state machine transitions
 *   - Message parsing (JSON, CSV, Line Protocol)
 *   - Error handling paths
 *   - Memory management
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

/* ============================================================
 * Standalone Test Framework
 * ============================================================ */

#define TEST_PASSED     0
#define TEST_FAILED     1
#define TEST_SKIPPED    2

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "  FAIL: %s (line %d)\n", message, __LINE__); \
            return TEST_FAILED; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(expected, actual, message) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "  FAIL: %s - expected %d, got %d (line %d)\n", \
                    message, (int)(expected), (int)(actual), __LINE__); \
            return TEST_FAILED; \
        } \
    } while(0)

#define TEST_ASSERT_STR_EQ(expected, actual, message) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            fprintf(stderr, "  FAIL: %s - expected '%s', got '%s' (line %d)\n", \
                    message, (expected), (actual), __LINE__); \
            return TEST_FAILED; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr, message) \
    do { \
        if ((ptr) != NULL) { \
            fprintf(stderr, "  FAIL: %s - expected NULL (line %d)\n", message, __LINE__); \
            return TEST_FAILED; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(ptr, message) \
    do { \
        if ((ptr) == NULL) { \
            fprintf(stderr, "  FAIL: %s - expected non-NULL (line %d)\n", message, __LINE__); \
            return TEST_FAILED; \
        } \
    } while(0)

#define RUN_TEST(test_func) \
    do { \
        printf("Running %s...\n", #test_func); \
        tests_run++; \
        int result = test_func(); \
        if (result == TEST_PASSED) { \
            printf("  PASSED\n"); \
            tests_passed++; \
        } else if (result == TEST_SKIPPED) { \
            printf("  SKIPPED\n"); \
        } else { \
            tests_failed++; \
        } \
    } while(0)

/* ============================================================
 * Mock Types (matching pipeline.h definitions)
 * ============================================================ */

typedef enum PipelineSource {
    PIPELINE_SOURCE_KAFKA = 0,
    PIPELINE_SOURCE_S3 = 1,
    PIPELINE_SOURCE_FILESYSTEM = 2,
    PIPELINE_SOURCE_HTTP = 3,
    PIPELINE_SOURCE_KINESIS = 4
} PipelineSource;

typedef enum PipelineState {
    PIPELINE_STATE_CREATED = 0,
    PIPELINE_STATE_RUNNING = 1,
    PIPELINE_STATE_PAUSED = 2,
    PIPELINE_STATE_STOPPED = 3,
    PIPELINE_STATE_ERROR = 4,
    PIPELINE_STATE_DRAINING = 5
} PipelineState;

typedef enum PipelineFormat {
    PIPELINE_FORMAT_JSON = 0,
    PIPELINE_FORMAT_CSV = 1,
    PIPELINE_FORMAT_AVRO = 2,
    PIPELINE_FORMAT_PARQUET = 3,
    PIPELINE_FORMAT_LINE = 4
} PipelineFormat;

typedef enum PipelineDelivery {
    PIPELINE_DELIVERY_AT_LEAST_ONCE = 0,
    PIPELINE_DELIVERY_AT_MOST_ONCE = 1,
    PIPELINE_DELIVERY_EXACTLY_ONCE = 2
} PipelineDelivery;

/* ============================================================
 * Mock Kafka Consumer
 * ============================================================ */

typedef struct MockKafkaMessage {
    char *payload;
    size_t len;
    int64_t offset;
    int32_t partition;
    int error_code;
} MockKafkaMessage;

typedef struct MockKafkaConsumer {
    bool is_initialized;
    bool is_subscribed;
    int64_t current_offset;
    int32_t current_partition;
    int64_t committed_offset;
    int64_t messages_received;
    int64_t messages_committed;
    int64_t errors;
    int consecutive_errors;

    /* Mock message queue */
    MockKafkaMessage *mock_messages;
    int num_mock_messages;
    int next_message_idx;

    /* Configuration */
    char *bootstrap_servers;
    char *topic;
    char *consumer_group;
} MockKafkaConsumer;

/* Mock consumer implementation */
static MockKafkaConsumer *mock_kafka_create(const char *servers, const char *topic, const char *group)
{
    MockKafkaConsumer *consumer = calloc(1, sizeof(MockKafkaConsumer));
    if (!consumer) return NULL;

    consumer->bootstrap_servers = strdup(servers);
    consumer->topic = strdup(topic);
    consumer->consumer_group = group ? strdup(group) : NULL;
    consumer->is_initialized = true;
    consumer->is_subscribed = true;
    consumer->current_offset = 0;
    consumer->current_partition = 0;
    consumer->committed_offset = -1;

    return consumer;
}

static void mock_kafka_destroy(MockKafkaConsumer *consumer)
{
    if (!consumer) return;
    free(consumer->bootstrap_servers);
    free(consumer->topic);
    free(consumer->consumer_group);
    if (consumer->mock_messages) {
        for (int i = 0; i < consumer->num_mock_messages; i++) {
            free(consumer->mock_messages[i].payload);
        }
        free(consumer->mock_messages);
    }
    free(consumer);
}

static void mock_kafka_add_message(MockKafkaConsumer *consumer, const char *payload, int64_t offset)
{
    consumer->num_mock_messages++;
    consumer->mock_messages = realloc(consumer->mock_messages,
                                       consumer->num_mock_messages * sizeof(MockKafkaMessage));

    MockKafkaMessage *msg = &consumer->mock_messages[consumer->num_mock_messages - 1];
    msg->payload = strdup(payload);
    msg->len = strlen(payload);
    msg->offset = offset;
    msg->partition = 0;
    msg->error_code = 0;
}

static void mock_kafka_add_error_message(MockKafkaConsumer *consumer, int error_code)
{
    consumer->num_mock_messages++;
    consumer->mock_messages = realloc(consumer->mock_messages,
                                       consumer->num_mock_messages * sizeof(MockKafkaMessage));

    MockKafkaMessage *msg = &consumer->mock_messages[consumer->num_mock_messages - 1];
    msg->payload = NULL;
    msg->len = 0;
    msg->offset = -1;
    msg->partition = 0;
    msg->error_code = error_code;
}

static int mock_kafka_poll(MockKafkaConsumer *consumer, char **messages, int max_messages)
{
    if (!consumer || !consumer->is_initialized) return -1;
    if (!messages || max_messages <= 0) return 0;

    int count = 0;
    while (count < max_messages && consumer->next_message_idx < consumer->num_mock_messages)
    {
        MockKafkaMessage *msg = &consumer->mock_messages[consumer->next_message_idx];

        if (msg->error_code != 0) {
            consumer->errors++;
            consumer->consecutive_errors++;
            consumer->next_message_idx++;
            if (consumer->consecutive_errors > 10) return -1;
            continue;
        }

        if (msg->payload && msg->len > 0) {
            messages[count] = malloc(msg->len + 1);
            memcpy(messages[count], msg->payload, msg->len);
            messages[count][msg->len] = '\0';
            count++;

            consumer->current_offset = msg->offset;
            consumer->messages_received++;
            consumer->consecutive_errors = 0;
        }

        consumer->next_message_idx++;
    }

    return count;
}

static bool mock_kafka_commit(MockKafkaConsumer *consumer, int64_t offset)
{
    if (!consumer || !consumer->is_initialized) return false;
    consumer->committed_offset = offset;
    consumer->messages_committed++;
    return true;
}

static bool mock_kafka_seek(MockKafkaConsumer *consumer, int64_t offset)
{
    if (!consumer || !consumer->is_initialized) return false;
    consumer->current_offset = offset;
    /* Find message index for this offset */
    for (int i = 0; i < consumer->num_mock_messages; i++) {
        if (consumer->mock_messages[i].offset >= offset) {
            consumer->next_message_idx = i;
            break;
        }
    }
    return true;
}

/* ============================================================
 * State Machine Implementation (for testing)
 * ============================================================ */

typedef struct PipelineStateMachine {
    PipelineState current_state;
    int64_t pipeline_id;
    int transition_count;
} PipelineStateMachine;

/* Valid state transitions table */
static bool is_valid_transition(PipelineState from, PipelineState to)
{
    switch (from) {
        case PIPELINE_STATE_CREATED:
            return (to == PIPELINE_STATE_RUNNING || to == PIPELINE_STATE_STOPPED);

        case PIPELINE_STATE_RUNNING:
            return (to == PIPELINE_STATE_PAUSED ||
                    to == PIPELINE_STATE_STOPPED ||
                    to == PIPELINE_STATE_ERROR ||
                    to == PIPELINE_STATE_DRAINING);

        case PIPELINE_STATE_PAUSED:
            return (to == PIPELINE_STATE_RUNNING || to == PIPELINE_STATE_STOPPED);

        case PIPELINE_STATE_STOPPED:
            return (to == PIPELINE_STATE_RUNNING);  /* Restart allowed */

        case PIPELINE_STATE_ERROR:
            return (to == PIPELINE_STATE_RUNNING || to == PIPELINE_STATE_STOPPED);

        case PIPELINE_STATE_DRAINING:
            return (to == PIPELINE_STATE_STOPPED);

        default:
            return false;
    }
}

static bool state_machine_transition(PipelineStateMachine *sm, PipelineState new_state)
{
    if (!is_valid_transition(sm->current_state, new_state)) {
        return false;
    }
    sm->current_state = new_state;
    sm->transition_count++;
    return true;
}

static const char *state_to_string(PipelineState state)
{
    switch (state) {
        case PIPELINE_STATE_CREATED:  return "created";
        case PIPELINE_STATE_RUNNING:  return "running";
        case PIPELINE_STATE_PAUSED:   return "paused";
        case PIPELINE_STATE_STOPPED:  return "stopped";
        case PIPELINE_STATE_ERROR:    return "error";
        case PIPELINE_STATE_DRAINING: return "draining";
        default:                      return "unknown";
    }
}

/* ============================================================
 * Message Parsing Implementation (standalone)
 * ============================================================ */

typedef struct ParsedRecord {
    char *json_str;
    struct ParsedRecord *next;
} ParsedRecord;

static void free_records(ParsedRecord *records)
{
    while (records) {
        ParsedRecord *next = records->next;
        free(records->json_str);
        free(records);
        records = next;
    }
}

static int count_records(ParsedRecord *records)
{
    int count = 0;
    while (records) {
        count++;
        records = records->next;
    }
    return count;
}

/* JSON parsing - handles single objects, arrays, and NDJSON */
static ParsedRecord *parse_json_test(const char *data, size_t size)
{
    ParsedRecord *head = NULL;
    ParsedRecord *tail = NULL;

    if (!data || size == 0) return NULL;

    /* Trim leading whitespace */
    while (size > 0 && (*data == ' ' || *data == '\n' || *data == '\r' || *data == '\t')) {
        data++;
        size--;
    }

    if (size == 0) return NULL;

    if (data[0] == '[') {
        /* Simple array parsing (for testing, not full JSON parser) */
        /* Skip [ */
        const char *p = data + 1;
        const char *end = data + size;

        while (p < end) {
            /* Skip whitespace and commas */
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ','))
                p++;

            if (p >= end || *p == ']') break;

            if (*p == '{') {
                /* Find matching } */
                const char *obj_start = p;
                int depth = 1;
                p++;
                while (p < end && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }

                size_t obj_len = p - obj_start;
                ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
                rec->json_str = malloc(obj_len + 1);
                memcpy(rec->json_str, obj_start, obj_len);
                rec->json_str[obj_len] = '\0';

                if (!head) head = rec;
                if (tail) tail->next = rec;
                tail = rec;
            }
        }
    }
    else if (data[0] == '{') {
        /* Check if it's NDJSON (has newlines) or a single object */
        bool has_newline = (memchr(data, '\n', size) != NULL);

        if (!has_newline) {
            /* Single object - no newlines */
            ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
            rec->json_str = malloc(size + 1);
            memcpy(rec->json_str, data, size);
            rec->json_str[size] = '\0';
            head = rec;
        }
        else {
            /* NDJSON - newline delimited */
            const char *line_start = data;
            const char *p = data;
            const char *end = data + size;

            while (p <= end) {
                bool is_end = (p == end);
                bool is_newline = (p < end && *p == '\n');

                if (is_newline || is_end) {
                    size_t line_len = p - line_start;

                    /* Skip leading whitespace */
                    while (line_len > 0 && (line_start[0] == ' ' || line_start[0] == '\t')) {
                        line_start++;
                        line_len--;
                    }

                    /* Skip trailing whitespace */
                    while (line_len > 0 && (line_start[line_len-1] == ' ' ||
                           line_start[line_len-1] == '\t' || line_start[line_len-1] == '\r')) {
                        line_len--;
                    }

                    if (line_len > 0 && line_start[0] == '{') {
                        ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
                        rec->json_str = malloc(line_len + 1);
                        memcpy(rec->json_str, line_start, line_len);
                        rec->json_str[line_len] = '\0';

                        if (!head) head = rec;
                        if (tail) tail->next = rec;
                        tail = rec;
                    }

                    line_start = p + 1;
                }
                if (is_end) break;
                p++;
            }
        }
    }
    else {
        /* Not JSON - return empty */
    }

    return head;
}

/* CSV parsing */
static ParsedRecord *parse_csv_test(const char *data, size_t size, char delimiter, bool has_header)
{
    ParsedRecord *head = NULL;
    ParsedRecord *tail = NULL;
    char **headers = NULL;
    int num_headers = 0;
    bool first_line = true;

    if (!data || size == 0) return NULL;

    const char *line_start = data;
    const char *p = data;
    const char *end = data + size;

    while (p <= end) {
        if (*p == '\n' || *p == '\r' || p == end) {
            size_t line_len = p - line_start;

            if (line_len > 0) {
                char *line = malloc(line_len + 1);
                memcpy(line, line_start, line_len);
                line[line_len] = '\0';

                if (first_line && has_header) {
                    /* Count and store headers */
                    num_headers = 1;
                    for (size_t i = 0; i < line_len; i++) {
                        if (line[i] == delimiter) num_headers++;
                    }

                    headers = calloc(num_headers, sizeof(char *));
                    char *saveptr;
                    char delim_str[2] = {delimiter, '\0'};
                    char *token = strtok_r(line, delim_str, &saveptr);
                    int idx = 0;
                    while (token && idx < num_headers) {
                        headers[idx++] = strdup(token);
                        token = strtok_r(NULL, delim_str, &saveptr);
                    }
                    first_line = false;
                }
                else {
                    /* Build JSON from CSV row */
                    char json[4096] = "{";
                    char delim_str[2] = {delimiter, '\0'};
                    char *saveptr;
                    char *line_copy = strdup(line);
                    char *token = strtok_r(line_copy, delim_str, &saveptr);
                    int col_idx = 0;

                    while (token) {
                        if (col_idx > 0) strcat(json, ", ");

                        char field[256];
                        if (headers && col_idx < num_headers) {
                            snprintf(field, sizeof(field), "\"%s\": ", headers[col_idx]);
                        } else {
                            snprintf(field, sizeof(field), "\"col%d\": ", col_idx);
                        }
                        strcat(json, field);

                        /* Check if numeric */
                        bool is_numeric = true;
                        for (char *tp = token; *tp; tp++) {
                            if (*tp != '-' && *tp != '+' && *tp != '.' && (*tp < '0' || *tp > '9')) {
                                is_numeric = false;
                                break;
                            }
                        }

                        if (is_numeric && strlen(token) > 0) {
                            strcat(json, token);
                        } else {
                            strcat(json, "\"");
                            strcat(json, token);
                            strcat(json, "\"");
                        }

                        token = strtok_r(NULL, delim_str, &saveptr);
                        col_idx++;
                    }
                    strcat(json, "}");

                    ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
                    rec->json_str = strdup(json);

                    if (!head) head = rec;
                    if (tail) tail->next = rec;
                    tail = rec;

                    free(line_copy);
                }

                free(line);
            }

            /* Skip \r\n */
            if (*p == '\r' && p + 1 < end && *(p + 1) == '\n')
                p++;

            line_start = p + 1;
        }
        if (p == end) break;
        p++;
    }

    /* Free headers */
    if (headers) {
        for (int i = 0; i < num_headers; i++) {
            free(headers[i]);
        }
        free(headers);
    }

    return head;
}

/* Line protocol parsing */
static ParsedRecord *parse_line_protocol_test(const char *data, size_t size)
{
    ParsedRecord *head = NULL;
    ParsedRecord *tail = NULL;

    if (!data || size == 0) return NULL;

    const char *line_start = data;
    const char *p = data;
    const char *end = data + size;

    while (p <= end) {
        if (*p == '\n' || p == end) {
            size_t line_len = p - line_start;

            if (line_len > 0 && line_start[0] != '#') {  /* Skip comments */
                char *line = malloc(line_len + 1);
                memcpy(line, line_start, line_len);
                line[line_len] = '\0';

                /* Parse: measurement,tag=value field=value timestamp */
                char json[4096] = "{";
                char *measurement = NULL;
                char *tags_str = NULL;
                char *fields_str = NULL;
                char *timestamp_str = NULL;

                char *space1 = strchr(line, ' ');
                if (space1) {
                    *space1 = '\0';

                    char *comma = strchr(line, ',');
                    if (comma) {
                        *comma = '\0';
                        measurement = line;
                        tags_str = comma + 1;
                    } else {
                        measurement = line;
                    }

                    fields_str = space1 + 1;
                    char *space2 = strchr(fields_str, ' ');
                    if (space2) {
                        *space2 = '\0';
                        timestamp_str = space2 + 1;
                    }
                }

                if (measurement) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "\"_measurement\": \"%s\"", measurement);
                    strcat(json, buf);
                }

                /* Parse tags */
                if (tags_str && strlen(tags_str) > 0) {
                    char *tags_copy = strdup(tags_str);
                    char *saveptr;
                    char *tag = strtok_r(tags_copy, ",", &saveptr);
                    while (tag) {
                        char *eq = strchr(tag, '=');
                        if (eq) {
                            *eq = '\0';
                            char buf[256];
                            snprintf(buf, sizeof(buf), ", \"%s\": \"%s\"", tag, eq + 1);
                            strcat(json, buf);
                        }
                        tag = strtok_r(NULL, ",", &saveptr);
                    }
                    free(tags_copy);
                }

                /* Parse fields */
                if (fields_str && strlen(fields_str) > 0) {
                    char *fields_copy = strdup(fields_str);
                    char *saveptr;
                    char *field = strtok_r(fields_copy, ",", &saveptr);
                    while (field) {
                        char *eq = strchr(field, '=');
                        if (eq) {
                            char *value = eq + 1;
                            *eq = '\0';

                            int val_len = strlen(value);
                            char buf[256];

                            if (val_len > 0 && value[val_len - 1] == 'i') {
                                /* Integer */
                                value[val_len - 1] = '\0';
                                snprintf(buf, sizeof(buf), ", \"%s\": %s", field, value);
                            } else if (value[0] == '"' && value[val_len - 1] == '"') {
                                /* String */
                                snprintf(buf, sizeof(buf), ", \"%s\": %s", field, value);
                            } else if (strcmp(value, "true") == 0 || strcmp(value, "t") == 0) {
                                snprintf(buf, sizeof(buf), ", \"%s\": true", field);
                            } else if (strcmp(value, "false") == 0 || strcmp(value, "f") == 0) {
                                snprintf(buf, sizeof(buf), ", \"%s\": false", field);
                            } else {
                                /* Float */
                                snprintf(buf, sizeof(buf), ", \"%s\": %s", field, value);
                            }
                            strcat(json, buf);
                        }
                        field = strtok_r(NULL, ",", &saveptr);
                    }
                    free(fields_copy);
                }

                /* Add timestamp */
                if (timestamp_str && strlen(timestamp_str) > 0) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), ", \"_time\": %s", timestamp_str);
                    strcat(json, buf);
                }

                strcat(json, "}");

                ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
                rec->json_str = strdup(json);

                if (!head) head = rec;
                if (tail) tail->next = rec;
                tail = rec;

                free(line);
            }

            line_start = p + 1;
        }
        if (p == end) break;
        p++;
    }

    return head;
}

/* ============================================================
 * Test Cases - State Machine Transitions
 * ============================================================ */

static int test_state_machine_initial_state(void)
{
    PipelineStateMachine sm = {
        .current_state = PIPELINE_STATE_CREATED,
        .pipeline_id = 1,
        .transition_count = 0
    };

    TEST_ASSERT_EQ(PIPELINE_STATE_CREATED, sm.current_state, "Initial state should be CREATED");
    TEST_ASSERT_STR_EQ("created", state_to_string(sm.current_state), "State string mismatch");

    return TEST_PASSED;
}

static int test_state_machine_valid_transitions(void)
{
    PipelineStateMachine sm = {
        .current_state = PIPELINE_STATE_CREATED,
        .pipeline_id = 1,
        .transition_count = 0
    };

    /* CREATED -> RUNNING */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_RUNNING),
                "CREATED -> RUNNING should be valid");
    TEST_ASSERT_EQ(PIPELINE_STATE_RUNNING, sm.current_state, "State should be RUNNING");

    /* RUNNING -> PAUSED */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_PAUSED),
                "RUNNING -> PAUSED should be valid");
    TEST_ASSERT_EQ(PIPELINE_STATE_PAUSED, sm.current_state, "State should be PAUSED");

    /* PAUSED -> RUNNING */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_RUNNING),
                "PAUSED -> RUNNING should be valid");
    TEST_ASSERT_EQ(PIPELINE_STATE_RUNNING, sm.current_state, "State should be RUNNING");

    /* RUNNING -> STOPPED */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_STOPPED),
                "RUNNING -> STOPPED should be valid");
    TEST_ASSERT_EQ(PIPELINE_STATE_STOPPED, sm.current_state, "State should be STOPPED");

    TEST_ASSERT_EQ(4, sm.transition_count, "Should have 4 transitions");

    return TEST_PASSED;
}

static int test_state_machine_invalid_transitions(void)
{
    PipelineStateMachine sm = {
        .current_state = PIPELINE_STATE_CREATED,
        .pipeline_id = 1,
        .transition_count = 0
    };

    /* CREATED -> PAUSED (invalid) */
    TEST_ASSERT(!state_machine_transition(&sm, PIPELINE_STATE_PAUSED),
                "CREATED -> PAUSED should be invalid");
    TEST_ASSERT_EQ(PIPELINE_STATE_CREATED, sm.current_state, "State should remain CREATED");

    /* CREATED -> ERROR (invalid) */
    TEST_ASSERT(!state_machine_transition(&sm, PIPELINE_STATE_ERROR),
                "CREATED -> ERROR should be invalid");

    /* CREATED -> DRAINING (invalid) */
    TEST_ASSERT(!state_machine_transition(&sm, PIPELINE_STATE_DRAINING),
                "CREATED -> DRAINING should be invalid");

    TEST_ASSERT_EQ(0, sm.transition_count, "No transitions should have occurred");

    return TEST_PASSED;
}

static int test_state_machine_error_recovery(void)
{
    PipelineStateMachine sm = {
        .current_state = PIPELINE_STATE_RUNNING,
        .pipeline_id = 1,
        .transition_count = 0
    };

    /* RUNNING -> ERROR */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_ERROR),
                "RUNNING -> ERROR should be valid");

    /* ERROR -> RUNNING (recovery) */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_RUNNING),
                "ERROR -> RUNNING should be valid (recovery)");

    /* Reset and test ERROR -> STOPPED */
    sm.current_state = PIPELINE_STATE_ERROR;
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_STOPPED),
                "ERROR -> STOPPED should be valid");

    return TEST_PASSED;
}

static int test_state_machine_draining(void)
{
    PipelineStateMachine sm = {
        .current_state = PIPELINE_STATE_RUNNING,
        .pipeline_id = 1,
        .transition_count = 0
    };

    /* RUNNING -> DRAINING */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_DRAINING),
                "RUNNING -> DRAINING should be valid");

    /* DRAINING -> STOPPED */
    TEST_ASSERT(state_machine_transition(&sm, PIPELINE_STATE_STOPPED),
                "DRAINING -> STOPPED should be valid");

    /* DRAINING -> RUNNING (invalid) */
    sm.current_state = PIPELINE_STATE_DRAINING;
    TEST_ASSERT(!state_machine_transition(&sm, PIPELINE_STATE_RUNNING),
                "DRAINING -> RUNNING should be invalid");

    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - JSON Parsing
 * ============================================================ */

static int test_json_parse_single_object(void)
{
    const char *json = "{\"name\": \"test\", \"value\": 42}";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should parse single JSON object");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"name\""), "Should contain name field");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"value\""), "Should contain value field");

    free_records(records);
    return TEST_PASSED;
}

static int test_json_parse_array(void)
{
    const char *json = "[{\"id\": 1}, {\"id\": 2}, {\"id\": 3}]";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should parse JSON array");
    TEST_ASSERT_EQ(3, count_records(records), "Should have 3 records");

    free_records(records);
    return TEST_PASSED;
}

static int test_json_parse_ndjson(void)
{
    const char *json = "{\"id\": 1}\n{\"id\": 2}\n{\"id\": 3}";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should parse NDJSON");
    TEST_ASSERT_EQ(3, count_records(records), "Should have 3 records");

    free_records(records);
    return TEST_PASSED;
}

static int test_json_parse_empty_input(void)
{
    ParsedRecord *records = parse_json_test("", 0);
    TEST_ASSERT_NULL(records, "Empty input should return NULL");

    records = parse_json_test(NULL, 0);
    TEST_ASSERT_NULL(records, "NULL input should return NULL");

    records = parse_json_test("   \n\t  ", 7);
    TEST_ASSERT_NULL(records, "Whitespace-only input should return NULL");

    return TEST_PASSED;
}

static int test_json_parse_with_whitespace(void)
{
    const char *json = "  \n\t  {\"name\": \"test\"}  \n";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should handle leading whitespace");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");

    free_records(records);
    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - CSV Parsing
 * ============================================================ */

static int test_csv_parse_with_header(void)
{
    const char *csv = "name,age,city\nAlice,30,Boston\nBob,25,Seattle";

    ParsedRecord *records = parse_csv_test(csv, strlen(csv), ',', true);
    TEST_ASSERT_NOT_NULL(records, "Should parse CSV with header");
    TEST_ASSERT_EQ(2, count_records(records), "Should have 2 data records");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"name\""), "Should have name field");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"age\""), "Should have age field");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"Alice\""), "Should have Alice value");

    free_records(records);
    return TEST_PASSED;
}

static int test_csv_parse_without_header(void)
{
    const char *csv = "Alice,30,Boston\nBob,25,Seattle";

    ParsedRecord *records = parse_csv_test(csv, strlen(csv), ',', false);
    TEST_ASSERT_NOT_NULL(records, "Should parse CSV without header");
    TEST_ASSERT_EQ(2, count_records(records), "Should have 2 records");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"col0\""), "Should have col0 field");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"col1\""), "Should have col1 field");

    free_records(records);
    return TEST_PASSED;
}

static int test_csv_parse_numeric_values(void)
{
    const char *csv = "value\n42\n3.14\n-100";

    ParsedRecord *records = parse_csv_test(csv, strlen(csv), ',', true);
    TEST_ASSERT_NOT_NULL(records, "Should parse CSV with numeric values");
    TEST_ASSERT_EQ(3, count_records(records), "Should have 3 records");

    /* Check that numeric values are not quoted */
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, ": 42"), "42 should be unquoted");

    free_records(records);
    return TEST_PASSED;
}

static int test_csv_parse_tab_delimiter(void)
{
    const char *csv = "name\tvalue\ntest\t100";

    ParsedRecord *records = parse_csv_test(csv, strlen(csv), '\t', true);
    TEST_ASSERT_NOT_NULL(records, "Should parse TSV");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");

    free_records(records);
    return TEST_PASSED;
}

static int test_csv_parse_empty_input(void)
{
    ParsedRecord *records = parse_csv_test("", 0, ',', true);
    TEST_ASSERT_NULL(records, "Empty CSV should return NULL");

    records = parse_csv_test(NULL, 0, ',', true);
    TEST_ASSERT_NULL(records, "NULL CSV should return NULL");

    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - Line Protocol Parsing
 * ============================================================ */

static int test_line_protocol_basic(void)
{
    const char *line = "cpu,host=server01 value=0.64 1609459200000000000";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse line protocol");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"_measurement\": \"cpu\""),
                        "Should have measurement");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"host\": \"server01\""),
                        "Should have host tag");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"value\": 0.64"),
                        "Should have value field");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_multiple_tags(void)
{
    const char *line = "temp,location=us-west,sensor=A1 reading=23.5 1609459200000000000";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse line protocol with multiple tags");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"location\": \"us-west\""),
                        "Should have location tag");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"sensor\": \"A1\""),
                        "Should have sensor tag");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_integer_field(void)
{
    const char *line = "events count=42i 1609459200000000000";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse integer field");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"count\": 42"),
                        "Integer should be parsed without 'i' suffix");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_boolean_field(void)
{
    const char *line = "status active=true,enabled=false 1609459200000000000";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse boolean fields");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"active\": true"),
                        "Should have true value");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"enabled\": false"),
                        "Should have false value");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_multiline(void)
{
    const char *lines = "cpu value=1.0 1000\ncpu value=2.0 2000\ncpu value=3.0 3000";

    ParsedRecord *records = parse_line_protocol_test(lines, strlen(lines));
    TEST_ASSERT_NOT_NULL(records, "Should parse multiple lines");
    TEST_ASSERT_EQ(3, count_records(records), "Should have 3 records");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_skip_comments(void)
{
    const char *lines = "# This is a comment\ncpu value=1.0 1000\n# Another comment\ncpu value=2.0 2000";

    ParsedRecord *records = parse_line_protocol_test(lines, strlen(lines));
    TEST_ASSERT_NOT_NULL(records, "Should parse lines with comments");
    TEST_ASSERT_EQ(2, count_records(records), "Should have 2 records (comments skipped)");

    free_records(records);
    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - Mock Kafka Consumer
 * ============================================================ */

static int test_kafka_consumer_create(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    TEST_ASSERT_NOT_NULL(consumer, "Consumer should be created");
    TEST_ASSERT(consumer->is_initialized, "Consumer should be initialized");
    TEST_ASSERT(consumer->is_subscribed, "Consumer should be subscribed");
    TEST_ASSERT_STR_EQ("localhost:9092", consumer->bootstrap_servers, "Servers mismatch");
    TEST_ASSERT_STR_EQ("test-topic", consumer->topic, "Topic mismatch");
    TEST_ASSERT_STR_EQ("test-group", consumer->consumer_group, "Group mismatch");

    mock_kafka_destroy(consumer);
    return TEST_PASSED;
}

static int test_kafka_consumer_poll(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    /* Add mock messages */
    mock_kafka_add_message(consumer, "{\"id\": 1}", 0);
    mock_kafka_add_message(consumer, "{\"id\": 2}", 1);
    mock_kafka_add_message(consumer, "{\"id\": 3}", 2);

    char *messages[10];
    int count = mock_kafka_poll(consumer, messages, 10);

    TEST_ASSERT_EQ(3, count, "Should poll 3 messages");
    TEST_ASSERT_EQ(3, consumer->messages_received, "Should have received 3 messages");
    TEST_ASSERT_EQ(2, consumer->current_offset, "Offset should be 2");

    /* Verify message content */
    TEST_ASSERT_STR_EQ("{\"id\": 1}", messages[0], "First message mismatch");
    TEST_ASSERT_STR_EQ("{\"id\": 2}", messages[1], "Second message mismatch");
    TEST_ASSERT_STR_EQ("{\"id\": 3}", messages[2], "Third message mismatch");

    /* Cleanup */
    for (int i = 0; i < count; i++) free(messages[i]);
    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

static int test_kafka_consumer_commit(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    mock_kafka_add_message(consumer, "{\"test\": true}", 100);

    char *messages[1];
    mock_kafka_poll(consumer, messages, 1);

    TEST_ASSERT(mock_kafka_commit(consumer, 100), "Commit should succeed");
    TEST_ASSERT_EQ(100, consumer->committed_offset, "Committed offset mismatch");
    TEST_ASSERT_EQ(1, consumer->messages_committed, "Commit count mismatch");

    free(messages[0]);
    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

static int test_kafka_consumer_seek(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    mock_kafka_add_message(consumer, "{\"id\": 1}", 0);
    mock_kafka_add_message(consumer, "{\"id\": 2}", 1);
    mock_kafka_add_message(consumer, "{\"id\": 3}", 2);

    /* Poll first message */
    char *messages[1];
    mock_kafka_poll(consumer, messages, 1);
    free(messages[0]);

    /* Seek back to beginning */
    TEST_ASSERT(mock_kafka_seek(consumer, 0), "Seek should succeed");
    TEST_ASSERT_EQ(0, consumer->current_offset, "Offset should be 0");
    TEST_ASSERT_EQ(0, consumer->next_message_idx, "Next index should be 0");

    /* Poll all messages again */
    char *all_messages[10];
    int count = mock_kafka_poll(consumer, all_messages, 10);
    TEST_ASSERT_EQ(3, count, "Should poll 3 messages after seek");

    for (int i = 0; i < count; i++) free(all_messages[i]);
    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

static int test_kafka_consumer_error_handling(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    /* Add mix of errors and valid messages */
    mock_kafka_add_error_message(consumer, 1);  /* Error */
    mock_kafka_add_message(consumer, "{\"id\": 1}", 0);  /* Valid */
    mock_kafka_add_error_message(consumer, 1);  /* Error */
    mock_kafka_add_message(consumer, "{\"id\": 2}", 1);  /* Valid */

    char *messages[10];
    int count = mock_kafka_poll(consumer, messages, 10);

    TEST_ASSERT_EQ(2, count, "Should get 2 valid messages despite errors");
    TEST_ASSERT_EQ(2, consumer->errors, "Should have 2 errors");

    for (int i = 0; i < count; i++) free(messages[i]);
    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

static int test_kafka_consumer_consecutive_errors(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    /* Add more than 10 consecutive errors */
    for (int i = 0; i < 15; i++) {
        mock_kafka_add_error_message(consumer, 1);
    }

    char *messages[10];
    int count = mock_kafka_poll(consumer, messages, 10);

    TEST_ASSERT_EQ(-1, count, "Should return -1 after too many consecutive errors");

    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - Error Handling Paths
 * ============================================================ */

static int test_null_consumer_operations(void)
{
    TEST_ASSERT_EQ(-1, mock_kafka_poll(NULL, NULL, 10), "Poll on NULL should return -1");
    TEST_ASSERT(!mock_kafka_commit(NULL, 0), "Commit on NULL should return false");
    TEST_ASSERT(!mock_kafka_seek(NULL, 0), "Seek on NULL should return false");

    return TEST_PASSED;
}

static int test_uninitialized_consumer(void)
{
    MockKafkaConsumer consumer = {0};
    consumer.is_initialized = false;

    char *messages[10];
    TEST_ASSERT_EQ(-1, mock_kafka_poll(&consumer, messages, 10),
                  "Poll on uninitialized should return -1");
    TEST_ASSERT(!mock_kafka_commit(&consumer, 0),
                "Commit on uninitialized should return false");
    TEST_ASSERT(!mock_kafka_seek(&consumer, 0),
                "Seek on uninitialized should return false");

    return TEST_PASSED;
}

static int test_invalid_poll_parameters(void)
{
    MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

    TEST_ASSERT_EQ(0, mock_kafka_poll(consumer, NULL, 10),
                  "Poll with NULL messages should return 0");

    char *messages[10];
    TEST_ASSERT_EQ(0, mock_kafka_poll(consumer, messages, 0),
                  "Poll with max_messages=0 should return 0");
    TEST_ASSERT_EQ(0, mock_kafka_poll(consumer, messages, -1),
                  "Poll with negative max_messages should return 0");

    mock_kafka_destroy(consumer);

    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - Memory Management
 * ============================================================ */

static int test_record_memory_cleanup(void)
{
    /* Test that free_records properly frees all memory */
    ParsedRecord *head = NULL;
    ParsedRecord *tail = NULL;

    for (int i = 0; i < 1000; i++) {
        ParsedRecord *rec = calloc(1, sizeof(ParsedRecord));
        rec->json_str = malloc(100);
        snprintf(rec->json_str, 100, "{\"id\": %d}", i);

        if (!head) head = rec;
        if (tail) tail->next = rec;
        tail = rec;
    }

    TEST_ASSERT_EQ(1000, count_records(head), "Should have 1000 records");

    /* This should not leak memory (run with valgrind to verify) */
    free_records(head);

    return TEST_PASSED;
}

static int test_consumer_memory_cleanup(void)
{
    /* Create and destroy many consumers to check for leaks */
    for (int i = 0; i < 100; i++) {
        MockKafkaConsumer *consumer = mock_kafka_create("localhost:9092", "test-topic", "test-group");

        /* Add some messages */
        for (int j = 0; j < 10; j++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"id\": %d}", j);
            mock_kafka_add_message(consumer, msg, j);
        }

        /* Poll and commit */
        char *messages[10];
        int count = mock_kafka_poll(consumer, messages, 10);
        for (int j = 0; j < count; j++) free(messages[j]);

        mock_kafka_destroy(consumer);
    }

    /* If we get here without crashing, memory management is working */
    return TEST_PASSED;
}

/* ============================================================
 * Test Cases - Edge Cases
 * ============================================================ */

static int test_json_nested_objects(void)
{
    const char *json = "{\"outer\": {\"inner\": {\"deep\": 42}}}";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should parse nested JSON");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "outer"), "Should contain outer");

    free_records(records);
    return TEST_PASSED;
}

static int test_json_array_in_object(void)
{
    const char *json = "{\"items\": [1, 2, 3], \"name\": \"test\"}";

    ParsedRecord *records = parse_json_test(json, strlen(json));
    TEST_ASSERT_NOT_NULL(records, "Should parse JSON with array field");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");

    free_records(records);
    return TEST_PASSED;
}

static int test_csv_crlf_line_endings(void)
{
    const char *csv = "name,value\r\ntest,100\r\ntest2,200\r\n";

    ParsedRecord *records = parse_csv_test(csv, strlen(csv), ',', true);
    TEST_ASSERT_NOT_NULL(records, "Should parse CSV with CRLF");
    TEST_ASSERT_EQ(2, count_records(records), "Should have 2 records");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_no_timestamp(void)
{
    const char *line = "cpu,host=server01 value=0.64";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse line protocol without timestamp");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");
    TEST_ASSERT_NULL(strstr(records->json_str, "_time"), "Should not have _time field");

    free_records(records);
    return TEST_PASSED;
}

static int test_line_protocol_no_tags(void)
{
    const char *line = "cpu value=0.64 1609459200000000000";

    ParsedRecord *records = parse_line_protocol_test(line, strlen(line));
    TEST_ASSERT_NOT_NULL(records, "Should parse line protocol without tags");
    TEST_ASSERT_EQ(1, count_records(records), "Should have 1 record");
    TEST_ASSERT_NOT_NULL(strstr(records->json_str, "\"_measurement\": \"cpu\""),
                        "Should have measurement");

    free_records(records);
    return TEST_PASSED;
}

/* ============================================================
 * Main Test Runner
 * ============================================================ */

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    printf("========================================\n");
    printf("Orochi DB Pipeline Unit Tests\n");
    printf("========================================\n\n");

    /* State Machine Tests */
    printf("--- State Machine Tests ---\n");
    RUN_TEST(test_state_machine_initial_state);
    RUN_TEST(test_state_machine_valid_transitions);
    RUN_TEST(test_state_machine_invalid_transitions);
    RUN_TEST(test_state_machine_error_recovery);
    RUN_TEST(test_state_machine_draining);

    /* JSON Parsing Tests */
    printf("\n--- JSON Parsing Tests ---\n");
    RUN_TEST(test_json_parse_single_object);
    RUN_TEST(test_json_parse_array);
    RUN_TEST(test_json_parse_ndjson);
    RUN_TEST(test_json_parse_empty_input);
    RUN_TEST(test_json_parse_with_whitespace);
    RUN_TEST(test_json_nested_objects);
    RUN_TEST(test_json_array_in_object);

    /* CSV Parsing Tests */
    printf("\n--- CSV Parsing Tests ---\n");
    RUN_TEST(test_csv_parse_with_header);
    RUN_TEST(test_csv_parse_without_header);
    RUN_TEST(test_csv_parse_numeric_values);
    RUN_TEST(test_csv_parse_tab_delimiter);
    RUN_TEST(test_csv_parse_empty_input);
    RUN_TEST(test_csv_crlf_line_endings);

    /* Line Protocol Parsing Tests */
    printf("\n--- Line Protocol Parsing Tests ---\n");
    RUN_TEST(test_line_protocol_basic);
    RUN_TEST(test_line_protocol_multiple_tags);
    RUN_TEST(test_line_protocol_integer_field);
    RUN_TEST(test_line_protocol_boolean_field);
    RUN_TEST(test_line_protocol_multiline);
    RUN_TEST(test_line_protocol_skip_comments);
    RUN_TEST(test_line_protocol_no_timestamp);
    RUN_TEST(test_line_protocol_no_tags);

    /* Mock Kafka Consumer Tests */
    printf("\n--- Mock Kafka Consumer Tests ---\n");
    RUN_TEST(test_kafka_consumer_create);
    RUN_TEST(test_kafka_consumer_poll);
    RUN_TEST(test_kafka_consumer_commit);
    RUN_TEST(test_kafka_consumer_seek);
    RUN_TEST(test_kafka_consumer_error_handling);
    RUN_TEST(test_kafka_consumer_consecutive_errors);

    /* Error Handling Tests */
    printf("\n--- Error Handling Tests ---\n");
    RUN_TEST(test_null_consumer_operations);
    RUN_TEST(test_uninitialized_consumer);
    RUN_TEST(test_invalid_poll_parameters);

    /* Memory Management Tests */
    printf("\n--- Memory Management Tests ---\n");
    RUN_TEST(test_record_memory_cleanup);
    RUN_TEST(test_consumer_memory_cleanup);

    /* Summary */
    printf("\n========================================\n");
    printf("Test Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(" (%d FAILED)", tests_failed);
    }
    printf("\n========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
