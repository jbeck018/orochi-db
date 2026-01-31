#!/bin/bash
#
# Orochi DB Connection Pooler Benchmark Runner
#
# Runs comprehensive connection benchmarks using pgbench and custom C benchmarks.
# Supports multiple poolers: pgbouncer, pgcat, pgdog, direct
#
# Usage: ./run_connection.sh [OPTIONS]
#

set -e

# Default configuration
POOLER="direct"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGPASSWORD="${PGPASSWORD:-}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
DURATION=30
WARMUP=5
CLIENTS_LIST="10 50 100 200 500 1000"
FORMAT="json"
OUTPUT_DIR="results"
VERBOSE=0
USE_TLS=0
RUN_PGBENCH=1
RUN_CUSTOM=1

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

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

log_header() {
    echo -e "\n${CYAN}========================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}========================================${NC}\n"
}

usage() {
    cat << EOF
Orochi DB Connection Pooler Benchmark

Usage: $(basename "$0") [OPTIONS]

Options:
  --pooler=TYPE       Pooler type: pgbouncer|pgcat|pgdog|direct [default: direct]
  --host=HOST         PostgreSQL/Pooler host [default: localhost]
  --port=PORT         PostgreSQL/Pooler port [default: 5432]
  --user=USER         Database user [default: postgres]
  --password=PASS     Database password
  --database=DB       Database name [default: orochi_bench]
  --duration=N        Test duration in seconds [default: 30]
  --warmup=N          Warmup duration in seconds [default: 5]
  --clients=LIST      Comma-separated client counts [default: 10,50,100,200,500,1000]
  --format=FMT        Output format: json|csv [default: json]
  --output=DIR        Output directory [default: results]
  --tls               Enable TLS/SSL
  --pgbench-only      Run only pgbench tests
  --custom-only       Run only custom C benchmark tests
  --verbose           Enable verbose output
  --help              Show this help

Examples:
  $(basename "$0") --pooler=pgbouncer --port=6432
  $(basename "$0") --pooler=pgcat --clients=10,100,500 --duration=60
  $(basename "$0") --pooler=direct --pgbench-only

Pooler Ports (typical):
  - Direct PostgreSQL: 5432
  - PgBouncer:         6432
  - PgCat:             6433
  - PgDog:             6434

EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --pooler=*)
                POOLER="${1#*=}"
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
            --password=*)
                PGPASSWORD="${1#*=}"
                ;;
            --database=*)
                PGDATABASE="${1#*=}"
                ;;
            --duration=*)
                DURATION="${1#*=}"
                ;;
            --warmup=*)
                WARMUP="${1#*=}"
                ;;
            --clients=*)
                CLIENTS_LIST="${1#*=}"
                CLIENTS_LIST="${CLIENTS_LIST//,/ }"
                ;;
            --format=*)
                FORMAT="${1#*=}"
                ;;
            --output=*)
                OUTPUT_DIR="${1#*=}"
                ;;
            --tls)
                USE_TLS=1
                ;;
            --pgbench-only)
                RUN_PGBENCH=1
                RUN_CUSTOM=0
                ;;
            --custom-only)
                RUN_PGBENCH=0
                RUN_CUSTOM=1
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

    # Check for pgbench
    if ! command -v pgbench &> /dev/null; then
        log_warn "pgbench not found. pgbench tests will be skipped."
        RUN_PGBENCH=0
    fi

    # Check for psql
    if ! command -v psql &> /dev/null; then
        log_error "psql not found. Please install PostgreSQL client."
        exit 1
    fi

    # Check custom benchmark binary
    if [[ ! -x "$SCRIPT_DIR/connection_bench" ]]; then
        log_warn "Custom benchmark not built. Attempting to build..."
        make -C "$SCRIPT_DIR" connection_bench 2>/dev/null || {
            log_warn "Failed to build custom benchmark. Custom tests will be skipped."
            RUN_CUSTOM=0
        }
    fi

    # Check connection
    local conn_opts="-h $PGHOST -p $PGPORT -U $PGUSER"
    if [[ -n "$PGPASSWORD" ]]; then
        export PGPASSWORD
    fi

    if ! psql $conn_opts -c "SELECT 1" -d "$PGDATABASE" &> /dev/null; then
        log_error "Cannot connect to PostgreSQL at $PGHOST:$PGPORT"
        log_error "Check your connection settings and ensure the database exists."
        exit 1
    fi

    log_success "Prerequisites check passed"
}

setup_output_directory() {
    RESULTS_PATH="${OUTPUT_DIR}/connection_${POOLER}_${TIMESTAMP}"
    mkdir -p "$RESULTS_PATH"
    log_info "Results will be saved to: $RESULTS_PATH"
}

# Initialize pgbench tables if needed
init_pgbench() {
    log_info "Initializing pgbench tables..."

    local conn_opts="-h $PGHOST -p $PGPORT -U $PGUSER"

    # Check if tables exist
    if psql $conn_opts -d "$PGDATABASE" -c "\d pgbench_accounts" &> /dev/null; then
        log_info "pgbench tables already exist"
        return 0
    fi

    # Initialize with scale factor 1
    pgbench $conn_opts -d "$PGDATABASE" -i -s 1 || {
        log_warn "Failed to initialize pgbench tables"
        return 1
    }

    log_success "pgbench tables initialized"
}

