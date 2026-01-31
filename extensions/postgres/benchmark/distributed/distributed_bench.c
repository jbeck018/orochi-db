/*
 * Orochi DB - Distributed Benchmark Driver
 *
 * Measures cross-shard query performance, shard rebalancing overhead,
 * and network latency impact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include "bench_common.h"

/* Benchmark modes */
typedef enum {
    MODE_CROSS_SHARD,
    MODE_LOCAL,
    MODE_REBALANCE,
    MODE_ALL
} BenchMode;

/* Benchmark configuration */
typedef struct {
    BenchConfig config;
    BenchMode mode;
    int num_shards;
    int64_t num_customers;
    int64_t num_orders;
} DistributedBenchmark;

/* Generate test data */
static void populate_customers(PGconn *conn, int64_t num_customers)
{
    LOG_INFO("Populating customers (%ld rows)...", num_customers);

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO customers_distributed (name, email, region) "
        "SELECT "
        "  'Customer ' || generate_series, "
        "  'customer' || generate_series || '@example.com', "
        "  (ARRAY['us-east','us-west','eu-west','eu-east','ap-south','ap-north'])[1 + (random() * 5)::int] "
        "FROM generate_series(1, %" PRId64 ")", num_customers);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

static void populate_orders(PGconn *conn, int64_t num_customers, int orders_per_customer)
{
    int64_t total_orders = num_customers * orders_per_customer;
    LOG_INFO("Populating orders (%ld rows)...", total_orders);

    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO orders_distributed (customer_id, order_date, status, total_amount) "
        "SELECT "
        "  c.customer_id, "
        "  NOW() - (random() * INTERVAL '365 days'), "
        "  (ARRAY['pending','processing','shipped','delivered','cancelled'])[1 + (random() * 4)::int], "
        "  (random() * 1000)::decimal(12,2) "
        "FROM customers_distributed c, "
        "     generate_series(1, %d)",
        orders_per_customer);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

