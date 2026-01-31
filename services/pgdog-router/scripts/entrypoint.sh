#!/bin/bash
# ==============================================================================
# PgDog Router Entrypoint Script
# ==============================================================================
#
# This script initializes and starts the PgDog connection pooler for Orochi DB.
# It handles:
#   - Configuration validation
#   - TLS certificate setup
#   - Environment variable substitution
#   - Graceful startup and shutdown
#
# Usage: entrypoint.sh [command] [args...]
#   Commands:
#     pgdog (default) - Start PgDog server
#     validate        - Validate configuration only
#     config-gen      - Generate config from templates
#     shell           - Start a shell
#
# Environment Variables:
#   PGDOG_CONFIG          - Path to pgdog.toml (default: /etc/pgdog/pgdog.toml)
#   PGDOG_USERS_CONFIG    - Path to users.toml (default: /etc/pgdog/users.toml)
#   PGDOG_LOG_LEVEL       - Log level (default: info)
#   PGDOG_HOST            - Listen address (default: 0.0.0.0)
#   PGDOG_PORT            - PostgreSQL port (default: 5432)
#   PGDOG_ADMIN_PORT      - Admin port (default: 6432)
#   PGDOG_METRICS_PORT    - Prometheus metrics port (default: 9090)
#   TLS_ENABLED           - Enable TLS (default: false)
#   TLS_CERT_PATH         - Path to TLS certificate
#   TLS_KEY_PATH          - Path to TLS private key
#   TLS_CA_PATH           - Path to CA certificate
#
# ==============================================================================

set -euo pipefail

# Configuration
readonly PGDOG_BIN="${PGDOG_BIN:-/usr/local/bin/pgdog}"
readonly CONFIG_DIR="${CONFIG_DIR:-/etc/pgdog}"
readonly PGDOG_CONFIG="${PGDOG_CONFIG:-${CONFIG_DIR}/pgdog.toml}"
readonly PGDOG_USERS_CONFIG="${PGDOG_USERS_CONFIG:-${CONFIG_DIR}/users.toml}"
readonly LOG_DIR="${LOG_DIR:-/var/log/pgdog}"
readonly PID_FILE="${PID_FILE:-/var/run/pgdog/pgdog.pid}"

# TLS paths
readonly TLS_CERT_PATH="${TLS_CERT_PATH:-${CONFIG_DIR}/certs/tls.crt}"
readonly TLS_KEY_PATH="${TLS_KEY_PATH:-${CONFIG_DIR}/certs/tls.key}"
readonly TLS_CA_PATH="${TLS_CA_PATH:-${CONFIG_DIR}/certs/ca.crt}"

# Colors for output
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[0;33m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m' # No Color

# ==============================================================================
# Logging Functions
# ==============================================================================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*" >&2
}

log_debug() {
    if [[ "${PGDOG_LOG_LEVEL:-info}" == "debug" ]] || [[ "${PGDOG_LOG_LEVEL:-info}" == "trace" ]]; then
        echo -e "${BLUE}[DEBUG]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
    fi
}

# ==============================================================================
# Validation Functions
# ==============================================================================

validate_binary() {
    log_debug "Checking for PgDog binary at ${PGDOG_BIN}..."

    if [[ ! -x "${PGDOG_BIN}" ]]; then
        log_error "PgDog binary not found or not executable at ${PGDOG_BIN}"
        exit 1
    fi

    log_info "PgDog binary found: ${PGDOG_BIN}"
}

