#!/bin/sh
#
# auto_scale_params.sh - Auto-scaling parameter calculator for Orochi DB benchmarks
# Calculates optimal benchmark parameters based on detected hardware
#
# Usage: ./auto_scale_params.sh [--hardware=FILE] [--output=FILE] [--profile=PROFILE]
#
# Scaling Rules:
#   RAM < 4GB:    scale=1,  clients=2
#   RAM 4-16GB:   scale=10, clients=cores
#   RAM 16-64GB:  scale=50, clients=cores*2
#   RAM > 64GB:   scale=100, clients=cores*4
#
# POSIX-compatible for portability

set -e

# Default files
HARDWARE_FILE="${OROCHI_HARDWARE_FILE:-hardware_info.json}"
OUTPUT_FILE="${OROCHI_PARAMS_FILE:-benchmark_params.env}"
PROFILE_OVERRIDE=""
SCRIPT_DIR=$(dirname "$0")

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --hardware=*)
            HARDWARE_FILE="${arg#*=}"
            ;;
        --output=*)
            OUTPUT_FILE="${arg#*=}"
            ;;
        --profile=*)
            PROFILE_OVERRIDE="${arg#*=}"
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --hardware=FILE   Hardware info JSON file [default: hardware_info.json]"
            echo "  --output=FILE     Output ENV file [default: benchmark_params.env]"
            echo "  --profile=NAME    Force specific profile (quick, standard, stress, cloud-small, cloud-large)"
            echo ""
            echo "Environment variables:"
            echo "  OROCHI_SCALE_FACTOR    Override scale factor"
            echo "  OROCHI_CLIENTS         Override client count"
            echo "  OROCHI_THREADS         Override thread count"
            echo "  OROCHI_DURATION        Override duration"
            exit 0
            ;;
    esac
done

# Helper: Extract JSON value (simple grep-based implementation)
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
    # Handle both "key": "value" and "key": value
    VALUE=$(echo "$LINE" | sed -E 's/.*"'"$KEY"'"[[:space:]]*:[[:space:]]*"?([^",}]*)"?.*/\1/')
    echo "$VALUE"
}

# Helper: Extract numeric JSON value
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

# ============================================================================
# Hardware Detection
# ============================================================================

# Run hardware detection if file doesn't exist
if [ ! -f "$HARDWARE_FILE" ]; then
    echo "Hardware info not found, running detection..."
    if [ -x "${SCRIPT_DIR}/detect_hardware.sh" ]; then
        "${SCRIPT_DIR}/detect_hardware.sh" --output="$HARDWARE_FILE" >/dev/null
    else
        echo "Error: detect_hardware.sh not found or not executable"
        exit 1
    fi
fi

# Extract hardware info
PHYSICAL_CORES=$(json_number "physical_cores" "$HARDWARE_FILE")
LOGICAL_CORES=$(json_number "logical_cores" "$HARDWARE_FILE")
TOTAL_RAM_GB=$(json_number "total_gb" "$HARDWARE_FILE")
AVAILABLE_RAM_GB=$(json_number "available_gb" "$HARDWARE_FILE")
DISK_TYPE=$(json_value "type" "$HARDWARE_FILE" | grep -E "nvme|ssd|hdd" | head -1)
DISK_IOPS=$(json_number "estimated_iops" "$HARDWARE_FILE")
CLOUD_PROVIDER=$(json_value "provider" "$HARDWARE_FILE")

# Set defaults if values are empty or zero
: "${PHYSICAL_CORES:=2}"
: "${LOGICAL_CORES:=4}"
: "${TOTAL_RAM_GB:=8}"
: "${AVAILABLE_RAM_GB:=4}"
: "${DISK_TYPE:=unknown}"
: "${DISK_IOPS:=1000}"
: "${CLOUD_PROVIDER:=local}"

# Ensure we have at least 1 core
[ "$PHYSICAL_CORES" -lt 1 ] 2>/dev/null && PHYSICAL_CORES=1
[ "$LOGICAL_CORES" -lt 1 ] 2>/dev/null && LOGICAL_CORES=1

