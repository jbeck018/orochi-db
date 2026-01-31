#!/bin/bash
# =============================================================================
# Orochi DB Benchmark Framework - Smart Entrypoint
#
# This entrypoint script:
# 1. Detects available hardware resources (CPU, memory, disk type)
# 2. Auto-scales benchmark parameters based on hardware
# 3. Runs specified benchmark suites
# 4. Outputs results to configurable location
#
# Environment Variables:
#   BENCHMARK_SUITE     - Suite(s) to run: all, tpch, timeseries, columnar,
#                         distributed, vectorized, sysbench, tsbs, pgbench
#   SCALE_FACTOR        - Data scale factor (1=1GB, 10=10GB, etc.)
#   NUM_CLIENTS         - Concurrent clients (auto = detect from CPU)
#   NUM_THREADS         - Threads per client (auto = detect from CPU)
#   DURATION            - Benchmark duration in seconds
#   WARMUP              - Warmup period in seconds
#   OUTPUT_FORMAT       - Output format: json, csv, text
#   RESULTS_DIR         - Directory for results output
#   AUTO_DETECT_HARDWARE - Enable hardware auto-detection
#   COMPARE_POOLERS     - Run comparison with connection poolers
# =============================================================================

set -euo pipefail

# =============================================================================
# Constants and Defaults
# =============================================================================
BENCHMARK_DIR="/opt/orochi-benchmark"
RESULTS_DIR="${RESULTS_DIR:-/results}"
DATA_DIR="${DATA_DIR:-/data}"
LOG_FILE="/var/log/benchmark/benchmark.log"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# =============================================================================
# Logging Functions
# =============================================================================
log_info() {
    echo -e "${BLUE}[INFO]${NC} $(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

log_header() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}========================================${NC}"
}

# =============================================================================
# Hardware Detection Functions
# =============================================================================
detect_cpu_cores() {
    local cores
    if [[ -f /proc/cpuinfo ]]; then
        cores=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo "1")
    elif command -v nproc &>/dev/null; then
        cores=$(nproc)
    elif command -v sysctl &>/dev/null; then
        cores=$(sysctl -n hw.ncpu 2>/dev/null || echo "1")
    else
        cores=1
    fi
    echo "$cores"
}

detect_memory_gb() {
    local mem_kb mem_gb
    if [[ -f /proc/meminfo ]]; then
        mem_kb=$(grep MemTotal /proc/meminfo | awk '{print $2}')
        mem_gb=$((mem_kb / 1024 / 1024))
    elif command -v sysctl &>/dev/null; then
        local mem_bytes
        mem_bytes=$(sysctl -n hw.memsize 2>/dev/null || echo "1073741824")
        mem_gb=$((mem_bytes / 1024 / 1024 / 1024))
    else
        mem_gb=1
    fi
    echo "$mem_gb"
}

detect_disk_type() {
    # Detect if disk is SSD or HDD based on rotational flag
    local disk_type="unknown"

    if [[ -d /sys/block ]]; then
        for disk in /sys/block/sd* /sys/block/nvme* /sys/block/vd* 2>/dev/null; do
            if [[ -f "$disk/queue/rotational" ]]; then
                local rotational
                rotational=$(cat "$disk/queue/rotational" 2>/dev/null || echo "1")
                if [[ "$rotational" == "0" ]]; then
                    disk_type="ssd"
                    break
                else
                    disk_type="hdd"
                fi
            fi
        done
    fi

    # Check for NVMe
    if [[ -d /sys/block/nvme0n1 ]]; then
        disk_type="nvme"
    fi

    echo "$disk_type"
}

detect_cpu_model() {
    if [[ -f /proc/cpuinfo ]]; then
        grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs
    else
        echo "Unknown"
    fi
}

# =============================================================================
# Auto-scaling Functions
# =============================================================================
calculate_clients() {
    local cpu_cores=$1
    local memory_gb=$2

    # Base: 2 clients per core, capped by memory (1GB per 8 clients)
    local cpu_based=$((cpu_cores * 2))
    local mem_based=$((memory_gb * 8))

    # Take minimum and cap at 256
    local clients=$((cpu_based < mem_based ? cpu_based : mem_based))
    clients=$((clients > 256 ? 256 : clients))
    clients=$((clients < 1 ? 1 : clients))

    echo "$clients"
}

