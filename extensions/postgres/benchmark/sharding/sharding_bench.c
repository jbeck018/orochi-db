/*
 * Orochi DB - Sharding/Distribution Benchmark Suite
 *
 * Comprehensive benchmarks for measuring sharding performance:
 * - Data distribution uniformity (Relative Standard Deviation)
 * - Shard key effectiveness (hash vs range)
 * - Cross-shard query routing efficiency
 * - Rebalancing overhead measurement
 * - Scalability factor calculation
 *
 * Key Metrics:
 * - Skew Score = StdDev / Mean * 100 (target: <10%)
 * - Cross-shard query ratio
 * - Rebalancing performance drop percentage
 * - Scalability Factor = Throughput(N) / (Throughput(1) * N)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <inttypes.h>
#include <stdbool.h>
#include "../common/bench_common.h"

/* Benchmark modes */
typedef enum {
    MODE_DISTRIBUTION,      /* Distribution uniformity tests */
    MODE_HASH_VS_RANGE,     /* Hash vs range comparison */
    MODE_ROUTING,           /* Query routing efficiency */
    MODE_REBALANCE,         /* Rebalancing overhead */
    MODE_SCALABILITY,       /* Scalability factor tests */
    MODE_ALL
} ShardingBenchMode;

/* Shard distribution strategy for comparison */
typedef enum {
    STRATEGY_HASH,
    STRATEGY_RANGE,
    STRATEGY_LIST,
    STRATEGY_COMPOSITE
} ShardStrategy;

/* Distribution statistics */
typedef struct {
    int64_t shard_id;
    int64_t row_count;
    int64_t size_bytes;
    double percentage;
} ShardStats;

/* Distribution analysis result */
typedef struct {
    int num_shards;
    ShardStats *shards;
    double mean_rows;
    double stddev_rows;
    double rsd_percent;         /* Relative Standard Deviation */
    double skew_score;          /* Skew Score = StdDev / Mean * 100 */
    int64_t min_rows;
    int64_t max_rows;
    double imbalance_ratio;
    bool acceptable;            /* RSD < 10% */
    int hotspot_count;          /* Shards with >2x mean */
} DistributionAnalysis;

/* Scalability test result */
typedef struct {
    int shard_count;
    int client_count;
    double throughput_ops;
    double latency_avg_ms;
    double latency_p99_ms;
    double scalability_factor;  /* Throughput(N) / (Throughput(1) * N) */
} ScalabilityResult;

/* Rebalancing metrics */
typedef struct {
    double baseline_throughput;
    double during_rebalance_throughput;
    double after_rebalance_throughput;
    double performance_drop_percent;
    double rebalance_duration_secs;
    int shards_moved;
    int64_t bytes_moved;
} RebalanceMetrics;

/* Benchmark configuration */
typedef struct {
    BenchConfig config;
    ShardingBenchMode mode;

    /* Shard configuration */
    int shard_counts[10];
    int num_shard_configs;
    int client_counts[10];
    int num_client_configs;

    /* Data configuration */
    int64_t num_records;
    const char *distribution_column;
    ShardStrategy strategy;

    /* Test parameters */
    int warmup_secs;
    int test_duration_secs;
    int iterations;

    /* Output */
    char output_file[MAX_PATH_LEN];
} ShardingBenchmark;

/* Worker context for concurrent tests */
typedef struct {
    int worker_id;
    ShardingBenchmark *bench;
    PGconn *conn;
    LatencyStats *stats;
    int64_t ops_completed;
    int64_t local_ops;
    int64_t cross_shard_ops;
    volatile bool *should_stop;
    pthread_t thread;
} ShardWorkerContext;

/* Global variables */
static volatile bool g_should_stop = false;

/* Forward declarations */
static void run_distribution_benchmark(ShardingBenchmark *bench, BenchSuite *suite);
static void run_hash_vs_range_benchmark(ShardingBenchmark *bench, BenchSuite *suite);
static void run_routing_benchmark(ShardingBenchmark *bench, BenchSuite *suite);
static void run_rebalance_benchmark(ShardingBenchmark *bench, BenchSuite *suite);
static void run_scalability_benchmark(ShardingBenchmark *bench, BenchSuite *suite);

/*
 * Calculate Relative Standard Deviation (RSD)
 * RSD = (StdDev / Mean) * 100
 */
static double calculate_rsd(int64_t *values, int count)
{
    if (count == 0) return 0.0;

    /* Calculate mean */
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    double mean = sum / count;

    if (mean == 0.0) return 0.0;

    /* Calculate variance */
    double variance = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = values[i] - mean;
        variance += diff * diff;
    }
    variance /= count;

    /* Calculate standard deviation */
    double stddev = sqrt(variance);

    /* Return RSD as percentage */
    return (stddev / mean) * 100.0;
}

/*
 * Analyze shard distribution
 */
