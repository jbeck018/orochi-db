# Orochi DB Deployment Guide

This guide provides comprehensive deployment instructions for Orochi DB across various cloud platforms and infrastructure configurations.

## Overview

Orochi DB is a PostgreSQL extension providing HTAP (Hybrid Transactional/Analytical Processing) capabilities. Deployment requires PostgreSQL 16, 17, or 18 with the extension installed and configured.

### Deployment Options Summary

| Deployment Type | Description | Complexity |
|-----------------|-------------|------------|
| **Single Node** | Development/testing, small workloads | Low |
| **Multi-Node Cluster** | Production with horizontal scaling | Medium |
| **Managed PostgreSQL** | Cloud-managed database with extension | Low-Medium |
| **Kubernetes** | Container orchestration for elastic scaling | High |
| **Hybrid Cloud** | Multi-region with tiered storage | High |

### When to Use Each Platform

- **AWS**: Enterprise deployments requiring maximum scalability, global presence, and comprehensive managed services. Best for organizations already invested in AWS ecosystem.

- **Google Cloud**: Analytics-heavy workloads benefiting from BigQuery integration, AlloyDB for PostgreSQL compatibility, and strong ML/AI pipeline support.

- **Oracle Cloud (OCI)**: Cost-sensitive deployments needing enterprise features. Competitive pricing and strong Oracle database migration paths.

- **Fly.io**: Developer-focused deployments, edge computing, and applications requiring low-latency global distribution with simpler operations.

### Cost Considerations

| Factor | Impact | Optimization Strategy |
|--------|--------|----------------------|
| Compute | 40-60% of cost | Right-size instances, use reserved capacity |
| Storage | 20-30% of cost | Enable tiered storage, use cold tiers for archives |
| Network | 10-20% of cost | Co-locate services, minimize cross-region traffic |
| Managed Services | Variable | Compare RDS vs self-managed based on team size |

**Cost-Saving Tips:**
- Use spot/preemptible instances for worker nodes
- Enable automatic tiering to move data to cheaper storage
- Consider reserved instances for predictable workloads
- Use compression (ZSTD for cold data, LZ4 for hot data)

---

## Quick Comparison Table

| Platform | Best For | Min Monthly Cost | Max Scale | Managed DB | Setup Time |
|----------|----------|------------------|-----------|------------|------------|
| **AWS** | Enterprise, Global | $200+ | Unlimited | RDS, Aurora | 2-4 hours |
| **Google Cloud** | Analytics, ML | $150+ | Very High | Cloud SQL, AlloyDB | 2-3 hours |
| **Oracle Cloud** | Cost-Sensitive | $50+ | High | OCI Database | 2-3 hours |
| **Fly.io** | Developers, Edge | $20+ | Medium | Fly Postgres | 30 min |

### Feature Comparison

| Feature | AWS | Google Cloud | OCI | Fly.io |
|---------|-----|--------------|-----|--------|
| Managed PostgreSQL 16+ | Yes | Yes | Yes | Yes |
| Custom Extensions | RDS: Limited, EC2: Full | Cloud SQL: Limited, GCE: Full | Full | Full |
| S3-Compatible Storage | S3 | Cloud Storage | Object Storage | Tigris |
| Global Distribution | Yes | Yes | Limited | Yes |
| Kubernetes Support | EKS | GKE | OKE | Machines |
| Serverless Options | Aurora Serverless | AlloyDB Serverless | No | No |
| Free Tier | Yes | Yes | Always Free | Limited |

---

## Hardware Requirements

### Minimum Specifications (Development/Testing)

| Resource | Minimum | Notes |
|----------|---------|-------|
| CPU | 2 cores | x86_64 with SSE4.2+ or ARM64 |
| RAM | 4 GB | Shared buffers: 1 GB |
| Storage | 20 GB SSD | NVMe preferred |
| Network | 100 Mbps | For single-node only |

### Recommended for Production

| Resource | Small | Medium | Large | Notes |
|----------|-------|--------|-------|-------|
| CPU | 4 cores | 16 cores | 64+ cores | More cores for parallel queries |
| RAM | 16 GB | 64 GB | 256+ GB | 25% for shared_buffers |
| Storage | 100 GB NVMe | 1 TB NVMe | 10+ TB NVMe | RAID 10 for high-write workloads |
| Network | 1 Gbps | 10 Gbps | 25+ Gbps | Low latency between nodes |

