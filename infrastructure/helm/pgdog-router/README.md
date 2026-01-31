# PgDog Router Helm Chart

A Helm chart for deploying [PgDog](https://github.com/pgdog/pgdog) - a high-performance PostgreSQL connection pooler and query router written in Rust.

## Overview

PgDog provides:
- Connection pooling (session, transaction, and statement modes)
- Load balancing across PostgreSQL replicas
- Query routing with read/write splitting
- Health checking and automatic failover
- Prometheus metrics
- JWT authentication proxy (optional)

## Prerequisites

- Kubernetes 1.21+
- Helm 3.8+
- PostgreSQL cluster to connect to

## Installation

### Add the Repository

```bash
helm repo add orochi https://charts.orochi.cloud
helm repo update
```

### Install the Chart

```bash
# Basic installation
helm install pgdog orochi/pgdog-router

# Install with custom values
helm install pgdog orochi/pgdog-router -f values.yaml

# Install in a specific namespace
helm install pgdog orochi/pgdog-router -n database --create-namespace

# Install with production values
helm install pgdog orochi/pgdog-router -f values-production.yaml
```

### Upgrade

```bash
helm upgrade pgdog orochi/pgdog-router -f values.yaml
```

### Uninstall

```bash
helm uninstall pgdog
```

## Configuration

### Basic Configuration

```yaml
# values.yaml
replicaCount: 3

pgdog:
  pool:
    mode: "transaction"
    defaultSize: 20
    maxSize: 100

  upstreams:
    - name: primary
      host: postgres-primary.database.svc.cluster.local
      port: 5432
      role: primary
    - name: replica-1
      host: postgres-replica-1.database.svc.cluster.local
      port: 5432
      role: replica

secrets:
  credentials:
    app_user: "secure-password"
```

### Full Configuration Reference

| Parameter | Description | Default |
|-----------|-------------|---------|
| `replicaCount` | Number of PgDog replicas | `2` |
| `image.repository` | PgDog image repository | `ghcr.io/pgdog/pgdog` |
| `image.tag` | PgDog image tag | `""` (uses appVersion) |
| `image.pullPolicy` | Image pull policy | `IfNotPresent` |

#### Service Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `service.type` | Kubernetes service type | `ClusterIP` |
| `service.port` | PostgreSQL proxy port | `5432` |
| `service.adminPort` | Admin/metrics port | `9898` |
| `service.annotations` | Service annotations | `{}` |

#### PgDog Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.general.host` | Bind address | `0.0.0.0` |
| `pgdog.general.port` | Listen port | `5432` |
| `pgdog.general.workers` | Worker threads (0=auto) | `0` |
| `pgdog.general.connectTimeout` | Connect timeout (ms) | `5000` |
| `pgdog.general.queryTimeout` | Query timeout (ms) | `0` |

#### Pool Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.pool.mode` | Pool mode (session/transaction/statement) | `transaction` |
| `pgdog.pool.defaultSize` | Default pool size | `10` |
| `pgdog.pool.minSize` | Minimum pool size | `2` |
| `pgdog.pool.maxSize` | Maximum pool size | `100` |
| `pgdog.pool.maxClientConnections` | Max client connections | `10000` |
| `pgdog.pool.connectionLifetime` | Connection lifetime (s) | `3600` |
| `pgdog.pool.queueTimeout` | Queue timeout (ms) | `10000` |

#### Load Balancing

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.loadBalancing.enabled` | Enable load balancing | `true` |
| `pgdog.loadBalancing.strategy` | Strategy (round_robin/random/least_connections) | `round_robin` |
| `pgdog.loadBalancing.healthCheckInterval` | Health check interval (ms) | `5000` |

#### Query Routing

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.routing.enabled` | Enable query routing | `true` |
| `pgdog.routing.readWriteSplit` | Enable read/write splitting | `true` |
| `pgdog.routing.readFromReplica` | Route reads to replicas | `true` |
| `pgdog.routing.stickyTransactions` | Sticky sessions for transactions | `true` |

#### TLS Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.tls.enabled` | Enable TLS | `false` |
| `pgdog.tls.mode` | TLS mode | `prefer` |
| `pgdog.tls.existingSecret` | Existing TLS secret name | `""` |

#### JWT Proxy (Optional)

| Parameter | Description | Default |
|-----------|-------------|---------|
| `jwtProxy.enabled` | Enable JWT proxy sidecar | `false` |
| `jwtProxy.port` | JWT proxy port | `5433` |
| `jwtProxy.algorithm` | JWT algorithm | `HS256` |
| `jwtProxy.existingSecret` | Existing JWT secret | `""` |

#### Autoscaling

| Parameter | Description | Default |
|-----------|-------------|---------|
| `autoscaling.enabled` | Enable HPA | `false` |
| `autoscaling.minReplicas` | Minimum replicas | `2` |
| `autoscaling.maxReplicas` | Maximum replicas | `10` |
| `autoscaling.targetCPUUtilizationPercentage` | Target CPU utilization | `70` |

#### Monitoring

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pgdog.metrics.enabled` | Enable Prometheus metrics | `true` |
| `pgdog.metrics.path` | Metrics endpoint path | `/metrics` |
| `serviceMonitor.enabled` | Enable ServiceMonitor | `false` |
| `serviceMonitor.interval` | Scrape interval | `30s` |

## Examples

### Basic Setup with CloudNativePG

```yaml
pgdog:
  upstreams:
    - name: primary
      host: my-cluster-rw.database.svc.cluster.local
      port: 5432
      role: primary
    - name: replica
      host: my-cluster-ro.database.svc.cluster.local
      port: 5432
      role: replica

  routing:
    enabled: true
    readWriteSplit: true

secrets:
  credentials:
    postgres: "your-password"
```

### High Availability Production Setup

```yaml
replicaCount: 5

autoscaling:
  enabled: true
  minReplicas: 3
  maxReplicas: 20

podDisruptionBudget:
  enabled: true
  minAvailable: 2

affinity:
  podAntiAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      - labelSelector:
          matchLabels:
            app.kubernetes.io/name: pgdog-router
        topologyKey: kubernetes.io/hostname

pgdog:
  pool:
    mode: transaction
    defaultSize: 50
    maxSize: 500
    maxClientConnections: 100000

  tls:
    enabled: true
    mode: require
    existingSecret: pgdog-tls-cert

serviceMonitor:
  enabled: true
  interval: 15s
```

### With JWT Authentication

```yaml
jwtProxy:
  enabled: true
  port: 5433
  algorithm: RS256
  existingSecret: jwt-signing-key
  existingSecretKey: private-key
  issuer: https://auth.example.com
  audience: pgdog

service:
  port: 5432      # Direct PostgreSQL access
  # JWT proxy available on port 5433
```

## Monitoring

### Prometheus Metrics

PgDog exposes Prometheus metrics at `/metrics` on the admin port (default 9898).

Key metrics:
- `pgdog_active_connections` - Active client connections
- `pgdog_pool_size` - Current pool size per database
- `pgdog_queries_total` - Total queries processed
- `pgdog_query_duration_seconds` - Query duration histogram
- `pgdog_upstream_health` - Upstream server health status

### Grafana Dashboard

Import the PgDog dashboard from the Grafana dashboard repository or use the JSON file in `dashboards/pgdog.json`.

## Troubleshooting

### Connection Issues

```bash
# Check pod status
kubectl get pods -l app.kubernetes.io/name=pgdog-router

# View logs
kubectl logs -l app.kubernetes.io/name=pgdog-router -f

# Check configuration
kubectl get configmap pgdog-router -o yaml
```

### Performance Issues

1. Check pool utilization in metrics
2. Verify upstream server health
3. Review connection limits
4. Check for slow queries in logs

### Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| Connection refused | PgDog not ready | Wait for startup probe to pass |
| Authentication failed | Wrong credentials | Verify secrets configuration |
| Pool exhausted | Too many connections | Increase pool size or replicas |
| Upstream unavailable | Database down | Check upstream server health |

## Upgrading

### From 0.0.x to 0.1.x

No breaking changes. Standard upgrade process:

```bash
helm upgrade pgdog orochi/pgdog-router
```

## License

This Helm chart is licensed under the MIT License.

PgDog itself is licensed under the MIT License. See the [PgDog repository](https://github.com/pgdog/pgdog) for details.
