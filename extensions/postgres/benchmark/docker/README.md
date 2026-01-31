# Orochi DB Benchmark Docker Infrastructure

Comprehensive Docker infrastructure for benchmarking the Orochi DB PostgreSQL extension.

## Overview

This Docker infrastructure provides:

- **Dockerfile.benchmark**: Multi-stage Dockerfile with PostgreSQL 16, Orochi extension, and benchmark tools
- **docker-compose.yml**: Local development orchestration
- **docker-compose.cloud.yml**: Cloud deployment overrides with enhanced resources
- **entrypoint.sh**: Smart entrypoint with hardware auto-detection
- **Supporting files**: PostgreSQL configuration, init scripts, and environment templates

## Quick Start

### Basic Usage

```bash
# Navigate to the docker directory
cd extensions/postgres/benchmark/docker

# Copy environment template
cp .env.example .env

# Build and start services
docker-compose up -d orochi-db

# Run benchmarks
docker-compose run benchmark-runner
```

### Run Specific Benchmark Suites

```bash
# TPC-H benchmarks only
docker-compose run -e BENCHMARK_SUITE=tpch benchmark-runner

# Multiple suites
docker-compose run -e BENCHMARK_SUITE=tpch,timeseries,columnar benchmark-runner

# All available benchmarks
docker-compose run -e BENCHMARK_SUITE=all benchmark-runner
```

### High-Scale Benchmarking

```bash
# Large scale benchmark with more clients
docker-compose run \
  -e SCALE_FACTOR=100 \
  -e NUM_CLIENTS=64 \
  -e NUM_THREADS=16 \
  -e DURATION=300 \
  benchmark-runner
```

### Connection Pooler Comparison

```bash
# Start with pooler services
docker-compose --profile poolers up -d

# Run comparison benchmarks
docker-compose run -e COMPARE_POOLERS=true benchmark-runner
```

## Available Benchmark Suites

| Suite | Description |
|-------|-------------|
| `tpch` | TPC-H style analytical queries |
| `timeseries` | Time-series insert and query workloads |
| `columnar` | Columnar storage compression and scan benchmarks |
| `distributed` | Distributed query execution benchmarks |
| `vectorized` | Vectorized execution engine benchmarks |
| `pgbench` | Standard PostgreSQL pgbench |
| `sysbench` | Sysbench OLTP workloads |
| `tsbs` | Time Series Benchmark Suite |

## Configuration

### Environment Variables

Key configuration options in `.env`:

```bash
# Benchmark settings
BENCHMARK_SUITE=all           # Suites to run
SCALE_FACTOR=1                # Data scale (1=1GB, 10=10GB, etc.)
NUM_CLIENTS=auto              # Concurrent clients (auto-detect)
NUM_THREADS=auto              # Threads per client (auto-detect)
DURATION=60                   # Benchmark duration (seconds)
OUTPUT_FORMAT=json            # Output: json, csv, text

# Resource limits
DB_CPU_LIMIT=4                # Database CPU cores
DB_MEMORY_LIMIT=8G            # Database memory
BENCH_CPU_LIMIT=2             # Benchmark runner CPU
BENCH_MEMORY_LIMIT=4G         # Benchmark runner memory
```

### Hardware Auto-Detection

When `AUTO_DETECT_HARDWARE=true` (default), the entrypoint script:

1. Detects CPU core count
2. Detects available memory
3. Detects disk type (SSD/HDD/NVMe)
4. Auto-scales benchmark parameters:
   - `NUM_CLIENTS`: 2x CPU cores (capped by memory)
   - `NUM_THREADS`: 1 per core (max 16)
   - `SCALE_FACTOR`: Based on memory and disk type

## Cloud Deployment

### Using Cloud Overrides

```bash
# Deploy with cloud configuration
docker-compose -f docker-compose.yml -f docker-compose.cloud.yml up -d
```

### AWS Configuration

```bash
# Set AWS environment variables
export AWS_REGION=us-east-1
export AWS_S3_BUCKET=my-benchmark-results
export AWS_ACCESS_KEY_ID=your-key
export AWS_SECRET_ACCESS_KEY=your-secret

docker-compose -f docker-compose.yml -f docker-compose.cloud.yml \
  run benchmark-runner
```

