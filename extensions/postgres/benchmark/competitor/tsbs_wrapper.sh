#!/usr/bin/env bash
#
# TSBS (Time Series Benchmark Suite) Wrapper for Orochi DB Competitor Comparison
#
# Uses the industry-standard TSBS from Timescale to benchmark time-series
# workloads across PostgreSQL-compatible databases.
#
# Repository: https://github.com/timescale/tsbs
#
# Environment variables:
#   BENCH_HOST       - Database host (default: localhost)
#   BENCH_PORT       - Database port (default: 5432)
#   BENCH_DB         - Database name (default: tsbs)
#   BENCH_USER       - Database user (default: postgres)
#   BENCH_PASSWORD   - Database password (optional)
#   BENCH_TARGET     - Target name for identification
#   BENCH_SCALE      - Scale (number of hosts) (default: 100)
#   BENCH_SEED       - Random seed for reproducibility (default: 12345)
#   BENCH_WORKLOAD   - Workload type: devops, iot (default: devops)
#   BENCH_DURATION   - Data duration to generate (default: 3d)
#   BENCH_WORKERS    - Number of parallel workers (default: 8)
#
# Output:
#   Writes metrics to stdout in parseable format
#   Creates detailed log in $RESULTS_DIR
#

set -euo pipefail

# Configuration
BENCH_HOST="${BENCH_HOST:-localhost}"
BENCH_PORT="${BENCH_PORT:-5432}"
BENCH_DB="${BENCH_DB:-tsbs}"
BENCH_USER="${BENCH_USER:-postgres}"
BENCH_PASSWORD="${BENCH_PASSWORD:-}"
BENCH_TARGET="${BENCH_TARGET:-unknown}"
BENCH_SCALE="${BENCH_SCALE:-100}"
BENCH_SEED="${BENCH_SEED:-12345}"
BENCH_WORKLOAD="${BENCH_WORKLOAD:-devops}"
BENCH_DURATION="${BENCH_DURATION:-3d}"
BENCH_WORKERS="${BENCH_WORKERS:-8}"

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
TOOLS_DIR="${SCRIPT_DIR}/tools"
DATA_DIR="${SCRIPT_DIR}/data"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${RESULTS_DIR}/tsbs_${BENCH_TARGET}_${TIMESTAMP}.log"

# TSBS binary paths (after installation)
TSBS_GENERATE_DATA=""
TSBS_LOAD=""
TSBS_GENERATE_QUERIES=""
TSBS_RUN_QUERIES=""

# Ensure directories exist
mkdir -p "${RESULTS_DIR}" "${TOOLS_DIR}" "${DATA_DIR}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

error() {
    log "ERROR: $*" >&2
    exit 1
}

# Check if TSBS is installed
check_tsbs() {
    local tsbs_dir="${TOOLS_DIR}/tsbs"

    if [[ -d "${tsbs_dir}" && -f "${tsbs_dir}/bin/tsbs_generate_data" ]]; then
        TSBS_GENERATE_DATA="${tsbs_dir}/bin/tsbs_generate_data"
        TSBS_LOAD="${tsbs_dir}/bin/tsbs_load_timescaledb"
        TSBS_GENERATE_QUERIES="${tsbs_dir}/bin/tsbs_generate_queries"
        TSBS_RUN_QUERIES="${tsbs_dir}/bin/tsbs_run_queries_timescaledb"
        return 0
    fi

    # Check if installed globally
    if command -v tsbs_generate_data &> /dev/null; then
        TSBS_GENERATE_DATA="tsbs_generate_data"
        TSBS_LOAD="tsbs_load_timescaledb"
        TSBS_GENERATE_QUERIES="tsbs_generate_queries"
        TSBS_RUN_QUERIES="tsbs_run_queries_timescaledb"
        return 0
    fi

    return 1
}

