#!/bin/bash
#
# Orochi DB Benchmark Report Generator
#
# Generates comprehensive benchmark reports with charts and analysis
# from JSON result files.
#
# Usage:
#   ./generate_report.sh [OPTIONS]
#
# Options:
#   -r, --results-dir DIR    Directory containing result JSON files (default: ../results)
#   -o, --output-dir DIR     Output directory for reports (default: ./output)
#   -f, --format FORMAT      Output format: html, markdown, all (default: all)
#   -c, --charts             Generate charts (requires matplotlib)
#   -s, --serve              Start local web server after generation
#   -p, --port PORT          Web server port (default: 8000)
#   -h, --help               Show this help message

set -e

# Default values
RESULTS_DIR="../results"
OUTPUT_DIR="./output"
FORMAT="all"
GENERATE_CHARTS=false
SERVE=false
PORT=8000

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_usage() {
    cat << EOF
Orochi DB Benchmark Report Generator

Usage: $(basename "$0") [OPTIONS]

Options:
  -r, --results-dir DIR    Directory containing result JSON files (default: ../results)
  -o, --output-dir DIR     Output directory for reports (default: ./output)
  -f, --format FORMAT      Output format: html, markdown, all (default: all)
  -c, --charts             Generate charts (requires matplotlib)
  -s, --serve              Start local web server after generation
  -p, --port PORT          Web server port (default: 8000)
  -h, --help               Show this help message

Examples:
  $(basename "$0") -r ./results -o ./report -c
  $(basename "$0") --results-dir /path/to/results --format html --serve
  $(basename "$0") -c -s -p 9000
EOF
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--results-dir)
            RESULTS_DIR="$2"
            shift 2
            ;;
        -o|--output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -f|--format)
            FORMAT="$2"
            shift 2
            ;;
        -c|--charts)
            GENERATE_CHARTS=true
            shift
            ;;
        -s|--serve)
            SERVE=true
            shift
            ;;
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Check Python availability
check_python() {
    if command -v python3 &> /dev/null; then
        PYTHON=python3
    elif command -v python &> /dev/null; then
        PYTHON=python
    else
        log_error "Python is not installed. Please install Python 3.8+."
        exit 1
    fi

    log_info "Using Python: $($PYTHON --version)"
}

# Check for required Python packages
check_dependencies() {
    log_info "Checking Python dependencies..."

    local missing_deps=()

    # Check core dependencies
    $PYTHON -c "import json" 2>/dev/null || missing_deps+=("json (built-in)")

    if $GENERATE_CHARTS; then
        $PYTHON -c "import matplotlib" 2>/dev/null || missing_deps+=("matplotlib")
        $PYTHON -c "import numpy" 2>/dev/null || missing_deps+=("numpy")
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_warning "Missing dependencies: ${missing_deps[*]}"
        log_info "Installing dependencies..."
        $PYTHON -m pip install -q -r "$SCRIPT_DIR/requirements.txt" 2>/dev/null || {
            log_warning "Could not install all dependencies. Some features may be unavailable."
        }
    fi
}

# Create output directory structure
setup_output_dir() {
    log_info "Setting up output directory: $OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR/charts"
    mkdir -p "$OUTPUT_DIR/data"
}

# Aggregate results
aggregate_results() {
    log_info "Aggregating benchmark results from: $RESULTS_DIR"

    if [ ! -d "$RESULTS_DIR" ]; then
        log_warning "Results directory not found: $RESULTS_DIR"
        log_info "Creating sample data for demonstration..."
        mkdir -p "$RESULTS_DIR"
        create_sample_data
    fi

    # Count JSON files
    local json_count=$(find "$RESULTS_DIR" -name "*.json" 2>/dev/null | wc -l | tr -d ' ')

    if [ "$json_count" -eq 0 ]; then
        log_warning "No JSON result files found in $RESULTS_DIR"
        log_info "Creating sample data for demonstration..."
        create_sample_data
    else
        log_info "Found $json_count JSON result file(s)"
    fi

    $PYTHON "$SCRIPT_DIR/results_aggregator.py" \
        --results-dir "$RESULTS_DIR" \
        --output "$OUTPUT_DIR/data/summary.json"
}

