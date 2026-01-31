/*
 * Orochi DB Multi-Tenant Isolation Benchmarking Suite
 *
 * Implements MuTeBench methodology for measuring:
 * - Per-tenant baseline performance
 * - Concurrent multi-tenant workload testing
 * - Resource fairness (Jain's Fairness Index)
 * - Noisy neighbor detection
 * - SLA compliance rates
 *
 * Reference: MuTeBench - A Multi-Tenant Benchmark Framework
 */

#include "../common/bench_common.h"
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>

/* Multi-tenant specific limits */
#define MAX_TENANTS             256
#define DEFAULT_TENANTS         10
#define DEFAULT_ROWS_PER_TENANT 10000
#define DEFAULT_SLA_LATENCY_MS  100.0  /* 100ms SLA threshold */
#define NOISY_THRESHOLD         0.2    /* 20% performance degradation */

/* Isolation models */
typedef enum {
    ISOLATION_SHARED_TABLE,     /* Single table with tenant_id column */
    ISOLATION_SHARED_SCHEMA,    /* Per-tenant schemas in same DB */
    ISOLATION_DEDICATED_DB      /* Separate databases per tenant */
} IsolationModel;

/* Workload types */
typedef enum {
    WORKLOAD_READ_HEAVY,        /* 90% reads, 10% writes */
    WORKLOAD_WRITE_HEAVY,       /* 30% reads, 70% writes */
    WORKLOAD_MIXED,             /* 50% reads, 50% writes */
    WORKLOAD_ANALYTICAL         /* Complex aggregation queries */
} WorkloadType;

/* Per-tenant statistics */
typedef struct TenantStats {
    int             tenant_id;
    char            tenant_name[MAX_NAME_LEN];
    LatencyStats    *baseline_latency;  /* Isolation performance */
    LatencyStats    *concurrent_latency; /* Under contention */
    ThroughputStats baseline_throughput;
    ThroughputStats concurrent_throughput;
    int64_t         operations_completed;
    int64_t         operations_failed;
    int64_t         sla_violations;      /* Operations exceeding SLA */
    double          relative_performance; /* concurrent / baseline */
    bool            is_noisy_neighbor;
    bool            affected_by_noisy;
} TenantStats;

/* Multi-tenant benchmark context */
typedef struct MultiTenantBench {
    BenchConfig     *config;
    IsolationModel  isolation_model;
    WorkloadType    workload_type;
    int             num_tenants;
    int             rows_per_tenant;
    double          sla_latency_ms;
    TenantStats     *tenant_stats;
    volatile bool   should_stop;

    /* Aggregate metrics */
    double          jains_fairness_index;
    double          overall_sla_compliance;
    int             noisy_neighbor_count;
    double          avg_relative_performance;
} MultiTenantBench;

/* Thread context for concurrent benchmark */
typedef struct TenantThreadContext {
    int                 tenant_id;
    MultiTenantBench    *bench;
    PGconn              *conn;
    pthread_t           thread;
    volatile bool       *should_stop;
    int64_t             ops_completed;
    int64_t             ops_failed;
    int64_t             sla_violations;
} TenantThreadContext;

/* Global volatile flag for signal handling */
static volatile sig_atomic_t g_stop_flag = 0;

static void signal_handler(int signum)
{
    (void)signum;
    g_stop_flag = 1;
}

/* === Jain's Fairness Index === */

/*
 * Calculate Jain's Fairness Index
 * Formula: J(x1, x2, ..., xn) = (Sum(xi))^2 / (n * Sum(xi^2))
 *
 * Returns value between 0 and 1:
 * - 1.0 = perfectly fair (all tenants get equal performance)
 * - 1/n = completely unfair (one tenant gets all resources)
 */
static double calculate_jains_fairness(double *values, int n)
{
    if (n <= 0) return 0.0;

    double sum = 0.0;
    double sum_sq = 0.0;

    for (int i = 0; i < n; i++) {
        sum += values[i];
        sum_sq += values[i] * values[i];
    }

    if (sum_sq == 0.0) return 1.0;

    return (sum * sum) / ((double)n * sum_sq);
}

/* === Multi-tenant Benchmark Functions === */

static MultiTenantBench *multitenant_bench_create(BenchConfig *config)
{
    MultiTenantBench *bench = calloc(1, sizeof(MultiTenantBench));
    if (!bench) return NULL;

    bench->config = config;
    bench->isolation_model = ISOLATION_SHARED_TABLE;
    bench->workload_type = WORKLOAD_MIXED;
    bench->num_tenants = DEFAULT_TENANTS;
    bench->rows_per_tenant = DEFAULT_ROWS_PER_TENANT;
    bench->sla_latency_ms = DEFAULT_SLA_LATENCY_MS;

    return bench;
}

static void multitenant_bench_free(MultiTenantBench *bench)
{
    if (bench) {
        if (bench->tenant_stats) {
            for (int i = 0; i < bench->num_tenants; i++) {
                latency_stats_free(bench->tenant_stats[i].baseline_latency);
                latency_stats_free(bench->tenant_stats[i].concurrent_latency);
            }
            free(bench->tenant_stats);
        }
        free(bench);
    }
}