# Install TSBS
install_tsbs() {
    log "Installing TSBS (Time Series Benchmark Suite)..."

    cd "${TOOLS_DIR}"

    # Check for Go
    if ! command -v go &> /dev/null; then
        error "Go is required to build TSBS. Please install Go 1.18+."
    fi

    # Clone repository
    if [[ ! -d "tsbs" ]]; then
        git clone https://github.com/timescale/tsbs.git
    fi

    cd tsbs

    # Build all binaries
    log "Building TSBS binaries..."
    make

    # Create bin directory and copy binaries
    mkdir -p bin
    cp cmd/tsbs_generate_data/tsbs_generate_data bin/
    cp cmd/tsbs_load_timescaledb/tsbs_load_timescaledb bin/ 2>/dev/null || true
    cp cmd/tsbs_load_questdb/tsbs_load_questdb bin/ 2>/dev/null || true
    cp cmd/tsbs_generate_queries/tsbs_generate_queries bin/
    cp cmd/tsbs_run_queries_timescaledb/tsbs_run_queries_timescaledb bin/ 2>/dev/null || true
    cp cmd/tsbs_run_queries_questdb/tsbs_run_queries_questdb bin/ 2>/dev/null || true

    # Update paths
    TSBS_GENERATE_DATA="${TOOLS_DIR}/tsbs/bin/tsbs_generate_data"
    TSBS_LOAD="${TOOLS_DIR}/tsbs/bin/tsbs_load_timescaledb"
    TSBS_GENERATE_QUERIES="${TOOLS_DIR}/tsbs/bin/tsbs_generate_queries"
    TSBS_RUN_QUERIES="${TOOLS_DIR}/tsbs/bin/tsbs_run_queries_timescaledb"

    log "TSBS installed successfully"
}

# Determine correct TSBS commands based on target
get_tsbs_commands() {
    local target="$1"

    case "${target}" in
        orochi|timescaledb|vanilla-postgres)
            TSBS_LOAD="${TOOLS_DIR}/tsbs/bin/tsbs_load_timescaledb"
            TSBS_RUN_QUERIES="${TOOLS_DIR}/tsbs/bin/tsbs_run_queries_timescaledb"
            ;;
        questdb)
            TSBS_LOAD="${TOOLS_DIR}/tsbs/bin/tsbs_load_questdb"
            TSBS_RUN_QUERIES="${TOOLS_DIR}/tsbs/bin/tsbs_run_queries_questdb"
            ;;
        *)
            # Default to TimescaleDB loader for PostgreSQL-compatible
            TSBS_LOAD="${TOOLS_DIR}/tsbs/bin/tsbs_load_timescaledb"
            TSBS_RUN_QUERIES="${TOOLS_DIR}/tsbs/bin/tsbs_run_queries_timescaledb"
            ;;
    esac
}

# Generate test data
generate_data() {
    local workload="$1"
    local scale="$2"
    local duration="$3"
    local data_file="${DATA_DIR}/tsbs_${workload}_${scale}_${duration}.gz"

    if [[ -f "${data_file}" ]]; then
        log "Using existing data file: ${data_file}"
        echo "${data_file}"
        return
    fi

    log "Generating TSBS data..."
    log "  Workload: ${workload}"
    log "  Scale: ${scale} hosts"
    log "  Duration: ${duration}"

    local format
    case "${BENCH_TARGET}" in
        questdb)
            format="questdb"
            ;;
        *)
            format="timescaledb"
            ;;
    esac

    "${TSBS_GENERATE_DATA}" \
        --use-case="${workload}" \
        --seed="${BENCH_SEED}" \
        --scale="${scale}" \
        --timestamp-start="2024-01-01T00:00:00Z" \
        --timestamp-end="2024-01-04T00:00:00Z" \
        --log-interval="10s" \
        --format="${format}" \
        2>&1 | gzip > "${data_file}"

    log "Data generated: ${data_file}"
    echo "${data_file}"
}

# Generate queries
generate_queries() {
    local workload="$1"
    local scale="$2"
    local query_types=("single-groupby-1-1-1" "single-groupby-1-8-1" "single-groupby-5-1-12" "double-groupby-1" "high-cpu-all" "high-cpu-1" "lastpoint")
    local query_count=1000

    log "Generating TSBS queries..."

    local queries_dir="${DATA_DIR}/queries"
    mkdir -p "${queries_dir}"

    for query_type in "${query_types[@]}"; do
        local query_file="${queries_dir}/${query_type}.txt"

        if [[ ! -f "${query_file}" ]]; then
            log "  Generating ${query_type} queries..."

            "${TSBS_GENERATE_QUERIES}" \
                --use-case="${workload}" \
                --seed="${BENCH_SEED}" \
                --scale="${scale}" \
                --timestamp-start="2024-01-01T00:00:00Z" \
                --timestamp-end="2024-01-04T00:00:00Z" \
                --queries="${query_count}" \
                --query-type="${query_type}" \
                > "${query_file}" 2>/dev/null || true
        fi
    done

    log "Queries generated in: ${queries_dir}"
}