echo "Detected Hardware:"
echo "  Physical Cores: $PHYSICAL_CORES"
echo "  Logical Cores:  $LOGICAL_CORES"
echo "  Total RAM:      ${TOTAL_RAM_GB}GB"
echo "  Available RAM:  ${AVAILABLE_RAM_GB}GB"
echo "  Disk Type:      $DISK_TYPE"
echo "  Estimated IOPS: $DISK_IOPS"
echo "  Cloud Provider: $CLOUD_PROVIDER"
echo ""

# ============================================================================
# Calculate Parameters
# ============================================================================

# Scale factor based on RAM
# Rule: RAM < 4GB: scale=1, 4-16GB: scale=10, 16-64GB: scale=50, >64GB: scale=100
calculate_scale_factor() {
    RAM_GB=$1

    if [ "$RAM_GB" -lt 4 ]; then
        echo 1
    elif [ "$RAM_GB" -lt 16 ]; then
        echo 10
    elif [ "$RAM_GB" -lt 64 ]; then
        echo 50
    else
        echo 100
    fi
}

# Client count based on CPU cores and RAM tier
# Rule: RAM < 4GB: 2, 4-16GB: cores, 16-64GB: cores*2, >64GB: cores*4
calculate_clients() {
    RAM_GB=$1
    CORES=$2

    if [ "$RAM_GB" -lt 4 ]; then
        echo 2
    elif [ "$RAM_GB" -lt 16 ]; then
        echo "$CORES"
    elif [ "$RAM_GB" -lt 64 ]; then
        echo $((CORES * 2))
    else
        echo $((CORES * 4))
    fi
}

# Thread count (usually 1 thread per client, but can be tuned for I/O workloads)
calculate_threads() {
    CLIENTS=$1
    DISK=$2

    # For NVMe, we can afford more threads per client
    case "$DISK" in
        nvme)
            echo 2
            ;;
        ssd)
            echo 1
            ;;
        *)
            echo 1
            ;;
    esac
}

# Duration based on environment
# Cloud: longer runs for stable results (cloud has more variance)
# Local: shorter runs acceptable
calculate_duration() {
    PROVIDER=$1
    SCALE=$2

    case "$PROVIDER" in
        aws|gcp|azure|digitalocean)
            # Cloud environments - longer duration
            case "$SCALE" in
                1)   echo 120 ;;
                10)  echo 300 ;;
                50)  echo 600 ;;
                100) echo 900 ;;
                *)   echo 300 ;;
            esac
            ;;
        docker|podman|kubernetes)
            # Container environments - medium duration
            case "$SCALE" in
                1)   echo 60 ;;
                10)  echo 180 ;;
                50)  echo 300 ;;
                100) echo 600 ;;
                *)   echo 180 ;;
            esac
            ;;
        *)
            # Local - shorter duration
            case "$SCALE" in
                1)   echo 60 ;;
                10)  echo 120 ;;
                50)  echo 300 ;;
                100) echo 600 ;;
                *)   echo 120 ;;
            esac
            ;;
    esac
}

# Warmup based on scale
calculate_warmup() {
    SCALE=$1

    case "$SCALE" in
        1)   echo 5 ;;
        10)  echo 15 ;;
        50)  echo 30 ;;
        100) echo 60 ;;
        *)   echo 10 ;;
    esac
}

# PostgreSQL shared_buffers recommendation (25% of RAM, capped at 8GB)
calculate_shared_buffers() {
    RAM_MB=$1

    SHARED=$((RAM_MB / 4))
    # Cap at 8GB
    if [ "$SHARED" -gt 8192 ]; then
        SHARED=8192
    fi
    # Minimum 128MB
    if [ "$SHARED" -lt 128 ]; then
        SHARED=128
    fi
    echo "${SHARED}MB"
}

# PostgreSQL work_mem recommendation
calculate_work_mem() {
    RAM_MB=$1
    CLIENTS=$2

    # Total work_mem pool = 25% of RAM / max_connections
    POOL=$((RAM_MB / 4))
    WORK_MEM=$((POOL / CLIENTS / 4))

    # Minimum 4MB, maximum 256MB
    if [ "$WORK_MEM" -lt 4 ]; then
        WORK_MEM=4
    elif [ "$WORK_MEM" -gt 256 ]; then
        WORK_MEM=256
    fi
    echo "${WORK_MEM}MB"
}