calculate_threads() {
    local cpu_cores=$1

    # 1 thread per core, minimum 1, max 16
    local threads=$((cpu_cores < 16 ? cpu_cores : 16))
    threads=$((threads < 1 ? 1 : threads))

    echo "$threads"
}

calculate_scale_factor() {
    local memory_gb=$1
    local disk_type=$2

    # Base scale factor on memory
    # 1GB memory = scale 1, 8GB = scale 10, 64GB = scale 100
    local scale=1

    if [[ $memory_gb -ge 64 ]]; then
        scale=100
    elif [[ $memory_gb -ge 16 ]]; then
        scale=10
    elif [[ $memory_gb -ge 4 ]]; then
        scale=1
    fi

    # Reduce for HDD
    if [[ "$disk_type" == "hdd" ]]; then
        scale=$((scale / 2))
        scale=$((scale < 1 ? 1 : scale))
    fi

    echo "$scale"
}

# =============================================================================
# Database Connection Functions
# =============================================================================
wait_for_database() {
    local max_attempts=60
    local attempt=1

    log_info "Waiting for database at $PGHOST:$PGPORT..."

    while [[ $attempt -le $max_attempts ]]; do
        if pg_isready -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" &>/dev/null; then
            log_success "Database is ready!"
            return 0
        fi

        log_info "Attempt $attempt/$max_attempts - Database not ready, waiting..."
        sleep 2
        ((attempt++))
    done

    log_error "Database not available after $max_attempts attempts"
    return 1
}

setup_database() {
    log_info "Setting up benchmark database..."

    # Create database if not exists
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d postgres \
        -c "SELECT 1 FROM pg_database WHERE datname='$PGDATABASE'" | grep -q 1 || \
        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d postgres \
            -c "CREATE DATABASE $PGDATABASE" 2>/dev/null || true

    # Create extension
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
        -c "CREATE EXTENSION IF NOT EXISTS orochi" 2>/dev/null || \
        log_warn "Orochi extension not available - some benchmarks may be limited"

    # Create pg_stat_statements for query analysis
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
        -c "CREATE EXTENSION IF NOT EXISTS pg_stat_statements" 2>/dev/null || true

    log_success "Database setup complete"
}

# =============================================================================
# Benchmark Execution Functions
# =============================================================================
run_orochi_suite() {
    local suite=$1
    local output_dir="$RESULTS_DIR/$suite"
    mkdir -p "$output_dir"

    log_header "Running Orochi $suite Benchmark"

    if [[ -x "$BENCHMARK_DIR/$suite/run_${suite}.sh" ]]; then
        "$BENCHMARK_DIR/$suite/run_${suite}.sh" \
            --scale="$SCALE_FACTOR" \
            --clients="$NUM_CLIENTS" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --duration="$DURATION" \
            --format="$OUTPUT_FORMAT" \
            --output="$output_dir" 2>&1 | tee -a "$LOG_FILE"
    else
        log_warn "Benchmark suite $suite not found, skipping"
    fi
}

run_pgbench() {
    local output_dir="$RESULTS_DIR/pgbench"
    mkdir -p "$output_dir"

    log_header "Running pgbench Benchmark"

    # Initialize pgbench schema
    pgbench -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
        -i -s "$SCALE_FACTOR" 2>&1 | tee -a "$LOG_FILE"

    # Run benchmark
    local result_file="$output_dir/pgbench_results.txt"
    pgbench -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
        -c "$NUM_CLIENTS" -j "$NUM_THREADS" -T "$DURATION" \
        -P 10 --progress-timestamp 2>&1 | tee "$result_file" | tee -a "$LOG_FILE"

    # Convert to JSON if requested
    if [[ "$OUTPUT_FORMAT" == "json" ]]; then
        convert_pgbench_to_json "$result_file" > "$output_dir/pgbench_results.json"
    fi

    log_success "pgbench benchmark complete"
}

