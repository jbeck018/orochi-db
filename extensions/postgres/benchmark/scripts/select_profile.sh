#!/bin/sh
#
# select_profile.sh - Automatic benchmark profile selector
# Selects appropriate profile based on detected hardware or allows override
#
# Usage: ./select_profile.sh [--profile=NAME] [--hardware=FILE] [--list]
#
# POSIX-compatible for portability

set -e

# Default configuration
HARDWARE_FILE="${OROCHI_HARDWARE_FILE:-hardware_info.json}"
PROFILES_FILE=""
PROFILE_OVERRIDE="${OROCHI_PROFILE:-}"
SCRIPT_DIR=$(dirname "$0")
ACTION="select"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --profile=*)
            PROFILE_OVERRIDE="${arg#*=}"
            ;;
        --hardware=*)
            HARDWARE_FILE="${arg#*=}"
            ;;
        --profiles=*)
            PROFILES_FILE="${arg#*=}"
            ;;
        --list)
            ACTION="list"
            ;;
        --export)
            ACTION="export"
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --profile=NAME    Force specific profile"
            echo "  --hardware=FILE   Hardware info JSON file"
            echo "  --profiles=FILE   Profiles JSON file"
            echo "  --list            List all available profiles"
            echo "  --export          Export selected profile as shell variables"
            echo ""
            echo "Available profiles:"
            echo "  quick       - Minimal resources, fast completion (1GB scale, 60s)"
            echo "  standard    - Balanced resources and duration (10GB scale, 5min)"
            echo "  stress      - Maximum load, extended duration (100GB scale, 1hr)"
            echo "  cloud-small - Small cloud instances (10GB scale, 10min)"
            echo "  cloud-medium- Medium cloud instances (50GB scale, 15min)"
            echo "  cloud-large - Large cloud instances (100GB scale, 30min)"
            echo "  distributed - Multi-node cluster testing"
            echo "  oltp        - High concurrency transactions"
            echo "  olap        - Complex analytics queries"
            echo "  htap        - Mixed OLTP/OLAP workload"
            echo "  timeseries  - High ingestion rate testing"
            echo "  vector      - Vector similarity search"
            echo "  ci          - Fast CI/CD validation"
            echo ""
            echo "Environment variables:"
            echo "  OROCHI_PROFILE        Override profile selection"
            echo "  OROCHI_HARDWARE_FILE  Hardware info JSON file"
            exit 0
            ;;
    esac
done

# Default profiles file
if [ -z "$PROFILES_FILE" ]; then
    PROFILES_FILE="${SCRIPT_DIR}/benchmark_profiles.json"
fi

# ============================================================================
# Helper Functions
# ============================================================================

# Extract JSON value (simple grep-based implementation)
json_value() {
    KEY="$1"
    FILE="$2"
    # Find the line with the key and extract the value
    LINE=$(grep "\"$KEY\"" "$FILE" 2>/dev/null | head -1)
    if [ -z "$LINE" ]; then
        echo ""
        return
    fi
    # Extract value after the colon
    VALUE=$(echo "$LINE" | sed -E 's/.*"'"$KEY"'"[[:space:]]*:[[:space:]]*"?([^",}]*)"?.*/\1/')
    echo "$VALUE"
}

# Extract numeric JSON value
json_number() {
    KEY="$1"
    FILE="$2"
    # Find the line and extract just the number
    LINE=$(grep "\"$KEY\"" "$FILE" 2>/dev/null | head -1)
    if [ -z "$LINE" ]; then
        echo "0"
        return
    fi
    # Extract only digits from the value portion
    NUMBER=$(echo "$LINE" | sed 's/.*: *//' | tr -cd '0-9')
    if [ -z "$NUMBER" ]; then
        echo "0"
    else
        echo "$NUMBER"
    fi
}

# Extract profile property from profiles file
get_profile_value() {
    PROFILE="$1"
    KEY="$2"

    if [ -f "$PROFILES_FILE" ]; then
        # Find the profile section and extract the key
        awk -v profile="$PROFILE" -v key="$KEY" '
            /"'"$PROFILE"'"/ { in_profile=1 }
            in_profile && /"'"$KEY"'"/ {
                gsub(/[^0-9]/, "", $2)
                print $2
                exit
            }
            in_profile && /^    }/ { in_profile=0 }
        ' "$PROFILES_FILE" 2>/dev/null || echo ""
    fi
}