static DistributionAnalysis *analyze_distribution(PGconn *conn, const char *table_name)
{
    char sql[MAX_QUERY_LEN];
    PGresult *res;
    DistributionAnalysis *analysis = calloc(1, sizeof(DistributionAnalysis));

    /* Query shard statistics from Orochi catalog */
    snprintf(sql, sizeof(sql),
        "SELECT s.shard_id, s.shard_index, "
        "       COALESCE(s.row_count, 0) as row_count, "
        "       COALESCE(s.size_bytes, 0) as size_bytes "
        "FROM orochi.orochi_shards s "
        "JOIN orochi.orochi_tables t ON s.table_oid = t.relid "
        "WHERE t.table_name = '%s' "
        "ORDER BY s.shard_index", table_name);

    res = bench_query(conn, sql);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        /* Fallback: estimate from physical shard tables */
        if (res) PQclear(res);

        snprintf(sql, sizeof(sql),
            "SELECT tablename, "
            "       pg_stat_get_live_tuples(c.oid) as row_count, "
            "       pg_total_relation_size(c.oid) as size_bytes "
            "FROM pg_tables t "
            "JOIN pg_class c ON c.relname = t.tablename "
            "WHERE tablename LIKE '%s_shard_%%' "
            "ORDER BY tablename", table_name);

        res = bench_query(conn, sql);
        if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
            if (res) PQclear(res);
            free(analysis);
            return NULL;
        }
    }

    int num_shards = PQntuples(res);
    if (num_shards == 0) {
        PQclear(res);
        free(analysis);
        return NULL;
    }

    analysis->num_shards = num_shards;
    analysis->shards = calloc(num_shards, sizeof(ShardStats));

    int64_t *row_counts = malloc(num_shards * sizeof(int64_t));
    int64_t total_rows = 0;
    analysis->min_rows = INT64_MAX;
    analysis->max_rows = 0;

    for (int i = 0; i < num_shards; i++) {
        analysis->shards[i].shard_id = atoll(PQgetvalue(res, i, 0));
        analysis->shards[i].row_count = atoll(PQgetvalue(res, i, 2));
        analysis->shards[i].size_bytes = atoll(PQgetvalue(res, i, 3));

        row_counts[i] = analysis->shards[i].row_count;
        total_rows += analysis->shards[i].row_count;

        if (analysis->shards[i].row_count < analysis->min_rows) {
            analysis->min_rows = analysis->shards[i].row_count;
        }
        if (analysis->shards[i].row_count > analysis->max_rows) {
            analysis->max_rows = analysis->shards[i].row_count;
        }
    }
    PQclear(res);

    /* Calculate percentages */
    for (int i = 0; i < num_shards; i++) {
        analysis->shards[i].percentage =
            total_rows > 0 ? (double)analysis->shards[i].row_count / total_rows * 100.0 : 0.0;
    }

    /* Calculate statistics */
    analysis->mean_rows = (double)total_rows / num_shards;
    analysis->rsd_percent = calculate_rsd(row_counts, num_shards);
    analysis->skew_score = analysis->rsd_percent;  /* Same metric */

    /* Calculate standard deviation */
    double variance = 0.0;
    for (int i = 0; i < num_shards; i++) {
        double diff = row_counts[i] - analysis->mean_rows;
        variance += diff * diff;
    }
    analysis->stddev_rows = sqrt(variance / num_shards);

    /* Calculate imbalance ratio */
    if (analysis->min_rows > 0) {
        analysis->imbalance_ratio = (double)analysis->max_rows / analysis->min_rows;
    } else {
        analysis->imbalance_ratio = analysis->max_rows > 0 ? 999.0 : 1.0;
    }

    /* Count hotspots (shards with >2x mean) */
    analysis->hotspot_count = 0;
    for (int i = 0; i < num_shards; i++) {
        if (row_counts[i] > 2.0 * analysis->mean_rows) {
            analysis->hotspot_count++;
        }
    }

    /* RSD < 10% is acceptable */
    analysis->acceptable = (analysis->rsd_percent < 10.0);

    free(row_counts);
    return analysis;
}

/*
 * Free distribution analysis
 */
static void free_distribution_analysis(DistributionAnalysis *analysis)
{
    if (analysis) {
        if (analysis->shards) free(analysis->shards);
        free(analysis);
    }
}

/*
 * Print distribution analysis
 */
static void print_distribution_analysis(DistributionAnalysis *analysis, const char *label)
{
    printf("\n  Distribution Analysis: %s\n", label);
    printf("  %s\n", "------------------------------------------------------------");
    printf("  Shards:           %d\n", analysis->num_shards);
    printf("  Mean rows/shard:  %.2f\n", analysis->mean_rows);
    printf("  Std deviation:    %.2f\n", analysis->stddev_rows);
    printf("  RSD (Skew Score): %.2f%% %s\n",
           analysis->rsd_percent,
           analysis->acceptable ? "(ACCEPTABLE)" : "(NEEDS ATTENTION)");
    printf("  Min rows:         %" PRId64 "\n", analysis->min_rows);
    printf("  Max rows:         %" PRId64 "\n", analysis->max_rows);
    printf("  Imbalance ratio:  %.2f\n", analysis->imbalance_ratio);
    printf("  Hotspots:         %d\n", analysis->hotspot_count);

    printf("\n  Per-Shard Distribution:\n");
    printf("  %-10s %15s %15s %10s\n", "Shard", "Row Count", "Size (MB)", "Percent");
    printf("  %s\n", "------------------------------------------------------------");

    for (int i = 0; i < analysis->num_shards; i++) {
        ShardStats *s = &analysis->shards[i];
        char marker[4] = "";
        if (s->row_count > 2.0 * analysis->mean_rows) {
            strcpy(marker, " **");  /* Hotspot marker */
        }
        printf("  %-10" PRId64 " %15" PRId64 " %15.2f %9.2f%%%s\n",
               s->shard_id, s->row_count,
               s->size_bytes / (1024.0 * 1024.0),
               s->percentage, marker);
    }
}

/*
 * Create test schema for sharding benchmarks
 */
