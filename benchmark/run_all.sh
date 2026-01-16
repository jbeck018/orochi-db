#!/bin/bash
#
# Orochi DB Comprehensive Benchmark Suite
# Runs all benchmark categories and generates unified reports
#

set -e

# Default configuration
SCALE_FACTOR=1
NUM_CLIENTS=1
NUM_THREADS=1
DURATION=60
WARMUP=10
FORMAT="json"
OUTPUT_DIR="results"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
VERBOSE=0

# Benchmark categories
RUN_TPCH=1
RUN_TIMESERIES=1
RUN_COLUMNAR=1
RUN_DISTRIBUTED=1
RUN_VECTORIZED=1

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat << EOF
Orochi DB Benchmark Suite

Usage: $(basename "$0") [OPTIONS]

Options:
  --scale=N         Scale factor (1=1GB, 10=10GB, 100=100GB) [default: 1]
  --clients=N       Number of concurrent clients [default: 1]
  --threads=N       Threads per client [default: 1]
  --duration=N      Benchmark duration in seconds [default: 60]
  --warmup=N        Warmup period in seconds [default: 10]
  --format=FORMAT   Output format: json, csv, text [default: json]
  --output=DIR      Output directory [default: results]
  --host=HOST       PostgreSQL host [default: localhost]
  --port=PORT       PostgreSQL port [default: 5432]
  --user=USER       PostgreSQL user [default: postgres]
  --database=DB     Database name [default: orochi_bench]
  --only=SUITE      Run only specified suite(s): tpch,timeseries,columnar,distributed,vectorized
  --skip=SUITE      Skip specified suite(s)
  --verbose         Enable verbose output
  --help            Show this help message

Examples:
  $(basename "$0") --scale=10 --clients=8
  $(basename "$0") --only=tpch,timeseries --duration=300
  $(basename "$0") --scale=100 --clients=16 --format=csv

EOF
    exit 0
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
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
                FORMAT="${1#*=}"
                ;;
            --output=*)
                OUTPUT_DIR="${1#*=}"
                ;;
            --host=*)
                PGHOST="${1#*=}"
                ;;
            --port=*)
                PGPORT="${1#*=}"
                ;;
            --user=*)
                PGUSER="${1#*=}"
                ;;
            --database=*)
                PGDATABASE="${1#*=}"
                ;;
            --only=*)
                RUN_TPCH=0
                RUN_TIMESERIES=0
                RUN_COLUMNAR=0
                RUN_DISTRIBUTED=0
                RUN_VECTORIZED=0
                IFS=',' read -ra SUITES <<< "${1#*=}"
                for suite in "${SUITES[@]}"; do
                    case $suite in
                        tpch) RUN_TPCH=1 ;;
                        timeseries) RUN_TIMESERIES=1 ;;
                        columnar) RUN_COLUMNAR=1 ;;
                        distributed) RUN_DISTRIBUTED=1 ;;
                        vectorized) RUN_VECTORIZED=1 ;;
                    esac
                done
                ;;
            --skip=*)
                IFS=',' read -ra SUITES <<< "${1#*=}"
                for suite in "${SUITES[@]}"; do
                    case $suite in
                        tpch) RUN_TPCH=0 ;;
                        timeseries) RUN_TIMESERIES=0 ;;
                        columnar) RUN_COLUMNAR=0 ;;
                        distributed) RUN_DISTRIBUTED=0 ;;
                        vectorized) RUN_VECTORIZED=0 ;;
                    esac
                done
                ;;
            --verbose)
                VERBOSE=1
                ;;
            --help)
                usage
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
        shift
    done
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check for psql
    if ! command -v psql &> /dev/null; then
        log_error "psql not found. Please install PostgreSQL client."
        exit 1
    fi

    # Check PostgreSQL connection
    if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -c "SELECT 1" &> /dev/null; then
        log_error "Cannot connect to PostgreSQL at $PGHOST:$PGPORT"
        exit 1
    fi

    # Check if database exists
    if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &> /dev/null; then
        log_warn "Database $PGDATABASE does not exist, creating..."
        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -c "CREATE DATABASE $PGDATABASE"
    fi

    # Check if orochi extension is available
    if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS orochi" &> /dev/null; then
        log_warn "Orochi extension not available. Some benchmarks may fail."
    fi

    log_success "Prerequisites check passed"
}