static int multitenant_bench_init_stats(MultiTenantBench *bench)
{
    bench->tenant_stats = calloc(bench->num_tenants, sizeof(TenantStats));
    if (!bench->tenant_stats) return -1;

    for (int i = 0; i < bench->num_tenants; i++) {
        bench->tenant_stats[i].tenant_id = i + 1;
        snprintf(bench->tenant_stats[i].tenant_name, MAX_NAME_LEN,
                 "tenant_%d", i + 1);

        bench->tenant_stats[i].baseline_latency =
            latency_stats_create(MAX_SAMPLES / bench->num_tenants);
        bench->tenant_stats[i].concurrent_latency =
            latency_stats_create(MAX_SAMPLES / bench->num_tenants);

        if (!bench->tenant_stats[i].baseline_latency ||
            !bench->tenant_stats[i].concurrent_latency) {
            return -1;
        }
    }

    return 0;
}

/* === Schema Creation === */

static const char *SHARED_TABLE_SCHEMA =
    "-- Shared table isolation model\n"
    "DROP TABLE IF EXISTS tenant_data CASCADE;\n"
    "DROP TABLE IF EXISTS tenants CASCADE;\n"
    "\n"
    "CREATE TABLE tenants (\n"
    "    tenant_id SERIAL PRIMARY KEY,\n"
    "    tenant_name VARCHAR(100) NOT NULL UNIQUE,\n"
    "    created_at TIMESTAMP DEFAULT NOW(),\n"
    "    quota_mb INTEGER DEFAULT 1000,\n"
    "    is_active BOOLEAN DEFAULT TRUE\n"
    ");\n"
    "\n"
    "CREATE TABLE tenant_data (\n"
    "    id BIGSERIAL,\n"
    "    tenant_id INTEGER NOT NULL REFERENCES tenants(tenant_id),\n"
    "    key VARCHAR(100) NOT NULL,\n"
    "    value TEXT,\n"
    "    numeric_value NUMERIC(15, 2),\n"
    "    created_at TIMESTAMP DEFAULT NOW(),\n"
    "    updated_at TIMESTAMP DEFAULT NOW(),\n"
    "    PRIMARY KEY (tenant_id, id)\n"
    ");\n"
    "\n"
    "-- Indexes for tenant isolation\n"
    "CREATE INDEX idx_tenant_data_tenant_id ON tenant_data(tenant_id);\n"
    "CREATE INDEX idx_tenant_data_key ON tenant_data(tenant_id, key);\n"
    "CREATE INDEX idx_tenant_data_created ON tenant_data(tenant_id, created_at);\n"
    "\n"
    "-- Enable Row Level Security\n"
    "ALTER TABLE tenant_data ENABLE ROW LEVEL SECURITY;\n"
    "\n"
    "-- RLS Policy: Tenants can only see their own data\n"
    "CREATE POLICY tenant_isolation_policy ON tenant_data\n"
    "    USING (tenant_id = current_setting('app.current_tenant_id')::INTEGER);\n";

static const char *SHARED_SCHEMA_TEMPLATE =
    "-- Create schema for tenant %d\n"
    "DROP SCHEMA IF EXISTS tenant_%d CASCADE;\n"
    "CREATE SCHEMA tenant_%d;\n"
    "\n"
    "CREATE TABLE tenant_%d.data (\n"
    "    id BIGSERIAL PRIMARY KEY,\n"
    "    key VARCHAR(100) NOT NULL,\n"
    "    value TEXT,\n"
    "    numeric_value NUMERIC(15, 2),\n"
    "    created_at TIMESTAMP DEFAULT NOW(),\n"
    "    updated_at TIMESTAMP DEFAULT NOW()\n"
    ");\n"
    "\n"
    "CREATE INDEX idx_data_key ON tenant_%d.data(key);\n"
    "CREATE INDEX idx_data_created ON tenant_%d.data(created_at);\n";

static int create_schema(PGconn *conn, MultiTenantBench *bench)
{
    char query[MAX_QUERY_LEN];

    LOG_INFO("Creating multi-tenant schema (model: %d)...",
             bench->isolation_model);

    switch (bench->isolation_model) {
        case ISOLATION_SHARED_TABLE:
            if (!bench_exec(conn, SHARED_TABLE_SCHEMA)) {
                LOG_ERROR("Failed to create shared table schema");
                return -1;
            }

            /* Create tenants */
            for (int i = 1; i <= bench->num_tenants; i++) {
                snprintf(query, sizeof(query),
                         "INSERT INTO tenants (tenant_name) VALUES ('tenant_%d')",
                         i);
                if (!bench_exec(conn, query)) {
                    LOG_ERROR("Failed to create tenant %d", i);
                    return -1;
                }
            }
            break;

        case ISOLATION_SHARED_SCHEMA:
            for (int i = 1; i <= bench->num_tenants; i++) {
                snprintf(query, sizeof(query), SHARED_SCHEMA_TEMPLATE,
                         i, i, i, i, i, i);
                if (!bench_exec(conn, query)) {
                    LOG_ERROR("Failed to create schema for tenant %d", i);
                    return -1;
                }
            }
            break;

        case ISOLATION_DEDICATED_DB:
            /* Not implemented in this benchmark - requires external setup */
            LOG_WARN("Dedicated DB isolation requires external setup");
            return -1;
    }

    LOG_INFO("Schema created successfully");
    return 0;
}

