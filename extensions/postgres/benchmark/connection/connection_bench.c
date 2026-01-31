/*
 * Orochi DB Connection Pooler Benchmarking Suite
 *
 * Tests connection pooler performance including:
 * - Connection establishment rate (new connections per second)
 * - Pool checkout latency (p50, p95, p99)
 * - Pool saturation behavior under load
 * - Idle connection overhead
 * - Transaction vs session pooling comparison
 * - TLS handshake overhead
 *
 * Copyright (c) 2025 Orochi DB
 */

#include "../common/bench_common.h"
#include <signal.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

/* Connection benchmark specific limits */
#define MAX_CONN_CLIENTS        2000
#define CONN_SAMPLE_SIZE        100000
#define DEFAULT_DURATION_SECS   30
#define DEFAULT_WARMUP_SECS     5

/* Benchmark test types */
typedef enum {
    BENCH_CONN_ESTABLISH,       /* Connection establishment rate */
    BENCH_CHECKOUT_LATENCY,     /* Pool checkout latency */
    BENCH_SATURATION,           /* Pool saturation behavior */
    BENCH_IDLE_OVERHEAD,        /* Idle connection overhead */
    BENCH_TXN_VS_SESSION,       /* Transaction vs session mode */
    BENCH_TLS_OVERHEAD,         /* TLS handshake overhead */
    BENCH_ALL                   /* Run all benchmarks */
} ConnectionBenchType;

/* Pooler type */
typedef enum {
    POOLER_DIRECT,      /* Direct PostgreSQL connection (no pooler) */
    POOLER_PGBOUNCER,   /* PgBouncer */
    POOLER_PGCAT,       /* PgCat (Rust) */
    POOLER_PGDOG        /* PgDog */
} PoolerType;

/* Connection benchmark configuration */
typedef struct {
    BenchConfig base;
    PoolerType pooler_type;
    ConnectionBenchType bench_type;
    int min_clients;
    int max_clients;
    int client_step;
    int idle_connections;
    bool use_tls;
    bool transaction_mode;
    char sslmode[32];
    char pooler_name[64];
} ConnectionBenchConfig;

/* Thread context for connection benchmarks */
typedef struct {
    int thread_id;
    ConnectionBenchConfig *config;
    LatencyStats *stats;
    volatile bool *should_stop;
    int64_t connections_established;
    int64_t connections_failed;
    int64_t queries_executed;
    pthread_t thread;
} ConnectionThreadContext;

/* Global stop flag */
static volatile bool g_should_stop = false;

/* Signal handler */
static void handle_signal(int sig)
{
    (void)sig;
    g_should_stop = true;
}

/*
 * Parse pooler type from string
 */
static PoolerType parse_pooler_type(const char *str)
{
    if (strcasecmp(str, "pgbouncer") == 0) return POOLER_PGBOUNCER;
    if (strcasecmp(str, "pgcat") == 0) return POOLER_PGCAT;
    if (strcasecmp(str, "pgdog") == 0) return POOLER_PGDOG;
    if (strcasecmp(str, "direct") == 0) return POOLER_DIRECT;
    return POOLER_DIRECT;
}

/*
 * Get pooler name string
 */
static const char *get_pooler_name(PoolerType type)
{
    switch (type) {
        case POOLER_PGBOUNCER: return "pgbouncer";
        case POOLER_PGCAT: return "pgcat";
        case POOLER_PGDOG: return "pgdog";
        case POOLER_DIRECT: return "direct";
        default: return "unknown";
    }
}

/*
 * Create connection benchmark configuration
 */
