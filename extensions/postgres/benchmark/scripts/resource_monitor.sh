#!/bin/sh
#
# resource_monitor.sh - Background resource monitoring for Orochi DB benchmarks
# Monitors CPU, memory, disk I/O during benchmark execution
#
# Usage: ./resource_monitor.sh [OPTIONS]
#
# Options:
#   --output=FILE     Output file for metrics [default: resource_metrics.csv]
#   --interval=N      Sampling interval in seconds [default: 1]
#   --pid=PID         Monitor specific process (e.g., postgres backend)
#   --duration=N      Maximum monitoring duration [default: unlimited]
#   --stop            Stop any running monitor
#   --status          Check if monitor is running
#
# Output: CSV file with time-series resource data
#
# POSIX-compatible for portability

set -e

# Default configuration
OUTPUT_FILE="${OROCHI_METRICS_FILE:-resource_metrics.csv}"
INTERVAL=1
MONITOR_PID=""
MAX_DURATION=0
PID_FILE="/tmp/orochi_resource_monitor.pid"
ACTION="start"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --output=*)
            OUTPUT_FILE="${arg#*=}"
            ;;
        --interval=*)
            INTERVAL="${arg#*=}"
            ;;
        --pid=*)
            MONITOR_PID="${arg#*=}"
            ;;
        --duration=*)
            MAX_DURATION="${arg#*=}"
            ;;
        --stop)
            ACTION="stop"
            ;;
        --status)
            ACTION="status"
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --output=FILE     Output CSV file [default: resource_metrics.csv]"
            echo "  --interval=N      Sampling interval in seconds [default: 1]"
            echo "  --pid=PID         Monitor specific process"
            echo "  --duration=N      Maximum duration in seconds [default: unlimited]"
            echo "  --stop            Stop running monitor"
            echo "  --status          Check monitor status"
            echo ""
            echo "Output format: CSV with columns:"
            echo "  timestamp, cpu_user, cpu_system, cpu_idle, cpu_iowait,"
            echo "  mem_total_mb, mem_used_mb, mem_available_mb, mem_percent,"
            echo "  disk_read_kb, disk_write_kb, disk_iops_read, disk_iops_write,"
            echo "  net_rx_kb, net_tx_kb, pg_connections, pg_cpu, pg_mem_mb"
            exit 0
            ;;
    esac
done

# ============================================================================
# Helper Functions
# ============================================================================

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

# Get timestamp in ISO format
get_timestamp() {
    date -Iseconds 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S'
}

# Get Unix epoch time
get_epoch() {
    date '+%s'
}

# ============================================================================
# Stop/Status Actions
# ============================================================================

if [ "$ACTION" = "stop" ]; then
    if [ -f "$PID_FILE" ]; then
        RUNNING_PID=$(cat "$PID_FILE")
        if kill -0 "$RUNNING_PID" 2>/dev/null; then
            echo "Stopping resource monitor (PID: $RUNNING_PID)..."
            kill "$RUNNING_PID" 2>/dev/null || true
            rm -f "$PID_FILE"
            echo "Monitor stopped."
        else
            echo "Monitor process not running, cleaning up PID file."
            rm -f "$PID_FILE"
        fi
    else
        echo "No monitor running (no PID file found)."
    fi
    exit 0
fi

if [ "$ACTION" = "status" ]; then
    if [ -f "$PID_FILE" ]; then
        RUNNING_PID=$(cat "$PID_FILE")
        if kill -0 "$RUNNING_PID" 2>/dev/null; then
            echo "Monitor is running (PID: $RUNNING_PID)"
            echo "Output file: $OUTPUT_FILE"
            if [ -f "$OUTPUT_FILE" ]; then
                LINES=$(wc -l < "$OUTPUT_FILE")
                echo "Samples collected: $((LINES - 1))"
            fi
            exit 0
        else
            echo "Monitor not running (stale PID file)"
            rm -f "$PID_FILE"
            exit 1
        fi
    else
        echo "Monitor is not running"
        exit 1
    fi
fi

# ============================================================================
# CPU Metrics
# ============================================================================

