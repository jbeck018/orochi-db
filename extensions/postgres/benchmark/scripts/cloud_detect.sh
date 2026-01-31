#!/bin/sh
#
# cloud_detect.sh - Cloud provider and instance type detection
# Detects AWS, GCP, Azure, DigitalOcean and maps to performance expectations
#
# Usage: ./cloud_detect.sh [--json]
#
# POSIX-compatible for portability

set -e

OUTPUT_JSON=0

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --json)
            OUTPUT_JSON=1
            ;;
        --help)
            echo "Usage: $0 [--json]"
            echo "Detects cloud provider and instance type"
            exit 0
            ;;
    esac
done

# Initialize variables
PROVIDER="local"
INSTANCE_TYPE=""
REGION=""
ZONE=""
VCPUS=""
MEMORY_GB=""
PERFORMANCE_TIER="unknown"
EXPECTED_IOPS=0
EXPECTED_NETWORK_GBPS=0

# Timeout for metadata requests (seconds)
TIMEOUT=2

# Helper: HTTP GET with timeout
http_get() {
    URL="$1"
    HEADERS="${2:-}"

    if command -v curl >/dev/null 2>&1; then
        if [ -n "$HEADERS" ]; then
            curl -s -m "$TIMEOUT" -H "$HEADERS" "$URL" 2>/dev/null
        else
            curl -s -m "$TIMEOUT" "$URL" 2>/dev/null
        fi
    elif command -v wget >/dev/null 2>&1; then
        if [ -n "$HEADERS" ]; then
            wget -q -O - --timeout="$TIMEOUT" --header="$HEADERS" "$URL" 2>/dev/null
        else
            wget -q -O - --timeout="$TIMEOUT" "$URL" 2>/dev/null
        fi
    else
        return 1
    fi
}

# ============================================================================
# AWS Detection
# ============================================================================