static ConnectionBenchConfig *conn_bench_config_create(void)
{
    ConnectionBenchConfig *config = calloc(1, sizeof(ConnectionBenchConfig));
    if (!config) return NULL;

    /* Initialize base config */
    strncpy(config->base.host, "localhost", MAX_NAME_LEN - 1);
    config->base.port = 5432;
    strncpy(config->base.user, "postgres", MAX_NAME_LEN - 1);
    strncpy(config->base.database, "orochi_bench", MAX_NAME_LEN - 1);
    config->base.duration_secs = DEFAULT_DURATION_SECS;
    config->base.warmup_secs = DEFAULT_WARMUP_SECS;
    config->base.output_format = OUTPUT_JSON;
    strncpy(config->base.output_dir, "results", MAX_PATH_LEN - 1);
    config->base.collect_samples = true;

    /* Connection-specific defaults */
    config->pooler_type = POOLER_DIRECT;
    config->bench_type = BENCH_ALL;
    config->min_clients = 10;
    config->max_clients = 1000;
    config->client_step = 0;  /* Auto-calculate */
    config->idle_connections = 1000;
    config->use_tls = false;
    config->transaction_mode = true;
    strncpy(config->sslmode, "disable", sizeof(config->sslmode) - 1);
    strncpy(config->pooler_name, "direct", sizeof(config->pooler_name) - 1);

    return config;
}

/*
 * Build connection string
 */
static void build_conninfo(const ConnectionBenchConfig *config, char *conninfo, size_t size)
{
    snprintf(conninfo, size,
             "host=%s port=%d user=%s dbname=%s sslmode=%s connect_timeout=10",
             config->base.host, config->base.port, config->base.user,
             config->base.database, config->sslmode);

    if (strlen(config->base.password) > 0) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), " password=%s", config->base.password);
        strncat(conninfo, tmp, size - strlen(conninfo) - 1);
    }
}

/*
 * Benchmark: Connection Establishment Rate
 *
 * Measures how many new connections can be established per second.
 * This tests the overhead of creating new connections (with or without pooler).
 */
static void *connection_establish_worker(void *arg)
{
    ConnectionThreadContext *ctx = (ConnectionThreadContext *)arg;
    char conninfo[1024];
    Timer timer;

    build_conninfo(ctx->config, conninfo, sizeof(conninfo));

    while (!*ctx->should_stop) {
        timer_start(&timer);

        PGconn *conn = PQconnectdb(conninfo);

        timer_stop(&timer);

        if (PQstatus(conn) == CONNECTION_OK) {
            ctx->connections_established++;
            latency_stats_add(ctx->stats, timer_elapsed_us(&timer));

            /* Execute a simple query to ensure connection is fully established */
            PGresult *res = PQexec(conn, "SELECT 1");
            if (res) PQclear(res);
        } else {
            ctx->connections_failed++;
            LOG_DEBUG("Connection failed: %s", PQerrorMessage(conn));
        }

        PQfinish(conn);
    }

    return NULL;
}