static void create_test_schema(PGconn *conn, int shard_count, ShardStrategy strategy)
{
    char sql[MAX_QUERY_LEN];

    LOG_INFO("Creating sharding benchmark schema with %d shards...", shard_count);

    /* Drop existing tables */
    bench_exec(conn, "DROP TABLE IF EXISTS shard_bench_orders CASCADE");
    bench_exec(conn, "DROP TABLE IF EXISTS shard_bench_customers CASCADE");
    bench_exec(conn, "DROP TABLE IF EXISTS shard_bench_events CASCADE");

    /* Create customers table (hash distributed) */
    bench_exec(conn,
        "CREATE TABLE shard_bench_customers ("
        "  customer_id   BIGSERIAL PRIMARY KEY,"
        "  name          VARCHAR(100) NOT NULL,"
        "  email         VARCHAR(100) NOT NULL,"
        "  region        VARCHAR(32) NOT NULL,"
        "  created_at    TIMESTAMPTZ DEFAULT NOW()"
        ")");

    /* Create orders table (hash distributed, co-located with customers) */
    bench_exec(conn,
        "CREATE TABLE shard_bench_orders ("
        "  order_id      BIGSERIAL,"
        "  customer_id   BIGINT NOT NULL,"
        "  order_date    TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "  status        VARCHAR(20) NOT NULL DEFAULT 'pending',"
        "  total_amount  DECIMAL(12,2) NOT NULL,"
        "  PRIMARY KEY (customer_id, order_id)"
        ")");

    /* Create events table (range distributed by time for comparison) */
    bench_exec(conn,
        "CREATE TABLE shard_bench_events ("
        "  event_id      BIGSERIAL,"
        "  event_time    TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "  event_type    VARCHAR(50) NOT NULL,"
        "  payload       JSONB,"
        "  PRIMARY KEY (event_time, event_id)"
        ")");

    /* Create indexes */
    bench_exec(conn, "CREATE INDEX idx_shard_orders_date ON shard_bench_orders(order_date)");
    bench_exec(conn, "CREATE INDEX idx_shard_events_type ON shard_bench_events(event_type)");

    /* Distribute tables using Orochi (if available) */
    int strategy_code = (strategy == STRATEGY_HASH) ? 0 : 1;

    snprintf(sql, sizeof(sql),
        "SELECT orochi_create_distributed_table("
        "  'shard_bench_customers'::regclass, 'customer_id', %d, %d)",
        strategy_code, shard_count);
    bench_exec(conn, sql);

    snprintf(sql, sizeof(sql),
        "SELECT orochi_create_distributed_table("
        "  'shard_bench_orders'::regclass, 'customer_id', %d, %d)",
        strategy_code, shard_count);
    bench_exec(conn, sql);

    /* Events table with range distribution (by time) */
    if (strategy == STRATEGY_RANGE) {
        snprintf(sql, sizeof(sql),
            "SELECT orochi_create_distributed_table("
            "  'shard_bench_events'::regclass, 'event_time', 1, %d)",
            shard_count);
        bench_exec(conn, sql);
    }

    LOG_INFO("Schema created successfully");
}

/*
 * Populate test data
 */
static void populate_test_data(PGconn *conn, int64_t num_customers, int orders_per_customer)
{
    char sql[MAX_QUERY_LEN];
    Timer timer;

    LOG_INFO("Populating test data...");

    /* Insert customers */
    LOG_INFO("  Inserting %" PRId64 " customers...", num_customers);
    snprintf(sql, sizeof(sql),
        "INSERT INTO shard_bench_customers (name, email, region) "
        "SELECT "
        "  'Customer ' || g, "
        "  'customer' || g || '@example.com', "
        "  (ARRAY['us-east','us-west','eu-west','eu-east','ap-south','ap-north'])"
        "    [1 + (random() * 5)::int] "
        "FROM generate_series(1, %" PRId64 ") g", num_customers);

    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);
    LOG_INFO("    Completed in %.2f seconds", timer_elapsed_secs(&timer));

    /* Insert orders */
    int64_t total_orders = num_customers * orders_per_customer;
    LOG_INFO("  Inserting %" PRId64 " orders...", total_orders);
    snprintf(sql, sizeof(sql),
        "INSERT INTO shard_bench_orders (customer_id, order_date, status, total_amount) "
        "SELECT "
        "  c.customer_id, "
        "  NOW() - (random() * INTERVAL '365 days'), "
        "  (ARRAY['pending','processing','shipped','delivered','cancelled'])"
        "    [1 + (random() * 4)::int], "
        "  (random() * 1000)::decimal(12,2) "
        "FROM shard_bench_customers c, generate_series(1, %d)",
        orders_per_customer);

    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);
    LOG_INFO("    Completed in %.2f seconds", timer_elapsed_secs(&timer));

    /* Insert events (time-distributed) */
    int64_t num_events = num_customers * 5;
    LOG_INFO("  Inserting %" PRId64 " events...", num_events);
    snprintf(sql, sizeof(sql),
        "INSERT INTO shard_bench_events (event_time, event_type, payload) "
        "SELECT "
        "  NOW() - (random() * INTERVAL '90 days'), "
        "  (ARRAY['page_view','click','purchase','signup','logout'])"
        "    [1 + (random() * 4)::int], "
        "  jsonb_build_object('user_id', (random() * %" PRId64 ")::int, 'value', random()) "
        "FROM generate_series(1, %" PRId64 ")", num_customers, num_events);

    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);
    LOG_INFO("    Completed in %.2f seconds", timer_elapsed_secs(&timer));

    /* Analyze tables */
    LOG_INFO("  Analyzing tables...");
    bench_exec(conn, "ANALYZE shard_bench_customers");
    bench_exec(conn, "ANALYZE shard_bench_orders");
    bench_exec(conn, "ANALYZE shard_bench_events");

    LOG_INFO("Data population complete");
}