setup_results_directory() {
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    RESULTS_PATH="${OUTPUT_DIR}/${TIMESTAMP}"
    mkdir -p "$RESULTS_PATH"/{tpch,timeseries,columnar,distributed,vectorized}

    log_info "Results will be saved to: $RESULTS_PATH"
}

run_tpch_benchmark() {
    if [[ $RUN_TPCH -ne 1 ]]; then
        return
    fi

    log_info "Running TPC-H Style Benchmarks..."

    cd "$SCRIPT_DIR/tpch"

    if [[ -x "./run_tpch.sh" ]]; then
        ./run_tpch.sh \
            --scale="$SCALE_FACTOR" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --duration="$DURATION" \
            --format="$FORMAT" \
            --output="$RESULTS_PATH/tpch"
    else
        log_warn "TPC-H benchmark script not found or not executable"
    fi

    cd "$SCRIPT_DIR"
    log_success "TPC-H benchmarks completed"
}

run_timeseries_benchmark() {
    if [[ $RUN_TIMESERIES -ne 1 ]]; then
        return
    fi

    log_info "Running Time-Series Benchmarks..."

    cd "$SCRIPT_DIR/timeseries"

    if [[ -x "./run_timeseries.sh" ]]; then
        ./run_timeseries.sh \
            --scale="$SCALE_FACTOR" \
            --clients="$NUM_CLIENTS" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --duration="$DURATION" \
            --format="$FORMAT" \
            --output="$RESULTS_PATH/timeseries"
    else
        log_warn "Time-series benchmark script not found or not executable"
    fi

    cd "$SCRIPT_DIR"
    log_success "Time-series benchmarks completed"
}

run_columnar_benchmark() {
    if [[ $RUN_COLUMNAR -ne 1 ]]; then
        return
    fi

    log_info "Running Columnar Storage Benchmarks..."

    cd "$SCRIPT_DIR/columnar"

    if [[ -x "./run_columnar.sh" ]]; then
        ./run_columnar.sh \
            --scale="$SCALE_FACTOR" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --format="$FORMAT" \
            --output="$RESULTS_PATH/columnar"
    else
        log_warn "Columnar benchmark script not found or not executable"
    fi

    cd "$SCRIPT_DIR"
    log_success "Columnar benchmarks completed"
}

run_distributed_benchmark() {
    if [[ $RUN_DISTRIBUTED -ne 1 ]]; then
        return
    fi

    log_info "Running Distributed Benchmarks..."

    cd "$SCRIPT_DIR/distributed"

    if [[ -x "./run_distributed.sh" ]]; then
        ./run_distributed.sh \
            --scale="$SCALE_FACTOR" \
            --clients="$NUM_CLIENTS" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --duration="$DURATION" \
            --format="$FORMAT" \
            --output="$RESULTS_PATH/distributed"
    else
        log_warn "Distributed benchmark script not found or not executable"
    fi

    cd "$SCRIPT_DIR"
    log_success "Distributed benchmarks completed"
}

run_vectorized_benchmark() {
    if [[ $RUN_VECTORIZED -ne 1 ]]; then
        return
    fi

    log_info "Running Vectorized Execution Benchmarks..."

    cd "$SCRIPT_DIR/vectorized"

    if [[ -x "./run_vectorized.sh" ]]; then
        ./run_vectorized.sh \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --user="$PGUSER" \
            --database="$PGDATABASE" \
            --format="$FORMAT" \
            --output="$RESULTS_PATH/vectorized"
    else
        log_warn "Vectorized benchmark script not found or not executable"
    fi

    cd "$SCRIPT_DIR"
    log_success "Vectorized benchmarks completed"
}