# Run pgbench connection test (-C flag for connection per query)
run_pgbench_connection_test() {
    local clients=$1
    local output_file="${RESULTS_PATH}/pgbench_conn_${clients}clients.${FORMAT}"

    log_info "Running pgbench connection test: $clients clients, $DURATION seconds"

    local conn_opts="-h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE"

    # Run pgbench with -C (connection per transaction)
    local result
    result=$(pgbench $conn_opts \
        -c "$clients" \
        -j "$(( clients < 16 ? clients : 16 ))" \
        -T "$DURATION" \
        -C \
        -S \
        --progress=5 \
        2>&1) || true

    # Parse results
    local tps=$(echo "$result" | grep -oP 'tps = \K[0-9.]+' | tail -1)
    local avg_latency=$(echo "$result" | grep -oP 'latency average = \K[0-9.]+' | head -1)
    local stddev=$(echo "$result" | grep -oP 'latency stddev = \K[0-9.]+' | head -1)
    local transactions=$(echo "$result" | grep -oP 'number of transactions actually processed: \K[0-9]+' | head -1)

    if [[ "$FORMAT" == "json" ]]; then
        cat > "$output_file" << EOF
{
  "benchmark": "pgbench_connection",
  "pooler": "$POOLER",
  "clients": $clients,
  "duration_secs": $DURATION,
  "results": {
    "tps": ${tps:-0},
    "avg_latency_ms": ${avg_latency:-0},
    "stddev_ms": ${stddev:-0},
    "transactions": ${transactions:-0}
  },
  "timestamp": "$(date -Iseconds)",
  "raw_output": $(echo "$result" | jq -Rs '.')
}
EOF
    else
        echo "benchmark,pooler,clients,duration,tps,avg_latency_ms,stddev_ms,transactions" > "$output_file"
        echo "pgbench_connection,$POOLER,$clients,$DURATION,${tps:-0},${avg_latency:-0},${stddev:-0},${transactions:-0}" >> "$output_file"
    fi

    log_success "pgbench ($clients clients): TPS=${tps:-0}, Latency=${avg_latency:-0}ms"
}

# Run pgbench simple query test (no -C, persistent connections)
run_pgbench_persistent_test() {
    local clients=$1
    local output_file="${RESULTS_PATH}/pgbench_persistent_${clients}clients.${FORMAT}"

    log_info "Running pgbench persistent connection test: $clients clients"

    local conn_opts="-h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE"

    local result
    result=$(pgbench $conn_opts \
        -c "$clients" \
        -j "$(( clients < 16 ? clients : 16 ))" \
        -T "$DURATION" \
        -S \
        --progress=5 \
        2>&1) || true

    local tps=$(echo "$result" | grep -oP 'tps = \K[0-9.]+' | tail -1)
    local avg_latency=$(echo "$result" | grep -oP 'latency average = \K[0-9.]+' | head -1)
    local stddev=$(echo "$result" | grep -oP 'latency stddev = \K[0-9.]+' | head -1)
    local transactions=$(echo "$result" | grep -oP 'number of transactions actually processed: \K[0-9]+' | head -1)

    if [[ "$FORMAT" == "json" ]]; then
        cat > "$output_file" << EOF
{
  "benchmark": "pgbench_persistent",
  "pooler": "$POOLER",
  "clients": $clients,
  "duration_secs": $DURATION,
  "results": {
    "tps": ${tps:-0},
    "avg_latency_ms": ${avg_latency:-0},
    "stddev_ms": ${stddev:-0},
    "transactions": ${transactions:-0}
  },
  "timestamp": "$(date -Iseconds)"
}
EOF
    else
        echo "benchmark,pooler,clients,duration,tps,avg_latency_ms,stddev_ms,transactions" > "$output_file"
        echo "pgbench_persistent,$POOLER,$clients,$DURATION,${tps:-0},${avg_latency:-0},${stddev:-0},${transactions:-0}" >> "$output_file"
    fi

    log_success "pgbench persistent ($clients clients): TPS=${tps:-0}, Latency=${avg_latency:-0}ms"
}

# Run custom C benchmark
run_custom_benchmark() {
    local bench_type=$1
    log_info "Running custom $bench_type benchmark..."

    local custom_opts="--host=$PGHOST --port=$PGPORT --user=$PGUSER --database=$PGDATABASE"
    custom_opts="$custom_opts --pooler=$POOLER --bench=$bench_type"
    custom_opts="$custom_opts --duration=$DURATION --warmup=$WARMUP"
    custom_opts="$custom_opts --format=$FORMAT --output=$RESULTS_PATH"

    if [[ -n "$PGPASSWORD" ]]; then
        custom_opts="$custom_opts --password=$PGPASSWORD"
    fi

    if [[ $USE_TLS -eq 1 ]]; then
        custom_opts="$custom_opts --tls"
    fi

    if [[ $VERBOSE -eq 1 ]]; then
        custom_opts="$custom_opts --verbose"
    fi

    "$SCRIPT_DIR/connection_bench" $custom_opts
}

