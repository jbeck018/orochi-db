# Deploying Orochi DB on Fly.io

This guide covers deploying Orochi DB on Fly.io, including managed Postgres, custom app deployment, Fly Machines, Tigris object storage integration, and multi-region configurations.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Fly Postgres](#fly-postgres)
3. [Custom App Deployment](#custom-app-deployment)
4. [Fly Machines](#fly-machines)
5. [Tigris Object Storage](#tigris-object-storage)
6. [Networking](#networking)
7. [Secrets Management](#secrets-management)
8. [Multi-Region Deployment](#multi-region-deployment)
9. [Monitoring](#monitoring)
10. [Troubleshooting](#troubleshooting)

---

## Prerequisites

### Install flyctl CLI

```bash
# macOS
brew install flyctl

# Linux
curl -L https://fly.io/install.sh | sh

# Windows (PowerShell)
pwsh -Command "iwr https://fly.io/install.ps1 -useb | iex"

# Add to PATH (Linux/macOS)
export FLYCTL_INSTALL="/home/$USER/.fly"
export PATH="$FLYCTL_INSTALL/bin:$PATH"
```

### Authenticate and Setup

```bash
# Sign up or log in
fly auth signup
# or
fly auth login

# Verify authentication
fly auth whoami

# List available organizations
fly orgs list

# Set default organization (optional)
fly orgs set-default your-org-name
```

### Create a New Fly App

```bash
# Create app in a specific region
fly apps create orochi-db --org personal

# Or interactively
fly launch --no-deploy
```

### Verify Prerequisites

```bash
# Check flyctl version (requires 0.1.0+)
fly version

# List available regions
fly platform regions

# Check VM sizes
fly platform vm-sizes
```

---

## Fly Postgres

Fly.io provides managed PostgreSQL clusters. While Fly Postgres uses standard PostgreSQL, you can extend it with Orochi DB using a custom image.

### Option 1: Standard Fly Postgres (Without Orochi Extensions)

For basic deployments without Orochi-specific features:

```bash
# Create a Fly Postgres cluster
fly postgres create \
  --name orochi-postgres \
  --region sjc \
  --initial-cluster-size 3 \
  --vm-size shared-cpu-2x \
  --volume-size 10

# Output includes connection string:
# postgres://postgres:PASSWORD@orochi-postgres.internal:5432
```

### Option 2: High Availability Postgres

```bash
# Create HA cluster with replicas
fly postgres create \
  --name orochi-postgres-ha \
  --region sjc \
  --initial-cluster-size 3 \
  --vm-size performance-2x \
  --volume-size 100

# Add read replicas in other regions
fly postgres replicas create \
  --app orochi-postgres-ha \
  --region ord \
  --vm-size performance-2x \
  --volume-size 100

fly postgres replicas create \
  --app orochi-postgres-ha \
  --region ams \
  --vm-size performance-2x \
  --volume-size 100
```

### Postgres Configuration

```bash
# Connect to the Postgres app
fly postgres connect -a orochi-postgres

# View cluster status
fly postgres status -a orochi-postgres

# Scale the cluster
fly scale vm performance-4x -a orochi-postgres

# Increase volume size
fly volumes extend vol_123456 --size 200 -a orochi-postgres
```

### Attach Postgres to Your App

```bash
# Attach Postgres to your application
fly postgres attach orochi-postgres -a your-app-name

# This sets DATABASE_URL automatically
fly secrets list -a your-app-name
```

---

## Custom App Deployment

For full Orochi DB functionality, deploy a custom PostgreSQL image with the extension pre-installed.

### Dockerfile

Create `Dockerfile` in your project root:

```dockerfile
# Dockerfile for Orochi DB on Fly.io
ARG PG_VERSION=17

FROM postgres:${PG_VERSION}-bookworm AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-${PG_VERSION} \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    librdkafka-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy Orochi DB source
WORKDIR /build
COPY . /build/

# Build the extension
RUN make clean && make && make install

# Runtime image
FROM postgres:${PG_VERSION}-bookworm

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    liblz4-1 \
    libzstd1 \
    libcurl4 \
    libssl3 \
    librdkafka1 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy extension files from builder
COPY --from=builder /usr/share/postgresql/${PG_VERSION}/extension/orochi* \
    /usr/share/postgresql/${PG_VERSION}/extension/
COPY --from=builder /usr/lib/postgresql/${PG_VERSION}/lib/orochi.so \
    /usr/lib/postgresql/${PG_VERSION}/lib/

# Copy custom configuration
COPY deploy/fly/postgresql.conf /etc/postgresql/postgresql.conf
COPY deploy/fly/pg_hba.conf /etc/postgresql/pg_hba.conf
COPY deploy/fly/init-orochi.sh /docker-entrypoint-initdb.d/

# Set configuration
ENV POSTGRES_INITDB_ARGS="--data-checksums"

# Health check
HEALTHCHECK --interval=10s --timeout=5s --start-period=30s --retries=3 \
    CMD pg_isready -U postgres || exit 1

EXPOSE 5432

CMD ["postgres", "-c", "config_file=/etc/postgresql/postgresql.conf"]
```

### PostgreSQL Configuration

Create `deploy/fly/postgresql.conf`:

```ini
# Fly.io optimized PostgreSQL configuration for Orochi DB
# Memory settings (adjust based on VM size)
shared_buffers = '256MB'
effective_cache_size = '768MB'
work_mem = '16MB'
maintenance_work_mem = '128MB'

# Orochi DB extension
shared_preload_libraries = 'orochi'

# Connection settings
listen_addresses = '*'
max_connections = 200
superuser_reserved_connections = 3

# WAL settings
wal_level = replica
max_wal_senders = 10
max_replication_slots = 10
wal_keep_size = '1GB'
hot_standby = on

# Performance
random_page_cost = 1.1
effective_io_concurrency = 200
default_statistics_target = 100

# Logging
log_destination = 'stderr'
logging_collector = off
log_line_prefix = '%t [%p]: [%l-1] '
log_statement = 'ddl'
log_min_duration_statement = 1000

# Orochi-specific GUCs
orochi.default_shard_count = 32
orochi.enable_vectorized_execution = on
orochi.compression_level = 3
orochi.tiering_enabled = on
```

Create `deploy/fly/pg_hba.conf`:

```
# TYPE  DATABASE        USER            ADDRESS                 METHOD
local   all             all                                     trust
host    all             all             127.0.0.1/32            scram-sha-256
host    all             all             ::1/128                 scram-sha-256
host    all             all             fdaa::/16               scram-sha-256
host    all             all             0.0.0.0/0               scram-sha-256
host    replication     all             fdaa::/16               scram-sha-256
```

Create `deploy/fly/init-orochi.sh`:

```bash
#!/bin/bash
set -e

# Create the Orochi extension in the default database
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
    CREATE EXTENSION IF NOT EXISTS orochi;

    -- Grant usage to application user if specified
    DO \$\$
    BEGIN
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'app') THEN
            GRANT USAGE ON SCHEMA orochi TO app;
            GRANT SELECT ON ALL TABLES IN SCHEMA orochi TO app;
        END IF;
    END
    \$\$;
EOSQL

echo "Orochi DB extension initialized successfully"
```

### fly.toml Configuration

Create `fly.toml`:

```toml
# Fly.io configuration for Orochi DB
app = "orochi-db"
primary_region = "sjc"

[build]
  dockerfile = "Dockerfile"

[env]
  POSTGRES_DB = "orochi"
  PGDATA = "/data/postgres"

[mounts]
  source = "orochi_data"
  destination = "/data"

[[services]]
  internal_port = 5432
  protocol = "tcp"

  [[services.ports]]
    port = 5432
    handlers = ["pg_tls"]

  [[services.tcp_checks]]
    interval = "15s"
    timeout = "2s"
    grace_period = "30s"

[checks]
  [checks.pg_isready]
    type = "tcp"
    port = 5432
    interval = "15s"
    timeout = "10s"
    grace_period = "30s"

[[vm]]
  size = "performance-2x"
  memory = "4gb"
  cpus = 2

[metrics]
  port = 9187
  path = "/metrics"
```

### Deploy the Application

```bash
# Create the app
fly apps create orochi-db

# Create a volume for data persistence
fly volumes create orochi_data \
  --region sjc \
  --size 100 \
  --encrypted

# Set required secrets
fly secrets set \
  POSTGRES_USER=postgres \
  POSTGRES_PASSWORD=$(openssl rand -base64 32)

# Deploy
fly deploy

# Check status
fly status

# View logs
fly logs
```

### Machine Sizing Options

| VM Size | CPUs | Memory | Use Case |
|---------|------|--------|----------|
| `shared-cpu-1x` | 1 shared | 256MB | Development |
| `shared-cpu-2x` | 2 shared | 512MB | Small workloads |
| `shared-cpu-4x` | 4 shared | 1GB | Medium workloads |
| `performance-1x` | 1 dedicated | 2GB | Production (small) |
| `performance-2x` | 2 dedicated | 4GB | Production (medium) |
| `performance-4x` | 4 dedicated | 8GB | Production (large) |
| `performance-8x` | 8 dedicated | 16GB | High performance |
| `performance-16x` | 16 dedicated | 32GB | Enterprise |

```bash
# Scale VM size
fly scale vm performance-4x

# Scale memory independently
fly scale memory 8192

# View current scale
fly scale show
```

---

## Fly Machines

For more control, deploy using Fly Machines API directly.

### Create Machine Directly

```bash
# Create a machine with specific configuration
fly machine create \
  --app orochi-db \
  --region sjc \
  --vm-size performance-2x \
  --volume orochi_data:/data \
  --env POSTGRES_USER=postgres \
  --env POSTGRES_DB=orochi \
  --port 5432:5432/tcp \
  --restart always \
  your-registry/orochi-db:latest
```

### Machine Configuration via API

```bash
# Create machine with full configuration
fly machine create \
  --app orochi-db \
  --config '{
    "image": "your-registry/orochi-db:latest",
    "guest": {
      "cpu_kind": "performance",
      "cpus": 4,
      "memory_mb": 8192
    },
    "env": {
      "POSTGRES_USER": "postgres",
      "POSTGRES_DB": "orochi",
      "PGDATA": "/data/postgres"
    },
    "mounts": [{
      "volume": "vol_xxx",
      "path": "/data"
    }],
    "services": [{
      "protocol": "tcp",
      "internal_port": 5432,
      "ports": [{
        "port": 5432,
        "handlers": ["pg_tls"]
      }]
    }],
    "checks": {
      "pg": {
        "type": "tcp",
        "port": 5432,
        "interval": "15s",
        "timeout": "10s"
      }
    },
    "restart": {
      "policy": "always"
    }
  }'
```

### Auto-Scaling Configuration

```bash
# Configure auto-scaling
fly scale count 3 --max-per-region 2

# Set up auto-scaling based on metrics
fly autoscale set \
  --min 2 \
  --max 10 \
  --balance-regions
```

### Machine Lifecycle Management

```bash
# List machines
fly machine list -a orochi-db

# Stop a machine
fly machine stop MACHINE_ID -a orochi-db

# Start a machine
fly machine start MACHINE_ID -a orochi-db

# Restart a machine
fly machine restart MACHINE_ID -a orochi-db

# Update machine configuration
fly machine update MACHINE_ID \
  --vm-size performance-4x \
  --memory 16384 \
  -a orochi-db

# Clone a machine (for scaling)
fly machine clone MACHINE_ID \
  --region ord \
  -a orochi-db

# Destroy a machine
fly machine destroy MACHINE_ID -a orochi-db
```

### Health Checks

```toml
# In fly.toml
[checks]
  [checks.pg_ready]
    type = "tcp"
    port = 5432
    interval = "10s"
    timeout = "5s"
    grace_period = "30s"

  [checks.pg_health]
    type = "http"
    port = 8080
    path = "/health"
    interval = "30s"
    timeout = "10s"
    grace_period = "60s"
    method = "GET"
```

Add a health endpoint to your deployment (optional sidecar):

```bash
# Simple health check script
#!/bin/bash
if pg_isready -U postgres; then
    echo '{"status": "healthy"}'
    exit 0
else
    echo '{"status": "unhealthy"}'
    exit 1
fi
```

---

## Tigris Object Storage

Tigris provides S3-compatible object storage on Fly.io, ideal for Orochi DB's tiered storage.

### Create Tigris Bucket

```bash
# Create a Tigris bucket
fly storage create orochi-tiered-storage

# This outputs:
# AWS_ACCESS_KEY_ID=tid_xxx
# AWS_SECRET_ACCESS_KEY=tsec_xxx
# AWS_ENDPOINT_URL_S3=https://fly.storage.tigris.dev
# BUCKET_NAME=orochi-tiered-storage
```

### Configure Orochi DB for Tigris

Set the secrets for your Orochi DB app:

```bash
# Set Tigris credentials as secrets
fly secrets set \
  OROCHI_S3_ENDPOINT="https://fly.storage.tigris.dev" \
  OROCHI_S3_BUCKET="orochi-tiered-storage" \
  OROCHI_S3_ACCESS_KEY="tid_xxx" \
  OROCHI_S3_SECRET_KEY="tsec_xxx" \
  OROCHI_S3_REGION="auto" \
  -a orochi-db
```

Update `postgresql.conf` to use environment variables:

```ini
# Tiered storage configuration (Tigris)
orochi.s3_endpoint = '${OROCHI_S3_ENDPOINT}'
orochi.s3_bucket = '${OROCHI_S3_BUCKET}'
orochi.s3_access_key = '${OROCHI_S3_ACCESS_KEY}'
orochi.s3_secret_key = '${OROCHI_S3_SECRET_KEY}'
orochi.s3_region = '${OROCHI_S3_REGION}'
orochi.tiering_enabled = on
```

Or configure via SQL after deployment:

```sql
-- Configure Tigris as cold storage backend
ALTER SYSTEM SET orochi.s3_endpoint = 'https://fly.storage.tigris.dev';
ALTER SYSTEM SET orochi.s3_bucket = 'orochi-tiered-storage';
-- Access keys should be set via secrets/env vars, not SQL

SELECT pg_reload_conf();

-- Set up tiering policy
SELECT set_tiering_policy(
    'metrics',
    hot_after => INTERVAL '1 day',
    warm_after => INTERVAL '7 days',
    cold_after => INTERVAL '30 days'  -- Move to Tigris
);
```

### Tigris Multi-Region Replication

Tigris automatically replicates data globally. Configure regions:

```bash
# Enable global distribution
fly storage update orochi-tiered-storage \
  --accelerate \
  --regions "sjc,ord,ams,nrt"
```

### Lifecycle Policies

```bash
# Set lifecycle policy via AWS CLI (Tigris-compatible)
aws s3api put-bucket-lifecycle-configuration \
  --endpoint-url https://fly.storage.tigris.dev \
  --bucket orochi-tiered-storage \
  --lifecycle-configuration '{
    "Rules": [{
      "ID": "archive-old-data",
      "Status": "Enabled",
      "Filter": {"Prefix": "frozen/"},
      "Transitions": [{
        "Days": 90,
        "StorageClass": "GLACIER"
      }],
      "Expiration": {"Days": 365}
    }]
  }'
```

---

## Networking

### Private Networking

Fly apps communicate over private IPv6 network (fdaa::/16):

```bash
# Apps can connect internally using:
# <app-name>.internal:5432

# Example connection from another Fly app:
psql postgres://user:pass@orochi-db.internal:5432/orochi
```

### Flycast (Internal Load Balancing)

Enable Flycast for internal TCP load balancing:

```bash
# Allocate a Flycast address
fly ips allocate-v6 --private -a orochi-db

# This creates an address like:
# fdaa:0:xxxx::1

# Other apps connect via:
# orochi-db.flycast:5432
```

Configure in `fly.toml`:

```toml
[[services]]
  internal_port = 5432
  protocol = "tcp"
  auto_stop_machines = false
  auto_start_machines = true

  [[services.ports]]
    port = 5432
    handlers = ["pg_tls"]

  [services.concurrency]
    type = "connections"
    hard_limit = 100
    soft_limit = 80
```

### Public Endpoints

For external access (not recommended for production databases):

```bash
# Allocate public IP
fly ips allocate-v4 -a orochi-db
fly ips allocate-v6 -a orochi-db

# List IPs
fly ips list -a orochi-db
```

Configure TLS for public access:

```toml
[[services]]
  internal_port = 5432
  protocol = "tcp"

  [[services.ports]]
    port = 5432
    handlers = ["pg_tls"]  # Automatic TLS termination

  # Or use proxy protocol for client IP preservation
  [[services.ports]]
    port = 5433
    handlers = ["proxy_proto", "pg_tls"]
```

### Connection Proxying with PgBouncer

For connection pooling, add PgBouncer as a sidecar:

```dockerfile
# Dockerfile.pgbouncer
FROM edoburu/pgbouncer:1.21.0

COPY pgbouncer.ini /etc/pgbouncer/pgbouncer.ini
COPY userlist.txt /etc/pgbouncer/userlist.txt

EXPOSE 6432

CMD ["pgbouncer", "/etc/pgbouncer/pgbouncer.ini"]
```

```ini
# pgbouncer.ini
[databases]
orochi = host=orochi-db.internal port=5432 dbname=orochi

[pgbouncer]
listen_addr = *
listen_port = 6432
auth_type = scram-sha-256
auth_file = /etc/pgbouncer/userlist.txt
pool_mode = transaction
max_client_conn = 1000
default_pool_size = 50
min_pool_size = 10
reserve_pool_size = 10
reserve_pool_timeout = 5
server_lifetime = 3600
server_idle_timeout = 600
```

---

## Secrets Management

### Setting Secrets

```bash
# Set individual secrets
fly secrets set POSTGRES_PASSWORD="secure-password-here" -a orochi-db

# Set multiple secrets at once
fly secrets set \
  POSTGRES_USER=postgres \
  POSTGRES_PASSWORD=$(openssl rand -base64 32) \
  POSTGRES_DB=orochi \
  REPLICATION_PASSWORD=$(openssl rand -base64 32) \
  -a orochi-db

# Set secrets from file
cat << 'EOF' > .env.production
POSTGRES_USER=postgres
POSTGRES_PASSWORD=xxx
POSTGRES_DB=orochi
EOF
fly secrets import < .env.production -a orochi-db
rm .env.production
```

### Managing Secrets

```bash
# List secrets (names only, not values)
fly secrets list -a orochi-db

# Unset a secret
fly secrets unset OLD_SECRET -a orochi-db

# Secrets are available as environment variables
# Access in postgresql.conf or scripts:
# ${POSTGRES_PASSWORD}
```

### Secret Rotation

```bash
# Generate new password
NEW_PASSWORD=$(openssl rand -base64 32)

# Update PostgreSQL password first (while app still has old secret)
fly ssh console -a orochi-db -C "psql -U postgres -c \"ALTER USER postgres PASSWORD '$NEW_PASSWORD'\""

# Then update the secret (triggers restart)
fly secrets set POSTGRES_PASSWORD="$NEW_PASSWORD" -a orochi-db

# For zero-downtime rotation, use staged deployments
fly deploy --strategy rolling
```

### Environment Configuration

```toml
# fly.toml - Non-sensitive configuration
[env]
  PGDATA = "/data/postgres"
  POSTGRES_DB = "orochi"
  TZ = "UTC"

  # Orochi-specific settings
  OROCHI_LOG_LEVEL = "info"
  OROCHI_VECTORIZED_EXECUTION = "on"
```

---

## Multi-Region Deployment

### Primary Region Selection

```bash
# Set primary region for writes
fly regions set sjc -a orochi-db

# View regions
fly regions list -a orochi-db
```

### Read Replicas

Create a multi-region deployment with read replicas:

```bash
# Primary in San Jose
fly deploy --region sjc

# Create read replicas
fly machine clone MACHINE_ID --region ord -a orochi-db
fly machine clone MACHINE_ID --region ams -a orochi-db
fly machine clone MACHINE_ID --region nrt -a orochi-db
```

### Streaming Replication Configuration

Update primary `postgresql.conf`:

```ini
# Primary configuration
wal_level = replica
max_wal_senders = 10
max_replication_slots = 10
wal_keep_size = '2GB'
synchronous_commit = on
```

Create replication slots:

```sql
-- On primary
SELECT pg_create_physical_replication_slot('replica_ord');
SELECT pg_create_physical_replication_slot('replica_ams');
SELECT pg_create_physical_replication_slot('replica_nrt');
```

Configure replicas with `recovery.conf` or `postgresql.auto.conf`:

```ini
# Replica configuration
primary_conninfo = 'host=orochi-db.internal port=5432 user=replicator password=${REPLICATION_PASSWORD}'
primary_slot_name = 'replica_ord'
hot_standby = on
hot_standby_feedback = on
```

### fly.toml for Multi-Region

```toml
app = "orochi-db"
primary_region = "sjc"

[env]
  PRIMARY_REGION = "sjc"
  FLY_REGION = "${FLY_REGION}"

[processes]
  primary = "postgres -c config_file=/etc/postgresql/postgresql.conf"
  replica = "postgres -c config_file=/etc/postgresql/postgresql-replica.conf"

[[services]]
  internal_port = 5432
  protocol = "tcp"
  processes = ["primary", "replica"]

  [[services.ports]]
    port = 5432
    handlers = ["pg_tls"]

  # Write traffic goes to primary
  [[services.http_checks]]
    interval = "15s"
    timeout = "5s"
    method = "GET"
    path = "/primary"

# Regional machine placement
[mounts]
  source = "orochi_data"
  destination = "/data"

[[vm]]
  size = "performance-4x"
  processes = ["primary"]

[[vm]]
  size = "performance-2x"
  processes = ["replica"]
```

### Latency-Based Routing

```bash
# Enable automatic routing to nearest replica
fly proxy 5432:5432 -a orochi-db

# Applications use the nearest machine automatically
# Writes are routed to primary via fly-replay header
```

Handle write routing in your application:

```python
import os
import psycopg2

def get_connection(write=False):
    """Get database connection, routing writes to primary."""
    host = "orochi-db.internal"

    # For writes, connect to primary region
    if write:
        host = f"{os.environ['PRIMARY_REGION']}.orochi-db.internal"

    return psycopg2.connect(
        host=host,
        port=5432,
        user=os.environ['POSTGRES_USER'],
        password=os.environ['POSTGRES_PASSWORD'],
        dbname=os.environ['POSTGRES_DB']
    )
```

### Regional Database Pattern

For true multi-region with separate databases:

```bash
# Deploy to multiple regions with separate volumes
for region in sjc ord ams; do
    fly volumes create orochi_data --region $region --size 100 -a orochi-db
    fly machine create \
        --region $region \
        --volume orochi_data:/data \
        -a orochi-db
done
```

---

## Monitoring

### Fly Metrics

Built-in metrics are available automatically:

```bash
# View metrics in dashboard
fly dashboard -a orochi-db

# Or via CLI
fly status -a orochi-db
fly vm status -a orochi-db
```

### PostgreSQL Exporter for Prometheus

Add `postgres_exporter` as a sidecar process:

```dockerfile
# Multi-stage build including exporter
FROM prometheuscommunity/postgres-exporter:v0.15.0 AS exporter

FROM your-orochi-image
COPY --from=exporter /postgres_exporter /usr/local/bin/
```

Configure in `fly.toml`:

```toml
[processes]
  app = "postgres -c config_file=/etc/postgresql/postgresql.conf"
  metrics = "/usr/local/bin/postgres_exporter"

[env]
  DATA_SOURCE_NAME = "postgresql://postgres:${POSTGRES_PASSWORD}@localhost:5432/orochi?sslmode=disable"

[metrics]
  port = 9187
  path = "/metrics"
```

### Grafana Integration

Deploy Grafana on Fly.io:

```bash
# Create Grafana app
fly apps create orochi-grafana

# Deploy with Prometheus data source configured
fly deploy -a orochi-grafana
```

Example Grafana dashboard queries for Orochi DB:

```promql
# Connection count
pg_stat_activity_count{datname="orochi"}

# Transactions per second
rate(pg_stat_database_xact_commit{datname="orochi"}[5m])

# Cache hit ratio
pg_stat_database_blks_hit{datname="orochi"} /
(pg_stat_database_blks_hit{datname="orochi"} + pg_stat_database_blks_read{datname="orochi"})

# Orochi-specific: Shard distribution
orochi_shard_row_count{table="orders"}

# Orochi-specific: Tiered storage usage
orochi_tier_bytes{tier="cold"}
```

### Custom Metrics

Add custom Orochi metrics:

```sql
-- Create metrics view
CREATE VIEW orochi_metrics AS
SELECT
    'orochi_distributed_tables' AS metric,
    COUNT(*)::float AS value
FROM orochi.tables
WHERE is_distributed = true
UNION ALL
SELECT
    'orochi_total_shards' AS metric,
    COUNT(*)::float AS value
FROM orochi.shards
UNION ALL
SELECT
    'orochi_cold_storage_bytes' AS metric,
    COALESCE(SUM(size_bytes), 0)::float AS value
FROM orochi.chunks
WHERE tier = 'cold';
```

### Log Management

```bash
# Stream logs
fly logs -a orochi-db

# Filter by instance
fly logs -a orochi-db --instance INSTANCE_ID

# Historical logs
fly logs -a orochi-db --since 1h
```

Configure PostgreSQL logging:

```ini
# postgresql.conf
log_destination = 'stderr'
logging_collector = off  # Let Fly capture stderr
log_line_prefix = '%t [%p]: [%l-1] user=%u,db=%d,app=%a,client=%h '
log_statement = 'ddl'
log_min_duration_statement = 500  # Log slow queries
log_checkpoints = on
log_connections = on
log_disconnections = on
log_lock_waits = on
```

### Alerting

Set up alerts using Fly.io's built-in monitoring:

```bash
# CPU alert
fly checks create \
  --type metric \
  --metric cpu \
  --threshold 80 \
  --alert-email your@email.com \
  -a orochi-db

# Memory alert
fly checks create \
  --type metric \
  --metric memory \
  --threshold 90 \
  --alert-email your@email.com \
  -a orochi-db
```

---

## Troubleshooting

### Common Issues

**Connection refused:**
```bash
# Check if machine is running
fly status -a orochi-db

# Check machine health
fly machine status MACHINE_ID -a orochi-db

# SSH into machine to debug
fly ssh console -a orochi-db
```

**Out of disk space:**
```bash
# Check volume usage
fly volumes list -a orochi-db

# Extend volume
fly volumes extend vol_xxx --size 200 -a orochi-db

# Clean up WAL files (emergency)
fly ssh console -a orochi-db -C "pg_archivecleanup /data/postgres/pg_wal 000000010000000000000010"
```

**Replication lag:**
```sql
-- On primary
SELECT
    client_addr,
    state,
    sent_lsn,
    write_lsn,
    flush_lsn,
    replay_lsn,
    pg_wal_lsn_diff(sent_lsn, replay_lsn) AS lag_bytes
FROM pg_stat_replication;
```

**Extension not loading:**
```bash
# Check shared_preload_libraries
fly ssh console -a orochi-db -C "psql -c \"SHOW shared_preload_libraries\""

# Check extension files exist
fly ssh console -a orochi-db -C "ls -la /usr/share/postgresql/17/extension/orochi*"
```

### Useful Commands

```bash
# SSH into container
fly ssh console -a orochi-db

# Run one-off command
fly ssh console -a orochi-db -C "psql -U postgres -c 'SELECT version()'"

# Copy files to/from machine
fly ssh sftp get /data/postgres/postgresql.conf ./postgresql.conf -a orochi-db
fly ssh sftp put ./new-config.conf /tmp/new-config.conf -a orochi-db

# Port forward for local access
fly proxy 15432:5432 -a orochi-db
# Then: psql postgres://postgres:password@localhost:15432/orochi

# Force restart
fly machine restart MACHINE_ID --force -a orochi-db

# View machine events
fly machine status MACHINE_ID -a orochi-db --json | jq '.events'
```

### Performance Debugging

```sql
-- Check active queries
SELECT pid, now() - pg_stat_activity.query_start AS duration, query
FROM pg_stat_activity
WHERE state = 'active'
ORDER BY duration DESC;

-- Check locks
SELECT
    locktype, relation::regclass, mode, granted, pid
FROM pg_locks
WHERE NOT granted;

-- Orochi shard statistics
SELECT * FROM orochi.shard_stats ORDER BY row_count DESC LIMIT 10;

-- Check chunk compression status
SELECT
    hypertable,
    COUNT(*) AS total_chunks,
    COUNT(*) FILTER (WHERE is_compressed) AS compressed_chunks,
    pg_size_pretty(SUM(size_bytes)) AS total_size
FROM orochi.chunks
GROUP BY hypertable;
```

---

## Quick Reference

### Essential Commands

```bash
# Deployment
fly deploy                          # Deploy application
fly deploy --strategy rolling       # Zero-downtime deploy
fly deploy --remote-only           # Build on Fly.io builders

# Scaling
fly scale vm performance-4x        # Change VM size
fly scale count 3                  # Run 3 instances
fly scale memory 8192              # Set memory (MB)

# Volumes
fly volumes create orochi_data --size 100 --region sjc
fly volumes extend vol_xxx --size 200
fly volumes list

# Secrets
fly secrets set KEY=value
fly secrets list
fly secrets unset KEY

# Networking
fly ips list
fly ips allocate-v4
fly ips allocate-v6 --private

# Storage
fly storage create bucket-name
fly storage list

# Monitoring
fly status
fly logs
fly dashboard

# Debugging
fly ssh console
fly proxy 5432:5432
fly machine list
fly machine status MACHINE_ID
```

### Connection Strings

```bash
# Internal (from other Fly apps)
postgres://user:pass@orochi-db.internal:5432/orochi

# Flycast (load-balanced internal)
postgres://user:pass@orochi-db.flycast:5432/orochi

# Regional (specific region)
postgres://user:pass@sjc.orochi-db.internal:5432/orochi

# External (with public IP)
postgres://user:pass@orochi-db.fly.dev:5432/orochi

# Local via proxy
fly proxy 5432:5432 -a orochi-db
postgres://user:pass@localhost:5432/orochi
```

---

## Additional Resources

- [Fly.io Documentation](https://fly.io/docs/)
- [Fly Postgres Guide](https://fly.io/docs/postgres/)
- [Tigris Documentation](https://www.tigrisdata.com/docs/)
- [Fly Machines API](https://fly.io/docs/machines/api/)
- [Orochi DB User Guide](../user-guide.md)
- [Orochi DB Architecture](../architecture.md)
