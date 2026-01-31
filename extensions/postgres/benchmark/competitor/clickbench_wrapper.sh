#!/usr/bin/env bash
#
# ClickBench Wrapper for Orochi DB Competitor Comparison
#
# Runs the ClickBench analytical benchmark (43 queries on web analytics data)
# against PostgreSQL-compatible databases to evaluate columnar/analytical performance.
#
# Reference: https://github.com/ClickHouse/ClickBench
#
# Environment variables:
#   BENCH_HOST       - Database host (default: localhost)
#   BENCH_PORT       - Database port (default: 5432)
#   BENCH_DB         - Database name (default: clickbench)
#   BENCH_USER       - Database user (default: postgres)
#   BENCH_PASSWORD   - Database password (optional)
#   BENCH_TARGET     - Target name for identification
#   BENCH_RUNS       - Number of query runs (default: 3)
#   BENCH_SKIP_LOAD  - Skip data loading if set to 1
#
# Output:
#   Writes metrics to stdout in parseable format
#   Creates detailed log in $RESULTS_DIR
#

set -euo pipefail

# Configuration
BENCH_HOST="${BENCH_HOST:-localhost}"
BENCH_PORT="${BENCH_PORT:-5432}"
BENCH_DB="${BENCH_DB:-clickbench}"
BENCH_USER="${BENCH_USER:-postgres}"
BENCH_PASSWORD="${BENCH_PASSWORD:-}"
BENCH_TARGET="${BENCH_TARGET:-unknown}"
BENCH_RUNS="${BENCH_RUNS:-3}"
BENCH_SKIP_LOAD="${BENCH_SKIP_LOAD:-0}"

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
DATA_DIR="${SCRIPT_DIR}/data"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${RESULTS_DIR}/clickbench_${BENCH_TARGET}_${TIMESTAMP}.log"
QUERIES_FILE="${SCRIPT_DIR}/clickbench_queries.sql"

# Data file URL
CLICKBENCH_DATA_URL="https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz"
CLICKBENCH_DATA_FILE="${DATA_DIR}/hits.tsv.gz"

# Ensure directories exist
mkdir -p "${RESULTS_DIR}" "${DATA_DIR}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

error() {
    log "ERROR: $*" >&2
    exit 1
}

# Download ClickBench data
download_data() {
    if [[ -f "${CLICKBENCH_DATA_FILE}" ]]; then
        log "ClickBench data already downloaded: ${CLICKBENCH_DATA_FILE}"
        return
    fi

    log "Downloading ClickBench dataset (~15GB compressed, ~100GB uncompressed)..."
    log "This may take a while..."

    wget -c -O "${CLICKBENCH_DATA_FILE}" "${CLICKBENCH_DATA_URL}" \
        || curl -C - -o "${CLICKBENCH_DATA_FILE}" "${CLICKBENCH_DATA_URL}"

    log "Download complete: ${CLICKBENCH_DATA_FILE}"
}

# Create ClickBench schema
create_schema() {
    log "Creating ClickBench schema..."

    # Create database if it doesn't exist
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d postgres -c "CREATE DATABASE ${BENCH_DB};" 2>/dev/null || true

    # Enable extensions based on target
    case "${BENCH_TARGET}" in
        orochi)
            PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
                -U "${BENCH_USER}" -d "${BENCH_DB}" -c "CREATE EXTENSION IF NOT EXISTS orochi CASCADE;" 2>/dev/null || true
            ;;
        timescaledb)
            PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
                -U "${BENCH_USER}" -d "${BENCH_DB}" -c "CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE;" 2>/dev/null || true
            ;;
        citus)
            PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
                -U "${BENCH_USER}" -d "${BENCH_DB}" -c "CREATE EXTENSION IF NOT EXISTS citus CASCADE;" 2>/dev/null || true
            ;;
    esac

    # Create hits table (ClickBench schema)
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" << 'EOSQL'

DROP TABLE IF EXISTS hits CASCADE;