# ============================================================================
# List Profiles
# ============================================================================

list_profiles() {
    echo "Available Benchmark Profiles:"
    echo "=============================="
    echo ""

    # Use hardcoded list for reliable output (profiles.json is used at runtime)
    cat << 'EOF'
  quick          Scale:   1  Duration:   60s   Clients:   4  Min RAM:  1GB
                 Quick validation benchmark - minimal resources

  standard       Scale:  10  Duration:  300s   Clients:  16  Min RAM:  8GB
                 Standard benchmark - balanced resources

  stress         Scale: 100  Duration: 3600s   Clients:  64  Min RAM: 64GB
                 Stress test - maximum load, extended duration

  cloud-small    Scale:  10  Duration:  600s   Clients:   8  Min RAM:  8GB
                 For small cloud instances (t3.large, e2-standard-4)

  cloud-medium   Scale:  50  Duration:  900s   Clients:  16  Min RAM: 32GB
                 For medium cloud instances (m5.2xlarge, n2-standard-8)

  cloud-large    Scale: 100  Duration: 1800s   Clients:  32  Min RAM: 64GB
                 For large cloud instances (m5.8xlarge, n2-standard-32)

  distributed    Scale: 100  Duration: 1200s   Clients:  48  Min RAM: 32GB
                 Multi-node cluster testing

  oltp           Scale:  10  Duration:  600s   Clients:  64  Min RAM: 16GB
                 High concurrency, short transactions

  olap           Scale: 100  Duration:  900s   Clients:   8  Min RAM: 64GB
                 Complex analytics queries, columnar storage

  htap           Scale:  50  Duration:  900s   Clients:  32  Min RAM: 32GB
                 Mixed OLTP/OLAP workload (70/30 split)

  timeseries     Scale:  10  Duration:  600s   Clients:  32  Min RAM: 16GB
                 High ingestion rate testing

  vector         Scale:  10  Duration:  300s   Clients:  16  Min RAM: 32GB
                 Vector similarity search benchmark

  ci             Scale:   1  Duration:   30s   Clients:   2  Min RAM:  1GB
                 Fast CI/CD pipeline validation
EOF
    return 0

    # Original dynamic parsing (kept for reference but not used)
    if [ -f "$PROFILES_FILE" ]; then
        # Fallback hardcoded list
        cat << EOF
  quick          Scale: 1    Duration: 60s    Clients: 4    Min RAM: 1GB
                 Quick validation benchmark

  standard       Scale: 10   Duration: 300s   Clients: 16   Min RAM: 8GB
                 Standard benchmark - balanced resources

  stress         Scale: 100  Duration: 3600s  Clients: 64   Min RAM: 64GB
                 Stress test - maximum load

  cloud-small    Scale: 10   Duration: 600s   Clients: 8    Min RAM: 8GB
                 For small cloud instances

  cloud-large    Scale: 100  Duration: 1800s  Clients: 32   Min RAM: 64GB
                 For large cloud instances

  ci             Scale: 1    Duration: 30s    Clients: 2    Min RAM: 1GB
                 Fast CI/CD validation
EOF
    fi
}

# ============================================================================
# Profile Selection Logic
# ============================================================================

select_profile_for_hardware() {
    RAM_GB=$1
    CORES=$2
    PROVIDER=$3
    DISK_TYPE=$4
    DISK_IOPS=$5

    # Cloud provider selection
    case "$PROVIDER" in
        aws|gcp|azure|digitalocean)
            # Select cloud profile based on RAM
            if [ "$RAM_GB" -lt 8 ]; then
                echo "cloud-small"
            elif [ "$RAM_GB" -lt 64 ]; then
                echo "cloud-medium"
            else
                echo "cloud-large"
            fi
            return
            ;;
        docker|podman|kubernetes)
            # Container environment - use quick or standard
            if [ "$RAM_GB" -lt 4 ]; then
                echo "quick"
            else
                echo "standard"
            fi
            return
            ;;
    esac

    # Local machine selection based on RAM
    # RAM < 4GB:    quick
    # RAM 4-16GB:   standard
    # RAM 16-64GB:  standard or htap (depending on cores)
    # RAM > 64GB:   stress or olap

    if [ "$RAM_GB" -lt 4 ]; then
        echo "quick"
    elif [ "$RAM_GB" -lt 16 ]; then
        echo "standard"
    elif [ "$RAM_GB" -lt 64 ]; then
        # Check if we have enough cores for HTAP
        if [ "$CORES" -ge 8 ]; then
            echo "htap"
        else
            echo "standard"
        fi
    else
        # High-end machine
        if [ "$CORES" -ge 16 ]; then
            # Check for fast storage for OLAP
            if [ "$DISK_IOPS" -gt 50000 ]; then
                echo "olap"
            else
                echo "stress"
            fi
        else
            echo "stress"
        fi
    fi
}

