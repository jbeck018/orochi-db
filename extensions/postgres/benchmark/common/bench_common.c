/*
 * Orochi DB Benchmarking Framework
 * Common utilities implementation
 */

#include "bench_common.h"
#include <stdarg.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <getopt.h>
#include <inttypes.h>

/* Global verbose flag */
bool g_verbose = false;

/* === Configuration === */

BenchConfig *bench_config_create(void)
{
    BenchConfig *config = calloc(1, sizeof(BenchConfig));
    if (!config) return NULL;

    /* Defaults */
    strncpy(config->host, "localhost", MAX_NAME_LEN - 1);
    config->port = 5432;
    strncpy(config->user, "postgres", MAX_NAME_LEN - 1);
    strncpy(config->database, "orochi_bench", MAX_NAME_LEN - 1);

    config->scale_factor = 1;
    config->num_clients = 1;
    config->num_threads = 1;
    config->duration_secs = 60;
    config->warmup_secs = 10;
    config->think_time_ms = 0;

    config->output_format = OUTPUT_JSON;
    strncpy(config->output_dir, "results", MAX_PATH_LEN - 1);

    config->use_prepared = true;
    config->use_transactions = true;
    config->collect_samples = true;
    config->verbose = false;

    return config;
}

void bench_config_free(BenchConfig *config)
{
    free(config);
}

int bench_config_parse_args(BenchConfig *config, int argc, char *argv[])
{
    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"user", required_argument, 0, 'U'},
        {"password", required_argument, 0, 'W'},
        {"database", required_argument, 0, 'd'},
        {"scale", required_argument, 0, 's'},
        {"clients", required_argument, 0, 'c'},
        {"threads", required_argument, 0, 't'},
        {"duration", required_argument, 0, 'D'},
        {"warmup", required_argument, 0, 'w'},
        {"format", required_argument, 0, 'f'},
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int c;
    int option_index = 0;

    while ((c = getopt_long(argc, argv, "h:p:U:W:d:s:c:t:D:w:f:o:v?",
                            long_options, &option_index)) != -1) {
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
            case 'W':
                strncpy(config->password, optarg, MAX_NAME_LEN - 1);
                break;
            case 'd':
                strncpy(config->database, optarg, MAX_NAME_LEN - 1);
                break;
            case 's':
                config->scale_factor = atoi(optarg);
                break;
            case 'c':
                config->num_clients = atoi(optarg);
                break;
            case 't':
                config->num_threads = atoi(optarg);
                break;
            case 'D':
                config->duration_secs = atoi(optarg);
                break;
            case 'w':
                config->warmup_secs = atoi(optarg);
                break;
            case 'f':
                if (strcmp(optarg, "json") == 0)
                    config->output_format = OUTPUT_JSON;
                else if (strcmp(optarg, "csv") == 0)
                    config->output_format = OUTPUT_CSV;
                else
                    config->output_format = OUTPUT_TEXT;
                break;
            case 'o':
                strncpy(config->output_dir, optarg, MAX_PATH_LEN - 1);
                break;
            case 'v':
                config->verbose = true;
                g_verbose = true;
                break;
            case '?':
            default:
                return -1;
        }
    }

    return 0;
}

void bench_config_print(const BenchConfig *config)
{
    printf("Benchmark Configuration:\n");
    printf("  Host:         %s\n", config->host);
    printf("  Port:         %d\n", config->port);
    printf("  User:         %s\n", config->user);
    printf("  Database:     %s\n", config->database);
    printf("  Scale Factor: %d\n", config->scale_factor);
    printf("  Clients:      %d\n", config->num_clients);
    printf("  Threads:      %d\n", config->num_threads);
    printf("  Duration:     %d seconds\n", config->duration_secs);
    printf("  Warmup:       %d seconds\n", config->warmup_secs);
    printf("  Output Dir:   %s\n", config->output_dir);
    printf("\n");
}

/* === Database Connection === */

PGconn *bench_connect(const BenchConfig *config)
{
    char conninfo[1024];

    if (strlen(config->password) > 0) {
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%d user=%s password=%s dbname=%s",
                 config->host, config->port, config->user,
                 config->password, config->database);
    } else {
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%d user=%s dbname=%s",
                 config->host, config->port, config->user, config->database);
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR("Connection failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    return conn;
}

void bench_disconnect(PGconn *conn)
{
    if (conn) {
        PQfinish(conn);
    }
}

