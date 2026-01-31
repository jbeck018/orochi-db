#!/bin/bash
#
# Multi-Tenant Data Isolation Verification Tests
#
# This script verifies that tenant data isolation is properly enforced:
# 1. Cross-tenant data access (should fail)
# 2. Row-Level Security (RLS) enforcement
# 3. Schema-based isolation verification
#
# Usage: ./isolation_test.sh [options]
#

set -e

# Default configuration
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGPASSWORD="${PGPASSWORD:-}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
VERBOSE="${VERBOSE:-0}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat << EOF
Multi-Tenant Data Isolation Verification Tests

Usage: $0 [options]

Options:
  -h, --host HOST           PostgreSQL host [${PGHOST}]
  -p, --port PORT           PostgreSQL port [${PGPORT}]
  -U, --user USER           PostgreSQL user [${PGUSER}]
  -d, --database DB         Database name [${PGDATABASE}]
  -v, --verbose             Enable verbose output
      --setup               Setup test schema before running tests
      --cleanup             Cleanup test data after tests
      --help                Show this help

Tests performed:
  1. RLS Policy Enforcement - Verifies tenants cannot see other tenants' data
  2. Cross-Tenant Access Denial - Attempts direct cross-tenant data access
  3. Schema Isolation - Verifies per-schema isolation boundaries
  4. Privilege Escalation - Tests for privilege escalation vulnerabilities
  5. Connection Context - Verifies tenant context is properly scoped

EOF
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_verbose() {
    if [[ "${VERBOSE}" == "1" ]]; then
        echo -e "       $1"
    fi
}

run_sql() {
    local sql="$1"
    local expect_fail="${2:-0}"

    log_verbose "SQL: ${sql}"

    if [[ "${expect_fail}" == "1" ]]; then
        # Expect this to fail
        if PGPASSWORD="${PGPASSWORD}" psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -d "${PGDATABASE}" -c "${sql}" 2>&1 | grep -qi "error\|denied\|permission"; then
            return 0  # Failed as expected
        else
            return 1  # Unexpectedly succeeded
        fi
    else
        PGPASSWORD="${PGPASSWORD}" psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -d "${PGDATABASE}" -c "${sql}" 2>&1
    fi
}

run_sql_count() {
    local sql="$1"
    PGPASSWORD="${PGPASSWORD}" psql -h "${PGHOST}" -p "${PGPORT}" -U "${PGUSER}" -d "${PGDATABASE}" -t -c "${sql}" 2>/dev/null | tr -d ' '
}

test_result() {
    local test_name="$1"
    local result="$2"

    TESTS_RUN=$((TESTS_RUN + 1))

    if [[ "${result}" == "0" ]]; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
        log_success "${test_name}"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        log_fail "${test_name}"
    fi
}

