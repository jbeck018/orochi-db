/*
 * Orochi DB - Columnar Storage Benchmark Driver
 *
 * Measures compression ratios, scan performance vs row store,
 * and filter pushdown effectiveness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "bench_common.h"

/* Benchmark modes */
typedef enum {
    MODE_COMPRESSION,
    MODE_SCAN,
    MODE_FILTER,
    MODE_ALL
} BenchMode;

/* Data type categories for compression testing */
typedef enum {
    DTYPE_INTEGER,
    DTYPE_FLOAT,
    DTYPE_TIMESTAMP,
    DTYPE_STRING,
    DTYPE_MIXED
} DataType;

/* Compression benchmark configuration */
typedef struct {
    BenchConfig config;
    BenchMode mode;
    int64_t rows_per_table;
} ColumnarBenchmark;

/* Compression result */
typedef struct {
    char table_name[64];
    DataType data_type;
    int64_t rows;
    int64_t raw_size_bytes;
    int64_t compressed_size_bytes;
    double compression_ratio;
    double compression_time_ms;
} CompressionResult;

/* Generate test data for integer table */
static void populate_integers(PGconn *conn, int64_t num_rows)
{
    LOG_INFO("Populating integer data (%ld rows)...", num_rows);

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO columnar_integers "
        "(monotonic_seq, random_int, small_range, boolean_col, nullable_int) "
        "SELECT "
        "  generate_series, "
        "  (random() * 2147483647)::int, "
        "  (random() * 100)::smallint, "
        "  random() > 0.5, "
        "  CASE WHEN random() > 0.2 THEN (random() * 1000)::int ELSE NULL END "
        "FROM generate_series(1, %" PRId64 ")", num_rows);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Generate test data for float table */
static void populate_floats(PGconn *conn, int64_t num_rows)
{
    LOG_INFO("Populating float data (%ld rows)...", num_rows);

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO columnar_floats "
        "(timestamp_col, price, temperature, percentage, nullable_float) "
        "SELECT "
        "  NOW() - (random() * INTERVAL '365 days'), "
        "  100.0 + random() * 900.0, "
        "  -20.0 + random() * 60.0, "
        "  random(), "
        "  CASE WHEN random() > 0.3 THEN random() * 100.0 ELSE NULL END "
        "FROM generate_series(1, %" PRId64 ")", num_rows);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Generate test data for timestamp table */
static void populate_timestamps(PGconn *conn, int64_t num_rows)
{
    LOG_INFO("Populating timestamp data (%ld rows)...", num_rows);

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO columnar_timestamps "
        "(event_time, created_at, date_only, time_only) "
        "SELECT "
        "  NOW() - (random() * INTERVAL '365 days'), "
        "  NOW() - (random() * INTERVAL '365 days'), "
        "  CURRENT_DATE - (random() * 365)::int, "
        "  TIME '00:00:00' + (random() * INTERVAL '24 hours') "
        "FROM generate_series(1, %" PRId64 ")", num_rows);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Generate test data for string table */
static void populate_strings(PGconn *conn, int64_t num_rows)
{
    LOG_INFO("Populating string data (%ld rows)...", num_rows);

    /* First, create categories for low cardinality columns */
    const char *categories[] = {
        "'electronics'", "'clothing'", "'food'", "'automotive'", "'home'",
        "'sports'", "'books'", "'toys'", "'health'", "'beauty'"
    };
    const char *statuses[] = {
        "'active'", "'inactive'", "'pending'", "'archived'", "'deleted'"
    };

    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO columnar_strings "
        "(category, status, description, uuid_col, nullable_str) "
        "SELECT "
        "  (ARRAY[%s,%s,%s,%s,%s,%s,%s,%s,%s,%s])[1 + (random() * 9)::int], "
        "  (ARRAY[%s,%s,%s,%s,%s])[1 + (random() * 4)::int], "
        "  'Description for item ' || generate_series || ' with random text ' || md5(random()::text), "
        "  gen_random_uuid(), "
        "  CASE WHEN random() > 0.5 THEN 'optional_' || (random() * 1000)::int ELSE NULL END "
        "FROM generate_series(1, %" PRId64 ")",
        categories[0], categories[1], categories[2], categories[3], categories[4],
        categories[5], categories[6], categories[7], categories[8], categories[9],
        statuses[0], statuses[1], statuses[2], statuses[3], statuses[4],
        num_rows);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Generate test data for mixed table */
