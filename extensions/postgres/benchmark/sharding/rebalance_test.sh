#!/bin/bash
#
# Orochi DB - Rebalancing Overhead Test
#
# Measures the performance impact of shard rebalancing:
# - Establishes baseline throughput
# - Triggers shard rebalancing
# - Measures continuous throughput during migration
# - Reports performance drop percentage
#
# Target: Performance drop < 50% during rebalancing
#

set -e

# Configuration
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
PGBENCH="${PGBENCH:-pgbench}"

# Test parameters
BASELINE_DURATION="${BASELINE_DURATION:-30}"
REBALANCE_DURATION="${REBALANCE_DURATION:-60}"
RECOVERY_DURATION="${RECOVERY_DURATION:-30}"
NUM_CLIENTS="${NUM_CLIENTS:-4}"
NUM_THREADS="${NUM_THREADS:-2}"
SCALE_FACTOR="${SCALE_FACTOR:-1}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Output directory
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${RESULTS_DIR:-$SCRIPT_DIR/../results/sharding}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

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

print_result() {
    if [ "$2" = "PASS" ]; then
        echo -e "  $1: ${GREEN}$3${NC}"
    elif [ "$2" = "WARN" ]; then
        echo -e "  $1: ${YELLOW}$3${NC}"
    else
        echo -e "  $1: ${RED}$3${NC}"
    fi
}

# SQL helper
sql_query() {
    PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -A -c "$1"
}

sql_exec() {
    PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "$1"
}

# Create test schema
setup_test_schema() {
    print_step "Setting up test schema..."

    sql_exec "DROP TABLE IF EXISTS rebalance_test_orders CASCADE"
    sql_exec "DROP TABLE IF EXISTS rebalance_test_customers CASCADE"

    sql_exec "
        CREATE TABLE rebalance_test_customers (
            customer_id BIGSERIAL PRIMARY KEY,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(100) NOT NULL,
            region VARCHAR(32) NOT NULL,
            balance DECIMAL(12,2) DEFAULT 0,
            created_at TIMESTAMPTZ DEFAULT NOW()
        )
    "

    sql_exec "
        CREATE TABLE rebalance_test_orders (
            order_id BIGSERIAL,
            customer_id BIGINT NOT NULL,
            order_date TIMESTAMPTZ DEFAULT NOW(),
            status VARCHAR(20) DEFAULT 'pending',
            total_amount DECIMAL(12,2) NOT NULL,
            PRIMARY KEY (customer_id, order_id)
        )
    "

    # Create indexes
    sql_exec "CREATE INDEX idx_rebalance_orders_date ON rebalance_test_orders(order_date)"
    sql_exec "CREATE INDEX idx_rebalance_orders_status ON rebalance_test_orders(status)"

    # Distribute tables with 8 shards
    sql_exec "SELECT orochi_create_distributed_table('rebalance_test_customers'::regclass, 'customer_id', 0, 8)" 2>/dev/null || true
    sql_exec "SELECT orochi_create_distributed_table('rebalance_test_orders'::regclass, 'customer_id', 0, 8)" 2>/dev/null || true

    echo "Schema created"
}

# Populate test data
populate_test_data() {
    local num_customers=$((10000 * SCALE_FACTOR))
    local orders_per_customer=10

    print_step "Populating test data ($num_customers customers, $((num_customers * orders_per_customer)) orders)..."

    sql_exec "
        INSERT INTO rebalance_test_customers (name, email, region, balance)
        SELECT
            'Customer ' || g,
            'customer' || g || '@example.com',
            (ARRAY['us-east','us-west','eu-west','eu-east','ap-south','ap-north'])[1 + (random() * 5)::int],
            (random() * 10000)::decimal(12,2)
        FROM generate_series(1, $num_customers) g
    "

    sql_exec "
        INSERT INTO rebalance_test_orders (customer_id, order_date, status, total_amount)
        SELECT
            c.customer_id,
            NOW() - (random() * INTERVAL '365 days'),
            (ARRAY['pending','processing','shipped','delivered','cancelled'])[1 + (random() * 4)::int],
            (random() * 1000)::decimal(12,2)
        FROM rebalance_test_customers c,
             generate_series(1, $orders_per_customer)
    "

    sql_exec "ANALYZE rebalance_test_customers"
    sql_exec "ANALYZE rebalance_test_orders"

    echo "Data populated"
}

# Create pgbench custom script
create_benchmark_script() {
    local script_file="$RESULTS_DIR/rebalance_workload.sql"

    mkdir -p "$RESULTS_DIR"

    cat > "$script_file" << 'EOF'
\set customer_id random(1, 10000)
\set amount random(1, 1000)

-- Mix of local and cross-shard operations
BEGIN;

-- Local read
SELECT * FROM rebalance_test_orders WHERE customer_id = :customer_id LIMIT 10;

-- Local write
UPDATE rebalance_test_customers SET balance = balance + :amount WHERE customer_id = :customer_id;

-- Insert
INSERT INTO rebalance_test_orders (customer_id, total_amount) VALUES (:customer_id, :amount);

COMMIT;
EOF

    echo "$script_file"
}