static BenchResult *bench_connection_establish(ConnectionBenchConfig *config, int num_clients)
{
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "conn_establish_%s_%d_clients",
             get_pooler_name(config->pooler_type), num_clients);

    BenchResult *result = bench_result_create(name);
    if (!result) return NULL;

    snprintf(result->description, sizeof(result->description),
             "Connection establishment rate with %d concurrent clients using %s",
             num_clients, get_pooler_name(config->pooler_type));

    /* Create thread contexts */
    ConnectionThreadContext *contexts = calloc(num_clients, sizeof(ConnectionThreadContext));
    LatencyStats **stats_array = calloc(num_clients, sizeof(LatencyStats *));

    if (!contexts || !stats_array) {
        result->success = false;
        snprintf(result->error_msg, sizeof(result->error_msg), "Memory allocation failed");
        free(contexts);
        free(stats_array);
        return result;
    }

    g_should_stop = false;

    /* Initialize thread contexts */
    for (int i = 0; i < num_clients; i++) {
        contexts[i].thread_id = i;
        contexts[i].config = config;
        contexts[i].stats = latency_stats_create(CONN_SAMPLE_SIZE / num_clients);
        contexts[i].should_stop = &g_should_stop;
        contexts[i].connections_established = 0;
        contexts[i].connections_failed = 0;
        stats_array[i] = contexts[i].stats;
    }

    LOG_INFO("Starting connection establish benchmark with %d clients...", num_clients);

    Timer bench_timer;
    timer_start(&bench_timer);

    /* Start worker threads */
    for (int i = 0; i < num_clients; i++) {
        pthread_create(&contexts[i].thread, NULL, connection_establish_worker, &contexts[i]);
    }

    /* Warmup period */
    if (config->base.warmup_secs > 0) {
        LOG_INFO("Warmup period: %d seconds...", config->base.warmup_secs);
        sleep(config->base.warmup_secs);

        /* Reset stats after warmup */
        for (int i = 0; i < num_clients; i++) {
            latency_stats_reset(contexts[i].stats);
            contexts[i].connections_established = 0;
            contexts[i].connections_failed = 0;
        }
    }

    /* Benchmark period */
    LOG_INFO("Benchmark period: %d seconds...", config->base.duration_secs);
    timer_start(&bench_timer);
    sleep(config->base.duration_secs);

    /* Stop threads */
    g_should_stop = true;

    for (int i = 0; i < num_clients; i++) {
        pthread_join(contexts[i].thread, NULL);
    }

    timer_stop(&bench_timer);

    /* Aggregate results */
    LatencyStats *merged_stats = latency_stats_create(CONN_SAMPLE_SIZE);
    int64_t total_established = 0;
    int64_t total_failed = 0;

    for (int i = 0; i < num_clients; i++) {
        latency_stats_compute(contexts[i].stats);
        latency_stats_merge(merged_stats, contexts[i].stats);
        total_established += contexts[i].connections_established;
        total_failed += contexts[i].connections_failed;
    }

    /* Combine samples for percentile calculation */
    int64_t sample_idx = 0;
    for (int i = 0; i < num_clients && sample_idx < CONN_SAMPLE_SIZE; i++) {
        for (int64_t j = 0; j < contexts[i].stats->num_samples && sample_idx < CONN_SAMPLE_SIZE; j++) {
            merged_stats->samples[sample_idx++] = contexts[i].stats->samples[j];
        }
    }
    merged_stats->num_samples = sample_idx;
    latency_stats_compute(merged_stats);

    /* Fill result */
    double duration = timer_elapsed_secs(&bench_timer);
    result->total_queries = total_established;
    result->failed_queries = total_failed;
    memcpy(&result->latency, merged_stats, sizeof(LatencyStats));
    result->latency.samples = NULL;  /* Don't copy samples pointer */
    throughput_stats_compute(&result->throughput, total_established, 0, 0, duration);
    result->success = total_failed < total_established / 10;  /* <10% failures */

    LOG_INFO("Completed: %ld connections in %.2fs (%.0f conn/s)",
             total_established, duration, result->throughput.ops_per_sec);

    /* Cleanup */
    latency_stats_free(merged_stats);
    for (int i = 0; i < num_clients; i++) {
        latency_stats_free(contexts[i].stats);
    }
    free(stats_array);
    free(contexts);

    return result;
}

/*
 * Benchmark: Pool Checkout Latency
 *
 * Measures the latency of acquiring a connection from the pool.
 * Uses persistent connections and measures query latency.
 */
static void *checkout_latency_worker(void *arg)
{
    ConnectionThreadContext *ctx = (ConnectionThreadContext *)arg;
    char conninfo[1024];
    Timer timer;

    build_conninfo(ctx->config, conninfo, sizeof(conninfo));

    /* Establish persistent connection */
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR("Thread %d: Connection failed: %s", ctx->thread_id, PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    while (!*ctx->should_stop) {
        timer_start(&timer);

        /* Execute simple query - measures checkout + query + checkin */
        PGresult *res = PQexec(conn, "SELECT 1");

        timer_stop(&timer);

        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            ctx->queries_executed++;
            latency_stats_add(ctx->stats, timer_elapsed_us(&timer));
        } else {
            ctx->connections_failed++;

            /* Try to reconnect */
            PQfinish(conn);
            conn = PQconnectdb(conninfo);
            if (PQstatus(conn) != CONNECTION_OK) {
                LOG_ERROR("Thread %d: Reconnection failed", ctx->thread_id);
                break;
            }
        }

        if (res) PQclear(res);
    }

    PQfinish(conn);
    return NULL;
}

