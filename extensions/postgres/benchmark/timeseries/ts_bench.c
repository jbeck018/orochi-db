/*
 * Orochi DB - Time-Series Benchmark Driver
 *
 * Measures INSERT throughput, time-range query performance,
 * and chunk management overhead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <inttypes.h>
#include "bench_common.h"

/* Benchmark modes */
typedef enum {
    MODE_INSERT,
    MODE_QUERY,
    MODE_CHUNK,
    MODE_ALL
} BenchMode;

/* Benchmark configuration */
typedef struct {
    BenchConfig config;
    BenchMode mode;
    int batch_size;
    int num_devices;
    int points_per_device;
    int64_t total_points;
    int64_t time_range_days;
} TsBenchmark;

/* Worker thread context */
typedef struct {
    int worker_id;
    TsBenchmark *bench;
    PGconn *conn;
    int64_t points_inserted;
    int64_t batches_completed;
    int64_t errors;
    LatencyStats *stats;
    volatile bool *should_stop;
} WorkerContext;

/* Generate sensor data point */
static void generate_sensor_point(char *buffer, size_t bufsize,
                                   int device_id, time_t timestamp)
{
    struct tm *tm = gmtime(&timestamp);
    char ts_str[32];
    strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", tm);

    /* Generate realistic sensor values with some variation */
    double temperature = 20.0 + (rand() % 200) / 10.0 + sin(device_id) * 5;
    double humidity = 40.0 + (rand() % 400) / 10.0;
    double pressure = 1000.0 + (rand() % 50);
    double battery = 70.0 + (rand() % 300) / 10.0;
    int signal = -90 + (rand() % 60);

    snprintf(buffer, bufsize,
             "('%s',%d,%.2f,%.2f,%.2f,%.2f,%d)",
             ts_str, device_id, temperature, humidity,
             pressure, battery, signal);
}

/* Batch INSERT worker */
static void *insert_worker(void *arg)
{
    WorkerContext *ctx = (WorkerContext *)arg;
    TsBenchmark *bench = ctx->bench;

    ctx->conn = bench_connect(&bench->config);
    if (!ctx->conn) {
        LOG_ERROR("Worker %d: Failed to connect", ctx->worker_id);
        return NULL;
    }

    /* Use COPY for bulk inserts */
    char *batch_buffer = malloc(bench->batch_size * 256);
    if (!batch_buffer) {
        bench_disconnect(ctx->conn);
        return NULL;
    }

    time_t base_time = time(NULL) - (bench->time_range_days * 86400);
    Timer timer;
    int64_t point_counter = 0;

    while (!*ctx->should_stop && point_counter < bench->points_per_device) {
        /* Build batch INSERT statement */
        int offset = snprintf(batch_buffer, bench->batch_size * 256,
            "INSERT INTO sensor_data (time, device_id, temperature, "
            "humidity, pressure, battery_level, signal_strength) VALUES ");

        for (int i = 0; i < bench->batch_size && !*ctx->should_stop; i++) {
            int device_id = (ctx->worker_id % bench->num_devices) + 1;
            time_t ts = base_time + point_counter + i;

            char point[256];
            generate_sensor_point(point, sizeof(point), device_id, ts);

            if (i > 0) {
                offset += snprintf(batch_buffer + offset,
                                   bench->batch_size * 256 - offset, ",");
            }
            offset += snprintf(batch_buffer + offset,
                               bench->batch_size * 256 - offset, "%s", point);
        }

        /* Execute batch */
        timer_start(&timer);
        PGresult *res = PQexec(ctx->conn, batch_buffer);
        timer_stop(&timer);

        if (PQresultStatus(res) == PGRES_COMMAND_OK) {
            ctx->points_inserted += bench->batch_size;
            ctx->batches_completed++;
            latency_stats_add(ctx->stats, timer_elapsed_us(&timer));
        } else {
            ctx->errors++;
            if (ctx->errors < 5) {
                LOG_ERROR("Worker %d: INSERT failed: %s",
                          ctx->worker_id, PQerrorMessage(ctx->conn));
            }
        }
        PQclear(res);

        point_counter += bench->batch_size;
    }

    free(batch_buffer);
    bench_disconnect(ctx->conn);
    return NULL;
}