get_cpu_metrics_linux() {
    # Read /proc/stat for CPU metrics
    if [ -f /proc/stat ]; then
        # Get first line (aggregate CPU)
        CPU_LINE=$(head -1 /proc/stat)
        USER=$(echo "$CPU_LINE" | awk '{print $2}')
        NICE=$(echo "$CPU_LINE" | awk '{print $3}')
        SYSTEM=$(echo "$CPU_LINE" | awk '{print $4}')
        IDLE=$(echo "$CPU_LINE" | awk '{print $5}')
        IOWAIT=$(echo "$CPU_LINE" | awk '{print $6}')

        TOTAL=$((USER + NICE + SYSTEM + IDLE + IOWAIT))
        if [ "$TOTAL" -gt 0 ]; then
            CPU_USER=$((100 * (USER + NICE) / TOTAL))
            CPU_SYSTEM=$((100 * SYSTEM / TOTAL))
            CPU_IDLE=$((100 * IDLE / TOTAL))
            CPU_IOWAIT=$((100 * IOWAIT / TOTAL))
        else
            CPU_USER=0; CPU_SYSTEM=0; CPU_IDLE=100; CPU_IOWAIT=0
        fi
    else
        CPU_USER=0; CPU_SYSTEM=0; CPU_IDLE=100; CPU_IOWAIT=0
    fi

    echo "$CPU_USER,$CPU_SYSTEM,$CPU_IDLE,$CPU_IOWAIT"
}

get_cpu_metrics_macos() {
    # Use top for CPU metrics on macOS
    TOP_OUTPUT=$(top -l 1 -n 0 2>/dev/null | grep "CPU usage" || echo "")
    if [ -n "$TOP_OUTPUT" ]; then
        CPU_USER=$(echo "$TOP_OUTPUT" | sed 's/.*: *\([0-9.]*\)% user.*/\1/' | cut -d. -f1)
        CPU_SYSTEM=$(echo "$TOP_OUTPUT" | sed 's/.* \([0-9.]*\)% sys.*/\1/' | cut -d. -f1)
        CPU_IDLE=$(echo "$TOP_OUTPUT" | sed 's/.* \([0-9.]*\)% idle.*/\1/' | cut -d. -f1)
        CPU_IOWAIT=0  # macOS doesn't report iowait directly
    else
        CPU_USER=0; CPU_SYSTEM=0; CPU_IDLE=100; CPU_IOWAIT=0
    fi

    # Default values if parsing fails
    : "${CPU_USER:=0}"
    : "${CPU_SYSTEM:=0}"
    : "${CPU_IDLE:=100}"

    echo "$CPU_USER,$CPU_SYSTEM,$CPU_IDLE,$CPU_IOWAIT"
}

get_cpu_metrics() {
    case "$OS_TYPE" in
        linux)   get_cpu_metrics_linux ;;
        macos)   get_cpu_metrics_macos ;;
        *)       echo "0,0,100,0" ;;
    esac
}

# ============================================================================
# Memory Metrics
# ============================================================================

get_memory_metrics_linux() {
    if [ -f /proc/meminfo ]; then
        TOTAL_KB=$(grep "MemTotal" /proc/meminfo | awk '{print $2}')
        FREE_KB=$(grep "MemFree" /proc/meminfo | awk '{print $2}')
        AVAILABLE_KB=$(grep "MemAvailable" /proc/meminfo | awk '{print $2}')
        BUFFERS_KB=$(grep "Buffers" /proc/meminfo | awk '{print $2}')
        CACHED_KB=$(grep "^Cached" /proc/meminfo | awk '{print $2}')

        # Fallback for older kernels
        if [ -z "$AVAILABLE_KB" ]; then
            AVAILABLE_KB=$((FREE_KB + BUFFERS_KB + CACHED_KB))
        fi

        TOTAL_MB=$((TOTAL_KB / 1024))
        USED_MB=$(( (TOTAL_KB - AVAILABLE_KB) / 1024 ))
        AVAILABLE_MB=$((AVAILABLE_KB / 1024))

        if [ "$TOTAL_MB" -gt 0 ]; then
            MEM_PERCENT=$((100 * USED_MB / TOTAL_MB))
        else
            MEM_PERCENT=0
        fi
    else
        TOTAL_MB=0; USED_MB=0; AVAILABLE_MB=0; MEM_PERCENT=0
    fi

    echo "$TOTAL_MB,$USED_MB,$AVAILABLE_MB,$MEM_PERCENT"
}

