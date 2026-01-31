#!/bin/bash
#
# Orochi DB Connection Pooler Comparison Suite
#
# Runs identical benchmark suite against multiple poolers and generates
# a comprehensive comparison report.
#
# Supported poolers:
#   - Direct PostgreSQL (baseline)
#   - PgBouncer (C, lightweight, ~44K TPS peak)
#   - PgCat (Rust/Tokio, ~59K TPS peak)
#   - PgDog (PgBouncer fork, ~10% improvement)
#
# Usage: ./pooler_compare.sh [OPTIONS]
#

set -e

# Default configuration
PGHOST="${PGHOST:-localhost}"
PGUSER="${PGUSER:-postgres}"
PGPASSWORD="${PGPASSWORD:-}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
DURATION=30
WARMUP=5
CLIENTS_LIST="10 50 100 200 500"
FORMAT="json"
OUTPUT_DIR="results/comparison"
VERBOSE=0

# Pooler configurations (port mapping)
declare -A POOLER_PORTS=(
    ["direct"]=5432
    ["pgbouncer"]=6432
    ["pgcat"]=6433
    ["pgdog"]=6434
)

# Poolers to test (can be overridden)
POOLERS_TO_TEST="direct pgbouncer pgcat pgdog"

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
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

log_pooler() {
    echo -e "${MAGENTA}[${1^^}]${NC} $2"
}

usage() {
    cat << EOF
Orochi DB Connection Pooler Comparison Suite

This script runs identical benchmarks against multiple connection poolers
and generates a comprehensive comparison report.

Usage: $(basename "$0") [OPTIONS]

Options:
  --host=HOST         PostgreSQL host [default: localhost]
  --user=USER         Database user [default: postgres]
  --password=PASS     Database password
  --database=DB       Database name [default: orochi_bench]
  --duration=N        Test duration per benchmark [default: 30]
  --warmup=N          Warmup duration [default: 5]
  --clients=LIST      Client counts to test [default: 10,50,100,200,500]
  --poolers=LIST      Poolers to test [default: direct,pgbouncer,pgcat,pgdog]
  --format=FMT        Output format: json|csv [default: json]
  --output=DIR        Output directory [default: results/comparison]
  --port-direct=N     Direct PostgreSQL port [default: 5432]
  --port-pgbouncer=N  PgBouncer port [default: 6432]
  --port-pgcat=N      PgCat port [default: 6433]
  --port-pgdog=N      PgDog port [default: 6434]
  --skip-unavailable  Skip poolers that aren't reachable
  --verbose           Enable verbose output
  --help              Show this help

Expected Performance (reference):
  - Direct:    Baseline (limited by max_connections)
  - PgBouncer: ~44,000 TPS peak, best for <50 connections
  - PgCat:     ~59,000 TPS peak, Rust/Tokio architecture
  - PgDog:     ~10% improvement over PgBouncer

Examples:
  $(basename "$0")
  $(basename "$0") --poolers=direct,pgbouncer --clients=10,100,500
  $(basename "$0") --duration=60 --skip-unavailable

EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --host=*)
                PGHOST="${1#*=}"
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
            --poolers=*)
                POOLERS_TO_TEST="${1#*=}"
                POOLERS_TO_TEST="${POOLERS_TO_TEST//,/ }"
                ;;
            --format=*)
                FORMAT="${1#*=}"
                ;;
            --output=*)
                OUTPUT_DIR="${1#*=}"
                ;;
            --port-direct=*)
                POOLER_PORTS["direct"]="${1#*=}"
                ;;
            --port-pgbouncer=*)
                POOLER_PORTS["pgbouncer"]="${1#*=}"
                ;;
            --port-pgcat=*)
                POOLER_PORTS["pgcat"]="${1#*=}"
                ;;
            --port-pgdog=*)
                POOLER_PORTS["pgdog"]="${1#*=}"
                ;;
            --skip-unavailable)
                SKIP_UNAVAILABLE=1
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

