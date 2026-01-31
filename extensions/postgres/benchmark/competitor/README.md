# Orochi DB Competitor Comparison Benchmark Framework

A comprehensive benchmarking framework for comparing Orochi DB against competitors
using industry-standard benchmarks with full transparency and reproducibility.

## Features

- **Standardized Benchmarks**: TPC-C (OLTP), TPC-H (Analytics), TSBS (Time-Series), ClickBench (Columnar)
- **Multiple Targets**: Orochi, TimescaleDB, Citus, QuestDB, Vanilla PostgreSQL
- **Automated Reporting**: JSON, Markdown, and CSV output formats
- **Chart Generation**: SVG and PNG visualizations
- **Full Transparency**: Hardware specs, software versions, configurations documented
- **Reproducibility**: Complete scripts for independent verification

## Quick Start

```bash
# Install Python dependencies (optional, for charts)
pip install -r requirements.txt

# Run comparison against TimescaleDB
./run_comparison.sh --competitor=timescaledb

# Run all benchmarks against all competitors
./run_comparison.sh --competitor=all --benchmark=all

# Generate charts from existing results
python generate_charts.py --input results/comparison_*.json --output charts/
```

## Directory Structure

```
competitor/
├── competitor_bench.py     # Main Python orchestrator
├── run_comparison.sh       # Shell-based comparison runner
├── tpcc_wrapper.sh         # TPC-C benchmark wrapper
├── tsbs_wrapper.sh         # TSBS benchmark wrapper
├── clickbench_wrapper.sh   # ClickBench wrapper
├── generate_charts.py      # Chart generation
├── report_template.md      # Markdown report template
├── requirements.txt        # Python dependencies
├── README.md               # This file
├── results/                # Benchmark results output
├── charts/                 # Generated charts
├── configs/                # Database configurations
├── data/                   # Downloaded benchmark data
└── tools/                  # Benchmark tools (BenchmarkSQL, TSBS, etc.)
```

## Benchmarks

### TPC-C (OLTP)

Industry-standard OLTP benchmark measuring transactions per minute.

```bash
# Run TPC-C only
./run_comparison.sh --competitor=timescaledb --benchmark=tpcc

# Configure warehouses and duration
BENCH_WAREHOUSES=100 BENCH_DURATION=30 ./tpcc_wrapper.sh
```

**Metrics:**
- tpmC (transactions per minute)
- NOPM (new orders per minute)
- Latency percentiles (P50, P95, P99)

### TSBS (Time Series Benchmark Suite)

Industry-standard time-series benchmark from Timescale.

```bash
# Install TSBS tools
./tsbs_wrapper.sh install

# Run TSBS benchmark
./run_comparison.sh --competitor=timescaledb --benchmark=tsbs
```

**Metrics:**
- Ingestion rate (rows/second)
- Query latencies by type
- Scalability curves

### ClickBench

43 analytical queries on web analytics dataset (~100GB).

```bash
# Download dataset (15GB compressed)
./clickbench_wrapper.sh download

# Run ClickBench
./run_comparison.sh --competitor=citus --benchmark=clickbench
```

**Metrics:**
- Individual query times
- Cold vs hot cache performance
- Total execution time

## Usage

### Python Orchestrator

```bash
# Full comparison with all options
python competitor_bench.py \
    --benchmark=tpcc,tsbs,clickbench \
    --targets=orochi,timescaledb,citus \
    --output-dir=./results \
    --generate-charts

# Report-only from existing results
python competitor_bench.py --report-only --results-dir=./results
```

### Shell Runner

```bash
# Basic usage
./run_comparison.sh --competitor=timescaledb

# Multiple competitors
./run_comparison.sh --competitor=timescaledb,citus

# Custom hosts
./run_comparison.sh \
    --competitor=timescaledb \
    --orochi-host=orochi.local \
    --orochi-port=5432 \
    --competitor-host=timescale.local \
    --competitor-port=5433

# Dry run (show commands without executing)
./run_comparison.sh --competitor=all --dry-run
```