### HTAP-Optimized Configurations

For mixed transactional and analytical workloads:

```
# Coordinator Node (Query Routing)
CPU: 8+ cores (high single-thread performance)
RAM: 32+ GB
Storage: 200 GB NVMe (metadata + query planning)
Network: 10 Gbps (low latency critical)

# Worker Nodes (Data Processing)
CPU: 16+ cores (high parallelism)
RAM: 64+ GB (large shared buffers)
Storage: 1+ TB NVMe (data storage)
Network: 10 Gbps

# Analytics Workers (Columnar Processing)
CPU: 32+ cores with AVX2/AVX-512 (SIMD vectorization)
RAM: 128+ GB (batch processing)
Storage: 2+ TB NVMe (columnar data)
Network: 25 Gbps (shuffle operations)
```

### Storage Configuration by Tier

| Tier | Storage Type | IOPS | Throughput | Cost |
|------|--------------|------|------------|------|
| Hot | NVMe SSD | 100K+ | 3+ GB/s | $$$$ |
| Warm | SSD | 10K+ | 500+ MB/s | $$ |
| Cold | S3/Object | N/A | 100+ MB/s | $ |
| Frozen | S3 Glacier | N/A | 10+ MB/s | Cents |

---

## Pre-Deployment Checklist

### PostgreSQL Version Requirements

- [x] PostgreSQL 16, 17, or 18 installed
- [x] Development headers available (postgresql-server-dev-all)
- [x] Verify version: `SELECT version();`

```bash
# Check PostgreSQL version
psql -c "SELECT version();"

# Should return PostgreSQL 16.x, 17.x, or 18.x
```

### Required Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    postgresql-16 \           # or 17, 18
    postgresql-server-dev-16 \
    liblz4-dev \              # LZ4 compression
    libzstd-dev \             # ZSTD compression
    libcurl4-openssl-dev \    # S3/HTTP operations
    libssl-dev \              # TLS support
    librdkafka-dev            # Kafka pipelines (optional)

# RHEL/CentOS
sudo dnf install -y \
    postgresql16-server \
    postgresql16-devel \
    lz4-devel \
    libzstd-devel \
    libcurl-devel \
    openssl-devel \
    librdkafka-devel
```

### Required PostgreSQL Extensions

```sql
-- Core requirements (usually pre-installed)
CREATE EXTENSION IF NOT EXISTS plpgsql;

-- Recommended for full functionality
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;  -- Query monitoring
CREATE EXTENSION IF NOT EXISTS pgcrypto;            -- Encryption support

-- Install Orochi DB
CREATE EXTENSION orochi;

-- Verify installation
SELECT * FROM pg_extension WHERE extname = 'orochi';
```

### Storage Requirements

| Data Volume | Hot Storage | Warm Storage | Cold Storage | Total |
|-------------|-------------|--------------|--------------|-------|
| 100 GB | 100 GB | 50 GB (compressed) | N/A | 150 GB |
| 1 TB | 500 GB | 300 GB | 200 GB | 1 TB |
| 10 TB | 2 TB | 3 TB | 5 TB | 10 TB |
| 100 TB | 10 TB | 30 TB | 60 TB | 100 TB |

**Storage Planning Formula:**
```
Hot Storage = Active Data Size
Warm Storage = Hot Size * 0.6 (with compression)
Cold Storage = Historical Data Size * 0.3 (with high compression)
WAL Storage = Hot Size * 0.1 (for PITR)
Temp Space = RAM Size * 2 (for sorts/joins)
```

### Network Configuration

#### Firewall Rules

| Port | Service | Direction | Required |
|------|---------|-----------|----------|
| 5432 | PostgreSQL | Inbound | Yes |
| 5433-5500 | Worker Nodes | Internal | Cluster only |
| 7000 | Raft Consensus | Internal | Cluster only |
| 9092 | Kafka (if used) | Outbound | Optional |
| 443 | S3/HTTPS | Outbound | For tiered storage |

#### DNS/Service Discovery

```bash
# Required hostnames for cluster deployment
coordinator.orochi.local    # Main coordinator
worker-1.orochi.local       # Worker node 1
worker-2.orochi.local       # Worker node 2
# ... additional workers