CREATE TABLE hits (
    WatchID BIGINT NOT NULL,
    JavaEnable SMALLINT NOT NULL,
    Title TEXT NOT NULL,
    GoodEvent SMALLINT NOT NULL,
    EventTime TIMESTAMP NOT NULL,
    EventDate Date NOT NULL,
    CounterID INTEGER NOT NULL,
    ClientIP INTEGER NOT NULL,
    RegionID INTEGER NOT NULL,
    UserID BIGINT NOT NULL,
    CounterClass SMALLINT NOT NULL,
    OS SMALLINT NOT NULL,
    UserAgent SMALLINT NOT NULL,
    URL TEXT NOT NULL,
    Referer TEXT NOT NULL,
    IsRefresh SMALLINT NOT NULL,
    RefererCategoryID SMALLINT NOT NULL,
    RefererRegionID INTEGER NOT NULL,
    URLCategoryID SMALLINT NOT NULL,
    URLRegionID INTEGER NOT NULL,
    ResolutionWidth SMALLINT NOT NULL,
    ResolutionHeight SMALLINT NOT NULL,
    ResolutionDepth SMALLINT NOT NULL,
    FlashMajor SMALLINT NOT NULL,
    FlashMinor SMALLINT NOT NULL,
    FlashMinor2 TEXT NOT NULL,
    NetMajor SMALLINT NOT NULL,
    NetMinor SMALLINT NOT NULL,
    UserAgentMajor SMALLINT NOT NULL,
    UserAgentMinor VARCHAR(255) NOT NULL,
    CookieEnable SMALLINT NOT NULL,
    JavascriptEnable SMALLINT NOT NULL,
    IsMobile SMALLINT NOT NULL,
    MobilePhone SMALLINT NOT NULL,
    MobilePhoneModel TEXT NOT NULL,
    Params TEXT NOT NULL,
    IPNetworkID INTEGER NOT NULL,
    TraficSourceID SMALLINT NOT NULL,
    SearchEngineID SMALLINT NOT NULL,
    SearchPhrase TEXT NOT NULL,
    AdvEngineID SMALLINT NOT NULL,
    IsArtifical SMALLINT NOT NULL,
    WindowClientWidth SMALLINT NOT NULL,
    WindowClientHeight SMALLINT NOT NULL,
    ClientTimeZone SMALLINT NOT NULL,
    ClientEventTime TIMESTAMP NOT NULL,
    SilverlightVersion1 SMALLINT NOT NULL,
    SilverlightVersion2 SMALLINT NOT NULL,
    SilverlightVersion3 INTEGER NOT NULL,
    SilverlightVersion4 SMALLINT NOT NULL,
    PageCharset TEXT NOT NULL,
    CodeVersion INTEGER NOT NULL,
    IsLink SMALLINT NOT NULL,
    IsDownload SMALLINT NOT NULL,
    IsNotBounce SMALLINT NOT NULL,
    FUniqID BIGINT NOT NULL,
    OriginalURL TEXT NOT NULL,
    HID INTEGER NOT NULL,
    IsOldCounter SMALLINT NOT NULL,
    IsEvent SMALLINT NOT NULL,
    IsParameter SMALLINT NOT NULL,
    DontCountHits SMALLINT NOT NULL,
    WithHash SMALLINT NOT NULL,
    HitColor CHAR(1) NOT NULL,
    LocalEventTime TIMESTAMP NOT NULL,
    Age SMALLINT NOT NULL,
    Sex SMALLINT NOT NULL,
    Income SMALLINT NOT NULL,
    Interests SMALLINT NOT NULL,
    Robotness SMALLINT NOT NULL,
    RemoteIP INTEGER NOT NULL,
    WindowName INTEGER NOT NULL,
    OpenerName INTEGER NOT NULL,
    HistoryLength SMALLINT NOT NULL,
    BrowserLanguage TEXT NOT NULL,
    BrowserCountry TEXT NOT NULL,
    SocialNetwork TEXT NOT NULL,
    SocialAction TEXT NOT NULL,
    HTTPError SMALLINT NOT NULL,
    SendTiming INTEGER NOT NULL,
    DNSTiming INTEGER NOT NULL,
    ConnectTiming INTEGER NOT NULL,
    ResponseStartTiming INTEGER NOT NULL,
    ResponseEndTiming INTEGER NOT NULL,
    FetchTiming INTEGER NOT NULL,
    SocialSourceNetworkID SMALLINT NOT NULL,
    SocialSourcePage TEXT NOT NULL,
    ParamPrice BIGINT NOT NULL,
    ParamOrderID TEXT NOT NULL,
    ParamCurrency TEXT NOT NULL,
    ParamCurrencyID SMALLINT NOT NULL,
    OpenstatServiceName TEXT NOT NULL,
    OpenstatCampaignID TEXT NOT NULL,
    OpenstatAdID TEXT NOT NULL,
    OpenstatSourceID TEXT NOT NULL,
    UTMSource TEXT NOT NULL,
    UTMMedium TEXT NOT NULL,
    UTMCampaign TEXT NOT NULL,
    UTMContent TEXT NOT NULL,
    UTMTerm TEXT NOT NULL,
    FromTag TEXT NOT NULL,
    HasGCLID SMALLINT NOT NULL,
    RefererHash BIGINT NOT NULL,
    URLHash BIGINT NOT NULL,
    CLID INTEGER NOT NULL
);

