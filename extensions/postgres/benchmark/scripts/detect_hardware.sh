#!/bin/sh
#
# detect_hardware.sh - Hardware detection for Orochi DB benchmarks
# Detects CPU, RAM, disk, network capabilities and cloud provider
#
# Usage: ./detect_hardware.sh [--output=FILE]
# Output: JSON file with all hardware information
#
# POSIX-compatible for portability

set -e

# Default output file
OUTPUT_FILE="${OROCHI_HARDWARE_FILE:-hardware_info.json}"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --output=*)
            OUTPUT_FILE="${arg#*=}"
            ;;
        --help)
            echo "Usage: $0 [--output=FILE]"
            echo "Detects hardware capabilities and outputs JSON"
            exit 0
            ;;
    esac
done

# Helper: Check if command exists
cmd_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Helper: Safe numeric extraction
extract_number() {
    echo "$1" | sed 's/[^0-9.]//g' | head -1
}

# Detect OS type
detect_os() {
    case "$(uname -s)" in
        Linux*)   echo "linux" ;;
        Darwin*)  echo "macos" ;;
        FreeBSD*) echo "freebsd" ;;
        *)        echo "unknown" ;;
    esac
}

OS_TYPE=$(detect_os)

# ============================================================================
# CPU Detection
# ============================================================================

detect_cpu() {
    CPU_MODEL="unknown"
    CPU_PHYSICAL_CORES=1
    CPU_LOGICAL_CORES=1
    CPU_FREQ_MHZ=0
    HAS_AVX2="false"
    HAS_AVX512="false"
    HAS_SSE42="false"

    case "$OS_TYPE" in
        linux)
            # CPU Model
            if [ -f /proc/cpuinfo ]; then
                CPU_MODEL=$(grep -m1 "model name" /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//' || echo "unknown")

                # Logical cores (threads)
                CPU_LOGICAL_CORES=$(grep -c "^processor" /proc/cpuinfo 2>/dev/null || echo 1)

                # Physical cores
                CORES_PER_SOCKET=$(grep -m1 "cpu cores" /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//' || echo 1)
                SOCKETS=$(grep "physical id" /proc/cpuinfo 2>/dev/null | sort -u | wc -l || echo 1)
                if [ "$SOCKETS" -eq 0 ]; then SOCKETS=1; fi
                if [ -z "$CORES_PER_SOCKET" ] || [ "$CORES_PER_SOCKET" = "0" ]; then
                    CORES_PER_SOCKET=$CPU_LOGICAL_CORES
                fi
                CPU_PHYSICAL_CORES=$((CORES_PER_SOCKET * SOCKETS))

                # CPU frequency
                CPU_FREQ_MHZ=$(grep -m1 "cpu MHz" /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//' | cut -d. -f1 || echo 0)

                # CPU flags
                FLAGS=$(grep -m1 "flags" /proc/cpuinfo 2>/dev/null || echo "")
                case "$FLAGS" in
                    *avx2*)    HAS_AVX2="true" ;;
                esac
                case "$FLAGS" in
                    *avx512f*) HAS_AVX512="true" ;;
                esac
                case "$FLAGS" in
                    *sse4_2*)  HAS_SSE42="true" ;;
                esac
            fi

            # Try lscpu as fallback
            if cmd_exists lscpu; then
                if [ "$CPU_MODEL" = "unknown" ]; then
                    CPU_MODEL=$(lscpu 2>/dev/null | grep "Model name" | cut -d: -f2 | sed 's/^ *//' || echo "unknown")
                fi
            fi
            ;;

        macos)
            CPU_MODEL=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
            CPU_PHYSICAL_CORES=$(sysctl -n hw.physicalcpu 2>/dev/null || echo 1)
            CPU_LOGICAL_CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 1)
            CPU_FREQ_MHZ=$(sysctl -n hw.cpufrequency 2>/dev/null | awk '{print int($1/1000000)}' || echo 0)

            # CPU features on macOS
            FEATURES=$(sysctl -n machdep.cpu.features 2>/dev/null || echo "")
            LEAF7=$(sysctl -n machdep.cpu.leaf7_features 2>/dev/null || echo "")

            case "$FEATURES $LEAF7" in
                *AVX2*)    HAS_AVX2="true" ;;
            esac
            case "$FEATURES $LEAF7" in
                *AVX512F*) HAS_AVX512="true" ;;
            esac
            case "$FEATURES" in
                *SSE4.2*)  HAS_SSE42="true" ;;
            esac
            ;;

        freebsd)
            CPU_MODEL=$(sysctl -n hw.model 2>/dev/null || echo "unknown")
            CPU_LOGICAL_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 1)
            CPU_PHYSICAL_CORES=$CPU_LOGICAL_CORES
            ;;
    esac

    # Sanitize values
    : "${CPU_PHYSICAL_CORES:=1}"
    : "${CPU_LOGICAL_CORES:=1}"
    : "${CPU_FREQ_MHZ:=0}"

    echo "\"cpu\": {"
    echo "  \"model\": \"$(echo "$CPU_MODEL" | tr '"' "'" | tr '\n' ' ')\","
    echo "  \"physical_cores\": $CPU_PHYSICAL_CORES,"
    echo "  \"logical_cores\": $CPU_LOGICAL_CORES,"
    echo "  \"frequency_mhz\": $CPU_FREQ_MHZ,"
    echo "  \"capabilities\": {"
    echo "    \"avx2\": $HAS_AVX2,"
    echo "    \"avx512\": $HAS_AVX512,"
    echo "    \"sse42\": $HAS_SSE42"
    echo "  }"
    echo "}"
}