/*
 * Run distribution uniformity benchmark
 */
static void run_distribution_benchmark(ShardingBenchmark *bench, BenchSuite *suite)
{
    printf("\n=== Distribution Uniformity Benchmark ===\n");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Test with different shard counts */
    for (int i = 0; i < bench->num_shard_configs; i++) {
        int shard_count = bench->shard_counts[i];

        printf("\n--- Testing with %d shards ---\n", shard_count);

        /* Create schema and populate data */
        create_test_schema(conn, shard_count, STRATEGY_HASH);
        populate_test_data(conn, bench->num_records, 10);

        /* Analyze distribution */
        DistributionAnalysis *analysis = analyze_distribution(conn, "shard_bench_customers");
        if (analysis) {
            char label[64];
            snprintf(label, sizeof(label), "%d Shards (Hash)", shard_count);
            print_distribution_analysis(analysis, label);

            /* Create result */
            char result_name[128];
            snprintf(result_name, sizeof(result_name), "distribution_%d_shards", shard_count);
            BenchResult *result = bench_result_create(result_name);
            snprintf(result->description, sizeof(result->description),
                "Distribution uniformity with %d shards", shard_count);
            result->success = analysis->acceptable;
            result->compression_ratio = analysis->rsd_percent;  /* Reuse for RSD */
            result->total_rows = (int64_t)analysis->mean_rows * shard_count;

            bench_suite_add_result(suite, result);
            bench_result_free(result);
            free_distribution_analysis(analysis);
        }

        /* Also test orders table (co-located) */
        analysis = analyze_distribution(conn, "shard_bench_orders");
        if (analysis) {
            char label[64];
            snprintf(label, sizeof(label), "%d Shards (Orders)", shard_count);
            print_distribution_analysis(analysis, label);
            free_distribution_analysis(analysis);
        }
    }

    bench_disconnect(conn);
}

/*
 * Hash vs Range distribution comparison
 */
static void run_hash_vs_range_benchmark(ShardingBenchmark *bench, BenchSuite *suite)
{
    printf("\n=== Hash vs Range Distribution Benchmark ===\n");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    int shard_count = bench->shard_counts[0];

    /* Test Hash distribution */
    printf("\n--- Hash Distribution Test ---\n");
    create_test_schema(conn, shard_count, STRATEGY_HASH);
    populate_test_data(conn, bench->num_records, 10);

    DistributionAnalysis *hash_analysis = analyze_distribution(conn, "shard_bench_customers");
    if (hash_analysis) {
        print_distribution_analysis(hash_analysis, "Hash Distribution");
    }

    /* Test Range distribution */
    printf("\n--- Range Distribution Test (Time-based) ---\n");
    create_test_schema(conn, shard_count, STRATEGY_RANGE);
    populate_test_data(conn, bench->num_records, 10);

    DistributionAnalysis *range_analysis = analyze_distribution(conn, "shard_bench_events");
    if (range_analysis) {
        print_distribution_analysis(range_analysis, "Range Distribution");
    }

    /* Comparison summary */
    printf("\n  Strategy Comparison:\n");
    printf("  %-20s %15s %15s\n", "Metric", "Hash", "Range");
    printf("  %s\n", "------------------------------------------------------");

    if (hash_analysis && range_analysis) {
        printf("  %-20s %14.2f%% %14.2f%%\n", "RSD (Skew Score)",
               hash_analysis->rsd_percent, range_analysis->rsd_percent);
        printf("  %-20s %15.2f %15.2f\n", "Imbalance Ratio",
               hash_analysis->imbalance_ratio, range_analysis->imbalance_ratio);
        printf("  %-20s %15d %15d\n", "Hotspots",
               hash_analysis->hotspot_count, range_analysis->hotspot_count);
        printf("  %-20s %15s %15s\n", "Acceptable",
               hash_analysis->acceptable ? "YES" : "NO",
               range_analysis->acceptable ? "YES" : "NO");

        /* Create comparison result */
        BenchResult *result = bench_result_create("hash_vs_range");
        snprintf(result->description, sizeof(result->description),
            "Hash vs Range distribution comparison");
        result->success = hash_analysis->rsd_percent <= range_analysis->rsd_percent;
        bench_suite_add_result(suite, result);
        bench_result_free(result);
    }

    if (hash_analysis) free_distribution_analysis(hash_analysis);
    if (range_analysis) free_distribution_analysis(range_analysis);
    bench_disconnect(conn);
}

/*
 * Query routing worker
 */