bool bench_exec(PGconn *conn, const char *query)
{
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_COMMAND_OK &&
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Query failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

PGresult *bench_query(PGconn *conn, const char *query)
{
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Query failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return NULL;
    }
    return res;
}

bool bench_prepare(PGconn *conn, const char *name, const char *query)
{
    PGresult *res = PQprepare(conn, name, query, 0, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Prepare failed: %s", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

PGresult *bench_exec_prepared(PGconn *conn, const char *name,
                               int nparams, const char *const *params)
{
    return PQexecPrepared(conn, name, nparams, params, NULL, NULL, 0);
}

/* === Timer Functions === */

void timer_start(Timer *timer)
{
    gettimeofday(&timer->start, NULL);
    timer->running = true;
}

void timer_stop(Timer *timer)
{
    gettimeofday(&timer->end, NULL);
    timer->running = false;
}

double timer_elapsed_us(const Timer *timer)
{
    struct timeval end;
    if (timer->running) {
        gettimeofday(&end, NULL);
    } else {
        end = timer->end;
    }

    return (double)(end.tv_sec - timer->start.tv_sec) * 1000000.0 +
           (double)(end.tv_usec - timer->start.tv_usec);
}

double timer_elapsed_ms(const Timer *timer)
{
    return timer_elapsed_us(timer) / 1000.0;
}

double timer_elapsed_secs(const Timer *timer)
{
    return timer_elapsed_us(timer) / 1000000.0;
}

int64_t get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* === Latency Statistics === */

LatencyStats *latency_stats_create(int64_t max_samples)
{
    LatencyStats *stats = calloc(1, sizeof(LatencyStats));
    if (!stats) return NULL;

    stats->min_us = INFINITY;
    stats->max_us = 0;
    stats->max_samples = max_samples;

    if (max_samples > 0) {
        stats->samples = calloc(max_samples, sizeof(double));
        if (!stats->samples) {
            free(stats);
            return NULL;
        }
    }

    return stats;
}

void latency_stats_free(LatencyStats *stats)
{
    if (stats) {
        free(stats->samples);
        free(stats);
    }
}

void latency_stats_add(LatencyStats *stats, double latency_us)
{
    if (latency_us < stats->min_us) stats->min_us = latency_us;
    if (latency_us > stats->max_us) stats->max_us = latency_us;

    /* Running average using Welford's algorithm */
    stats->count++;
    double delta = latency_us - stats->avg_us;
    stats->avg_us += delta / stats->count;
    double delta2 = latency_us - stats->avg_us;
    stats->stddev_us += delta * delta2;

    /* Store sample for percentile calculation */
    if (stats->samples && stats->num_samples < stats->max_samples) {
        stats->samples[stats->num_samples++] = latency_us;
    }
}

void latency_stats_merge(LatencyStats *dest, const LatencyStats *src)
{
    if (src->min_us < dest->min_us) dest->min_us = src->min_us;
    if (src->max_us > dest->max_us) dest->max_us = src->max_us;

    /* Combine means using weighted average */
    int64_t total_count = dest->count + src->count;
    if (total_count > 0) {
        dest->avg_us = (dest->avg_us * dest->count + src->avg_us * src->count) / total_count;
    }
    dest->count = total_count;
}

int compare_doubles(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

double calculate_percentile(double *samples, int64_t count, double percentile)
{
    if (count == 0) return 0;
    if (count == 1) return samples[0];

    int64_t idx = (int64_t)(percentile / 100.0 * (count - 1));
    if (idx >= count) idx = count - 1;

    return samples[idx];
}

void latency_stats_compute(LatencyStats *stats)
{
    /* Compute standard deviation */
    if (stats->count > 1) {
        stats->stddev_us = sqrt(stats->stddev_us / (stats->count - 1));
    }

    /* Compute percentiles from samples */
    if (stats->samples && stats->num_samples > 0) {
        qsort(stats->samples, stats->num_samples, sizeof(double), compare_doubles);

        stats->p50_us = calculate_percentile(stats->samples, stats->num_samples, 50.0);
        stats->p90_us = calculate_percentile(stats->samples, stats->num_samples, 90.0);
        stats->p95_us = calculate_percentile(stats->samples, stats->num_samples, 95.0);
        stats->p99_us = calculate_percentile(stats->samples, stats->num_samples, 99.0);
        stats->p999_us = calculate_percentile(stats->samples, stats->num_samples, 99.9);
    }
}

void latency_stats_reset(LatencyStats *stats)
{
    stats->min_us = INFINITY;
    stats->max_us = 0;
    stats->avg_us = 0;
    stats->stddev_us = 0;
    stats->count = 0;
    stats->num_samples = 0;
}

/* === Throughput Statistics === */

void throughput_stats_compute(ThroughputStats *stats, int64_t ops,
                               int64_t rows, int64_t bytes, double duration)
{
    stats->total_ops = ops;
    stats->duration_secs = duration;
    stats->ops_per_sec = duration > 0 ? ops / duration : 0;
    stats->rows_per_sec = duration > 0 ? rows / duration : 0;
    stats->mb_per_sec = duration > 0 ? (bytes / 1024.0 / 1024.0) / duration : 0;
}

/* === Benchmark Result === */

BenchResult *bench_result_create(const char *name)
{
    BenchResult *result = calloc(1, sizeof(BenchResult));
    if (!result) return NULL;

    strncpy(result->name, name, MAX_NAME_LEN - 1);
    result->latency.min_us = INFINITY;
    result->success = true;

    return result;
}

void bench_result_free(BenchResult *result)
{
    free(result);
}

/* === Benchmark Suite === */

BenchSuite *bench_suite_create(const char *name, int max_results)
{
    BenchSuite *suite = calloc(1, sizeof(BenchSuite));
    if (!suite) return NULL;

    strncpy(suite->name, name, MAX_NAME_LEN - 1);
    strncpy(suite->version, BENCH_VERSION, sizeof(suite->version) - 1);

    suite->max_results = max_results;
    suite->results = calloc(max_results, sizeof(BenchResult));
    if (!suite->results) {
        free(suite);
        return NULL;
    }

    suite->start_time = time(NULL);

    return suite;
}

void bench_suite_free(BenchSuite *suite)
{
    if (suite) {
        free(suite->results);
        free(suite);
    }
}

void bench_suite_add_result(BenchSuite *suite, BenchResult *result)
{
    if (suite->num_results < suite->max_results) {
        memcpy(&suite->results[suite->num_results], result, sizeof(BenchResult));
        suite->num_results++;
    }
}

void bench_suite_collect_system_info(BenchSuite *suite)
{
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(suite->system_info, sizeof(suite->system_info),
                 "OS: %s %s %s, Arch: %s",
                 uts.sysname, uts.release, uts.version, uts.machine);
    }
}

/* === Output Functions === */

static void print_json_result(const BenchResult *result, FILE *fp)
{
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"name\": \"%s\",\n", result->name);
    fprintf(fp, "      \"success\": %s,\n", result->success ? "true" : "false");
    fprintf(fp, "      \"total_queries\": %" PRId64 ",\n", result->total_queries);
    fprintf(fp, "      \"failed_queries\": %" PRId64 ",\n", result->failed_queries);
    fprintf(fp, "      \"total_rows\": %" PRId64 ",\n", result->total_rows);
    fprintf(fp, "      \"latency\": {\n");
    fprintf(fp, "        \"min_us\": %.2f,\n", result->latency.min_us);
    fprintf(fp, "        \"max_us\": %.2f,\n", result->latency.max_us);
    fprintf(fp, "        \"avg_us\": %.2f,\n", result->latency.avg_us);
    fprintf(fp, "        \"stddev_us\": %.2f,\n", result->latency.stddev_us);
    fprintf(fp, "        \"p50_us\": %.2f,\n", result->latency.p50_us);
    fprintf(fp, "        \"p90_us\": %.2f,\n", result->latency.p90_us);
    fprintf(fp, "        \"p95_us\": %.2f,\n", result->latency.p95_us);
    fprintf(fp, "        \"p99_us\": %.2f,\n", result->latency.p99_us);
    fprintf(fp, "        \"p999_us\": %.2f\n", result->latency.p999_us);
    fprintf(fp, "      },\n");
    fprintf(fp, "      \"throughput\": {\n");
    fprintf(fp, "        \"ops_per_sec\": %.2f,\n", result->throughput.ops_per_sec);
    fprintf(fp, "        \"rows_per_sec\": %.2f,\n", result->throughput.rows_per_sec);
    fprintf(fp, "        \"mb_per_sec\": %.4f\n", result->throughput.mb_per_sec);
    fprintf(fp, "      }\n");
    fprintf(fp, "    }");
}

void bench_result_print(const BenchResult *result, OutputFormat format, FILE *fp)
{
    switch (format) {
        case OUTPUT_JSON:
            print_json_result(result, fp);
            break;

        case OUTPUT_CSV:
            fprintf(fp, "%s,%" PRId64 ",%" PRId64 ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                    result->name, result->total_queries, result->total_rows,
                    result->latency.min_us, result->latency.max_us,
                    result->latency.avg_us, result->latency.p50_us,
                    result->latency.p95_us, result->latency.p99_us,
                    result->throughput.ops_per_sec, result->throughput.rows_per_sec,
                    result->throughput.mb_per_sec);
            break;

        case OUTPUT_TEXT:
        default:
            printf("\n=== %s ===\n", result->name);
            printf("Queries: %" PRId64 " (failed: %" PRId64 ")\n", result->total_queries, result->failed_queries);
            printf("Rows:    %" PRId64 "\n", result->total_rows);
            printf("Latency (us): min=%.0f, avg=%.0f, p50=%.0f, p95=%.0f, p99=%.0f, max=%.0f\n",
                   result->latency.min_us, result->latency.avg_us,
                   result->latency.p50_us, result->latency.p95_us,
                   result->latency.p99_us, result->latency.max_us);
            printf("Throughput: %.0f ops/s, %.0f rows/s, %.2f MB/s\n",
                   result->throughput.ops_per_sec, result->throughput.rows_per_sec,
                   result->throughput.mb_per_sec);
            break;
    }
}

void bench_suite_print(const BenchSuite *suite, OutputFormat format, FILE *fp)
{
    if (format == OUTPUT_JSON) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"suite\": \"%s\",\n", suite->name);
        fprintf(fp, "  \"version\": \"%s\",\n", suite->version);
        fprintf(fp, "  \"system_info\": \"%s\",\n", suite->system_info);
        fprintf(fp, "  \"scale_factor\": %d,\n", suite->config.scale_factor);
        fprintf(fp, "  \"num_clients\": %d,\n", suite->config.num_clients);
        fprintf(fp, "  \"start_time\": %ld,\n", suite->start_time);
        fprintf(fp, "  \"end_time\": %ld,\n", suite->end_time);
        fprintf(fp, "  \"results\": [\n");

        for (int i = 0; i < suite->num_results; i++) {
            bench_result_print(&suite->results[i], format, fp);
            if (i < suite->num_results - 1) fprintf(fp, ",");
            fprintf(fp, "\n");
        }

        fprintf(fp, "  ]\n");
        fprintf(fp, "}\n");
    } else if (format == OUTPUT_CSV) {
        fprintf(fp, "name,queries,rows,min_us,max_us,avg_us,p50_us,p95_us,p99_us,ops_per_sec,rows_per_sec,mb_per_sec\n");
        for (int i = 0; i < suite->num_results; i++) {
            bench_result_print(&suite->results[i], format, fp);
        }
    } else {
        printf("\n========================================\n");
        printf("Benchmark Suite: %s\n", suite->name);
        printf("Version: %s\n", suite->version);
        printf("System: %s\n", suite->system_info);
        printf("Scale Factor: %d\n", suite->config.scale_factor);
        printf("Clients: %d\n", suite->config.num_clients);
        printf("========================================\n");

        for (int i = 0; i < suite->num_results; i++) {
            bench_result_print(&suite->results[i], format, fp);
        }
    }
}