static BenchResult *bench_checkout_latency(ConnectionBenchConfig *config, int num_clients)
{
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "checkout_latency_%s_%d_clients",
             get_pooler_name(config->pooler_type), num_clients);

    BenchResult *result = bench_result_create(name);
    if (!result) return NULL;

    snprintf(result->description, sizeof(result->description),
             "Pool checkout latency with %d concurrent clients using %s",
             num_clients, get_pooler_name(config->pooler_type));

    /* Create thread contexts */
    ConnectionThreadContext *contexts = calloc(num_clients, sizeof(ConnectionThreadContext));
    if (!contexts) {
        result->success = false;
        return result;
    }

    g_should_stop = false;

    for (int i = 0; i < num_clients; i++) {
        contexts[i].thread_id = i;
        contexts[i].config = config;
        contexts[i].stats = latency_stats_create(CONN_SAMPLE_SIZE / num_clients);
        contexts[i].should_stop = &g_should_stop;
        contexts[i].queries_executed = 0;
        contexts[i].connections_failed = 0;
    }

    LOG_INFO("Starting checkout latency benchmark with %d clients...", num_clients);

    Timer bench_timer;

    /* Start worker threads */
    for (int i = 0; i < num_clients; i++) {
        pthread_create(&contexts[i].thread, NULL, checkout_latency_worker, &contexts[i]);
    }

    /* Warmup */
    if (config->base.warmup_secs > 0) {
        sleep(config->base.warmup_secs);
        for (int i = 0; i < num_clients; i++) {
            latency_stats_reset(contexts[i].stats);
            contexts[i].queries_executed = 0;
            contexts[i].connections_failed = 0;
        }
    }

    /* Benchmark */
    timer_start(&bench_timer);
    sleep(config->base.duration_secs);
    g_should_stop = true;

    for (int i = 0; i < num_clients; i++) {
        pthread_join(contexts[i].thread, NULL);
    }

    timer_stop(&bench_timer);

    /* Aggregate results */
    LatencyStats *merged_stats = latency_stats_create(CONN_SAMPLE_SIZE);
    int64_t total_queries = 0;
    int64_t total_failed = 0;

    for (int i = 0; i < num_clients; i++) {
        latency_stats_compute(contexts[i].stats);
        latency_stats_merge(merged_stats, contexts[i].stats);
        total_queries += contexts[i].queries_executed;
        total_failed += contexts[i].connections_failed;
    }

    /* Combine samples */
    int64_t sample_idx = 0;
    for (int i = 0; i < num_clients && sample_idx < CONN_SAMPLE_SIZE; i++) {
        for (int64_t j = 0; j < contexts[i].stats->num_samples && sample_idx < CONN_SAMPLE_SIZE; j++) {
            merged_stats->samples[sample_idx++] = contexts[i].stats->samples[j];
        }
    }
    merged_stats->num_samples = sample_idx;
    latency_stats_compute(merged_stats);

    /* Fill result */
    double duration = timer_elapsed_secs(&bench_timer);
    result->total_queries = total_queries;
    result->failed_queries = total_failed;
    memcpy(&result->latency, merged_stats, sizeof(LatencyStats));
    result->latency.samples = NULL;
    throughput_stats_compute(&result->throughput, total_queries, total_queries, 0, duration);
    result->success = true;

    LOG_INFO("Completed: %ld queries in %.2fs (%.0f qps), p50=%.2fus, p95=%.2fus, p99=%.2fus",
             total_queries, duration, result->throughput.ops_per_sec,
             result->latency.p50_us, result->latency.p95_us, result->latency.p99_us);

    /* Cleanup */
    latency_stats_free(merged_stats);
    for (int i = 0; i < num_clients; i++) {
        latency_stats_free(contexts[i].stats);
    }
    free(contexts);

    return result;
}

/*
 * Benchmark: Pool Saturation
 *
 * Tests behavior under increasing load from 10 to 1000 clients.
 */
static BenchResult *bench_saturation(ConnectionBenchConfig *config, int num_clients)
{
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "saturation_%s_%d_clients",
             get_pooler_name(config->pooler_type), num_clients);

    LOG_INFO("Running saturation test at %d clients...", num_clients);

    /* Use checkout latency test with varied clients */
    return bench_checkout_latency(config, num_clients);
}

/*
 * Benchmark: Idle Connection Overhead
 *
 * Measures memory and CPU overhead with many idle connections.
 */
