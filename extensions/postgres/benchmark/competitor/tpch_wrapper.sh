#!/bin/bash
# TPC-H Benchmark Wrapper Script
# Outputs parseable format: QUERY:name:time_ms

set -euo pipefail

QUERIES_DIR="${TPCH_QUERIES_DIR:-../tpch/queries}"
SCALE="${TPCH_SCALE:-1}"

# Run each query file and time it
for qfile in "$QUERIES_DIR"/q*.sql; do
    qname=$(basename "$qfile" .sql)

    start_ns=$(date +%s%N 2>/dev/null || python3 -c "import time; print(int(time.time()*1e9))")

    if psql -v ON_ERROR_STOP=1 -f "$qfile" > /dev/null 2>&1; then
        end_ns=$(date +%s%N 2>/dev/null || python3 -c "import time; print(int(time.time()*1e9))")
        time_ms=$(( (end_ns - start_ns) / 1000000 ))
        echo "QUERY:${qname}:${time_ms}"
    else
        echo "QUERY:${qname}:-1"
    fi
done