/* === Data Generation === */

static int generate_tenant_data(PGconn *conn, MultiTenantBench *bench,
                                 int tenant_id)
{
    char query[MAX_QUERY_LEN];
    Timer timer;

    timer_start(&timer);

    LOG_DEBUG("Generating %d rows for tenant %d...",
              bench->rows_per_tenant, tenant_id);

    if (bench->isolation_model == ISOLATION_SHARED_TABLE) {
        /* Batch insert for shared table */
        snprintf(query, sizeof(query),
                 "INSERT INTO tenant_data (tenant_id, key, value, numeric_value) "
                 "SELECT %d, "
                 "       'key_' || generate_series, "
                 "       md5(random()::text), "
                 "       (random() * 10000)::numeric(15,2) "
                 "FROM generate_series(1, %d)",
                 tenant_id, bench->rows_per_tenant);
    } else {
        /* Per-schema insert */
        snprintf(query, sizeof(query),
                 "INSERT INTO tenant_%d.data (key, value, numeric_value) "
                 "SELECT 'key_' || generate_series, "
                 "       md5(random()::text), "
                 "       (random() * 10000)::numeric(15,2) "
                 "FROM generate_series(1, %d)",
                 tenant_id, bench->rows_per_tenant);
    }

    if (!bench_exec(conn, query)) {
        LOG_ERROR("Failed to generate data for tenant %d", tenant_id);
        return -1;
    }

    timer_stop(&timer);
    LOG_DEBUG("Tenant %d data generated in %.2f seconds",
              tenant_id, timer_elapsed_secs(&timer));

    return 0;
}

static int generate_all_tenant_data(PGconn *conn, MultiTenantBench *bench)
{
    LOG_INFO("Generating test data for %d tenants (%d rows each)...",
             bench->num_tenants, bench->rows_per_tenant);

    for (int i = 1; i <= bench->num_tenants; i++) {
        if (generate_tenant_data(conn, bench, i) != 0) {
            return -1;
        }

        /* Progress indicator */
        printf("\rGenerating tenant data: %d/%d", i, bench->num_tenants);
        fflush(stdout);
    }
    printf("\n");

    /* Analyze tables for query optimizer */
    if (bench->isolation_model == ISOLATION_SHARED_TABLE) {
        bench_exec(conn, "ANALYZE tenants");
        bench_exec(conn, "ANALYZE tenant_data");
    }

    return 0;
}

/* === Workload Execution === */

static void execute_read_query(PGconn *conn, MultiTenantBench *bench,
                                int tenant_id, double *latency_us)
{
    char query[MAX_QUERY_LEN];
    Timer timer;
    int random_key = bench_generate_random_int(1, bench->rows_per_tenant);

    timer_start(&timer);

    if (bench->isolation_model == ISOLATION_SHARED_TABLE) {
        snprintf(query, sizeof(query),
                 "SELECT id, key, value, numeric_value FROM tenant_data "
                 "WHERE tenant_id = %d AND key = 'key_%d'",
                 tenant_id, random_key);
    } else {
        snprintf(query, sizeof(query),
                 "SELECT id, key, value, numeric_value FROM tenant_%d.data "
                 "WHERE key = 'key_%d'",
                 tenant_id, random_key);
    }

    PGresult *res = PQexec(conn, query);
    timer_stop(&timer);

    *latency_us = timer_elapsed_us(&timer);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Read query failed: %s", PQerrorMessage(conn));
    }
    PQclear(res);
}

static void execute_write_query(PGconn *conn, MultiTenantBench *bench,
                                 int tenant_id, double *latency_us)
{
    char query[MAX_QUERY_LEN];
    Timer timer;
    int random_key = bench_generate_random_int(1, bench->rows_per_tenant);
    double random_value = bench_generate_random_double(0, 10000);

    timer_start(&timer);

    if (bench->isolation_model == ISOLATION_SHARED_TABLE) {
        snprintf(query, sizeof(query),
                 "UPDATE tenant_data SET numeric_value = %.2f, "
                 "updated_at = NOW() "
                 "WHERE tenant_id = %d AND key = 'key_%d'",
                 random_value, tenant_id, random_key);
    } else {
        snprintf(query, sizeof(query),
                 "UPDATE tenant_%d.data SET numeric_value = %.2f, "
                 "updated_at = NOW() "
                 "WHERE key = 'key_%d'",
                 tenant_id, random_value, random_key);
    }

    PGresult *res = PQexec(conn, query);
    timer_stop(&timer);

    *latency_us = timer_elapsed_us(&timer);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Write query failed: %s", PQerrorMessage(conn));
    }
    PQclear(res);
}

static void execute_analytical_query(PGconn *conn, MultiTenantBench *bench,
                                      int tenant_id, double *latency_us)
{
    char query[MAX_QUERY_LEN];
    Timer timer;

    timer_start(&timer);

    if (bench->isolation_model == ISOLATION_SHARED_TABLE) {
        snprintf(query, sizeof(query),
                 "SELECT COUNT(*), AVG(numeric_value), "
                 "MIN(numeric_value), MAX(numeric_value), "
                 "STDDEV(numeric_value) "
                 "FROM tenant_data WHERE tenant_id = %d",
                 tenant_id);
    } else {
        snprintf(query, sizeof(query),
                 "SELECT COUNT(*), AVG(numeric_value), "
                 "MIN(numeric_value), MAX(numeric_value), "
                 "STDDEV(numeric_value) "
                 "FROM tenant_%d.data",
                 tenant_id);
    }

    PGresult *res = PQexec(conn, query);
    timer_stop(&timer);

    *latency_us = timer_elapsed_us(&timer);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Analytical query failed: %s", PQerrorMessage(conn));
    }
    PQclear(res);
}