# ============================================================================
# Memory Detection
# ============================================================================

detect_memory() {
    TOTAL_RAM_MB=0
    AVAILABLE_RAM_MB=0

    case "$OS_TYPE" in
        linux)
            if [ -f /proc/meminfo ]; then
                TOTAL_RAM_KB=$(grep "MemTotal" /proc/meminfo | awk '{print $2}')
                AVAILABLE_RAM_KB=$(grep "MemAvailable" /proc/meminfo | awk '{print $2}')
                # Fallback if MemAvailable not present (older kernels)
                if [ -z "$AVAILABLE_RAM_KB" ]; then
                    FREE_KB=$(grep "MemFree" /proc/meminfo | awk '{print $2}')
                    BUFFERS_KB=$(grep "Buffers" /proc/meminfo | awk '{print $2}')
                    CACHED_KB=$(grep "^Cached" /proc/meminfo | awk '{print $2}')
                    AVAILABLE_RAM_KB=$((FREE_KB + BUFFERS_KB + CACHED_KB))
                fi
                TOTAL_RAM_MB=$((TOTAL_RAM_KB / 1024))
                AVAILABLE_RAM_MB=$((AVAILABLE_RAM_KB / 1024))
            fi
            ;;

        macos)
            # Total RAM in bytes
            TOTAL_BYTES=$(sysctl -n hw.memsize 2>/dev/null || echo 0)
            TOTAL_RAM_MB=$((TOTAL_BYTES / 1024 / 1024))

            # Available RAM (approximate via vm_stat)
            if cmd_exists vm_stat; then
                PAGE_SIZE=$(vm_stat | head -1 | grep -o '[0-9]*' || echo 4096)
                FREE_PAGES=$(vm_stat | grep "Pages free" | awk '{print $3}' | tr -d '.')
                INACTIVE_PAGES=$(vm_stat | grep "Pages inactive" | awk '{print $3}' | tr -d '.')
                : "${FREE_PAGES:=0}"
                : "${INACTIVE_PAGES:=0}"
                AVAILABLE_RAM_MB=$(( (FREE_PAGES + INACTIVE_PAGES) * PAGE_SIZE / 1024 / 1024 ))
            else
                AVAILABLE_RAM_MB=$((TOTAL_RAM_MB / 2))
            fi
            ;;

        freebsd)
            TOTAL_RAM_MB=$(sysctl -n hw.physmem 2>/dev/null | awk '{print int($1/1024/1024)}' || echo 0)
            AVAILABLE_RAM_MB=$((TOTAL_RAM_MB / 2))
            ;;
    esac

    TOTAL_RAM_GB=$((TOTAL_RAM_MB / 1024))
    AVAILABLE_RAM_GB=$((AVAILABLE_RAM_MB / 1024))

    echo "\"memory\": {"
    echo "  \"total_mb\": $TOTAL_RAM_MB,"
    echo "  \"total_gb\": $TOTAL_RAM_GB,"
    echo "  \"available_mb\": $AVAILABLE_RAM_MB,"
    echo "  \"available_gb\": $AVAILABLE_RAM_GB"
    echo "}"
}

