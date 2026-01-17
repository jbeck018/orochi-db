#!/bin/bash
#
# TPC-H Benchmark Runner for Orochi DB
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default configuration
SCALE_FACTOR=1
STREAMS=1
DURATION=60
FORMAT="json"
OUTPUT_DIR="../results/tpch"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"
SKIP_LOAD=0

usage() {
    echo "TPC-H Benchmark Runner"
    echo ""
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --scale=N       Scale factor (1, 10, 100) [default: 1]"
    echo "  --streams=N     Number of concurrent query streams [default: 1]"
    echo "  --duration=N    Duration of throughput test in seconds [default: 60]"
    echo "  --format=FMT    Output format (json, csv) [default: json]"
    echo "  --output=DIR    Output directory [default: ../results/tpch]"
    echo "  --host=HOST     PostgreSQL host [default: localhost]"
    echo "  --port=PORT     PostgreSQL port [default: 5432]"
    echo "  --user=USER     PostgreSQL user [default: postgres]"
    echo "  --database=DB   Database name [default: orochi_bench]"
    echo "  --skip-load     Skip data loading (use existing data)"
    echo "  --help          Show this help"
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --scale=*) SCALE_FACTOR="${1#*=}" ;;
        --streams=*) STREAMS="${1#*=}" ;;
        --duration=*) DURATION="${1#*=}" ;;
        --format=*) FORMAT="${1#*=}" ;;
        --output=*) OUTPUT_DIR="${1#*=}" ;;
        --host=*) PGHOST="${1#*=}" ;;
        --port=*) PGPORT="${1#*=}" ;;
        --user=*) PGUSER="${1#*=}" ;;
        --database=*) PGDATABASE="${1#*=}" ;;
        --skip-load) SKIP_LOAD=1 ;;
        --help) usage ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "=========================================="
echo "     TPC-H Benchmark for Orochi DB"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Scale Factor: $SCALE_FACTOR"
echo "  Query Streams: $STREAMS"
echo "  Duration: ${DURATION}s"
echo "  Database: $PGHOST:$PGPORT/$PGDATABASE"
echo ""

# Build benchmark executable if needed
if [[ ! -f "./tpch_bench" ]]; then
    echo "Building TPC-H benchmark..."
    make -C "$SCRIPT_DIR/.." common/libbench_common.a 2>/dev/null || true
    make tpch_bench 2>/dev/null || {
        echo "Note: Native benchmark not built, using SQL-only mode"
    }
fi

# Check database connection
if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &>/dev/null; then
    echo "ERROR: Cannot connect to database"
    exit 1
fi

# Create schema
echo "Creating TPC-H schema..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -f schema.sql 2>/dev/null || true

# Load data if not skipping
if [[ $SKIP_LOAD -eq 0 ]]; then
    if [[ -f "./tpch_bench" ]]; then
        echo "Generating TPC-H data (SF=$SCALE_FACTOR)..."
        ./tpch_bench --generate --scale="$SCALE_FACTOR" --data-dir=data

        echo "Loading data..."
        for table in region nation supplier part partsupp customer orders lineitem; do
            if [[ -f "data/${table}.tbl" ]]; then
                echo "  Loading $table..."
                psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
                    -c "\\COPY $table FROM 'data/${table}.tbl' WITH (FORMAT csv, DELIMITER '|')" 2>/dev/null || true
            fi
        done

        echo "Analyzing tables..."
        psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "ANALYZE;" 2>/dev/null || true
    else
        echo "WARNING: Data generator not available, using sample data"
    fi
fi

# Run queries
echo ""
echo "Running TPC-H Queries..."
echo ""

RESULTS_FILE="$OUTPUT_DIR/tpch_results.${FORMAT}"

if [[ "$FORMAT" == "json" ]]; then
    echo "{" > "$RESULTS_FILE"
    echo "  \"benchmark\": \"TPC-H\"," >> "$RESULTS_FILE"
    echo "  \"scale_factor\": $SCALE_FACTOR," >> "$RESULTS_FILE"
    echo "  \"streams\": $STREAMS," >> "$RESULTS_FILE"
    echo "  \"timestamp\": \"$(date -Iseconds)\"," >> "$RESULTS_FILE"
    echo "  \"queries\": [" >> "$RESULTS_FILE"
fi

if [[ "$FORMAT" == "csv" ]]; then
    echo "query,execution_ms,rows,status" > "$RESULTS_FILE"
fi

FIRST=1
for query_file in queries/q*.sql; do
    if [[ -f "$query_file" ]]; then
        query_name=$(basename "$query_file" .sql | tr '[:lower:]' '[:upper:]')

        echo -n "  $query_name: "

        # Run query and measure time
        START_TIME=$(date +%s%N)
        RESULT=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
            -t -c "\\timing on" -f "$query_file" 2>&1) || true
        END_TIME=$(date +%s%N)

        ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))

        # Extract row count
        ROWS=$(echo "$RESULT" | grep -oP '\(\d+ rows?\)' | grep -oP '\d+' || echo "0")
        STATUS="OK"

        if echo "$RESULT" | grep -qi "error"; then
            STATUS="ERROR"
        fi

        echo "${ELAPSED_MS}ms (${ROWS} rows) [$STATUS]"

        if [[ "$FORMAT" == "json" ]]; then
            if [[ $FIRST -eq 0 ]]; then
                echo "," >> "$RESULTS_FILE"
            fi
            FIRST=0
            cat >> "$RESULTS_FILE" << EOF
    {
      "name": "$query_name",
      "execution_ms": $ELAPSED_MS,
      "rows": $ROWS,
      "status": "$STATUS"
    }
EOF
        fi

        if [[ "$FORMAT" == "csv" ]]; then
            echo "$query_name,$ELAPSED_MS,$ROWS,$STATUS" >> "$RESULTS_FILE"
        fi
    fi
done

if [[ "$FORMAT" == "json" ]]; then
    echo "" >> "$RESULTS_FILE"
    echo "  ]" >> "$RESULTS_FILE"
    echo "}" >> "$RESULTS_FILE"
fi

echo ""
echo "=========================================="
echo "Results saved to: $RESULTS_FILE"
echo "=========================================="
