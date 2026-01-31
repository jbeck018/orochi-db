# Orochi DB Competitor Comparison Report

{{REPORT_ID}}

**Generated:** {{TIMESTAMP}}
**Framework Version:** 1.0.0

---

## Transparency Checklist

Before reviewing these benchmarks, please verify:

- [ ] Hardware specifications match your target environment
- [ ] Software versions are documented and current
- [ ] Configuration files are reproducible
- [ ] Raw results are available in JSON/CSV format
- [ ] Statistical methodology is clearly described
- [ ] Benchmark scripts can be independently verified

---

## Executive Summary

{{EXECUTIVE_SUMMARY}}

### Key Findings

| Workload | Orochi DB Performance | Best Competitor | Orochi Advantage |
|----------|----------------------|-----------------|------------------|
{{KEY_FINDINGS_TABLE}}

### Performance Highlights

- **OLTP (TPC-C):** {{TPCC_SUMMARY}}
- **Time-Series (TSBS):** {{TSBS_SUMMARY}}
- **Analytics (ClickBench):** {{CLICKBENCH_SUMMARY}}

---

## Hardware Specifications

All benchmarks were run on identical hardware configurations:

| Specification | Value |
|---------------|-------|
| **Instance Type** | {{INSTANCE_TYPE}} |
| **CPU Model** | {{CPU_MODEL}} |
| **CPU Cores** | {{CPU_CORES}} |
| **CPU Threads** | {{CPU_THREADS}} |
| **Memory (RAM)** | {{MEMORY_GB}} GB |
| **Storage Type** | {{STORAGE_TYPE}} |
| **Storage Size** | {{STORAGE_SIZE}} |
| **Storage IOPS** | {{STORAGE_IOPS}} |
| **Network** | {{NETWORK_BANDWIDTH}} |

### Operating System

| Property | Value |
|----------|-------|
| **OS** | {{OS_NAME}} |
| **Kernel** | {{KERNEL_VERSION}} |
| **File System** | {{FILESYSTEM}} |

---

## Software Versions

### Database Systems

| System | Version | Extension Version | Notes |
|--------|---------|------------------|-------|
| **Orochi DB** | PostgreSQL {{PG_VERSION}} | orochi {{OROCHI_VERSION}} | With columnar, sharding, time-series |
| **TimescaleDB** | PostgreSQL {{TIMESCALE_PG_VERSION}} | timescaledb {{TIMESCALE_VERSION}} | Community edition |
| **Citus** | PostgreSQL {{CITUS_PG_VERSION}} | citus {{CITUS_VERSION}} | Open source |
| **Vanilla PostgreSQL** | {{VANILLA_PG_VERSION}} | N/A | Default configuration |

### Benchmark Tools

| Tool | Version | Configuration |
|------|---------|---------------|
| **BenchmarkSQL** | {{BENCHMARKSQL_VERSION}} | {{BENCHMARKSQL_CONFIG}} |
| **TSBS** | {{TSBS_VERSION}} | {{TSBS_CONFIG}} |
| **ClickBench** | Official Suite | {{CLICKBENCH_CONFIG}} |

---

## Configuration Details

### Orochi DB Configuration

```ini
# postgresql.conf (key settings)
shared_preload_libraries = 'orochi'
shared_buffers = {{SHARED_BUFFERS}}
effective_cache_size = {{EFFECTIVE_CACHE_SIZE}}
maintenance_work_mem = {{MAINTENANCE_WORK_MEM}}
work_mem = {{WORK_MEM}}
max_parallel_workers_per_gather = {{MAX_PARALLEL_WORKERS}}
max_worker_processes = {{MAX_WORKER_PROCESSES}}

# Orochi-specific
orochi.columnar_compression = 'lz4'
orochi.default_shard_count = 32
orochi.time_chunk_interval = '1 day'
```

### Competitor Configurations

<details>
<summary>TimescaleDB Configuration</summary>

```ini
# postgresql.conf
shared_preload_libraries = 'timescaledb'
shared_buffers = {{SHARED_BUFFERS}}
# ... (matching settings)
```

</details>