/* Run INSERT benchmark */
static void run_insert_benchmark(TsBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Time-Series INSERT Benchmark ===");
    LOG_INFO("Devices: %d, Points/Device: %d, Batch Size: %d",
             bench->num_devices, bench->points_per_device, bench->batch_size);
    LOG_INFO("Total Points: %ld", bench->total_points);

    int num_workers = bench->config.num_clients;
    WorkerContext *workers = calloc(num_workers, sizeof(WorkerContext));
    pthread_t *threads = calloc(num_workers, sizeof(pthread_t));
    volatile bool should_stop = false;

    /* Initialize workers */
    for (int i = 0; i < num_workers; i++) {
        workers[i].worker_id = i;
        workers[i].bench = bench;
        workers[i].stats = latency_stats_create(100000);
        workers[i].should_stop = &should_stop;
    }

    /* Start timer */
    Timer total_timer;
    timer_start(&total_timer);

    /* Start workers */
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&threads[i], NULL, insert_worker, &workers[i]);
    }

    /* Progress monitoring */
    int64_t last_total = 0;
    while (!should_stop) {
        sleep(1);

        int64_t current_total = 0;
        bool all_done = true;

        for (int i = 0; i < num_workers; i++) {
            current_total += workers[i].points_inserted;
            if (workers[i].points_inserted < bench->points_per_device) {
                all_done = false;
            }
        }

        double elapsed = timer_elapsed_secs(&total_timer);
        double rate = (current_total - last_total);

        printf("\r  Progress: %" PRId64 " / %" PRId64 " points (%.0f points/sec)   ",
               current_total, bench->total_points, rate);
        fflush(stdout);

        last_total = current_total;

        if (all_done || elapsed > bench->config.duration_secs) {
            should_stop = true;
        }
    }
    printf("\n");

    /* Wait for workers */
    for (int i = 0; i < num_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    timer_stop(&total_timer);

    /* Aggregate results */
    BenchResult *result = bench_result_create("insert_throughput");
    strncpy(result->description, "High-volume INSERT performance",
            sizeof(result->description) - 1);

    LatencyStats *combined = latency_stats_create(num_workers * 100000);
    int64_t total_points = 0;
    int64_t total_batches = 0;
    int64_t total_errors = 0;

    for (int i = 0; i < num_workers; i++) {
        latency_stats_merge(combined, workers[i].stats);
        total_points += workers[i].points_inserted;
        total_batches += workers[i].batches_completed;
        total_errors += workers[i].errors;
        latency_stats_free(workers[i].stats);
    }

    latency_stats_compute(combined);
    result->latency = *combined;
    result->total_queries = total_batches;
    result->total_rows = total_points;
    result->failed_queries = total_errors;

    double duration = timer_elapsed_secs(&total_timer);
    throughput_stats_compute(&result->throughput, total_batches,
                              total_points, 0, duration);

    printf("\n  Results:\n");
    printf("    Total Points:     %" PRId64 "\n", total_points);
    printf("    Total Batches:    %" PRId64 "\n", total_batches);
    printf("    Duration:         %.2f seconds\n", duration);
    printf("    Throughput:       %.0f points/sec\n", total_points / duration);
    printf("    Batch Latency:    avg=%.2fms, p95=%.2fms, p99=%.2fms\n",
           result->latency.avg_us / 1000.0,
           result->latency.p95_us / 1000.0,
           result->latency.p99_us / 1000.0);

    bench_suite_add_result(suite, result);

    latency_stats_free(combined);
    bench_result_free(result);
    free(workers);
    free(threads);
}

/* Time-range query definitions */
typedef struct {
    const char *name;
    const char *description;
    const char *sql_template;
} TimeQuery;