static void *routing_worker(void *arg)
{
    ShardWorkerContext *ctx = (ShardWorkerContext *)arg;
    char sql[1024];
    Timer timer;

    ctx->conn = bench_connect(&ctx->bench->config);
    if (!ctx->conn) return NULL;

    srand(time(NULL) ^ ctx->worker_id);

    while (!*ctx->should_stop) {
        /* Mix of local and cross-shard queries */
        int query_type = rand() % 4;
        int customer_id = rand() % 10000 + 1;

        timer_start(&timer);

        switch (query_type) {
            case 0:
                /* Local: single customer orders */
                snprintf(sql, sizeof(sql),
                    "SELECT * FROM shard_bench_orders WHERE customer_id = %d",
                    customer_id);
                ctx->local_ops++;
                break;

            case 1:
                /* Local: customer with orders (co-located join) */
                snprintf(sql, sizeof(sql),
                    "SELECT c.*, COUNT(o.order_id) as order_count "
                    "FROM shard_bench_customers c "
                    "LEFT JOIN shard_bench_orders o ON c.customer_id = o.customer_id "
                    "WHERE c.customer_id = %d "
                    "GROUP BY c.customer_id, c.name, c.email, c.region, c.created_at",
                    customer_id);
                ctx->local_ops++;
                break;

            case 2:
                /* Cross-shard: aggregate all */
                snprintf(sql, sizeof(sql), "%s",
                    "SELECT COUNT(*), SUM(total_amount) FROM shard_bench_orders");
                ctx->cross_shard_ops++;
                break;

            case 3:
                /* Cross-shard: top customers */
                snprintf(sql, sizeof(sql), "%s",
                    "SELECT customer_id, SUM(total_amount) as total "
                    "FROM shard_bench_orders "
                    "GROUP BY customer_id ORDER BY total DESC LIMIT 10");
                ctx->cross_shard_ops++;
                break;
        }

        PGresult *res = bench_query(ctx->conn, sql);
        timer_stop(&timer);

        if (res) {
            latency_stats_add(ctx->stats, timer_elapsed_us(&timer));
            ctx->ops_completed++;
            PQclear(res);
        }
    }

    bench_disconnect(ctx->conn);
    return NULL;
}

/*
 * Run query routing efficiency benchmark
 */
static void run_routing_benchmark(ShardingBenchmark *bench, BenchSuite *suite)
{
    printf("\n=== Query Routing Efficiency Benchmark ===\n");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Setup with default shard count */
    create_test_schema(conn, bench->shard_counts[0], STRATEGY_HASH);
    populate_test_data(conn, bench->num_records, 10);
    bench_disconnect(conn);

    /* Test with different client counts */
    for (int c = 0; c < bench->num_client_configs; c++) {
        int num_clients = bench->client_counts[c];

        printf("\n--- Routing Test: %d clients ---\n", num_clients);

        ShardWorkerContext *workers = calloc(num_clients, sizeof(ShardWorkerContext));
        volatile bool should_stop = false;

        /* Initialize workers */
        for (int i = 0; i < num_clients; i++) {
            workers[i].worker_id = i;
            workers[i].bench = bench;
            workers[i].stats = latency_stats_create(10000);
            workers[i].should_stop = &should_stop;
        }

        Timer total_timer;
        timer_start(&total_timer);

        /* Start workers */
        for (int i = 0; i < num_clients; i++) {
            pthread_create(&workers[i].thread, NULL, routing_worker, &workers[i]);
        }

        /* Run for duration */
        sleep(bench->test_duration_secs);
        should_stop = true;

        /* Wait for workers */
        for (int i = 0; i < num_clients; i++) {
            pthread_join(workers[i].thread, NULL);
        }

        timer_stop(&total_timer);

        /* Aggregate results */
        int64_t total_ops = 0;
        int64_t total_local = 0;
        int64_t total_cross = 0;
        LatencyStats *combined = latency_stats_create(num_clients * 10000);

        for (int i = 0; i < num_clients; i++) {
            total_ops += workers[i].ops_completed;
            total_local += workers[i].local_ops;
            total_cross += workers[i].cross_shard_ops;
            latency_stats_merge(combined, workers[i].stats);
            latency_stats_free(workers[i].stats);
        }

        latency_stats_compute(combined);
        double duration = timer_elapsed_secs(&total_timer);
        double cross_shard_ratio = (double)total_cross / total_ops * 100.0;

        printf("  Results:\n");
        printf("    Clients:             %d\n", num_clients);
        printf("    Duration:            %.2f seconds\n", duration);
        printf("    Total operations:    %" PRId64 "\n", total_ops);
        printf("    Throughput:          %.2f ops/sec\n", total_ops / duration);
        printf("    Local queries:       %" PRId64 " (%.1f%%)\n",
               total_local, (double)total_local / total_ops * 100.0);
        printf("    Cross-shard queries: %" PRId64 " (%.1f%%)\n",
               total_cross, cross_shard_ratio);
        printf("    Latency (avg):       %.2f ms\n", combined->avg_us / 1000.0);
        printf("    Latency (p95):       %.2f ms\n", combined->p95_us / 1000.0);
        printf("    Latency (p99):       %.2f ms\n", combined->p99_us / 1000.0);

        /* Create result */
        char result_name[128];
        snprintf(result_name, sizeof(result_name), "routing_%d_clients", num_clients);
        BenchResult *result = bench_result_create(result_name);
        snprintf(result->description, sizeof(result->description),
            "Query routing with %d clients", num_clients);
        result->latency = *combined;
        throughput_stats_compute(&result->throughput, total_ops, 0, 0, duration);
        result->total_queries = total_ops;
        result->compression_ratio = cross_shard_ratio;  /* Reuse for cross-shard ratio */
        result->success = true;

        bench_suite_add_result(suite, result);
        bench_result_free(result);
        latency_stats_free(combined);
        free(workers);
    }
}

/*
 * Run rebalancing overhead benchmark
 */