# ============================================================================
# Disk Detection
# ============================================================================

detect_disk() {
    DISK_TYPE="unknown"
    DISK_SIZE_GB=0
    ESTIMATED_IOPS=100
    DISK_DEVICE=""

    case "$OS_TYPE" in
        linux)
            # Find the root filesystem device
            ROOT_DEV=$(df / 2>/dev/null | tail -1 | awk '{print $1}')
            DISK_DEVICE=$(echo "$ROOT_DEV" | sed 's/[0-9]*$//' | sed 's/p[0-9]*$//')
            BASE_DEV=$(basename "$DISK_DEVICE" 2>/dev/null || echo "sda")

            # Remove partition numbers for nvme devices
            BASE_DEV=$(echo "$BASE_DEV" | sed 's/p[0-9]*$//')

            # Detect disk type
            if [ -f "/sys/block/${BASE_DEV}/queue/rotational" ]; then
                ROTATIONAL=$(cat "/sys/block/${BASE_DEV}/queue/rotational" 2>/dev/null || echo 1)
                if [ "$ROTATIONAL" = "0" ]; then
                    # Non-rotational - check if NVMe
                    case "$BASE_DEV" in
                        nvme*)
                            DISK_TYPE="nvme"
                            ESTIMATED_IOPS=500000
                            ;;
                        *)
                            DISK_TYPE="ssd"
                            ESTIMATED_IOPS=50000
                            ;;
                    esac
                else
                    DISK_TYPE="hdd"
                    ESTIMATED_IOPS=200
                fi
            else
                # Fallback detection based on device name
                case "$BASE_DEV" in
                    nvme*) DISK_TYPE="nvme"; ESTIMATED_IOPS=500000 ;;
                    sd*)   DISK_TYPE="unknown_sata"; ESTIMATED_IOPS=5000 ;;
                    vd*)   DISK_TYPE="virtio"; ESTIMATED_IOPS=10000 ;;
                    xvd*)  DISK_TYPE="xen"; ESTIMATED_IOPS=3000 ;;
                esac
            fi

            # Get disk size
            if [ -f "/sys/block/${BASE_DEV}/size" ]; then
                SECTORS=$(cat "/sys/block/${BASE_DEV}/size" 2>/dev/null || echo 0)
                DISK_SIZE_GB=$((SECTORS * 512 / 1024 / 1024 / 1024))
            fi

            # Fallback to df
            if [ "$DISK_SIZE_GB" = "0" ]; then
                DISK_SIZE_GB=$(df -BG / 2>/dev/null | tail -1 | awk '{print $2}' | tr -d 'G' || echo 0)
            fi
            ;;

        macos)
            # Get boot disk info
            DISK_INFO=$(diskutil info / 2>/dev/null || echo "")

            if echo "$DISK_INFO" | grep -qi "solid state"; then
                DISK_TYPE="ssd"
                ESTIMATED_IOPS=50000
            elif echo "$DISK_INFO" | grep -qi "nvme"; then
                DISK_TYPE="nvme"
                ESTIMATED_IOPS=500000
            else
                # Modern Macs typically have SSDs
                DISK_TYPE="ssd"
                ESTIMATED_IOPS=50000
            fi

            # Get disk size using df (more reliable on macOS)
            # df -g shows size in GB directly
            DISK_SIZE_GB=$(df -g / 2>/dev/null | tail -1 | awk '{print $2}' || echo 0)

            # Remove any non-numeric characters
            DISK_SIZE_GB=$(echo "$DISK_SIZE_GB" | sed 's/[^0-9]//g')

            # Fallback to reasonable default
            if [ -z "$DISK_SIZE_GB" ] || [ "$DISK_SIZE_GB" = "0" ]; then
                DISK_SIZE_GB=256
            fi
            ;;

        freebsd)
            DISK_TYPE="unknown"
            DISK_SIZE_GB=$(df -g / 2>/dev/null | tail -1 | awk '{print $2}' || echo 100)
            ESTIMATED_IOPS=5000
            ;;
    esac

    echo "\"disk\": {"
    echo "  \"type\": \"$DISK_TYPE\","
    echo "  \"device\": \"$DISK_DEVICE\","
    echo "  \"size_gb\": $DISK_SIZE_GB,"
    echo "  \"estimated_iops\": $ESTIMATED_IOPS"
    echo "}"
}