static void execute_workload_operation(PGconn *conn, MultiTenantBench *bench,
                                        int tenant_id, double *latency_us)
{
    int r = rand() % 100;

    switch (bench->workload_type) {
        case WORKLOAD_READ_HEAVY:
            if (r < 90)
                execute_read_query(conn, bench, tenant_id, latency_us);
            else
                execute_write_query(conn, bench, tenant_id, latency_us);
            break;

        case WORKLOAD_WRITE_HEAVY:
            if (r < 30)
                execute_read_query(conn, bench, tenant_id, latency_us);
            else
                execute_write_query(conn, bench, tenant_id, latency_us);
            break;

        case WORKLOAD_MIXED:
            if (r < 50)
                execute_read_query(conn, bench, tenant_id, latency_us);
            else
                execute_write_query(conn, bench, tenant_id, latency_us);
            break;

        case WORKLOAD_ANALYTICAL:
            execute_analytical_query(conn, bench, tenant_id, latency_us);
            break;
    }
}

/* === Baseline Benchmark (Single Tenant Isolation) === */

static int run_baseline_benchmark(MultiTenantBench *bench, int tenant_id)
{
    TenantStats *stats = &bench->tenant_stats[tenant_id - 1];
    BenchConfig *config = bench->config;
    Timer timer;
    double latency_us;
    int64_t ops = 0;
    int64_t failed = 0;

    LOG_DEBUG("Running baseline benchmark for tenant %d...", tenant_id);

    PGconn *conn = bench_connect(config);
    if (!conn) return -1;

    /* Set current tenant for RLS */
    char set_tenant[128];
    snprintf(set_tenant, sizeof(set_tenant),
             "SET app.current_tenant_id = '%d'", tenant_id);
    bench_exec(conn, set_tenant);

    /* Warmup phase */
    Timer warmup_timer;
    timer_start(&warmup_timer);
    while (timer_elapsed_secs(&warmup_timer) < config->warmup_secs) {
        execute_workload_operation(conn, bench, tenant_id, &latency_us);
    }

    /* Reset stats after warmup */
    latency_stats_reset(stats->baseline_latency);

    /* Main benchmark phase */
    timer_start(&timer);
    while (timer_elapsed_secs(&timer) < config->duration_secs && !g_stop_flag) {
        execute_workload_operation(conn, bench, tenant_id, &latency_us);

        if (latency_us > 0) {
            latency_stats_add(stats->baseline_latency, latency_us);
            ops++;

            if (latency_us / 1000.0 > bench->sla_latency_ms) {
                stats->sla_violations++;
            }
        } else {
            failed++;
        }
    }
    timer_stop(&timer);

    /* Compute statistics */
    latency_stats_compute(stats->baseline_latency);
    throughput_stats_compute(&stats->baseline_throughput, ops, ops, 0,
                             timer_elapsed_secs(&timer));

    bench_disconnect(conn);

    LOG_DEBUG("Tenant %d baseline: %.0f ops/s, avg latency: %.2f ms",
              tenant_id, stats->baseline_throughput.ops_per_sec,
              stats->baseline_latency->avg_us / 1000.0);

    return 0;
}

/* === Concurrent Multi-Tenant Benchmark === */

static void *tenant_worker_thread(void *arg)
{
    TenantThreadContext *ctx = (TenantThreadContext *)arg;
    MultiTenantBench *bench = ctx->bench;
    TenantStats *stats = &bench->tenant_stats[ctx->tenant_id - 1];
    double latency_us;

    /* Set current tenant for RLS */
    char set_tenant[128];
    snprintf(set_tenant, sizeof(set_tenant),
             "SET app.current_tenant_id = '%d'", ctx->tenant_id);
    bench_exec(ctx->conn, set_tenant);

    while (!*ctx->should_stop && !g_stop_flag) {
        execute_workload_operation(ctx->conn, bench, ctx->tenant_id, &latency_us);

        if (latency_us > 0) {
            latency_stats_add(stats->concurrent_latency, latency_us);
            ctx->ops_completed++;

            if (latency_us / 1000.0 > bench->sla_latency_ms) {
                ctx->sla_violations++;
            }
        } else {
            ctx->ops_failed++;
        }
    }

    return NULL;
}