<details>
<summary>Citus Configuration</summary>

```ini
# postgresql.conf
shared_preload_libraries = 'citus'
shared_buffers = {{SHARED_BUFFERS}}
# ... (matching settings)
```

</details>

---

## TPC-C Results (OLTP)

### Configuration

| Parameter | Value |
|-----------|-------|
| Warehouses | {{TPCC_WAREHOUSES}} |
| Terminals | {{TPCC_TERMINALS}} |
| Ramp-up Time | {{TPCC_RAMPUP}} minutes |
| Measurement Time | {{TPCC_DURATION}} minutes |

### Throughput Results

| Database | tpmC | NOPM | Relative Performance |
|----------|------|------|---------------------|
{{TPCC_THROUGHPUT_TABLE}}

### Latency Results (milliseconds)

| Database | P50 | P95 | P99 | P99.9 |
|----------|-----|-----|-----|-------|
{{TPCC_LATENCY_TABLE}}

### TPC-C Performance Chart

![TPC-C Throughput Comparison]({{CHARTS_DIR}}/tpcc_throughput.svg)

![TPC-C Latency Distribution]({{CHARTS_DIR}}/tpcc_latency.svg)

---

## TSBS Results (Time-Series)

### Configuration

| Parameter | Value |
|-----------|-------|
| Workload | {{TSBS_WORKLOAD}} |
| Scale (Hosts) | {{TSBS_SCALE}} |
| Data Duration | {{TSBS_DURATION}} |
| Workers | {{TSBS_WORKERS}} |

### Ingestion Performance

| Database | Rows/Second | Total Rows | Duration (s) |
|----------|-------------|------------|--------------|
{{TSBS_INGESTION_TABLE}}

### Query Performance (Average Latency in ms)

| Query Type | Orochi | TimescaleDB | Citus | Vanilla |
|------------|--------|-------------|-------|---------|
{{TSBS_QUERY_TABLE}}

### TSBS Performance Chart

![TSBS Ingestion Rate]({{CHARTS_DIR}}/tsbs_ingestion.svg)

![TSBS Query Latency]({{CHARTS_DIR}}/tsbs_queries.svg)

---

## ClickBench Results (Analytics)

### Configuration

| Parameter | Value |
|-----------|-------|
| Dataset | hits.tsv (~100GB) |
| Queries | 43 analytical queries |
| Runs per Query | {{CLICKBENCH_RUNS}} |

### Query Performance Summary

| Database | Total Time (s) | Avg Query (s) | Relative |
|----------|----------------|---------------|----------|
{{CLICKBENCH_SUMMARY_TABLE}}

### Individual Query Times (Cold Cache)

| Query | Orochi | TimescaleDB | Citus | Vanilla |
|-------|--------|-------------|-------|---------|
{{CLICKBENCH_COLD_TABLE}}

### Individual Query Times (Hot Cache)

| Query | Orochi | TimescaleDB | Citus | Vanilla |
|-------|--------|-------------|-------|---------|
{{CLICKBENCH_HOT_TABLE}}

### ClickBench Performance Chart

![ClickBench Total Time]({{CHARTS_DIR}}/clickbench_total.svg)

![ClickBench Query Breakdown]({{CHARTS_DIR}}/clickbench_queries.svg)

---

## Scalability Analysis

### Throughput vs. Data Size

| Data Size | Orochi | TimescaleDB | Citus | Vanilla |
|-----------|--------|-------------|-------|---------|
| 1 GB | {{SCALE_1GB}} | - | - | - |
| 10 GB | {{SCALE_10GB}} | - | - | - |
| 100 GB | {{SCALE_100GB}} | - | - | - |
| 1 TB | {{SCALE_1TB}} | - | - | - |

### Scalability Chart

![Scalability Comparison]({{CHARTS_DIR}}/scalability.svg)

---

## Statistical Methodology

### Data Collection

- Each benchmark was run **{{NUM_RUNS}}** times
- First run (cold cache) measured separately
- Results show **mean** values with **standard deviation**
- Outliers removed using **IQR method** (>1.5x interquartile range)

### Statistical Significance

