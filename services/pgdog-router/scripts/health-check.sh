#!/bin/bash
# ==============================================================================
# PgDog Router Health Check Script
# ==============================================================================
#
# Performs health checks on the PgDog router to ensure it's functioning correctly.
# This script is used by Docker HEALTHCHECK and Kubernetes probes.
#
# Exit Codes:
#   0 - Healthy
#   1 - Unhealthy
#
# Environment Variables:
#   PGDOG_HOST           - Host to check (default: 127.0.0.1)
#   PGDOG_PORT           - PostgreSQL port (default: 5432)
#   PGDOG_ADMIN_PORT     - Admin port (default: 6432)
#   PGDOG_METRICS_PORT   - Metrics port (default: 9090)
#   HEALTH_CHECK_TIMEOUT - Timeout in seconds (default: 5)
#   HEALTH_CHECK_USER    - PostgreSQL user for connection test (default: postgres)
#   HEALTH_CHECK_DB      - Database for connection test (default: postgres)
#   CHECK_METRICS        - Also check metrics endpoint (default: true)
#   CHECK_ADMIN          - Also check admin endpoint (default: true)
#   VERBOSE              - Enable verbose output (default: false)
#
# ==============================================================================

set -euo pipefail

# Configuration
readonly HOST="${PGDOG_HOST:-127.0.0.1}"
readonly PG_PORT="${PGDOG_PORT:-5432}"
readonly ADMIN_PORT="${PGDOG_ADMIN_PORT:-6432}"
readonly METRICS_PORT="${PGDOG_METRICS_PORT:-9090}"
readonly TIMEOUT="${HEALTH_CHECK_TIMEOUT:-5}"
readonly USER="${HEALTH_CHECK_USER:-postgres}"
readonly DATABASE="${HEALTH_CHECK_DB:-postgres}"
readonly CHECK_METRICS="${CHECK_METRICS:-true}"
readonly CHECK_ADMIN="${CHECK_ADMIN:-true}"
readonly VERBOSE="${VERBOSE:-false}"

# Track check results
declare -a FAILED_CHECKS=()
declare -a PASSED_CHECKS=()

# ==============================================================================
# Logging Functions
# ==============================================================================

log_debug() {
    if [[ "${VERBOSE}" == "true" ]]; then
        echo "[DEBUG] $*"
    fi
}

log_info() {
    echo "[INFO] $*"
}

log_error() {
    echo "[ERROR] $*" >&2
}

# ==============================================================================
# Health Check Functions
# ==============================================================================

check_port_open() {
    local host="$1"
    local port="$2"
    local name="$3"

    log_debug "Checking if port ${port} is open on ${host}..."

    if command -v nc &> /dev/null; then
        if nc -z -w "${TIMEOUT}" "${host}" "${port}" 2>/dev/null; then
            log_debug "Port ${port} (${name}) is open"
            PASSED_CHECKS+=("${name}_port")
            return 0
        fi
    elif command -v timeout &> /dev/null; then
        if timeout "${TIMEOUT}" bash -c "echo > /dev/tcp/${host}/${port}" 2>/dev/null; then
            log_debug "Port ${port} (${name}) is open"
            PASSED_CHECKS+=("${name}_port")
            return 0
        fi
    fi

    log_error "Port ${port} (${name}) is not open"
    FAILED_CHECKS+=("${name}_port")
    return 1
}

check_postgres_connection() {
    log_debug "Checking PostgreSQL connection..."

    # Check if pg_isready is available
    if command -v pg_isready &> /dev/null; then
        if pg_isready -h "${HOST}" -p "${PG_PORT}" -t "${TIMEOUT}" -q 2>/dev/null; then
            log_debug "PostgreSQL connection successful (pg_isready)"
            PASSED_CHECKS+=("postgres_ready")
            return 0
        fi
    fi

    # Fallback to psql if available
    if command -v psql &> /dev/null; then
        if PGCONNECT_TIMEOUT="${TIMEOUT}" psql -h "${HOST}" -p "${PG_PORT}" -U "${USER}" -d "${DATABASE}" -c "SELECT 1" -t -A 2>/dev/null | grep -q "1"; then
            log_debug "PostgreSQL connection successful (psql)"
            PASSED_CHECKS+=("postgres_query")
            return 0
        fi
    fi

    # Fallback to port check
    if check_port_open "${HOST}" "${PG_PORT}" "postgres"; then
        return 0
    fi

    log_error "PostgreSQL connection check failed"
    FAILED_CHECKS+=("postgres_connection")
    return 1
}

check_admin_port() {
    if [[ "${CHECK_ADMIN}" != "true" ]]; then
        log_debug "Admin check disabled, skipping..."
        return 0
    fi

    log_debug "Checking admin port..."

    if check_port_open "${HOST}" "${ADMIN_PORT}" "admin"; then
        return 0
    fi

    return 1
}