setup_test_schema() {
    log_info "Setting up test schema..."

    run_sql "DROP TABLE IF EXISTS tenant_data CASCADE;"
    run_sql "DROP TABLE IF EXISTS tenants CASCADE;"
    run_sql "DROP ROLE IF EXISTS tenant1_user;"
    run_sql "DROP ROLE IF EXISTS tenant2_user;"

    # Create tenant management table
    run_sql "
    CREATE TABLE tenants (
        tenant_id SERIAL PRIMARY KEY,
        tenant_name VARCHAR(100) NOT NULL UNIQUE,
        created_at TIMESTAMP DEFAULT NOW()
    );"

    # Create shared tenant data table
    run_sql "
    CREATE TABLE tenant_data (
        id BIGSERIAL,
        tenant_id INTEGER NOT NULL REFERENCES tenants(tenant_id),
        key VARCHAR(100) NOT NULL,
        value TEXT,
        secret_data TEXT,
        PRIMARY KEY (tenant_id, id)
    );"

    run_sql "CREATE INDEX idx_tenant_data_tenant ON tenant_data(tenant_id);"

    # Create tenants
    run_sql "INSERT INTO tenants (tenant_name) VALUES ('tenant_1'), ('tenant_2'), ('tenant_3');"

    # Insert test data for each tenant
    run_sql "
    INSERT INTO tenant_data (tenant_id, key, value, secret_data)
    SELECT 1, 'key_' || i, 'tenant1_value_' || i, 'tenant1_secret_' || i
    FROM generate_series(1, 100) i;"

    run_sql "
    INSERT INTO tenant_data (tenant_id, key, value, secret_data)
    SELECT 2, 'key_' || i, 'tenant2_value_' || i, 'tenant2_secret_' || i
    FROM generate_series(1, 100) i;"

    run_sql "
    INSERT INTO tenant_data (tenant_id, key, value, secret_data)
    SELECT 3, 'key_' || i, 'tenant3_value_' || i, 'tenant3_secret_' || i
    FROM generate_series(1, 100) i;"

    # Enable Row Level Security
    run_sql "ALTER TABLE tenant_data ENABLE ROW LEVEL SECURITY;"

    # Create RLS policy
    run_sql "
    CREATE POLICY tenant_isolation_policy ON tenant_data
        FOR ALL
        USING (tenant_id = current_setting('app.current_tenant_id', true)::INTEGER)
        WITH CHECK (tenant_id = current_setting('app.current_tenant_id', true)::INTEGER);"

    # Create tenant-specific roles
    run_sql "CREATE ROLE tenant1_user LOGIN PASSWORD 'tenant1pass';"
    run_sql "CREATE ROLE tenant2_user LOGIN PASSWORD 'tenant2pass';"

    run_sql "GRANT USAGE ON SCHEMA public TO tenant1_user, tenant2_user;"
    run_sql "GRANT SELECT, INSERT, UPDATE, DELETE ON tenant_data TO tenant1_user, tenant2_user;"
    run_sql "GRANT SELECT ON tenants TO tenant1_user, tenant2_user;"
    run_sql "GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO tenant1_user, tenant2_user;"

    # Create per-tenant schemas for schema isolation tests
    run_sql "DROP SCHEMA IF EXISTS tenant_schema_1 CASCADE;"
    run_sql "DROP SCHEMA IF EXISTS tenant_schema_2 CASCADE;"

    run_sql "CREATE SCHEMA tenant_schema_1;"
    run_sql "CREATE SCHEMA tenant_schema_2;"

    run_sql "
    CREATE TABLE tenant_schema_1.data (
        id SERIAL PRIMARY KEY,
        key VARCHAR(100),
        value TEXT,
        secret_data TEXT
    );"

    run_sql "
    CREATE TABLE tenant_schema_2.data (
        id SERIAL PRIMARY KEY,
        key VARCHAR(100),
        value TEXT,
        secret_data TEXT
    );"

    run_sql "INSERT INTO tenant_schema_1.data (key, value, secret_data) VALUES ('key1', 'schema1_value', 'schema1_secret');"
    run_sql "INSERT INTO tenant_schema_2.data (key, value, secret_data) VALUES ('key1', 'schema2_value', 'schema2_secret');"

    run_sql "GRANT USAGE ON SCHEMA tenant_schema_1 TO tenant1_user;"
    run_sql "GRANT ALL ON ALL TABLES IN SCHEMA tenant_schema_1 TO tenant1_user;"

    run_sql "GRANT USAGE ON SCHEMA tenant_schema_2 TO tenant2_user;"
    run_sql "GRANT ALL ON ALL TABLES IN SCHEMA tenant_schema_2 TO tenant2_user;"

    log_success "Test schema setup complete"
}

cleanup_test_data() {
    log_info "Cleaning up test data..."

    run_sql "DROP TABLE IF EXISTS tenant_data CASCADE;"
    run_sql "DROP TABLE IF EXISTS tenants CASCADE;"
    run_sql "DROP SCHEMA IF EXISTS tenant_schema_1 CASCADE;"
    run_sql "DROP SCHEMA IF EXISTS tenant_schema_2 CASCADE;"
    run_sql "DROP ROLE IF EXISTS tenant1_user;"
    run_sql "DROP ROLE IF EXISTS tenant2_user;"

    log_success "Test data cleanup complete"
}

# =============================================================================
# Test Cases
# =============================================================================

test_rls_no_context() {
    log_info "Test: RLS without tenant context"

    # Without setting tenant context, should return no rows
    local count=$(run_sql_count "SELECT COUNT(*) FROM tenant_data;")

    if [[ "${count}" == "0" ]] || [[ -z "${count}" ]]; then
        test_result "RLS blocks access without tenant context" 0
    else
        log_verbose "Expected 0 rows, got ${count}"
        test_result "RLS blocks access without tenant context" 1
    fi
}