detect_aws() {
    # Check AWS metadata service
    TOKEN=$(http_get "http://169.254.169.254/latest/api/token" "X-aws-ec2-metadata-token-ttl-seconds: 60" || echo "")

    if [ -n "$TOKEN" ]; then
        # IMDSv2
        HEADER="X-aws-ec2-metadata-token: $TOKEN"
    else
        # IMDSv1 fallback
        HEADER=""
    fi

    # Check if this is AWS
    AMI_ID=$(http_get "http://169.254.169.254/latest/meta-data/ami-id" "$HEADER")
    if [ -z "$AMI_ID" ]; then
        return 1
    fi

    PROVIDER="aws"
    INSTANCE_TYPE=$(http_get "http://169.254.169.254/latest/meta-data/instance-type" "$HEADER")
    REGION=$(http_get "http://169.254.169.254/latest/meta-data/placement/region" "$HEADER")
    ZONE=$(http_get "http://169.254.169.254/latest/meta-data/placement/availability-zone" "$HEADER")

    # Map AWS instance types to performance expectations
    case "$INSTANCE_TYPE" in
        # General Purpose
        t3.micro|t3a.micro)
            VCPUS=2; MEMORY_GB=1; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=0.5
            ;;
        t3.small|t3a.small)
            VCPUS=2; MEMORY_GB=2; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=1
            ;;
        t3.medium|t3a.medium)
            VCPUS=2; MEMORY_GB=4; PERFORMANCE_TIER="low"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=2
            ;;
        t3.large|t3a.large)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="low"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=2
            ;;
        t3.xlarge|t3a.xlarge)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=5
            ;;
        t3.2xlarge|t3a.2xlarge)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=5
            ;;
        m5.large|m5a.large|m6i.large|m6a.large)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        m5.xlarge|m5a.xlarge|m6i.xlarge|m6a.xlarge)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        m5.2xlarge|m5a.2xlarge|m6i.2xlarge|m6a.2xlarge)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        m5.4xlarge|m5a.4xlarge|m6i.4xlarge|m6a.4xlarge)
            VCPUS=16; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=20000; EXPECTED_NETWORK_GBPS=10
            ;;
        m5.8xlarge|m5a.8xlarge|m6i.8xlarge|m6a.8xlarge)
            VCPUS=32; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=40000; EXPECTED_NETWORK_GBPS=12
            ;;
        m5.12xlarge|m6i.12xlarge)
            VCPUS=48; MEMORY_GB=192; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=12
            ;;
        m5.16xlarge|m6i.16xlarge)
            VCPUS=64; MEMORY_GB=256; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=80000; EXPECTED_NETWORK_GBPS=25
            ;;
        # Memory Optimized
        r5.large|r5a.large|r6i.large)
            VCPUS=2; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        r5.xlarge|r5a.xlarge|r6i.xlarge)
            VCPUS=4; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        r5.2xlarge|r5a.2xlarge|r6i.2xlarge)
            VCPUS=8; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=20000; EXPECTED_NETWORK_GBPS=10
            ;;
        r5.4xlarge|r5a.4xlarge|r6i.4xlarge)
            VCPUS=16; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=40000; EXPECTED_NETWORK_GBPS=10
            ;;
        r5.8xlarge|r6i.8xlarge)
            VCPUS=32; MEMORY_GB=256; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=12
            ;;
        # Compute Optimized
        c5.large|c5a.large|c6i.large)
            VCPUS=2; MEMORY_GB=4; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        c5.xlarge|c5a.xlarge|c6i.xlarge)
            VCPUS=4; MEMORY_GB=8; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=10
            ;;
        c5.2xlarge|c5a.2xlarge|c6i.2xlarge)
            VCPUS=8; MEMORY_GB=16; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=20000; EXPECTED_NETWORK_GBPS=10
            ;;
        c5.4xlarge|c5a.4xlarge|c6i.4xlarge)
            VCPUS=16; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=40000; EXPECTED_NETWORK_GBPS=10
            ;;
        # Storage Optimized
        i3.large)
            VCPUS=2; MEMORY_GB=16; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=100000; EXPECTED_NETWORK_GBPS=10
            ;;
        i3.xlarge)
            VCPUS=4; MEMORY_GB=32; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=200000; EXPECTED_NETWORK_GBPS=10
            ;;
        i3.2xlarge)
            VCPUS=8; MEMORY_GB=64; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=400000; EXPECTED_NETWORK_GBPS=10
            ;;
        i3.4xlarge)
            VCPUS=16; MEMORY_GB=128; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=800000; EXPECTED_NETWORK_GBPS=10
            ;;
        *)
            # Generic fallback based on instance size pattern
            case "$INSTANCE_TYPE" in
                *.micro)   VCPUS=2;  MEMORY_GB=1;   PERFORMANCE_TIER="minimal" ;;
                *.small)   VCPUS=2;  MEMORY_GB=2;   PERFORMANCE_TIER="minimal" ;;
                *.medium)  VCPUS=2;  MEMORY_GB=4;   PERFORMANCE_TIER="low" ;;
                *.large)   VCPUS=2;  MEMORY_GB=8;   PERFORMANCE_TIER="medium" ;;
                *.xlarge)  VCPUS=4;  MEMORY_GB=16;  PERFORMANCE_TIER="medium" ;;
                *.2xlarge) VCPUS=8;  MEMORY_GB=32;  PERFORMANCE_TIER="high" ;;
                *.4xlarge) VCPUS=16; MEMORY_GB=64;  PERFORMANCE_TIER="high" ;;
                *.8xlarge) VCPUS=32; MEMORY_GB=128; PERFORMANCE_TIER="very_high" ;;
                *)         VCPUS=4;  MEMORY_GB=16;  PERFORMANCE_TIER="medium" ;;
            esac
            EXPECTED_IOPS=10000
            EXPECTED_NETWORK_GBPS=10
            ;;
    esac

    return 0
}

# ============================================================================
# GCP Detection
# ============================================================================

