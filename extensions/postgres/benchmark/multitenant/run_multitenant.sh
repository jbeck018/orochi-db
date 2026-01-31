#!/bin/bash
#
# Multi-Tenant Isolation Benchmark Runner
#
# This script orchestrates the multi-tenant benchmark suite:
# 1. Creates N tenants (default: 10)
# 2. Runs baseline workload per tenant in isolation
# 3. Runs concurrent multi-tenant workload
# 4. Calculates relative performance and fairness metrics
#
# Usage: ./run_multitenant.sh [options]
#

set -e

# Default configuration
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGPASSWORD="${PGPASSWORD:-}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

NUM_TENANTS="${NUM_TENANTS:-10}"
ROWS_PER_TENANT="${ROWS_PER_TENANT:-10000}"
DURATION="${DURATION:-60}"
WARMUP="${WARMUP:-10}"
SLA_LATENCY="${SLA_LATENCY:-100}"
ISOLATION="${ISOLATION:-shared_table}"
WORKLOAD="${WORKLOAD:-mixed}"
OUTPUT_DIR="${OUTPUT_DIR:-../results/multitenant}"
OUTPUT_FORMAT="${OUTPUT_FORMAT:-json}"
VERBOSE="${VERBOSE:-0}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCHMARK_BIN="${SCRIPT_DIR}/tenant_bench"

usage() {
    cat << EOF
Multi-Tenant Isolation Benchmark Runner

Usage: $0 [options]

Options:
  -h, --host HOST           PostgreSQL host [${PGHOST}]
  -p, --port PORT           PostgreSQL port [${PGPORT}]
  -U, --user USER           PostgreSQL user [${PGUSER}]
  -d, --database DB         Database name [${PGDATABASE}]
  -n, --tenants N           Number of tenants [${NUM_TENANTS}]
  -r, --rows N              Rows per tenant [${ROWS_PER_TENANT}]
  -D, --duration SECS       Benchmark duration [${DURATION}]
  -w, --warmup SECS         Warmup duration [${WARMUP}]
  -s, --sla MS              SLA latency threshold [${SLA_LATENCY}]
  -i, --isolation MODEL     Isolation: shared_table, shared_schema [${ISOLATION}]
  -W, --workload TYPE       Workload: read, write, mixed, analytical [${WORKLOAD}]
  -o, --output DIR          Output directory [${OUTPUT_DIR}]
  -f, --format FMT          Output format: text, json, csv [${OUTPUT_FORMAT}]
  -v, --verbose             Enable verbose output
      --quick               Quick test (5 tenants, 30s duration)
      --stress              Stress test (50 tenants, 300s duration)
      --all-workloads       Run all workload types
      --all-isolations      Run all isolation models
      --help                Show this help

Examples:
  $0                                    # Run with defaults
  $0 -n 20 -D 120                       # 20 tenants, 2 minute test
  $0 --quick                            # Quick validation test
  $0 --stress                           # Extended stress test
  $0 --all-workloads                    # Compare workload types
  $0 -i shared_schema -W analytical     # Schema isolation with analytics

Environment Variables:
  PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE - Database connection
  NUM_TENANTS, ROWS_PER_TENANT, DURATION, etc. - Benchmark parameters

EOF
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check for benchmark binary
    if [[ ! -x "${BENCHMARK_BIN}" ]]; then
        log_error "Benchmark binary not found: ${BENCHMARK_BIN}"
        log_info "Building benchmark..."
        (cd "${SCRIPT_DIR}" && make)
    fi

    # Check PostgreSQL connection
    if ! psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -d postgres -c "SELECT 1" > /dev/null 2>&1; then
        log_error "Cannot connect to PostgreSQL at ${PGHOST}:${PGPORT}"
        exit 1
    fi

    # Check if database exists, create if not
    if ! psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -lqt | cut -d \| -f 1 | grep -qw "${PGDATABASE}"; then
        log_info "Creating database ${PGDATABASE}..."
        psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -c "CREATE DATABASE ${PGDATABASE};"
    fi

    log_success "Prerequisites check passed"
}

setup_output_dir() {
    if [[ ! -d "${OUTPUT_DIR}" ]]; then
        mkdir -p "${OUTPUT_DIR}"
        log_info "Created output directory: ${OUTPUT_DIR}"
    fi
}

