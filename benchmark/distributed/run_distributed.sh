#!/bin/bash
#
# Distributed Benchmark Runner for Orochi DB
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default configuration
SCALE_FACTOR=1
NUM_SHARDS=8
NUM_CLIENTS=4
DURATION=30
FORMAT="json"
OUTPUT_DIR="../results/distributed"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

usage() {
    echo "Distributed Benchmark Runner"
    echo ""
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --scale=N       Scale factor [default: 1]"
    echo "  --shards=N      Number of shards [default: 8]"
    echo "  --clients=N     Number of concurrent clients [default: 4]"
    echo "  --duration=N    Throughput test duration [default: 30]"
    echo "  --format=FMT    Output format (json, csv) [default: json]"
    echo "  --output=DIR    Output directory"
    echo "  --host=HOST     PostgreSQL host [default: localhost]"
    echo "  --port=PORT     PostgreSQL port [default: 5432]"
    echo "  --user=USER     PostgreSQL user [default: postgres]"
    echo "  --database=DB   Database name [default: orochi_bench]"
    echo "  --help          Show this help"
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --scale=*) SCALE_FACTOR="${1#*=}" ;;
        --shards=*) NUM_SHARDS="${1#*=}" ;;
        --clients=*) NUM_CLIENTS="${1#*=}" ;;
        --duration=*) DURATION="${1#*=}" ;;
        --format=*) FORMAT="${1#*=}" ;;
        --output=*) OUTPUT_DIR="${1#*=}" ;;
        --host=*) PGHOST="${1#*=}" ;;
        --port=*) PGPORT="${1#*=}" ;;
        --user=*) PGUSER="${1#*=}" ;;
        --database=*) PGDATABASE="${1#*=}" ;;
        --help) usage ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

mkdir -p "$OUTPUT_DIR"

echo "=========================================="
echo "  Distributed Benchmark for Orochi DB"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Scale Factor: $SCALE_FACTOR"
echo "  Shards:       $NUM_SHARDS"
echo "  Clients:      $NUM_CLIENTS"
echo "  Duration:     ${DURATION}s"
echo "  Database:     $PGHOST:$PGPORT/$PGDATABASE"
echo ""

# Check database connection
if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &>/dev/null; then
    echo "ERROR: Cannot connect to database"
    exit 1
fi

# Create schema
echo "Creating distributed benchmark schema..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -f schema.sql 2>/dev/null || true

RESULTS_FILE="$OUTPUT_DIR/distributed_results.${FORMAT}"
CUSTOMERS=$((SCALE_FACTOR * 10000))
ORDERS=$((SCALE_FACTOR * 100000))

# Check if native benchmark exists
if [[ -f "./distributed_bench" ]]; then
    echo "Running native benchmark..."
    ./distributed_bench \
        --mode=all \
        --scale="$SCALE_FACTOR" \
        --shards="$NUM_SHARDS" \
        --clients="$NUM_CLIENTS" \
        --duration="$DURATION" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$OUTPUT_DIR"
else
    echo "Native benchmark not built, using SQL-only mode..."

    # Populate data
    echo ""
    echo "Populating test data..."

    # Check if data exists
    EXISTING=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT COUNT(*) FROM customers_distributed;" 2>/dev/null | tr -d ' ')

    if [[ "$EXISTING" -lt "$CUSTOMERS" ]]; then
        echo "  Inserting $CUSTOMERS customers..."
        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
        INSERT INTO customers_distributed (name, email, region)
        SELECT 'Customer ' || g, 'customer' || g || '@example.com',
               (ARRAY['us-east','us-west','eu-west','ap-south'])[1 + (random() * 3)::int]
        FROM generate_series(1, $CUSTOMERS) g;" 2>/dev/null

        echo "  Inserting orders..."
        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
        INSERT INTO orders_distributed (customer_id, order_date, status, total_amount)
        SELECT c.customer_id, NOW() - (random() * INTERVAL '365 days'),
               (ARRAY['pending','processing','shipped','delivered'])[1 + (random() * 3)::int],
               (random() * 1000)::decimal(12,2)
        FROM customers_distributed c, generate_series(1, 10);" 2>/dev/null

        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "ANALYZE;" 2>/dev/null
    fi

    echo ""
    echo "Running cross-shard queries..."

    declare -A RESULTS

    # Query 1: Aggregate all orders
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT COUNT(*), SUM(total_amount) FROM orders_distributed;" &>/dev/null
    END_TIME=$(date +%s%N)
    Q1_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Aggregate all orders: ${Q1_MS}ms"

    # Query 2: Join customers and orders
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT c.region, COUNT(*), SUM(o.total_amount)
    FROM customers_distributed c
    JOIN orders_distributed o ON c.customer_id = o.customer_id
    GROUP BY c.region;" &>/dev/null
    END_TIME=$(date +%s%N)
    Q2_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Join customers/orders: ${Q2_MS}ms"

    # Query 3: Top customers
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT c.customer_id, SUM(o.total_amount) as total
    FROM customers_distributed c
    JOIN orders_distributed o ON c.customer_id = o.customer_id
    GROUP BY c.customer_id
    ORDER BY total DESC LIMIT 100;" &>/dev/null
    END_TIME=$(date +%s%N)
    Q3_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Top customers: ${Q3_MS}ms"

    echo ""
    echo "Running local shard queries..."

    # Query 4: Single customer orders
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT * FROM orders_distributed WHERE customer_id = 1 ORDER BY order_date DESC;" &>/dev/null
    END_TIME=$(date +%s%N)
    Q4_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Single customer orders: ${Q4_MS}ms"

    # Save results
    if [[ "$FORMAT" == "json" ]]; then
        cat > "$RESULTS_FILE" << EOF
{
  "benchmark": "Distributed",
  "scale_factor": $SCALE_FACTOR,
  "shards": $NUM_SHARDS,
  "customers": $CUSTOMERS,
  "timestamp": "$(date -Iseconds)",
  "cross_shard_queries": {
    "aggregate_all_orders_ms": $Q1_MS,
    "join_customers_orders_ms": $Q2_MS,
    "top_customers_ms": $Q3_MS
  },
  "local_queries": {
    "single_customer_orders_ms": $Q4_MS
  }
}
EOF
    else
        cat > "$RESULTS_FILE" << EOF
query_type,query_name,duration_ms
cross_shard,aggregate_all_orders,$Q1_MS
cross_shard,join_customers_orders,$Q2_MS
cross_shard,top_customers,$Q3_MS
local,single_customer_orders,$Q4_MS
EOF
    fi
fi

echo ""
echo "=========================================="
echo "Results saved to: $RESULTS_FILE"
echo "=========================================="