int bench_suite_save(const BenchSuite *suite, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        LOG_ERROR("Failed to open file: %s", filename);
        return -1;
    }

    bench_suite_print(suite, suite->config.output_format, fp);
    fclose(fp);

    LOG_INFO("Results saved to: %s", filename);
    return 0;
}

void bench_suite_print_summary(const BenchSuite *suite)
{
    printf("\n========================================\n");
    printf("       BENCHMARK SUMMARY\n");
    printf("========================================\n");
    printf("Suite: %s\n", suite->name);
    printf("Tests: %d\n", suite->num_results);
    printf("\n");

    int passed = 0, failed = 0;
    double total_ops = 0;

    for (int i = 0; i < suite->num_results; i++) {
        if (suite->results[i].success) {
            passed++;
        } else {
            failed++;
        }
        total_ops += suite->results[i].throughput.ops_per_sec;

        printf("  %-30s %s (%.0f ops/s)\n",
               suite->results[i].name,
               suite->results[i].success ? "PASS" : "FAIL",
               suite->results[i].throughput.ops_per_sec);
    }

    printf("\n");
    printf("Passed: %d, Failed: %d\n", passed, failed);
    printf("Total Throughput: %.0f ops/s\n", total_ops);
    printf("========================================\n");
}

/* === Data Generation === */