| Comparison | p-value | Significant (p<0.05)? |
|------------|---------|----------------------|
{{STATISTICAL_TABLE}}

### Confidence Intervals (95%)

| Metric | Orochi CI | Competitor CI |
|--------|-----------|---------------|
{{CONFIDENCE_INTERVALS}}

---

## Reproduction Instructions

### Quick Start

```bash
# Clone repository
git clone https://github.com/orochi-db/orochi-db.git
cd orochi-db/extensions/postgres/benchmark/competitor

# Install dependencies
pip install -r requirements.txt

# Run the exact same benchmark
python competitor_bench.py \
    --benchmark={{BENCHMARKS}} \
    --targets={{TARGETS}} \
    --config=configs/{{CONFIG_FILE}}

# Or use the shell wrapper
./run_comparison.sh \
    --competitor={{COMPETITORS}} \
    --benchmark={{BENCHMARKS}}
```

### Docker Compose Setup

```bash
# Start all databases
docker-compose -f docker-compose.benchmark.yml up -d

# Run benchmarks
./run_comparison.sh --competitor=all

# Stop databases
docker-compose -f docker-compose.benchmark.yml down
```

### Manual Setup

1. **Install Orochi DB:**
   ```bash
   cd extensions/postgres
   make && sudo make install
   ```

2. **Install Competitors:**
   ```bash
   # TimescaleDB
   apt install timescaledb-2-postgresql-16

   # Citus
   apt install postgresql-16-citus
   ```

3. **Configure Databases:**
   - Apply configurations from `configs/` directory
   - Restart PostgreSQL services

4. **Run Benchmarks:**
   ```bash
   ./run_comparison.sh --competitor=timescaledb --benchmark=all
   ```

---

## Raw Data

### JSON Results

- [Full Results (JSON)](results/{{REPORT_ID}}.json)
- [TPC-C Raw Data](results/{{REPORT_ID}}_tpcc.json)
- [TSBS Raw Data](results/{{REPORT_ID}}_tsbs.json)
- [ClickBench Raw Data](results/{{REPORT_ID}}_clickbench.json)

### CSV Results

- [Summary CSV](results/{{REPORT_ID}}_summary.csv)
- [TPC-C CSV](results/{{REPORT_ID}}_tpcc.csv)
- [TSBS CSV](results/{{REPORT_ID}}_tsbs.csv)
- [ClickBench CSV](results/{{REPORT_ID}}_clickbench.csv)

---

## Limitations and Caveats

1. **Single Node Testing:** These benchmarks are primarily single-node. Distributed performance may vary.

2. **Default Configurations:** Competitors use near-default configurations tuned for fair comparison. Production deployments may achieve better results with expert tuning.

3. **Hardware Specific:** Results are specific to the test hardware. Performance ratios may differ on other configurations.

4. **Point-in-Time:** These results reflect a specific version snapshot. Performance may change with updates.

5. **Workload Specific:** Real-world workloads may differ. These benchmarks represent common patterns but not all use cases.

---

## Feedback and Contributions

We welcome feedback on our benchmarking methodology:

- **Issues:** [GitHub Issues](https://github.com/orochi-db/orochi-db/issues)
- **Discussions:** [GitHub Discussions](https://github.com/orochi-db/orochi-db/discussions)
- **Pull Requests:** Improvements to benchmark scripts welcome

### Third-Party Verification

If you'd like to independently verify these results:

1. Clone the repository
2. Follow the reproduction instructions above
3. Run on your own hardware
4. Share your results with the community

---

## Version History

| Date | Version | Changes |
|------|---------|---------|
| {{REPORT_DATE}} | 1.0 | Initial benchmark results |

---

## License

This benchmark framework and results are provided under the **Apache 2.0 License**.

Database products benchmarked are property of their respective owners:
- TimescaleDB is a trademark of Timescale, Inc.
- Citus is a trademark of Microsoft Corporation
- PostgreSQL is a trademark of the PostgreSQL Community Association of Canada

---

*Report generated by Orochi DB Competitor Comparison Framework v1.0*
*{{TIMESTAMP}}*