# ============================================================================
# Main Logic
# ============================================================================

if [ "$ACTION" = "list" ]; then
    list_profiles
    exit 0
fi

# Run hardware detection if needed
if [ ! -f "$HARDWARE_FILE" ]; then
    if [ -x "${SCRIPT_DIR}/detect_hardware.sh" ]; then
        "${SCRIPT_DIR}/detect_hardware.sh" --output="$HARDWARE_FILE" >/dev/null 2>&1
    fi
fi

# Extract hardware info
if [ -f "$HARDWARE_FILE" ]; then
    PHYSICAL_CORES=$(json_number "physical_cores" "$HARDWARE_FILE")
    TOTAL_RAM_GB=$(json_number "total_gb" "$HARDWARE_FILE")
    AVAILABLE_RAM_GB=$(json_number "available_gb" "$HARDWARE_FILE")
    DISK_TYPE=$(json_value "type" "$HARDWARE_FILE" | grep -E "nvme|ssd|hdd" | head -1)
    DISK_IOPS=$(json_number "estimated_iops" "$HARDWARE_FILE")
    CLOUD_PROVIDER=$(json_value "provider" "$HARDWARE_FILE")
else
    # Fallback defaults
    PHYSICAL_CORES=4
    TOTAL_RAM_GB=8
    AVAILABLE_RAM_GB=4
    DISK_TYPE="unknown"
    DISK_IOPS=1000
    CLOUD_PROVIDER="local"
fi

# Set defaults if values are empty
: "${PHYSICAL_CORES:=4}"
: "${TOTAL_RAM_GB:=8}"
: "${AVAILABLE_RAM_GB:=4}"
: "${DISK_TYPE:=unknown}"
: "${DISK_IOPS:=1000}"
: "${CLOUD_PROVIDER:=local}"

# Select profile
if [ -n "$PROFILE_OVERRIDE" ]; then
    SELECTED_PROFILE="$PROFILE_OVERRIDE"
    SELECTION_METHOD="override"
else
    SELECTED_PROFILE=$(select_profile_for_hardware "$AVAILABLE_RAM_GB" "$PHYSICAL_CORES" "$CLOUD_PROVIDER" "$DISK_TYPE" "$DISK_IOPS")
    SELECTION_METHOD="auto"
fi

# Get profile parameters
if [ -f "$PROFILES_FILE" ]; then
    # Extract from JSON
    SCALE=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"scale"' | head -1 | sed 's/[^0-9]//g')
    DURATION=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"duration"' | head -1 | sed 's/[^0-9]//g')
    CLIENTS=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"clients"' | head -1 | sed 's/[^0-9]//g')
    THREADS=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"threads"' | head -1 | sed 's/[^0-9]//g')
    WARMUP=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"warmup"' | head -1 | sed 's/[^0-9]//g')
    DESCRIPTION=$(grep -A15 "\"$SELECTED_PROFILE\":" "$PROFILES_FILE" 2>/dev/null | grep '"description"' | head -1 | sed 's/.*: *"\([^"]*\)".*/\1/')
fi