char *bench_generate_random_string(int length)
{
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char *str = malloc(length + 1);
    if (!str) return NULL;

    for (int i = 0; i < length; i++) {
        str[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    str[length] = '\0';

    return str;
}

int64_t bench_generate_random_int(int64_t min, int64_t max)
{
    return min + rand() % (max - min + 1);
}

double bench_generate_random_double(double min, double max)
{
    return min + (double)rand() / RAND_MAX * (max - min);
}

void bench_generate_random_timestamp(char *buffer, int64_t base_epoch, int64_t range_secs)
{
    time_t ts = base_epoch + (rand() % range_secs);
    struct tm *tm = gmtime(&ts);
    strftime(buffer, 32, "%Y-%m-%d %H:%M:%S", tm);
}

void bench_generate_random_uuid(char *buffer)
{
    snprintf(buffer, 37, "%08x-%04x-%04x-%04x-%012lx",
             rand(), rand() & 0xFFFF, (rand() & 0x0FFF) | 0x4000,
             (rand() & 0x3FFF) | 0x8000, (long)rand() << 16 | rand());
}

float *bench_generate_random_vector(int dimensions)
{
    float *vec = malloc(dimensions * sizeof(float));
    if (!vec) return NULL;

    for (int i = 0; i < dimensions; i++) {
        vec[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    }

    return vec;
}

/* === Progress Bar === */

ProgressBar *progress_bar_create(int64_t total, int width)
{
    ProgressBar *pb = calloc(1, sizeof(ProgressBar));
    if (!pb) return NULL;

    pb->total = total;
    pb->width = width;
    pb->start_time = time(NULL);
    pb->show_eta = true;

    return pb;
}

void progress_bar_update(ProgressBar *pb, int64_t current)
{
    pb->current = current;

    int percent = (int)(100.0 * current / pb->total);
    int filled = (int)((double)pb->width * current / pb->total);

    printf("\r[");
    for (int i = 0; i < pb->width; i++) {
        if (i < filled) printf("=");
        else if (i == filled) printf(">");
        else printf(" ");
    }
    printf("] %3d%% ", percent);

    if (pb->show_eta && current > 0) {
        time_t elapsed = time(NULL) - pb->start_time;
        time_t eta = (time_t)(elapsed * (pb->total - current) / current);
        printf("ETA: %lds", eta);
    }

    fflush(stdout);
}

void progress_bar_finish(ProgressBar *pb)
{
    progress_bar_update(pb, pb->total);
    printf("\n");
}

void progress_bar_free(ProgressBar *pb)
{
    free(pb);
}

/* === Utility Functions === */

void shuffle_array(void *array, size_t n, size_t size)
{
    char *arr = array;
    char *tmp = malloc(size);
    if (!tmp) return;

    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        memcpy(tmp, arr + j * size, size);
        memcpy(arr + j * size, arr + i * size, size);
        memcpy(arr + i * size, tmp, size);
    }

    free(tmp);
}

char *format_bytes(int64_t bytes, char *buffer, size_t buflen)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double value = bytes;

    while (value >= 1024 && unit < 4) {
        value /= 1024;
        unit++;
    }

    snprintf(buffer, buflen, "%.2f %s", value, units[unit]);
    return buffer;
}

char *format_duration(double seconds, char *buffer, size_t buflen)
{
    if (seconds < 1) {
        snprintf(buffer, buflen, "%.2f ms", seconds * 1000);
    } else if (seconds < 60) {
        snprintf(buffer, buflen, "%.2f s", seconds);
    } else if (seconds < 3600) {
        snprintf(buffer, buflen, "%.0f min %.0f s", seconds / 60, fmod(seconds, 60));
    } else {
        snprintf(buffer, buflen, "%.0f h %.0f min", seconds / 3600, fmod(seconds / 60, 60));
    }
    return buffer;
}

char *format_number(int64_t number, char *buffer, size_t buflen)
{
    if (number < 1000) {
        snprintf(buffer, buflen, "%" PRId64, number);
    } else if (number < 1000000) {
        snprintf(buffer, buflen, "%.1fK", number / 1000.0);
    } else if (number < 1000000000) {
        snprintf(buffer, buflen, "%.1fM", number / 1000000.0);
    } else {
        snprintf(buffer, buflen, "%.1fB", number / 1000000000.0);
    }
    return buffer;
}

/* === Logging === */

void bench_log(const char *level, const char *format, ...)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(stderr, "[%s] [%s] ", timestamp, level);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
}