# Check if a pooler is available
check_pooler_available() {
    local pooler=$1
    local port=${POOLER_PORTS[$pooler]}

    log_info "Checking $pooler on port $port..."

    if [[ -n "$PGPASSWORD" ]]; then
        export PGPASSWORD
    fi

    if psql -h "$PGHOST" -p "$port" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &> /dev/null; then
        log_success "$pooler is available"
        return 0
    else
        log_warn "$pooler is not available on $PGHOST:$port"
        return 1
    fi
}

# Discover available poolers
discover_poolers() {
    log_header "Discovering Available Poolers"

    AVAILABLE_POOLERS=""

    for pooler in $POOLERS_TO_TEST; do
        if check_pooler_available "$pooler"; then
            AVAILABLE_POOLERS="$AVAILABLE_POOLERS $pooler"
        elif [[ "${SKIP_UNAVAILABLE:-0}" -eq 0 ]]; then
            log_error "Required pooler $pooler is not available. Use --skip-unavailable to continue."
            exit 1
        fi
    done

    AVAILABLE_POOLERS=$(echo "$AVAILABLE_POOLERS" | xargs)  # Trim whitespace

    if [[ -z "$AVAILABLE_POOLERS" ]]; then
        log_error "No poolers available for testing!"
        exit 1
    fi

    log_success "Available poolers: $AVAILABLE_POOLERS"
}

setup_output_directory() {
    RESULTS_PATH="${OUTPUT_DIR}/${TIMESTAMP}"
    mkdir -p "$RESULTS_PATH"

    for pooler in $AVAILABLE_POOLERS; do
        mkdir -p "$RESULTS_PATH/$pooler"
    done

    log_info "Results will be saved to: $RESULTS_PATH"
}

# Run benchmark for a single pooler
run_pooler_benchmark() {
    local pooler=$1
    local port=${POOLER_PORTS[$pooler]}

    log_header "Benchmarking: ${pooler^^}"

    local pooler_output="$RESULTS_PATH/$pooler"

    # Run the connection benchmark script
    "$SCRIPT_DIR/run_connection.sh" \
        --pooler="$pooler" \
        --host="$PGHOST" \
        --port="$port" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --duration="$DURATION" \
        --warmup="$WARMUP" \
        --clients="${CLIENTS_LIST// /,}" \
        --format="$FORMAT" \
        --output="$pooler_output" \
        $([ -n "$PGPASSWORD" ] && echo "--password=$PGPASSWORD") \
        $([ "$VERBOSE" -eq 1 ] && echo "--verbose") \
        2>&1 | while read -r line; do
            log_pooler "$pooler" "$line"
        done

    log_success "Completed benchmarks for $pooler"
}