check_metrics_endpoint() {
    if [[ "${CHECK_METRICS}" != "true" ]]; then
        log_debug "Metrics check disabled, skipping..."
        return 0
    fi

    log_debug "Checking metrics endpoint..."

    # Try curl first
    if command -v curl &> /dev/null; then
        local response
        response=$(curl -s --max-time "${TIMEOUT}" "http://${HOST}:${METRICS_PORT}/metrics" 2>/dev/null || echo "")

        if echo "${response}" | grep -q "^# HELP\|^# TYPE\|pgdog_"; then
            log_debug "Metrics endpoint is healthy"
            PASSED_CHECKS+=("metrics_endpoint")
            return 0
        fi
    fi

    # Fallback to wget
    if command -v wget &> /dev/null; then
        local response
        response=$(wget -q --timeout="${TIMEOUT}" -O - "http://${HOST}:${METRICS_PORT}/metrics" 2>/dev/null || echo "")

        if echo "${response}" | grep -q "^# HELP\|^# TYPE\|pgdog_"; then
            log_debug "Metrics endpoint is healthy"
            PASSED_CHECKS+=("metrics_endpoint")
            return 0
        fi
    fi

    # Fallback to port check
    if check_port_open "${HOST}" "${METRICS_PORT}" "metrics"; then
        return 0
    fi

    log_error "Metrics endpoint check failed"
    FAILED_CHECKS+=("metrics_endpoint")
    return 1
}

check_process_running() {
    log_debug "Checking if PgDog process is running..."

    # Check for pgdog process
    if pgrep -x "pgdog" > /dev/null 2>&1; then
        log_debug "PgDog process is running"
        PASSED_CHECKS+=("process_running")
        return 0
    fi

    # Check PID file if exists
    local pid_file="/var/run/pgdog/pgdog.pid"
    if [[ -f "${pid_file}" ]]; then
        local pid
        pid=$(cat "${pid_file}")
        if kill -0 "${pid}" 2>/dev/null; then
            log_debug "PgDog process is running (PID: ${pid})"
            PASSED_CHECKS+=("process_running")
            return 0
        fi
    fi

    log_debug "Could not verify process, will check via connection"
    return 0
}

check_connection_pool_stats() {
    log_debug "Checking connection pool statistics..."

    # This check verifies the pool is accepting connections
    # We use the admin interface if available
    if command -v psql &> /dev/null; then
        local result
        result=$(PGCONNECT_TIMEOUT="${TIMEOUT}" psql -h "${HOST}" -p "${ADMIN_PORT}" -U pgdog_admin -c "SHOW POOLS" -t -A 2>/dev/null || echo "")

        if [[ -n "${result}" ]]; then
            log_debug "Connection pool stats retrieved successfully"
            PASSED_CHECKS+=("pool_stats")
            return 0
        fi
    fi

    # Not a critical failure if we can't get stats
    log_debug "Could not retrieve pool stats (non-critical)"
    return 0
}

# ==============================================================================
# Main Health Check
# ==============================================================================

run_health_checks() {
    local exit_code=0

    log_debug "Starting health checks..."
    log_debug "Host: ${HOST}, PostgreSQL Port: ${PG_PORT}, Admin Port: ${ADMIN_PORT}, Metrics Port: ${METRICS_PORT}"

    # Required checks (must pass)
    if ! check_postgres_connection; then
        exit_code=1
    fi

    # Optional checks (logged but don't fail)
    check_process_running || true
    check_admin_port || true
    check_metrics_endpoint || true
    check_connection_pool_stats || true

    return ${exit_code}
}

print_summary() {
    if [[ "${VERBOSE}" == "true" ]]; then
        echo ""
        echo "Health Check Summary"
        echo "===================="

        if [[ ${#PASSED_CHECKS[@]} -gt 0 ]]; then
            echo "Passed checks: ${PASSED_CHECKS[*]}"
        fi

        if [[ ${#FAILED_CHECKS[@]} -gt 0 ]]; then
            echo "Failed checks: ${FAILED_CHECKS[*]}"
        fi
    fi
}

main() {
    # Handle help
    if [[ "${1:-}" == "--help" ]] || [[ "${1:-}" == "-h" ]]; then
        cat << EOF
PgDog Health Check Script

Usage: health-check.sh [options]

Options:
  -h, --help     Show this help message
  -v, --verbose  Enable verbose output

Environment Variables:
  PGDOG_HOST           Host to check (default: 127.0.0.1)
  PGDOG_PORT           PostgreSQL port (default: 5432)
  PGDOG_ADMIN_PORT     Admin port (default: 6432)
  PGDOG_METRICS_PORT   Metrics port (default: 9090)
  HEALTH_CHECK_TIMEOUT Timeout in seconds (default: 5)
  CHECK_METRICS        Check metrics endpoint (default: true)
  CHECK_ADMIN          Check admin endpoint (default: true)
  VERBOSE              Enable verbose output (default: false)

Exit Codes:
  0 - All required checks passed (healthy)
  1 - One or more required checks failed (unhealthy)

EOF
        exit 0
    fi

    # Handle verbose flag
    if [[ "${1:-}" == "-v" ]] || [[ "${1:-}" == "--verbose" ]]; then
        VERBOSE="true"
    fi

    # Run checks
    if run_health_checks; then
        print_summary
        if [[ "${VERBOSE}" == "true" ]]; then
            log_info "Health check passed"
        fi
        exit 0
    else
        print_summary
        log_error "Health check failed"
        exit 1
    fi
}

# Run main function
main "$@"