# Aggregate results into summary
generate_summary() {
    local summary_file="${RESULTS_PATH}/summary.${FORMAT}"

    log_info "Generating summary..."

    if [[ "$FORMAT" == "json" ]]; then
        # Collect all JSON results
        echo '{' > "$summary_file"
        echo '  "benchmark_suite": "connection_pooler",' >> "$summary_file"
        echo "  \"pooler\": \"$POOLER\"," >> "$summary_file"
        echo "  \"host\": \"$PGHOST\"," >> "$summary_file"
        echo "  \"port\": $PGPORT," >> "$summary_file"
        echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$summary_file"
        echo "  \"config\": {" >> "$summary_file"
        echo "    \"duration_secs\": $DURATION," >> "$summary_file"
        echo "    \"warmup_secs\": $WARMUP," >> "$summary_file"
        echo "    \"clients_tested\": [$(echo $CLIENTS_LIST | tr ' ' ',')]," >> "$summary_file"
        echo "    \"tls_enabled\": $([ $USE_TLS -eq 1 ] && echo 'true' || echo 'false')" >> "$summary_file"
        echo "  }," >> "$summary_file"
        echo '  "results": [' >> "$summary_file"

        local first=1
        for f in "$RESULTS_PATH"/*.json; do
            [[ "$f" == "$summary_file" ]] && continue
            [[ ! -f "$f" ]] && continue

            if [[ $first -eq 0 ]]; then
                echo ',' >> "$summary_file"
            fi
            first=0

            # Include file content
            cat "$f" >> "$summary_file"
        done

        echo '' >> "$summary_file"
        echo '  ]' >> "$summary_file"
        echo '}' >> "$summary_file"

    else
        # CSV summary
        echo "benchmark,pooler,clients,tps,avg_latency_ms,p50_us,p95_us,p99_us" > "$summary_file"

        for f in "$RESULTS_PATH"/*.csv; do
            [[ "$f" == "$summary_file" ]] && continue
            [[ ! -f "$f" ]] && continue
            tail -n +2 "$f" >> "$summary_file"
        done
    fi

    log_success "Summary saved to: $summary_file"
}

# Print final report
print_report() {
    log_header "BENCHMARK RESULTS SUMMARY"

    echo "Pooler:       $POOLER"
    echo "Host:         $PGHOST:$PGPORT"
    echo "Duration:     $DURATION seconds"
    echo "Client Counts: $CLIENTS_LIST"
    echo ""

    if [[ -f "${RESULTS_PATH}/summary.json" ]]; then
        echo "Results Summary:"
        echo "----------------"

        # Extract key metrics using jq if available
        if command -v jq &> /dev/null; then
            jq -r '
                .results[]? |
                select(.benchmark? or .name?) |
                "\(.benchmark // .name): TPS=\(.results?.tps // .throughput?.ops_per_sec // "N/A"), Latency=\(.results?.avg_latency_ms // (.latency?.avg_us? / 1000) // "N/A")ms"
            ' "${RESULTS_PATH}/summary.json" 2>/dev/null || echo "  (Use jq for detailed results)"
        fi
    fi

    echo ""
    echo "Full results saved to: $RESULTS_PATH"
    echo ""
}

main() {
    log_header "Connection Pooler Benchmark Suite"

    parse_args "$@"

    echo "Configuration:"
    echo "  Pooler:     $POOLER"
    echo "  Host:       $PGHOST:$PGPORT"
    echo "  Database:   $PGDATABASE"
    echo "  Duration:   $DURATION seconds"
    echo "  Warmup:     $WARMUP seconds"
    echo "  Clients:    $CLIENTS_LIST"
    echo "  TLS:        $([ $USE_TLS -eq 1 ] && echo 'enabled' || echo 'disabled')"
    echo ""

    check_prerequisites
    setup_output_directory

    START_TIME=$(date +%s)

    # Initialize pgbench tables
    if [[ $RUN_PGBENCH -eq 1 ]]; then
        init_pgbench
    fi

    # Run pgbench tests
    if [[ $RUN_PGBENCH -eq 1 ]]; then
        log_header "pgbench Connection Tests"

        for clients in $CLIENTS_LIST; do
            # Connection per transaction test
            run_pgbench_connection_test "$clients"
            sleep 2

            # Persistent connection test
            run_pgbench_persistent_test "$clients"
            sleep 2
        done
    fi

    # Run custom C benchmarks
    if [[ $RUN_CUSTOM -eq 1 ]] && [[ -x "$SCRIPT_DIR/connection_bench" ]]; then
        log_header "Custom Connection Benchmarks"

        # Run saturation sweep (tests multiple client counts)
        run_custom_benchmark "saturation"
    fi

    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    generate_summary
    print_report

    log_success "Total benchmark time: ${ELAPSED}s"
}

main "$@"