validate_config() {
    log_info "Validating configuration..."

    # Check main config exists
    if [[ ! -f "${PGDOG_CONFIG}" ]]; then
        # Try to generate from template
        if [[ -f "${PGDOG_CONFIG}.tmpl" ]]; then
            log_warn "Main config not found, generating from template..."
            generate_config
        else
            log_error "Configuration file not found: ${PGDOG_CONFIG}"
            log_error "Please provide a configuration file or template (${PGDOG_CONFIG}.tmpl)"
            exit 1
        fi
    fi

    # Validate TOML syntax
    if command -v python3 &> /dev/null; then
        log_debug "Validating TOML syntax..."
        if ! python3 -c "import tomllib; tomllib.load(open('${PGDOG_CONFIG}', 'rb'))" 2>/dev/null; then
            log_error "Invalid TOML syntax in ${PGDOG_CONFIG}"
            exit 1
        fi
    fi

    # Check users config if specified
    if [[ -f "${PGDOG_USERS_CONFIG}" ]]; then
        log_debug "Users configuration found: ${PGDOG_USERS_CONFIG}"
    elif [[ -f "${PGDOG_USERS_CONFIG}.tmpl" ]]; then
        log_warn "Users config not found, generating from template..."
        generate_users_config
    fi

    log_info "Configuration validation passed"
}

validate_tls() {
    if [[ "${TLS_ENABLED:-false}" == "true" ]]; then
        log_info "Validating TLS configuration..."

        # Check certificate
        if [[ ! -f "${TLS_CERT_PATH}" ]]; then
            log_error "TLS certificate not found: ${TLS_CERT_PATH}"
            exit 1
        fi

        # Check private key
        if [[ ! -f "${TLS_KEY_PATH}" ]]; then
            log_error "TLS private key not found: ${TLS_KEY_PATH}"
            exit 1
        fi

        # Verify key permissions
        local key_perms
        key_perms=$(stat -c '%a' "${TLS_KEY_PATH}" 2>/dev/null || stat -f '%p' "${TLS_KEY_PATH}" 2>/dev/null | tail -c 4)
        if [[ "${key_perms}" != "600" ]] && [[ "${key_perms}" != "400" ]]; then
            log_warn "TLS private key has insecure permissions: ${key_perms} (should be 600 or 400)"
        fi

        # Verify certificate validity
        if command -v openssl &> /dev/null; then
            log_debug "Checking certificate validity..."
            if ! openssl x509 -in "${TLS_CERT_PATH}" -noout -checkend 0 2>/dev/null; then
                log_error "TLS certificate is expired or invalid"
                exit 1
            fi

            # Check expiration warning (30 days)
            if ! openssl x509 -in "${TLS_CERT_PATH}" -noout -checkend 2592000 2>/dev/null; then
                log_warn "TLS certificate will expire within 30 days"
            fi
        fi

        # Check CA certificate if specified
        if [[ -f "${TLS_CA_PATH}" ]]; then
            log_debug "CA certificate found: ${TLS_CA_PATH}"
        fi

        log_info "TLS validation passed"
    else
        log_debug "TLS is disabled, skipping validation"
    fi
}

# ==============================================================================
# Configuration Generation
# ==============================================================================

generate_config() {
    local template="${PGDOG_CONFIG}.tmpl"

    if [[ ! -f "${template}" ]]; then
        log_error "Configuration template not found: ${template}"
        exit 1
    fi

    log_info "Generating configuration from template..."

    # Simple environment variable substitution
    # Replace {{.VAR}} with environment variable value
    local output="${PGDOG_CONFIG}"

    # Use envsubst if available, otherwise use sed
    if command -v envsubst &> /dev/null; then
        envsubst < "${template}" > "${output}"
    else
        # Manual substitution for common variables
        cp "${template}" "${output}"

        # Substitute known variables
        sed -i "s|{{.ClusterName}}|${CLUSTER_NAME:-default}|g" "${output}"
        sed -i "s|{{.Namespace}}|${NAMESPACE:-orochi-cloud}|g" "${output}"
        sed -i "s|{{.DatabaseName}}|${DATABASE_NAME:-postgres}|g" "${output}"
        sed -i "s|{{.ShardCount}}|${SHARD_COUNT:-32}|g" "${output}"
        sed -i "s|{{.PoolMode}}|${POOL_MODE:-transaction}|g" "${output}"
        sed -i "s|{{.MaxPoolSize}}|${MAX_POOL_SIZE:-100}|g" "${output}"
        sed -i "s|{{.Host}}|${PGDOG_HOST:-0.0.0.0}|g" "${output}"
        sed -i "s|{{.Port}}|${PGDOG_PORT:-5432}|g" "${output}"
        sed -i "s|{{.AdminPort}}|${PGDOG_ADMIN_PORT:-6432}|g" "${output}"
        sed -i "s|{{.MetricsPort}}|${PGDOG_METRICS_PORT:-9090}|g" "${output}"
        sed -i "s|{{.LogLevel}}|${PGDOG_LOG_LEVEL:-info}|g" "${output}"
        sed -i "s|{{.TLSEnabled}}|${TLS_ENABLED:-false}|g" "${output}"
        sed -i "s|{{.WorkerThreads}}|${WORKER_THREADS:-4}|g" "${output}"
    fi

    log_info "Configuration generated: ${output}"
}