# ============================================================================
# Calculate Final Parameters
# ============================================================================

SCALE_FACTOR=$(calculate_scale_factor "$AVAILABLE_RAM_GB")
NUM_CLIENTS=$(calculate_clients "$AVAILABLE_RAM_GB" "$PHYSICAL_CORES")
NUM_THREADS=$(calculate_threads "$NUM_CLIENTS" "$DISK_TYPE")
DURATION=$(calculate_duration "$CLOUD_PROVIDER" "$SCALE_FACTOR")
WARMUP=$(calculate_warmup "$SCALE_FACTOR")

# PostgreSQL tuning
RAM_MB=$((TOTAL_RAM_GB * 1024))
SHARED_BUFFERS=$(calculate_shared_buffers "$RAM_MB")
WORK_MEM=$(calculate_work_mem "$RAM_MB" "$NUM_CLIENTS")

# Maximum connections (clients * 1.5 + buffer)
MAX_CONNECTIONS=$((NUM_CLIENTS * 3 / 2 + 10))

# Effective cache size (75% of RAM)
EFFECTIVE_CACHE=$((TOTAL_RAM_GB * 3 / 4))
if [ "$EFFECTIVE_CACHE" -lt 1 ]; then
    EFFECTIVE_CACHE=1
fi
EFFECTIVE_CACHE_SIZE="${EFFECTIVE_CACHE}GB"

# ============================================================================
# Profile Override
# ============================================================================

if [ -n "$PROFILE_OVERRIDE" ]; then
    echo "Using profile override: $PROFILE_OVERRIDE"

    PROFILES_FILE="${SCRIPT_DIR}/benchmark_profiles.json"
    if [ -f "$PROFILES_FILE" ]; then
        # Extract profile values
        PROFILE_SCALE=$(grep -A10 "\"$PROFILE_OVERRIDE\"" "$PROFILES_FILE" | grep '"scale"' | head -1 | sed 's/[^0-9]//g')
        PROFILE_DURATION=$(grep -A10 "\"$PROFILE_OVERRIDE\"" "$PROFILES_FILE" | grep '"duration"' | head -1 | sed 's/[^0-9]//g')
        PROFILE_CLIENTS=$(grep -A10 "\"$PROFILE_OVERRIDE\"" "$PROFILES_FILE" | grep '"clients"' | head -1 | sed 's/[^0-9]//g')

        [ -n "$PROFILE_SCALE" ] && SCALE_FACTOR=$PROFILE_SCALE
        [ -n "$PROFILE_DURATION" ] && DURATION=$PROFILE_DURATION
        [ -n "$PROFILE_CLIENTS" ] && NUM_CLIENTS=$PROFILE_CLIENTS
    else
        # Fallback to hardcoded profiles
        case "$PROFILE_OVERRIDE" in
            quick)
                SCALE_FACTOR=1; DURATION=60; NUM_CLIENTS=4
                ;;
            standard)
                SCALE_FACTOR=10; DURATION=300; NUM_CLIENTS=16
                ;;
            stress)
                SCALE_FACTOR=100; DURATION=3600; NUM_CLIENTS=64
                ;;
            cloud-small)
                SCALE_FACTOR=10; DURATION=600; NUM_CLIENTS=8
                ;;
            cloud-large)
                SCALE_FACTOR=100; DURATION=1800; NUM_CLIENTS=32
                ;;
            *)
                echo "Warning: Unknown profile '$PROFILE_OVERRIDE', using calculated values"
                ;;
        esac
    fi

    # Recalculate dependent values
    WARMUP=$(calculate_warmup "$SCALE_FACTOR")
fi

# ============================================================================
# Environment Variable Overrides
# ============================================================================

[ -n "$OROCHI_SCALE_FACTOR" ] && SCALE_FACTOR="$OROCHI_SCALE_FACTOR"
[ -n "$OROCHI_CLIENTS" ] && NUM_CLIENTS="$OROCHI_CLIENTS"
[ -n "$OROCHI_THREADS" ] && NUM_THREADS="$OROCHI_THREADS"
[ -n "$OROCHI_DURATION" ] && DURATION="$OROCHI_DURATION"
[ -n "$OROCHI_WARMUP" ] && WARMUP="$OROCHI_WARMUP"