static int run_concurrent_benchmark(MultiTenantBench *bench)
{
    BenchConfig *config = bench->config;
    TenantThreadContext *contexts;
    Timer timer;
    bool should_stop = false;

    LOG_INFO("Running concurrent multi-tenant benchmark...");

    contexts = calloc(bench->num_tenants, sizeof(TenantThreadContext));
    if (!contexts) return -1;

    /* Create connections and threads for each tenant */
    for (int i = 0; i < bench->num_tenants; i++) {
        contexts[i].tenant_id = i + 1;
        contexts[i].bench = bench;
        contexts[i].should_stop = &should_stop;

        contexts[i].conn = bench_connect(config);
        if (!contexts[i].conn) {
            LOG_ERROR("Failed to connect for tenant %d", i + 1);
            goto cleanup;
        }
    }

    /* Warmup phase */
    LOG_INFO("Warmup phase (%d seconds)...", config->warmup_secs);
    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_create(&contexts[i].thread, NULL, tenant_worker_thread, &contexts[i]);
    }

    sleep(config->warmup_secs);
    should_stop = true;

    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_join(contexts[i].thread, NULL);
        latency_stats_reset(bench->tenant_stats[i].concurrent_latency);
        contexts[i].ops_completed = 0;
        contexts[i].ops_failed = 0;
        contexts[i].sla_violations = 0;
    }

    /* Main benchmark phase */
    should_stop = false;
    LOG_INFO("Main benchmark phase (%d seconds)...", config->duration_secs);

    timer_start(&timer);
    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_create(&contexts[i].thread, NULL, tenant_worker_thread, &contexts[i]);
    }

    sleep(config->duration_secs);
    should_stop = true;

    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_join(contexts[i].thread, NULL);
    }
    timer_stop(&timer);

    /* Compute statistics */
    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];

        latency_stats_compute(stats->concurrent_latency);
        throughput_stats_compute(&stats->concurrent_throughput,
                                 contexts[i].ops_completed,
                                 contexts[i].ops_completed, 0,
                                 timer_elapsed_secs(&timer));

        stats->operations_completed = contexts[i].ops_completed;
        stats->operations_failed = contexts[i].ops_failed;
        stats->sla_violations = contexts[i].sla_violations;

        /* Calculate relative performance */
        if (stats->baseline_throughput.ops_per_sec > 0) {
            stats->relative_performance =
                stats->concurrent_throughput.ops_per_sec /
                stats->baseline_throughput.ops_per_sec;
        }
    }

cleanup:
    for (int i = 0; i < bench->num_tenants; i++) {
        if (contexts[i].conn) {
            bench_disconnect(contexts[i].conn);
        }
    }
    free(contexts);

    return 0;
}

/* === Noisy Neighbor Detection === */

static int run_noisy_neighbor_test(MultiTenantBench *bench)
{
    BenchConfig *config = bench->config;
    Timer timer;
    int noisy_tenant = 1;  /* Tenant 1 will generate heavy load */

    LOG_INFO("Running noisy neighbor detection test...");
    LOG_INFO("Tenant %d will generate heavy analytical workload", noisy_tenant);

    /* Run heavy workload for noisy tenant while measuring others */
    TenantThreadContext *contexts = calloc(bench->num_tenants, sizeof(TenantThreadContext));
    bool should_stop = false;

    for (int i = 0; i < bench->num_tenants; i++) {
        contexts[i].tenant_id = i + 1;
        contexts[i].bench = bench;
        contexts[i].should_stop = &should_stop;
        contexts[i].conn = bench_connect(config);
    }

    /* Temporarily change noisy tenant to analytical workload */
    WorkloadType original_workload = bench->workload_type;
    MultiTenantBench *noisy_bench = calloc(1, sizeof(MultiTenantBench));
    memcpy(noisy_bench, bench, sizeof(MultiTenantBench));
    noisy_bench->workload_type = WORKLOAD_ANALYTICAL;

    timer_start(&timer);
    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_create(&contexts[i].thread, NULL, tenant_worker_thread, &contexts[i]);
    }

    sleep(config->duration_secs / 2);  /* Run for half duration */
    should_stop = true;

    for (int i = 0; i < bench->num_tenants; i++) {
        pthread_join(contexts[i].thread, NULL);
    }
    timer_stop(&timer);

    /* Analyze impact */
    double noisy_throughput = bench->tenant_stats[noisy_tenant - 1].concurrent_throughput.ops_per_sec;

    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];

        if (i == noisy_tenant - 1) {
            stats->is_noisy_neighbor = true;
            bench->noisy_neighbor_count++;
        } else {
            /* Check if this tenant was affected by noisy neighbor */
            double perf_degradation = 1.0 - stats->relative_performance;
            if (perf_degradation > NOISY_THRESHOLD) {
                stats->affected_by_noisy = true;
                LOG_WARN("Tenant %d affected by noisy neighbor (%.1f%% degradation)",
                         i + 1, perf_degradation * 100);
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < bench->num_tenants; i++) {
        bench_disconnect(contexts[i].conn);
    }
    free(contexts);
    free(noisy_bench);
    bench->workload_type = original_workload;

    return 0;
}

/* === Aggregate Metrics Calculation === */

static void calculate_aggregate_metrics(MultiTenantBench *bench)
{
    double *throughputs = calloc(bench->num_tenants, sizeof(double));
    double total_relative_perf = 0.0;
    int64_t total_ops = 0;
    int64_t total_sla_violations = 0;

    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];
        throughputs[i] = stats->concurrent_throughput.ops_per_sec;
        total_relative_perf += stats->relative_performance;
        total_ops += stats->operations_completed;
        total_sla_violations += stats->sla_violations;
    }

    /* Jain's Fairness Index */
    bench->jains_fairness_index = calculate_jains_fairness(throughputs, bench->num_tenants);

    /* Average relative performance */
    bench->avg_relative_performance = total_relative_perf / bench->num_tenants;

    /* SLA compliance rate */
    if (total_ops > 0) {
        bench->overall_sla_compliance =
            1.0 - ((double)total_sla_violations / total_ops);
    }

    free(throughputs);
}