static void populate_mixed(PGconn *conn, int64_t num_rows, bool row_store)
{
    const char *table = row_store ? "row_store_baseline" : "columnar_mixed";
    LOG_INFO("Populating %s (%ld rows)...", table, num_rows);

    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO %s "
        "(event_time, device_id, metric_name, metric_value, tags, raw_data) "
        "SELECT "
        "  NOW() - (random() * INTERVAL '30 days'), "
        "  (random() * 1000)::int, "
        "  (ARRAY['cpu_usage','memory_used','disk_io','network_rx','network_tx'])[1 + (random() * 4)::int], "
        "  random() * 100.0, "
        "  jsonb_build_object('host', 'server-' || (random() * 100)::int, 'region', "
        "    (ARRAY['us-east','us-west','eu-west','ap-south'])[1 + (random() * 3)::int]), "
        "  decode(md5(random()::text), 'hex') "
        "FROM generate_series(1, %" PRId64 ")",
        table, num_rows);

    Timer timer;
    timer_start(&timer);
    bench_exec(conn, sql);
    timer_stop(&timer);

    LOG_INFO("  Completed in %.2f seconds", timer_elapsed_secs(&timer));
}

/* Get table size */
static int64_t get_table_size(PGconn *conn, const char *table)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT pg_total_relation_size('%s')", table);

    PGresult *res = bench_query(conn, sql);
    if (!res) return -1;

    int64_t size = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return size;
}

/* Get row count */
static int64_t get_row_count(PGconn *conn, const char *table)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);

    PGresult *res = bench_query(conn, sql);
    if (!res) return -1;

    int64_t count = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return count;
}

/* Run compression benchmark */
static void run_compression_benchmark(ColumnarBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Columnar Compression Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Populate test tables */
    int64_t rows = bench->rows_per_table;

    populate_integers(conn, rows);
    populate_floats(conn, rows);
    populate_timestamps(conn, rows);
    populate_strings(conn, rows);
    populate_mixed(conn, rows, false);
    populate_mixed(conn, rows, true);

    /* Analyze tables */
    bench_exec(conn, "ANALYZE");

    /* Measure sizes and compression ratios */
    struct {
        const char *name;
        const char *table;
        DataType dtype;
    } tables[] = {
        {"integers", "columnar_integers", DTYPE_INTEGER},
        {"floats", "columnar_floats", DTYPE_FLOAT},
        {"timestamps", "columnar_timestamps", DTYPE_TIMESTAMP},
        {"strings", "columnar_strings", DTYPE_STRING},
        {"mixed_columnar", "columnar_mixed", DTYPE_MIXED},
        {"mixed_row", "row_store_baseline", DTYPE_MIXED},
        {NULL, NULL, 0}
    };

    printf("\n  Compression Results:\n");
    printf("  %-20s %12s %12s %10s\n",
           "Table", "Rows", "Size", "Bytes/Row");
    printf("  %s\n", "------------------------------------------------------------");

    int64_t baseline_size = 0;

    for (int i = 0; tables[i].name != NULL; i++) {
        int64_t size = get_table_size(conn, tables[i].table);
        int64_t count = get_row_count(conn, tables[i].table);

        if (count <= 0) continue;

        double bytes_per_row = (double)size / count;

        char size_str[32];
        format_bytes(size, size_str, sizeof(size_str));

        printf("  %-20s %12" PRId64 " %12s %10.2f\n",
               tables[i].name, count, size_str, bytes_per_row);

        /* Track baseline for ratio calculation */
        if (strcmp(tables[i].name, "mixed_row") == 0) {
            baseline_size = size;
        }

        /* Create result */
        char result_name[64];
        snprintf(result_name, sizeof(result_name), "compression_%s", tables[i].name);

        BenchResult *result = bench_result_create(result_name);
        snprintf(result->description, sizeof(result->description),
                 "Compression test for %s", tables[i].name);

        result->total_rows = count;
        result->total_bytes = size;

        if (strcmp(tables[i].name, "mixed_columnar") == 0 && baseline_size > 0) {
            result->compression_ratio = (double)baseline_size / size;
        }

        bench_suite_add_result(suite, result);
        bench_result_free(result);
    }

    if (baseline_size > 0) {
        int64_t columnar_size = get_table_size(conn, "columnar_mixed");
        double ratio = (double)baseline_size / columnar_size;
        printf("\n  Columnar vs Row Store Ratio: %.2fx smaller\n", ratio);
    }

    bench_disconnect(conn);
}