static TimeQuery time_queries[] = {
    {
        "last_hour",
        "Query last hour of data",
        "SELECT device_id, AVG(temperature), AVG(humidity), COUNT(*) "
        "FROM sensor_data WHERE time > NOW() - INTERVAL '1 hour' "
        "GROUP BY device_id"
    },
    {
        "last_day",
        "Query last 24 hours",
        "SELECT device_id, "
        "  date_trunc('hour', time) AS hour, "
        "  AVG(temperature), MIN(temperature), MAX(temperature) "
        "FROM sensor_data WHERE time > NOW() - INTERVAL '1 day' "
        "GROUP BY device_id, hour ORDER BY device_id, hour"
    },
    {
        "last_week",
        "Query last 7 days aggregated by day",
        "SELECT device_id, "
        "  date_trunc('day', time) AS day, "
        "  AVG(temperature), AVG(humidity), COUNT(*) "
        "FROM sensor_data WHERE time > NOW() - INTERVAL '7 days' "
        "GROUP BY device_id, day ORDER BY device_id, day"
    },
    {
        "device_specific",
        "Query specific device last 6 hours",
        "SELECT time, temperature, humidity, pressure "
        "FROM sensor_data "
        "WHERE device_id = 1 AND time > NOW() - INTERVAL '6 hours' "
        "ORDER BY time DESC LIMIT 1000"
    },
    {
        "anomaly_detection",
        "Find temperature anomalies",
        "SELECT device_id, time, temperature "
        "FROM sensor_data "
        "WHERE temperature > 35 OR temperature < 10 "
        "ORDER BY time DESC LIMIT 100"
    },
    {
        "moving_average",
        "Calculate moving average (window function)",
        "SELECT device_id, time, temperature, "
        "  AVG(temperature) OVER (PARTITION BY device_id "
        "    ORDER BY time ROWS BETWEEN 9 PRECEDING AND CURRENT ROW) "
        "    AS moving_avg "
        "FROM sensor_data "
        "WHERE time > NOW() - INTERVAL '1 hour' AND device_id = 1 "
        "ORDER BY time"
    },
    {
        "percentiles",
        "Calculate percentiles over time range",
        "SELECT device_id, "
        "  PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY temperature) AS p50, "
        "  PERCENTILE_CONT(0.95) WITHIN GROUP (ORDER BY temperature) AS p95, "
        "  PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY temperature) AS p99 "
        "FROM sensor_data "
        "WHERE time > NOW() - INTERVAL '1 day' "
        "GROUP BY device_id"
    },
    {NULL, NULL, NULL}
};