static BenchResult *bench_idle_overhead(ConnectionBenchConfig *config)
{
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "idle_overhead_%s_%d_conns",
             get_pooler_name(config->pooler_type), config->idle_connections);

    BenchResult *result = bench_result_create(name);
    if (!result) return NULL;

    snprintf(result->description, sizeof(result->description),
             "Idle connection overhead with %d connections using %s",
             config->idle_connections, get_pooler_name(config->pooler_type));

    char conninfo[1024];
    build_conninfo(config, conninfo, sizeof(conninfo));

    LOG_INFO("Establishing %d idle connections...", config->idle_connections);

    /* Establish connections */
    PGconn **connections = calloc(config->idle_connections, sizeof(PGconn *));
    if (!connections) {
        result->success = false;
        return result;
    }

    Timer establish_timer;
    timer_start(&establish_timer);

    int established = 0;
    for (int i = 0; i < config->idle_connections; i++) {
        connections[i] = PQconnectdb(conninfo);
        if (PQstatus(connections[i]) == CONNECTION_OK) {
            established++;
        } else {
            LOG_WARN("Connection %d failed: %s", i, PQerrorMessage(connections[i]));
        }

        if (i % 100 == 0) {
            LOG_DEBUG("Established %d connections...", i);
        }
    }

    timer_stop(&establish_timer);

    LOG_INFO("Established %d/%d connections in %.2fs",
             established, config->idle_connections, timer_elapsed_secs(&establish_timer));

    /* Measure memory usage */
    struct rusage usage_before, usage_after;
    getrusage(RUSAGE_SELF, &usage_before);

    /* Keep connections idle for measurement period */
    LOG_INFO("Holding connections idle for %d seconds...", config->base.duration_secs);
    sleep(config->base.duration_secs);

    getrusage(RUSAGE_SELF, &usage_after);

    /* Cleanup connections */
    LOG_INFO("Closing idle connections...");
    for (int i = 0; i < config->idle_connections; i++) {
        if (connections[i]) {
            PQfinish(connections[i]);
        }
    }
    free(connections);

    /* Fill result */
    result->total_queries = established;
    result->failed_queries = config->idle_connections - established;
    result->total_bytes = (usage_after.ru_maxrss - usage_before.ru_maxrss) * 1024;  /* KB to bytes */
    result->throughput.ops_per_sec = established / timer_elapsed_secs(&establish_timer);
    result->latency.avg_us = timer_elapsed_us(&establish_timer) / established;
    result->success = established > config->idle_connections * 0.9;  /* >90% success */

    LOG_INFO("Memory per connection: %.2f KB",
             (double)result->total_bytes / established / 1024.0);

    return result;
}

/*
 * Benchmark: TLS Handshake Overhead
 *
 * Compares connection establishment with and without TLS.
 */
static BenchResult *bench_tls_overhead(ConnectionBenchConfig *config, bool with_tls)
{
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "tls_%s_%s",
             with_tls ? "enabled" : "disabled",
             get_pooler_name(config->pooler_type));

    BenchResult *result = bench_result_create(name);
    if (!result) return NULL;

    snprintf(result->description, sizeof(result->description),
             "TLS handshake overhead (%s) using %s",
             with_tls ? "enabled" : "disabled",
             get_pooler_name(config->pooler_type));

    /* Temporarily modify SSL mode */
    char original_sslmode[32];
    strncpy(original_sslmode, config->sslmode, sizeof(original_sslmode) - 1);

    if (with_tls) {
        strncpy(config->sslmode, "require", sizeof(config->sslmode) - 1);
    } else {
        strncpy(config->sslmode, "disable", sizeof(config->sslmode) - 1);
    }

    char conninfo[1024];
    build_conninfo(config, conninfo, sizeof(conninfo));

    LOG_INFO("Benchmarking connection establishment with TLS %s...",
             with_tls ? "enabled" : "disabled");

    LatencyStats *stats = latency_stats_create(10000);
    Timer timer;
    int successful = 0;
    int failed = 0;
    int iterations = 1000;

    for (int i = 0; i < iterations && !g_should_stop; i++) {
        timer_start(&timer);

        PGconn *conn = PQconnectdb(conninfo);

        timer_stop(&timer);

        if (PQstatus(conn) == CONNECTION_OK) {
            successful++;
            latency_stats_add(stats, timer_elapsed_us(&timer));
        } else {
            failed++;
            if (failed < 5) {
                LOG_WARN("Connection failed: %s", PQerrorMessage(conn));
            }
        }

        PQfinish(conn);
    }

    latency_stats_compute(stats);

    /* Restore original SSL mode */
    strncpy(config->sslmode, original_sslmode, sizeof(config->sslmode) - 1);

    /* Fill result */
    result->total_queries = successful;
    result->failed_queries = failed;
    memcpy(&result->latency, stats, sizeof(LatencyStats));
    result->latency.samples = NULL;
    result->throughput.ops_per_sec = successful / (stats->avg_us * successful / 1000000.0);
    result->success = failed < successful / 10;

    LOG_INFO("TLS %s: avg=%.2fms, p95=%.2fms, p99=%.2fms",
             with_tls ? "enabled" : "disabled",
             stats->avg_us / 1000.0, stats->p95_us / 1000.0, stats->p99_us / 1000.0);

    latency_stats_free(stats);

    return result;
}