detect_gcp() {
    # Check GCP metadata service
    MACHINE_TYPE=$(http_get "http://metadata.google.internal/computeMetadata/v1/instance/machine-type" "Metadata-Flavor: Google")

    if [ -z "$MACHINE_TYPE" ]; then
        return 1
    fi

    PROVIDER="gcp"
    INSTANCE_TYPE=$(basename "$MACHINE_TYPE")
    ZONE=$(http_get "http://metadata.google.internal/computeMetadata/v1/instance/zone" "Metadata-Flavor: Google")
    ZONE=$(basename "$ZONE")
    REGION=$(echo "$ZONE" | rev | cut -d'-' -f2- | rev)

    # Map GCP instance types to performance expectations
    case "$INSTANCE_TYPE" in
        # E2 (General Purpose - Cost optimized)
        e2-micro)
            VCPUS=2; MEMORY_GB=1; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=1
            ;;
        e2-small)
            VCPUS=2; MEMORY_GB=2; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=2
            ;;
        e2-medium)
            VCPUS=2; MEMORY_GB=4; PERFORMANCE_TIER="low"
            EXPECTED_IOPS=3000; EXPECTED_NETWORK_GBPS=2
            ;;
        e2-standard-2)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=4
            ;;
        e2-standard-4)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=8
            ;;
        e2-standard-8)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=16
            ;;
        e2-standard-16)
            VCPUS=16; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=16
            ;;
        # N2 (General Purpose - Balanced)
        n2-standard-2)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=10
            ;;
        n2-standard-4)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=10
            ;;
        n2-standard-8)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=16
            ;;
        n2-standard-16)
            VCPUS=16; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=32
            ;;
        n2-standard-32)
            VCPUS=32; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=32
            ;;
        n2-standard-48)
            VCPUS=48; MEMORY_GB=192; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=32
            ;;
        n2-standard-64)
            VCPUS=64; MEMORY_GB=256; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=80000; EXPECTED_NETWORK_GBPS=32
            ;;
        # N2 High-Memory
        n2-highmem-2)
            VCPUS=2; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=10
            ;;
        n2-highmem-4)
            VCPUS=4; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=15000; EXPECTED_NETWORK_GBPS=10
            ;;
        n2-highmem-8)
            VCPUS=8; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=16
            ;;
        n2-highmem-16)
            VCPUS=16; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=32
            ;;
        # C2 (Compute Optimized)
        c2-standard-4)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=10
            ;;
        c2-standard-8)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=30000; EXPECTED_NETWORK_GBPS=16
            ;;
        c2-standard-16)
            VCPUS=16; MEMORY_GB=64; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=32
            ;;
        c2-standard-30)
            VCPUS=30; MEMORY_GB=120; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=60000; EXPECTED_NETWORK_GBPS=32
            ;;
        c2-standard-60)
            VCPUS=60; MEMORY_GB=240; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=100000; EXPECTED_NETWORK_GBPS=32
            ;;
        *)
            # Generic fallback based on vCPU count in name
            VCPU_COUNT=$(echo "$INSTANCE_TYPE" | grep -o '[0-9]*$' || echo 4)
            VCPUS=$VCPU_COUNT
            MEMORY_GB=$((VCPU_COUNT * 4))
            if [ "$VCPU_COUNT" -le 2 ]; then
                PERFORMANCE_TIER="low"
            elif [ "$VCPU_COUNT" -le 8 ]; then
                PERFORMANCE_TIER="medium"
            elif [ "$VCPU_COUNT" -le 32 ]; then
                PERFORMANCE_TIER="high"
            else
                PERFORMANCE_TIER="very_high"
            fi
            EXPECTED_IOPS=15000
            EXPECTED_NETWORK_GBPS=10
            ;;
    esac

    return 0
}

# ============================================================================
# Azure Detection
# ============================================================================