# ============================================================================
# Network Detection
# ============================================================================

detect_network() {
    NETWORK_SPEED_MBPS=1000
    PRIMARY_INTERFACE=""

    case "$OS_TYPE" in
        linux)
            # Find primary interface
            PRIMARY_INTERFACE=$(ip route 2>/dev/null | grep default | awk '{print $5}' | head -1)

            if [ -n "$PRIMARY_INTERFACE" ] && [ -f "/sys/class/net/${PRIMARY_INTERFACE}/speed" ]; then
                NETWORK_SPEED_MBPS=$(cat "/sys/class/net/${PRIMARY_INTERFACE}/speed" 2>/dev/null || echo 1000)
                # Handle -1 (unknown speed)
                if [ "$NETWORK_SPEED_MBPS" -lt 0 ]; then
                    NETWORK_SPEED_MBPS=1000
                fi
            fi
            ;;

        macos)
            PRIMARY_INTERFACE=$(route -n get default 2>/dev/null | grep interface | awk '{print $2}')
            # macOS doesn't easily expose link speed, assume gigabit
            NETWORK_SPEED_MBPS=1000
            ;;

        freebsd)
            NETWORK_SPEED_MBPS=1000
            ;;
    esac

    echo "\"network\": {"
    echo "  \"interface\": \"$PRIMARY_INTERFACE\","
    echo "  \"speed_mbps\": $NETWORK_SPEED_MBPS"
    echo "}"
}

# ============================================================================
# Cloud Provider Detection (calls cloud_detect.sh if available)
# ============================================================================