/*
 * Run saturation test across multiple client counts
 */
static void run_saturation_sweep(ConnectionBenchConfig *config, BenchSuite *suite)
{
    int client_counts[] = {10, 50, 100, 200, 500, 1000};
    int num_counts = sizeof(client_counts) / sizeof(client_counts[0]);

    LOG_INFO("Running saturation sweep across %d client counts...", num_counts);

    for (int i = 0; i < num_counts; i++) {
        if (g_should_stop) break;

        LOG_INFO("\n=== Saturation test: %d clients ===", client_counts[i]);

        BenchResult *result = bench_saturation(config, client_counts[i]);
        if (result) {
            bench_suite_add_result(suite, result);
            bench_result_free(result);
        }

        /* Brief pause between tests */
        sleep(2);
    }
}

/*
 * Print usage information
 */
static void print_usage(const char *progname)
{
    printf("Orochi DB Connection Pooler Benchmark\n\n");
    printf("Usage: %s [OPTIONS]\n\n", progname);
    printf("Options:\n");
    printf("  --host=HOST       PostgreSQL/Pooler host [default: localhost]\n");
    printf("  --port=PORT       PostgreSQL/Pooler port [default: 5432]\n");
    printf("  --user=USER       Database user [default: postgres]\n");
    printf("  --password=PASS   Database password\n");
    printf("  --database=DB     Database name [default: orochi_bench]\n");
    printf("  --pooler=TYPE     Pooler type: direct, pgbouncer, pgcat, pgdog [default: direct]\n");
    printf("  --bench=TYPE      Benchmark type: establish, checkout, saturation, idle, tls, all [default: all]\n");
    printf("  --clients=N       Number of concurrent clients [default: 100]\n");
    printf("  --min-clients=N   Minimum clients for sweep [default: 10]\n");
    printf("  --max-clients=N   Maximum clients for sweep [default: 1000]\n");
    printf("  --idle=N          Number of idle connections for idle test [default: 1000]\n");
    printf("  --duration=N      Benchmark duration in seconds [default: 30]\n");
    printf("  --warmup=N        Warmup period in seconds [default: 5]\n");
    printf("  --tls             Enable TLS/SSL\n");
    printf("  --format=FMT      Output format: json, csv, text [default: json]\n");
    printf("  --output=DIR      Output directory [default: results]\n");
    printf("  --verbose         Enable verbose output\n");
    printf("  --help            Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --pooler=pgbouncer --port=6432 --bench=saturation\n", progname);
    printf("  %s --pooler=pgcat --clients=500 --duration=60\n", progname);
    printf("  %s --pooler=direct --bench=tls --tls\n", progname);
}

/*
 * Parse command line arguments
 */