run_sysbench() {
    local output_dir="$RESULTS_DIR/sysbench"
    mkdir -p "$output_dir"

    log_header "Running sysbench Benchmark"

    if ! command -v sysbench &>/dev/null; then
        log_warn "sysbench not installed, skipping"
        return
    fi

    local conn_string="pgsql-host=$PGHOST pgsql-port=$PGPORT pgsql-user=$PGUSER pgsql-password=$PGPASSWORD pgsql-db=$PGDATABASE"

    # OLTP Read-Write test
    log_info "Running OLTP Read-Write test..."
    sysbench /usr/local/share/sysbench/oltp_read_write.lua \
        --db-driver=pgsql $conn_string \
        --tables=10 --table-size=$((SCALE_FACTOR * 100000)) \
        prepare 2>&1 | tee -a "$LOG_FILE"

    sysbench /usr/local/share/sysbench/oltp_read_write.lua \
        --db-driver=pgsql $conn_string \
        --tables=10 --table-size=$((SCALE_FACTOR * 100000)) \
        --threads="$NUM_THREADS" --time="$DURATION" \
        --report-interval=10 \
        run 2>&1 | tee "$output_dir/oltp_rw_results.txt" | tee -a "$LOG_FILE"

    # OLTP Read-Only test
    log_info "Running OLTP Read-Only test..."
    sysbench /usr/local/share/sysbench/oltp_read_only.lua \
        --db-driver=pgsql $conn_string \
        --tables=10 --table-size=$((SCALE_FACTOR * 100000)) \
        --threads="$NUM_THREADS" --time="$DURATION" \
        --report-interval=10 \
        run 2>&1 | tee "$output_dir/oltp_ro_results.txt" | tee -a "$LOG_FILE"

    # Cleanup
    sysbench /usr/local/share/sysbench/oltp_read_write.lua \
        --db-driver=pgsql $conn_string \
        --tables=10 \
        cleanup 2>&1 | tee -a "$LOG_FILE"

    log_success "sysbench benchmark complete"
}

run_tsbs() {
    local output_dir="$RESULTS_DIR/tsbs"
    mkdir -p "$output_dir"

    log_header "Running TSBS (Time Series Benchmark Suite)"

    if ! command -v tsbs_generate_data &>/dev/null; then
        log_warn "TSBS tools not installed, skipping"
        return
    fi

    # Generate data
    log_info "Generating TSBS data..."
    tsbs_generate_data --use-case=cpu-only \
        --seed=123 --scale=$SCALE_FACTOR \
        --timestamp-start="2024-01-01T00:00:00Z" \
        --timestamp-end="2024-01-02T00:00:00Z" \
        --log-interval="10s" \
        --format=timescaledb > "$DATA_DIR/tsbs_data.csv" 2>&1

    # Load data
    log_info "Loading TSBS data..."
    tsbs_load_timescaledb \
        --host="$PGHOST" --port="$PGPORT" \
        --user="$PGUSER" --pass="$PGPASSWORD" \
        --db-name="$PGDATABASE" \
        --workers="$NUM_THREADS" \
        --file="$DATA_DIR/tsbs_data.csv" 2>&1 | tee -a "$LOG_FILE"

    # Generate queries
    log_info "Generating TSBS queries..."
    tsbs_generate_queries --use-case=cpu-only \
        --seed=456 --scale=$SCALE_FACTOR \
        --timestamp-start="2024-01-01T00:00:00Z" \
        --timestamp-end="2024-01-02T00:00:00Z" \
        --queries=1000 --query-type=single-groupby-1-1-1 \
        --format=timescaledb > "$DATA_DIR/tsbs_queries.txt" 2>&1

    # Run queries
    log_info "Running TSBS queries..."
    tsbs_run_queries_timescaledb \
        --host="$PGHOST" --port="$PGPORT" \
        --user="$PGUSER" --pass="$PGPASSWORD" \
        --db-name="$PGDATABASE" \
        --workers="$NUM_THREADS" \
        --file="$DATA_DIR/tsbs_queries.txt" 2>&1 | tee "$output_dir/tsbs_results.txt" | tee -a "$LOG_FILE"

    log_success "TSBS benchmark complete"
}