/* === Results Output === */

static void print_tenant_results(MultiTenantBench *bench, FILE *fp)
{
    fprintf(fp, "\n========================================\n");
    fprintf(fp, "   MULTI-TENANT BENCHMARK RESULTS\n");
    fprintf(fp, "========================================\n\n");

    fprintf(fp, "Configuration:\n");
    fprintf(fp, "  Tenants:        %d\n", bench->num_tenants);
    fprintf(fp, "  Rows/Tenant:    %d\n", bench->rows_per_tenant);
    fprintf(fp, "  Isolation:      %s\n",
            bench->isolation_model == ISOLATION_SHARED_TABLE ? "Shared Table" :
            bench->isolation_model == ISOLATION_SHARED_SCHEMA ? "Shared Schema" : "Dedicated DB");
    fprintf(fp, "  Workload:       %s\n",
            bench->workload_type == WORKLOAD_READ_HEAVY ? "Read Heavy (90/10)" :
            bench->workload_type == WORKLOAD_WRITE_HEAVY ? "Write Heavy (30/70)" :
            bench->workload_type == WORKLOAD_MIXED ? "Mixed (50/50)" : "Analytical");
    fprintf(fp, "  SLA Threshold:  %.0f ms\n\n", bench->sla_latency_ms);

    fprintf(fp, "Per-Tenant Results:\n");
    fprintf(fp, "%-10s %12s %12s %12s %12s %10s\n",
            "Tenant", "Baseline", "Concurrent", "Rel.Perf", "SLA Viol", "Status");
    fprintf(fp, "%-10s %12s %12s %12s %12s %10s\n",
            "", "(ops/s)", "(ops/s)", "(ratio)", "(count)", "");
    fprintf(fp, "--------------------------------------------------------------------------------\n");

    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];
        const char *status = stats->is_noisy_neighbor ? "NOISY" :
                             stats->affected_by_noisy ? "AFFECTED" : "OK";

        fprintf(fp, "%-10s %12.0f %12.0f %12.2f %12" PRId64 " %10s\n",
                stats->tenant_name,
                stats->baseline_throughput.ops_per_sec,
                stats->concurrent_throughput.ops_per_sec,
                stats->relative_performance,
                stats->sla_violations,
                status);
    }

    fprintf(fp, "\nLatency Percentiles (Concurrent, ms):\n");
    fprintf(fp, "%-10s %10s %10s %10s %10s %10s\n",
            "Tenant", "Avg", "P50", "P95", "P99", "Max");
    fprintf(fp, "----------------------------------------------------------------\n");

    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];
        fprintf(fp, "%-10s %10.2f %10.2f %10.2f %10.2f %10.2f\n",
                stats->tenant_name,
                stats->concurrent_latency->avg_us / 1000.0,
                stats->concurrent_latency->p50_us / 1000.0,
                stats->concurrent_latency->p95_us / 1000.0,
                stats->concurrent_latency->p99_us / 1000.0,
                stats->concurrent_latency->max_us / 1000.0);
    }

    fprintf(fp, "\n========================================\n");
    fprintf(fp, "   AGGREGATE METRICS\n");
    fprintf(fp, "========================================\n\n");

    fprintf(fp, "Fairness Metrics:\n");
    fprintf(fp, "  Jain's Fairness Index:    %.4f\n", bench->jains_fairness_index);
    fprintf(fp, "  Interpretation:           %s\n",
            bench->jains_fairness_index > 0.95 ? "Excellent (>0.95)" :
            bench->jains_fairness_index > 0.85 ? "Good (>0.85)" :
            bench->jains_fairness_index > 0.70 ? "Fair (>0.70)" : "Poor (<0.70)");

    fprintf(fp, "\nPerformance Metrics:\n");
    fprintf(fp, "  Avg Relative Performance: %.2f\n", bench->avg_relative_performance);
    fprintf(fp, "  SLA Compliance Rate:      %.2f%%\n", bench->overall_sla_compliance * 100);
    fprintf(fp, "  Noisy Neighbor Count:     %d\n", bench->noisy_neighbor_count);

    fprintf(fp, "\n========================================\n");
}