EOSQL

    log "Schema created"
}

# Load data into database
load_data() {
    if [[ "${BENCH_SKIP_LOAD}" == "1" ]]; then
        log "Skipping data load (BENCH_SKIP_LOAD=1)"
        return
    fi

    log "Loading ClickBench data..."
    log "This may take 30-60 minutes for the full dataset..."

    local start_time end_time duration
    start_time=$(date +%s)

    # Load data using COPY
    gunzip -c "${CLICKBENCH_DATA_FILE}" | \
        PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
            -U "${BENCH_USER}" -d "${BENCH_DB}" \
            -c "\\copy hits FROM STDIN"

    end_time=$(date +%s)
    duration=$((end_time - start_time))

    # Get row count
    local row_count
    row_count=$(PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -t -c "SELECT COUNT(*) FROM hits;")

    log "Data loading complete"
    log "  Duration: ${duration} seconds"
    log "  Rows loaded: ${row_count}"

    # Create indexes for better query performance
    log "Creating indexes..."
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" << 'EOSQL'

CREATE INDEX IF NOT EXISTS hits_eventdate_idx ON hits(EventDate);
CREATE INDEX IF NOT EXISTS hits_counterid_idx ON hits(CounterID);
CREATE INDEX IF NOT EXISTS hits_userid_idx ON hits(UserID);
CREATE INDEX IF NOT EXISTS hits_regionid_idx ON hits(RegionID);

EOSQL

    log "Indexes created"

    # Run ANALYZE
    log "Running ANALYZE..."
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -c "ANALYZE hits;"

    log "Data load complete"
}