generate_users_config() {
    local template="${PGDOG_USERS_CONFIG}.tmpl"

    if [[ ! -f "${template}" ]]; then
        log_warn "Users configuration template not found: ${template}"
        return 0
    fi

    log_info "Generating users configuration from template..."

    local output="${PGDOG_USERS_CONFIG}"
    cp "${template}" "${output}"

    # Substitute known variables
    sed -i "s|{{.ClusterName}}|${CLUSTER_NAME:-default}|g" "${output}"
    sed -i "s|{{.DatabaseName}}|${DATABASE_NAME:-postgres}|g" "${output}"
    sed -i "s|{{.AdminPassword}}|${ADMIN_PASSWORD:-}|g" "${output}"
    sed -i "s|{{.AppPassword}}|${APP_PASSWORD:-}|g" "${output}"
    sed -i "s|{{.ReadOnlyPassword}}|${READONLY_PASSWORD:-}|g" "${output}"

    log_info "Users configuration generated: ${output}"
}

# ==============================================================================
# Setup Functions
# ==============================================================================

setup_directories() {
    log_debug "Setting up directories..."

    # Create log directory if it doesn't exist
    if [[ ! -d "${LOG_DIR}" ]]; then
        mkdir -p "${LOG_DIR}" 2>/dev/null || true
    fi

    # Create PID file directory
    local pid_dir
    pid_dir=$(dirname "${PID_FILE}")
    if [[ ! -d "${pid_dir}" ]]; then
        mkdir -p "${pid_dir}" 2>/dev/null || true
    fi
}

setup_tls() {
    if [[ "${TLS_ENABLED:-false}" == "true" ]]; then
        log_info "Setting up TLS..."

        # Ensure certs directory exists
        local certs_dir="${CONFIG_DIR}/certs"
        if [[ ! -d "${certs_dir}" ]]; then
            mkdir -p "${certs_dir}"
        fi

        # If certs are provided via environment, write them
        if [[ -n "${TLS_CERT:-}" ]]; then
            echo "${TLS_CERT}" > "${TLS_CERT_PATH}"
            log_debug "TLS certificate written from environment"
        fi

        if [[ -n "${TLS_KEY:-}" ]]; then
            echo "${TLS_KEY}" > "${TLS_KEY_PATH}"
            chmod 600 "${TLS_KEY_PATH}"
            log_debug "TLS private key written from environment"
        fi

        if [[ -n "${TLS_CA:-}" ]]; then
            echo "${TLS_CA}" > "${TLS_CA_PATH}"
            log_debug "CA certificate written from environment"
        fi

        log_info "TLS setup complete"
    fi
}

# ==============================================================================
# Signal Handlers
# ==============================================================================