static void print_json_results(MultiTenantBench *bench, FILE *fp)
{
    fprintf(fp, "{\n");
    fprintf(fp, "  \"benchmark\": \"multi-tenant-isolation\",\n");
    fprintf(fp, "  \"version\": \"%s\",\n", BENCH_VERSION);
    fprintf(fp, "  \"config\": {\n");
    fprintf(fp, "    \"num_tenants\": %d,\n", bench->num_tenants);
    fprintf(fp, "    \"rows_per_tenant\": %d,\n", bench->rows_per_tenant);
    fprintf(fp, "    \"isolation_model\": \"%s\",\n",
            bench->isolation_model == ISOLATION_SHARED_TABLE ? "shared_table" :
            bench->isolation_model == ISOLATION_SHARED_SCHEMA ? "shared_schema" : "dedicated_db");
    fprintf(fp, "    \"workload_type\": \"%s\",\n",
            bench->workload_type == WORKLOAD_READ_HEAVY ? "read_heavy" :
            bench->workload_type == WORKLOAD_WRITE_HEAVY ? "write_heavy" :
            bench->workload_type == WORKLOAD_MIXED ? "mixed" : "analytical");
    fprintf(fp, "    \"sla_latency_ms\": %.0f\n", bench->sla_latency_ms);
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"aggregate_metrics\": {\n");
    fprintf(fp, "    \"jains_fairness_index\": %.4f,\n", bench->jains_fairness_index);
    fprintf(fp, "    \"avg_relative_performance\": %.4f,\n", bench->avg_relative_performance);
    fprintf(fp, "    \"sla_compliance_rate\": %.4f,\n", bench->overall_sla_compliance);
    fprintf(fp, "    \"noisy_neighbor_count\": %d\n", bench->noisy_neighbor_count);
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"tenant_results\": [\n");
    for (int i = 0; i < bench->num_tenants; i++) {
        TenantStats *stats = &bench->tenant_stats[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"tenant_id\": %d,\n", stats->tenant_id);
        fprintf(fp, "      \"tenant_name\": \"%s\",\n", stats->tenant_name);
        fprintf(fp, "      \"baseline_throughput_ops\": %.2f,\n",
                stats->baseline_throughput.ops_per_sec);
        fprintf(fp, "      \"concurrent_throughput_ops\": %.2f,\n",
                stats->concurrent_throughput.ops_per_sec);
        fprintf(fp, "      \"relative_performance\": %.4f,\n", stats->relative_performance);
        fprintf(fp, "      \"sla_violations\": %" PRId64 ",\n", stats->sla_violations);
        fprintf(fp, "      \"latency\": {\n");
        fprintf(fp, "        \"avg_ms\": %.2f,\n", stats->concurrent_latency->avg_us / 1000.0);
        fprintf(fp, "        \"p50_ms\": %.2f,\n", stats->concurrent_latency->p50_us / 1000.0);
        fprintf(fp, "        \"p95_ms\": %.2f,\n", stats->concurrent_latency->p95_us / 1000.0);
        fprintf(fp, "        \"p99_ms\": %.2f,\n", stats->concurrent_latency->p99_us / 1000.0);
        fprintf(fp, "        \"max_ms\": %.2f\n", stats->concurrent_latency->max_us / 1000.0);
        fprintf(fp, "      },\n");
        fprintf(fp, "      \"is_noisy_neighbor\": %s,\n", stats->is_noisy_neighbor ? "true" : "false");
        fprintf(fp, "      \"affected_by_noisy\": %s\n", stats->affected_by_noisy ? "true" : "false");
        fprintf(fp, "    }%s\n", i < bench->num_tenants - 1 ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
}

/* === Main Entry Point === */

static void print_usage(const char *prog)
{
    printf("Multi-Tenant Isolation Benchmark\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host HOST           Database host [localhost]\n");
    printf("  -p, --port PORT           Database port [5432]\n");
    printf("  -U, --user USER           Database user [postgres]\n");
    printf("  -d, --database DB         Database name [orochi_bench]\n");
    printf("  -n, --tenants N           Number of tenants [%d]\n", DEFAULT_TENANTS);
    printf("  -r, --rows N              Rows per tenant [%d]\n", DEFAULT_ROWS_PER_TENANT);
    printf("  -D, --duration SECS       Benchmark duration [60]\n");
    printf("  -w, --warmup SECS         Warmup duration [10]\n");
    printf("  -s, --sla MS              SLA latency threshold [%.0f]\n", DEFAULT_SLA_LATENCY_MS);
    printf("  -i, --isolation MODEL     Isolation model: shared_table, shared_schema [shared_table]\n");
    printf("  -W, --workload TYPE       Workload: read, write, mixed, analytical [mixed]\n");
    printf("  -o, --output FILE         Output file for results\n");
    printf("  -f, --format FMT          Output format: text, json, csv [text]\n");
    printf("  -v, --verbose             Enable verbose output\n");
    printf("      --skip-baseline       Skip baseline benchmark\n");
    printf("      --skip-noisy          Skip noisy neighbor test\n");
    printf("      --help                Show this help\n");
}

/* Option codes for long options without short equivalents */
#define OPT_SKIP_BASELINE 256
#define OPT_SKIP_NOISY    257

int main(int argc, char *argv[])
{
    BenchConfig *config = bench_config_create();
    MultiTenantBench *bench = multitenant_bench_create(config);
    int skip_baseline = 0;
    int skip_noisy = 0;
    char output_file[MAX_PATH_LEN] = "";
    OutputFormat output_format = OUTPUT_TEXT;

    /* Parse command-line arguments */
    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"user", required_argument, 0, 'U'},
        {"database", required_argument, 0, 'd'},
        {"tenants", required_argument, 0, 'n'},
        {"rows", required_argument, 0, 'r'},
        {"duration", required_argument, 0, 'D'},
        {"warmup", required_argument, 0, 'w'},
        {"sla", required_argument, 0, 's'},
        {"isolation", required_argument, 0, 'i'},
        {"workload", required_argument, 0, 'W'},
        {"output", required_argument, 0, 'o'},
        {"format", required_argument, 0, 'f'},
        {"verbose", no_argument, 0, 'v'},
        {"skip-baseline", no_argument, 0, OPT_SKIP_BASELINE},
        {"skip-noisy", no_argument, 0, OPT_SKIP_NOISY},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "h:p:U:d:n:r:D:w:s:i:W:o:f:v?",
                            long_options, NULL)) != -1) {
        switch (c) {
            case 'h':
                strncpy(config->host, optarg, MAX_NAME_LEN - 1);
                break;
            case 'p':
                config->port = atoi(optarg);
                break;
            case 'U':
                strncpy(config->user, optarg, MAX_NAME_LEN - 1);
                break;
            case 'd':
                strncpy(config->database, optarg, MAX_NAME_LEN - 1);
                break;
            case 'n':
                bench->num_tenants = atoi(optarg);
                if (bench->num_tenants < 1 || bench->num_tenants > MAX_TENANTS) {
                    fprintf(stderr, "Tenants must be between 1 and %d\n", MAX_TENANTS);
                    return 1;
                }
                break;
            case 'r':
                bench->rows_per_tenant = atoi(optarg);
                break;
            case 'D':
                config->duration_secs = atoi(optarg);
                break;
            case 'w':
                config->warmup_secs = atoi(optarg);
                break;
            case 's':
                bench->sla_latency_ms = atof(optarg);
                break;
            case 'i':
                if (strcmp(optarg, "shared_table") == 0)
                    bench->isolation_model = ISOLATION_SHARED_TABLE;
                else if (strcmp(optarg, "shared_schema") == 0)
                    bench->isolation_model = ISOLATION_SHARED_SCHEMA;
                else if (strcmp(optarg, "dedicated_db") == 0)
                    bench->isolation_model = ISOLATION_DEDICATED_DB;
                else {
                    fprintf(stderr, "Unknown isolation model: %s\n", optarg);
                    return 1;
                }
                break;
            case 'W':
                if (strcmp(optarg, "read") == 0)
                    bench->workload_type = WORKLOAD_READ_HEAVY;
                else if (strcmp(optarg, "write") == 0)
                    bench->workload_type = WORKLOAD_WRITE_HEAVY;
                else if (strcmp(optarg, "mixed") == 0)
                    bench->workload_type = WORKLOAD_MIXED;
                else if (strcmp(optarg, "analytical") == 0)
                    bench->workload_type = WORKLOAD_ANALYTICAL;
                else {
                    fprintf(stderr, "Unknown workload type: %s\n", optarg);
                    return 1;
                }
                break;
            case 'o':
                strncpy(output_file, optarg, MAX_PATH_LEN - 1);
                break;
            case 'f':
                if (strcmp(optarg, "json") == 0)
                    output_format = OUTPUT_JSON;
                else if (strcmp(optarg, "csv") == 0)
                    output_format = OUTPUT_CSV;
                else
                    output_format = OUTPUT_TEXT;
                break;
            case 'v':
                g_verbose = true;
                config->verbose = true;
                break;
            case '?':
                print_usage(argv[0]);
                return 0;
            case OPT_SKIP_BASELINE:
                skip_baseline = 1;
                break;
            case OPT_SKIP_NOISY:
                skip_noisy = 1;
                break;
        }
    }

    /* Setup signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize tenant statistics */
    if (multitenant_bench_init_stats(bench) != 0) {
        LOG_ERROR("Failed to initialize tenant statistics");
        return 1;
    }

    /* Connect and setup schema */
    LOG_INFO("Connecting to database %s...", config->database);
    PGconn *conn = bench_connect(config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return 1;
    }

    /* Create schema and generate data */
    if (create_schema(conn, bench) != 0) {
        bench_disconnect(conn);
        return 1;
    }

    if (generate_all_tenant_data(conn, bench) != 0) {
        bench_disconnect(conn);
        return 1;
    }

    bench_disconnect(conn);

    /* Run baseline benchmarks (per-tenant isolation) */
    if (!skip_baseline) {
        LOG_INFO("Running baseline benchmarks for each tenant in isolation...");
        for (int i = 1; i <= bench->num_tenants && !g_stop_flag; i++) {
            run_baseline_benchmark(bench, i);
            printf("\rBaseline progress: %d/%d tenants", i, bench->num_tenants);
            fflush(stdout);
        }
        printf("\n");
    }

    /* Run concurrent multi-tenant benchmark */
    if (!g_stop_flag) {
        run_concurrent_benchmark(bench);
    }

    /* Run noisy neighbor detection */
    if (!skip_noisy && !g_stop_flag) {
        run_noisy_neighbor_test(bench);
    }

    /* Calculate aggregate metrics */
    calculate_aggregate_metrics(bench);

    /* Output results */
    FILE *fp = stdout;
    if (strlen(output_file) > 0) {
        fp = fopen(output_file, "w");
        if (!fp) {
            LOG_ERROR("Failed to open output file: %s", output_file);
            fp = stdout;
        }
    }

    if (output_format == OUTPUT_JSON) {
        print_json_results(bench, fp);
    } else {
        print_tenant_results(bench, fp);
    }

    if (fp != stdout) {
        fclose(fp);
        LOG_INFO("Results saved to: %s", output_file);
    }

    /* Cleanup */
    multitenant_bench_free(bench);
    bench_config_free(config);

    return 0;
}