## Configuration

### Database Connection

Environment variables for database connections:

```bash
export BENCH_HOST=localhost
export BENCH_PORT=5432
export BENCH_DB=benchmark_db
export BENCH_USER=postgres
export BENCH_PASSWORD=secret
```

### Benchmark Parameters

```bash
# TPC-C
export BENCH_WAREHOUSES=100    # Scale factor
export BENCH_TERMINALS=10      # Concurrent connections
export BENCH_DURATION=10       # Minutes
export BENCH_RAMPUP=2          # Warmup minutes

# TSBS
export BENCH_SCALE=100         # Number of simulated hosts
export BENCH_WORKLOAD=devops   # Workload type
export BENCH_WORKERS=8         # Parallel workers

# ClickBench
export BENCH_RUNS=3            # Runs per query
```

### Custom Configuration File

Create `configs/custom.json`:

```json
{
  "orochi": {
    "host": "localhost",
    "port": 5432,
    "database": "orochi_bench",
    "user": "postgres",
    "extension": "orochi"
  },
  "timescaledb": {
    "host": "localhost",
    "port": 5433,
    "database": "timescale_bench",
    "user": "postgres",
    "extension": "timescaledb"
  }
}
```

Run with custom config:

```bash
python competitor_bench.py --config=configs/custom.json
```

## Report Generation

### Automatic Reports

Reports are generated automatically after each benchmark run:

- `results/<run_id>.json` - Full JSON results
- `results/<run_id>.md` - Markdown report
- `results/<run_id>_raw.csv` - Raw data CSV
- `charts/<benchmark>_*.svg` - Visualization charts

### Manual Report Generation

```bash
# From existing JSON results
python generate_charts.py \
    --input results/comparison_20240101_120000.json \
    --output charts/ \
    --format both
```

## Transparency Checklist

Our benchmarks follow these transparency principles:

- [ ] **Hardware documented**: CPU, memory, storage, network
- [ ] **Software versions**: PostgreSQL, extensions, benchmark tools
- [ ] **Configurations**: postgresql.conf settings documented
- [ ] **Raw data**: JSON and CSV exports available
- [ ] **Methodology**: Statistical methods described
- [ ] **Reproduction**: Scripts provided for independent verification

## Docker Setup

For isolated testing:

```bash
# Start database containers
docker-compose -f docker-compose.benchmark.yml up -d

# Run benchmarks
./run_comparison.sh --competitor=all

# Cleanup
docker-compose -f docker-compose.benchmark.yml down -v
```

## Installing Benchmark Tools

### BenchmarkSQL (TPC-C)

```bash
./tpcc_wrapper.sh install benchmarksql
```

### HammerDB (TPC-C alternative)

```bash
./tpcc_wrapper.sh install hammerdb
```

### TSBS

```bash
./tsbs_wrapper.sh install
```

## Troubleshooting

### Database Connection Issues

```bash
# Test Orochi connection
pg_isready -h localhost -p 5432

# Test with psql
psql -h localhost -p 5432 -U postgres -c "SELECT version();"
```

### Benchmark Tool Not Found

```bash
# Install BenchmarkSQL
./tpcc_wrapper.sh install benchmarksql

# Install TSBS
./tsbs_wrapper.sh install
```

### Large Dataset Downloads

ClickBench dataset is ~15GB compressed. Use resume on failure:

```bash
# Resume interrupted download
./clickbench_wrapper.sh download
```

## Contributing

We welcome contributions to improve benchmark accuracy and coverage:

1. Fork the repository
2. Create a feature branch
3. Run existing benchmarks to verify no regressions
4. Submit a pull request with benchmark results

## References

- [TPC-C Specification](http://www.tpc.org/tpcc/)
- [TSBS Repository](https://github.com/timescale/tsbs)
- [ClickBench](https://github.com/ClickHouse/ClickBench)
- [BenchmarkSQL](https://github.com/pgsql-io/benchmarksql)

## License

Apache 2.0 License

---

*Part of the Orochi DB Project*