# Setup database schema
setup_database() {
    log "Setting up database schema..."

    # Create database if it doesn't exist
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d postgres -c "CREATE DATABASE ${BENCH_DB};" 2>/dev/null || true

    # For TimescaleDB targets, enable extension
    if [[ "${BENCH_TARGET}" == "timescaledb" ]]; then
        PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
            -U "${BENCH_USER}" -d "${BENCH_DB}" -c "CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE;" 2>/dev/null || true
    fi

    # For Orochi targets, enable extension
    if [[ "${BENCH_TARGET}" == "orochi" ]]; then
        PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
            -U "${BENCH_USER}" -d "${BENCH_DB}" -c "CREATE EXTENSION IF NOT EXISTS orochi CASCADE;" 2>/dev/null || true
    fi

    log "Database schema ready"
}

# Load data into database
load_data() {
    local data_file="$1"

    log "Loading data into ${BENCH_TARGET}..."

    local start_time end_time duration
    start_time=$(date +%s.%N)

    # Construct connection string
    local conn_string="host=${BENCH_HOST} port=${BENCH_PORT} user=${BENCH_USER} dbname=${BENCH_DB}"
    if [[ -n "${BENCH_PASSWORD}" ]]; then
        conn_string="${conn_string} password=${BENCH_PASSWORD}"
    fi

    # Load based on target
    case "${BENCH_TARGET}" in
        questdb)
            gunzip -c "${data_file}" | "${TSBS_LOAD}" \
                --host="${BENCH_HOST}" \
                --port=9009 \
                --workers="${BENCH_WORKERS}" \
                2>&1 | tee -a "${LOG_FILE}"
            ;;
        *)
            # PostgreSQL-compatible (Orochi, TimescaleDB, vanilla)
            gunzip -c "${data_file}" | "${TSBS_LOAD}" \
                --postgres="${conn_string}" \
                --workers="${BENCH_WORKERS}" \
                --batch-size=10000 \
                --do-create-db=false \
                2>&1 | tee -a "${LOG_FILE}"
            ;;
    esac

    end_time=$(date +%s.%N)
    duration=$(echo "${end_time} - ${start_time}" | bc)

    # Get row count
    local row_count
    row_count=$(PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -t -c \
        "SELECT COUNT(*) FROM cpu;" 2>/dev/null || echo "0")

    # Calculate rows/second
    local rows_per_second
    rows_per_second=$(echo "scale=2; ${row_count// /} / ${duration}" | bc || echo "0")

    log "Data loading complete"
    log "  Duration: ${duration} seconds"
    log "  Rows loaded: ${row_count}"
    log "  Rows/second: ${rows_per_second}"

    # Return metrics
    echo "LOAD_DURATION=${duration}"
    echo "LOAD_ROWS=${row_count// /}"
    echo "LOAD_ROWS_PER_SEC=${rows_per_second}"
}