shutdown_handler() {
    log_info "Received shutdown signal, stopping PgDog..."

    if [[ -f "${PID_FILE}" ]]; then
        local pid
        pid=$(cat "${PID_FILE}")

        if kill -0 "${pid}" 2>/dev/null; then
            # Send SIGTERM for graceful shutdown
            kill -TERM "${pid}"

            # Wait for process to exit (max 30 seconds)
            local count=0
            while kill -0 "${pid}" 2>/dev/null && [[ ${count} -lt 30 ]]; do
                sleep 1
                ((count++))
            done

            # Force kill if still running
            if kill -0 "${pid}" 2>/dev/null; then
                log_warn "Process did not stop gracefully, sending SIGKILL..."
                kill -KILL "${pid}"
            fi
        fi

        rm -f "${PID_FILE}"
    fi

    log_info "PgDog stopped"
    exit 0
}

reload_handler() {
    log_info "Received reload signal, reloading configuration..."

    if [[ -f "${PID_FILE}" ]]; then
        local pid
        pid=$(cat "${PID_FILE}")

        if kill -0 "${pid}" 2>/dev/null; then
            kill -HUP "${pid}"
            log_info "Reload signal sent to PgDog (PID: ${pid})"
        fi
    fi
}

# ==============================================================================
# Main Functions
# ==============================================================================

start_pgdog() {
    log_info "Starting PgDog connection pooler..."
    log_info "Configuration: ${PGDOG_CONFIG}"

    # Build command arguments
    local cmd_args=("--config" "${PGDOG_CONFIG}")

    if [[ -f "${PGDOG_USERS_CONFIG}" ]]; then
        cmd_args+=("--users" "${PGDOG_USERS_CONFIG}")
    fi

    # Set up signal handlers
    trap shutdown_handler SIGTERM SIGINT SIGQUIT
    trap reload_handler SIGHUP

    log_info "PgDog is ready to accept connections"
    log_info "  PostgreSQL port: ${PGDOG_PORT:-5432}"
    log_info "  Admin port: ${PGDOG_ADMIN_PORT:-6432}"
    log_info "  Metrics port: ${PGDOG_METRICS_PORT:-9090}"

    # Start PgDog
    exec "${PGDOG_BIN}" "${cmd_args[@]}"
}

print_help() {
    cat << EOF
PgDog Router Entrypoint Script

Usage: entrypoint.sh [command] [args...]

Commands:
  pgdog        Start PgDog server (default)
  validate     Validate configuration only
  config-gen   Generate config from templates
  shell        Start a bash shell
  help         Show this help message

Environment Variables:
  PGDOG_CONFIG          Path to pgdog.toml
  PGDOG_USERS_CONFIG    Path to users.toml
  PGDOG_LOG_LEVEL       Log level (trace, debug, info, warn, error)
  PGDOG_HOST            Listen address
  PGDOG_PORT            PostgreSQL port
  PGDOG_ADMIN_PORT      Admin interface port
  PGDOG_METRICS_PORT    Prometheus metrics port
  TLS_ENABLED           Enable TLS (true/false)
  TLS_CERT_PATH         Path to TLS certificate
  TLS_KEY_PATH          Path to TLS private key
  TLS_CA_PATH           Path to CA certificate

Examples:
  entrypoint.sh                  Start PgDog with default config
  entrypoint.sh validate         Validate configuration
  entrypoint.sh config-gen       Generate config from templates
  entrypoint.sh shell            Start interactive shell

EOF
}

main() {
    local command="${1:-pgdog}"
    shift || true

    case "${command}" in
        pgdog|start|run)
            setup_directories
            validate_binary
            validate_config
            setup_tls
            validate_tls
            start_pgdog
            ;;
        validate)
            validate_binary
            validate_config
            validate_tls
            log_info "All validations passed"
            ;;
        config-gen|generate)
            generate_config
            generate_users_config
            ;;
        shell|bash)
            exec /bin/bash "$@"
            ;;
        help|--help|-h)
            print_help
            ;;
        *)
            # Pass through to exec
            exec "$command" "$@"
            ;;
    esac
}

# ==============================================================================
# Entry Point
# ==============================================================================

main "$@"