# ============================================================================
# Determine Recommended Profile Name
# ============================================================================

determine_profile_name() {
    SCALE=$1
    CLIENTS=$2
    PROVIDER=$3

    case "$PROVIDER" in
        aws|gcp|azure|digitalocean)
            if [ "$SCALE" -ge 100 ]; then
                echo "cloud-large"
            else
                echo "cloud-small"
            fi
            ;;
        *)
            if [ "$SCALE" -le 1 ] && [ "$CLIENTS" -le 4 ]; then
                echo "quick"
            elif [ "$SCALE" -ge 100 ] && [ "$CLIENTS" -ge 64 ]; then
                echo "stress"
            else
                echo "standard"
            fi
            ;;
    esac
}

RECOMMENDED_PROFILE=$(determine_profile_name "$SCALE_FACTOR" "$NUM_CLIENTS" "$CLOUD_PROVIDER")

# ============================================================================
# Output
# ============================================================================

echo "Calculated Parameters:"
echo "  Scale Factor:    $SCALE_FACTOR"
echo "  Clients:         $NUM_CLIENTS"
echo "  Threads:         $NUM_THREADS"
echo "  Duration:        ${DURATION}s"
echo "  Warmup:          ${WARMUP}s"
echo "  Profile:         $RECOMMENDED_PROFILE"
echo ""
echo "PostgreSQL Recommendations:"
echo "  shared_buffers:      $SHARED_BUFFERS"
echo "  work_mem:            $WORK_MEM"
echo "  max_connections:     $MAX_CONNECTIONS"
echo "  effective_cache_size: $EFFECTIVE_CACHE_SIZE"
echo ""

# Write ENV file
cat > "$OUTPUT_FILE" << EOF
# Orochi DB Benchmark Parameters
# Generated: $(date -Iseconds 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S')
# Hardware: ${PHYSICAL_CORES} cores, ${TOTAL_RAM_GB}GB RAM, ${DISK_TYPE} disk
# Provider: ${CLOUD_PROVIDER}

# Benchmark Parameters
OROCHI_SCALE_FACTOR=$SCALE_FACTOR
OROCHI_NUM_CLIENTS=$NUM_CLIENTS
OROCHI_NUM_THREADS=$NUM_THREADS
OROCHI_DURATION=$DURATION
OROCHI_WARMUP=$WARMUP
OROCHI_PROFILE=$RECOMMENDED_PROFILE

# Hardware Info
OROCHI_PHYSICAL_CORES=$PHYSICAL_CORES
OROCHI_LOGICAL_CORES=$LOGICAL_CORES
OROCHI_TOTAL_RAM_GB=$TOTAL_RAM_GB
OROCHI_AVAILABLE_RAM_GB=$AVAILABLE_RAM_GB
OROCHI_DISK_TYPE=$DISK_TYPE
OROCHI_DISK_IOPS=$DISK_IOPS
OROCHI_CLOUD_PROVIDER=$CLOUD_PROVIDER

# PostgreSQL Tuning Recommendations
OROCHI_PG_SHARED_BUFFERS=$SHARED_BUFFERS
OROCHI_PG_WORK_MEM=$WORK_MEM
OROCHI_PG_MAX_CONNECTIONS=$MAX_CONNECTIONS
OROCHI_PG_EFFECTIVE_CACHE_SIZE=$EFFECTIVE_CACHE_SIZE

# Compatibility exports (for run_all.sh)
SCALE_FACTOR=$SCALE_FACTOR
NUM_CLIENTS=$NUM_CLIENTS
NUM_THREADS=$NUM_THREADS
DURATION=$DURATION
WARMUP=$WARMUP
EOF

echo "Parameters written to: $OUTPUT_FILE"
echo ""
echo "Usage:"
echo "  source $OUTPUT_FILE"
echo "  ./run_all.sh --scale=\$SCALE_FACTOR --clients=\$NUM_CLIENTS --duration=\$DURATION"

exit 0