detect_azure() {
    # Check Azure metadata service
    AZURE_META=$(http_get "http://169.254.169.254/metadata/instance?api-version=2021-02-01" "Metadata: true")

    if [ -z "$AZURE_META" ]; then
        return 1
    fi

    PROVIDER="azure"

    # Parse Azure metadata (simplified parsing)
    INSTANCE_TYPE=$(echo "$AZURE_META" | grep -o '"vmSize":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")
    REGION=$(echo "$AZURE_META" | grep -o '"location":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")
    ZONE=$(echo "$AZURE_META" | grep -o '"zone":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")

    # Map Azure VM sizes to performance expectations
    case "$INSTANCE_TYPE" in
        # B-series (Burstable)
        Standard_B1s)
            VCPUS=1; MEMORY_GB=1; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=640; EXPECTED_NETWORK_GBPS=0.5
            ;;
        Standard_B1ms)
            VCPUS=1; MEMORY_GB=2; PERFORMANCE_TIER="minimal"
            EXPECTED_IOPS=640; EXPECTED_NETWORK_GBPS=0.5
            ;;
        Standard_B2s)
            VCPUS=2; MEMORY_GB=4; PERFORMANCE_TIER="low"
            EXPECTED_IOPS=1280; EXPECTED_NETWORK_GBPS=1
            ;;
        Standard_B2ms)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="low"
            EXPECTED_IOPS=1920; EXPECTED_NETWORK_GBPS=1
            ;;
        Standard_B4ms)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=2880; EXPECTED_NETWORK_GBPS=2
            ;;
        Standard_B8ms)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=4320; EXPECTED_NETWORK_GBPS=4
            ;;
        # D-series (General Purpose)
        Standard_D2s_v3|Standard_D2s_v4|Standard_D2s_v5)
            VCPUS=2; MEMORY_GB=8; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=3200; EXPECTED_NETWORK_GBPS=2
            ;;
        Standard_D4s_v3|Standard_D4s_v4|Standard_D4s_v5)
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=6400; EXPECTED_NETWORK_GBPS=4
            ;;
        Standard_D8s_v3|Standard_D8s_v4|Standard_D8s_v5)
            VCPUS=8; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=12800; EXPECTED_NETWORK_GBPS=8
            ;;
        Standard_D16s_v3|Standard_D16s_v4|Standard_D16s_v5)
            VCPUS=16; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=25600; EXPECTED_NETWORK_GBPS=8
            ;;
        Standard_D32s_v3|Standard_D32s_v4|Standard_D32s_v5)
            VCPUS=32; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=51200; EXPECTED_NETWORK_GBPS=16
            ;;
        Standard_D48s_v3|Standard_D48s_v4|Standard_D48s_v5)
            VCPUS=48; MEMORY_GB=192; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=76800; EXPECTED_NETWORK_GBPS=24
            ;;
        Standard_D64s_v3|Standard_D64s_v4|Standard_D64s_v5)
            VCPUS=64; MEMORY_GB=256; PERFORMANCE_TIER="extreme"
            EXPECTED_IOPS=80000; EXPECTED_NETWORK_GBPS=30
            ;;
        # E-series (Memory Optimized)
        Standard_E2s_v3|Standard_E2s_v4|Standard_E2s_v5)
            VCPUS=2; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=3200; EXPECTED_NETWORK_GBPS=2
            ;;
        Standard_E4s_v3|Standard_E4s_v4|Standard_E4s_v5)
            VCPUS=4; MEMORY_GB=32; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=6400; EXPECTED_NETWORK_GBPS=4
            ;;
        Standard_E8s_v3|Standard_E8s_v4|Standard_E8s_v5)
            VCPUS=8; MEMORY_GB=64; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=12800; EXPECTED_NETWORK_GBPS=8
            ;;
        Standard_E16s_v3|Standard_E16s_v4|Standard_E16s_v5)
            VCPUS=16; MEMORY_GB=128; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=25600; EXPECTED_NETWORK_GBPS=8
            ;;
        Standard_E32s_v3|Standard_E32s_v4|Standard_E32s_v5)
            VCPUS=32; MEMORY_GB=256; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=51200; EXPECTED_NETWORK_GBPS=16
            ;;
        # F-series (Compute Optimized)
        Standard_F2s_v2)
            VCPUS=2; MEMORY_GB=4; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=3200; EXPECTED_NETWORK_GBPS=2
            ;;
        Standard_F4s_v2)
            VCPUS=4; MEMORY_GB=8; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=6400; EXPECTED_NETWORK_GBPS=4
            ;;
        Standard_F8s_v2)
            VCPUS=8; MEMORY_GB=16; PERFORMANCE_TIER="high"
            EXPECTED_IOPS=12800; EXPECTED_NETWORK_GBPS=8
            ;;
        Standard_F16s_v2)
            VCPUS=16; MEMORY_GB=32; PERFORMANCE_TIER="very_high"
            EXPECTED_IOPS=25600; EXPECTED_NETWORK_GBPS=8
            ;;
        *)
            # Generic fallback
            VCPUS=4; MEMORY_GB=16; PERFORMANCE_TIER="medium"
            EXPECTED_IOPS=10000; EXPECTED_NETWORK_GBPS=4
            ;;
    esac

    return 0
}

# ============================================================================
# DigitalOcean Detection
# ============================================================================