get_memory_metrics_macos() {
    # Total memory
    TOTAL_BYTES=$(sysctl -n hw.memsize 2>/dev/null || echo 0)
    TOTAL_MB=$((TOTAL_BYTES / 1024 / 1024))

    # Parse vm_stat for memory usage
    if command -v vm_stat >/dev/null 2>&1; then
        VM_STAT=$(vm_stat)
        PAGE_SIZE=$(echo "$VM_STAT" | head -1 | grep -o '[0-9]*' || echo 4096)

        FREE_PAGES=$(echo "$VM_STAT" | grep "Pages free" | awk '{print $3}' | tr -d '.')
        ACTIVE_PAGES=$(echo "$VM_STAT" | grep "Pages active" | awk '{print $3}' | tr -d '.')
        INACTIVE_PAGES=$(echo "$VM_STAT" | grep "Pages inactive" | awk '{print $3}' | tr -d '.')
        WIRED_PAGES=$(echo "$VM_STAT" | grep "Pages wired" | awk '{print $4}' | tr -d '.')
        COMPRESSED_PAGES=$(echo "$VM_STAT" | grep "Pages occupied by compressor" | awk '{print $5}' | tr -d '.' || echo 0)

        : "${FREE_PAGES:=0}"
        : "${ACTIVE_PAGES:=0}"
        : "${INACTIVE_PAGES:=0}"
        : "${WIRED_PAGES:=0}"
        : "${COMPRESSED_PAGES:=0}"

        # Available = free + inactive (can be reclaimed)
        AVAILABLE_MB=$(( (FREE_PAGES + INACTIVE_PAGES) * PAGE_SIZE / 1024 / 1024 ))
        USED_MB=$(( (ACTIVE_PAGES + WIRED_PAGES + COMPRESSED_PAGES) * PAGE_SIZE / 1024 / 1024 ))
    else
        AVAILABLE_MB=$((TOTAL_MB / 2))
        USED_MB=$((TOTAL_MB / 2))
    fi

    if [ "$TOTAL_MB" -gt 0 ]; then
        MEM_PERCENT=$((100 * USED_MB / TOTAL_MB))
    else
        MEM_PERCENT=0
    fi

    echo "$TOTAL_MB,$USED_MB,$AVAILABLE_MB,$MEM_PERCENT"
}

get_memory_metrics() {
    case "$OS_TYPE" in
        linux)   get_memory_metrics_linux ;;
        macos)   get_memory_metrics_macos ;;
        *)       echo "0,0,0,0" ;;
    esac
}

# ============================================================================
# Disk I/O Metrics
# ============================================================================

# Store previous values for rate calculation
PREV_DISK_READ=0
PREV_DISK_WRITE=0
PREV_DISK_READ_OPS=0
PREV_DISK_WRITE_OPS=0
PREV_DISK_TIME=0

get_disk_metrics_linux() {
    DISK_READ=0
    DISK_WRITE=0
    DISK_READ_OPS=0
    DISK_WRITE_OPS=0

    if [ -f /proc/diskstats ]; then
        # Sum all disk activity (filter real disks)
        while read -r _ _ NAME READ_COMPLETED _ READ_SECTORS _ WRITE_COMPLETED _ WRITE_SECTORS _; do
            case "$NAME" in
                sd*|nvme*|vd*|xvd*)
                    # Skip partitions (only count whole disks)
                    case "$NAME" in
                        *[0-9]) continue ;;
                        *p[0-9]) continue ;;
                    esac
                    DISK_READ_OPS=$((DISK_READ_OPS + READ_COMPLETED))
                    DISK_WRITE_OPS=$((DISK_WRITE_OPS + WRITE_COMPLETED))
                    # Sectors are 512 bytes
                    DISK_READ=$((DISK_READ + READ_SECTORS / 2))
                    DISK_WRITE=$((DISK_WRITE + WRITE_SECTORS / 2))
                    ;;
            esac
        done < /proc/diskstats
    fi

    # Calculate rates
    CURRENT_TIME=$(get_epoch)
    if [ "$PREV_DISK_TIME" -gt 0 ]; then
        ELAPSED=$((CURRENT_TIME - PREV_DISK_TIME))
        if [ "$ELAPSED" -gt 0 ]; then
            DISK_READ_RATE=$(( (DISK_READ - PREV_DISK_READ) / ELAPSED ))
            DISK_WRITE_RATE=$(( (DISK_WRITE - PREV_DISK_WRITE) / ELAPSED ))
            DISK_READ_IOPS=$(( (DISK_READ_OPS - PREV_DISK_READ_OPS) / ELAPSED ))
            DISK_WRITE_IOPS=$(( (DISK_WRITE_OPS - PREV_DISK_WRITE_OPS) / ELAPSED ))
        else
            DISK_READ_RATE=0; DISK_WRITE_RATE=0; DISK_READ_IOPS=0; DISK_WRITE_IOPS=0
        fi
    else
        DISK_READ_RATE=0; DISK_WRITE_RATE=0; DISK_READ_IOPS=0; DISK_WRITE_IOPS=0
    fi

    # Store for next iteration
    PREV_DISK_READ=$DISK_READ
    PREV_DISK_WRITE=$DISK_WRITE
    PREV_DISK_READ_OPS=$DISK_READ_OPS
    PREV_DISK_WRITE_OPS=$DISK_WRITE_OPS
    PREV_DISK_TIME=$CURRENT_TIME

    echo "$DISK_READ_RATE,$DISK_WRITE_RATE,$DISK_READ_IOPS,$DISK_WRITE_IOPS"
}