# Create queries file
create_queries_file() {
    if [[ -f "${QUERIES_FILE}" ]]; then
        return
    fi

    log "Creating ClickBench queries file..."

    cat > "${QUERIES_FILE}" << 'EOF'
-- Q0: Count all rows
SELECT COUNT(*) FROM hits;

-- Q1: Count with simple filter
SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0;

-- Q2: Sum with filter
SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits;

-- Q3: Aggregation by date
SELECT SUM(UserID) FROM hits;

-- Q4: Distinct count
SELECT COUNT(DISTINCT UserID) FROM hits;

-- Q5: Distinct count with filter
SELECT COUNT(DISTINCT SearchPhrase) FROM hits;

-- Q6: Min/Max
SELECT MIN(EventDate), MAX(EventDate) FROM hits;

-- Q7: Aggregation by low cardinality column
SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC;

-- Q8: Aggregation by medium cardinality
SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DESC LIMIT 10;

-- Q9: Aggregation by date with count
SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM hits GROUP BY RegionID ORDER BY c DESC LIMIT 10;

-- Q10: Text search
SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10;

-- Q11: Text aggregation
SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10;

-- Q12: Search phrase analysis
SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10;

-- Q13: Search phrase with distinct
SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10;

-- Q14: Search engine analysis
SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase ORDER BY c DESC LIMIT 10;

-- Q15: User agent analysis
SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10;

-- Q16: User session analysis
SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10;

-- Q17: User session with limit
SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase LIMIT 10;

-- Q18: Time series aggregation
SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, m, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10;

-- Q19: Complex filter
SELECT UserID FROM hits WHERE UserID = 435090932899640449;

-- Q20: Count with multiple conditions
SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%';

-- Q21: URL analysis
SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM hits WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10;

-- Q22: URL and search analysis
SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM hits WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.telerik.com%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10;

-- Q23: Event time analysis
SELECT * FROM hits WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10;

-- Q24: Search phrase with URL
SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10;

-- Q25: Search phrase analysis with time
SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10;

-- Q26: Search phrase distinct time
SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase LIMIT 10;

-- Q27: Counter analysis
SELECT CounterID, AVG(length(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25;

-- Q28: Referrer analysis
SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS key, AVG(length(Referer)) AS l, COUNT(*) AS c, MIN(Referer) FROM hits WHERE Referer <> '' GROUP BY key HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25;

-- Q29: Daily unique users
SELECT SUM(ResolutionWidth), SUM(ResolutionHeight) FROM hits;

-- Q30: Resolution analysis
SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS c FROM hits GROUP BY M ORDER BY M LIMIT 10;

-- Q31: Traffic source analysis
SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN SearchEngineID = 0 AND AdvEngineID = 0 THEN Referer ELSE '' END AS Src, URL AS Dst, COUNT(*) AS c FROM hits GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY c DESC LIMIT 10;

-- Q32: Referrer domain
SELECT DATE_TRUNC('minute', EventTime) AS m, COUNT(DISTINCT UserID) FROM hits GROUP BY m ORDER BY m LIMIT 10;

-- Q33: Social network
SELECT DATE_TRUNC('minute', EventTime) AS m, COUNT(*) AS c, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GROUP BY m ORDER BY m LIMIT 10;

-- Q34: Complex join simulation
SELECT DATE_TRUNC('minute', EventTime) AS m, COUNT(*) AS c, COUNT(DISTINCT UserID) AS u, COUNT(DISTINCT SearchPhrase) AS s FROM hits WHERE SearchPhrase <> '' GROUP BY m ORDER BY m LIMIT 10;

-- Q35: Window function
SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10;

-- Q36: Percentile
SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10;

-- Q37: Distinct URL
SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10;

-- Q38: Search conversion
SELECT EventDate, COUNT(*) AS c FROM hits GROUP BY EventDate ORDER BY c DESC LIMIT 10;

-- Q39: Event date aggregation
SELECT EventDate, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY EventDate ORDER BY EventDate LIMIT 10;

-- Q40: Hourly analysis
SELECT DATE_TRUNC('hour', EventTime) AS h, COUNT(*) AS c FROM hits GROUP BY h ORDER BY h LIMIT 24;

-- Q41: Region time analysis
SELECT RegionID, DATE_TRUNC('day', EventTime) AS d, COUNT(*) AS c FROM hits GROUP BY RegionID, d ORDER BY c DESC LIMIT 10;

-- Q42: Complex aggregation
SELECT RegionID, COUNT(DISTINCT UserID) AS u, COUNT(*) AS c, SUM(CASE WHEN IsRefresh = 1 THEN 1 ELSE 0 END) AS refresh FROM hits GROUP BY RegionID ORDER BY c DESC LIMIT 10;

EOF

    log "Queries file created: ${QUERIES_FILE}"
}

# Run a single query and measure time
run_query() {
    local query_num="$1"
    local query="$2"
    local run_num="$3"

    local start_time end_time duration
    start_time=$(date +%s.%N)

    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" -t -c "${query}" > /dev/null 2>&1

    end_time=$(date +%s.%N)
    duration=$(echo "${end_time} - ${start_time}" | bc)

    echo "${duration}"
}

# Run all queries
run_queries() {
    log "Running ClickBench queries (${BENCH_RUNS} runs each)..."

    local query_times=()
    local query_num=0
    local total_cold=0
    local total_hot=0

    # Read queries and run them
    while IFS= read -r line || [[ -n "${line}" ]]; do
        # Skip empty lines and comments
        [[ -z "${line}" ]] && continue
        [[ "${line}" == --* ]] && continue

        local query="${line}"
        query_num=$((query_num + 1))

        log "  Running Q${query_num}..."

        # Cold run
        local cold_time
        cold_time=$(run_query "${query_num}" "${query}" 0)
        total_cold=$(echo "${total_cold} + ${cold_time}" | bc)

        # Hot runs
        local hot_times=()
        for ((run=1; run<=BENCH_RUNS; run++)); do
            local hot_time
            hot_time=$(run_query "${query_num}" "${query}" "${run}")
            hot_times+=("${hot_time}")
        done

        # Calculate average hot time
        local hot_sum=0
        for t in "${hot_times[@]}"; do
            hot_sum=$(echo "${hot_sum} + ${t}" | bc)
        done
        local avg_hot
        avg_hot=$(echo "scale=3; ${hot_sum} / ${BENCH_RUNS}" | bc)
        total_hot=$(echo "${total_hot} + ${avg_hot}" | bc)

        # Find min hot time
        local min_hot="${hot_times[0]}"
        for t in "${hot_times[@]}"; do
            if (( $(echo "${t} < ${min_hot}" | bc -l) )); then
                min_hot="${t}"
            fi
        done

        log "    Q${query_num}: cold=${cold_time}s, hot_avg=${avg_hot}s, hot_min=${min_hot}s"

        echo "Q${query_num}: ${cold_time}s (cold), ${avg_hot}s (hot avg), ${min_hot}s (hot min)"

    done < "${QUERIES_FILE}"

    # Output summary
    echo ""
    echo "=== ClickBench Results ==="
    echo "Target: ${BENCH_TARGET}"
    echo "Queries: ${query_num}"
    echo "Runs per query: ${BENCH_RUNS}"
    echo ""
    echo "Total cold time: ${total_cold}s"
    echo "Total hot time (avg): ${total_hot}s"
    echo "=========================="
}

# Run synthetic ClickBench-like benchmark (smaller dataset)
run_synthetic_clickbench() {
    log "Running synthetic ClickBench-like benchmark..."
    log "(Using smaller dataset for quick testing)"

    # Create smaller test table
    PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
        -U "${BENCH_USER}" -d "${BENCH_DB}" << 'EOSQL'

-- Create smaller hits table
DROP TABLE IF EXISTS hits CASCADE;

CREATE TABLE hits (
    WatchID BIGINT,
    EventTime TIMESTAMP,
    EventDate DATE,
    CounterID INTEGER,
    UserID BIGINT,
    RegionID INTEGER,
    URL TEXT,
    Referer TEXT,
    SearchPhrase TEXT,
    AdvEngineID SMALLINT,
    ResolutionWidth SMALLINT,
    ResolutionHeight SMALLINT
);

-- Insert test data (~1M rows)
INSERT INTO hits
SELECT
    g,
    NOW() - (g * interval '1 second'),
    (NOW() - (g * interval '1 second'))::date,
    (g % 1000)::integer,
    (g * 12345)::bigint,
    (g % 100)::integer,
    'https://example.com/page/' || (g % 10000),
    CASE WHEN g % 3 = 0 THEN 'https://google.com/search?q=' || (g % 1000) ELSE '' END,
    CASE WHEN g % 5 = 0 THEN 'search term ' || (g % 500) ELSE '' END,
    (g % 10)::smallint,
    (1024 + (g % 1000))::smallint,
    (768 + (g % 500))::smallint
FROM generate_series(1, 1000000) g;

CREATE INDEX hits_eventdate_idx ON hits(EventDate);
CREATE INDEX hits_counterid_idx ON hits(CounterID);
CREATE INDEX hits_userid_idx ON hits(UserID);

ANALYZE hits;
EOSQL

    log "Test data created (1M rows)"

    # Run simplified queries
    local queries=(
        "SELECT COUNT(*) FROM hits"
        "SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0"
        "SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits"
        "SELECT COUNT(DISTINCT UserID) FROM hits"
        "SELECT RegionID, COUNT(*) AS c FROM hits GROUP BY RegionID ORDER BY c DESC LIMIT 10"
        "SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"
        "SELECT DATE_TRUNC('hour', EventTime) AS h, COUNT(*) FROM hits GROUP BY h ORDER BY h LIMIT 24"
        "SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10"
        "SELECT EventDate, COUNT(*) AS c FROM hits GROUP BY EventDate ORDER BY c DESC LIMIT 10"
        "SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DESC LIMIT 10"
    )

    local query_num=0
    local total_time=0
    local query_times=()

    log "Running test queries..."

    for query in "${queries[@]}"; do
        query_num=$((query_num + 1))

        local start_time end_time duration
        start_time=$(date +%s.%N)

        PGPASSWORD="${BENCH_PASSWORD}" psql -h "${BENCH_HOST}" -p "${BENCH_PORT}" \
            -U "${BENCH_USER}" -d "${BENCH_DB}" -t -c "${query}" > /dev/null 2>&1

        end_time=$(date +%s.%N)
        duration=$(echo "${end_time} - ${start_time}" | bc)
        total_time=$(echo "${total_time} + ${duration}" | bc)

        query_times+=("${duration}")
        log "  Q${query_num}: ${duration}s"
    done

    # Output results
    echo ""
    echo "=== ClickBench Results (Synthetic) ==="
    echo "Target: ${BENCH_TARGET}"
    echo "Queries: ${query_num}"
    echo "Dataset: 1M rows (synthetic)"
    echo ""
    echo "Total time: ${total_time}s"
    echo ""

    for i in "${!query_times[@]}"; do
        echo "Q$((i+1)): ${query_times[$i]}s"
    done

    echo "======================================="
}

# Main
main() {
    log "=== ClickBench Wrapper ==="
    log "Target: ${BENCH_TARGET}"
    log "Host: ${BENCH_HOST}:${BENCH_PORT}"
    log "Database: ${BENCH_DB}"
    log ""

    # Check if full benchmark or synthetic
    if [[ "${BENCH_SKIP_LOAD}" != "1" ]] && [[ ! -f "${CLICKBENCH_DATA_FILE}" ]]; then
        log "Full ClickBench dataset not found."
        log "To download the full dataset (~15GB), run: $0 download"
        log "Running synthetic benchmark instead..."
        echo ""

        create_schema
        run_synthetic_clickbench
    else
        # Full benchmark
        create_schema
        create_queries_file

        if [[ "${BENCH_SKIP_LOAD}" != "1" ]]; then
            download_data
            load_data
        fi

        run_queries
    fi

    log ""
    log "ClickBench complete. Results logged to: ${LOG_FILE}"
}

# Handle commands
case "${1:-}" in
    download)
        download_data
        exit 0
        ;;
    schema)
        create_schema
        exit 0
        ;;
    load)
        create_schema
        download_data
        load_data
        exit 0
        ;;
    queries)
        create_queries_file
        exit 0
        ;;
    synthetic)
        create_schema
        run_synthetic_clickbench
        exit 0
        ;;
    *)
        main "$@"
        ;;
esac