test_rls_with_context() {
    log_info "Test: RLS with tenant context"

    # Set tenant context to tenant 1
    local count=$(run_sql_count "
        SET app.current_tenant_id = '1';
        SELECT COUNT(*) FROM tenant_data;
    ")

    if [[ "${count}" == "100" ]]; then
        test_result "RLS allows access with valid tenant context (count=100)" 0
    else
        log_verbose "Expected 100 rows, got ${count}"
        test_result "RLS allows access with valid tenant context" 1
    fi
}

test_rls_cross_tenant_read() {
    log_info "Test: RLS cross-tenant read prevention"

    # Set tenant context to tenant 1, try to read tenant 2 data
    local count=$(run_sql_count "
        SET app.current_tenant_id = '1';
        SELECT COUNT(*) FROM tenant_data WHERE tenant_id = 2;
    ")

    if [[ "${count}" == "0" ]] || [[ -z "${count}" ]]; then
        test_result "RLS prevents cross-tenant read (tenant 1 -> tenant 2)" 0
    else
        log_verbose "Expected 0 rows, got ${count}"
        test_result "RLS prevents cross-tenant read" 1
    fi
}

test_rls_cross_tenant_write() {
    log_info "Test: RLS cross-tenant write prevention"

    # Set tenant context to tenant 1, try to insert into tenant 2
    run_sql "SET app.current_tenant_id = '1';"

    if run_sql "INSERT INTO tenant_data (tenant_id, key, value) VALUES (2, 'hack', 'hacked');" 1; then
        test_result "RLS prevents cross-tenant write (tenant 1 -> tenant 2)" 0
    else
        test_result "RLS prevents cross-tenant write" 1
    fi

    # Cleanup any accidentally inserted rows
    run_sql "DELETE FROM tenant_data WHERE key = 'hack';" 2>/dev/null || true
}

test_rls_cross_tenant_update() {
    log_info "Test: RLS cross-tenant update prevention"

    # Set tenant context to tenant 1, try to update tenant 2 data
    local updated=$(run_sql_count "
        SET app.current_tenant_id = '1';
        UPDATE tenant_data SET value = 'hacked' WHERE tenant_id = 2;
        SELECT COUNT(*) FROM tenant_data WHERE value = 'hacked';
    ")

    if [[ "${updated}" == "0" ]] || [[ -z "${updated}" ]]; then
        test_result "RLS prevents cross-tenant update" 0
    else
        log_verbose "Unexpectedly updated ${updated} rows"
        test_result "RLS prevents cross-tenant update" 1
    fi
}

test_rls_cross_tenant_delete() {
    log_info "Test: RLS cross-tenant delete prevention"

    # Count tenant 2 rows before attempt
    local before=$(run_sql_count "SELECT COUNT(*) FROM tenant_data WHERE tenant_id = 2;")

    # Set tenant context to tenant 1, try to delete tenant 2 data
    run_sql "
        SET app.current_tenant_id = '1';
        DELETE FROM tenant_data WHERE tenant_id = 2;
    " 2>/dev/null || true

    # Count tenant 2 rows after attempt (as superuser)
    run_sql "RESET app.current_tenant_id;"
    local after=$(run_sql_count "SELECT COUNT(*) FROM tenant_data WHERE tenant_id = 2;")

    if [[ "${before}" == "${after}" ]]; then
        test_result "RLS prevents cross-tenant delete (before=${before}, after=${after})" 0
    else
        log_verbose "Rows changed: ${before} -> ${after}"
        test_result "RLS prevents cross-tenant delete" 1
    fi
}

test_schema_isolation_access() {
    log_info "Test: Schema isolation - authorized access"

    # Tenant 1 user should access tenant_schema_1
    local result=$(PGPASSWORD="tenant1pass" psql -h "${PGHOST}" -p "${PGPORT}" -U "tenant1_user" -d "${PGDATABASE}" -t -c "SELECT value FROM tenant_schema_1.data WHERE key = 'key1';" 2>/dev/null | tr -d ' ')

    if [[ "${result}" == "schema1_value" ]]; then
        test_result "Schema isolation allows authorized access" 0
    else
        log_verbose "Expected 'schema1_value', got '${result}'"
        test_result "Schema isolation allows authorized access" 1
    fi
}

test_schema_isolation_denial() {
    log_info "Test: Schema isolation - unauthorized access"

    # Tenant 1 user should NOT access tenant_schema_2
    if PGPASSWORD="tenant1pass" psql -h "${PGHOST}" -p "${PGPORT}" -U "tenant1_user" -d "${PGDATABASE}" -c "SELECT * FROM tenant_schema_2.data;" 2>&1 | grep -qi "permission denied\|does not exist"; then
        test_result "Schema isolation denies unauthorized access" 0
    else
        test_result "Schema isolation denies unauthorized access" 1
    fi
}

test_tenant_context_isolation() {
    log_info "Test: Tenant context isolation between sessions"

    # Start two parallel sessions with different tenant contexts
    # Verify they don't interfere

    # Session 1: Set tenant 1
    local session1_count=$(run_sql_count "
        SET app.current_tenant_id = '1';
        SELECT COUNT(*) FROM tenant_data;
    ")

    # Session 2: Set tenant 2 (simulated as separate query)
    local session2_count=$(run_sql_count "
        SET app.current_tenant_id = '2';
        SELECT COUNT(*) FROM tenant_data;
    ")

    if [[ "${session1_count}" == "100" ]] && [[ "${session2_count}" == "100" ]]; then
        test_result "Tenant context isolation between sessions" 0
    else
        log_verbose "Session 1 count: ${session1_count}, Session 2 count: ${session2_count}"
        test_result "Tenant context isolation between sessions" 1
    fi
}

test_rls_policy_bypass_attempt() {
    log_info "Test: RLS policy bypass attempts"

    local bypass_attempted=0

    # Attempt 1: Direct table access with subquery
    local count1=$(run_sql_count "
        SET app.current_tenant_id = '1';
        SELECT COUNT(*) FROM (SELECT * FROM tenant_data WHERE tenant_id IN (1,2)) sub;
    ")
    if [[ "${count1}" != "100" ]] && [[ "${count1}" != "0" ]]; then
        bypass_attempted=1
        log_verbose "Subquery bypass returned ${count1} rows"
    fi

    # Attempt 2: UNION attack
    local count2=$(run_sql_count "
        SET app.current_tenant_id = '1';
        SELECT COUNT(*) FROM (
            SELECT * FROM tenant_data WHERE tenant_id = 1
            UNION ALL
            SELECT * FROM tenant_data WHERE tenant_id = 2
        ) combined;
    ")
    if [[ "${count2}" != "100" ]]; then
        bypass_attempted=1
        log_verbose "UNION bypass returned ${count2} rows"
    fi

    if [[ "${bypass_attempted}" == "0" ]]; then
        test_result "RLS policy resists bypass attempts" 0
    else
        test_result "RLS policy resists bypass attempts" 1
    fi
}

test_information_leakage() {
    log_info "Test: Information leakage via error messages"

    # Try to cause an error that might reveal other tenant's data
    local error_output=$(run_sql "
        SET app.current_tenant_id = '1';
        SELECT * FROM tenant_data WHERE key = 'key_1' AND secret_data = (
            SELECT secret_data FROM tenant_data WHERE tenant_id = 2 LIMIT 1
        );
    " 2>&1)

    if echo "${error_output}" | grep -qi "tenant2_secret"; then
        log_verbose "Error message contains tenant 2 secret data"
        test_result "No information leakage via error messages" 1
    else
        test_result "No information leakage via error messages" 0
    fi
}

test_tenant_data_counts() {
    log_info "Test: Tenant data count verification"

    local all_correct=1

    for tenant_id in 1 2 3; do
        local count=$(run_sql_count "
            SET app.current_tenant_id = '${tenant_id}';
            SELECT COUNT(*) FROM tenant_data;
        ")

        if [[ "${count}" != "100" ]]; then
            log_verbose "Tenant ${tenant_id} has ${count} rows (expected 100)"
            all_correct=0
        fi
    done

    test_result "Each tenant sees exactly their own data (100 rows each)" ${all_correct}
}

# =============================================================================
# Main
# =============================================================================

main() {
    echo ""
    echo "=========================================="
    echo "  Multi-Tenant Isolation Tests"
    echo "=========================================="
    echo ""
    echo "Database: ${PGHOST}:${PGPORT}/${PGDATABASE}"
    echo ""

    # Parse arguments
    local do_setup=0
    local do_cleanup=0

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
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            --setup)
                do_setup=1
                shift
                ;;
            --cleanup)
                do_cleanup=1
                shift
                ;;
            --help)
                usage
                exit 0
                ;;
            *)
                log_warn "Unknown option: $1"
                shift
                ;;
        esac
    done

    # Setup if requested
    if [[ "${do_setup}" == "1" ]]; then
        setup_test_schema
    fi

    echo ""
    log_info "Running isolation tests..."
    echo ""

    # Run all tests
    test_rls_no_context
    test_rls_with_context
    test_rls_cross_tenant_read
    test_rls_cross_tenant_write
    test_rls_cross_tenant_update
    test_rls_cross_tenant_delete
    test_schema_isolation_access
    test_schema_isolation_denial
    test_tenant_context_isolation
    test_rls_policy_bypass_attempt
    test_information_leakage
    test_tenant_data_counts

    # Cleanup if requested
    if [[ "${do_cleanup}" == "1" ]]; then
        echo ""
        cleanup_test_data
    fi

    # Summary
    echo ""
    echo "=========================================="
    echo "  Test Summary"
    echo "=========================================="
    echo ""
    echo "  Total Tests:  ${TESTS_RUN}"
    echo -e "  Passed:       ${GREEN}${TESTS_PASSED}${NC}"
    echo -e "  Failed:       ${RED}${TESTS_FAILED}${NC}"
    echo ""

    if [[ "${TESTS_FAILED}" -gt 0 ]]; then
        log_fail "Some isolation tests failed!"
        exit 1
    else
        log_success "All isolation tests passed!"
        exit 0
    fi
}

main "$@"