get_disk_metrics_macos() {
    # Use iostat on macOS
    if command -v iostat >/dev/null 2>&1; then
        IOSTAT=$(iostat -d -c 1 2>/dev/null | tail -1 || echo "")
        if [ -n "$IOSTAT" ]; then
            DISK_READ_RATE=$(echo "$IOSTAT" | awk '{print int($3)}')
            DISK_WRITE_RATE=$(echo "$IOSTAT" | awk '{print int($4)}')
            # iostat doesn't give IOPS directly on macOS
            DISK_READ_IOPS=0
            DISK_WRITE_IOPS=0
        else
            DISK_READ_RATE=0; DISK_WRITE_RATE=0; DISK_READ_IOPS=0; DISK_WRITE_IOPS=0
        fi
    else
        DISK_READ_RATE=0; DISK_WRITE_RATE=0; DISK_READ_IOPS=0; DISK_WRITE_IOPS=0
    fi

    echo "$DISK_READ_RATE,$DISK_WRITE_RATE,$DISK_READ_IOPS,$DISK_WRITE_IOPS"
}

get_disk_metrics() {
    case "$OS_TYPE" in
        linux)   get_disk_metrics_linux ;;
        macos)   get_disk_metrics_macos ;;
        *)       echo "0,0,0,0" ;;
    esac
}

# ============================================================================
# Network Metrics
# ============================================================================

PREV_NET_RX=0
PREV_NET_TX=0
PREV_NET_TIME=0

get_network_metrics_linux() {
    NET_RX=0
    NET_TX=0

    if [ -f /proc/net/dev ]; then
        while read -r LINE; do
            # Skip header lines
            case "$LINE" in
                *"|"*) continue ;;
            esac

            IFACE=$(echo "$LINE" | cut -d: -f1 | tr -d ' ')
            VALUES=$(echo "$LINE" | cut -d: -f2)

            # Skip loopback
            case "$IFACE" in
                lo) continue ;;
            esac

            RX_BYTES=$(echo "$VALUES" | awk '{print $1}')
            TX_BYTES=$(echo "$VALUES" | awk '{print $9}')

            NET_RX=$((NET_RX + RX_BYTES / 1024))
            NET_TX=$((NET_TX + TX_BYTES / 1024))
        done < /proc/net/dev
    fi

    # Calculate rates
    CURRENT_TIME=$(get_epoch)
    if [ "$PREV_NET_TIME" -gt 0 ]; then
        ELAPSED=$((CURRENT_TIME - PREV_NET_TIME))
        if [ "$ELAPSED" -gt 0 ]; then
            NET_RX_RATE=$(( (NET_RX - PREV_NET_RX) / ELAPSED ))
            NET_TX_RATE=$(( (NET_TX - PREV_NET_TX) / ELAPSED ))
        else
            NET_RX_RATE=0; NET_TX_RATE=0
        fi
    else
        NET_RX_RATE=0; NET_TX_RATE=0
    fi

    PREV_NET_RX=$NET_RX
    PREV_NET_TX=$NET_TX
    PREV_NET_TIME=$CURRENT_TIME

    echo "$NET_RX_RATE,$NET_TX_RATE"
}

get_network_metrics_macos() {
    # macOS network metrics are complex to get accurately
    # Use netstat for basic info, but rates are not easily available
    NET_RX_RATE=0
    NET_TX_RATE=0

    if command -v netstat >/dev/null 2>&1; then
        # Get total bytes from en0 (primary interface)
        # netstat -ib format: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll
        STATS=$(netstat -ib 2>/dev/null | grep "^en0" | grep "<Link" | head -1 || echo "")
        if [ -n "$STATS" ]; then
            # Extract Ibytes (column 7) and Obytes (column 10)
            IBYTES=$(echo "$STATS" | awk '{print $7}' | tr -cd '0-9')
            OBYTES=$(echo "$STATS" | awk '{print $10}' | tr -cd '0-9')

            # For now just report 0 rates (would need previous values for rate calculation)
            : "${IBYTES:=0}"
            : "${OBYTES:=0}"
        fi
    fi

    echo "$NET_RX_RATE,$NET_TX_RATE"
}

get_network_metrics() {
    case "$OS_TYPE" in
        linux)   get_network_metrics_linux ;;
        macos)   get_network_metrics_macos ;;
        *)       echo "0,0" ;;
    esac
}

