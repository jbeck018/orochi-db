#!/bin/bash
#
# Orochi DB - Sharding Benchmark Runner
#
# Tests sharding performance with varying configurations:
# - Shard counts: 2, 4, 8, 16, 32, 64
# - Client counts: 1, 4, 8, 16, 32, 64
# - Measures scalability across node additions
# - Triggers rebalancing and measures impact
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$SCRIPT_DIR"
RESULTS_DIR="${RESULTS_DIR:-$BENCH_DIR/../results/sharding}"
BENCHMARK="$BENCH_DIR/sharding_bench"

# Database connection
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

# Test parameters
SHARD_COUNTS="${SHARD_COUNTS:-2,4,8,16,32,64}"
CLIENT_COUNTS="${CLIENT_COUNTS:-1,4,8,16,32,64}"
RECORDS="${RECORDS:-10000}"
DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-5}"
VERBOSE="${VERBOSE:-0}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print functions
print_header() {
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}========================================${NC}\n"
}

print_step() {
    echo -e "${GREEN}>>> $1${NC}"
}

print_warn() {
    echo -e "${YELLOW}WARNING: $1${NC}"
}

print_error() {
    echo -e "${RED}ERROR: $1${NC}"
}

# Check prerequisites
check_prerequisites() {
    print_step "Checking prerequisites..."

    # Check for benchmark executable
    if [ ! -x "$BENCHMARK" ]; then
        print_warn "Benchmark not built. Building now..."
        make -C "$BENCH_DIR" || {
            print_error "Failed to build benchmark"
            exit 1
        }
    fi

    # Check database connection
    if ! PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" > /dev/null 2>&1; then
        print_error "Cannot connect to database $PGDATABASE on $PGHOST:$PGPORT"
        exit 1
    fi

    # Check for Orochi extension
    local ext_check=$(PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT COUNT(*) FROM pg_extension WHERE extname = 'orochi'")
    if [ "$ext_check" -eq "0" ]; then
        print_warn "Orochi extension not installed. Installing..."
        PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS orochi"
    fi

    # Create results directory
    mkdir -p "$RESULTS_DIR"

    echo "Prerequisites check passed"
}

# Run full benchmark suite
run_full_benchmark() {
    print_header "Running Full Sharding Benchmark Suite"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/sharding_full_$timestamp.json"

    local verbose_flag=""
    [ "$VERBOSE" -eq 1 ] && verbose_flag="--verbose"

    $BENCHMARK \
        --mode=all \
        --records="$RECORDS" \
        --shards="$SHARD_COUNTS" \
        --clients="$CLIENT_COUNTS" \
        --duration="$DURATION" \
        --warmup="$WARMUP" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file" \
        $verbose_flag

    echo ""
    print_step "Results saved to: $output_file"
}

# Run distribution uniformity test
run_distribution_test() {
    print_header "Running Distribution Uniformity Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/distribution_$timestamp.json"

    $BENCHMARK \
        --mode=distribution \
        --records="$RECORDS" \
        --shards="$SHARD_COUNTS" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Run hash vs range comparison
run_hash_vs_range_test() {
    print_header "Running Hash vs Range Distribution Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/hash_vs_range_$timestamp.json"

    $BENCHMARK \
        --mode=hash-vs-range \
        --records="$RECORDS" \
        --shards="8" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Run routing efficiency test
run_routing_test() {
    print_header "Running Query Routing Efficiency Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/routing_$timestamp.json"

    $BENCHMARK \
        --mode=routing \
        --records="$RECORDS" \
        --shards="8" \
        --clients="$CLIENT_COUNTS" \
        --duration="$DURATION" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Run rebalancing overhead test
run_rebalance_test() {
    print_header "Running Rebalancing Overhead Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/rebalance_$timestamp.json"

    $BENCHMARK \
        --mode=rebalance \
        --records="$RECORDS" \
        --shards="8" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Run scalability test
run_scalability_test() {
    print_header "Running Scalability Factor Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/scalability_$timestamp.json"

    $BENCHMARK \
        --mode=scalability \
        --records="$RECORDS" \
        --shards="$SHARD_COUNTS" \
        --clients="$CLIENT_COUNTS" \
        --duration="$DURATION" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Quick test (reduced configurations)
run_quick_test() {
    print_header "Running Quick Sharding Test"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="$RESULTS_DIR/sharding_quick_$timestamp.json"

    $BENCHMARK \
        --mode=all \
        --records=1000 \
        --shards="2,4,8" \
        --clients="1,4" \
        --duration=10 \
        --warmup=2 \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$output_file"

    echo ""
    print_step "Results saved to: $output_file"
}

# Generate report from results
generate_report() {
    print_header "Generating Sharding Benchmark Report"

    local latest_result=$(ls -t "$RESULTS_DIR"/sharding_*.json 2>/dev/null | head -1)

    if [ -z "$latest_result" ]; then
        print_error "No results found in $RESULTS_DIR"
        exit 1
    fi

    echo "Analyzing: $latest_result"
    echo ""

    # Parse JSON and generate summary
    if command -v jq &> /dev/null; then
        echo "=== Distribution Metrics ==="
        jq -r '.results[] | select(.name | contains("distribution")) |
            "  \(.name): RSD=\(.compression_ratio | tostring)%, Success=\(.success)"' "$latest_result" 2>/dev/null || true

        echo ""
        echo "=== Scalability Factors ==="
        jq -r '.results[] | select(.name | contains("scalability")) |
            "  \(.name): Scale=\(.compression_ratio | tostring), Throughput=\(.throughput.ops_per_sec | tostring) ops/sec"' "$latest_result" 2>/dev/null || true

        echo ""
        echo "=== Routing Efficiency ==="
        jq -r '.results[] | select(.name | contains("routing")) |
            "  \(.name): Cross-shard=\(.compression_ratio | tostring)%, Throughput=\(.throughput.ops_per_sec | tostring) ops/sec"' "$latest_result" 2>/dev/null || true

    else
        print_warn "jq not installed. Showing raw results."
        cat "$latest_result"
    fi
}

# Cleanup test data
cleanup() {
    print_header "Cleaning Up Test Data"

    PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" << 'EOF'
DROP TABLE IF EXISTS shard_bench_orders CASCADE;
DROP TABLE IF EXISTS shard_bench_customers CASCADE;
DROP TABLE IF EXISTS shard_bench_events CASCADE;
EOF

    echo "Cleanup complete"
}

# Show usage
usage() {
    cat << EOF
Orochi DB Sharding Benchmark Runner

Usage: $(basename "$0") [command] [options]

Commands:
  all           Run full benchmark suite (default)
  distribution  Run distribution uniformity test only
  hash-range    Run hash vs range comparison
  routing       Run query routing efficiency test
  rebalance     Run rebalancing overhead test
  scalability   Run scalability factor test
  quick         Run quick test with reduced configs
  report        Generate report from latest results
  cleanup       Remove test tables
  help          Show this help message

Options (via environment variables):
  PGHOST        Database host [default: localhost]
  PGPORT        Database port [default: 5432]
  PGUSER        Database user [default: postgres]
  PGDATABASE    Database name [default: orochi_bench]
  PGPASSWORD    Database password
  RECORDS       Number of base records [default: 10000]
  DURATION      Test duration per config [default: 30]
  WARMUP        Warmup duration [default: 5]
  SHARD_COUNTS  Comma-separated shard counts [default: 2,4,8,16,32,64]
  CLIENT_COUNTS Comma-separated client counts [default: 1,4,8,16,32,64]
  RESULTS_DIR   Results output directory
  VERBOSE       Enable verbose output (1/0) [default: 0]

Examples:
  # Run full benchmark
  $(basename "$0") all

  # Run quick test
  $(basename "$0") quick

  # Run with custom settings
  RECORDS=50000 DURATION=60 $(basename "$0") scalability

  # Run distribution test with specific shards
  SHARD_COUNTS="4,8,16" $(basename "$0") distribution

  # Generate report
  $(basename "$0") report

Metrics:
  - RSD (Skew Score): Target < 10% for acceptable distribution
  - Scalability Factor: Target > 0.7 (70% linear scaling)
  - Rebalance Performance Drop: Target < 50%
EOF
}

# Main entry point
main() {
    local command="${1:-all}"

    case "$command" in
        all)
            check_prerequisites
            run_full_benchmark
            ;;
        distribution)
            check_prerequisites
            run_distribution_test
            ;;
        hash-range)
            check_prerequisites
            run_hash_vs_range_test
            ;;
        routing)
            check_prerequisites
            run_routing_test
            ;;
        rebalance)
            check_prerequisites
            run_rebalance_test
            ;;
        scalability)
            check_prerequisites
            run_scalability_test
            ;;
        quick)
            check_prerequisites
            run_quick_test
            ;;
        report)
            generate_report
            ;;
        cleanup)
            cleanup
            ;;
        help|--help|-h)
            usage
            ;;
        *)
            print_error "Unknown command: $command"
            usage
            exit 1
            ;;
    esac
}

main "$@"
