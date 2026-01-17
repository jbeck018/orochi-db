#!/bin/bash
#
# Time-Series Benchmark Runner for Orochi DB
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default configuration
SCALE_FACTOR=1
NUM_CLIENTS=1
BATCH_SIZE=1000
NUM_DEVICES=100
DURATION=60
FORMAT="json"
OUTPUT_DIR="../results/timeseries"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

usage() {
    echo "Time-Series Benchmark Runner"
    echo ""
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --scale=N       Scale factor [default: 1]"
    echo "  --clients=N     Number of concurrent clients [default: 1]"
    echo "  --batch=N       Batch size for inserts [default: 1000]"
    echo "  --devices=N     Number of simulated devices [default: 100]"
    echo "  --duration=N    Max duration in seconds [default: 60]"
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
        --clients=*) NUM_CLIENTS="${1#*=}" ;;
        --batch=*) BATCH_SIZE="${1#*=}" ;;
        --devices=*) NUM_DEVICES="${1#*=}" ;;
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
echo "  Time-Series Benchmark for Orochi DB"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Scale Factor: $SCALE_FACTOR"
echo "  Clients:      $NUM_CLIENTS"
echo "  Batch Size:   $BATCH_SIZE"
echo "  Devices:      $NUM_DEVICES"
echo "  Duration:     ${DURATION}s"
echo "  Database:     $PGHOST:$PGPORT/$PGDATABASE"
echo ""

# Check database connection
if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &>/dev/null; then
    echo "ERROR: Cannot connect to database"
    exit 1
fi

# Create schema
echo "Creating time-series schema..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -f schema.sql 2>/dev/null || true

RESULTS_FILE="$OUTPUT_DIR/timeseries_results.${FORMAT}"

# Check if native benchmark exists
if [[ -f "./ts_bench" ]]; then
    echo "Running native benchmark..."
    ./ts_bench \
        --mode=all \
        --scale="$SCALE_FACTOR" \
        --clients="$NUM_CLIENTS" \
        --batch="$BATCH_SIZE" \
        --devices="$NUM_DEVICES" \
        --duration="$DURATION" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$OUTPUT_DIR"
else
    echo "Native benchmark not built, using SQL-only mode..."

    # Simple SQL-based benchmark
    if [[ "$FORMAT" == "json" ]]; then
        echo "{" > "$RESULTS_FILE"
        echo "  \"benchmark\": \"TimeSeries\"," >> "$RESULTS_FILE"
        echo "  \"scale_factor\": $SCALE_FACTOR," >> "$RESULTS_FILE"
        echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$RESULTS_FILE"
        echo "  \"tests\": [" >> "$RESULTS_FILE"
    fi

    echo ""
    echo "Running INSERT benchmark..."

    # Generate test data using PostgreSQL
    START_TIME=$(date +%s%N)
    ROWS=$((SCALE_FACTOR * 100000))

    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
    INSERT INTO sensor_data (time, device_id, temperature, humidity, pressure, battery_level, signal_strength)
    SELECT
        NOW() - (random() * INTERVAL '30 days'),
        (random() * $NUM_DEVICES)::int + 1,
        20 + random() * 20,
        40 + random() * 40,
        1000 + random() * 50,
        70 + random() * 30,
        -90 + (random() * 60)::int
    FROM generate_series(1, $ROWS);" 2>/dev/null

    END_TIME=$(date +%s%N)
    INSERT_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    INSERT_RATE=$(echo "scale=2; $ROWS * 1000 / $INSERT_MS" | bc)

    echo "  INSERT: ${ROWS} rows in ${INSERT_MS}ms (${INSERT_RATE} rows/sec)"

    echo ""
    echo "Running query benchmarks..."

    # Query 1: Last hour aggregation
    START_TIME=$(date +%s%N)
    RESULT=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT COUNT(*), AVG(temperature) FROM sensor_data
    WHERE time > NOW() - INTERVAL '1 hour';" 2>/dev/null)
    END_TIME=$(date +%s%N)
    Q1_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Query (last hour): ${Q1_MS}ms"

    # Query 2: Daily aggregation
    START_TIME=$(date +%s%N)
    RESULT=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT date_trunc('day', time), device_id, COUNT(*), AVG(temperature)
    FROM sensor_data
    WHERE time > NOW() - INTERVAL '7 days'
    GROUP BY 1, 2 ORDER BY 1, 2;" 2>/dev/null)
    END_TIME=$(date +%s%N)
    Q2_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Query (daily agg): ${Q2_MS}ms"

    # Query 3: Device-specific query
    START_TIME=$(date +%s%N)
    RESULT=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "
    SELECT time, temperature, humidity FROM sensor_data
    WHERE device_id = 1 ORDER BY time DESC LIMIT 1000;" 2>/dev/null)
    END_TIME=$(date +%s%N)
    Q3_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Query (device specific): ${Q3_MS}ms"

    if [[ "$FORMAT" == "json" ]]; then
        cat >> "$RESULTS_FILE" << EOF
    {"name": "insert", "rows": $ROWS, "duration_ms": $INSERT_MS, "rows_per_sec": $INSERT_RATE},
    {"name": "query_last_hour", "duration_ms": $Q1_MS},
    {"name": "query_daily_agg", "duration_ms": $Q2_MS},
    {"name": "query_device_specific", "duration_ms": $Q3_MS}
  ]
}
EOF
    fi

    if [[ "$FORMAT" == "csv" ]]; then
        echo "test,metric,value" > "$RESULTS_FILE"
        echo "insert,rows,$ROWS" >> "$RESULTS_FILE"
        echo "insert,duration_ms,$INSERT_MS" >> "$RESULTS_FILE"
        echo "insert,rows_per_sec,$INSERT_RATE" >> "$RESULTS_FILE"
        echo "query_last_hour,duration_ms,$Q1_MS" >> "$RESULTS_FILE"
        echo "query_daily_agg,duration_ms,$Q2_MS" >> "$RESULTS_FILE"
        echo "query_device_specific,duration_ms,$Q3_MS" >> "$RESULTS_FILE"
    fi
fi

echo ""
echo "=========================================="
echo "Results saved to: $RESULTS_FILE"
echo "=========================================="