# Or use environment variables
export OROCHI_COORDINATOR_HOST=10.0.1.10
export OROCHI_WORKER_HOSTS=10.0.1.11,10.0.1.12,10.0.1.13
```

### Security Checklist

- [ ] TLS/SSL certificates configured
- [ ] `pg_hba.conf` restricts access appropriately
- [ ] Strong passwords for all database roles
- [ ] S3 credentials stored securely (not in config files)
- [ ] Network security groups configured
- [ ] Audit logging enabled
- [ ] Encryption at rest enabled

---

## Post-Deployment

### Health Checks

#### Basic Connectivity

```sql
-- Verify extension is loaded
SELECT * FROM pg_extension WHERE extname = 'orochi';

-- Check shared memory allocation
SHOW shared_preload_libraries;  -- Should include 'orochi'

-- Verify GUC parameters
SHOW orochi.default_shard_count;
SHOW orochi.enable_columnar_scan;
```

#### Cluster Health (Multi-Node)

```sql
-- Check node status
SELECT * FROM orochi_cluster_status();

-- Verify shard distribution
SELECT * FROM orochi_shard_distribution();

-- Check Raft consensus state
SELECT * FROM orochi_raft_status();
```

#### System Metrics

```sql
-- Query performance statistics
SELECT * FROM orochi_query_stats();

-- Storage utilization
SELECT * FROM orochi_storage_stats();

-- Compression ratios
SELECT
    table_name,
    pg_size_pretty(raw_size) as raw_size,
    pg_size_pretty(compressed_size) as compressed_size,
    round(compression_ratio, 2) as ratio
FROM orochi_compression_stats();
```

### Performance Tuning

#### PostgreSQL Configuration

```ini
# postgresql.conf - HTAP optimized settings

# Memory
shared_buffers = '8GB'              # 25% of RAM
effective_cache_size = '24GB'       # 75% of RAM
work_mem = '256MB'                  # Per-operation memory
maintenance_work_mem = '2GB'        # For VACUUM, CREATE INDEX

# Parallelism
max_parallel_workers = 16           # Match CPU cores
max_parallel_workers_per_gather = 8
max_parallel_maintenance_workers = 4

# WAL Configuration
wal_level = replica
max_wal_size = '8GB'
min_wal_size = '2GB'
checkpoint_completion_target = 0.9

# Query Planning
random_page_cost = 1.1              # For SSDs
effective_io_concurrency = 200      # For NVMe
default_statistics_target = 200     # Better query plans

# Orochi Extension
shared_preload_libraries = 'orochi'
orochi.default_shard_count = 32
orochi.enable_columnar_scan = on
orochi.vectorized_batch_size = 1024
orochi.jit_threshold = 10000
```

#### Orochi-Specific Tuning

```sql
-- Optimal shard count (rule of thumb: 2-4x CPU cores)
ALTER SYSTEM SET orochi.default_shard_count = 64;

-- Enable vectorized execution for analytics
ALTER SYSTEM SET orochi.enable_vectorized = on;

-- Configure compression for columnar storage
ALTER SYSTEM SET orochi.default_compression = 'zstd';
ALTER SYSTEM SET orochi.compression_level = 3;

-- Tiered storage thresholds
ALTER SYSTEM SET orochi.hot_tier_age = '1 day';
ALTER SYSTEM SET orochi.warm_tier_age = '7 days';
ALTER SYSTEM SET orochi.cold_tier_age = '30 days';

-- Apply changes
SELECT pg_reload_conf();
```

### Backup Configuration

#### Continuous Archiving (WAL)

```bash
# postgresql.conf
archive_mode = on
archive_command = 'aws s3 cp %p s3://orochi-backup/wal/%f'
# Or for other platforms:
# archive_command = 'gsutil cp %p gs://orochi-backup/wal/%f'
# archive_command = 'oci os object put -bn orochi-backup --file %p --name wal/%f'
```

#### Base Backup Schedule

```bash
# Daily full backup (recommended)
pg_basebackup -D /backup/base -Ft -z -P

# With S3 streaming
pg_basebackup -D - -Ft | aws s3 cp - s3://orochi-backup/base/backup-$(date +%Y%m%d).tar
```

#### Orochi-Specific Backup

```sql
-- Export metadata for distributed tables
SELECT orochi_export_metadata('/backup/orochi_metadata.json');

-- Backup includes:
-- - Shard mappings
-- - Chunk boundaries
-- - Tiering policies
-- - Continuous aggregate definitions
```

### Monitoring Setup

#### Prometheus Metrics

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'postgresql'
    static_configs:
      - targets: ['coordinator:9187']
    metrics_path: /metrics
```

