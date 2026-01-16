/*
 * Orochi DB Benchmarking Framework
 * Common utilities and definitions
 */

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <math.h>
#include <libpq-fe.h>

/* Version */
#define BENCH_VERSION "1.0.0"

/* Maximum limits */
#define MAX_QUERY_LEN       65536
#define MAX_PATH_LEN        4096
#define MAX_NAME_LEN        256
#define MAX_CLIENTS         256
#define MAX_SAMPLES         1000000
#define MAX_PERCENTILES     10

/* Result output formats */
typedef enum {
    OUTPUT_JSON,
    OUTPUT_CSV,
    OUTPUT_TEXT
} OutputFormat;

/* Benchmark configuration */
typedef struct BenchConfig {
    /* Database connection */
    char host[MAX_NAME_LEN];
    int port;
    char user[MAX_NAME_LEN];
    char password[MAX_NAME_LEN];
    char database[MAX_NAME_LEN];

    /* Benchmark parameters */
    int scale_factor;
    int num_clients;
    int num_threads;
    int duration_secs;
    int warmup_secs;
    int think_time_ms;

    /* Output settings */
    OutputFormat output_format;
    char output_dir[MAX_PATH_LEN];
    char output_file[MAX_PATH_LEN];
    bool verbose;

    /* Feature flags */
    bool use_prepared;
    bool use_transactions;
    bool collect_samples;
} BenchConfig;

/* Latency statistics */
typedef struct LatencyStats {
    double min_us;
    double max_us;
    double avg_us;
    double stddev_us;
    double p50_us;      /* median */
    double p90_us;
    double p95_us;
    double p99_us;
    double p999_us;
    int64_t count;
    double *samples;    /* raw samples for percentile calculation */
    int64_t num_samples;
    int64_t max_samples;
} LatencyStats;

/* Throughput statistics */
typedef struct ThroughputStats {
    double total_ops;
    double ops_per_sec;
    double rows_per_sec;
    double mb_per_sec;
    double duration_secs;
} ThroughputStats;

/* Single benchmark result */
typedef struct BenchResult {
    char name[MAX_NAME_LEN];
    char description[MAX_QUERY_LEN];
    LatencyStats latency;
    ThroughputStats throughput;
    int64_t total_queries;
    int64_t failed_queries;
    int64_t total_rows;
    int64_t total_bytes;
    double compression_ratio;
    bool success;
    char error_msg[MAX_QUERY_LEN];
    time_t start_time;
    time_t end_time;
} BenchResult;

/* Benchmark suite */
typedef struct BenchSuite {
    char name[MAX_NAME_LEN];
    char version[64];
    BenchConfig config;
    BenchResult *results;
    int num_results;
    int max_results;
    time_t start_time;
    time_t end_time;
    char system_info[MAX_QUERY_LEN];
} BenchSuite;

/* Timer utilities */
typedef struct Timer {
    struct timeval start;
    struct timeval end;
    bool running;
} Timer;

/* Thread context for concurrent benchmarks */
typedef struct ThreadContext {
    int thread_id;
    BenchConfig *config;
    PGconn *conn;
    LatencyStats *stats;
    volatile bool *should_stop;
    int64_t ops_completed;
    int64_t ops_failed;
    int64_t rows_processed;
    int64_t bytes_processed;
    pthread_t thread;
} ThreadContext;

/* === Function declarations === */

/* Configuration */
BenchConfig *bench_config_create(void);
void bench_config_free(BenchConfig *config);
int bench_config_parse_args(BenchConfig *config, int argc, char *argv[]);
void bench_config_print(const BenchConfig *config);

/* Database connection */
PGconn *bench_connect(const BenchConfig *config);
void bench_disconnect(PGconn *conn);
bool bench_exec(PGconn *conn, const char *query);
PGresult *bench_query(PGconn *conn, const char *query);
bool bench_prepare(PGconn *conn, const char *name, const char *query);
PGresult *bench_exec_prepared(PGconn *conn, const char *name,
                               int nparams, const char *const *params);

/* Timer functions */
void timer_start(Timer *timer);
void timer_stop(Timer *timer);
double timer_elapsed_us(const Timer *timer);
double timer_elapsed_ms(const Timer *timer);
double timer_elapsed_secs(const Timer *timer);
int64_t get_timestamp_us(void);

/* Latency statistics */
LatencyStats *latency_stats_create(int64_t max_samples);
void latency_stats_free(LatencyStats *stats);
void latency_stats_add(LatencyStats *stats, double latency_us);
void latency_stats_merge(LatencyStats *dest, const LatencyStats *src);
void latency_stats_compute(LatencyStats *stats);
void latency_stats_reset(LatencyStats *stats);

/* Throughput statistics */
void throughput_stats_compute(ThroughputStats *stats, int64_t ops,
                               int64_t rows, int64_t bytes, double duration);

/* Benchmark result */
BenchResult *bench_result_create(const char *name);
void bench_result_free(BenchResult *result);

/* Benchmark suite */
BenchSuite *bench_suite_create(const char *name, int max_results);
void bench_suite_free(BenchSuite *suite);
void bench_suite_add_result(BenchSuite *suite, BenchResult *result);
void bench_suite_collect_system_info(BenchSuite *suite);

/* Output functions */
void bench_result_print(const BenchResult *result, OutputFormat format, FILE *fp);
void bench_suite_print(const BenchSuite *suite, OutputFormat format, FILE *fp);
int bench_suite_save(const BenchSuite *suite, const char *filename);
void bench_suite_print_summary(const BenchSuite *suite);

/* Data generation */
char *bench_generate_random_string(int length);
int64_t bench_generate_random_int(int64_t min, int64_t max);
double bench_generate_random_double(double min, double max);
void bench_generate_random_timestamp(char *buffer, int64_t base_epoch, int64_t range_secs);
void bench_generate_random_uuid(char *buffer);
float *bench_generate_random_vector(int dimensions);

/* Progress reporting */
typedef struct ProgressBar {
    int64_t total;
    int64_t current;
    int width;
    time_t start_time;
    bool show_eta;
} ProgressBar;

ProgressBar *progress_bar_create(int64_t total, int width);
void progress_bar_update(ProgressBar *pb, int64_t current);
void progress_bar_finish(ProgressBar *pb);
void progress_bar_free(ProgressBar *pb);

/* Utility functions */
double calculate_percentile(double *samples, int64_t count, double percentile);
void shuffle_array(void *array, size_t n, size_t size);
char *format_bytes(int64_t bytes, char *buffer, size_t buflen);
char *format_duration(double seconds, char *buffer, size_t buflen);
char *format_number(int64_t number, char *buffer, size_t buflen);
int compare_doubles(const void *a, const void *b);

/* Logging */
void bench_log(const char *level, const char *format, ...);
#define LOG_INFO(...)  bench_log("INFO", __VA_ARGS__)
#define LOG_WARN(...)  bench_log("WARN", __VA_ARGS__)
#define LOG_ERROR(...) bench_log("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...) if (g_verbose) bench_log("DEBUG", __VA_ARGS__)

/* Global verbose flag */
extern bool g_verbose;

#endif /* BENCH_COMMON_H */