static void run_rebalance_benchmark(ShardingBenchmark *bench, BenchSuite *suite)
{
    printf("\n=== Rebalancing Overhead Benchmark ===\n");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    int shard_count = bench->shard_counts[0];

    /* Setup */
    create_test_schema(conn, shard_count, STRATEGY_HASH);
    populate_test_data(conn, bench->num_records, 10);

    /* Phase 1: Establish baseline throughput */
    printf("\n--- Phase 1: Baseline Measurement ---\n");

    volatile bool should_stop = false;
    ShardWorkerContext *workers = calloc(4, sizeof(ShardWorkerContext));

    for (int i = 0; i < 4; i++) {
        workers[i].worker_id = i;
        workers[i].bench = bench;
        workers[i].stats = latency_stats_create(10000);
        workers[i].should_stop = &should_stop;
    }

    Timer baseline_timer;
    timer_start(&baseline_timer);

    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i].thread, NULL, routing_worker, &workers[i]);
    }

    sleep(15);  /* 15 second baseline */
    should_stop = true;

    for (int i = 0; i < 4; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    timer_stop(&baseline_timer);

    int64_t baseline_ops = 0;
    for (int i = 0; i < 4; i++) {
        baseline_ops += workers[i].ops_completed;
    }
    double baseline_throughput = baseline_ops / timer_elapsed_secs(&baseline_timer);
    printf("  Baseline throughput: %.2f ops/sec\n", baseline_throughput);

    /* Phase 2: Trigger rebalancing and measure during */
    printf("\n--- Phase 2: During Rebalancing ---\n");

    /* Reset workers */
    should_stop = false;
    for (int i = 0; i < 4; i++) {
        workers[i].ops_completed = 0;
        workers[i].local_ops = 0;
        workers[i].cross_shard_ops = 0;
        latency_stats_reset(workers[i].stats);
    }

    Timer rebalance_timer;
    timer_start(&rebalance_timer);

    /* Start workers again */
    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i].thread, NULL, routing_worker, &workers[i]);
    }

    /* Trigger rebalancing (in a separate thread) */
    bench_exec(conn,
        "SELECT orochi_rebalance_table('shard_bench_customers'::regclass)");
    bench_exec(conn,
        "SELECT orochi_rebalance_table('shard_bench_orders'::regclass)");

    sleep(15);  /* 15 seconds during rebalance */
    should_stop = true;

    for (int i = 0; i < 4; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    timer_stop(&rebalance_timer);

    int64_t rebalance_ops = 0;
    for (int i = 0; i < 4; i++) {
        rebalance_ops += workers[i].ops_completed;
    }
    double rebalance_throughput = rebalance_ops / timer_elapsed_secs(&rebalance_timer);
    printf("  During rebalance throughput: %.2f ops/sec\n", rebalance_throughput);

    /* Phase 3: After rebalancing */
    printf("\n--- Phase 3: After Rebalancing ---\n");

    should_stop = false;
    for (int i = 0; i < 4; i++) {
        workers[i].ops_completed = 0;
        latency_stats_reset(workers[i].stats);
    }

    Timer after_timer;
    timer_start(&after_timer);

    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i].thread, NULL, routing_worker, &workers[i]);
    }

    sleep(15);
    should_stop = true;

    for (int i = 0; i < 4; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    timer_stop(&after_timer);

    int64_t after_ops = 0;
    for (int i = 0; i < 4; i++) {
        after_ops += workers[i].ops_completed;
        latency_stats_free(workers[i].stats);
    }
    double after_throughput = after_ops / timer_elapsed_secs(&after_timer);
    printf("  After rebalance throughput: %.2f ops/sec\n", after_throughput);

    /* Calculate metrics */
    double performance_drop = (1.0 - rebalance_throughput / baseline_throughput) * 100.0;
    double recovery_percent = after_throughput / baseline_throughput * 100.0;

    printf("\n  Rebalancing Impact Summary:\n");
    printf("    Baseline throughput:      %.2f ops/sec\n", baseline_throughput);
    printf("    During rebalance:         %.2f ops/sec\n", rebalance_throughput);
    printf("    After rebalance:          %.2f ops/sec\n", after_throughput);
    printf("    Performance drop:         %.2f%%\n", performance_drop);
    printf("    Recovery:                 %.2f%% of baseline\n", recovery_percent);

    /* Create result */
    BenchResult *result = bench_result_create("rebalancing_overhead");
    snprintf(result->description, sizeof(result->description),
        "Rebalancing overhead measurement");
    result->throughput.ops_per_sec = rebalance_throughput;
    result->compression_ratio = performance_drop;  /* Reuse for performance drop */
    result->success = performance_drop < 50.0;  /* Less than 50% drop is acceptable */

    bench_suite_add_result(suite, result);
    bench_result_free(result);
    free(workers);
    bench_disconnect(conn);
}

/*
 * Run scalability benchmark
 */