# Run throughput measurement
measure_throughput() {
    local phase="$1"
    local duration="$2"
    local output_file="$RESULTS_DIR/rebalance_${phase}_${TIMESTAMP}.log"

    print_step "Measuring throughput: $phase (${duration}s)..."

    # Run pgbench
    local script_file=$(create_benchmark_script)

    PGPASSWORD="$PGPASSWORD" $PGBENCH \
        -h "$PGHOST" \
        -p "$PGPORT" \
        -U "$PGUSER" \
        -d "$PGDATABASE" \
        -c "$NUM_CLIENTS" \
        -j "$NUM_THREADS" \
        -T "$duration" \
        -f "$script_file" \
        -P 5 \
        --no-vacuum \
        2>&1 | tee "$output_file"

    # Extract TPS from output
    local tps=$(grep -E "^tps = [0-9.]+ \(excluding connections" "$output_file" | awk '{print $3}')

    if [ -z "$tps" ]; then
        # Try alternative format
        tps=$(grep -E "number of transactions actually processed:" "$output_file" | awk '{print $NF}')
        tps=$(echo "scale=2; $tps / $duration" | bc -l)
    fi

    echo "$tps"
}

# Trigger rebalancing in background
trigger_rebalancing() {
    print_step "Triggering shard rebalancing..."

    # Start rebalancing in background
    (
        sleep 5  # Wait for workload to stabilize

        echo "Starting rebalance at $(date)"

        # Rebalance customers table
        sql_exec "SELECT orochi_rebalance_table('rebalance_test_customers'::regclass)" 2>/dev/null || true

        # Rebalance orders table
        sql_exec "SELECT orochi_rebalance_table('rebalance_test_orders'::regclass)" 2>/dev/null || true

        echo "Rebalance completed at $(date)"
    ) &

    echo $!  # Return background PID
}

# Get shard distribution stats
get_shard_stats() {
    local table_name="$1"

    sql_query "
        SELECT
            s.shard_index,
            COALESCE(s.row_count, 0) as rows,
            COALESCE(s.node_id, 0) as node
        FROM orochi.orochi_shards s
        JOIN orochi.orochi_tables t ON s.table_oid = t.relid
        WHERE t.table_name = '$table_name'
        ORDER BY s.shard_index
    " 2>/dev/null || echo ""
}