# Run queries
run_queries() {
    log "Running TSBS queries..."

    local queries_dir="${DATA_DIR}/queries"
    local query_types=("single-groupby-1-1-1" "single-groupby-1-8-1" "high-cpu-1" "lastpoint")
    local results=""

    for query_type in "${query_types[@]}"; do
        local query_file="${queries_dir}/${query_type}.txt"

        if [[ ! -f "${query_file}" ]]; then
            log "  Skipping ${query_type} (no query file)"
            continue
        fi

        log "  Running ${query_type} queries..."

        local start_time end_time duration
        start_time=$(date +%s.%N)

        local output
        case "${BENCH_TARGET}" in
            questdb)
                output=$(cat "${query_file}" | "${TSBS_RUN_QUERIES}" \
                    --host="${BENCH_HOST}" \
                    --port=8812 \
                    --workers="${BENCH_WORKERS}" \
                    2>&1 || true)
                ;;
            *)
                local conn_string="host=${BENCH_HOST} port=${BENCH_PORT} user=${BENCH_USER} dbname=${BENCH_DB}"
                output=$(cat "${query_file}" | "${TSBS_RUN_QUERIES}" \
                    --postgres="${conn_string}" \
                    --workers="${BENCH_WORKERS}" \
                    2>&1 || true)
                ;;
        esac

        end_time=$(date +%s.%N)
        duration=$(echo "${end_time} - ${start_time}" | bc)

        # Parse latency from output
        local mean_latency min_latency max_latency
        mean_latency=$(echo "${output}" | grep -i "mean:" | awk '{print $2}' || echo "0")
        min_latency=$(echo "${output}" | grep -i "min:" | awk '{print $2}' || echo "0")
        max_latency=$(echo "${output}" | grep -i "max:" | awk '{print $2}' || echo "0")

        log "    ${query_type}: mean=${mean_latency}ms, duration=${duration}s"

        results="${results}QUERY_${query_type}_MEAN=${mean_latency}\n"
        results="${results}QUERY_${query_type}_DURATION=${duration}\n"
    done

    echo -e "${results}"
}

# Run synthetic TSBS-like benchmark (fallback)
run_synthetic_tsbs() {
    log "Running synthetic TSBS-like benchmark..."
    log "  Host: ${BENCH_HOST}:${BENCH_PORT}"
    log "  Scale: ${BENCH_SCALE} hosts"
    log "  Workload: ${BENCH_WORKLOAD}"

    # Create schema
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" << 'EOSQL' 2>&1 | tee -a "${LOG_FILE}"

-- Create TSBS-like schema
CREATE TABLE IF NOT EXISTS cpu (
    time TIMESTAMPTZ NOT NULL,
    tags_id INTEGER,
    hostname VARCHAR(100),
    region VARCHAR(50),
    datacenter VARCHAR(50),
    usage_user DOUBLE PRECISION,
    usage_system DOUBLE PRECISION,
    usage_idle DOUBLE PRECISION,
    usage_nice DOUBLE PRECISION,
    usage_iowait DOUBLE PRECISION,
    usage_irq DOUBLE PRECISION,
    usage_softirq DOUBLE PRECISION,
    usage_steal DOUBLE PRECISION,
    usage_guest DOUBLE PRECISION,
    usage_guest_nice DOUBLE PRECISION
);

CREATE INDEX IF NOT EXISTS cpu_time_idx ON cpu(time DESC);
CREATE INDEX IF NOT EXISTS cpu_hostname_time_idx ON cpu(hostname, time DESC);

EOSQL

    # Generate and insert data
    log "Inserting test data..."
    local start_time end_time
    start_time=$(date +%s.%N)

    local batch_size=10000
    local total_rows=$((BENCH_SCALE * 1000))
    local rows_inserted=0

    while [[ ${rows_inserted} -lt ${total_rows} ]]; do
        PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
            -U "${BENCH_USER}" -d "${BENCH_DB}" -q << EOSQL
INSERT INTO cpu (time, hostname, region, datacenter, usage_user, usage_system, usage_idle)
SELECT
    NOW() - (g || ' seconds')::interval,
    'host_' || (g % ${BENCH_SCALE}),
    'region_' || (g % 5),
    'dc_' || (g % 3),
    random() * 100,
    random() * 50,
    random() * 100
FROM generate_series(1, ${batch_size}) g;
EOSQL
        rows_inserted=$((rows_inserted + batch_size))

        if [[ $((rows_inserted % 50000)) -eq 0 ]]; then
            log "  Inserted ${rows_inserted} rows..."
        fi
    done

    end_time=$(date +%s.%N)
    local load_duration
    load_duration=$(echo "${end_time} - ${start_time}" | bc)
    local rows_per_second
    rows_per_second=$(echo "scale=2; ${total_rows} / ${load_duration}" | bc)

    log "Data loading complete"
    log "  Duration: ${load_duration} seconds"
    log "  Rows loaded: ${total_rows}"
    log "  Rows/second: ${rows_per_second}"

    # Run queries
    log "Running test queries..."

    local query_start query_end query_duration

    # Query 1: Time-based aggregation
    query_start=$(date +%s.%N)
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -q -c \
        "SELECT date_trunc('hour', time), hostname, AVG(usage_user) FROM cpu GROUP BY 1, 2 LIMIT 100;" > /dev/null
    query_end=$(date +%s.%N)
    local query1_duration
    query1_duration=$(echo "${query_end} - ${query_start}" | bc)
    log "  Query 1 (time aggregation): ${query1_duration}s"

    # Query 2: High CPU filter
    query_start=$(date +%s.%N)
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -q -c \
        "SELECT * FROM cpu WHERE usage_user > 90 ORDER BY time DESC LIMIT 100;" > /dev/null
    query_end=$(date +%s.%N)
    local query2_duration
    query2_duration=$(echo "${query_end} - ${query_start}" | bc)
    log "  Query 2 (high CPU filter): ${query2_duration}s"

    # Query 3: Last point per host
    query_start=$(date +%s.%N)
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -q -c \
        "SELECT DISTINCT ON (hostname) hostname, time, usage_user FROM cpu ORDER BY hostname, time DESC;" > /dev/null
    query_end=$(date +%s.%N)
    local query3_duration
    query3_duration=$(echo "${query_end} - ${query_start}" | bc)
    log "  Query 3 (last point): ${query3_duration}s"

    # Output results
    echo ""
    echo "=== TSBS Results (Synthetic) ==="
    echo "Target: ${BENCH_TARGET}"
    echo "Scale: ${BENCH_SCALE} hosts"
    echo "Workload: ${BENCH_WORKLOAD}"
    echo ""
    echo "rows/sec: ${rows_per_second}"
    echo "total rows: ${total_rows}"
    echo "load_duration: ${load_duration}"
    echo ""
    echo "Query 1 (time aggregation): ${query1_duration}s"
    echo "Query 2 (high CPU filter): ${query2_duration}s"
    echo "Query 3 (last point): ${query3_duration}s"
    echo "================================="
}