# Extract TPS from result files
extract_tps() {
    local file=$1
    local clients=$2

    if [[ -f "$file" ]] && command -v jq &> /dev/null; then
        jq -r ".results?.tps // .throughput?.ops_per_sec // 0" "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

# Generate comparison report
generate_comparison_report() {
    log_header "Generating Comparison Report"

    local report_file="$RESULTS_PATH/comparison_report.md"
    local json_file="$RESULTS_PATH/comparison_results.json"

    # Markdown report
    cat > "$report_file" << EOF
# Connection Pooler Comparison Report

**Generated:** $(date)
**Host:** $PGHOST
**Database:** $PGDATABASE
**Test Duration:** ${DURATION}s per test
**Client Counts:** $CLIENTS_LIST

## Summary

| Pooler | 10 Clients | 50 Clients | 100 Clients | 200 Clients | 500 Clients | Peak TPS |
|--------|-----------|-----------|------------|------------|------------|----------|
EOF

    # Collect results for each pooler
    for pooler in $AVAILABLE_POOLERS; do
        local row="| $pooler |"
        local peak_tps=0

        for clients in $CLIENTS_LIST; do
            # Try to find result file
            local tps=0
            local result_file="$RESULTS_PATH/$pooler/pgbench_persistent_${clients}clients.json"

            if [[ -f "$result_file" ]] && command -v jq &> /dev/null; then
                tps=$(jq -r '.results.tps // 0' "$result_file" 2>/dev/null || echo "0")
            fi

            # Format TPS with commas
            if [[ "$tps" != "0" ]] && [[ "$tps" != "null" ]]; then
                tps_formatted=$(printf "%'.0f" "$tps" 2>/dev/null || echo "$tps")
                row="$row $tps_formatted |"

                # Track peak
                if (( $(echo "$tps > $peak_tps" | bc -l) )); then
                    peak_tps=$tps
                fi
            else
                row="$row - |"
            fi
        done

        # Add peak TPS
        if (( $(echo "$peak_tps > 0" | bc -l) )); then
            peak_formatted=$(printf "%'.0f" "$peak_tps" 2>/dev/null || echo "$peak_tps")
            row="$row $peak_formatted |"
        else
            row="$row - |"
        fi

        echo "$row" >> "$report_file"
    done

    # Add analysis section
    cat >> "$report_file" << 'EOF'

## Performance Analysis

### Expected Performance Characteristics

Based on industry benchmarks:

- **Direct PostgreSQL**: Baseline performance, limited by `max_connections`
- **PgBouncer**: ~44,000 TPS peak, excellent for workloads with <50 concurrent connections
- **PgCat**: ~59,000 TPS peak, Rust/Tokio async architecture, better scaling
- **PgDog**: ~10% improvement over PgBouncer, enhanced feature set

### Recommendations

1. **Low concurrency (<50 connections)**: PgBouncer provides lowest latency
2. **High concurrency (>200 connections)**: PgCat handles load better
3. **Mixed workloads**: Consider PgCat for its connection balancing features
4. **Maximum stability**: PgBouncer has the longest track record

## Latency Comparison

| Pooler | p50 (us) | p95 (us) | p99 (us) |
|--------|----------|----------|----------|
EOF

    for pooler in $AVAILABLE_POOLERS; do
        local p50="N/A"
        local p95="N/A"
        local p99="N/A"

        # Try to extract from custom benchmark results
        for f in "$RESULTS_PATH/$pooler"/*.json; do
            [[ ! -f "$f" ]] && continue
            if command -v jq &> /dev/null; then
                local latency_data=$(jq -r '.latency // empty' "$f" 2>/dev/null)
                if [[ -n "$latency_data" ]]; then
                    p50=$(jq -r '.latency.p50_us // "N/A"' "$f" 2>/dev/null)
                    p95=$(jq -r '.latency.p95_us // "N/A"' "$f" 2>/dev/null)
                    p99=$(jq -r '.latency.p99_us // "N/A"' "$f" 2>/dev/null)
                    break
                fi
            fi
        done

        echo "| $pooler | $p50 | $p95 | $p99 |" >> "$report_file"
    done

    cat >> "$report_file" << 'EOF'

## Test Environment

- **Benchmark Tool**: Orochi DB Connection Benchmark Suite
- **pgbench**: PostgreSQL built-in benchmark with -C (connection per transaction)
- **Custom Benchmark**: C11 multi-threaded connection stress test

## Files Generated

EOF

    # List generated files
    find "$RESULTS_PATH" -name "*.json" -o -name "*.csv" | sort | while read -r f; do
        echo "- \`$(basename "$f")\`" >> "$report_file"
    done

    log_success "Comparison report saved to: $report_file"

    # Generate JSON comparison
    if command -v jq &> /dev/null; then
        echo '{' > "$json_file"
        echo '  "comparison": {' >> "$json_file"
        echo "    \"timestamp\": \"$(date -Iseconds)\"," >> "$json_file"
        echo "    \"host\": \"$PGHOST\"," >> "$json_file"
        echo "    \"database\": \"$PGDATABASE\"," >> "$json_file"
        echo "    \"duration_secs\": $DURATION," >> "$json_file"
        echo "    \"clients_tested\": [$(echo $CLIENTS_LIST | tr ' ' ',')]," >> "$json_file"
        echo '    "poolers": {' >> "$json_file"

        local first_pooler=1
        for pooler in $AVAILABLE_POOLERS; do
            if [[ $first_pooler -eq 0 ]]; then
                echo ',' >> "$json_file"
            fi
            first_pooler=0

            echo "      \"$pooler\": {" >> "$json_file"
            echo "        \"port\": ${POOLER_PORTS[$pooler]}," >> "$json_file"
            echo '        "results": {' >> "$json_file"

            local first_client=1
            for clients in $CLIENTS_LIST; do
                if [[ $first_client -eq 0 ]]; then
                    echo ',' >> "$json_file"
                fi
                first_client=0

                local tps=0
                local result_file="$RESULTS_PATH/$pooler/pgbench_persistent_${clients}clients.json"
                if [[ -f "$result_file" ]]; then
                    tps=$(jq -r '.results.tps // 0' "$result_file" 2>/dev/null || echo "0")
                fi

                echo "          \"${clients}_clients\": { \"tps\": $tps }" >> "$json_file"
            done

            echo '' >> "$json_file"
            echo '        }' >> "$json_file"
            echo -n '      }' >> "$json_file"
        done

        echo '' >> "$json_file"
        echo '    }' >> "$json_file"
        echo '  }' >> "$json_file"
        echo '}' >> "$json_file"

        log_success "JSON comparison saved to: $json_file"
    fi
}

# Print final summary to terminal
print_summary() {
    log_header "COMPARISON COMPLETE"

    echo "Poolers Tested: $AVAILABLE_POOLERS"
    echo "Client Counts:  $CLIENTS_LIST"
    echo "Test Duration:  ${DURATION}s per test"
    echo ""

    if [[ -f "$RESULTS_PATH/comparison_report.md" ]]; then
        echo "Quick Summary (TPS at 100 clients):"
        echo "-----------------------------------"

        for pooler in $AVAILABLE_POOLERS; do
            local tps="N/A"
            local result_file="$RESULTS_PATH/$pooler/pgbench_persistent_100clients.json"

            if [[ -f "$result_file" ]] && command -v jq &> /dev/null; then
                tps=$(jq -r '.results.tps // "N/A"' "$result_file" 2>/dev/null)
                if [[ "$tps" != "N/A" ]] && [[ "$tps" != "null" ]]; then
                    tps=$(printf "%'.0f" "$tps" 2>/dev/null || echo "$tps")
                fi
            fi

            printf "  %-12s %s TPS\n" "${pooler}:" "$tps"
        done
    fi

    echo ""
    echo "Full results: $RESULTS_PATH"
    echo "Report:       $RESULTS_PATH/comparison_report.md"
    echo ""
}

main() {
    log_header "Connection Pooler Comparison Suite"

    parse_args "$@"

    echo "Configuration:"
    echo "  Host:       $PGHOST"
    echo "  Database:   $PGDATABASE"
    echo "  Duration:   ${DURATION}s"
    echo "  Clients:    $CLIENTS_LIST"
    echo "  Poolers:    $POOLERS_TO_TEST"
    echo ""

    # Check for required tools
    if ! command -v bc &> /dev/null; then
        log_warn "bc not found. Some calculations may not work."
    fi

    discover_poolers
    setup_output_directory

    START_TIME=$(date +%s)

    # Run benchmarks for each pooler
    for pooler in $AVAILABLE_POOLERS; do
        run_pooler_benchmark "$pooler"

        # Brief pause between poolers
        sleep 5
    done

    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    generate_comparison_report
    print_summary

    log_success "Total comparison time: ${ELAPSED}s ($(( ELAPSED / 60 ))m $(( ELAPSED % 60 ))s)"
}

main "$@"
