# PgDog Router Service

PostgreSQL connection pooler and query router for Orochi DB, built on [PgDog](https://pgdog.dev).

## Overview

The PgDog Router provides high-performance connection pooling, read/write splitting, and distributed query routing for Orochi DB clusters. It sits between client applications and PostgreSQL, handling:

- **Connection Pooling**: Transaction-mode pooling for efficient connection reuse
- **Read/Write Splitting**: Automatic routing of reads to replicas
- **Distributed Sharding**: Query routing to appropriate shards based on shard keys
- **Health Checks**: Automatic backend health monitoring and failover
- **Prometheus Metrics**: Comprehensive observability

## Architecture

```
                    +------------------+
                    |     Clients      |
                    +--------+---------+
                             |
                    +--------v---------+
                    |   JWT Gateway    |  (Envoy - optional)
                    |   Port: 5433     |
                    +--------+---------+
                             |
                    +--------v---------+
                    |   PgDog Router   |
                    |   Port: 5432     |
                    |   Admin: 6432    |
                    |   Metrics: 9090  |
                    +--------+---------+
                             |
          +------------------+------------------+
          |                  |                  |
+---------v------+  +--------v-------+  +-------v--------+
|    Primary     |  |    Replica 1   |  |    Replica 2   |
| (Read/Write)   |  |  (Read Only)   |  |  (Read Only)   |
+----------------+  +----------------+  +----------------+
```

## Quick Start

### Local Development

```bash
# Build the development image
make docker-build-dev

# Run with default configuration
make run

# Or with custom config
docker run -p 5432:5432 -p 6432:6432 -p 9090:9090 \
    -v $(pwd)/config:/etc/pgdog \
    pgdog-router:dev
```

### Production Build

```bash
# Build production image
make docker-build

# Push to registry
make docker-push

# Deploy to Kubernetes
make deploy
```

## Configuration

### Configuration Files

| File | Description |
|------|-------------|
| `config/pgdog.toml.tmpl` | Main PgDog configuration template |
| `config/users.toml.tmpl` | User authentication configuration |

### Template Variables

The configuration templates support Go template syntax with these variables:

| Variable | Description | Default |
|----------|-------------|---------|
| `{{.ClusterName}}` | Orochi DB cluster name | `default` |
| `{{.Namespace}}` | Kubernetes namespace | `orochi-cloud` |
| `{{.DatabaseName}}` | Default database name | `postgres` |
| `{{.ShardCount}}` | Number of shards | `32` |
| `{{.PoolMode}}` | Connection pool mode | `transaction` |
| `{{.MaxPoolSize}}` | Max connections per pool | `100` |
| `{{.TLSEnabled}}` | Enable TLS | `false` |

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `PGDOG_CONFIG` | Path to main config | `/etc/pgdog/pgdog.toml` |
| `PGDOG_USERS_CONFIG` | Path to users config | `/etc/pgdog/users.toml` |
| `PGDOG_HOST` | Listen address | `0.0.0.0` |
| `PGDOG_PORT` | PostgreSQL port | `5432` |
| `PGDOG_ADMIN_PORT` | Admin interface port | `6432` |
| `PGDOG_METRICS_PORT` | Prometheus metrics port | `9090` |
| `PGDOG_LOG_LEVEL` | Log level | `info` |
| `TLS_ENABLED` | Enable TLS | `false` |
| `CLUSTER_NAME` | Cluster name for config gen | `default` |
| `NAMESPACE` | Kubernetes namespace | `orochi-cloud` |

### Generate Configuration

```bash
# Generate config from templates
make config-gen

# With custom values
./build/bin/pgdog-config-gen \
    -template config/pgdog.toml.tmpl \
    -output config/pgdog.toml \
    -cluster-name my-cluster \
    -namespace production \
    -shard-count 64
```

## Connection Pooling

### Pool Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| `transaction` | Connection released after each transaction | Web applications (recommended) |
| `session` | Connection held for entire session | Long-running connections |
| `statement` | Connection released after each statement | Simple queries |

### Configuration Example

```toml
[pools.default]
pool_mode = "transaction"
min_pool_size = 2
max_pool_size = 100
server_idle_timeout = 600000
```

## Read/Write Splitting

PgDog automatically routes queries based on their type:

- **Writes** (INSERT, UPDATE, DELETE): Routed to primary
- **Reads** (SELECT, SHOW): Routed to replicas
- **Transactions**: All queries within a transaction go to primary

```toml
[query_routing]
read_write_split = true
sticky_transactions = true
max_replica_lag = 1000  # milliseconds
```

## Sharding

For distributed Orochi DB tables, PgDog routes queries to the appropriate shard:

```toml
[sharding]
enabled = true
default_shard_count = 32
sharding_function = "postgres_hash"  # Compatible with orochi_hash()

[[sharding.tables]]
name = "users"
column = "tenant_id"
shards = 32
```

### Two-Phase Commit (2PC)

Cross-shard transactions use 2PC for consistency:

```toml
[sharding.two_phase_commit]
enabled = true
timeout_seconds = 30
cleanup_interval_seconds = 60
```

## Health Checks

### Kubernetes Probes

```yaml
livenessProbe:
  exec:
    command: ["/health-check.sh"]
  initialDelaySeconds: 10
  periodSeconds: 30

readinessProbe:
  tcpSocket:
    port: 5432
  initialDelaySeconds: 5
  periodSeconds: 10
```

### Manual Health Check

```bash
# Check all endpoints
./scripts/health-check.sh -v

# Check PostgreSQL connection
pg_isready -h localhost -p 5432

# Check metrics
curl http://localhost:9090/metrics
```

## Metrics

PgDog exposes Prometheus metrics at `:9090/metrics`:

| Metric | Type | Description |
|--------|------|-------------|
| `orochi_pgdog_connections_active` | Gauge | Active client connections |
| `orochi_pgdog_connections_idle` | Gauge | Idle pooled connections |
| `orochi_pgdog_queries_total` | Counter | Total queries processed |
| `orochi_pgdog_query_duration_seconds` | Histogram | Query latency distribution |
| `orochi_pgdog_pool_size` | Gauge | Current pool size |
| `orochi_pgdog_pool_available` | Gauge | Available connections |
| `orochi_pgdog_2pc_commits_total` | Counter | 2PC commits |
| `orochi_pgdog_2pc_rollbacks_total` | Counter | 2PC rollbacks |

## Admin Interface

Connect to the admin interface for pool management:

```bash
psql -h localhost -p 6432 -U pgdog_admin

# Show pool status
SHOW POOLS;

# Show active clients
SHOW CLIENTS;

# Show backend servers
SHOW SERVERS;

# Reload configuration
RELOAD;
```

## TLS Configuration

### Enable TLS

```bash
# Set environment variables
export TLS_ENABLED=true
export TLS_CERT_PATH=/etc/pgdog/certs/tls.crt
export TLS_KEY_PATH=/etc/pgdog/certs/tls.key
export TLS_CA_PATH=/etc/pgdog/certs/ca.crt

# Or mount certificates
docker run -v /path/to/certs:/etc/pgdog/certs pgdog-router
```

### Certificate Requirements

- Certificate: PEM format, includes full chain
- Private key: PEM format, permissions 600
- CA certificate: For client verification (optional)

## Kubernetes Deployment

### ConfigMap

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: pgdog-config
data:
  pgdog.toml: |
    [general]
    host = "0.0.0.0"
    port = 5432
    # ... configuration
```

### Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: pgdog-router
spec:
  replicas: 3
  template:
    spec:
      containers:
        - name: pgdog
          image: registry.digitalocean.com/orochi-registry/pgdog-router:latest
          ports:
            - containerPort: 5432
            - containerPort: 6432
            - containerPort: 9090
          volumeMounts:
            - name: config
              mountPath: /etc/pgdog
      volumes:
        - name: config
          configMap:
            name: pgdog-config
```

See `infrastructure/digitalocean/pgdog-router.yaml` for the full manifest.

## Development

### Build

```bash
# Build config generator
make build

# Run tests
make test

# Lint
make lint

# Format code
make format
```

### Development Mode

```bash
# Start with hot reload
docker run -it \
    -p 5432:5432 \
    -v $(pwd)/config:/etc/pgdog \
    pgdog-router:dev watch
```

### Debug Mode

```bash
# Enable debug logging
docker run -e PGDOG_LOG_LEVEL=debug pgdog-router:dev
```

## Troubleshooting

### Common Issues

**Connection Refused**
```bash
# Check if PgDog is running
docker logs pgdog-router

# Verify port binding
netstat -tlnp | grep 5432
```

**Authentication Failed**
```bash
# Check users.toml configuration
cat /etc/pgdog/users.toml

# Verify backend credentials
psql -h <backend-host> -U postgres -c "SELECT 1"
```

**Pool Exhausted**
```bash
# Check pool statistics
psql -h localhost -p 6432 -U pgdog_admin -c "SHOW POOLS"

# Increase pool size
# Edit pgdog.toml: max_pool_size = 200
```

**High Latency**
```bash
# Check metrics
curl -s http://localhost:9090/metrics | grep query_duration

# Enable query logging
# Edit pgdog.toml: log_queries = true
```

### Logs

```bash
# Docker logs
docker logs -f pgdog-router

# Kubernetes logs
kubectl logs -f deployment/pgdog-router -n orochi-cloud
```

## Related Documentation

- [PgDog Documentation](https://pgdog.dev/docs)
- [Orochi DB Implementation Strategy](../../docs/PGDOG_IMPLEMENTATION_STRATEGY.md)
- [PgDog Testing Strategy](../../docs/testing/PGDOG_TESTING_STRATEGY.md)
- [Kubernetes Manifests](../../infrastructure/digitalocean/pgdog-router.yaml)

## License

PgDog is licensed under the [AGPL-3.0 License](https://github.com/pgdogdev/pgdog/blob/main/LICENSE).