static int parse_args(ConnectionBenchConfig *config, int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (strncmp(arg, "--host=", 7) == 0) {
            strncpy(config->base.host, arg + 7, MAX_NAME_LEN - 1);
        } else if (strncmp(arg, "--port=", 7) == 0) {
            config->base.port = atoi(arg + 7);
        } else if (strncmp(arg, "--user=", 7) == 0) {
            strncpy(config->base.user, arg + 7, MAX_NAME_LEN - 1);
        } else if (strncmp(arg, "--password=", 11) == 0) {
            strncpy(config->base.password, arg + 11, MAX_NAME_LEN - 1);
        } else if (strncmp(arg, "--database=", 11) == 0) {
            strncpy(config->base.database, arg + 11, MAX_NAME_LEN - 1);
        } else if (strncmp(arg, "--pooler=", 9) == 0) {
            config->pooler_type = parse_pooler_type(arg + 9);
            strncpy(config->pooler_name, arg + 9, sizeof(config->pooler_name) - 1);
        } else if (strncmp(arg, "--bench=", 8) == 0) {
            const char *bench = arg + 8;
            if (strcasecmp(bench, "establish") == 0) config->bench_type = BENCH_CONN_ESTABLISH;
            else if (strcasecmp(bench, "checkout") == 0) config->bench_type = BENCH_CHECKOUT_LATENCY;
            else if (strcasecmp(bench, "saturation") == 0) config->bench_type = BENCH_SATURATION;
            else if (strcasecmp(bench, "idle") == 0) config->bench_type = BENCH_IDLE_OVERHEAD;
            else if (strcasecmp(bench, "tls") == 0) config->bench_type = BENCH_TLS_OVERHEAD;
            else config->bench_type = BENCH_ALL;
        } else if (strncmp(arg, "--clients=", 10) == 0) {
            config->base.num_clients = atoi(arg + 10);
        } else if (strncmp(arg, "--min-clients=", 14) == 0) {
            config->min_clients = atoi(arg + 14);
        } else if (strncmp(arg, "--max-clients=", 14) == 0) {
            config->max_clients = atoi(arg + 14);
        } else if (strncmp(arg, "--idle=", 7) == 0) {
            config->idle_connections = atoi(arg + 7);
        } else if (strncmp(arg, "--duration=", 11) == 0) {
            config->base.duration_secs = atoi(arg + 11);
        } else if (strncmp(arg, "--warmup=", 9) == 0) {
            config->base.warmup_secs = atoi(arg + 9);
        } else if (strcmp(arg, "--tls") == 0) {
            config->use_tls = true;
            strncpy(config->sslmode, "require", sizeof(config->sslmode) - 1);
        } else if (strncmp(arg, "--format=", 9) == 0) {
            const char *fmt = arg + 9;
            if (strcasecmp(fmt, "csv") == 0) config->base.output_format = OUTPUT_CSV;
            else if (strcasecmp(fmt, "text") == 0) config->base.output_format = OUTPUT_TEXT;
            else config->base.output_format = OUTPUT_JSON;
        } else if (strncmp(arg, "--output=", 9) == 0) {
            strncpy(config->base.output_dir, arg + 9, MAX_PATH_LEN - 1);
        } else if (strcmp(arg, "--verbose") == 0) {
            config->base.verbose = true;
            g_verbose = true;
        } else if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else {
            LOG_ERROR("Unknown option: %s", arg);
            return -1;
        }
    }

    /* Default clients if not specified */
    if (config->base.num_clients == 0) {
        config->base.num_clients = 100;
    }

    return 0;
}

/*
 * Main entry point
 */