static void populate_order_items(PGconn *conn)
{
    LOG_INFO("Populating order items...");

    const char *sql =
        "INSERT INTO order_items_distributed (order_id, customer_id, product_id, quantity, unit_price) "
        "SELECT "
        "  o.order_id, "
        "  o.customer_id, "
        "  (random() * 9 + 1)::int, "
        "  (random() * 5 + 1)::int, "
        "  p.price "
        "FROM orders_distributed o "
        "CROSS JOIN LATERAL ("
        "  SELECT product_id, price FROM products_distributed "
        "  ORDER BY random() LIMIT (random() * 4 + 1)::int"
        ") p";

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Cross-shard query definitions */
typedef struct {
    const char *name;
    const char *description;
    const char *sql;
} DistributedQuery;

static DistributedQuery cross_shard_queries[] = {
    {
        "aggregate_all_orders",
        "Aggregate across all shards",
        "SELECT COUNT(*), SUM(total_amount), AVG(total_amount) FROM orders_distributed"
    },
    {
        "join_customers_orders",
        "Join customers with orders (cross-shard)",
        "SELECT c.region, COUNT(*) as order_count, SUM(o.total_amount) as total "
        "FROM customers_distributed c "
        "JOIN orders_distributed o ON c.customer_id = o.customer_id "
        "GROUP BY c.region ORDER BY total DESC"
    },
    {
        "top_customers",
        "Top customers by order value (distributed sort)",
        "SELECT c.customer_id, c.name, SUM(o.total_amount) as total_spent "
        "FROM customers_distributed c "
        "JOIN orders_distributed o ON c.customer_id = o.customer_id "
        "GROUP BY c.customer_id, c.name "
        "ORDER BY total_spent DESC LIMIT 100"
    },
    {
        "cross_shard_subquery",
        "Subquery spanning multiple shards",
        "SELECT * FROM customers_distributed "
        "WHERE customer_id IN ("
        "  SELECT customer_id FROM orders_distributed "
        "  WHERE total_amount > 500 "
        "  GROUP BY customer_id HAVING COUNT(*) > 3"
        ") LIMIT 100"
    },
    {
        "distributed_window",
        "Window function across shards",
        "SELECT customer_id, order_id, total_amount, "
        "  ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY total_amount DESC) as rank "
        "FROM orders_distributed WHERE status = 'delivered' "
        "ORDER BY customer_id, rank LIMIT 1000"
    },
    {
        "multi_table_join",
        "Multi-table join with reference table",
        "SELECT p.category, COUNT(DISTINCT oi.customer_id) as customers, "
        "  SUM(oi.quantity) as total_quantity, SUM(oi.quantity * oi.unit_price) as revenue "
        "FROM order_items_distributed oi "
        "JOIN products_distributed p ON oi.product_id = p.product_id "
        "GROUP BY p.category ORDER BY revenue DESC"
    },
    {NULL, NULL, NULL}
};

static DistributedQuery local_queries[] = {
    {
        "single_customer_orders",
        "Orders for single customer (local to shard)",
        "SELECT * FROM orders_distributed WHERE customer_id = 1 ORDER BY order_date DESC"
    },
    {
        "customer_order_items",
        "Order items for single customer (co-located)",
        "SELECT o.order_id, o.order_date, oi.product_id, oi.quantity, oi.unit_price "
        "FROM orders_distributed o "
        "JOIN order_items_distributed oi ON o.customer_id = oi.customer_id AND o.order_id = oi.order_id "
        "WHERE o.customer_id = 1"
    },
    {
        "customer_total",
        "Total spent by single customer",
        "SELECT SUM(total_amount) FROM orders_distributed WHERE customer_id = 1"
    },
    {
        "recent_customer_orders",
        "Recent orders for customer",
        "SELECT * FROM orders_distributed "
        "WHERE customer_id = 1 AND order_date > NOW() - INTERVAL '30 days' "
        "ORDER BY order_date DESC"
    },
    {NULL, NULL, NULL}
};

/* Run cross-shard benchmark */
static void run_cross_shard_benchmark(DistributedBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Cross-Shard Query Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Populate data if needed */
    int64_t row_count = 0;
    PGresult *res = bench_query(conn, "SELECT COUNT(*) FROM customers_distributed");
    if (res) {
        row_count = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    if (row_count < bench->num_customers) {
        populate_customers(conn, bench->num_customers);
        populate_orders(conn, bench->num_customers, 10);
        populate_order_items(conn);
        bench_exec(conn, "ANALYZE");
    }

    printf("\n  Cross-Shard Query Performance:\n");
    printf("  %-30s %12s %12s %10s\n",
           "Query", "Avg (ms)", "P95 (ms)", "Rows");
    printf("  %s\n",
           "------------------------------------------------------------------------");

    int iterations = 5;

    for (int q = 0; cross_shard_queries[q].name != NULL; q++) {
        DistributedQuery *query = &cross_shard_queries[q];
        BenchResult *result = bench_result_create(query->name);
        strncpy(result->description, query->description,
                sizeof(result->description) - 1);

        LatencyStats *stats = latency_stats_create(iterations);
        Timer timer;

        /* Warmup */
        res = bench_query(conn, query->sql);
        if (res) PQclear(res);

        /* Benchmark */
        for (int i = 0; i < iterations; i++) {
            timer_start(&timer);
            res = bench_query(conn, query->sql);
            timer_stop(&timer);

            if (res) {
                latency_stats_add(stats, timer_elapsed_us(&timer));
                result->total_queries++;
                result->total_rows += PQntuples(res);
                PQclear(res);
            } else {
                result->failed_queries++;
            }
        }

        latency_stats_compute(stats);
        result->latency = *stats;

        printf("  %-30s %10.2fms %10.2fms %10" PRId64 "\n",
               query->name,
               result->latency.avg_us / 1000.0,
               result->latency.p95_us / 1000.0,
               result->total_rows / iterations);

        bench_suite_add_result(suite, result);
        latency_stats_free(stats);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

/* Run local shard benchmark */
static void run_local_benchmark(DistributedBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Local Shard Query Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    printf("\n  Local Query Performance (co-located data):\n");
    printf("  %-30s %12s %12s %10s\n",
           "Query", "Avg (ms)", "P95 (ms)", "Rows");
    printf("  %s\n",
           "------------------------------------------------------------------------");

    int iterations = 10;

    for (int q = 0; local_queries[q].name != NULL; q++) {
        DistributedQuery *query = &local_queries[q];
        BenchResult *result = bench_result_create(query->name);
        strncpy(result->description, query->description,
                sizeof(result->description) - 1);

        LatencyStats *stats = latency_stats_create(iterations);
        Timer timer;

        /* Warmup */
        PGresult *res = bench_query(conn, query->sql);
        if (res) PQclear(res);

        /* Benchmark with different customer IDs */
        for (int i = 0; i < iterations; i++) {
            /* Substitute customer_id for variety */
            char sql[2048];
            snprintf(sql, sizeof(sql), "%s", query->sql);

            timer_start(&timer);
            res = bench_query(conn, sql);
            timer_stop(&timer);

            if (res) {
                latency_stats_add(stats, timer_elapsed_us(&timer));
                result->total_queries++;
                result->total_rows += PQntuples(res);
                PQclear(res);
            } else {
                result->failed_queries++;
            }
        }

        latency_stats_compute(stats);
        result->latency = *stats;

        printf("  %-30s %10.2fms %10.2fms %10" PRId64 "\n",
               query->name,
               result->latency.avg_us / 1000.0,
               result->latency.p95_us / 1000.0,
               result->total_rows / iterations);

        bench_suite_add_result(suite, result);
        latency_stats_free(stats);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

/* Concurrent query workers */
typedef struct {
    int worker_id;
    DistributedBenchmark *bench;
    DistributedQuery *queries;
    int num_queries;
    LatencyStats *stats;
    int64_t queries_run;
    volatile bool *should_stop;
} QueryWorkerContext;

static void *query_worker(void *arg)
{
    QueryWorkerContext *ctx = (QueryWorkerContext *)arg;

    PGconn *conn = bench_connect(&ctx->bench->config);
    if (!conn) return NULL;

    Timer timer;
    srand(time(NULL) ^ ctx->worker_id);

    while (!*ctx->should_stop) {
        int q = rand() % ctx->num_queries;

        timer_start(&timer);
        PGresult *res = bench_query(conn, ctx->queries[q].sql);
        timer_stop(&timer);

        if (res) {
            latency_stats_add(ctx->stats, timer_elapsed_us(&timer));
            ctx->queries_run++;
            PQclear(res);
        }
    }

    bench_disconnect(conn);
    return NULL;
}

/* Run concurrent throughput test */
static void run_throughput_benchmark(DistributedBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Distributed Throughput Benchmark ===");

    int num_workers = bench->config.num_clients;
    if (num_workers < 2) num_workers = 4;

    LOG_INFO("Running with %d concurrent clients for %d seconds...",
             num_workers, bench->config.duration_secs);

    /* Count cross-shard queries */
    int num_queries = 0;
    while (cross_shard_queries[num_queries].name != NULL) num_queries++;

    QueryWorkerContext *workers = calloc(num_workers, sizeof(QueryWorkerContext));
    pthread_t *threads = calloc(num_workers, sizeof(pthread_t));
    volatile bool should_stop = false;

    /* Initialize workers */
    for (int i = 0; i < num_workers; i++) {
        workers[i].worker_id = i;
        workers[i].bench = bench;
        workers[i].queries = cross_shard_queries;
        workers[i].num_queries = num_queries;
        workers[i].stats = latency_stats_create(10000);
        workers[i].should_stop = &should_stop;
    }

    Timer total_timer;
    timer_start(&total_timer);

    /* Start workers */
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&threads[i], NULL, query_worker, &workers[i]);
    }

    /* Run for duration */
    sleep(bench->config.duration_secs);
    should_stop = true;

    /* Wait for workers */
    for (int i = 0; i < num_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    timer_stop(&total_timer);

    /* Aggregate results */
    BenchResult *result = bench_result_create("distributed_throughput");
    strncpy(result->description, "Concurrent distributed query throughput",
            sizeof(result->description) - 1);

    LatencyStats *combined = latency_stats_create(num_workers * 10000);
    int64_t total_queries = 0;

    for (int i = 0; i < num_workers; i++) {
        latency_stats_merge(combined, workers[i].stats);
        total_queries += workers[i].queries_run;
        latency_stats_free(workers[i].stats);
    }

    latency_stats_compute(combined);
    result->latency = *combined;
    result->total_queries = total_queries;

    double duration = timer_elapsed_secs(&total_timer);
    throughput_stats_compute(&result->throughput, total_queries, 0, 0, duration);

    printf("\n  Throughput Results:\n");
    printf("    Clients:      %d\n", num_workers);
    printf("    Duration:     %.2f seconds\n", duration);
    printf("    Total Queries: %" PRId64 "\n", total_queries);
    printf("    Throughput:   %.2f queries/sec\n", result->throughput.ops_per_sec);
    printf("    Latency:      avg=%.2fms, p95=%.2fms, p99=%.2fms\n",
           result->latency.avg_us / 1000.0,
           result->latency.p95_us / 1000.0,
           result->latency.p99_us / 1000.0);

    bench_suite_add_result(suite, result);

    latency_stats_free(combined);
    bench_result_free(result);
    free(workers);
    free(threads);
}

static void print_usage(const char *prog)
{
    printf("Distributed Benchmark for Orochi DB\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --mode=MODE        Mode: cross-shard, local, rebalance, all [default: all]\n");
    printf("  --scale=N          Scale factor [default: 1]\n");
    printf("  --shards=N         Number of shards [default: 8]\n");
    printf("  --clients=N        Number of concurrent clients [default: 1]\n");
    printf("  --duration=N       Throughput test duration [default: 30]\n");
    printf("  --host=HOST        Database host [default: localhost]\n");
    printf("  --port=PORT        Database port [default: 5432]\n");
    printf("  --user=USER        Database user [default: postgres]\n");
    printf("  --database=DB      Database name [default: orochi_bench]\n");
    printf("  --output=DIR       Output directory\n");
    printf("  --help             Show this help\n");
}

int main(int argc, char *argv[])
{
    DistributedBenchmark bench = {0};

    /* Initialize defaults */
    BenchConfig *cfg = bench_config_create();
    memcpy(&bench.config, cfg, sizeof(BenchConfig));
    bench_config_free(cfg);

    bench.mode = MODE_ALL;
    bench.num_shards = 8;
    bench.num_customers = 10000;
    bench.num_orders = 100000;
    bench.config.duration_secs = 30;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *mode = argv[i] + 7;
            if (strcmp(mode, "cross-shard") == 0) bench.mode = MODE_CROSS_SHARD;
            else if (strcmp(mode, "local") == 0) bench.mode = MODE_LOCAL;
            else if (strcmp(mode, "rebalance") == 0) bench.mode = MODE_REBALANCE;
            else bench.mode = MODE_ALL;
        }
        else if (strncmp(argv[i], "--scale=", 8) == 0) {
            bench.config.scale_factor = atoi(argv[i] + 8);
            bench.num_customers = 10000 * bench.config.scale_factor;
            bench.num_orders = 100000 * bench.config.scale_factor;
        }
        else if (strncmp(argv[i], "--shards=", 9) == 0) {
            bench.num_shards = atoi(argv[i] + 9);
        }
        else if (strncmp(argv[i], "--clients=", 10) == 0) {
            bench.config.num_clients = atoi(argv[i] + 10);
        }
        else if (strncmp(argv[i], "--duration=", 11) == 0) {
            bench.config.duration_secs = atoi(argv[i] + 11);
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
            strncpy(bench.config.output_dir, argv[i] + 9, MAX_PATH_LEN - 1);
        }
    }

    printf("\n==========================================\n");
    printf("  Distributed Benchmark for Orochi DB\n");
    printf("==========================================\n\n");

    bench_config_print(&bench.config);
    printf("Shards: %d\n", bench.num_shards);
    printf("Customers: %" PRId64 "\n", bench.num_customers);
    printf("Orders: %" PRId64 "\n\n", bench.num_orders);

    /* Create benchmark suite */
    BenchSuite *suite = bench_suite_create("Distributed", 20);
    memcpy(&suite->config, &bench.config, sizeof(BenchConfig));
    bench_suite_collect_system_info(suite);

    /* Run selected benchmarks */
    if (bench.mode == MODE_CROSS_SHARD || bench.mode == MODE_ALL) {
        run_cross_shard_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_LOCAL || bench.mode == MODE_ALL) {
        run_local_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_ALL && bench.config.num_clients > 1) {
        run_throughput_benchmark(&bench, suite);
    }

    suite->end_time = time(NULL);

    /* Print and save results */
    bench_suite_print_summary(suite);

    char output_file[MAX_PATH_LEN];
    snprintf(output_file, sizeof(output_file), "%s/distributed_results.json",
             bench.config.output_dir);
    bench_suite_save(suite, output_file);

    bench_suite_free(suite);
    return 0;
}