# Monitor rebalancing progress
monitor_rebalancing() {
    print_step "Monitoring rebalancing progress..."

    while true; do
        local moves=$(sql_query "
            SELECT COUNT(*)
            FROM orochi.orochi_shard_moves
            WHERE status IN (0, 1)
        " 2>/dev/null || echo "0")

        if [ "$moves" = "0" ] || [ -z "$moves" ]; then
            echo "No active shard moves"
            break
        fi

        echo "Active shard moves: $moves"
        sleep 5
    done
}

# Run the rebalancing overhead test
run_rebalance_test() {
    print_header "Rebalancing Overhead Test"

    echo "Configuration:"
    echo "  Database:          $PGDATABASE on $PGHOST:$PGPORT"
    echo "  Clients:           $NUM_CLIENTS"
    echo "  Threads:           $NUM_THREADS"
    echo "  Baseline duration: ${BASELINE_DURATION}s"
    echo "  Rebalance window:  ${REBALANCE_DURATION}s"
    echo "  Recovery duration: ${RECOVERY_DURATION}s"
    echo ""

    # Setup
    setup_test_schema
    populate_test_data

    # Show initial distribution
    echo ""
    print_step "Initial shard distribution:"
    get_shard_stats "rebalance_test_customers"

    # Phase 1: Baseline measurement
    print_header "Phase 1: Baseline Measurement"
    local baseline_tps=$(measure_throughput "baseline" "$BASELINE_DURATION")
    echo ""
    echo "Baseline TPS: $baseline_tps"

    # Phase 2: During rebalancing
    print_header "Phase 2: Rebalancing (with concurrent workload)"

    # Start rebalancing in background
    local rebalance_pid=$(trigger_rebalancing)

    # Measure throughput during rebalancing
    local rebalance_tps=$(measure_throughput "during_rebalance" "$REBALANCE_DURATION")

    # Wait for rebalancing to complete
    wait $rebalance_pid 2>/dev/null || true

    echo ""
    echo "During-Rebalance TPS: $rebalance_tps"

    # Phase 3: Post-rebalance recovery
    print_header "Phase 3: Post-Rebalance Recovery"

    # Wait a moment for things to settle
    sleep 5

    local recovery_tps=$(measure_throughput "recovery" "$RECOVERY_DURATION")
    echo ""
    echo "Recovery TPS: $recovery_tps"

    # Show final distribution
    echo ""
    print_step "Final shard distribution:"
    get_shard_stats "rebalance_test_customers"

    # Calculate metrics
    print_header "Results Summary"

    local performance_drop=0
    local recovery_percent=100

    if [ -n "$baseline_tps" ] && [ "$baseline_tps" != "0" ]; then
        performance_drop=$(echo "scale=2; (1 - $rebalance_tps / $baseline_tps) * 100" | bc -l)
        recovery_percent=$(echo "scale=2; ($recovery_tps / $baseline_tps) * 100" | bc -l)
    fi

    echo "Phase                    TPS"
    echo "----------------------------------------"
    printf "Baseline:                %.2f\n" "$baseline_tps"
    printf "During Rebalance:        %.2f\n" "$rebalance_tps"
    printf "Post-Rebalance:          %.2f\n" "$recovery_tps"
    echo "----------------------------------------"
    printf "Performance Drop:        %.2f%%\n" "$performance_drop"
    printf "Recovery vs Baseline:    %.2f%%\n" "$recovery_percent"
    echo ""

    # Evaluate results
    local drop_status="FAIL"
    local drop_msg="POOR - significant impact"

    if [ "$(echo "$performance_drop < 20" | bc -l)" = "1" ]; then
        drop_status="PASS"
        drop_msg="EXCELLENT - minimal impact"
    elif [ "$(echo "$performance_drop < 35" | bc -l)" = "1" ]; then
        drop_status="PASS"
        drop_msg="GOOD - acceptable impact"
    elif [ "$(echo "$performance_drop < 50" | bc -l)" = "1" ]; then
        drop_status="WARN"
        drop_msg="ACCEPTABLE - moderate impact"
    fi

    print_result "Performance Impact" "$drop_status" "$drop_msg"

    local recovery_status="PASS"
    local recovery_msg="GOOD"

    if [ "$(echo "$recovery_percent < 90" | bc -l)" = "1" ]; then
        recovery_status="WARN"
        recovery_msg="Recovery incomplete"
    fi
    if [ "$(echo "$recovery_percent < 80" | bc -l)" = "1" ]; then
        recovery_status="FAIL"
        recovery_msg="Poor recovery"
    fi

    print_result "Post-Rebalance Recovery" "$recovery_status" "$recovery_msg"

    # Save results to JSON
    local results_file="$RESULTS_DIR/rebalance_results_${TIMESTAMP}.json"
    cat > "$results_file" << EOF
{
    "test": "rebalancing_overhead",
    "timestamp": "$(date -Iseconds)",
    "config": {
        "database": "$PGDATABASE",
        "host": "$PGHOST",
        "port": $PGPORT,
        "clients": $NUM_CLIENTS,
        "threads": $NUM_THREADS,
        "baseline_duration": $BASELINE_DURATION,
        "rebalance_duration": $REBALANCE_DURATION,
        "recovery_duration": $RECOVERY_DURATION
    },
    "results": {
        "baseline_tps": $baseline_tps,
        "rebalance_tps": $rebalance_tps,
        "recovery_tps": $recovery_tps,
        "performance_drop_percent": $performance_drop,
        "recovery_percent": $recovery_percent
    },
    "evaluation": {
        "performance_drop_acceptable": $([ "$drop_status" != "FAIL" ] && echo "true" || echo "false"),
        "recovery_acceptable": $([ "$recovery_status" != "FAIL" ] && echo "true" || echo "false")
    }
}
EOF

    echo ""
    print_step "Results saved to: $results_file"
}

# Quick test with shorter durations
run_quick_test() {
    BASELINE_DURATION=10
    REBALANCE_DURATION=20
    RECOVERY_DURATION=10
    run_rebalance_test
}

# Cleanup
cleanup() {
    print_step "Cleaning up test tables..."

    sql_exec "DROP TABLE IF EXISTS rebalance_test_orders CASCADE"
    sql_exec "DROP TABLE IF EXISTS rebalance_test_customers CASCADE"

    echo "Cleanup complete"
}

# Usage
usage() {
    cat << EOF
Orochi DB Rebalancing Overhead Test

Usage: $(basename "$0") [command]

Commands:
  run       Run full rebalancing overhead test (default)
  quick     Run quick test with shorter durations
  cleanup   Remove test tables
  help      Show this help

Environment Variables:
  PGHOST              Database host [default: localhost]
  PGPORT              Database port [default: 5432]
  PGUSER              Database user [default: postgres]
  PGDATABASE          Database name [default: orochi_bench]
  PGPASSWORD          Database password
  PGBENCH             Path to pgbench [default: pgbench]
  NUM_CLIENTS         Number of concurrent clients [default: 4]
  NUM_THREADS         Number of threads [default: 2]
  SCALE_FACTOR        Data scale factor [default: 1]
  BASELINE_DURATION   Baseline measurement duration [default: 30]
  REBALANCE_DURATION  Rebalance window duration [default: 60]
  RECOVERY_DURATION   Post-rebalance measurement [default: 30]
  RESULTS_DIR         Results output directory

Target Metrics:
  - Performance drop during rebalancing: < 50%
  - Post-rebalance recovery: > 90% of baseline

Example:
  # Run full test
  $(basename "$0") run

  # Run quick test
  $(basename "$0") quick

  # Run with custom parameters
  NUM_CLIENTS=8 BASELINE_DURATION=60 $(basename "$0") run
EOF
}

# Main entry point
main() {
    local command="${1:-run}"

    mkdir -p "$RESULTS_DIR"

    case "$command" in
        run)
            run_rebalance_test
            ;;
        quick)
            run_quick_test
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