# Fallback to hardcoded values
case "$SELECTED_PROFILE" in
    quick)
        : "${SCALE:=1}"; : "${DURATION:=60}"; : "${CLIENTS:=4}"; : "${THREADS:=1}"; : "${WARMUP:=5}"
        : "${DESCRIPTION:=Quick validation benchmark}"
        ;;
    standard)
        : "${SCALE:=10}"; : "${DURATION:=300}"; : "${CLIENTS:=16}"; : "${THREADS:=1}"; : "${WARMUP:=30}"
        : "${DESCRIPTION:=Standard benchmark}"
        ;;
    stress)
        : "${SCALE:=100}"; : "${DURATION:=3600}"; : "${CLIENTS:=64}"; : "${THREADS:=2}"; : "${WARMUP:=60}"
        : "${DESCRIPTION:=Stress test benchmark}"
        ;;
    cloud-small)
        : "${SCALE:=10}"; : "${DURATION:=600}"; : "${CLIENTS:=8}"; : "${THREADS:=1}"; : "${WARMUP:=30}"
        : "${DESCRIPTION:=Cloud benchmark for small instances}"
        ;;
    cloud-medium)
        : "${SCALE:=50}"; : "${DURATION:=900}"; : "${CLIENTS:=16}"; : "${THREADS:=2}"; : "${WARMUP:=45}"
        : "${DESCRIPTION:=Cloud benchmark for medium instances}"
        ;;
    cloud-large)
        : "${SCALE:=100}"; : "${DURATION:=1800}"; : "${CLIENTS:=32}"; : "${THREADS:=2}"; : "${WARMUP:=60}"
        : "${DESCRIPTION:=Cloud benchmark for large instances}"
        ;;
    ci)
        : "${SCALE:=1}"; : "${DURATION:=30}"; : "${CLIENTS:=2}"; : "${THREADS:=1}"; : "${WARMUP:=5}"
        : "${DESCRIPTION:=CI/CD pipeline benchmark}"
        ;;
    *)
        # Default to standard if unknown
        : "${SCALE:=10}"; : "${DURATION:=300}"; : "${CLIENTS:=16}"; : "${THREADS:=1}"; : "${WARMUP:=30}"
        : "${DESCRIPTION:=Unknown profile - using standard defaults}"
        ;;
esac

# ============================================================================
# Output
# ============================================================================

if [ "$ACTION" = "export" ]; then
    # Export as shell variables
    cat << EOF
# Orochi DB Benchmark Profile: $SELECTED_PROFILE
# Selection method: $SELECTION_METHOD
# Hardware: ${PHYSICAL_CORES} cores, ${TOTAL_RAM_GB}GB RAM, ${CLOUD_PROVIDER}

export OROCHI_PROFILE="$SELECTED_PROFILE"
export OROCHI_SCALE_FACTOR="$SCALE"
export OROCHI_NUM_CLIENTS="$CLIENTS"
export OROCHI_NUM_THREADS="$THREADS"
export OROCHI_DURATION="$DURATION"
export OROCHI_WARMUP="$WARMUP"

# For run_all.sh compatibility
export SCALE_FACTOR="$SCALE"
export NUM_CLIENTS="$CLIENTS"
export NUM_THREADS="$THREADS"
export DURATION="$DURATION"
export WARMUP="$WARMUP"
EOF
else
    # Human-readable output
    echo "Benchmark Profile Selection"
    echo "==========================="
    echo ""
    echo "Hardware Detected:"
    echo "  CPU Cores:     $PHYSICAL_CORES"
    echo "  Total RAM:     ${TOTAL_RAM_GB}GB"
    echo "  Available RAM: ${AVAILABLE_RAM_GB}GB"
    echo "  Disk Type:     $DISK_TYPE"
    echo "  Provider:      $CLOUD_PROVIDER"
    echo ""
    echo "Selected Profile: $SELECTED_PROFILE"
    echo "Selection Method: $SELECTION_METHOD"
    echo "Description:      $DESCRIPTION"
    echo ""
    echo "Profile Parameters:"
    echo "  Scale Factor:  $SCALE"
    echo "  Duration:      ${DURATION}s"
    echo "  Clients:       $CLIENTS"
    echo "  Threads:       $THREADS"
    echo "  Warmup:        ${WARMUP}s"
    echo ""
    echo "Usage:"
    echo "  eval \"\$($0 --export)\" && ./run_all.sh"
    echo ""
    echo "Or with run_all.sh directly:"
    echo "  ./run_all.sh --scale=$SCALE --clients=$CLIENTS --duration=$DURATION"
fi

exit 0