#### Key Metrics to Monitor

| Metric | Warning Threshold | Critical Threshold |
|--------|-------------------|-------------------|
| Connection count | 80% of max_connections | 95% of max_connections |
| Replication lag | > 30 seconds | > 5 minutes |
| Disk usage | > 80% | > 90% |
| Query duration (p99) | > 5 seconds | > 30 seconds |
| Shard imbalance | > 20% variance | > 50% variance |
| Raft leader elections | > 1/hour | > 5/hour |

#### Alerting Rules

```yaml
# alerts.yml
groups:
  - name: orochi
    rules:
      - alert: OrochiHighReplicationLag
        expr: orochi_replication_lag_seconds > 300
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Replication lag exceeds 5 minutes"

      - alert: OrochiShardImbalance
        expr: orochi_shard_size_variance > 0.5
        for: 30m
        labels:
          severity: warning
        annotations:
          summary: "Shard sizes are significantly imbalanced"
```

---

## Platform-Specific Guides

For detailed deployment instructions on each platform, see:

- **[AWS Deployment Guide](AWS.md)** - Amazon Web Services (EC2, RDS, Aurora, EKS)
- **[Google Cloud Deployment Guide](GCLOUD.md)** - Google Cloud Platform (GCE, Cloud SQL, AlloyDB, GKE)
- **[Oracle Cloud Deployment Guide](OCI.md)** - Oracle Cloud Infrastructure (Compute, OCI Database)
- **[Fly.io Deployment Guide](FLYIO.md)** - Fly.io (Machines, Fly Postgres, Edge Deployment)

---

## Quick Start Commands

### Single-Node Development Setup

```bash
# 1. Install dependencies
make install-deps

# 2. Build extension
make && sudo make install

# 3. Configure PostgreSQL
echo "shared_preload_libraries = 'orochi'" | sudo tee -a /etc/postgresql/16/main/postgresql.conf

# 4. Restart PostgreSQL
sudo systemctl restart postgresql

# 5. Create extension
sudo -u postgres psql -c "CREATE EXTENSION orochi;"

# 6. Verify
sudo -u postgres psql -c "SELECT orochi_version();"
```

### Docker Quick Start

```bash
# Build Docker image
docker build -t orochi-db:latest .

# Run single node
docker run -d \
  --name orochi \
  -e POSTGRES_PASSWORD=secretpassword \
  -p 5432:5432 \
  orochi-db:latest

# Connect
psql -h localhost -U postgres -c "CREATE EXTENSION orochi;"
```

### Kubernetes Quick Start

```bash
# Deploy with Helm
helm repo add orochi https://charts.orochi-db.io
helm install orochi orochi/orochi-db \
  --set cluster.nodes=3 \
  --set storage.size=100Gi

# Check status
kubectl get pods -l app=orochi-db
```

---

## Troubleshooting

### Common Issues

| Issue | Symptom | Solution |
|-------|---------|----------|
| Extension not loading | ERROR: could not load library | Check `shared_preload_libraries`, restart PostgreSQL |
| Shared memory error | FATAL: could not map anonymous shared memory | Increase kernel shmmax/shmall |
| Connection refused | Could not connect to server | Check firewall, pg_hba.conf |
| Slow queries | Query timeout | Check explain analyze, add indexes, increase work_mem |
| Replication lag | Data not syncing | Check network, increase wal_sender_timeout |

### Diagnostic Commands

```bash
# Check PostgreSQL logs
tail -f /var/log/postgresql/postgresql-16-main.log

# Verify extension loaded
psql -c "SELECT * FROM pg_extension WHERE extname = 'orochi';"

# Check shared memory
ipcs -m

# Network connectivity
nc -zv worker-1 5432

# Disk I/O stats
iostat -x 1
```

### Getting Help

- **Documentation**: https://docs.orochi-db.io
- **GitHub Issues**: https://github.com/orochi-db/orochi/issues
- **Community Slack**: https://orochi-db.slack.com
- **Enterprise Support**: support@orochi-db.io

---

## Next Steps

1. Choose your deployment platform and follow the specific guide
2. Complete the pre-deployment checklist
3. Deploy and verify with health checks
4. Apply performance tuning based on your workload
5. Set up monitoring and alerting
6. Configure backups and test recovery

For production deployments, we recommend starting with a small cluster and scaling based on observed metrics.