run_pooler_comparison() {
    local output_dir="$RESULTS_DIR/pooler_comparison"
    mkdir -p "$output_dir"

    log_header "Running Connection Pooler Comparison"

    local poolers=()

    # Direct connection (baseline)
    poolers+=("direct:$PGHOST:$PGPORT")

    # Check for poolers
    if [[ -n "${PGBOUNCER_HOST:-}" ]]; then
        poolers+=("pgbouncer:$PGBOUNCER_HOST:6432")
    fi

    if [[ -n "${PGCAT_HOST:-}" ]]; then
        poolers+=("pgcat:$PGCAT_HOST:6433")
    fi

    if [[ -n "${PGDOG_HOST:-}" ]]; then
        poolers+=("pgdog:$PGDOG_HOST:6434")
    fi

    for pooler_spec in "${poolers[@]}"; do
        IFS=':' read -r name host port <<< "$pooler_spec"

        log_info "Testing $name ($host:$port)..."

        local result_file="$output_dir/${name}_results.txt"

        pgbench -h "$host" -p "$port" -U "$PGUSER" -d "$PGDATABASE" \
            -c "$NUM_CLIENTS" -j "$NUM_THREADS" -T "$((DURATION / 2))" \
            -P 5 --progress-timestamp 2>&1 | tee "$result_file" | tee -a "$LOG_FILE"
    done

    # Generate comparison report
    generate_pooler_comparison_report "$output_dir"

    log_success "Pooler comparison complete"
}

# =============================================================================
# Report Generation Functions
# =============================================================================
convert_pgbench_to_json() {
    local input_file=$1

    # Extract key metrics from pgbench output
    local tps latency_avg latency_stddev
    tps=$(grep "tps = " "$input_file" | tail -1 | awk '{print $3}')
    latency_avg=$(grep "latency average" "$input_file" | awk '{print $4}')
    latency_stddev=$(grep "latency stddev" "$input_file" | awk '{print $4}')

    cat << EOF
{
  "benchmark": "pgbench",
  "timestamp": "$(date -Iseconds)",
  "config": {
    "scale_factor": $SCALE_FACTOR,
    "clients": $NUM_CLIENTS,
    "threads": $NUM_THREADS,
    "duration": $DURATION
  },
  "results": {
    "tps": ${tps:-0},
    "latency_avg_ms": ${latency_avg:-0},
    "latency_stddev_ms": ${latency_stddev:-0}
  }
}
EOF
}