run_benchmark() {
    local isolation_model="$1"
    local workload_type="$2"
    local output_suffix="$3"

    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_file="${OUTPUT_DIR}/multitenant_${isolation_model}_${workload_type}_${timestamp}.${OUTPUT_FORMAT}"

    log_info "Running benchmark:"
    log_info "  Isolation Model: ${isolation_model}"
    log_info "  Workload Type:   ${workload_type}"
    log_info "  Tenants:         ${NUM_TENANTS}"
    log_info "  Rows/Tenant:     ${ROWS_PER_TENANT}"
    log_info "  Duration:        ${DURATION}s"
    log_info "  SLA Threshold:   ${SLA_LATENCY}ms"

    local verbose_flag=""
    if [[ "${VERBOSE}" == "1" ]]; then
        verbose_flag="-v"
    fi

    "${BENCHMARK_BIN}" \
        -h "${PGHOST}" \
        -p "${PGPORT}" \
        -U "${PGUSER}" \
        -d "${PGDATABASE}" \
        -n "${NUM_TENANTS}" \
        -r "${ROWS_PER_TENANT}" \
        -D "${DURATION}" \
        -w "${WARMUP}" \
        -s "${SLA_LATENCY}" \
        -i "${isolation_model}" \
        -W "${workload_type}" \
        -o "${output_file}" \
        -f "${OUTPUT_FORMAT}" \
        ${verbose_flag}

    log_success "Benchmark complete. Results saved to: ${output_file}"
}

run_all_workloads() {
    log_info "Running all workload types..."

    for workload in read write mixed analytical; do
        log_info "=========================================="
        run_benchmark "${ISOLATION}" "${workload}" "${workload}"
        log_info "=========================================="
        echo ""
    done
}

run_all_isolations() {
    log_info "Running all isolation models..."

    for isolation in shared_table shared_schema; do
        log_info "=========================================="
        run_benchmark "${isolation}" "${WORKLOAD}" "${isolation}"
        log_info "=========================================="
        echo ""
    done
}

generate_comparison_report() {
    log_info "Generating comparison report..."

    local report_file="${OUTPUT_DIR}/comparison_report_$(date +%Y%m%d_%H%M%S).txt"

    echo "Multi-Tenant Benchmark Comparison Report" > "${report_file}"
    echo "Generated: $(date)" >> "${report_file}"
    echo "=========================================" >> "${report_file}"
    echo "" >> "${report_file}"

    # Find all JSON result files
    for result_file in "${OUTPUT_DIR}"/*.json; do
        if [[ -f "${result_file}" ]]; then
            echo "File: $(basename "${result_file}")" >> "${report_file}"

            # Extract key metrics using jq if available
            if command -v jq &> /dev/null; then
                echo "  Jain's Fairness Index: $(jq -r '.aggregate_metrics.jains_fairness_index' "${result_file}")" >> "${report_file}"
                echo "  Avg Relative Perf:     $(jq -r '.aggregate_metrics.avg_relative_performance' "${result_file}")" >> "${report_file}"
                echo "  SLA Compliance:        $(jq -r '.aggregate_metrics.sla_compliance_rate' "${result_file}")" >> "${report_file}"
                echo "  Noisy Neighbors:       $(jq -r '.aggregate_metrics.noisy_neighbor_count' "${result_file}")" >> "${report_file}"
            fi

            echo "" >> "${report_file}"
        fi
    done

    log_success "Comparison report saved to: ${report_file}"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)
            PGHOST="$2"
            shift 2
            ;;
        -p|--port)
            PGPORT="$2"
            shift 2
            ;;
        -U|--user)
            PGUSER="$2"
            shift 2
            ;;
        -d|--database)
            PGDATABASE="$2"
            shift 2
            ;;
        -n|--tenants)
            NUM_TENANTS="$2"
            shift 2
            ;;
        -r|--rows)
            ROWS_PER_TENANT="$2"
            shift 2
            ;;
        -D|--duration)
            DURATION="$2"
            shift 2
            ;;
        -w|--warmup)
            WARMUP="$2"
            shift 2
            ;;
        -s|--sla)
            SLA_LATENCY="$2"
            shift 2
            ;;
        -i|--isolation)
            ISOLATION="$2"
            shift 2
            ;;
        -W|--workload)
            WORKLOAD="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -f|--format)
            OUTPUT_FORMAT="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        --quick)
            NUM_TENANTS=5
            ROWS_PER_TENANT=1000
            DURATION=30
            WARMUP=5
            shift
            ;;
        --stress)
            NUM_TENANTS=50
            ROWS_PER_TENANT=50000
            DURATION=300
            WARMUP=30
            shift
            ;;
        --all-workloads)
            ALL_WORKLOADS=1
            shift
            ;;
        --all-isolations)
            ALL_ISOLATIONS=1
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Main execution
main() {
    echo ""
    echo "=========================================="
    echo "  Multi-Tenant Isolation Benchmark"
    echo "=========================================="
    echo ""

    check_prerequisites
    setup_output_dir

    if [[ "${ALL_WORKLOADS}" == "1" ]]; then
        run_all_workloads
        generate_comparison_report
    elif [[ "${ALL_ISOLATIONS}" == "1" ]]; then
        run_all_isolations
        generate_comparison_report
    else
        run_benchmark "${ISOLATION}" "${WORKLOAD}" ""
    fi

    echo ""
    echo "=========================================="
    echo "  Benchmark Complete"
    echo "=========================================="
    echo ""
    echo "Results directory: ${OUTPUT_DIR}"
    echo ""
}

main "$@"