int main(int argc, char *argv[])
{
    printf("\n");
    printf("========================================\n");
    printf("  Connection Pooler Benchmark Suite\n");
    printf("  Orochi DB v%s\n", BENCH_VERSION);
    printf("========================================\n\n");

    /* Setup signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Create and parse configuration */
    ConnectionBenchConfig *config = conn_bench_config_create();
    if (!config) {
        LOG_ERROR("Failed to create configuration");
        return 1;
    }

    if (parse_args(config, argc, argv) != 0) {
        free(config);
        return 1;
    }

    /* Print configuration */
    printf("Configuration:\n");
    printf("  Pooler:     %s\n", get_pooler_name(config->pooler_type));
    printf("  Host:       %s:%d\n", config->base.host, config->base.port);
    printf("  Database:   %s\n", config->base.database);
    printf("  Clients:    %d\n", config->base.num_clients);
    printf("  Duration:   %d seconds\n", config->base.duration_secs);
    printf("  TLS:        %s\n", config->use_tls ? "enabled" : "disabled");
    printf("\n");

    /* Create benchmark suite */
    char suite_name[MAX_NAME_LEN];
    snprintf(suite_name, sizeof(suite_name), "connection_%s", get_pooler_name(config->pooler_type));
    BenchSuite *suite = bench_suite_create(suite_name, 50);
    if (!suite) {
        LOG_ERROR("Failed to create benchmark suite");
        free(config);
        return 1;
    }

    memcpy(&suite->config, &config->base, sizeof(BenchConfig));
    bench_suite_collect_system_info(suite);

    /* Run selected benchmarks */
    switch (config->bench_type) {
        case BENCH_CONN_ESTABLISH: {
            BenchResult *result = bench_connection_establish(config, config->base.num_clients);
            if (result) {
                bench_suite_add_result(suite, result);
                bench_result_free(result);
            }
            break;
        }

        case BENCH_CHECKOUT_LATENCY: {
            BenchResult *result = bench_checkout_latency(config, config->base.num_clients);
            if (result) {
                bench_suite_add_result(suite, result);
                bench_result_free(result);
            }
            break;
        }

        case BENCH_SATURATION:
            run_saturation_sweep(config, suite);
            break;

        case BENCH_IDLE_OVERHEAD: {
            BenchResult *result = bench_idle_overhead(config);
            if (result) {
                bench_suite_add_result(suite, result);
                bench_result_free(result);
            }
            break;
        }

        case BENCH_TLS_OVERHEAD: {
            BenchResult *result_no_tls = bench_tls_overhead(config, false);
            BenchResult *result_tls = bench_tls_overhead(config, true);

            if (result_no_tls) {
                bench_suite_add_result(suite, result_no_tls);
                bench_result_free(result_no_tls);
            }
            if (result_tls) {
                bench_suite_add_result(suite, result_tls);
                bench_result_free(result_tls);
            }
            break;
        }

        case BENCH_ALL:
        default:
            LOG_INFO("Running all benchmarks...\n");

            /* Connection establishment */
            if (!g_should_stop) {
                LOG_INFO("\n=== Connection Establishment ===");
                BenchResult *result = bench_connection_establish(config, config->base.num_clients);
                if (result) {
                    bench_suite_add_result(suite, result);
                    bench_result_free(result);
                }
            }

            /* Checkout latency */
            if (!g_should_stop) {
                LOG_INFO("\n=== Checkout Latency ===");
                BenchResult *result = bench_checkout_latency(config, config->base.num_clients);
                if (result) {
                    bench_suite_add_result(suite, result);
                    bench_result_free(result);
                }
            }

            /* Saturation sweep */
            if (!g_should_stop) {
                LOG_INFO("\n=== Saturation Sweep ===");
                run_saturation_sweep(config, suite);
            }

            /* Idle overhead */
            if (!g_should_stop) {
                LOG_INFO("\n=== Idle Connection Overhead ===");
                BenchResult *result = bench_idle_overhead(config);
                if (result) {
                    bench_suite_add_result(suite, result);
                    bench_result_free(result);
                }
            }

            /* TLS overhead */
            if (!g_should_stop) {
                LOG_INFO("\n=== TLS Overhead ===");
                BenchResult *result_no_tls = bench_tls_overhead(config, false);
                BenchResult *result_tls = bench_tls_overhead(config, true);

                if (result_no_tls) {
                    bench_suite_add_result(suite, result_no_tls);
                    bench_result_free(result_no_tls);
                }
                if (result_tls) {
                    bench_suite_add_result(suite, result_tls);
                    bench_result_free(result_tls);
                }
            }
            break;
    }

    suite->end_time = time(NULL);

    /* Save results */
    char output_file[MAX_PATH_LEN];
    const char *ext = config->base.output_format == OUTPUT_JSON ? "json" :
                      config->base.output_format == OUTPUT_CSV ? "csv" : "txt";
    snprintf(output_file, sizeof(output_file), "%s/connection_%s_%ld.%s",
             config->base.output_dir, get_pooler_name(config->pooler_type),
             time(NULL), ext);

    bench_suite_save(suite, output_file);

    /* Print summary */
    bench_suite_print_summary(suite);

    /* Cleanup */
    bench_suite_free(suite);
    free(config);

    printf("\nBenchmark complete. Results saved to: %s\n", output_file);

    return 0;
}
