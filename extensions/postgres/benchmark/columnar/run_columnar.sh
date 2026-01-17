#!/bin/bash
#
# Columnar Storage Benchmark Runner for Orochi DB
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default configuration
SCALE_FACTOR=1
FORMAT="json"
OUTPUT_DIR="../results/columnar"
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

usage() {
    echo "Columnar Storage Benchmark Runner"
    echo ""
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --scale=N       Scale factor [default: 1]"
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

echo "============================================"
echo "  Columnar Storage Benchmark for Orochi DB"
echo "============================================"
echo ""
echo "Configuration:"
echo "  Scale Factor: $SCALE_FACTOR"
echo "  Database:     $PGHOST:$PGPORT/$PGDATABASE"
echo ""

# Check database connection
if ! psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT 1" &>/dev/null; then
    echo "ERROR: Cannot connect to database"
    exit 1
fi

# Create schema
echo "Creating columnar benchmark schema..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -f schema.sql 2>/dev/null || true

RESULTS_FILE="$OUTPUT_DIR/columnar_results.${FORMAT}"
ROWS=$((SCALE_FACTOR * 100000))

# Check if native benchmark exists
if [[ -f "./columnar_bench" ]]; then
    echo "Running native benchmark..."
    ./columnar_bench \
        --mode=all \
        --scale="$SCALE_FACTOR" \
        --host="$PGHOST" \
        --port="$PGPORT" \
        --user="$PGUSER" \
        --database="$PGDATABASE" \
        --output="$OUTPUT_DIR"
else
    echo "Native benchmark not built, using SQL-only mode..."

    # Populate test data
    echo ""
    echo "Populating test data ($ROWS rows per table)..."

    # Integer table
    echo "  Populating integer data..."
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
    INSERT INTO columnar_integers (monotonic_seq, random_int, small_range, boolean_col)
    SELECT generate_series, (random() * 2147483647)::int, (random() * 100)::smallint, random() > 0.5
    FROM generate_series(1, $ROWS);" 2>/dev/null

    # Mixed table (columnar)
    echo "  Populating columnar mixed data..."
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
    INSERT INTO columnar_mixed (event_time, device_id, metric_name, metric_value)
    SELECT NOW() - (random() * INTERVAL '30 days'),
           (random() * 1000)::int,
           (ARRAY['cpu_usage','memory_used','disk_io','network_rx','network_tx'])[1 + (random() * 4)::int],
           random() * 100.0
    FROM generate_series(1, $ROWS);" 2>/dev/null

    # Row store baseline
    echo "  Populating row store baseline..."
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "
    INSERT INTO row_store_baseline (event_time, device_id, metric_name, metric_value)
    SELECT NOW() - (random() * INTERVAL '30 days'),
           (random() * 1000)::int,
           (ARRAY['cpu_usage','memory_used','disk_io','network_rx','network_tx'])[1 + (random() * 4)::int],
           random() * 100.0
    FROM generate_series(1, $ROWS);" 2>/dev/null

    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "ANALYZE;" 2>/dev/null

    echo ""
    echo "Running compression benchmark..."

    # Get table sizes
    COLUMNAR_SIZE=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT pg_total_relation_size('columnar_mixed');" 2>/dev/null | tr -d ' ')
    ROW_SIZE=$(psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT pg_total_relation_size('row_store_baseline');" 2>/dev/null | tr -d ' ')

    RATIO=$(echo "scale=2; $ROW_SIZE / $COLUMNAR_SIZE" | bc 2>/dev/null || echo "1.00")

    echo "  Columnar size:   $(numfmt --to=iec $COLUMNAR_SIZE 2>/dev/null || echo $COLUMNAR_SIZE)"
    echo "  Row store size:  $(numfmt --to=iec $ROW_SIZE 2>/dev/null || echo $ROW_SIZE)"
    echo "  Compression ratio: ${RATIO}x"

    echo ""
    echo "Running scan benchmark..."

    # Full scan
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT COUNT(*), AVG(metric_value) FROM columnar_mixed;" &>/dev/null
    END_TIME=$(date +%s%N)
    COLUMNAR_SCAN=$(( (END_TIME - START_TIME) / 1000000 ))

    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT COUNT(*), AVG(metric_value) FROM row_store_baseline;" &>/dev/null
    END_TIME=$(date +%s%N)
    ROW_SCAN=$(( (END_TIME - START_TIME) / 1000000 ))

    SCAN_SPEEDUP=$(echo "scale=2; $ROW_SCAN / $COLUMNAR_SCAN" | bc 2>/dev/null || echo "1.00")

    echo "  Columnar scan:   ${COLUMNAR_SCAN}ms"
    echo "  Row store scan:  ${ROW_SCAN}ms"
    echo "  Speedup:         ${SCAN_SPEEDUP}x"

    echo ""
    echo "Running filter benchmark..."

    # Filter query
    START_TIME=$(date +%s%N)
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -c "SELECT COUNT(*) FROM columnar_mixed WHERE device_id < 100;" &>/dev/null
    END_TIME=$(date +%s%N)
    FILTER_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    echo "  Filter query (device_id < 100): ${FILTER_MS}ms"

    # Save results
    if [[ "$FORMAT" == "json" ]]; then
        cat > "$RESULTS_FILE" << EOF
{
  "benchmark": "Columnar",
  "scale_factor": $SCALE_FACTOR,
  "rows": $ROWS,
  "timestamp": "$(date -Iseconds)",
  "compression": {
    "columnar_size_bytes": $COLUMNAR_SIZE,
    "row_size_bytes": $ROW_SIZE,
    "ratio": $RATIO
  },
  "scan": {
    "columnar_ms": $COLUMNAR_SCAN,
    "row_ms": $ROW_SCAN,
    "speedup": $SCAN_SPEEDUP
  },
  "filter": {
    "duration_ms": $FILTER_MS
  }
}
EOF
    else
        cat > "$RESULTS_FILE" << EOF
test,metric,value
compression,columnar_size,$COLUMNAR_SIZE
compression,row_size,$ROW_SIZE
compression,ratio,$RATIO
scan,columnar_ms,$COLUMNAR_SCAN
scan,row_ms,$ROW_SCAN
scan,speedup,$SCAN_SPEEDUP
filter,duration_ms,$FILTER_MS
EOF
    fi
fi

echo ""
echo "============================================"
echo "Results saved to: $RESULTS_FILE"
echo "============================================"