### Multi-Region Testing

```bash
# Enable multi-region
export MULTI_REGION_ENABLED=true
export PRIMARY_REGION=us-east-1
export SECONDARY_REGIONS=eu-west-1,ap-northeast-1

docker-compose -f docker-compose.yml -f docker-compose.cloud.yml \
  --profile multi-region up -d
```

## Results

### Output Location

Results are saved to the `/results` volume (mounted from `benchmark-results` volume):

```
/results/
├── system_info.json          # System hardware info
├── summary_YYYYMMDD_HHMMSS.json
├── tpch/
│   └── tpch_results.json
├── timeseries/
│   └── timeseries_results.json
├── columnar/
│   └── columnar_results.json
├── pgbench/
│   └── pgbench_results.json
└── pooler_comparison/
    ├── direct_results.txt
    ├── pgbouncer_results.txt
    └── comparison_report.json
```

### Accessing Results

```bash
# Copy results from volume to host
docker cp benchmark-runner:/results ./local-results

# Or mount a host directory
docker-compose run \
  -v $(pwd)/results:/results \
  benchmark-runner
```

## Monitoring (Full Profile)

Start with monitoring services:

```bash
docker-compose --profile full up -d
```

Access monitoring:
- **Grafana**: http://localhost:3000 (admin/admin)
- **Prometheus**: http://localhost:9090
- **PostgreSQL Exporter**: http://localhost:9187/metrics

## Services Overview

### Core Services

| Service | Port | Description |
|---------|------|-------------|
| `orochi-db` | 5432 | PostgreSQL with Orochi extension |
| `benchmark-runner` | - | Executes benchmark suites |

### Connection Poolers (--profile poolers)

| Service | Port | Description |
|---------|------|-------------|
| `pgbouncer` | 6432 | PgBouncer connection pooler |
| `pgcat` | 6433 | PgCat modern pooler |
| `pgdog` | 6434 | PgDog lightweight pooler |

### Monitoring (--profile full)

| Service | Port | Description |
|---------|------|-------------|
| `prometheus` | 9090 | Metrics collection |
| `grafana` | 3000 | Visualization |
| `postgres-exporter` | 9187 | PostgreSQL metrics |

## Building Custom Images

### Build Benchmark Image

```bash
# Build from project root
docker build \
  -f extensions/postgres/benchmark/docker/Dockerfile.benchmark \
  -t orochi-benchmark:custom \
  .

# Build with specific PostgreSQL version
docker build \
  -f extensions/postgres/benchmark/docker/Dockerfile.benchmark \
  --build-arg PG_MAJOR=17 \
  -t orochi-benchmark:pg17 \
  .
```

## Troubleshooting

### Database Connection Issues

```bash
# Check database logs
docker-compose logs orochi-db

# Test connection
docker-compose exec orochi-db pg_isready

# Connect interactively
docker-compose exec orochi-db psql -U postgres -d orochi_bench
```

### Benchmark Failures

```bash
# Check benchmark logs
docker-compose logs benchmark-runner

# Run with verbose output
docker-compose run -e VERBOSE=true benchmark-runner

# Run interactively
docker-compose run --entrypoint bash benchmark-runner
```

### Resource Issues

```bash
# Check resource usage
docker stats

# Increase limits in .env
DB_CPU_LIMIT=8
DB_MEMORY_LIMIT=16G
```

## Directory Structure

```
docker/
├── Dockerfile.benchmark      # Multi-stage benchmark image
├── docker-compose.yml        # Local orchestration
├── docker-compose.cloud.yml  # Cloud deployment overrides
├── entrypoint.sh             # Smart benchmark entrypoint
├── .env.example              # Environment template
├── postgresql.conf           # Optimized PostgreSQL config
├── init-scripts/
│   └── 00-init-orochi.sql    # Database initialization
├── monitoring/               # Prometheus/Grafana configs
│   ├── prometheus.yml
│   └── grafana/
│       └── provisioning/
└── README.md                 # This file
```

## License

This benchmark infrastructure is part of Orochi DB and is licensed under the same terms.