generate_summary() {
    log_info "Generating summary report..."

    SUMMARY_FILE="$RESULTS_PATH/summary.${FORMAT}"

    if [[ "$FORMAT" == "json" ]]; then
        cat > "$SUMMARY_FILE" << EOF
{
  "benchmark_run": {
    "timestamp": "$(date -Iseconds)",
    "config": {
      "scale_factor": $SCALE_FACTOR,
      "num_clients": $NUM_CLIENTS,
      "num_threads": $NUM_THREADS,
      "duration_secs": $DURATION,
      "warmup_secs": $WARMUP
    },
    "database": {
      "host": "$PGHOST",
      "port": $PGPORT,
      "database": "$PGDATABASE"
    },
    "system": {
      "hostname": "$(hostname)",
      "kernel": "$(uname -r)",
      "cpus": $(nproc),
      "memory_gb": $(free -g | awk '/Mem:/{print $2}')
    },
    "suites": {
      "tpch": $(if [[ $RUN_TPCH -eq 1 ]]; then echo "true"; else echo "false"; fi),
      "timeseries": $(if [[ $RUN_TIMESERIES -eq 1 ]]; then echo "true"; else echo "false"; fi),
      "columnar": $(if [[ $RUN_COLUMNAR -eq 1 ]]; then echo "true"; else echo "false"; fi),
      "distributed": $(if [[ $RUN_DISTRIBUTED -eq 1 ]]; then echo "true"; else echo "false"; fi),
      "vectorized": $(if [[ $RUN_VECTORIZED -eq 1 ]]; then echo "true"; else echo "false"; fi)
    }
  }
}
EOF
    else
        cat > "$SUMMARY_FILE" << EOF
Orochi DB Benchmark Summary
===========================
Timestamp: $(date)
Scale Factor: $SCALE_FACTOR
Clients: $NUM_CLIENTS
Duration: ${DURATION}s

System Information:
- Hostname: $(hostname)
- Kernel: $(uname -r)
- CPUs: $(nproc)
- Memory: $(free -h | awk '/Mem:/{print $2}')

Suites Run:
- TPC-H: $(if [[ $RUN_TPCH -eq 1 ]]; then echo "Yes"; else echo "No"; fi)
- Time-Series: $(if [[ $RUN_TIMESERIES -eq 1 ]]; then echo "Yes"; else echo "No"; fi)
- Columnar: $(if [[ $RUN_COLUMNAR -eq 1 ]]; then echo "Yes"; else echo "No"; fi)
- Distributed: $(if [[ $RUN_DISTRIBUTED -eq 1 ]]; then echo "Yes"; else echo "No"; fi)
- Vectorized: $(if [[ $RUN_VECTORIZED -eq 1 ]]; then echo "Yes"; else echo "No"; fi)
EOF
    fi

    log_success "Summary saved to: $SUMMARY_FILE"
}

print_final_report() {
    echo ""
    echo "========================================"
    echo "       OROCHI DB BENCHMARK COMPLETE"
    echo "========================================"
    echo ""
    echo "Configuration:"
    echo "  Scale Factor: $SCALE_FACTOR"
    echo "  Clients:      $NUM_CLIENTS"
    echo "  Duration:     ${DURATION}s"
    echo ""
    echo "Results Location: $RESULTS_PATH"
    echo ""

    # List result files
    echo "Result Files:"
    find "$RESULTS_PATH" -name "*.${FORMAT}" -type f | while read -r file; do
        echo "  - $(basename "$(dirname "$file")")/$(basename "$file")"
    done

    echo ""
    echo "========================================"
}

main() {
    echo ""
    echo "========================================"
    echo "    OROCHI DB BENCHMARK SUITE v1.0"
    echo "========================================"
    echo ""

    parse_args "$@"

    log_info "Configuration:"
    log_info "  Scale Factor: $SCALE_FACTOR"
    log_info "  Clients:      $NUM_CLIENTS"
    log_info "  Threads:      $NUM_THREADS"
    log_info "  Duration:     ${DURATION}s"
    log_info "  Warmup:       ${WARMUP}s"
    log_info "  Database:     $PGHOST:$PGPORT/$PGDATABASE"
    echo ""

    check_prerequisites
    setup_results_directory

    START_TIME=$(date +%s)

    # Run benchmark suites
    run_tpch_benchmark
    run_timeseries_benchmark
    run_columnar_benchmark
    run_distributed_benchmark
    run_vectorized_benchmark

    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    generate_summary
    print_final_report

    log_success "Total benchmark time: ${ELAPSED}s"
}

main "$@"