static void run_scalability_benchmark(ShardingBenchmark *bench, BenchSuite *suite)
{
    printf("\n=== Scalability Factor Benchmark ===\n");
    printf("  Scalability Factor = Throughput(N) / (Throughput(1) * N)\n");
    printf("  Target: >0.7 (70%% linear scaling)\n\n");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    ScalabilityResult results[20];
    int result_count = 0;

    /* Baseline: 1 shard, 1 client */
    double baseline_throughput = 0.0;

    for (int s = 0; s < bench->num_shard_configs; s++) {
        int shard_count = bench->shard_counts[s];

        /* Create schema with this shard count */
        create_test_schema(conn, shard_count, STRATEGY_HASH);
        populate_test_data(conn, bench->num_records, 10);
        bench_disconnect(conn);

        for (int c = 0; c < bench->num_client_configs; c++) {
            int client_count = bench->client_counts[c];

            printf("--- Testing: %d shards, %d clients ---\n", shard_count, client_count);

            volatile bool should_stop = false;
            ShardWorkerContext *workers = calloc(client_count, sizeof(ShardWorkerContext));

            for (int i = 0; i < client_count; i++) {
                workers[i].worker_id = i;
                workers[i].bench = bench;
                workers[i].stats = latency_stats_create(10000);
                workers[i].should_stop = &should_stop;
            }

            Timer timer;
            timer_start(&timer);

            for (int i = 0; i < client_count; i++) {
                pthread_create(&workers[i].thread, NULL, routing_worker, &workers[i]);
            }

            sleep(bench->test_duration_secs);
            should_stop = true;

            for (int i = 0; i < client_count; i++) {
                pthread_join(workers[i].thread, NULL);
            }

            timer_stop(&timer);

            int64_t total_ops = 0;
            LatencyStats *combined = latency_stats_create(client_count * 10000);

            for (int i = 0; i < client_count; i++) {
                total_ops += workers[i].ops_completed;
                latency_stats_merge(combined, workers[i].stats);
                latency_stats_free(workers[i].stats);
            }

            latency_stats_compute(combined);
            double throughput = total_ops / timer_elapsed_secs(&timer);

            /* Set baseline */
            if (shard_count == bench->shard_counts[0] && client_count == bench->client_counts[0]) {
                baseline_throughput = throughput;
            }

            /* Calculate scalability factor */
            double scale_factor = shard_count * client_count;
            double expected_throughput = baseline_throughput * scale_factor;
            double scalability_factor = throughput / expected_throughput;

            results[result_count].shard_count = shard_count;
            results[result_count].client_count = client_count;
            results[result_count].throughput_ops = throughput;
            results[result_count].latency_avg_ms = combined->avg_us / 1000.0;
            results[result_count].latency_p99_ms = combined->p99_us / 1000.0;
            results[result_count].scalability_factor = scalability_factor;
            result_count++;

            printf("    Throughput:          %.2f ops/sec\n", throughput);
            printf("    Latency (avg):       %.2f ms\n", combined->avg_us / 1000.0);
            printf("    Scalability Factor:  %.2f (%.1f%% linear)\n",
                   scalability_factor, scalability_factor * 100.0);

            latency_stats_free(combined);
            free(workers);
        }

        conn = bench_connect(&bench->config);
    }

    /* Print summary table */
    printf("\n  Scalability Summary:\n");
    printf("  %-10s %-10s %15s %12s %12s %15s\n",
           "Shards", "Clients", "Throughput", "Avg (ms)", "P99 (ms)", "Scale Factor");
    printf("  %s\n",
           "--------------------------------------------------------------------------");

    for (int i = 0; i < result_count; i++) {
        ScalabilityResult *r = &results[i];
        char marker[4] = "";
        if (r->scalability_factor < 0.7) {
            strcpy(marker, " !");  /* Below target */
        }
        printf("  %-10d %-10d %15.2f %12.2f %12.2f %14.2f%s\n",
               r->shard_count, r->client_count,
               r->throughput_ops, r->latency_avg_ms, r->latency_p99_ms,
               r->scalability_factor, marker);

        /* Create result */
        char result_name[128];
        snprintf(result_name, sizeof(result_name), "scalability_%d_shards_%d_clients",
                 r->shard_count, r->client_count);
        BenchResult *result = bench_result_create(result_name);
        snprintf(result->description, sizeof(result->description),
            "Scalability: %d shards, %d clients", r->shard_count, r->client_count);
        result->throughput.ops_per_sec = r->throughput_ops;
        result->latency.avg_us = r->latency_avg_ms * 1000.0;
        result->latency.p99_us = r->latency_p99_ms * 1000.0;
        result->compression_ratio = r->scalability_factor;  /* Reuse for scale factor */
        result->success = r->scalability_factor >= 0.7;

        bench_suite_add_result(suite, result);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

/*
 * Print usage
 */
static void print_usage(const char *prog)
{
    printf("Sharding/Distribution Benchmark for Orochi DB\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --mode=MODE        Mode: distribution, hash-vs-range, routing,\n");
    printf("                           rebalance, scalability, all [default: all]\n");
    printf("  --records=N        Number of base records [default: 10000]\n");
    printf("  --shards=N,N,...   Shard counts to test [default: 2,4,8,16,32,64]\n");
    printf("  --clients=N,N,...  Client counts to test [default: 1,4,8,16,32,64]\n");
    printf("  --duration=N       Test duration per config (seconds) [default: 30]\n");
    printf("  --warmup=N         Warmup duration (seconds) [default: 5]\n");
    printf("  --host=HOST        Database host [default: localhost]\n");
    printf("  --port=PORT        Database port [default: 5432]\n");
    printf("  --user=USER        Database user [default: postgres]\n");
    printf("  --database=DB      Database name [default: orochi_bench]\n");
    printf("  --output=FILE      Output JSON file\n");
    printf("  --verbose          Enable verbose output\n");
    printf("  --help             Show this help\n");
    printf("\nMetrics:\n");
    printf("  - RSD (Skew Score): Target < 10%% for acceptable distribution\n");
    printf("  - Scalability Factor: Target > 0.7 (70%% linear scaling)\n");
    printf("  - Rebalance Drop: Target < 50%% performance impact\n");
}

/*
 * Parse comma-separated integers
 */
static int parse_int_list(const char *str, int *values, int max_count)
{
    int count = 0;
    char *copy = strdup(str);
    char *token = strtok(copy, ",");

    while (token && count < max_count) {
        values[count++] = atoi(token);
        token = strtok(NULL, ",");
    }

    free(copy);
    return count;
}

int main(int argc, char *argv[])
{
    ShardingBenchmark bench = {0};

    /* Initialize defaults */
    BenchConfig *cfg = bench_config_create();
    memcpy(&bench.config, cfg, sizeof(BenchConfig));
    bench_config_free(cfg);

    bench.mode = MODE_ALL;
    bench.num_records = 10000;
    bench.test_duration_secs = 30;
    bench.warmup_secs = 5;
    bench.iterations = 5;

    /* Default shard counts */
    bench.shard_counts[0] = 2;
    bench.shard_counts[1] = 4;
    bench.shard_counts[2] = 8;
    bench.shard_counts[3] = 16;
    bench.shard_counts[4] = 32;
    bench.shard_counts[5] = 64;
    bench.num_shard_configs = 6;

    /* Default client counts */
    bench.client_counts[0] = 1;
    bench.client_counts[1] = 4;
    bench.client_counts[2] = 8;
    bench.client_counts[3] = 16;
    bench.client_counts[4] = 32;
    bench.client_counts[5] = 64;
    bench.num_client_configs = 6;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *mode = argv[i] + 7;
            if (strcmp(mode, "distribution") == 0) bench.mode = MODE_DISTRIBUTION;
            else if (strcmp(mode, "hash-vs-range") == 0) bench.mode = MODE_HASH_VS_RANGE;
            else if (strcmp(mode, "routing") == 0) bench.mode = MODE_ROUTING;
            else if (strcmp(mode, "rebalance") == 0) bench.mode = MODE_REBALANCE;
            else if (strcmp(mode, "scalability") == 0) bench.mode = MODE_SCALABILITY;
            else bench.mode = MODE_ALL;
        }
        else if (strncmp(argv[i], "--records=", 10) == 0) {
            bench.num_records = atoll(argv[i] + 10);
        }
        else if (strncmp(argv[i], "--shards=", 9) == 0) {
            bench.num_shard_configs = parse_int_list(argv[i] + 9,
                bench.shard_counts, 10);
        }
        else if (strncmp(argv[i], "--clients=", 10) == 0) {
            bench.num_client_configs = parse_int_list(argv[i] + 10,
                bench.client_counts, 10);
        }
        else if (strncmp(argv[i], "--duration=", 11) == 0) {
            bench.test_duration_secs = atoi(argv[i] + 11);
        }
        else if (strncmp(argv[i], "--warmup=", 9) == 0) {
            bench.warmup_secs = atoi(argv[i] + 9);
        }
        else if (strncmp(argv[i], "--host=", 7) == 0) {
            strncpy(bench.config.host, argv[i] + 7, MAX_NAME_LEN - 1);
        }
        else if (strncmp(argv[i], "--port=", 7) == 0) {
            bench.config.port = atoi(argv[i] + 7);
        }
        else if (strncmp(argv[i], "--user=", 7) == 0) {
            strncpy(bench.config.user, argv[i] + 7, MAX_NAME_LEN - 1);
        }
        else if (strncmp(argv[i], "--database=", 11) == 0) {
            strncpy(bench.config.database, argv[i] + 11, MAX_NAME_LEN - 1);
        }
        else if (strncmp(argv[i], "--output=", 9) == 0) {
            strncpy(bench.output_file, argv[i] + 9, MAX_PATH_LEN - 1);
        }
        else if (strcmp(argv[i], "--verbose") == 0) {
            bench.config.verbose = true;
            g_verbose = true;
        }
    }

    printf("\n==========================================\n");
    printf("  Sharding Benchmark for Orochi DB\n");
    printf("==========================================\n\n");

    bench_config_print(&bench.config);
    printf("Records: %" PRId64 "\n", bench.num_records);
    printf("Shard configs: ");
    for (int i = 0; i < bench.num_shard_configs; i++) {
        printf("%d%s", bench.shard_counts[i],
               i < bench.num_shard_configs - 1 ? ", " : "\n");
    }
    printf("Client configs: ");
    for (int i = 0; i < bench.num_client_configs; i++) {
        printf("%d%s", bench.client_counts[i],
               i < bench.num_client_configs - 1 ? ", " : "\n");
    }
    printf("Test duration: %d seconds per config\n\n", bench.test_duration_secs);

    /* Create benchmark suite */
    BenchSuite *suite = bench_suite_create("Sharding", 100);
    memcpy(&suite->config, &bench.config, sizeof(BenchConfig));
    bench_suite_collect_system_info(suite);

    /* Run selected benchmarks */
    if (bench.mode == MODE_DISTRIBUTION || bench.mode == MODE_ALL) {
        run_distribution_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_HASH_VS_RANGE || bench.mode == MODE_ALL) {
        run_hash_vs_range_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_ROUTING || bench.mode == MODE_ALL) {
        run_routing_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_REBALANCE || bench.mode == MODE_ALL) {
        run_rebalance_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_SCALABILITY || bench.mode == MODE_ALL) {
        run_scalability_benchmark(&bench, suite);
    }

    suite->end_time = time(NULL);

    /* Print and save results */
    bench_suite_print_summary(suite);

    if (bench.output_file[0]) {
        bench_suite_save(suite, bench.output_file);
    } else {
        char output_file[MAX_PATH_LEN];
        snprintf(output_file, sizeof(output_file), "%s/sharding_results.json",
                 bench.config.output_dir[0] ? bench.config.output_dir : ".");
        bench_suite_save(suite, output_file);
    }

    bench_suite_free(suite);
    return 0;
}