# Main
main() {
    log "=== TSBS Benchmark Wrapper ==="
    log "Target: ${BENCH_TARGET}"
    log "Scale: ${BENCH_SCALE} hosts"
    log "Workload: ${BENCH_WORKLOAD}"
    log "Duration: ${BENCH_DURATION}"
    log ""

    # Setup database
    setup_database

    # Check for TSBS tools
    if check_tsbs; then
        log "Using TSBS tools"
        get_tsbs_commands "${BENCH_TARGET}"

        # Generate data
        data_file=$(generate_data "${BENCH_WORKLOAD}" "${BENCH_SCALE}" "${BENCH_DURATION}")

        # Generate queries
        generate_queries "${BENCH_WORKLOAD}" "${BENCH_SCALE}"

        # Load data
        load_metrics=$(load_data "${data_file}")
        echo "${load_metrics}" | tee -a "${LOG_FILE}"

        # Run queries
        query_metrics=$(run_queries)
        echo "${query_metrics}" | tee -a "${LOG_FILE}"

        # Output final summary
        rows_per_sec=$(echo "${load_metrics}" | grep "LOAD_ROWS_PER_SEC" | cut -d= -f2)
        total_rows=$(echo "${load_metrics}" | grep "LOAD_ROWS=" | cut -d= -f2)

        echo ""
        echo "=== TSBS Results ==="
        echo "Target: ${BENCH_TARGET}"
        echo "Scale: ${BENCH_SCALE} hosts"
        echo "Workload: ${BENCH_WORKLOAD}"
        echo ""
        echo "rows/sec: ${rows_per_sec}"
        echo "total rows: ${total_rows}"
        echo "==================="

    else
        log "WARNING: TSBS not found. Run '$0 install' to install."
        log "Running synthetic TSBS simulation as fallback..."
        run_synthetic_tsbs
    fi

    log ""
    log "TSBS benchmark complete. Results logged to: ${LOG_FILE}"
}

# Handle install command
if [[ "${1:-}" == "install" ]]; then
    install_tsbs
    exit 0
fi

main "$@"
