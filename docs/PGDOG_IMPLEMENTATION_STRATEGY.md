# PgDog Integration: Implementation & Testing Strategy

**Version**: 1.0.0
**Status**: Draft
**Created**: 2026-01-31
**Timeline**: 14 weeks (3.5 months)

---

## Executive Summary

This document outlines the comprehensive strategy for integrating PgDog as the connection pooler/router for Orochi DB, replacing the previously planned PgCat integration. PgDog offers superior sharding, cross-shard query support, and 2PC capabilities that align with Orochi's distributed architecture.

### Critical Discovery

**PgDog does NOT support custom authentication plugins.** Authentication is configuration-based only. This requires an architectural adjustment using one of these approaches:

1. **External JWT Gateway** (Recommended) - Nginx/Envoy validates JWT before PgDog
2. **Passthrough Auth + PostgreSQL RLS** - Use PgDog passthrough with Orochi's RLS policies
3. **Hybrid Approach** - JWT gateway for external traffic, passthrough for internal

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Phase 1: Foundation](#2-phase-1-foundation-weeks-1-3)
3. [Phase 2: Core Features](#3-phase-2-core-features-weeks-4-7)
4. [Phase 3: Advanced Features](#4-phase-3-advanced-features-weeks-8-11)
5. [Phase 4: Production Readiness](#5-phase-4-production-readiness-weeks-12-14)
6. [Testing Strategy](#6-testing-strategy)
7. [Completion Checklist](#7-completion-checklist)

---

## 1. Architecture Overview

### Target Architecture

```
                                    ┌─────────────────────────────────┐
                                    │         Dashboard (React)       │
                                    └───────────────┬─────────────────┘
                                                    │ HTTPS
                                                    ▼
┌─────────────┐    HTTPS    ┌─────────────────────────────────────────┐
│   CLI       │────────────▶│           Control Plane API             │
└─────────────┘             │         (Go - Chi Router)               │
                            └───────────────┬─────────────────────────┘
                                            │ gRPC
                            ┌───────────────┴─────────────────────────┐
                            │                                         │
                            ▼                                         ▼
               ┌────────────────────────┐            ┌────────────────────────┐
               │      Provisioner       │            │       Autoscaler       │
               │   (CloudNativePG)      │            │    (Metrics-based)     │
               └───────────┬────────────┘            └────────────────────────┘
                           │ K8s API
                           ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                              Kubernetes Cluster                               │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                         Per-Cluster Namespace                          │  │
│  │                                                                        │  │
│  │  ┌──────────────┐     ┌──────────────┐     ┌──────────────────────┐   │  │
│  │  │  JWT Gateway │────▶│    PgDog     │────▶│  PostgreSQL + Orochi │   │  │
│  │  │  (Envoy)     │     │   Router     │     │  (CloudNativePG)     │   │  │
│  │  └──────────────┘     └──────────────┘     └──────────────────────┘   │  │
│  │         │                    │                       │                │  │
│  │         │                    │                       ▼                │  │
│  │         │                    │              ┌──────────────────┐      │  │
│  │         │                    └─────────────▶│    Replicas      │      │  │
│  │         │                                   └──────────────────┘      │  │
│  │         ▼                                                             │  │
│  │  ┌──────────────────────────────────────────────────────────────┐    │  │
│  │  │                    Prometheus Metrics                         │    │  │
│  │  └──────────────────────────────────────────────────────────────┘    │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Responsibility |
|-----------|----------------|
| **JWT Gateway (Envoy)** | JWT validation, token extraction, header injection |
| **PgDog Router** | Connection pooling, sharding, load balancing, 2PC |
| **PostgreSQL + Orochi** | Data storage, columnar, time-series, tiered storage |
| **Control Plane** | Cluster lifecycle, configuration management |
| **Provisioner** | Kubernetes resource management, CloudNativePG |

---

## 2. Phase 1: Foundation (Weeks 1-3)

**Goal**: Establish basic PgDog service infrastructure with minimal viable configuration

### 2.1 Service Directory Structure

**Complexity**: S (Small)

```
services/pgdog-router/
├── cmd/
│   └── pgdog-config-gen/      # Config generator tool (Go)
│       └── main.go
├── Dockerfile
├── Dockerfile.dev
├── Makefile
├── config/
│   ├── pgdog.toml.tmpl        # Template for cluster configs
│   └── users.toml.tmpl        # User authentication template
├── jwt-gateway/
│   └── envoy.yaml             # Envoy JWT validation config
├── scripts/
│   ├── entrypoint.sh
│   └── health-check.sh
└── README.md
```

#### Completion Markers

- [ ] **P1.1.1** Create `services/pgdog-router/` directory structure
- [ ] **P1.1.2** Create basic `Makefile` with build targets
- [ ] **P1.1.3** Create `README.md` with setup instructions
- [ ] **P1.1.4** Add to root `go.work` if Go components added

---

### 2.2 PgDog Docker Image

**Complexity**: M (Medium)

#### Dockerfile Specification

```dockerfile
# Multi-stage build for PgDog
FROM rust:1.75-bookworm AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    libssl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Clone and build PgDog
WORKDIR /build
RUN git clone --depth 1 --branch v0.9.0 https://github.com/pgdogdev/pgdog.git
WORKDIR /build/pgdog
RUN cargo build --release

# Runtime image
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/pgdog/target/release/pgdog /usr/local/bin/pgdog

# Create config directories
RUN mkdir -p /etc/pgdog/certs /var/log/pgdog

# Copy default configs (will be overwritten by ConfigMap)
COPY config/pgdog.toml.tmpl /etc/pgdog/pgdog.toml
COPY config/users.toml.tmpl /etc/pgdog/users.toml

# Copy entrypoint
COPY scripts/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 5432 6432 9090

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
    CMD /usr/local/bin/pgdog --health-check || exit 1

ENTRYPOINT ["/entrypoint.sh"]
CMD ["pgdog", "--config", "/etc/pgdog/pgdog.toml"]
```

#### Completion Markers

- [ ] **P1.2.1** Create `Dockerfile` with multi-stage build
- [ ] **P1.2.2** Create `Dockerfile.dev` for local development
- [ ] **P1.2.3** Build and test image locally
- [ ] **P1.2.4** Push to container registry
- [ ] **P1.2.5** Document image build process

---

### 2.3 Basic PgDog Configuration Template

**Complexity**: M (Medium)

#### pgdog.toml.tmpl

```toml
# PgDog Configuration Template for Orochi DB
# Variables: {{.ClusterName}}, {{.Namespace}}, {{.ShardCount}}

[general]
host = "0.0.0.0"
port = 5432
admin_port = 6432
connect_timeout = 5000
idle_timeout = 30000
shutdown_timeout = 5000
worker_threads = 4

# Authentication (passthrough mode - JWT handled by gateway)
auth_type = "scram"
passthrough_auth = "enabled"

[general.tls]
enabled = true
certificate = "/etc/pgdog/certs/tls.crt"
private_key = "/etc/pgdog/certs/tls.key"
ca_file = "/etc/pgdog/certs/ca.crt"

[metrics]
prometheus_enabled = true
prometheus_port = 9090

[admin]
enabled = true
port = 6432

# Connection pool defaults
[pools.default]
pool_mode = "transaction"
min_pool_size = 0
max_pool_size = 100
statement_timeout = 30000
idle_timeout = 600000

# Primary database
[[databases]]
name = "{{.DatabaseName}}"
host = "{{.ClusterName}}-rw.{{.Namespace}}.svc.cluster.local"
port = 5432
role = "primary"

# Replica for read queries
[[databases]]
name = "{{.DatabaseName}}"
host = "{{.ClusterName}}-ro.{{.Namespace}}.svc.cluster.local"
port = 5432
role = "replica"

# Query routing
[query_routing]
read_write_split = true
read_query_keywords = ["SELECT", "SHOW", "EXPLAIN", "ANALYZE"]

# Health checks
[health_check]
query = "SELECT 1"
interval = 10000
timeout = 5000
retries = 3
```

#### Completion Markers

- [ ] **P1.3.1** Create `pgdog.toml.tmpl` with all required settings
- [ ] **P1.3.2** Create `users.toml.tmpl` for user configuration
- [ ] **P1.3.3** Create config generator tool in Go
- [ ] **P1.3.4** Test config generation for sample cluster
- [ ] **P1.3.5** Document all configuration options

---

### 2.4 JWT Gateway (Envoy Sidecar)

**Complexity**: L (Large)

Since PgDog lacks authentication hooks, we use Envoy as a JWT validation layer.

#### envoy.yaml

```yaml
static_resources:
  listeners:
    - name: postgres_listener
      address:
        socket_address:
          address: 0.0.0.0
          port_value: 5433  # External port
      filter_chains:
        - filters:
            - name: envoy.filters.network.postgres_proxy
              typed_config:
                "@type": type.googleapis.com/envoy.extensions.filters.network.postgres_proxy.v3alpha.PostgresProxy
                stat_prefix: postgres
                enable_sql_parsing: true
            - name: envoy.filters.network.tcp_proxy
              typed_config:
                "@type": type.googleapis.com/envoy.extensions.filters.network.tcp_proxy.v3.TcpProxy
                stat_prefix: tcp
                cluster: pgdog_cluster

  clusters:
    - name: pgdog_cluster
      connect_timeout: 5s
      type: STRICT_DNS
      lb_policy: ROUND_ROBIN
      load_assignment:
        cluster_name: pgdog_cluster
        endpoints:
          - lb_endpoints:
              - endpoint:
                  address:
                    socket_address:
                      address: 127.0.0.1
                      port_value: 5432

# JWT validation handled via HTTP filter for initial connection
# PostgreSQL password field contains JWT token
```

#### Alternative: Custom Go JWT Proxy

```go
// cmd/jwt-proxy/main.go
// Lightweight JWT validation proxy for PostgreSQL connections
// Validates JWT in password field, forwards to PgDog with extracted claims
```

#### Completion Markers

- [ ] **P1.4.1** Design JWT validation approach (Envoy vs custom proxy)
- [ ] **P1.4.2** Implement JWT validation layer
- [ ] **P1.4.3** Configure JWT public key management
- [ ] **P1.4.4** Test JWT validation flow end-to-end
- [ ] **P1.4.5** Document authentication architecture

---

### 2.5 Kubernetes Deployment Manifests

**Complexity**: M (Medium)

#### infrastructure/digitalocean/pgdog-router.yaml

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: pgdog-router
  namespace: orochi-cloud
  labels:
    app: pgdog-router
    component: connection-pooler
spec:
  replicas: 2
  selector:
    matchLabels:
      app: pgdog-router
  template:
    metadata:
      labels:
        app: pgdog-router
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "9090"
    spec:
      containers:
        - name: pgdog
          image: registry.digitalocean.com/orochi-registry/pgdog-router:latest
          ports:
            - containerPort: 5432
              name: postgres
            - containerPort: 6432
              name: admin
            - containerPort: 9090
              name: metrics
          env:
            - name: PGDOG_CONFIG
              value: "/etc/pgdog/pgdog.toml"
          volumeMounts:
            - name: config
              mountPath: /etc/pgdog
              readOnly: true
            - name: certs
              mountPath: /etc/pgdog/certs
              readOnly: true
          resources:
            requests:
              memory: "256Mi"
              cpu: "250m"
            limits:
              memory: "1Gi"
              cpu: "1000m"
          livenessProbe:
            tcpSocket:
              port: 5432
            initialDelaySeconds: 10
            periodSeconds: 30
          readinessProbe:
            tcpSocket:
              port: 5432
            initialDelaySeconds: 5
            periodSeconds: 10
      volumes:
        - name: config
          configMap:
            name: pgdog-config
        - name: certs
          secret:
            secretName: pgdog-tls
      affinity:
        podAntiAffinity:
          preferredDuringSchedulingIgnoredDuringExecution:
            - weight: 100
              podAffinityTerm:
                labelSelector:
                  matchLabels:
                    app: pgdog-router
                topologyKey: kubernetes.io/hostname
---
apiVersion: v1
kind: Service
metadata:
  name: pgdog-router
  namespace: orochi-cloud
spec:
  type: LoadBalancer
  selector:
    app: pgdog-router
  ports:
    - name: postgres
      port: 5432
      targetPort: 5432
    - name: admin
      port: 6432
      targetPort: 6432
    - name: metrics
      port: 9090
      targetPort: 9090
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: pgdog-config
  namespace: orochi-cloud
data:
  pgdog.toml: |
    # Generated configuration - see template
---
apiVersion: policy/v1
kind: PodDisruptionBudget
metadata:
  name: pgdog-router-pdb
  namespace: orochi-cloud
spec:
  minAvailable: 1
  selector:
    matchLabels:
      app: pgdog-router
```

#### Completion Markers

- [ ] **P1.5.1** Create Deployment manifest
- [ ] **P1.5.2** Create Service manifest (LoadBalancer)
- [ ] **P1.5.3** Create ConfigMap for pgdog.toml
- [ ] **P1.5.4** Create Secret for TLS certificates
- [ ] **P1.5.5** Create PodDisruptionBudget
- [ ] **P1.5.6** Create RBAC resources (ServiceAccount, Role, RoleBinding)
- [ ] **P1.5.7** Deploy to development cluster
- [ ] **P1.5.8** Verify health checks passing

---

### 2.6 Helm Chart Integration

**Complexity**: S (Small)

#### Completion Markers

- [ ] **P1.6.1** Create `infrastructure/helm/orochi-cloud/templates/pgdog-router/` directory
- [ ] **P1.6.2** Add deployment.yaml template
- [ ] **P1.6.3** Add service.yaml template
- [ ] **P1.6.4** Add configmap.yaml template
- [ ] **P1.6.5** Update values.yaml with pgdog section
- [ ] **P1.6.6** Update values-production.yaml
- [ ] **P1.6.7** Test Helm install/upgrade

---

### Phase 1 Deliverables Summary

| Deliverable | Status |
|-------------|--------|
| Docker image with PgDog + TLS | ⬜ |
| JWT validation layer | ⬜ |
| Basic Kubernetes deployment | ⬜ |
| Health checks passing | ⬜ |
| Prometheus metrics accessible | ⬜ |
| Helm chart templates | ⬜ |

---

## 3. Phase 2: Core Features (Weeks 4-7)

**Goal**: Implement sharding integration and control plane API

### 3.1 orochi_hash() Compatible Sharding

**Complexity**: M (Medium)

PgDog uses PostgreSQL's native `PARTITION BY HASH` which is compatible with Orochi's CRC32-based hashing.

#### Orochi Hash Constants (from distribution.c)

```c
#define OROCHI_HASH_SEED        0x9747b28c
#define OROCHI_HASH_MODULUS     INT32_MAX
```

#### PgDog Sharding Configuration

```toml
[sharding]
enabled = true
default_shard_count = 32

# Use PostgreSQL's hash function (compatible with Orochi)
sharding_function = "postgres_hash"

# Sharded tables configuration
[[sharding.tables]]
name = "distributed_table"
column = "tenant_id"
shards = 32

[[sharding.tables]]
name = "orders"
column = "customer_id"
shards = 32
```

#### Completion Markers

- [ ] **P2.1.1** Verify PgDog hash compatibility with orochi_hash()
- [ ] **P2.1.2** Create hash compatibility test suite
- [ ] **P2.1.3** Configure sharding for distributed tables
- [ ] **P2.1.4** Test single-shard query routing
- [ ] **P2.1.5** Test multi-shard query execution
- [ ] **P2.1.6** Document sharding configuration

---

### 3.2 Read/Write Splitting

**Complexity**: S (Small)

```toml
[query_routing]
read_write_split = true
read_query_keywords = ["SELECT", "SHOW", "EXPLAIN", "ANALYZE"]

# Sticky connections for transactions
sticky_transactions = true

# Replica lag tolerance (milliseconds)
max_replica_lag = 1000
```

#### Completion Markers

- [ ] **P2.2.1** Configure read/write splitting in pgdog.toml
- [ ] **P2.2.2** Test SELECT routing to replicas
- [ ] **P2.2.3** Test write routing to primary
- [ ] **P2.2.4** Test transaction stickiness
- [ ] **P2.2.5** Test replica lag detection

---

### 3.3 Control Plane API Integration

**Complexity**: L (Large)

#### New API Endpoints

```go
// services/control-plane/internal/api/handlers/pooler.go

// GET /api/v1/clusters/{id}/pooler
func (h *Handler) GetPoolerStatus(w http.ResponseWriter, r *http.Request)

// PATCH /api/v1/clusters/{id}/pooler
func (h *Handler) UpdatePoolerConfig(w http.ResponseWriter, r *http.Request)

// GET /api/v1/clusters/{id}/pooler/stats
func (h *Handler) GetPoolerStats(w http.ResponseWriter, r *http.Request)

// POST /api/v1/clusters/{id}/pooler/reload
func (h *Handler) ReloadPoolerConfig(w http.ResponseWriter, r *http.Request)
```

#### Pooler Configuration Model

```go
// services/control-plane/internal/models/pooler.go

type PoolerConfig struct {
    Enabled           bool   `json:"enabled"`
    Mode              string `json:"mode"` // transaction, session
    MaxConnections    int    `json:"max_connections"`
    IdleTimeout       int    `json:"idle_timeout_seconds"`
    ReadWriteSplit    bool   `json:"read_write_split"`
    ShardingEnabled   bool   `json:"sharding_enabled"`
    DefaultShardCount int    `json:"default_shard_count"`
}

type PoolerStats struct {
    ActiveConnections   int     `json:"active_connections"`
    IdleConnections     int     `json:"idle_connections"`
    TotalQueries        int64   `json:"total_queries"`
    QueriesPerSecond    float64 `json:"queries_per_second"`
    AverageLatencyMs    float64 `json:"average_latency_ms"`
    P99LatencyMs        float64 `json:"p99_latency_ms"`
    ShardDistribution   map[int]int64 `json:"shard_distribution"`
}
```

#### Completion Markers

- [ ] **P2.3.1** Create pooler handler in control-plane
- [ ] **P2.3.2** Add pooler routes to router.go
- [ ] **P2.3.3** Implement GetPoolerStatus endpoint
- [ ] **P2.3.4** Implement UpdatePoolerConfig endpoint
- [ ] **P2.3.5** Implement GetPoolerStats endpoint
- [ ] **P2.3.6** Add PgDog admin client for stats retrieval
- [ ] **P2.3.7** Update cluster provisioning to include pooler
- [ ] **P2.3.8** Write API integration tests

---

### 3.4 Provisioner Integration

**Complexity**: M (Medium)

Update provisioner to deploy PgDog alongside PostgreSQL clusters.

#### Updated ClusterSpec

```go
// services/provisioner/pkg/types/cluster.go

type ConnectionPoolerSpec struct {
    Enabled           bool              `json:"enabled"`
    Image             string            `json:"image"`
    Replicas          int32             `json:"replicas"`
    Resources         ResourceRequirements `json:"resources"`
    Mode              string            `json:"mode"`
    MaxConnections    int32             `json:"max_connections"`
    ReadWriteSplit    bool              `json:"read_write_split"`
    ShardingConfig    *ShardingConfig   `json:"sharding_config,omitempty"`
}

type ShardingConfig struct {
    Enabled       bool     `json:"enabled"`
    ShardCount    int32    `json:"shard_count"`
    ShardedTables []string `json:"sharded_tables"`
}
```

#### Completion Markers

- [ ] **P2.4.1** Update ClusterSpec with pooler configuration
- [ ] **P2.4.2** Create PgDog deployment template
- [ ] **P2.4.3** Update CreateCluster to deploy PgDog
- [ ] **P2.4.4** Update DeleteCluster to remove PgDog
- [ ] **P2.4.5** Add pooler status to GetCluster response
- [ ] **P2.4.6** Test end-to-end cluster provisioning with pooler

---

### Phase 2 Deliverables Summary

| Deliverable | Status |
|-------------|--------|
| orochi_hash() compatible sharding | ⬜ |
| Read/write splitting operational | ⬜ |
| Control plane pooler API endpoints | ⬜ |
| Provisioner PgDog integration | ⬜ |
| Transaction-mode pooling verified | ⬜ |

---

## 4. Phase 3: Advanced Features (Weeks 8-11)

**Goal**: Multi-tenant routing, scale-to-zero, and 2PC support

### 4.1 Multi-Tenant SNI Routing

**Complexity**: L (Large)

Route connections based on SNI hostname pattern: `{cluster-id}.{branch}.db.orochi.cloud`

#### Completion Markers

- [ ] **P3.1.1** Design SNI extraction and parsing logic
- [ ] **P3.1.2** Implement cluster discovery from control plane
- [ ] **P3.1.3** Configure dynamic backend resolution
- [ ] **P3.1.4** Add cluster configuration caching
- [ ] **P3.1.5** Handle unknown clusters gracefully
- [ ] **P3.1.6** Test multi-tenant isolation
- [ ] **P3.1.7** Document SNI routing patterns

---

### 4.2 Branch-Aware Query Routing

**Complexity**: M (Medium)

Route queries based on `orochi.branch_id` session variable.

```toml
[query_routing.branch]
enabled = true
session_variable = "orochi.branch_id"
default_branch = "main"
```

#### Completion Markers

- [ ] **P3.2.1** Configure session variable extraction
- [ ] **P3.2.2** Implement branch-to-shard mapping
- [ ] **P3.2.3** Test branch isolation
- [ ] **P3.2.4** Integrate with JWT claims (branch_id)
- [ ] **P3.2.5** Document branch routing behavior

---

### 4.3 Scale-to-Zero Wake-on-Connect

**Complexity**: XL (Extra Large)

This is the most complex feature requiring coordination with multiple services.

#### Architecture

```
Client Connection
       │
       ▼
┌─────────────────┐
│   PgDog/Proxy   │──── Check cluster state
└────────┬────────┘           │
         │                    ▼
         │           ┌─────────────────┐
         │           │  Control Plane  │
         │           │  GET /clusters  │
         │           └────────┬────────┘
         │                    │
         │    If suspended:   │
         │           ┌────────▼────────┐
         │           │ POST /wake      │
         │           └────────┬────────┘
         │                    │
         │           ┌────────▼────────┐
         │           │   Provisioner   │
         │           │   Resume Pod    │
         │           └────────┬────────┘
         │                    │
         │◀───────────────────┘
         │    Wait for ready
         ▼
┌─────────────────┐
│   PostgreSQL    │
└─────────────────┘
```

#### Control Plane Wake Endpoint

```go
// POST /api/v1/clusters/{id}/wake
func (h *Handler) WakeCluster(w http.ResponseWriter, r *http.Request) {
    clusterID := chi.URLParam(r, "id")

    // Trigger provisioner to resume cluster
    err := h.provisioner.ResumeCluster(r.Context(), clusterID)
    if err != nil {
        // Return error with retry-after header
    }

    // Return 202 Accepted with status endpoint
    w.Header().Set("Location", fmt.Sprintf("/api/v1/clusters/%s/status", clusterID))
    w.WriteHeader(http.StatusAccepted)
}
```

#### Completion Markers

- [ ] **P3.3.1** Add cluster state tracking (active, suspended, waking)
- [ ] **P3.3.2** Create control plane wake endpoint
- [ ] **P3.3.3** Implement provisioner suspend/resume
- [ ] **P3.3.4** Create connection queue in proxy layer
- [ ] **P3.3.5** Implement wake-on-connect hook
- [ ] **P3.3.6** Add timeout and error handling
- [ ] **P3.3.7** Test cold start latency (target: <5s)
- [ ] **P3.3.8** Document scale-to-zero behavior

---

### 4.4 Two-Phase Commit (2PC) Support

**Complexity**: M (Medium)

Leverage PgDog's native 2PC for cross-shard writes.

```toml
[sharding.two_phase_commit]
enabled = true
transaction_id_prefix = "__orochi_2pc_"
timeout_seconds = 30
cleanup_interval_seconds = 60
```

#### Completion Markers

- [ ] **P3.4.1** Enable 2PC in PgDog configuration
- [ ] **P3.4.2** Test cross-shard INSERT/UPDATE/DELETE
- [ ] **P3.4.3** Test 2PC rollback on failure
- [ ] **P3.4.4** Implement orphaned transaction cleanup
- [ ] **P3.4.5** Add 2PC metrics to monitoring
- [ ] **P3.4.6** Document 2PC behavior and limitations

---

### 4.5 Enhanced Prometheus Metrics

**Complexity**: S (Small)

```toml
[metrics]
prometheus_enabled = true
prometheus_port = 9090
include_labels = ["cluster_id", "branch_id", "pool_name", "shard_id"]

# Custom metric prefixes
metric_prefix = "orochi_pgdog"
```

#### Metrics to Expose

| Metric | Type | Description |
|--------|------|-------------|
| `orochi_pgdog_connections_active` | Gauge | Active connections |
| `orochi_pgdog_connections_idle` | Gauge | Idle connections |
| `orochi_pgdog_queries_total` | Counter | Total queries processed |
| `orochi_pgdog_query_duration_seconds` | Histogram | Query latency |
| `orochi_pgdog_shard_queries_total` | Counter | Queries per shard |
| `orochi_pgdog_2pc_commits_total` | Counter | 2PC commits |
| `orochi_pgdog_2pc_rollbacks_total` | Counter | 2PC rollbacks |
| `orochi_pgdog_wake_events_total` | Counter | Scale-to-zero wake events |

#### Completion Markers

- [ ] **P3.5.1** Configure comprehensive metrics
- [ ] **P3.5.2** Create Grafana dashboard
- [ ] **P3.5.3** Create alerting rules
- [ ] **P3.5.4** Test metrics under load
- [ ] **P3.5.5** Document metrics reference

---

### Phase 3 Deliverables Summary

| Deliverable | Status |
|-------------|--------|
| SNI-based multi-tenant routing | ⬜ |
| Branch-aware query routing | ⬜ |
| Scale-to-zero wake-on-connect | ⬜ |
| 2PC for cross-shard writes | ⬜ |
| Enhanced Prometheus metrics | ⬜ |
| Grafana dashboard | ⬜ |

---

## 5. Phase 4: Production Readiness (Weeks 12-14)

**Goal**: Security hardening, HA, testing, and documentation

### 5.1 Security Hardening

**Complexity**: M (Medium)

#### Completion Markers

- [ ] **P4.1.1** Implement rate limiting per client IP
- [ ] **P4.1.2** Add connection limits per tenant
- [ ] **P4.1.3** Enable IP allowlist from endpoint config
- [ ] **P4.1.4** Configure audit logging for auth events
- [ ] **P4.1.5** Implement token revocation support
- [ ] **P4.1.6** Enable mTLS for internal communication
- [ ] **P4.1.7** Security review and penetration testing

---

### 5.2 High Availability Configuration

**Complexity**: M (Medium)

#### Completion Markers

- [ ] **P4.2.1** Configure 3+ replicas for production
- [ ] **P4.2.2** Add pod anti-affinity rules
- [ ] **P4.2.3** Implement graceful shutdown (SIGTERM handling)
- [ ] **P4.2.4** Configure connection draining
- [ ] **P4.2.5** Test rolling updates without downtime
- [ ] **P4.2.6** Verify PodDisruptionBudget behavior
- [ ] **P4.2.7** Document HA configuration

---

### 5.3 CLI Integration

**Complexity**: S (Small)

```bash
# New CLI commands
orochi cluster pooler status <cluster-id>
orochi cluster pooler configure <cluster-id> --max-connections 200
orochi cluster pooler stats <cluster-id>
orochi cluster pooler reload <cluster-id>
```

#### Completion Markers

- [ ] **P4.3.1** Add `pooler` subcommand to CLI
- [ ] **P4.3.2** Implement `pooler status` command
- [ ] **P4.3.3** Implement `pooler configure` command
- [ ] **P4.3.4** Implement `pooler stats` command
- [ ] **P4.3.5** Add CLI tests
- [ ] **P4.3.6** Update CLI documentation

---

### 5.4 Dashboard Integration

**Complexity**: M (Medium)

#### Completion Markers

- [ ] **P4.4.1** Add pooler status card to cluster detail page
- [ ] **P4.4.2** Create connection pool visualization
- [ ] **P4.4.3** Add pooler configuration panel
- [ ] **P4.4.4** Show shard distribution chart
- [ ] **P4.4.5** Display wake-on-connect events in activity log
- [ ] **P4.4.6** Add pooler metrics to cluster metrics page

---

### 5.5 Documentation

**Complexity**: S (Small)

#### Completion Markers

- [ ] **P4.5.1** Create operational runbook
- [ ] **P4.5.2** Document configuration reference
- [ ] **P4.5.3** Write troubleshooting guide
- [ ] **P4.5.4** Document scaling guidelines
- [ ] **P4.5.5** Create architecture decision records (ADRs)
- [ ] **P4.5.6** Update CLAUDE.md with PgDog information

---

### Phase 4 Deliverables Summary

| Deliverable | Status |
|-------------|--------|
| Security hardening complete | ⬜ |
| HA configuration verified | ⬜ |
| CLI pooler commands | ⬜ |
| Dashboard pooler integration | ⬜ |
| Complete documentation | ⬜ |

---

## 6. Testing Strategy

### 6.1 Test Categories Overview

| Category | Test Count | Priority | Tools |
|----------|------------|----------|-------|
| Unit Tests | 40+ | P0 | Go testing, Rust cargo test |
| Integration Tests | 50+ | P0 | Go testing, Docker Compose |
| End-to-End Tests | 20+ | P1 | Playwright, k6 |
| Performance Tests | 15+ | P1 | k6, pgbench |
| Chaos Tests | 10+ | P2 | Chaos Mesh, Litmus |
| Security Tests | 15+ | P0 | OWASP ZAP, custom |

---

### 6.2 Unit Tests

#### JWT Gateway Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| JWT-001 | Valid RS256 token accepted | P0 | ⬜ |
| JWT-002 | Expired token rejected | P0 | ⬜ |
| JWT-003 | Invalid signature rejected | P0 | ⬜ |
| JWT-004 | Missing required claims rejected | P0 | ⬜ |
| JWT-005 | Token with wrong issuer rejected | P0 | ⬜ |
| JWT-006 | Token cache hit returns cached result | P1 | ⬜ |
| JWT-007 | Token cache miss triggers validation | P1 | ⬜ |
| JWT-008 | Cache TTL expiration works | P1 | ⬜ |
| JWT-009 | Concurrent validation thread-safe | P1 | ⬜ |
| JWT-010 | Claims extraction correct | P0 | ⬜ |

#### Completion Markers

- [ ] **T1.1** All JWT unit tests passing
- [ ] **T1.2** Unit test coverage > 80%

---

### 6.3 Integration Tests

#### Connection Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| CONN-001 | Basic connection through proxy | P0 | ⬜ |
| CONN-002 | TLS connection establishment | P0 | ⬜ |
| CONN-003 | Connection with valid JWT | P0 | ⬜ |
| CONN-004 | Connection rejected with invalid JWT | P0 | ⬜ |
| CONN-005 | Connection reuse from pool | P0 | ⬜ |
| CONN-006 | Connection timeout handling | P1 | ⬜ |
| CONN-007 | Prepared statement works | P0 | ⬜ |
| CONN-008 | Transaction isolation | P0 | ⬜ |
| CONN-009 | Large result set handling | P1 | ⬜ |
| CONN-010 | Binary protocol support | P2 | ⬜ |

#### Sharding Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| SHARD-001 | Single-shard SELECT routes correctly | P0 | ⬜ |
| SHARD-002 | Multi-shard SELECT aggregates results | P0 | ⬜ |
| SHARD-003 | INSERT routes to correct shard | P0 | ⬜ |
| SHARD-004 | UPDATE routes to correct shard | P0 | ⬜ |
| SHARD-005 | DELETE routes to correct shard | P0 | ⬜ |
| SHARD-006 | Hash function matches orochi_hash() | P0 | ⬜ |
| SHARD-007 | Cross-shard JOIN works | P1 | ⬜ |
| SHARD-008 | Cross-shard ORDER BY works | P1 | ⬜ |
| SHARD-009 | Cross-shard GROUP BY works | P1 | ⬜ |
| SHARD-010 | 2PC commit succeeds | P0 | ⬜ |
| SHARD-011 | 2PC rollback on failure | P0 | ⬜ |

#### Read/Write Split Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| RW-001 | SELECT routes to replica | P0 | ⬜ |
| RW-002 | INSERT routes to primary | P0 | ⬜ |
| RW-003 | UPDATE routes to primary | P0 | ⬜ |
| RW-004 | DELETE routes to primary | P0 | ⬜ |
| RW-005 | Transaction stays on primary | P0 | ⬜ |
| RW-006 | Read-after-write consistency | P1 | ⬜ |

#### Completion Markers

- [ ] **T2.1** All connection tests passing
- [ ] **T2.2** All sharding tests passing
- [ ] **T2.3** All read/write split tests passing
- [ ] **T2.4** Integration test coverage > 70%

---

### 6.4 Performance Tests

| Test ID | Description | Target | Priority | Status |
|---------|-------------|--------|----------|--------|
| PERF-001 | Connection establishment latency | < 50ms | P0 | ⬜ |
| PERF-002 | Query proxy overhead | < 2ms p99 | P0 | ⬜ |
| PERF-003 | Throughput (queries/sec) | > 50K | P1 | ⬜ |
| PERF-004 | Connection pool efficiency | > 95% | P1 | ⬜ |
| PERF-005 | Memory usage under load | < 1GB | P1 | ⬜ |
| PERF-006 | 10K concurrent connections | Stable | P1 | ⬜ |
| PERF-007 | Cross-shard query latency | < 100ms p99 | P1 | ⬜ |
| PERF-008 | JWT validation latency (cached) | < 1ms | P0 | ⬜ |
| PERF-009 | Scale-to-zero wake latency | < 5s | P1 | ⬜ |

#### Completion Markers

- [ ] **T3.1** All P0 performance targets met
- [ ] **T3.2** All P1 performance targets met
- [ ] **T3.3** Performance regression tests in CI

---

### 6.5 Chaos/Resilience Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| CHAOS-001 | Primary failover handled | P0 | ⬜ |
| CHAOS-002 | Replica failure handled | P1 | ⬜ |
| CHAOS-003 | Network partition recovery | P1 | ⬜ |
| CHAOS-004 | PgDog pod restart recovery | P0 | ⬜ |
| CHAOS-005 | Control plane unavailable | P1 | ⬜ |
| CHAOS-006 | 2PC coordinator failure | P1 | ⬜ |
| CHAOS-007 | Connection storm handling | P2 | ⬜ |

#### Completion Markers

- [ ] **T4.1** All P0 chaos tests passing
- [ ] **T4.2** All P1 chaos tests passing
- [ ] **T4.3** Chaos tests documented in runbook

---

### 6.6 Security Tests

| Test ID | Description | Priority | Status |
|---------|-------------|----------|--------|
| SEC-001 | Invalid JWT rejected | P0 | ⬜ |
| SEC-002 | Expired JWT rejected | P0 | ⬜ |
| SEC-003 | SQL injection blocked | P0 | ⬜ |
| SEC-004 | TLS required for connections | P0 | ⬜ |
| SEC-005 | Cross-tenant data isolation | P0 | ⬜ |
| SEC-006 | Rate limiting enforced | P1 | ⬜ |
| SEC-007 | Unauthorized admin access blocked | P0 | ⬜ |
| SEC-008 | Token revocation works | P1 | ⬜ |

#### Completion Markers

- [ ] **T5.1** All P0 security tests passing
- [ ] **T5.2** Security review completed
- [ ] **T5.3** Penetration test passed

---

## 7. Completion Checklist

### Phase 1: Foundation ⬜

- [ ] Service directory structure created
- [ ] Docker image builds successfully
- [ ] PgDog configuration template complete
- [ ] JWT gateway implemented
- [ ] Kubernetes manifests deployed
- [ ] Helm chart templates added
- [ ] Basic health checks passing
- [ ] Prometheus metrics accessible

### Phase 2: Core Features ⬜

- [ ] orochi_hash() sharding compatible
- [ ] Read/write splitting operational
- [ ] Control plane API endpoints added
- [ ] Provisioner integration complete
- [ ] Transaction pooling verified
- [ ] All integration tests passing

### Phase 3: Advanced Features ⬜

- [ ] Multi-tenant SNI routing working
- [ ] Branch-aware routing implemented
- [ ] Scale-to-zero operational
- [ ] 2PC working for cross-shard writes
- [ ] Grafana dashboard created
- [ ] Alerting rules configured

### Phase 4: Production Readiness ⬜

- [ ] Security hardening complete
- [ ] HA configuration verified
- [ ] CLI commands implemented
- [ ] Dashboard integration complete
- [ ] Documentation finalized
- [ ] All tests passing
- [ ] Production deployment successful

---

## Appendix A: Key File References

| Component | File Path |
|-----------|-----------|
| JWT Implementation | `extensions/postgres/src/auth/jwt.c` |
| Endpoint Auth | `extensions/postgres/src/auth/endpoint_auth.c` |
| Hash Function | `extensions/postgres/src/sharding/distribution.c` |
| Control Plane Router | `services/control-plane/internal/api/router.go` |
| Cluster Service | `services/control-plane/internal/services/cluster_service.go` |
| Provisioner Service | `services/provisioner/internal/provisioner/service.go` |
| K8s Provisioner Manifest | `infrastructure/digitalocean/provisioner.yaml` |

---

## Appendix B: Risk Register

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| PgDog lacks auth hooks | High | Confirmed | Use external JWT gateway (Envoy) |
| Hash function incompatibility | High | Low | Extensive compatibility testing |
| Scale-to-zero latency > 5s | Medium | Medium | Warm standby pods, aggressive caching |
| 2PC performance overhead | Medium | Medium | Make 2PC optional per-table |
| AGPL license concerns | Medium | Low | Legal review, cloud-only deployment |

---

## Appendix C: Timeline Summary

| Phase | Duration | Start | End |
|-------|----------|-------|-----|
| Phase 1: Foundation | 3 weeks | Week 1 | Week 3 |
| Phase 2: Core Features | 4 weeks | Week 4 | Week 7 |
| Phase 3: Advanced Features | 4 weeks | Week 8 | Week 11 |
| Phase 4: Production Readiness | 3 weeks | Week 12 | Week 14 |
| **Total** | **14 weeks** | | |

---

**Document Status**: Draft
**Last Updated**: 2026-01-31
**Authors**: Claude (AI Assistant)
**Reviewers**: Pending