# Create sample data for demonstration
create_sample_data() {
    mkdir -p "$RESULTS_DIR"

    cat > "$RESULTS_DIR/sample_results.json" << 'SAMPLE_EOF'
{
    "name": "Orochi DB Benchmark Suite",
    "version": "1.0.0",
    "timestamp": "2024-01-15T10:30:00Z",
    "config": {
        "iterations": 100,
        "warmup": 10,
        "threads": 4
    },
    "system_info": {
        "os": "Linux",
        "cpu": "AMD EPYC 7R13",
        "cores": 8,
        "memory_gb": 32,
        "postgres_version": "16.1"
    },
    "results": [
        {
            "name": "tpch_q1",
            "success": true,
            "total_queries": 100,
            "failed_queries": 0,
            "total_rows": 150000,
            "latency": {
                "avg_us": 45000,
                "min_us": 35000,
                "max_us": 85000,
                "p95_us": 65000,
                "p99_us": 78000
            },
            "throughput": {
                "ops_per_sec": 22.2,
                "rows_per_sec": 3333
            }
        },
        {
            "name": "tpch_q6",
            "success": true,
            "total_queries": 100,
            "failed_queries": 2,
            "total_rows": 50000,
            "latency": {
                "avg_us": 12000,
                "min_us": 8000,
                "max_us": 25000,
                "p95_us": 18000,
                "p99_us": 22000
            },
            "throughput": {
                "ops_per_sec": 83.3,
                "rows_per_sec": 4166
            }
        },
        {
            "name": "timeseries_insert",
            "success": true,
            "total_queries": 10000,
            "failed_queries": 0,
            "total_rows": 10000,
            "latency": {
                "avg_us": 500,
                "min_us": 200,
                "max_us": 5000,
                "p95_us": 1500,
                "p99_us": 3000
            },
            "throughput": {
                "ops_per_sec": 2000,
                "rows_per_sec": 2000
            }
        },
        {
            "name": "timeseries_range_query",
            "success": true,
            "total_queries": 1000,
            "failed_queries": 5,
            "total_rows": 500000,
            "latency": {
                "avg_us": 8500,
                "min_us": 5000,
                "max_us": 20000,
                "p95_us": 15000,
                "p99_us": 18000
            },
            "throughput": {
                "ops_per_sec": 117.6,
                "rows_per_sec": 58823
            }
        },
        {
            "name": "columnar_scan",
            "success": true,
            "total_queries": 500,
            "failed_queries": 0,
            "total_rows": 5000000,
            "latency": {
                "avg_us": 25000,
                "min_us": 18000,
                "max_us": 45000,
                "p95_us": 38000,
                "p99_us": 42000
            },
            "throughput": {
                "ops_per_sec": 40.0,
                "rows_per_sec": 200000
            }
        },
        {
            "name": "distributed_query",
            "success": true,
            "total_queries": 200,
            "failed_queries": 3,
            "total_rows": 100000,
            "latency": {
                "avg_us": 55000,
                "min_us": 40000,
                "max_us": 120000,
                "p95_us": 95000,
                "p99_us": 110000
            },
            "throughput": {
                "ops_per_sec": 18.2,
                "rows_per_sec": 1818
            }
        },
        {
            "name": "multitenant_isolation",
            "success": true,
            "total_queries": 1000,
            "failed_queries": 0,
            "total_rows": 10000,
            "latency": {
                "avg_us": 1200,
                "min_us": 800,
                "max_us": 5000,
                "p95_us": 2500,
                "p99_us": 4000
            },
            "throughput": {
                "ops_per_sec": 833.3,
                "rows_per_sec": 8333
            }
        },
        {
            "name": "sharding_rebalance",
            "success": true,
            "total_queries": 50,
            "failed_queries": 1,
            "total_rows": 1000000,
            "latency": {
                "avg_us": 250000,
                "min_us": 180000,
                "max_us": 500000,
                "p95_us": 420000,
                "p99_us": 480000
            },
            "throughput": {
                "ops_per_sec": 4.0,
                "rows_per_sec": 4000
            }
        },
        {
            "name": "connection_pool_stress",
            "success": true,
            "total_queries": 50000,
            "failed_queries": 12,
            "total_rows": 50000,
            "latency": {
                "avg_us": 350,
                "min_us": 100,
                "max_us": 8000,
                "p95_us": 1200,
                "p99_us": 3500
            },
            "throughput": {
                "ops_per_sec": 2857,
                "rows_per_sec": 2857
            }
        }
    ]
}
SAMPLE_EOF

    log_success "Created sample data in $RESULTS_DIR/sample_results.json"
}

