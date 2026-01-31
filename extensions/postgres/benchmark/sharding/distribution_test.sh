#!/bin/bash
#
# Orochi DB - Distribution Uniformity Test
#
# Tests data distribution across shards:
# - Queries shard metadata tables
# - Calculates Relative Standard Deviation (RSD) for each table
# - Reports hotspot detection
# - Provides recommendations
#
# Target: RSD < 10% for acceptable distribution
#

set -e

# Configuration
PGHOST="${PGHOST:-localhost}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-orochi_bench}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Print functions
print_header() {
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}========================================${NC}\n"
}

print_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}

# Execute SQL query
sql_query() {
    PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -t -A -c "$1"
}

sql_query_formatted() {
    PGPASSWORD="$PGPASSWORD" psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "$1"
}

# Get list of distributed tables
get_distributed_tables() {
    sql_query "
        SELECT t.table_name
        FROM orochi.orochi_tables t
        WHERE t.is_distributed = true
        ORDER BY t.table_name
    " 2>/dev/null || echo ""
}

# Analyze distribution for a single table
analyze_table_distribution() {
    local table_name="$1"

    echo ""
    echo "Table: $table_name"
    echo "----------------------------------------"

    # Get shard statistics
    local stats=$(sql_query "
        SELECT
            COUNT(*) as shard_count,
            COALESCE(AVG(row_count), 0) as mean_rows,
            COALESCE(STDDEV(row_count), 0) as stddev_rows,
            COALESCE(MIN(row_count), 0) as min_rows,
            COALESCE(MAX(row_count), 0) as max_rows,
            COALESCE(SUM(row_count), 0) as total_rows
        FROM orochi.orochi_shards s
        JOIN orochi.orochi_tables t ON s.table_oid = t.relid
        WHERE t.table_name = '$table_name'
    " 2>/dev/null)

    if [ -z "$stats" ]; then
        # Fallback: try to get from physical shard tables
        stats=$(sql_query "
            SELECT
                COUNT(*) as shard_count,
                AVG(n_live_tup) as mean_rows,
                COALESCE(STDDEV(n_live_tup), 0) as stddev_rows,
                MIN(n_live_tup) as min_rows,
                MAX(n_live_tup) as max_rows,
                SUM(n_live_tup) as total_rows
            FROM pg_stat_user_tables
            WHERE relname LIKE '${table_name}_shard_%'
        " 2>/dev/null)
    fi

    if [ -z "$stats" ] || [ "$stats" = "0|0|0|0|0|0" ]; then
        echo "  No shard data available for this table"
        return
    fi

    # Parse stats
    IFS='|' read -r shard_count mean_rows stddev_rows min_rows max_rows total_rows <<< "$stats"

    # Calculate RSD
    local rsd=0
    if [ "$(echo "$mean_rows > 0" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        rsd=$(echo "scale=2; ($stddev_rows / $mean_rows) * 100" | bc -l 2>/dev/null || echo "0")
    fi

    # Calculate imbalance ratio
    local imbalance=1
    if [ "$(echo "$min_rows > 0" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        imbalance=$(echo "scale=2; $max_rows / $min_rows" | bc -l 2>/dev/null || echo "999")
    fi

    # Display results
    printf "  %-25s %s\n" "Shards:" "$shard_count"
    printf "  %-25s %s\n" "Total Rows:" "$total_rows"
    printf "  %-25s %.2f\n" "Mean Rows/Shard:" "${mean_rows:-0}"
    printf "  %-25s %.2f\n" "Std Deviation:" "${stddev_rows:-0}"
    printf "  %-25s %s\n" "Min Rows:" "$min_rows"
    printf "  %-25s %s\n" "Max Rows:" "$max_rows"
    printf "  %-25s %.2f\n" "Imbalance Ratio:" "$imbalance"
    printf "  %-25s %.2f%%\n" "RSD (Skew Score):" "$rsd"

    # Evaluate
    echo ""
    if [ "$(echo "$rsd < 10" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        print_success "Distribution is ACCEPTABLE (RSD < 10%)"
    elif [ "$(echo "$rsd < 20" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        print_warning "Distribution needs attention (10% <= RSD < 20%)"
    else
        print_fail "Distribution is POOR (RSD >= 20%)"
    fi

    # Show per-shard breakdown
    echo ""
    echo "  Per-Shard Distribution:"

    sql_query_formatted "
        SELECT
            s.shard_index as \"Shard\",
            COALESCE(s.row_count, 0) as \"Rows\",
            COALESCE(s.size_bytes / 1024 / 1024, 0) as \"Size (MB)\",
            CASE
                WHEN SUM(s.row_count) OVER () > 0
                THEN ROUND(s.row_count * 100.0 / SUM(s.row_count) OVER (), 2)
                ELSE 0
            END as \"Percent\",
            CASE
                WHEN s.row_count > 2 * AVG(s.row_count) OVER ()
                THEN '** HOTSPOT'
                WHEN s.row_count < 0.5 * AVG(s.row_count) OVER ()
                THEN '** COLD'
                ELSE ''
            END as \"Status\"
        FROM orochi.orochi_shards s
        JOIN orochi.orochi_tables t ON s.table_oid = t.relid
        WHERE t.table_name = '$table_name'
        ORDER BY s.shard_index
    " 2>/dev/null || echo "  (Unable to retrieve per-shard data)"
}

# Detect hotspots
detect_hotspots() {
    print_header "Hotspot Detection"

    local hotspots=$(sql_query "
        WITH shard_stats AS (
            SELECT
                t.table_name,
                s.shard_index,
                s.row_count,
                AVG(s.row_count) OVER (PARTITION BY t.table_name) as mean_rows
            FROM orochi.orochi_shards s
            JOIN orochi.orochi_tables t ON s.table_oid = t.relid
            WHERE t.is_distributed = true
        )
        SELECT
            table_name,
            shard_index,
            row_count,
            ROUND(row_count / NULLIF(mean_rows, 0), 2) as ratio
        FROM shard_stats
        WHERE row_count > 2 * mean_rows
        ORDER BY ratio DESC
    " 2>/dev/null)

    if [ -z "$hotspots" ]; then
        print_success "No hotspots detected (no shards with >2x mean rows)"
    else
        print_warning "Hotspots detected:"
        echo ""
        printf "  %-30s %-10s %-15s %-10s\n" "Table" "Shard" "Rows" "Ratio"
        echo "  ----------------------------------------------------------------"

        echo "$hotspots" | while IFS='|' read -r table shard rows ratio; do
            printf "  %-30s %-10s %-15s %-10sx\n" "$table" "$shard" "$rows" "$ratio"
        done
    fi
}

# Detect cold spots
detect_cold_spots() {
    print_header "Cold Spot Detection"

    local cold_spots=$(sql_query "
        WITH shard_stats AS (
            SELECT
                t.table_name,
                s.shard_index,
                s.row_count,
                AVG(s.row_count) OVER (PARTITION BY t.table_name) as mean_rows
            FROM orochi.orochi_shards s
            JOIN orochi.orochi_tables t ON s.table_oid = t.relid
            WHERE t.is_distributed = true
        )
        SELECT
            table_name,
            shard_index,
            row_count,
            ROUND(row_count / NULLIF(mean_rows, 0), 2) as ratio
        FROM shard_stats
        WHERE row_count < 0.5 * mean_rows AND mean_rows > 0
        ORDER BY ratio ASC
    " 2>/dev/null)

    if [ -z "$cold_spots" ]; then
        print_success "No cold spots detected (no shards with <0.5x mean rows)"
    else
        print_warning "Cold spots detected (underutilized shards):"
        echo ""
        printf "  %-30s %-10s %-15s %-10s\n" "Table" "Shard" "Rows" "Ratio"
        echo "  ----------------------------------------------------------------"

        echo "$cold_spots" | while IFS='|' read -r table shard rows ratio; do
            printf "  %-30s %-10s %-15s %-10sx\n" "$table" "$shard" "$rows" "$ratio"
        done
    fi
}

# Generate recommendations
generate_recommendations() {
    print_header "Recommendations"

    # Check for tables needing rebalancing
    local needs_rebalance=$(sql_query "
        WITH shard_stats AS (
            SELECT
                t.table_name,
                STDDEV(s.row_count) / NULLIF(AVG(s.row_count), 0) * 100 as rsd
            FROM orochi.orochi_shards s
            JOIN orochi.orochi_tables t ON s.table_oid = t.relid
            WHERE t.is_distributed = true
            GROUP BY t.table_name
        )
        SELECT table_name, ROUND(rsd, 2)
        FROM shard_stats
        WHERE rsd > 10
        ORDER BY rsd DESC
    " 2>/dev/null)

    if [ -n "$needs_rebalance" ]; then
        echo "Tables that may benefit from rebalancing:"
        echo ""

        echo "$needs_rebalance" | while IFS='|' read -r table rsd; do
            echo "  - $table (RSD: ${rsd}%)"
            echo "    Run: SELECT orochi_rebalance_table('$table'::regclass);"
        done
    else
        print_success "All tables have acceptable distribution (RSD < 10%)"
    fi

    echo ""
    echo "General recommendations:"
    echo "  1. Choose distribution columns with high cardinality"
    echo "  2. Avoid distribution on columns with skewed value distribution"
    echo "  3. Co-locate related tables on the same distribution column"
    echo "  4. Monitor distribution after bulk data loads"
    echo "  5. Consider time-based range distribution for event data"
}

# Summary report
generate_summary() {
    print_header "Distribution Summary"

    sql_query_formatted "
        WITH shard_stats AS (
            SELECT
                t.table_name,
                t.shard_count,
                t.distribution_column,
                COUNT(DISTINCT s.shard_id) as actual_shards,
                SUM(s.row_count) as total_rows,
                AVG(s.row_count) as mean_rows,
                STDDEV(s.row_count) as stddev_rows,
                MIN(s.row_count) as min_rows,
                MAX(s.row_count) as max_rows
            FROM orochi.orochi_tables t
            LEFT JOIN orochi.orochi_shards s ON t.relid = s.table_oid
            WHERE t.is_distributed = true
            GROUP BY t.table_name, t.shard_count, t.distribution_column
        )
        SELECT
            table_name as \"Table\",
            shard_count as \"Shards\",
            distribution_column as \"Dist Column\",
            total_rows as \"Total Rows\",
            ROUND(mean_rows) as \"Mean\",
            ROUND(stddev_rows) as \"StdDev\",
            CASE
                WHEN mean_rows > 0
                THEN ROUND((stddev_rows / mean_rows) * 100, 2)
                ELSE 0
            END as \"RSD %\",
            CASE
                WHEN mean_rows > 0 AND (stddev_rows / mean_rows) * 100 < 10
                THEN 'OK'
                WHEN mean_rows > 0 AND (stddev_rows / mean_rows) * 100 < 20
                THEN 'WARN'
                ELSE 'POOR'
            END as \"Status\"
        FROM shard_stats
        ORDER BY table_name
    " 2>/dev/null || echo "Unable to generate summary. Is Orochi extension installed?"
}

# Main entry point
main() {
    local command="${1:-all}"

    print_header "Orochi DB Distribution Uniformity Test"

    echo "Database: $PGDATABASE on $PGHOST:$PGPORT"
    echo "User: $PGUSER"
    echo ""

    case "$command" in
        all)
            # Get all distributed tables
            local tables=$(get_distributed_tables)

            if [ -z "$tables" ]; then
                echo "No distributed tables found."
                echo "Create distributed tables using:"
                echo "  SELECT orochi_create_distributed_table('table_name', 'column');"
                exit 0
            fi

            echo "Found distributed tables: $(echo "$tables" | wc -l | tr -d ' ')"

            # Analyze each table
            for table in $tables; do
                analyze_table_distribution "$table"
            done

            # Summary sections
            generate_summary
            detect_hotspots
            detect_cold_spots
            generate_recommendations
            ;;

        summary)
            generate_summary
            ;;

        hotspots)
            detect_hotspots
            detect_cold_spots
            ;;

        recommendations)
            generate_recommendations
            ;;

        table)
            if [ -z "$2" ]; then
                echo "Usage: $0 table <table_name>"
                exit 1
            fi
            analyze_table_distribution "$2"
            ;;

        help|--help|-h)
            cat << EOF
Orochi DB Distribution Uniformity Test

Usage: $(basename "$0") [command] [options]

Commands:
  all             Run full distribution analysis (default)
  summary         Show summary table only
  hotspots        Detect hot and cold spots
  recommendations Show optimization recommendations
  table <name>    Analyze specific table
  help            Show this help

Environment Variables:
  PGHOST      Database host [default: localhost]
  PGPORT      Database port [default: 5432]
  PGUSER      Database user [default: postgres]
  PGDATABASE  Database name [default: orochi_bench]
  PGPASSWORD  Database password

Metrics:
  - RSD (Relative Standard Deviation): Target < 10%
  - Hotspot: Shard with > 2x mean rows
  - Cold Spot: Shard with < 0.5x mean rows
  - Imbalance Ratio: max_rows / min_rows
EOF
            ;;

        *)
            echo "Unknown command: $command"
            echo "Run '$0 help' for usage"
            exit 1
            ;;
    esac
}

main "$@"