# ============================================================================
# PostgreSQL Process Metrics
# ============================================================================

get_postgres_metrics() {
    PG_CONNECTIONS=0
    PG_CPU=0
    PG_MEM_MB=0

    # Count postgres processes
    PG_PIDS=$(pgrep -f "postgres" 2>/dev/null || echo "")

    if [ -n "$PG_PIDS" ]; then
        PG_CONNECTIONS=$(echo "$PG_PIDS" | wc -l | tr -d ' ')

        # Get aggregate CPU and memory for postgres processes
        for PID in $PG_PIDS; do
            if [ -f "/proc/$PID/stat" ]; then
                # Linux: read from /proc
                STAT=$(cat "/proc/$PID/stat" 2>/dev/null || echo "")
                # Memory in pages
                RSS=$(echo "$STAT" | awk '{print $24}')
                if [ -n "$RSS" ]; then
                    PAGE_SIZE=$(getconf PAGE_SIZE 2>/dev/null || echo 4096)
                    PG_MEM_MB=$((PG_MEM_MB + RSS * PAGE_SIZE / 1024 / 1024))
                fi
            elif command -v ps >/dev/null 2>&1; then
                # macOS/BSD: use ps
                PS_OUT=$(ps -o %cpu=,rss= -p "$PID" 2>/dev/null | head -1 || echo "0 0")
                CPU=$(echo "$PS_OUT" | awk '{print int($1)}')
                RSS=$(echo "$PS_OUT" | awk '{print int($2)}')
                PG_CPU=$((PG_CPU + CPU))
                PG_MEM_MB=$((PG_MEM_MB + RSS / 1024))
            fi
        done
    fi

    echo "$PG_CONNECTIONS,$PG_CPU,$PG_MEM_MB"
}

# ============================================================================
# Main Monitoring Loop
# ============================================================================

# Check if already running
if [ -f "$PID_FILE" ]; then
    RUNNING_PID=$(cat "$PID_FILE")
    if kill -0 "$RUNNING_PID" 2>/dev/null; then
        echo "Monitor already running (PID: $RUNNING_PID)"
        echo "Use --stop to stop it first"
        exit 1
    else
        rm -f "$PID_FILE"
    fi
fi

# Start monitoring
echo "Starting resource monitor..."
echo "  Output:   $OUTPUT_FILE"
echo "  Interval: ${INTERVAL}s"
if [ "$MAX_DURATION" -gt 0 ]; then
    echo "  Duration: ${MAX_DURATION}s"
fi
if [ -n "$MONITOR_PID" ]; then
    echo "  Process:  $MONITOR_PID"
fi
echo ""

# Write CSV header
cat > "$OUTPUT_FILE" << EOF
timestamp,cpu_user,cpu_system,cpu_idle,cpu_iowait,mem_total_mb,mem_used_mb,mem_available_mb,mem_percent,disk_read_kb,disk_write_kb,disk_iops_read,disk_iops_write,net_rx_kb,net_tx_kb,pg_connections,pg_cpu,pg_mem_mb
EOF

# Store our PID
echo $$ > "$PID_FILE"

# Cleanup on exit
cleanup() {
    rm -f "$PID_FILE"
    echo ""
    echo "Monitor stopped."
    if [ -f "$OUTPUT_FILE" ]; then
        LINES=$(wc -l < "$OUTPUT_FILE")
        echo "Collected $((LINES - 1)) samples to $OUTPUT_FILE"
    fi
}
trap cleanup EXIT INT TERM

# Main loop
START_TIME=$(get_epoch)
SAMPLE_COUNT=0

echo "Monitoring... (Ctrl+C to stop)"

while true; do
    TIMESTAMP=$(get_timestamp)
    CPU_METRICS=$(get_cpu_metrics)
    MEM_METRICS=$(get_memory_metrics)
    DISK_METRICS=$(get_disk_metrics)
    NET_METRICS=$(get_network_metrics)
    PG_METRICS=$(get_postgres_metrics)

    # Write sample
    echo "$TIMESTAMP,$CPU_METRICS,$MEM_METRICS,$DISK_METRICS,$NET_METRICS,$PG_METRICS" >> "$OUTPUT_FILE"

    SAMPLE_COUNT=$((SAMPLE_COUNT + 1))

    # Check duration limit
    if [ "$MAX_DURATION" -gt 0 ]; then
        ELAPSED=$(($(get_epoch) - START_TIME))
        if [ "$ELAPSED" -ge "$MAX_DURATION" ]; then
            echo "Duration limit reached ($MAX_DURATION seconds)"
            break
        fi
    fi

    # Sleep until next interval
    sleep "$INTERVAL"
done

exit 0