detect_digitalocean() {
    # Check DigitalOcean metadata service
    DROPLET_ID=$(http_get "http://169.254.169.254/metadata/v1/id")

    if [ -z "$DROPLET_ID" ]; then
        return 1
    fi

    PROVIDER="digitalocean"
    REGION=$(http_get "http://169.254.169.254/metadata/v1/region")
    # DigitalOcean doesn't expose size via metadata directly
    # But we can get memory and vcpus
    DO_MEMORY=$(http_get "http://169.254.169.254/metadata/v1/memory")
    DO_VCPUS=$(http_get "http://169.254.169.254/metadata/v1/vcpus")

    VCPUS="${DO_VCPUS:-1}"
    MEMORY_GB=$((${DO_MEMORY:-512} / 1024))

    # Map based on memory/vcpu combination
    if [ "$MEMORY_GB" -le 1 ]; then
        INSTANCE_TYPE="s-1vcpu-1gb"
        PERFORMANCE_TIER="minimal"
        EXPECTED_IOPS=1500
        EXPECTED_NETWORK_GBPS=1
    elif [ "$MEMORY_GB" -le 2 ]; then
        INSTANCE_TYPE="s-1vcpu-2gb"
        PERFORMANCE_TIER="minimal"
        EXPECTED_IOPS=1500
        EXPECTED_NETWORK_GBPS=1
    elif [ "$MEMORY_GB" -le 4 ]; then
        INSTANCE_TYPE="s-2vcpu-4gb"
        PERFORMANCE_TIER="low"
        EXPECTED_IOPS=3000
        EXPECTED_NETWORK_GBPS=2
    elif [ "$MEMORY_GB" -le 8 ]; then
        if [ "$VCPUS" -le 2 ]; then
            INSTANCE_TYPE="s-2vcpu-8gb"
        else
            INSTANCE_TYPE="s-4vcpu-8gb"
        fi
        PERFORMANCE_TIER="medium"
        EXPECTED_IOPS=6000
        EXPECTED_NETWORK_GBPS=4
    elif [ "$MEMORY_GB" -le 16 ]; then
        INSTANCE_TYPE="s-4vcpu-16gb"
        PERFORMANCE_TIER="medium"
        EXPECTED_IOPS=6000
        EXPECTED_NETWORK_GBPS=5
    elif [ "$MEMORY_GB" -le 32 ]; then
        INSTANCE_TYPE="s-8vcpu-32gb"
        PERFORMANCE_TIER="high"
        EXPECTED_IOPS=7500
        EXPECTED_NETWORK_GBPS=5
    elif [ "$MEMORY_GB" -le 64 ]; then
        INSTANCE_TYPE="s-16vcpu-64gb"
        PERFORMANCE_TIER="high"
        EXPECTED_IOPS=7500
        EXPECTED_NETWORK_GBPS=6
    elif [ "$MEMORY_GB" -le 128 ]; then
        INSTANCE_TYPE="s-24vcpu-128gb"
        PERFORMANCE_TIER="very_high"
        EXPECTED_IOPS=7500
        EXPECTED_NETWORK_GBPS=9
    elif [ "$MEMORY_GB" -le 192 ]; then
        INSTANCE_TYPE="s-32vcpu-192gb"
        PERFORMANCE_TIER="very_high"
        EXPECTED_IOPS=7500
        EXPECTED_NETWORK_GBPS=9
    else
        INSTANCE_TYPE="so-48vcpu-384gb"
        PERFORMANCE_TIER="extreme"
        EXPECTED_IOPS=10000
        EXPECTED_NETWORK_GBPS=9
    fi

    return 0
}

# ============================================================================
# Main Detection Logic
# ============================================================================

# Try each cloud provider in order
if detect_aws 2>/dev/null; then
    : # AWS detected
elif detect_gcp 2>/dev/null; then
    : # GCP detected
elif detect_azure 2>/dev/null; then
    : # Azure detected
elif detect_digitalocean 2>/dev/null; then
    : # DigitalOcean detected
else
    # Check for container environments
    if [ -f /.dockerenv ]; then
        PROVIDER="docker"
        PERFORMANCE_TIER="container"
    elif [ -f /run/.containerenv ]; then
        PROVIDER="podman"
        PERFORMANCE_TIER="container"
    elif grep -q "kubepods" /proc/1/cgroup 2>/dev/null; then
        PROVIDER="kubernetes"
        PERFORMANCE_TIER="container"
    else
        PROVIDER="local"
        PERFORMANCE_TIER="local"
    fi
fi

# ============================================================================
# Output
# ============================================================================

if [ "$OUTPUT_JSON" -eq 1 ]; then
    cat << EOF
{
  "provider": "$PROVIDER",
  "instance_type": "$INSTANCE_TYPE",
  "region": "$REGION",
  "zone": "$ZONE",
  "vcpus": ${VCPUS:-0},
  "memory_gb": ${MEMORY_GB:-0},
  "performance_tier": "$PERFORMANCE_TIER",
  "expected_iops": ${EXPECTED_IOPS:-0},
  "expected_network_gbps": ${EXPECTED_NETWORK_GBPS:-0}
}
EOF
else
    echo "Cloud Provider:    $PROVIDER"
    echo "Instance Type:     ${INSTANCE_TYPE:-N/A}"
    echo "Region:            ${REGION:-N/A}"
    echo "Zone:              ${ZONE:-N/A}"
    echo "vCPUs:             ${VCPUS:-N/A}"
    echo "Memory (GB):       ${MEMORY_GB:-N/A}"
    echo "Performance Tier:  $PERFORMANCE_TIER"
    echo "Expected IOPS:     ${EXPECTED_IOPS:-N/A}"
    echo "Expected Network:  ${EXPECTED_NETWORK_GBPS:-N/A} Gbps"
fi

exit 0