generate_pooler_comparison_report() {
    local output_dir=$1
    local report_file="$output_dir/comparison_report.json"

    echo "{" > "$report_file"
    echo '  "benchmark": "pooler_comparison",' >> "$report_file"
    echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$report_file"
    echo '  "results": {' >> "$report_file"

    local first=true
    for result_file in "$output_dir"/*_results.txt; do
        if [[ -f "$result_file" ]]; then
            local name
            name=$(basename "$result_file" _results.txt)
            local tps
            tps=$(grep "tps = " "$result_file" | tail -1 | awk '{print $3}' || echo "0")

            if [[ "$first" != "true" ]]; then
                echo "," >> "$report_file"
            fi
            first=false

            echo -n "    \"$name\": {\"tps\": ${tps:-0}}" >> "$report_file"
        fi
    done

    echo "" >> "$report_file"
    echo "  }" >> "$report_file"
    echo "}" >> "$report_file"
}

generate_system_info() {
    local output_file="$RESULTS_DIR/system_info.json"

    cat > "$output_file" << EOF
{
  "timestamp": "$(date -Iseconds)",
  "hostname": "$(hostname)",
  "system": {
    "cpu_model": "$(detect_cpu_model)",
    "cpu_cores": $CPU_CORES,
    "memory_gb": $MEMORY_GB,
    "disk_type": "$DISK_TYPE"
  },
  "benchmark_config": {
    "scale_factor": $SCALE_FACTOR,
    "num_clients": $NUM_CLIENTS,
    "num_threads": $NUM_THREADS,
    "duration": $DURATION,
    "warmup": $WARMUP,
    "output_format": "$OUTPUT_FORMAT"
  },
  "database": {
    "host": "$PGHOST",
    "port": $PGPORT,
    "database": "$PGDATABASE"
  }
}
EOF

    log_info "System info saved to $output_file"
}

generate_summary_report() {
    local summary_file="$RESULTS_DIR/summary_$(date +%Y%m%d_%H%M%S).json"

    log_header "Generating Summary Report"

    cat > "$summary_file" << EOF
{
  "benchmark_run": {
    "timestamp": "$(date -Iseconds)",
    "suites_run": "$BENCHMARK_SUITE",
    "duration_total": $SECONDS,
    "status": "complete"
  },
  "system_info": $(cat "$RESULTS_DIR/system_info.json" 2>/dev/null || echo '{}'),
  "results_location": "$RESULTS_DIR"
}
EOF

    log_success "Summary report saved to $summary_file"

    # Print summary to console
    echo ""
    log_header "Benchmark Run Complete"
    echo "Results saved to: $RESULTS_DIR"
    echo "Duration: $SECONDS seconds"
    echo ""
    ls -la "$RESULTS_DIR/"
}

# =============================================================================
# Help Function
# =============================================================================
show_help() {
    cat << EOF
Orochi DB Benchmark Framework

Usage: entrypoint.sh [OPTIONS] [SUITE...]

Options:
  --help, -h          Show this help message
  --list              List available benchmark suites
  --scale=N           Override scale factor
  --clients=N         Override number of clients
  --threads=N         Override number of threads
  --duration=N        Override benchmark duration (seconds)
  --warmup=N          Override warmup period (seconds)
  --format=FMT        Output format: json, csv, text
  --output=DIR        Results output directory

Available Benchmark Suites:
  all                 Run all available benchmarks
  tpch                TPC-H style analytical queries
  timeseries          Time-series workloads
  columnar            Columnar storage benchmarks
  distributed         Distributed query benchmarks
  vectorized          Vectorized execution benchmarks
  pgbench             Standard PostgreSQL pgbench
  sysbench            Sysbench OLTP workloads
  tsbs                Time Series Benchmark Suite

Environment Variables:
  BENCHMARK_SUITE     Suite(s) to run (comma-separated)
  SCALE_FACTOR        Data scale factor (1=1GB, 10=10GB, etc.)
  NUM_CLIENTS         Concurrent clients (auto = detect from CPU)
  NUM_THREADS         Threads per client (auto = detect from CPU)
  DURATION            Benchmark duration in seconds
  WARMUP              Warmup period in seconds
  OUTPUT_FORMAT       Output format: json, csv, text
  RESULTS_DIR         Directory for results output
  AUTO_DETECT_HARDWARE Enable hardware auto-detection
  COMPARE_POOLERS     Run comparison with connection poolers
  PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE

Examples:
  # Run all benchmarks with auto-detection
  docker run orochi-benchmark

  # Run specific suites
  docker run -e BENCHMARK_SUITE=tpch,timeseries orochi-benchmark

  # High-scale benchmark
  docker run -e SCALE_FACTOR=100 -e NUM_CLIENTS=64 orochi-benchmark

  # Compare connection poolers
  docker run -e COMPARE_POOLERS=true orochi-benchmark

EOF
}

# =============================================================================
# Main Execution
# =============================================================================
main() {
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --help|-h)
                show_help
                exit 0
                ;;
            --list)
                echo "Available benchmark suites:"
                echo "  tpch, timeseries, columnar, distributed, vectorized"
                echo "  pgbench, sysbench, tsbs"
                exit 0
                ;;
            --scale=*)
                SCALE_FACTOR="${1#*=}"
                ;;
            --clients=*)
                NUM_CLIENTS="${1#*=}"
                ;;
            --threads=*)
                NUM_THREADS="${1#*=}"
                ;;
            --duration=*)
                DURATION="${1#*=}"
                ;;
            --warmup=*)
                WARMUP="${1#*=}"
                ;;
            --format=*)
                OUTPUT_FORMAT="${1#*=}"
                ;;
            --output=*)
                RESULTS_DIR="${1#*=}"
                ;;
            *)
                # Treat as suite name
                BENCHMARK_SUITE="$1"
                ;;
        esac
        shift
    done

    # Create log directory
    mkdir -p "$(dirname "$LOG_FILE")"
    mkdir -p "$RESULTS_DIR"
    mkdir -p "$DATA_DIR"

    log_header "Orochi DB Benchmark Framework v1.0"

    # Hardware detection
    log_info "Detecting hardware..."
    CPU_CORES=$(detect_cpu_cores)
    MEMORY_GB=$(detect_memory_gb)
    DISK_TYPE=$(detect_disk_type)

    log_info "Detected: ${CPU_CORES} CPU cores, ${MEMORY_GB}GB memory, ${DISK_TYPE} storage"

    # Auto-scale parameters if set to auto
    if [[ "${AUTO_DETECT_HARDWARE:-true}" == "true" ]]; then
        if [[ "${NUM_CLIENTS:-auto}" == "auto" ]]; then
            NUM_CLIENTS=$(calculate_clients "$CPU_CORES" "$MEMORY_GB")
            log_info "Auto-scaled clients: $NUM_CLIENTS"
        fi

        if [[ "${NUM_THREADS:-auto}" == "auto" ]]; then
            NUM_THREADS=$(calculate_threads "$CPU_CORES")
            log_info "Auto-scaled threads: $NUM_THREADS"
        fi

        if [[ -z "${SCALE_FACTOR:-}" ]] || [[ "${SCALE_FACTOR:-}" == "auto" ]]; then
            SCALE_FACTOR=$(calculate_scale_factor "$MEMORY_GB" "$DISK_TYPE")
            log_info "Auto-scaled scale factor: $SCALE_FACTOR"
        fi
    fi

    # Set defaults for any remaining unset variables
    SCALE_FACTOR="${SCALE_FACTOR:-1}"
    NUM_CLIENTS="${NUM_CLIENTS:-1}"
    NUM_THREADS="${NUM_THREADS:-1}"
    DURATION="${DURATION:-60}"
    WARMUP="${WARMUP:-10}"
    OUTPUT_FORMAT="${OUTPUT_FORMAT:-json}"
    BENCHMARK_SUITE="${BENCHMARK_SUITE:-all}"

    log_info "Configuration:"
    log_info "  Scale Factor: $SCALE_FACTOR"
    log_info "  Clients: $NUM_CLIENTS"
    log_info "  Threads: $NUM_THREADS"
    log_info "  Duration: ${DURATION}s"
    log_info "  Warmup: ${WARMUP}s"
    log_info "  Output Format: $OUTPUT_FORMAT"
    log_info "  Suites: $BENCHMARK_SUITE"

    # Wait for database
    wait_for_database || exit 1

    # Setup database
    setup_database

    # Generate system info
    generate_system_info

    # Run selected benchmark suites
    SECONDS=0

    IFS=',' read -ra SUITES <<< "$BENCHMARK_SUITE"
    for suite in "${SUITES[@]}"; do
        case $suite in
            all)
                run_orochi_suite "tpch"
                run_orochi_suite "timeseries"
                run_orochi_suite "columnar"
                run_orochi_suite "distributed"
                run_orochi_suite "vectorized"
                run_pgbench
                run_sysbench
                ;;
            tpch|timeseries|columnar|distributed|vectorized)
                run_orochi_suite "$suite"
                ;;
            pgbench)
                run_pgbench
                ;;
            sysbench)
                run_sysbench
                ;;
            tsbs)
                run_tsbs
                ;;
            *)
                log_warn "Unknown benchmark suite: $suite"
                ;;
        esac
    done

    # Run pooler comparison if enabled
    if [[ "${COMPARE_POOLERS:-false}" == "true" ]]; then
        run_pooler_comparison
    fi

    # Generate final summary
    generate_summary_report
}

# Execute main function
main "$@"