/* Run query benchmark */
static void run_query_benchmark(TsBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Time-Series Query Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    int iterations = 5;

    for (int q = 0; time_queries[q].name != NULL; q++) {
        TimeQuery *query = &time_queries[q];
        BenchResult *result = bench_result_create(query->name);
        strncpy(result->description, query->description,
                sizeof(result->description) - 1);

        LatencyStats *stats = latency_stats_create(iterations);
        Timer timer;

        printf("  %s: ", query->name);
        fflush(stdout);

        /* Warmup */
        PGresult *res = bench_query(conn, query->sql_template);
        if (res) PQclear(res);

        /* Benchmark runs */
        for (int i = 0; i < iterations; i++) {
            timer_start(&timer);
            res = bench_query(conn, query->sql_template);
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
        throughput_stats_compute(&result->throughput, result->total_queries,
                                  result->total_rows, 0,
                                  stats->avg_us * iterations / 1000000.0);

        printf("avg=%.2fms, p95=%.2fms, rows=%" PRId64 "\n",
               result->latency.avg_us / 1000.0,
               result->latency.p95_us / 1000.0,
               result->total_rows / iterations);

        bench_suite_add_result(suite, result);

        latency_stats_free(stats);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

/* Run chunk management benchmark */
static void run_chunk_benchmark(TsBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Chunk Management Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Test 1: Chunk creation overhead */
    {
        BenchResult *result = bench_result_create("chunk_creation");
        strncpy(result->description, "Chunk creation overhead",
                sizeof(result->description) - 1);

        LatencyStats *stats = latency_stats_create(100);
        Timer timer;

        printf("  Testing chunk creation...\n");

        /* Create multiple chunks by inserting data across time boundaries */
        for (int day = 0; day < 30; day++) {
            char sql[512];
            snprintf(sql, sizeof(sql),
                "INSERT INTO sensor_data (time, device_id, temperature, "
                "humidity, pressure, battery_level, signal_strength) "
                "SELECT NOW() - INTERVAL '%d days' + (generate_series * INTERVAL '1 second'), "
                "1, random() * 30, random() * 100, 1013, 80, -70 "
                "FROM generate_series(1, 1000)",
                day);

            timer_start(&timer);
            PGresult *res = PQexec(conn, sql);
            timer_stop(&timer);

            if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                latency_stats_add(stats, timer_elapsed_us(&timer));
                result->total_queries++;
                result->total_rows += 1000;
            } else {
                result->failed_queries++;
            }
            PQclear(res);
        }

        latency_stats_compute(stats);
        result->latency = *stats;

        printf("    Chunk creation: avg=%.2fms per 1000 rows\n",
               result->latency.avg_us / 1000.0);

        bench_suite_add_result(suite, result);
        latency_stats_free(stats);
        bench_result_free(result);
    }

    /* Test 2: Cross-chunk query performance */
    {
        BenchResult *result = bench_result_create("cross_chunk_query");
        strncpy(result->description, "Query spanning multiple chunks",
                sizeof(result->description) - 1);

        LatencyStats *stats = latency_stats_create(10);
        Timer timer;

        const char *sql =
            "SELECT date_trunc('day', time) AS day, COUNT(*), AVG(temperature) "
            "FROM sensor_data GROUP BY day ORDER BY day";

        printf("  Testing cross-chunk queries...\n");

        for (int i = 0; i < 10; i++) {
            timer_start(&timer);
            PGresult *res = bench_query(conn, sql);
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

        printf("    Cross-chunk query: avg=%.2fms, rows=%" PRId64 "\n",
               result->latency.avg_us / 1000.0,
               result->total_rows / 10);

        bench_suite_add_result(suite, result);
        latency_stats_free(stats);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

static void print_usage(const char *prog)
{
    printf("Time-Series Benchmark for Orochi DB\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --mode=MODE        Benchmark mode: insert, query, chunk, all [default: all]\n");
    printf("  --scale=N          Scale factor [default: 1]\n");
    printf("  --clients=N        Number of concurrent clients [default: 1]\n");
    printf("  --batch=N          Batch size for inserts [default: 1000]\n");
    printf("  --devices=N        Number of simulated devices [default: 100]\n");
    printf("  --duration=N       Max duration in seconds [default: 60]\n");
    printf("  --host=HOST        Database host [default: localhost]\n");
    printf("  --port=PORT        Database port [default: 5432]\n");
    printf("  --user=USER        Database user [default: postgres]\n");
    printf("  --database=DB      Database name [default: orochi_bench]\n");
    printf("  --output=DIR       Output directory\n");
    printf("  --help             Show this help\n");
}

int main(int argc, char *argv[])
{
    TsBenchmark bench = {0};

    /* Initialize defaults */
    BenchConfig *cfg = bench_config_create();
    memcpy(&bench.config, cfg, sizeof(BenchConfig));
    bench_config_free(cfg);

    bench.mode = MODE_ALL;
    bench.batch_size = 1000;
    bench.num_devices = 100;
    bench.points_per_device = 100000;
    bench.time_range_days = 30;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *mode = argv[i] + 7;
            if (strcmp(mode, "insert") == 0) bench.mode = MODE_INSERT;
            else if (strcmp(mode, "query") == 0) bench.mode = MODE_QUERY;
            else if (strcmp(mode, "chunk") == 0) bench.mode = MODE_CHUNK;
            else bench.mode = MODE_ALL;
        }
        else if (strncmp(argv[i], "--scale=", 8) == 0) {
            bench.config.scale_factor = atoi(argv[i] + 8);
            bench.points_per_device = 100000 * bench.config.scale_factor;
        }
        else if (strncmp(argv[i], "--clients=", 10) == 0) {
            bench.config.num_clients = atoi(argv[i] + 10);
        }
        else if (strncmp(argv[i], "--batch=", 8) == 0) {
            bench.batch_size = atoi(argv[i] + 8);
        }
        else if (strncmp(argv[i], "--devices=", 10) == 0) {
            bench.num_devices = atoi(argv[i] + 10);
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

    bench.total_points = (int64_t)bench.num_devices * bench.points_per_device;

    printf("\n========================================\n");
    printf("   Time-Series Benchmark for Orochi DB\n");
    printf("========================================\n\n");

    bench_config_print(&bench.config);
    printf("Devices: %d\n", bench.num_devices);
    printf("Points/Device: %d\n", bench.points_per_device);
    printf("Batch Size: %d\n", bench.batch_size);
    printf("\n");

    /* Create benchmark suite */
    BenchSuite *suite = bench_suite_create("TimeSeries", 20);
    memcpy(&suite->config, &bench.config, sizeof(BenchConfig));
    bench_suite_collect_system_info(suite);

    /* Run selected benchmarks */
    if (bench.mode == MODE_INSERT || bench.mode == MODE_ALL) {
        run_insert_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_QUERY || bench.mode == MODE_ALL) {
        run_query_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_CHUNK || bench.mode == MODE_ALL) {
        run_chunk_benchmark(&bench, suite);
    }

    suite->end_time = time(NULL);

    /* Print and save results */
    bench_suite_print_summary(suite);

    char output_file[MAX_PATH_LEN];
    snprintf(output_file, sizeof(output_file), "%s/timeseries_results.json",
             bench.config.output_dir);
    bench_suite_save(suite, output_file);

    bench_suite_free(suite);
    return 0;
}