# Generate charts
generate_charts() {
    if ! $GENERATE_CHARTS; then
        log_info "Skipping chart generation (use -c to enable)"
        return
    fi

    log_info "Generating charts..."

    $PYTHON "$SCRIPT_DIR/chart_generator.py" \
        --input "$OUTPUT_DIR/data/summary.json" \
        --output-dir "$OUTPUT_DIR/charts" \
        --chart all || {
        log_warning "Chart generation failed. Continuing without charts."
    }
}

# Generate reports
generate_reports() {
    log_info "Generating reports (format: $FORMAT)..."

    local format_args=""
    case $FORMAT in
        html)
            format_args="--format html"
            ;;
        markdown)
            format_args="--format markdown"
            ;;
        all|both)
            format_args="--format both"
            ;;
        *)
            log_error "Unknown format: $FORMAT"
            exit 1
            ;;
    esac

    local charts_arg=""
    if $GENERATE_CHARTS && [ -d "$OUTPUT_DIR/charts" ]; then
        charts_arg="--charts-dir $OUTPUT_DIR/charts"
    fi

    $PYTHON "$SCRIPT_DIR/report_generator.py" \
        --input "$OUTPUT_DIR/data/summary.json" \
        --output "$OUTPUT_DIR/index" \
        $format_args \
        $charts_arg
}

# Start web server
start_server() {
    if ! $SERVE; then
        return
    fi

    log_info "Starting local web server on port $PORT..."

    cd "$OUTPUT_DIR"

    if [ -f "index.html" ]; then
        log_success "Dashboard available at: http://localhost:$PORT"
        log_info "Press Ctrl+C to stop the server"
        $PYTHON -m http.server $PORT
    else
        log_warning "No index.html found. Serving directory listing."
        $PYTHON -m http.server $PORT
    fi
}

# Print summary
print_summary() {
    echo ""
    echo "========================================"
    echo "  Benchmark Report Generation Complete"
    echo "========================================"
    echo ""
    echo "Output directory: $OUTPUT_DIR"
    echo ""

    if [ -f "$OUTPUT_DIR/index.html" ]; then
        echo "Generated files:"
        echo "  - index.html (HTML Dashboard)"
    fi

    if [ -f "$OUTPUT_DIR/report.md" ]; then
        echo "  - report.md (Markdown Report)"
    fi

    if [ -d "$OUTPUT_DIR/charts" ] && [ "$(ls -A "$OUTPUT_DIR/charts" 2>/dev/null)" ]; then
        echo "  - charts/ (Visualization Charts)"
        ls "$OUTPUT_DIR/charts" 2>/dev/null | sed 's/^/      /'
    fi

    if [ -f "$OUTPUT_DIR/data/summary.json" ]; then
        echo "  - data/summary.json (Aggregated Data)"
    fi

    echo ""

    if ! $SERVE; then
        echo "To view the dashboard:"
        echo "  cd $OUTPUT_DIR && python3 -m http.server 8000"
        echo "  Then open: http://localhost:8000"
    fi

    echo ""
}

# Main execution
main() {
    echo ""
    echo "========================================"
    echo "  Orochi DB Benchmark Report Generator"
    echo "========================================"
    echo ""

    check_python
    check_dependencies
    setup_output_dir
    aggregate_results
    generate_charts
    generate_reports
    print_summary
    start_server
}

main