detect_cloud() {
    CLOUD_PROVIDER="local"
    INSTANCE_TYPE=""
    REGION=""

    SCRIPT_DIR=$(dirname "$0")

    # Use cloud_detect.sh if available
    if [ -x "${SCRIPT_DIR}/cloud_detect.sh" ]; then
        CLOUD_INFO=$("${SCRIPT_DIR}/cloud_detect.sh" --json 2>/dev/null || echo "{}")
        CLOUD_PROVIDER=$(echo "$CLOUD_INFO" | grep '"provider"' | cut -d'"' -f4 || echo "local")
        INSTANCE_TYPE=$(echo "$CLOUD_INFO" | grep '"instance_type"' | cut -d'"' -f4 || echo "")
        REGION=$(echo "$CLOUD_INFO" | grep '"region"' | cut -d'"' -f4 || echo "")
    else
        # Basic cloud detection

        # AWS
        if curl -s -m 1 http://169.254.169.254/latest/meta-data/ >/dev/null 2>&1; then
            # Could be AWS or another cloud using this endpoint
            if curl -s -m 1 http://169.254.169.254/latest/meta-data/ami-id >/dev/null 2>&1; then
                CLOUD_PROVIDER="aws"
                INSTANCE_TYPE=$(curl -s -m 2 http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || echo "")
                REGION=$(curl -s -m 2 http://169.254.169.254/latest/meta-data/placement/region 2>/dev/null || echo "")
            fi
        fi

        # GCP
        if [ "$CLOUD_PROVIDER" = "local" ]; then
            if curl -s -m 1 -H "Metadata-Flavor: Google" http://metadata.google.internal/computeMetadata/v1/ >/dev/null 2>&1; then
                CLOUD_PROVIDER="gcp"
                MACHINE_TYPE=$(curl -s -m 2 -H "Metadata-Flavor: Google" http://metadata.google.internal/computeMetadata/v1/instance/machine-type 2>/dev/null || echo "")
                INSTANCE_TYPE=$(basename "$MACHINE_TYPE")
                ZONE=$(curl -s -m 2 -H "Metadata-Flavor: Google" http://metadata.google.internal/computeMetadata/v1/instance/zone 2>/dev/null || echo "")
                REGION=$(echo "$ZONE" | rev | cut -d'-' -f2- | rev)
            fi
        fi

        # Azure
        if [ "$CLOUD_PROVIDER" = "local" ]; then
            if curl -s -m 1 -H "Metadata: true" "http://169.254.169.254/metadata/instance?api-version=2021-02-01" >/dev/null 2>&1; then
                CLOUD_PROVIDER="azure"
                AZURE_META=$(curl -s -m 2 -H "Metadata: true" "http://169.254.169.254/metadata/instance?api-version=2021-02-01" 2>/dev/null || echo "{}")
                INSTANCE_TYPE=$(echo "$AZURE_META" | grep -o '"vmSize":"[^"]*"' | cut -d'"' -f4 || echo "")
                REGION=$(echo "$AZURE_META" | grep -o '"location":"[^"]*"' | cut -d'"' -f4 || echo "")
            fi
        fi

        # DigitalOcean
        if [ "$CLOUD_PROVIDER" = "local" ]; then
            if curl -s -m 1 http://169.254.169.254/metadata/v1/ >/dev/null 2>&1; then
                if curl -s -m 1 http://169.254.169.254/metadata/v1/id >/dev/null 2>&1; then
                    CLOUD_PROVIDER="digitalocean"
                    REGION=$(curl -s -m 2 http://169.254.169.254/metadata/v1/region 2>/dev/null || echo "")
                    # DigitalOcean uses droplet size
                    INSTANCE_TYPE=$(curl -s -m 2 http://169.254.169.254/metadata/v1/droplet-size 2>/dev/null || echo "")
                fi
            fi
        fi

        # Check for container environments
        if [ "$CLOUD_PROVIDER" = "local" ]; then
            if [ -f /.dockerenv ]; then
                CLOUD_PROVIDER="docker"
            elif [ -f /run/.containerenv ]; then
                CLOUD_PROVIDER="podman"
            elif grep -q "kubepods" /proc/1/cgroup 2>/dev/null; then
                CLOUD_PROVIDER="kubernetes"
            fi
        fi
    fi

    echo "\"cloud\": {"
    echo "  \"provider\": \"$CLOUD_PROVIDER\","
    echo "  \"instance_type\": \"$INSTANCE_TYPE\","
    echo "  \"region\": \"$REGION\""
    echo "}"
}

# ============================================================================
# Main Output
# ============================================================================

generate_json() {
    echo "{"
    echo "  \"timestamp\": \"$(date -Iseconds 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S')\","
    echo "  \"hostname\": \"$(hostname)\","
    echo "  \"os\": {"
    echo "    \"type\": \"$OS_TYPE\","
    echo "    \"kernel\": \"$(uname -r)\","
    echo "    \"arch\": \"$(uname -m)\""
    echo "  },"
    echo "  $(detect_cpu),"
    echo "  $(detect_memory),"
    echo "  $(detect_disk),"
    echo "  $(detect_network),"
    echo "  $(detect_cloud)"
    echo "}"
}

# Generate output
JSON_OUTPUT=$(generate_json)

# Write to file
echo "$JSON_OUTPUT" > "$OUTPUT_FILE"

# Also output to stdout for piping
echo "$JSON_OUTPUT"

exit 0