/* Run scan performance benchmark */
static void run_scan_benchmark(ColumnarBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Columnar Scan Performance Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Scan queries */
    struct {
        const char *name;
        const char *columnar_sql;
        const char *row_sql;
    } scans[] = {
        {
            "full_scan_agg",
            "SELECT COUNT(*), AVG(metric_value), MIN(metric_value), MAX(metric_value) FROM columnar_mixed",
            "SELECT COUNT(*), AVG(metric_value), MIN(metric_value), MAX(metric_value) FROM row_store_baseline"
        },
        {
            "single_column_scan",
            "SELECT SUM(metric_value) FROM columnar_mixed",
            "SELECT SUM(metric_value) FROM row_store_baseline"
        },
        {
            "multi_column_scan",
            "SELECT device_id, metric_name, AVG(metric_value) FROM columnar_mixed GROUP BY device_id, metric_name",
            "SELECT device_id, metric_name, AVG(metric_value) FROM row_store_baseline GROUP BY device_id, metric_name"
        },
        {
            "time_range_scan",
            "SELECT date_trunc('hour', event_time), AVG(metric_value) FROM columnar_mixed WHERE event_time > NOW() - INTERVAL '7 days' GROUP BY 1 ORDER BY 1",
            "SELECT date_trunc('hour', event_time), AVG(metric_value) FROM row_store_baseline WHERE event_time > NOW() - INTERVAL '7 days' GROUP BY 1 ORDER BY 1"
        },
        {NULL, NULL, NULL}
    };

    printf("\n  Scan Performance (Columnar vs Row Store):\n");
    printf("  %-25s %12s %12s %10s\n",
           "Query", "Columnar", "Row Store", "Speedup");
    printf("  %s\n", "------------------------------------------------------------");

    int iterations = 3;

    for (int q = 0; scans[q].name != NULL; q++) {
        Timer timer;
        double columnar_time = 0;
        double row_time = 0;

        /* Warmup */
        PGresult *res = bench_query(conn, scans[q].columnar_sql);
        if (res) PQclear(res);
        res = bench_query(conn, scans[q].row_sql);
        if (res) PQclear(res);

        /* Benchmark columnar */
        for (int i = 0; i < iterations; i++) {
            timer_start(&timer);
            res = bench_query(conn, scans[q].columnar_sql);
            timer_stop(&timer);
            if (res) {
                columnar_time += timer_elapsed_ms(&timer);
                PQclear(res);
            }
        }
        columnar_time /= iterations;

        /* Benchmark row store */
        for (int i = 0; i < iterations; i++) {
            timer_start(&timer);
            res = bench_query(conn, scans[q].row_sql);
            timer_stop(&timer);
            if (res) {
                row_time += timer_elapsed_ms(&timer);
                PQclear(res);
            }
        }
        row_time /= iterations;

        double speedup = row_time / columnar_time;

        printf("  %-25s %10.2fms %10.2fms %9.2fx\n",
               scans[q].name, columnar_time, row_time, speedup);

        /* Create result */
        char result_name[64];
        snprintf(result_name, sizeof(result_name), "scan_%s", scans[q].name);

        BenchResult *result = bench_result_create(result_name);
        result->latency.avg_us = columnar_time * 1000;
        result->throughput.ops_per_sec = 1000.0 / columnar_time;
        result->compression_ratio = speedup;  /* Reusing for speedup */

        bench_suite_add_result(suite, result);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

/* Run filter pushdown benchmark */
static void run_filter_benchmark(ColumnarBenchmark *bench, BenchSuite *suite)
{
    LOG_INFO("=== Filter Pushdown Benchmark ===");

    PGconn *conn = bench_connect(&bench->config);
    if (!conn) {
        LOG_ERROR("Failed to connect to database");
        return;
    }

    /* Filter queries with varying selectivity */
    struct {
        const char *name;
        const char *sql;
        double expected_selectivity;
    } filters[] = {
        {
            "high_selectivity",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE device_id = 1",
            0.001
        },
        {
            "medium_selectivity",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE device_id < 100",
            0.1
        },
        {
            "low_selectivity",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE device_id < 500",
            0.5
        },
        {
            "range_filter",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE metric_value BETWEEN 25 AND 75",
            0.5
        },
        {
            "string_filter",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE metric_name = 'cpu_usage'",
            0.2
        },
        {
            "compound_filter",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE device_id < 100 AND metric_value > 50",
            0.05
        },
        {
            "time_filter",
            "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed WHERE event_time > NOW() - INTERVAL '1 day'",
            0.033
        },
        {NULL, NULL, 0}
    };

    printf("\n  Filter Pushdown Effectiveness:\n");
    printf("  %-25s %12s %12s %12s\n",
           "Filter", "Time (ms)", "Rows", "Selectivity");
    printf("  %s\n", "------------------------------------------------------------");

    int64_t total_rows = get_row_count(conn, "columnar_mixed");
    int iterations = 3;

    for (int q = 0; filters[q].name != NULL; q++) {
        Timer timer;
        double total_time = 0;
        int64_t result_rows = 0;

        /* Warmup */
        PGresult *res = bench_query(conn, filters[q].sql);
        if (res) PQclear(res);

        /* Benchmark */
        for (int i = 0; i < iterations; i++) {
            timer_start(&timer);
            res = bench_query(conn, filters[q].sql);
            timer_stop(&timer);
            if (res) {
                total_time += timer_elapsed_ms(&timer);
                if (i == 0 && PQntuples(res) > 0) {
                    result_rows = atoll(PQgetvalue(res, 0, 0));
                }
                PQclear(res);
            }
        }
        double avg_time = total_time / iterations;
        double selectivity = total_rows > 0 ? (double)result_rows / total_rows : 0;

        printf("  %-25s %10.2fms %12" PRId64 " %11.2f%%\n",
               filters[q].name, avg_time, result_rows, selectivity * 100);

        /* Create result */
        char result_name[64];
        snprintf(result_name, sizeof(result_name), "filter_%s", filters[q].name);

        BenchResult *result = bench_result_create(result_name);
        result->latency.avg_us = avg_time * 1000;
        result->total_rows = result_rows;

        bench_suite_add_result(suite, result);
        bench_result_free(result);
    }

    bench_disconnect(conn);
}

static void print_usage(const char *prog)
{
    printf("Columnar Storage Benchmark for Orochi DB\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --mode=MODE        Benchmark mode: compression, scan, filter, all [default: all]\n");
    printf("  --scale=N          Scale factor (rows = 100000 * N) [default: 1]\n");
    printf("  --host=HOST        Database host [default: localhost]\n");
    printf("  --port=PORT        Database port [default: 5432]\n");
    printf("  --user=USER        Database user [default: postgres]\n");
    printf("  --database=DB      Database name [default: orochi_bench]\n");
    printf("  --output=DIR       Output directory\n");
    printf("  --help             Show this help\n");
}

int main(int argc, char *argv[])
{
    ColumnarBenchmark bench = {0};

    /* Initialize defaults */
    BenchConfig *cfg = bench_config_create();
    memcpy(&bench.config, cfg, sizeof(BenchConfig));
    bench_config_free(cfg);

    bench.mode = MODE_ALL;
    bench.rows_per_table = 100000;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *mode = argv[i] + 7;
            if (strcmp(mode, "compression") == 0) bench.mode = MODE_COMPRESSION;
            else if (strcmp(mode, "scan") == 0) bench.mode = MODE_SCAN;
            else if (strcmp(mode, "filter") == 0) bench.mode = MODE_FILTER;
            else bench.mode = MODE_ALL;
        }
        else if (strncmp(argv[i], "--scale=", 8) == 0) {
            bench.config.scale_factor = atoi(argv[i] + 8);
            bench.rows_per_table = 100000 * bench.config.scale_factor;
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
    printf("  Columnar Storage Benchmark for Orochi DB\n");
    printf("==========================================\n\n");

    bench_config_print(&bench.config);
    printf("Rows per table: %" PRId64 "\n\n", bench.rows_per_table);

    /* Create benchmark suite */
    BenchSuite *suite = bench_suite_create("Columnar", 30);
    memcpy(&suite->config, &bench.config, sizeof(BenchConfig));
    bench_suite_collect_system_info(suite);

    /* Run selected benchmarks */
    if (bench.mode == MODE_COMPRESSION || bench.mode == MODE_ALL) {
        run_compression_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_SCAN || bench.mode == MODE_ALL) {
        run_scan_benchmark(&bench, suite);
    }

    if (bench.mode == MODE_FILTER || bench.mode == MODE_ALL) {
        run_filter_benchmark(&bench, suite);
    }

    suite->end_time = time(NULL);

    /* Print and save results */
    bench_suite_print_summary(suite);

    char output_file[MAX_PATH_LEN];
    snprintf(output_file, sizeof(output_file), "%s/columnar_results.json",
             bench.config.output_dir);
    bench_suite_save(suite, output_file);

    bench_suite_free(suite);
    return 0;
}
