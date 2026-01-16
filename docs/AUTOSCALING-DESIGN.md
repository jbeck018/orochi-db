# Orochi Cloud Auto-Scaling System Design

## Executive Summary

This document describes the architecture and implementation of an auto-scaling system for Orochi Cloud, the managed database service for Orochi DB. The system enables automatic scaling of database resources based on workload demands while minimizing costs through aggressive scale-down and scale-to-zero capabilities.

**Version**: 1.0
**Status**: Architecture Design Document
**Last Updated**: 2026-01-16

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Scaling Dimensions](#3-scaling-dimensions)
4. [Serverless and Scale-to-Zero](#4-serverless-and-scale-to-zero)
5. [Metrics and Triggers](#5-metrics-and-triggers)
6. [Scaling Algorithms](#6-scaling-algorithms)
7. [Implementation Approach](#7-implementation-approach)
8. [Kubernetes Resources](#8-kubernetes-resources)
9. [Wake-Up Mechanism](#9-wake-up-mechanism)
10. [Cost Optimization](#10-cost-optimization)
11. [Operational Runbook](#11-operational-runbook)
12. [Testing Strategy](#12-testing-strategy)

---

## 1. Overview

### 1.1 Goals

| Goal | Description | Target |
|------|-------------|--------|
| **Zero-Downtime Scaling** | Scale without connection drops or query failures | 99.99% success rate |
| **Fast Response** | React to load changes quickly | < 30 seconds to scale-up decision |
| **Cost Efficiency** | Minimize idle resource costs | Scale-to-zero for inactive clusters |
| **Burst Handling** | Handle sudden traffic spikes | 10x capacity in < 60 seconds |
| **Predictive Scaling** | Anticipate load patterns | 80% of scaling proactive |

### 1.2 Constraints

- **Data Locality**: Minimize data movement during horizontal scaling
- **Connection Continuity**: Existing connections must not be dropped during scaling
- **PostgreSQL Limitations**: Respect PostgreSQL's shared memory and process model
- **Kubernetes Resources**: Work within K8s node pool capacity
- **Budget Limits**: Respect per-organization spending caps

### 1.3 Scaling Scope

```
+------------------------------------------------------------------+
|                     SCALING DIMENSIONS                            |
+------------------------------------------------------------------+
|                                                                    |
|  VERTICAL (Instance Size)          HORIZONTAL (Replicas)          |
|  +-------------------------+       +-------------------------+     |
|  | CPU: 0.25 - 128 vCPU   |       | Primary: 1              |     |
|  | Memory: 1GB - 512GB    |       | Read Replicas: 0 - 15   |     |
|  | IOPS: 3000 - 256000    |       | Serverless Pools: 1 - N |     |
|  +-------------------------+       +-------------------------+     |
|                                                                    |
|  STORAGE (Capacity)                CONNECTION POOLING              |
|  +-------------------------+       +-------------------------+     |
|  | Hot: 10GB - 64TB       |       | Connections: 25 - 10000 |     |
|  | Auto-expand: Yes       |       | Pool Size: 10 - 500     |     |
|  | Never shrink (data)    |       | Transaction Mode        |     |
|  +-------------------------+       +-------------------------+     |
|                                                                    |
+------------------------------------------------------------------+
```

---

## 2. Architecture

### 2.1 System Architecture (C4 Context)

```
+------------------------------------------------------------------+
|                    OROCHI CLOUD AUTOSCALING                        |
+------------------------------------------------------------------+
|                                                                    |
|  External Actors                                                   |
|  +-------------+  +-------------+  +-------------+                |
|  | Application |  | Prometheus  |  | CloudWatch  |                |
|  | Connections |  |  /Victoria  |  | /Stackdriver|                |
|  +------+------+  +------+------+  +------+------+                |
|         |                |                |                        |
|         v                v                v                        |
|  +------------------------------------------------------------------+
|  |                     CONNECTION PROXY LAYER                      |
|  |  +----------------------------------------------------------+  |
|  |  |                  Orochi Connection Router                 |  |
|  |  |  - SNI-based routing    - Connection multiplexing        |  |
|  |  |  - Wake-on-connect      - Graceful connection migration  |  |
|  |  +----------------------------------------------------------+  |
|  +------------------------------------------------------------------+
|         |                                                          |
|         v                                                          |
|  +------------------------------------------------------------------+
|  |                     AUTOSCALER CONTROL PLANE                    |
|  |                                                                  |
|  |  +----------------+  +----------------+  +----------------+     |
|  |  |    Metrics     |  |   Decision     |  |   Execution    |     |
|  |  |   Aggregator   |->|    Engine      |->|   Controller   |     |
|  |  +----------------+  +----------------+  +----------------+     |
|  |         ^                    ^                   |              |
|  |         |                    |                   v              |
|  |  +----------------+  +----------------+  +----------------+     |
|  |  |   Predictive   |  |    Policy      |  |   Kubernetes   |     |
|  |  |    Model       |  |   Evaluator    |  |   Operator     |     |
|  |  +----------------+  +----------------+  +----------------+     |
|  |                                                                  |
|  +------------------------------------------------------------------+
|         |                                                          |
|         v                                                          |
|  +------------------------------------------------------------------+
|  |                      DATA PLANE CLUSTERS                        |
|  |                                                                  |
|  |  Serverless Pool           Dedicated Clusters                   |
|  |  +-----------------+       +-----------------+                  |
|  |  | +---+ +---+     |       | Primary         |                  |
|  |  | |DB1| |DB2| ... |       | +---+           |                  |
|  |  | +---+ +---+     |       | |DB |  Replicas |                  |
|  |  | Shared Compute  |       | +---+  +---+---+|                  |
|  |  +-----------------+       |        |R1 |R2 ||                  |
|  |                            |        +---+---+|                  |
|  |                            +-----------------+                  |
|  +------------------------------------------------------------------+
|                                                                    |
+------------------------------------------------------------------+
```

### 2.2 Component Architecture (C4 Container)

```
+------------------------------------------------------------------+
|                    AUTOSCALER COMPONENTS                           |
+------------------------------------------------------------------+
|                                                                    |
|  +------------------------+     +---------------------------+      |
|  |   METRICS AGGREGATOR   |     |   DECISION ENGINE         |      |
|  +------------------------+     +---------------------------+      |
|  | - Prometheus adapter   |     | - Rule evaluation         |      |
|  | - Custom metrics API   |     | - Cooldown management     |      |
|  | - Historical storage   |     | - Scaling plan generation |      |
|  | - Anomaly detection    |     | - Conflict resolution     |      |
|  +----------+-------------+     +-------------+-------------+      |
|             |                                 |                    |
|             v                                 v                    |
|  +------------------------+     +---------------------------+      |
|  |   PREDICTIVE MODEL     |     |   POLICY EVALUATOR        |      |
|  +------------------------+     +---------------------------+      |
|  | - Time-series forecast |     | - Org budget limits       |      |
|  | - Pattern recognition  |     | - Tier constraints        |      |
|  | - Load prediction      |     | - SLA requirements        |      |
|  | - Seasonal adjustment  |     | - Maintenance windows     |      |
|  +----------+-------------+     +-------------+-------------+      |
|             |                                 |                    |
|             +----------------+----------------+                    |
|                              |                                     |
|                              v                                     |
|  +----------------------------------------------------------+     |
|  |                   EXECUTION CONTROLLER                    |     |
|  +----------------------------------------------------------+     |
|  | - Scaling orchestration    - Rollback handling            |     |
|  | - State machine management - Event publishing             |     |
|  | - Kubernetes API calls     - Audit logging                |     |
|  +----------------------------------------------------------+     |
|             |                                                      |
|             v                                                      |
|  +----------------------------------------------------------+     |
|  |                   KUBERNETES OPERATOR                     |     |
|  +----------------------------------------------------------+     |
|  | - OrochiCluster CRD        - StatefulSet management       |     |
|  | - PodDisruptionBudgets     - Service mesh integration     |     |
|  | - Resource quota mgmt      - Node affinity updates        |     |
|  +----------------------------------------------------------+     |
|                                                                    |
+------------------------------------------------------------------+
```

### 2.3 Data Flow

```
                     AUTOSCALING DATA FLOW

    +--------+     +--------+     +--------+     +--------+
    |Postgres|---->|Exporter|---->|Victoria|---->|Metrics |
    |Instance|     |Sidecar |     |Metrics |     |Aggreg. |
    +--------+     +--------+     +--------+     +---+----+
                                                     |
    +--------+     +--------+                        |
    |PgBounce|---->|Conn    |------------------------+
    |        |     |Metrics |                        |
    +--------+     +--------+                        v
                                               +----------+
    +--------+     +--------+                  |Decision  |
    |K8s Node|---->|Node    |----------------->|Engine    |
    |        |     |Exporter|                  +----+-----+
    +--------+     +--------+                       |
                                                    v
                                               +----------+
                                               |Execution |
                                               |Controller|
                                               +----+-----+
                                                    |
              +-------------+--------------+---------+
              |             |              |
              v             v              v
         +--------+    +--------+    +--------+
         |Vertical|    |Horiz.  |    |Pooler  |
         |Scaling |    |Scaling |    |Scaling |
         +--------+    +--------+    +--------+
```

---

## 3. Scaling Dimensions

### 3.1 Vertical Scaling (CPU/Memory)

Vertical scaling adjusts the compute resources (CPU and memory) of database instances.

#### Instance Size Tiers

| Tier | vCPU | Memory | Max Connections | Use Case |
|------|------|--------|-----------------|----------|
| nano | 0.25 | 0.5 GB | 25 | Development, CI/CD |
| micro | 0.5 | 1 GB | 50 | Low-traffic apps |
| small | 1 | 2 GB | 100 | Small production |
| medium | 2 | 4 GB | 200 | Standard workloads |
| large | 4 | 8 GB | 400 | High-traffic apps |
| xlarge | 8 | 16 GB | 800 | Analytics + OLTP |
| 2xlarge | 16 | 32 GB | 1500 | Large datasets |
| 4xlarge | 32 | 64 GB | 3000 | Enterprise |
| 8xlarge | 64 | 128 GB | 5000 | Heavy analytics |
| 16xlarge | 128 | 256 GB | 10000 | Extreme workloads |

#### Vertical Scaling Strategy

```
VERTICAL SCALING DECISION TREE

                    +-------------------+
                    | Current Metrics   |
                    +--------+----------+
                             |
              +--------------+--------------+
              |                             |
              v                             v
    +------------------+          +------------------+
    | CPU > 80% (5min) |          | Memory > 85%     |
    +--------+---------+          +--------+---------+
             |                             |
             v                             v
    +------------------+          +------------------+
    | Scale UP CPU     |          | Scale UP Memory  |
    | (next tier)      |          | (next tier)      |
    +------------------+          +------------------+
              |                             |
              +-------------+---------------+
                            |
                            v
              +----------------------------+
              | Validate:                  |
              | - Budget allows            |
              | - Tier not at max          |
              | - Cooldown expired         |
              +-------------+--------------+
                            |
              +-------------+--------------+
              |                            |
              v                            v
    +------------------+          +------------------+
    | Execute Scale    |          | Reject + Alert   |
    +------------------+          +------------------+
```

#### Zero-Downtime Vertical Scaling

PostgreSQL cannot change shared_buffers without restart. We implement zero-downtime vertical scaling through:

1. **Rolling Update with Connection Migration**:
   - Create new pod with target size
   - Wait for WAL catch-up (streaming replication)
   - Promote new pod to primary via controlled switchover
   - Migrate connections gracefully
   - Terminate old pod

2. **Serverless Mode** (Preferred):
   - Multiple warm containers at different sizes
   - Instant connection handoff between containers
   - No PostgreSQL restart required

```
ZERO-DOWNTIME VERTICAL SCALE SEQUENCE

Time -->

Old Primary:    [==========Running==========][--Drain--][X]
                                              \
Connection                                     \--Migrate-->
Migration:                                                  \
                                                             \
New Primary:                   [--Replica--][--Promote--][===Running===]
```

### 3.2 Horizontal Scaling (Read Replicas)

#### Replica Configuration

```yaml
horizontal_scaling:
  read_replicas:
    min: 0
    max: 15
    replication_mode: "async"  # or "sync" for HA

  replica_routing:
    strategy: "least_connections"  # or "round_robin", "latency_based"
    health_check_interval: "5s"
    failover_threshold: "3 failed checks"

  scaling_triggers:
    scale_up:
      - condition: "read_qps > 1000 per replica"
        action: "add_replica"
      - condition: "read_latency_p99 > 100ms"
        action: "add_replica"
    scale_down:
      - condition: "read_qps < 200 per replica for 15min"
        action: "remove_replica"
      - condition: "replica_count > 1 AND cpu < 30% for 30min"
        action: "remove_replica"
```

#### Replica Scaling Algorithm

```python
def evaluate_horizontal_scaling(cluster_metrics, config):
    """
    Evaluate whether to add or remove read replicas.

    Returns: ScalingDecision (scale_up, scale_down, or no_action)
    """
    current_replicas = cluster_metrics.replica_count

    # Calculate per-replica load
    read_qps_per_replica = cluster_metrics.total_read_qps / max(current_replicas, 1)
    avg_cpu_per_replica = cluster_metrics.avg_replica_cpu
    p99_latency = cluster_metrics.read_latency_p99

    # Scale-up conditions
    if (read_qps_per_replica > config.qps_threshold_high or
        p99_latency > config.latency_threshold_ms or
        avg_cpu_per_replica > config.cpu_threshold_high):

        if current_replicas < config.max_replicas:
            target_replicas = min(
                current_replicas + calculate_replicas_needed(cluster_metrics),
                config.max_replicas
            )
            return ScalingDecision(
                action="scale_up",
                target_replicas=target_replicas,
                reason=f"High load: QPS={read_qps_per_replica}, P99={p99_latency}ms"
            )

    # Scale-down conditions (with longer evaluation window)
    elif (current_replicas > config.min_replicas and
          read_qps_per_replica < config.qps_threshold_low and
          avg_cpu_per_replica < config.cpu_threshold_low and
          cluster_metrics.scale_down_stable_duration > config.scale_down_cooldown):

        target_replicas = max(
            calculate_optimal_replicas(cluster_metrics),
            config.min_replicas
        )
        return ScalingDecision(
            action="scale_down",
            target_replicas=target_replicas,
            reason=f"Low load: QPS={read_qps_per_replica}, CPU={avg_cpu_per_replica}%"
        )

    return ScalingDecision(action="no_action")


def calculate_replicas_needed(metrics):
    """Calculate additional replicas needed based on current load."""
    # Target 60% utilization for headroom
    target_utilization = 0.6

    cpu_based = math.ceil(metrics.total_cpu / (target_utilization * 100)) - metrics.replica_count
    qps_based = math.ceil(metrics.total_read_qps / (metrics.target_qps_per_replica * target_utilization)) - metrics.replica_count

    return max(cpu_based, qps_based, 1)
```

### 3.3 Storage Scaling

Storage scaling is unidirectional - we only expand, never shrink (to prevent data loss).

#### Auto-Expansion Rules

```yaml
storage_scaling:
  auto_expand:
    enabled: true
    threshold_percent: 80  # Expand when 80% full
    expansion_percent: 25  # Expand by 25% each time
    min_expansion_gb: 10   # Minimum 10GB expansion
    max_size_tb: 64        # Maximum storage size

  monitoring:
    check_interval: "1m"
    forecast_window: "7d"  # Predict storage needs

  alerts:
    warning_threshold: 70
    critical_threshold: 90

  cost_optimization:
    tiered_storage:
      enabled: true
      hot_to_warm_days: 7
      warm_to_cold_days: 30
      cold_to_archive_days: 90
```

#### Storage Expansion Algorithm

```python
def evaluate_storage_scaling(cluster_metrics, config):
    """
    Evaluate whether storage expansion is needed.
    Uses both current usage and growth rate prediction.
    """
    current_usage_percent = cluster_metrics.storage_used / cluster_metrics.storage_allocated * 100
    daily_growth_rate = cluster_metrics.storage_growth_rate_gb_per_day

    # Calculate days until threshold breach
    remaining_gb = cluster_metrics.storage_allocated - cluster_metrics.storage_used
    if daily_growth_rate > 0:
        days_to_threshold = (remaining_gb * (1 - config.threshold_percent/100)) / daily_growth_rate
    else:
        days_to_threshold = float('inf')

    # Immediate expansion needed
    if current_usage_percent >= config.threshold_percent:
        return expand_storage(cluster_metrics, config, reason="threshold_breach")

    # Proactive expansion (7-day forecast)
    if days_to_threshold <= 7:
        return expand_storage(cluster_metrics, config, reason="proactive_forecast")

    return StorageDecision(action="no_action")


def expand_storage(metrics, config, reason):
    """Calculate new storage allocation."""
    current_allocated = metrics.storage_allocated

    # Calculate expansion size
    expansion_gb = max(
        current_allocated * config.expansion_percent / 100,
        config.min_expansion_gb
    )

    # Apply maximum limit
    new_size = min(
        current_allocated + expansion_gb,
        config.max_size_tb * 1024
    )

    return StorageDecision(
        action="expand",
        current_size_gb=current_allocated,
        new_size_gb=new_size,
        reason=reason
    )
```

### 3.4 Connection Pooling Scaling

PgBouncer pool scaling adjusts the connection multiplexing capacity.

#### Pool Configuration

```yaml
connection_pooling:
  mode: "transaction"  # session, transaction, or statement

  scaling:
    min_pool_size: 10
    max_pool_size: 500
    default_pool_size: 50

    # Per-database settings
    per_db_pool:
      min: 5
      max: 100

  auto_scaling:
    enabled: true
    scale_up_threshold: 80  # % pool utilization
    scale_down_threshold: 20
    scale_increment: 20
    evaluation_period: "30s"
    cooldown: "60s"

  limits:
    max_client_connections: 10000
    max_db_connections: 1000  # Actual PG connections
    reserve_pool_size: 5
    reserve_pool_timeout: 5
```

#### Pool Scaling Algorithm

```python
def evaluate_pool_scaling(pool_metrics, config):
    """
    Evaluate whether to scale connection pool.

    Metrics:
    - active_connections: Currently in-use connections
    - waiting_clients: Clients waiting for a connection
    - avg_wait_time: Average time clients wait
    - pool_size: Current pool allocation
    """
    utilization = pool_metrics.active_connections / pool_metrics.pool_size * 100

    # Scale up: high utilization or clients waiting
    if utilization > config.scale_up_threshold or pool_metrics.waiting_clients > 0:
        new_size = min(
            pool_metrics.pool_size + config.scale_increment,
            config.max_pool_size
        )

        # Ensure we don't exceed PostgreSQL max_connections
        effective_new_size = min(
            new_size,
            pool_metrics.pg_max_connections - pool_metrics.reserved_connections
        )

        if effective_new_size > pool_metrics.pool_size:
            return PoolDecision(
                action="scale_up",
                new_size=effective_new_size,
                reason=f"Utilization: {utilization}%, Waiting: {pool_metrics.waiting_clients}"
            )

    # Scale down: low utilization for sustained period
    elif (utilization < config.scale_down_threshold and
          pool_metrics.low_utilization_duration > config.cooldown):

        new_size = max(
            pool_metrics.pool_size - config.scale_increment,
            config.min_pool_size,
            pool_metrics.active_connections * 2  # Keep 2x headroom
        )

        if new_size < pool_metrics.pool_size:
            return PoolDecision(
                action="scale_down",
                new_size=new_size,
                reason=f"Low utilization: {utilization}%"
            )

    return PoolDecision(action="no_action")
```

---

## 4. Serverless and Scale-to-Zero

### 4.1 Serverless Architecture

```
+------------------------------------------------------------------+
|                    SERVERLESS ARCHITECTURE                        |
+------------------------------------------------------------------+
|                                                                    |
|  Client Request                                                    |
|       |                                                            |
|       v                                                            |
|  +------------------------+                                        |
|  |  Connection Router     |  SNI: cluster-abc.db.orochi.cloud     |
|  |  (Always Running)      |                                        |
|  +----------+-------------+                                        |
|             |                                                      |
|             v                                                      |
|  +------------------------+                                        |
|  |  Cluster State Check   |                                        |
|  +----------+-------------+                                        |
|             |                                                      |
|      +------+------+                                               |
|      |             |                                               |
|      v             v                                               |
|  [RUNNING]     [SUSPENDED]                                         |
|      |             |                                               |
|      |             v                                               |
|      |    +-------------------+                                    |
|      |    |  Wake-Up Service  |                                    |
|      |    +--------+----------+                                    |
|      |             |                                               |
|      |             v                                               |
|      |    +-------------------+                                    |
|      |    | Start Compute Pod |                                    |
|      |    +--------+----------+                                    |
|      |             |                                               |
|      |             v                                               |
|      |    +-------------------+                                    |
|      |    | Load State from   |                                    |
|      |    | Cold Storage      |                                    |
|      |    +--------+----------+                                    |
|      |             |                                               |
|      +------+------+                                               |
|             |                                                      |
|             v                                                      |
|  +------------------------+                                        |
|  |  Route to Compute      |                                        |
|  +------------------------+                                        |
|                                                                    |
+------------------------------------------------------------------+
```

### 4.2 Idle Detection and Suspension

#### Idle Detection Criteria

```yaml
idle_detection:
  # All conditions must be true for cluster to be considered idle
  conditions:
    - metric: "active_connections"
      operator: "equals"
      value: 0
      duration: "5m"

    - metric: "queries_per_second"
      operator: "less_than"
      value: 0.1
      duration: "5m"

    - metric: "active_transactions"
      operator: "equals"
      value: 0
      duration: "1m"

  # Additional considerations
  exclusions:
    - has_active_replication_slots: true
    - has_pending_logical_replication: true
    - within_scheduled_window: true
    - explicit_keep_alive: true

  grace_period: "10m"  # Total time before suspension

  # Per-tier overrides
  tier_overrides:
    free:
      grace_period: "5m"
    pro:
      grace_period: "10m"
    enterprise:
      grace_period: "30m"
      allow_disable: true
```

#### Suspension Process

```python
class ClusterSuspensionManager:
    """Manages the suspension of idle serverless clusters."""

    def evaluate_suspension(self, cluster_id: str) -> SuspensionDecision:
        """
        Evaluate whether a cluster should be suspended.
        """
        cluster = self.get_cluster(cluster_id)
        metrics = self.metrics_service.get_cluster_metrics(cluster_id)

        # Check if cluster meets idle criteria
        if not self.is_cluster_idle(metrics):
            return SuspensionDecision(action="keep_running", reason="Active workload")

        # Check exclusions
        if cluster.has_active_replication_slots():
            return SuspensionDecision(action="keep_running", reason="Active replication slots")

        if cluster.is_in_maintenance_window():
            return SuspensionDecision(action="keep_running", reason="Maintenance window")

        if cluster.explicit_keep_alive:
            return SuspensionDecision(action="keep_running", reason="Explicit keep-alive")

        # Check grace period
        idle_duration = metrics.idle_duration_seconds
        grace_period = self.get_grace_period(cluster.tier)

        if idle_duration < grace_period:
            return SuspensionDecision(
                action="pending",
                reason=f"Grace period: {grace_period - idle_duration}s remaining"
            )

        return SuspensionDecision(
            action="suspend",
            reason=f"Idle for {idle_duration}s (threshold: {grace_period}s)"
        )

    async def suspend_cluster(self, cluster_id: str):
        """
        Execute cluster suspension with checkpoint and state preservation.
        """
        cluster = self.get_cluster(cluster_id)

        # 1. Announce pending suspension
        await self.publish_event(ClusterSuspendingEvent(cluster_id))

        # 2. Stop accepting new connections
        await self.connection_router.drain_cluster(cluster_id, timeout=30)

        # 3. Checkpoint PostgreSQL
        await cluster.execute_checkpoint()

        # 4. Archive WAL to cold storage
        await cluster.archive_wal()

        # 5. Snapshot state metadata
        state = ClusterState(
            cluster_id=cluster_id,
            last_lsn=cluster.get_current_lsn(),
            pg_version=cluster.pg_version,
            extensions=cluster.installed_extensions,
            config=cluster.get_config_snapshot(),
            suspended_at=datetime.utcnow()
        )
        await self.state_store.save(state)

        # 6. Terminate compute pod
        await self.k8s_client.delete_pod(cluster.compute_pod_name)

        # 7. Update cluster status
        await self.update_cluster_status(cluster_id, "suspended")

        # 8. Publish suspension complete event
        await self.publish_event(ClusterSuspendedEvent(cluster_id, state))
```

### 4.3 Cold Storage State Preservation

```yaml
cold_storage:
  storage_backend: "s3"
  bucket_pattern: "orochi-suspended-{region}"

  preserved_state:
    # Always preserved
    required:
      - "pg_data/"          # Data directory snapshot
      - "pg_wal/"           # Write-ahead logs
      - "cluster_state.json" # Metadata and config
      - "extensions/"       # Custom extension state

    # Optionally preserved based on tier
    optional:
      - "pg_stat_statements" # Query statistics
      - "connection_state"   # For session persistence

  compression:
    algorithm: "zstd"
    level: 3

  encryption:
    enabled: true
    key_source: "kms"  # AWS KMS, GCP KMS, etc.

  retention:
    suspended_clusters: "90d"
    deleted_clusters: "7d"  # Soft-delete recovery window
```

### 4.4 Wake-Up Latency Targets

| Component | Target Latency | P99 Latency | Notes |
|-----------|----------------|-------------|-------|
| Connection Router Decision | 10ms | 50ms | SNI parsing + state lookup |
| Pod Scheduling | 500ms | 2s | Depends on node availability |
| Container Start | 1s | 3s | Pre-pulled images |
| PostgreSQL Start | 2s | 5s | Shared buffer allocation |
| State Restore | 1s | 5s | Depends on data size |
| WAL Recovery | 500ms | 2s | Minimal WAL for clean shutdown |
| Connection Ready | 500ms | 1s | First query capability |
| **Total Cold Start** | **5s** | **15s** | End-to-end |

#### Wake-Up Optimization Strategies

```yaml
wake_up_optimization:
  # Pre-warming strategies
  pod_pre_warming:
    enabled: true
    warm_pool_size: 10  # Per region
    instance_types:
      - "small"   # Most common
      - "medium"
    max_wait_time: "30s"

  # Predictive wake-up
  predictive_wake:
    enabled: true
    model: "time_series_forecast"
    lookahead_minutes: 5
    confidence_threshold: 0.8

  # Connection proxy buffering
  connection_buffering:
    enabled: true
    max_wait_time: "30s"
    retry_interval: "100ms"
    client_timeout_response: "Server is starting, please wait..."

  # Lazy state loading
  lazy_loading:
    enabled: true
    essential_tables_first: true
    background_prefetch: true
```

---

## 5. Metrics and Triggers

### 5.1 Core Metrics Collection

```yaml
metrics_collection:
  sources:
    postgresql:
      - name: "pg_stat_activity"
        metrics:
          - "active_connections"
          - "idle_connections"
          - "waiting_connections"
          - "active_transactions"
        interval: "10s"

      - name: "pg_stat_database"
        metrics:
          - "xact_commit"
          - "xact_rollback"
          - "blks_read"
          - "blks_hit"
          - "tup_returned"
          - "tup_fetched"
          - "tup_inserted"
          - "tup_updated"
          - "tup_deleted"
        interval: "15s"

      - name: "pg_stat_statements"
        metrics:
          - "total_exec_time"
          - "calls"
          - "rows"
          - "mean_exec_time"
        interval: "30s"

    pgbouncer:
      - name: "pgbouncer_pools"
        metrics:
          - "cl_active"
          - "cl_waiting"
          - "sv_active"
          - "sv_idle"
          - "maxwait"
        interval: "10s"

    system:
      - name: "node_exporter"
        metrics:
          - "cpu_usage_percent"
          - "memory_usage_percent"
          - "disk_usage_percent"
          - "disk_iops"
          - "network_bytes_in"
          - "network_bytes_out"
        interval: "15s"

    kubernetes:
      - name: "kube_state_metrics"
        metrics:
          - "container_cpu_usage"
          - "container_memory_usage"
          - "pod_status"
        interval: "15s"
```

### 5.2 Scaling Trigger Definitions

```yaml
scaling_triggers:
  cpu_utilization:
    metric: "container_cpu_usage_percent"
    thresholds:
      scale_up: 80
      scale_down: 30
    evaluation_period: "5m"
    consecutive_breaches: 2

  memory_pressure:
    metric: "container_memory_usage_percent"
    thresholds:
      scale_up: 85
      scale_down: 40
    evaluation_period: "5m"
    consecutive_breaches: 2

  connection_count:
    metric: "active_connections / max_connections * 100"
    thresholds:
      scale_up: 80
      scale_down: 20
    evaluation_period: "2m"
    consecutive_breaches: 3

  query_latency_p99:
    metric: "pg_query_latency_p99_ms"
    thresholds:
      scale_up: 500  # ms
      scale_down: 50
    evaluation_period: "5m"
    consecutive_breaches: 2

  storage_utilization:
    metric: "disk_usage_percent"
    thresholds:
      scale_up: 80  # Expand storage
      critical: 90  # Urgent expansion
    evaluation_period: "1m"
    consecutive_breaches: 1

  queries_per_second:
    metric: "rate(pg_stat_statements_calls[1m])"
    thresholds:
      scale_up_replicas: 1000  # Add read replica
      scale_down_replicas: 200
    evaluation_period: "5m"
    consecutive_breaches: 3
```

### 5.3 Composite Triggers

```yaml
composite_triggers:
  high_load_alert:
    name: "High Load - Scale Up Required"
    logic: "ANY"
    conditions:
      - trigger: "cpu_utilization.scale_up"
      - trigger: "memory_pressure.scale_up"
      - trigger: "connection_count.scale_up"
    action: "vertical_scale_up"
    priority: "high"

  sustained_high_latency:
    name: "Sustained High Latency"
    logic: "ALL"
    conditions:
      - trigger: "query_latency_p99.scale_up"
      - condition: "cpu_utilization > 60"  # Not just CPU-bound
    action: "investigate_and_scale"
    priority: "medium"

  read_heavy_workload:
    name: "Read-Heavy Workload - Add Replicas"
    logic: "ALL"
    conditions:
      - trigger: "queries_per_second.scale_up_replicas"
      - condition: "read_write_ratio > 5"  # 5:1 reads to writes
    action: "add_read_replica"
    priority: "medium"

  idle_cluster:
    name: "Idle Cluster - Scale to Zero"
    logic: "ALL"
    conditions:
      - condition: "active_connections == 0"
        duration: "5m"
      - condition: "queries_per_second < 0.1"
        duration: "5m"
    action: "suspend_cluster"
    priority: "low"
```

### 5.4 Predictive Scaling (Time-Based Patterns)

```yaml
predictive_scaling:
  enabled: true

  model:
    type: "prophet"  # Facebook Prophet for time-series forecasting
    training_data: "30d"
    forecast_horizon: "1h"
    update_frequency: "1h"

  patterns:
    daily:
      enabled: true
      min_samples: 7  # Need 7 days of data

    weekly:
      enabled: true
      min_samples: 4  # Need 4 weeks of data

    custom_events:
      enabled: true
      events:
        - name: "monthly_billing"
          pattern: "0 0 1 * *"  # First of month
          expected_increase: 200  # percent

        - name: "black_friday"
          pattern: "specific_dates"
          dates: ["2026-11-27", "2026-11-28"]
          expected_increase: 500

  actions:
    proactive_scale_up:
      lookahead: "15m"
      confidence_threshold: 0.75
      buffer_percent: 20  # Scale 20% more than predicted

    proactive_scale_down:
      lookahead: "30m"
      confidence_threshold: 0.85
      delay_minutes: 15  # Wait before scaling down
```

#### Predictive Model Implementation

```python
class PredictiveScaler:
    """
    Implements predictive scaling using time-series forecasting.
    """

    def __init__(self, config: PredictiveConfig):
        self.config = config
        self.model = Prophet(
            yearly_seasonality=False,
            weekly_seasonality=True,
            daily_seasonality=True,
            changepoint_prior_scale=0.05
        )

    def train(self, cluster_id: str):
        """Train the model on historical metrics."""
        history = self.metrics_store.get_history(
            cluster_id=cluster_id,
            metric="cpu_utilization",
            duration=self.config.training_data
        )

        df = pd.DataFrame({
            'ds': history.timestamps,
            'y': history.values
        })

        # Add custom regressors for known events
        for event in self.config.custom_events:
            df[event.name] = self._create_event_regressor(df, event)
            self.model.add_regressor(event.name)

        self.model.fit(df)

    def predict(self, cluster_id: str, horizon_minutes: int = 60) -> Forecast:
        """Generate scaling predictions."""
        future = self.model.make_future_dataframe(
            periods=horizon_minutes,
            freq='min'
        )

        # Add event regressors to future
        for event in self.config.custom_events:
            future[event.name] = self._create_event_regressor(future, event)

        forecast = self.model.predict(future)

        return Forecast(
            timestamps=forecast['ds'].tolist(),
            predicted=forecast['yhat'].tolist(),
            lower_bound=forecast['yhat_lower'].tolist(),
            upper_bound=forecast['yhat_upper'].tolist()
        )

    def get_scaling_recommendation(self, cluster_id: str) -> ScalingRecommendation:
        """Generate proactive scaling recommendation."""
        forecast = self.predict(cluster_id, self.config.forecast_horizon)
        current_capacity = self.get_current_capacity(cluster_id)

        # Find peak in forecast window
        peak_load = max(forecast.upper_bound[:self.config.lookahead_minutes])
        peak_time = forecast.timestamps[forecast.upper_bound.index(peak_load)]

        # Calculate required capacity with buffer
        required_capacity = peak_load * (1 + self.config.buffer_percent / 100)

        if required_capacity > current_capacity:
            return ScalingRecommendation(
                action="proactive_scale_up",
                target_capacity=required_capacity,
                execute_at=peak_time - timedelta(minutes=self.config.lookahead_minutes),
                confidence=self._calculate_confidence(forecast),
                reason=f"Predicted peak of {peak_load}% at {peak_time}"
            )

        return ScalingRecommendation(action="no_action")
```

---

## 6. Scaling Algorithms

### 6.1 Main Scaling Decision Engine

```python
class ScalingDecisionEngine:
    """
    Central decision engine for all scaling operations.
    Coordinates vertical, horizontal, storage, and pool scaling.
    """

    def __init__(self, config: ScalingConfig):
        self.config = config
        self.vertical_scaler = VerticalScaler(config.vertical)
        self.horizontal_scaler = HorizontalScaler(config.horizontal)
        self.storage_scaler = StorageScaler(config.storage)
        self.pool_scaler = PoolScaler(config.pool)
        self.predictive_scaler = PredictiveScaler(config.predictive)
        self.cooldown_manager = CooldownManager()

    async def evaluate(self, cluster_id: str) -> List[ScalingDecision]:
        """
        Evaluate all scaling dimensions and return prioritized decisions.
        """
        decisions = []

        # Gather current state and metrics
        cluster = await self.get_cluster(cluster_id)
        metrics = await self.get_metrics(cluster_id)
        policy = await self.get_scaling_policy(cluster_id)

        # Check cooldowns
        cooldowns = self.cooldown_manager.get_active_cooldowns(cluster_id)

        # 1. Evaluate predictive scaling first (proactive)
        if policy.predictive_enabled and 'predictive' not in cooldowns:
            predictive = self.predictive_scaler.get_scaling_recommendation(cluster_id)
            if predictive.action != "no_action" and predictive.confidence >= policy.confidence_threshold:
                decisions.append(predictive)

        # 2. Evaluate reactive triggers
        triggers = self.evaluate_triggers(metrics, policy)

        # 3. Vertical scaling evaluation
        if 'vertical' not in cooldowns:
            vertical = self.vertical_scaler.evaluate(metrics, cluster, policy)
            if vertical.action != "no_action":
                decisions.append(vertical)

        # 4. Horizontal scaling evaluation
        if 'horizontal' not in cooldowns:
            horizontal = self.horizontal_scaler.evaluate(metrics, cluster, policy)
            if horizontal.action != "no_action":
                decisions.append(horizontal)

        # 5. Storage scaling (always evaluate - never has cooldown)
        storage = self.storage_scaler.evaluate(metrics, cluster, policy)
        if storage.action != "no_action":
            decisions.append(storage)

        # 6. Pool scaling evaluation
        if 'pool' not in cooldowns:
            pool = self.pool_scaler.evaluate(metrics, cluster, policy)
            if pool.action != "no_action":
                decisions.append(pool)

        # 7. Prioritize and deduplicate decisions
        decisions = self.prioritize_decisions(decisions)

        # 8. Validate against budget and tier limits
        decisions = self.validate_decisions(decisions, cluster, policy)

        return decisions

    def prioritize_decisions(self, decisions: List[ScalingDecision]) -> List[ScalingDecision]:
        """
        Prioritize scaling decisions based on urgency and impact.

        Priority order:
        1. Storage expansion (data safety)
        2. Scale-up operations (service quality)
        3. Proactive scaling (efficiency)
        4. Scale-down operations (cost savings)
        """
        priority_map = {
            'storage_expand': 1,
            'vertical_scale_up': 2,
            'horizontal_scale_up': 2,
            'pool_scale_up': 3,
            'proactive_scale_up': 4,
            'vertical_scale_down': 5,
            'horizontal_scale_down': 5,
            'pool_scale_down': 5,
        }

        return sorted(decisions, key=lambda d: priority_map.get(d.action, 10))

    def validate_decisions(
        self,
        decisions: List[ScalingDecision],
        cluster: Cluster,
        policy: ScalingPolicy
    ) -> List[ScalingDecision]:
        """Validate decisions against constraints."""
        validated = []

        for decision in decisions:
            # Check tier limits
            if not self.is_within_tier_limits(decision, cluster.tier):
                decision.status = "rejected"
                decision.reason = f"Exceeds tier limits for {cluster.tier}"
                continue

            # Check budget limits
            projected_cost = self.calculate_cost_impact(decision, cluster)
            if not self.is_within_budget(projected_cost, cluster.org_id):
                decision.status = "rejected"
                decision.reason = "Exceeds organization budget"
                continue

            # Check maintenance windows for disruptive operations
            if decision.is_disruptive and cluster.is_in_protected_window():
                decision.status = "deferred"
                decision.execute_after = cluster.get_next_maintenance_window()

            validated.append(decision)

        return validated
```

### 6.2 Scaling State Machine

```
                    SCALING STATE MACHINE

    +----------+     trigger     +-----------+
    |  STABLE  |---------------->| EVALUATING|
    +----+-----+                 +-----+-----+
         ^                             |
         |                    decision |
         |                             v
         |                       +-----+-----+
         |         no_action     |  DECIDING |
         +<----------------------+-----+-----+
         |                             |
         |                    scale    |
         |                             v
         |                       +-----+-----+
         |                       | PREPARING |
         |                       +-----+-----+
         |                             |
         |                             v
         |                       +-----+-----+
         |                       | EXECUTING |
         |                       +-----+-----+
         |                             |
         |              +--------------+--------------+
         |              |                             |
         |              v                             v
         |        +-----+-----+                 +-----+-----+
         |        |  SUCCESS  |                 |  FAILED   |
         |        +-----+-----+                 +-----+-----+
         |              |                             |
         |   cooldown   |                    retry?   |
         |              v                             v
         |        +-----+-----+                 +-----+-----+
         +--------| COOLDOWN  |                 | ROLLBACK  |
                  +-----------+                 +-----------+
                                                      |
                                                      v
                                               +-----+-----+
                                               |  STABLE   |
                                               |  (orig)   |
                                               +-----------+
```

### 6.3 Cooldown Management

```python
class CooldownManager:
    """
    Manages cooldown periods between scaling operations.
    Prevents scaling oscillation and allows system stabilization.
    """

    DEFAULT_COOLDOWNS = {
        'vertical_scale_up': timedelta(minutes=5),
        'vertical_scale_down': timedelta(minutes=15),
        'horizontal_scale_up': timedelta(minutes=3),
        'horizontal_scale_down': timedelta(minutes=10),
        'pool_scale_up': timedelta(minutes=1),
        'pool_scale_down': timedelta(minutes=5),
        'predictive': timedelta(minutes=10),
    }

    def __init__(self, redis_client):
        self.redis = redis_client

    def start_cooldown(self, cluster_id: str, operation_type: str, duration: timedelta = None):
        """Start a cooldown period for a scaling operation."""
        if duration is None:
            duration = self.DEFAULT_COOLDOWNS.get(operation_type, timedelta(minutes=5))

        key = f"cooldown:{cluster_id}:{operation_type}"
        self.redis.setex(key, duration, "1")

    def is_in_cooldown(self, cluster_id: str, operation_type: str) -> bool:
        """Check if an operation type is in cooldown."""
        key = f"cooldown:{cluster_id}:{operation_type}"
        return self.redis.exists(key)

    def get_active_cooldowns(self, cluster_id: str) -> Dict[str, timedelta]:
        """Get all active cooldowns for a cluster."""
        cooldowns = {}
        pattern = f"cooldown:{cluster_id}:*"

        for key in self.redis.scan_iter(pattern):
            operation = key.split(':')[-1]
            ttl = self.redis.ttl(key)
            if ttl > 0:
                cooldowns[operation] = timedelta(seconds=ttl)

        return cooldowns

    def clear_cooldown(self, cluster_id: str, operation_type: str):
        """Clear a cooldown (for emergency scaling)."""
        key = f"cooldown:{cluster_id}:{operation_type}"
        self.redis.delete(key)
```

### 6.4 Burst Traffic Handling

```python
class BurstTrafficHandler:
    """
    Handles sudden traffic spikes that require immediate scaling.
    Bypasses normal evaluation cycles for emergency response.
    """

    BURST_THRESHOLDS = {
        'connection_spike': {
            'metric': 'connection_rate_per_second',
            'threshold': 100,  # New connections per second
            'window': '10s'
        },
        'cpu_spike': {
            'metric': 'cpu_utilization_percent',
            'threshold': 95,
            'window': '30s'
        },
        'memory_spike': {
            'metric': 'memory_utilization_percent',
            'threshold': 95,
            'window': '30s'
        },
        'latency_spike': {
            'metric': 'query_latency_p99_ms',
            'threshold': 2000,  # 2 seconds
            'window': '30s'
        }
    }

    async def detect_and_handle(self, cluster_id: str, metrics: Metrics) -> Optional[EmergencyScaling]:
        """
        Detect burst traffic and trigger emergency scaling if needed.
        """
        for burst_type, config in self.BURST_THRESHOLDS.items():
            value = getattr(metrics, config['metric'])

            if value >= config['threshold']:
                logger.warning(
                    f"Burst detected: {burst_type} for cluster {cluster_id}. "
                    f"Value: {value}, Threshold: {config['threshold']}"
                )

                # Trigger emergency scaling
                return await self.emergency_scale(cluster_id, burst_type, value)

        return None

    async def emergency_scale(
        self,
        cluster_id: str,
        burst_type: str,
        current_value: float
    ) -> EmergencyScaling:
        """
        Execute emergency scaling without normal cooldown/evaluation.
        """
        cluster = await self.get_cluster(cluster_id)

        # Calculate aggressive scaling target
        if burst_type in ['connection_spike', 'cpu_spike', 'memory_spike']:
            # Scale up by 2 tiers
            target_tier = self.get_tier_plus_n(cluster.current_tier, 2)
        else:
            # Scale up by 1 tier
            target_tier = self.get_tier_plus_n(cluster.current_tier, 1)

        # Also add read replicas for connection spikes
        add_replicas = 2 if burst_type == 'connection_spike' else 0

        result = EmergencyScaling(
            cluster_id=cluster_id,
            trigger=burst_type,
            current_tier=cluster.current_tier,
            target_tier=target_tier,
            add_replicas=add_replicas,
            bypass_cooldown=True,
            priority="critical"
        )

        # Execute immediately
        await self.execution_controller.execute_emergency(result)

        # Start shortened cooldown
        self.cooldown_manager.start_cooldown(
            cluster_id,
            'emergency',
            timedelta(minutes=2)
        )

        return result
```

---

## 7. Implementation Approach

### 7.1 Technology Selection

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Metrics Storage** | VictoriaMetrics | High-cardinality, long retention, PromQL compatible |
| **Custom Metrics** | Kubernetes Metrics API | Standard K8s integration |
| **Event-Driven Scaling** | KEDA | Scalers for Prometheus, external metrics |
| **Operator Framework** | Kubebuilder | Go-based, strong typing, well-maintained |
| **State Storage** | etcd (via K8s) | Consistent, built into K8s |
| **Predictive Model** | Prophet + TensorFlow Lite | Time-series forecasting |
| **Connection Proxy** | PgCat (Rust) | Multi-tenant pooling, sharding, wake-on-connect hooks |

### 7.2 Architecture Layers

```
+------------------------------------------------------------------+
|                    IMPLEMENTATION LAYERS                          |
+------------------------------------------------------------------+
|                                                                    |
|  Layer 5: API & Dashboard                                         |
|  +----------------------------------------------------------+    |
|  | REST API | GraphQL | Web Dashboard | CLI                  |    |
|  +----------------------------------------------------------+    |
|                              |                                    |
|  Layer 4: Control Plane Services                                  |
|  +----------------------------------------------------------+    |
|  | Cluster Manager | Scaling Service | Billing Integration   |    |
|  +----------------------------------------------------------+    |
|                              |                                    |
|  Layer 3: Autoscaler Core                                         |
|  +----------------------------------------------------------+    |
|  | Decision Engine | Predictive Model | Policy Evaluator     |    |
|  +----------------------------------------------------------+    |
|                              |                                    |
|  Layer 2: Kubernetes Operator                                     |
|  +----------------------------------------------------------+    |
|  | OrochiCluster CRD | Reconciliation | Status Management    |    |
|  +----------------------------------------------------------+    |
|                              |                                    |
|  Layer 1: Infrastructure                                          |
|  +----------------------------------------------------------+    |
|  | Kubernetes | KEDA | VictoriaMetrics | etcd                |    |
|  +----------------------------------------------------------+    |
|                                                                    |
+------------------------------------------------------------------+
```

### 7.3 CloudNativePG Integration

We extend CloudNativePG (CNPG) operator rather than building from scratch:

```yaml
# CloudNativePG cluster with Orochi extensions
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: orochi-cluster
spec:
  instances: 3

  postgresql:
    parameters:
      shared_preload_libraries: "orochi"
      orochi.default_shard_count: "32"

  # Orochi-specific extensions
  imageName: orochi/postgresql:17-orochi

  # Storage configuration
  storage:
    size: 100Gi
    storageClass: gp3

  # Resources managed by our autoscaler
  resources:
    requests:
      memory: "4Gi"
      cpu: "2"
    limits:
      memory: "8Gi"
      cpu: "4"
```

### 7.4 Custom Autoscaler Controller

```go
// pkg/controller/autoscaler_controller.go

package controller

import (
    "context"
    "time"

    "sigs.k8s.io/controller-runtime/pkg/client"
    "sigs.k8s.io/controller-runtime/pkg/reconcile"

    orochicloudv1 "github.com/orochi-cloud/operator/api/v1"
)

// OrochiAutoscalerReconciler reconciles OrochiCluster scaling
type OrochiAutoscalerReconciler struct {
    client.Client
    MetricsClient    MetricsClient
    DecisionEngine   *DecisionEngine
    ExecutionEngine  *ExecutionEngine
    PredictiveModel  *PredictiveModel
}

func (r *OrochiAutoscalerReconciler) Reconcile(ctx context.Context, req reconcile.Request) (reconcile.Result, error) {
    log := log.FromContext(ctx)

    // 1. Fetch the OrochiCluster
    var cluster orochicloudv1.OrochiCluster
    if err := r.Get(ctx, req.NamespacedName, &cluster); err != nil {
        return reconcile.Result{}, client.IgnoreNotFound(err)
    }

    // 2. Skip if autoscaling disabled
    if !cluster.Spec.Autoscaling.Enabled {
        return reconcile.Result{RequeueAfter: time.Minute}, nil
    }

    // 3. Gather metrics
    metrics, err := r.MetricsClient.GetClusterMetrics(ctx, cluster.Name)
    if err != nil {
        log.Error(err, "Failed to get metrics")
        return reconcile.Result{RequeueAfter: 30 * time.Second}, nil
    }

    // 4. Evaluate scaling decisions
    decisions, err := r.DecisionEngine.Evaluate(ctx, &cluster, metrics)
    if err != nil {
        log.Error(err, "Failed to evaluate scaling")
        return reconcile.Result{RequeueAfter: 30 * time.Second}, nil
    }

    // 5. Execute decisions
    for _, decision := range decisions {
        if decision.Action == "no_action" {
            continue
        }

        log.Info("Executing scaling decision",
            "cluster", cluster.Name,
            "action", decision.Action,
            "reason", decision.Reason,
        )

        if err := r.ExecutionEngine.Execute(ctx, &cluster, decision); err != nil {
            log.Error(err, "Failed to execute scaling",
                "action", decision.Action,
            )
            // Continue with other decisions
            continue
        }

        // Update cluster status
        r.updateClusterStatus(ctx, &cluster, decision)
    }

    // 6. Requeue for next evaluation
    return reconcile.Result{RequeueAfter: r.getEvaluationInterval(&cluster)}, nil
}

func (r *OrochiAutoscalerReconciler) getEvaluationInterval(cluster *orochicloudv1.OrochiCluster) time.Duration {
    // More frequent evaluation for active clusters
    if cluster.Status.Phase == "Running" {
        return 15 * time.Second
    }
    return time.Minute
}
```

---

## 8. Kubernetes Resources

### 8.1 OrochiCluster Custom Resource Definition

```yaml
apiVersion: apiextensions.k8s.io/v1
kind: CustomResourceDefinition
metadata:
  name: orochiclusters.cloud.orochi.io
spec:
  group: cloud.orochi.io
  names:
    kind: OrochiCluster
    listKind: OrochiClusterList
    plural: orochiclusters
    singular: orochicluster
    shortNames:
      - oc
  scope: Namespaced
  versions:
    - name: v1
      served: true
      storage: true
      schema:
        openAPIV3Schema:
          type: object
          properties:
            spec:
              type: object
              properties:
                # Tier configuration
                tier:
                  type: string
                  enum: ["serverless", "dedicated"]
                  default: "serverless"

                # Compute configuration
                compute:
                  type: object
                  properties:
                    class:
                      type: string
                      enum: ["nano", "micro", "small", "medium", "large", "xlarge", "2xlarge", "4xlarge"]
                      default: "small"

                    # Autoscaling configuration
                    autoscaling:
                      type: object
                      properties:
                        enabled:
                          type: boolean
                          default: true
                        minClass:
                          type: string
                          default: "nano"
                        maxClass:
                          type: string
                          default: "xlarge"

                        # Scaling thresholds
                        scaleUpCpuThreshold:
                          type: integer
                          minimum: 50
                          maximum: 95
                          default: 80
                        scaleDownCpuThreshold:
                          type: integer
                          minimum: 10
                          maximum: 50
                          default: 30
                        scaleUpMemoryThreshold:
                          type: integer
                          minimum: 50
                          maximum: 95
                          default: 85
                        scaleDownMemoryThreshold:
                          type: integer
                          minimum: 10
                          maximum: 50
                          default: 40

                        # Cooldown periods
                        scaleUpCooldown:
                          type: string
                          default: "5m"
                        scaleDownCooldown:
                          type: string
                          default: "15m"

                        # Predictive scaling
                        predictiveScaling:
                          type: object
                          properties:
                            enabled:
                              type: boolean
                              default: false
                            lookaheadMinutes:
                              type: integer
                              default: 15
                            confidenceThreshold:
                              type: number
                              default: 0.75

                # Read replicas
                replicas:
                  type: object
                  properties:
                    count:
                      type: integer
                      minimum: 0
                      maximum: 15
                      default: 0
                    autoscaling:
                      type: object
                      properties:
                        enabled:
                          type: boolean
                          default: false
                        minReplicas:
                          type: integer
                          default: 0
                        maxReplicas:
                          type: integer
                          default: 5
                        targetQpsPerReplica:
                          type: integer
                          default: 1000

                # Storage configuration
                storage:
                  type: object
                  properties:
                    size:
                      type: string
                      default: "10Gi"
                    class:
                      type: string
                      default: "gp3"
                    autoExpand:
                      type: object
                      properties:
                        enabled:
                          type: boolean
                          default: true
                        thresholdPercent:
                          type: integer
                          default: 80
                        incrementPercent:
                          type: integer
                          default: 25
                        maxSize:
                          type: string
                          default: "1Ti"

                # Serverless specific
                serverless:
                  type: object
                  properties:
                    scaleToZero:
                      type: object
                      properties:
                        enabled:
                          type: boolean
                          default: true
                        idleTimeout:
                          type: string
                          default: "10m"
                        warmPoolSize:
                          type: integer
                          default: 2

                # Connection pooling
                connectionPooling:
                  type: object
                  properties:
                    enabled:
                      type: boolean
                      default: true
                    mode:
                      type: string
                      enum: ["session", "transaction", "statement"]
                      default: "transaction"
                    poolSize:
                      type: integer
                      default: 50
                    autoscaling:
                      type: object
                      properties:
                        enabled:
                          type: boolean
                          default: true
                        minSize:
                          type: integer
                          default: 10
                        maxSize:
                          type: integer
                          default: 500

            status:
              type: object
              properties:
                phase:
                  type: string
                endpoint:
                  type: string
                currentClass:
                  type: string
                currentReplicas:
                  type: integer
                currentStorageSize:
                  type: string
                lastScalingEvent:
                  type: object
                  properties:
                    type:
                      type: string
                    timestamp:
                      type: string
                    reason:
                      type: string
                conditions:
                  type: array
                  items:
                    type: object
                    properties:
                      type:
                        type: string
                      status:
                        type: string
                      lastTransitionTime:
                        type: string
                      reason:
                        type: string
                      message:
                        type: string
```

### 8.2 Example OrochiCluster Instance

```yaml
apiVersion: cloud.orochi.io/v1
kind: OrochiCluster
metadata:
  name: production-db
  namespace: tenant-acme
  labels:
    orochi.io/org: acme-corp
    orochi.io/project: main-app
spec:
  tier: dedicated

  compute:
    class: medium
    autoscaling:
      enabled: true
      minClass: small
      maxClass: 2xlarge
      scaleUpCpuThreshold: 75
      scaleDownCpuThreshold: 25
      scaleUpMemoryThreshold: 80
      scaleDownMemoryThreshold: 35
      scaleUpCooldown: "3m"
      scaleDownCooldown: "20m"
      predictiveScaling:
        enabled: true
        lookaheadMinutes: 30
        confidenceThreshold: 0.8

  replicas:
    count: 1
    autoscaling:
      enabled: true
      minReplicas: 0
      maxReplicas: 5
      targetQpsPerReplica: 800

  storage:
    size: 100Gi
    class: gp3
    autoExpand:
      enabled: true
      thresholdPercent: 75
      incrementPercent: 50
      maxSize: 2Ti

  connectionPooling:
    enabled: true
    mode: transaction
    poolSize: 100
    autoscaling:
      enabled: true
      minSize: 20
      maxSize: 300
```

### 8.3 KEDA ScaledObject for Custom Metrics

```yaml
apiVersion: keda.sh/v1alpha1
kind: ScaledObject
metadata:
  name: orochi-cluster-scaler
  namespace: tenant-acme
spec:
  scaleTargetRef:
    apiVersion: cloud.orochi.io/v1
    kind: OrochiCluster
    name: production-db

  pollingInterval: 15
  cooldownPeriod: 300
  minReplicaCount: 1
  maxReplicaCount: 10

  triggers:
    # CPU-based scaling
    - type: prometheus
      metadata:
        serverAddress: http://victoria-metrics:8428
        query: |
          avg(container_cpu_usage_seconds_total{
            pod=~"production-db-.*",
            namespace="tenant-acme"
          }) by (pod) * 100
        threshold: "80"

    # Connection-based scaling
    - type: prometheus
      metadata:
        serverAddress: http://victoria-metrics:8428
        query: |
          sum(pg_stat_activity_count{
            datname="production",
            state="active"
          })
        threshold: "150"

    # Query latency scaling
    - type: prometheus
      metadata:
        serverAddress: http://victoria-metrics:8428
        query: |
          histogram_quantile(0.99,
            rate(pg_query_duration_seconds_bucket{
              datname="production"
            }[5m])
          ) * 1000
        threshold: "500"
```

### 8.4 Horizontal Pod Autoscaler (Fallback)

```yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: orochi-replica-hpa
  namespace: tenant-acme
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: StatefulSet
    name: production-db-replicas

  minReplicas: 1
  maxReplicas: 5

  metrics:
    - type: Resource
      resource:
        name: cpu
        target:
          type: Utilization
          averageUtilization: 70

    - type: Resource
      resource:
        name: memory
        target:
          type: Utilization
          averageUtilization: 80

    - type: External
      external:
        metric:
          name: pg_queries_per_second
          selector:
            matchLabels:
              cluster: production-db
        target:
          type: AverageValue
          averageValue: 1000

  behavior:
    scaleUp:
      stabilizationWindowSeconds: 60
      policies:
        - type: Pods
          value: 2
          periodSeconds: 60
        - type: Percent
          value: 50
          periodSeconds: 60
      selectPolicy: Max

    scaleDown:
      stabilizationWindowSeconds: 300
      policies:
        - type: Pods
          value: 1
          periodSeconds: 120
      selectPolicy: Min
```

### 8.5 Vertical Pod Autoscaler

```yaml
apiVersion: autoscaling.k8s.io/v1
kind: VerticalPodAutoscaler
metadata:
  name: orochi-primary-vpa
  namespace: tenant-acme
spec:
  targetRef:
    apiVersion: apps/v1
    kind: StatefulSet
    name: production-db-primary

  updatePolicy:
    updateMode: "Off"  # Recommendations only - we handle execution

  resourcePolicy:
    containerPolicies:
      - containerName: postgresql
        minAllowed:
          cpu: 500m
          memory: 1Gi
        maxAllowed:
          cpu: 32
          memory: 128Gi
        controlledResources:
          - cpu
          - memory
        controlledValues: RequestsAndLimits
```

### 8.6 PodDisruptionBudget for Safe Scaling

```yaml
apiVersion: policy/v1
kind: PodDisruptionBudget
metadata:
  name: orochi-cluster-pdb
  namespace: tenant-acme
spec:
  minAvailable: 1
  selector:
    matchLabels:
      orochi.io/cluster: production-db
      orochi.io/role: primary
---
apiVersion: policy/v1
kind: PodDisruptionBudget
metadata:
  name: orochi-replicas-pdb
  namespace: tenant-acme
spec:
  maxUnavailable: 1
  selector:
    matchLabels:
      orochi.io/cluster: production-db
      orochi.io/role: replica
```

---

## 9. Wake-Up Mechanism

### 9.1 Connection Router Architecture

```
+------------------------------------------------------------------+
|                  CONNECTION ROUTER ARCHITECTURE                    |
+------------------------------------------------------------------+
|                                                                    |
|  Client Connection (TLS + SNI)                                    |
|  SNI: cluster-abc123.us-east-1.db.orochi.cloud                    |
|       |                                                            |
|       v                                                            |
|  +---------------------------+                                     |
|  |    TCP Load Balancer      |  (AWS NLB / GCP TCP LB)            |
|  +-------------+-------------+                                     |
|                |                                                   |
|                v                                                   |
|  +---------------------------+                                     |
|  |   Connection Router Pod   |  (Always running, HA)              |
|  |   +-------------------+   |                                     |
|  |   | SNI Parser        |   |                                     |
|  |   +-------------------+   |                                     |
|  |   | Cluster Resolver  |   |  --> etcd/Redis: cluster state     |
|  |   +-------------------+   |                                     |
|  |   | Wake-Up Trigger   |   |  --> Kubernetes API                |
|  |   +-------------------+   |                                     |
|  |   | Connection Buffer |   |  (Hold during wake-up)             |
|  |   +-------------------+   |                                     |
|  |   | Backend Router    |   |                                     |
|  |   +-------------------+   |                                     |
|  +-------------+-------------+                                     |
|                |                                                   |
|       +--------+--------+                                          |
|       |                 |                                          |
|       v                 v                                          |
|  +---------+      +------------+                                   |
|  | Running |      | Suspended  |                                   |
|  | Cluster |      | (Wake-Up)  |                                   |
|  +---------+      +------------+                                   |
|                                                                    |
+------------------------------------------------------------------+
```

### 9.2 Wake-Up Sequence Diagram

```
Client          Router          StateStore      K8s API         Compute Pod
  |                |                |               |                |
  |  TLS Hello     |                |               |                |
  |  (SNI: abc)    |                |               |                |
  |--------------->|                |               |                |
  |                |                |               |                |
  |                | Get State(abc) |               |                |
  |                |--------------->|               |                |
  |                |<---------------|               |                |
  |                | state=suspended|               |                |
  |                |                |               |                |
  |                | Wake Request   |               |                |
  |                |-------------------------------->|                |
  |                |                |               |                |
  |                |                |               | Create Pod     |
  |                |                |               |--------------->|
  |                |                |               |                |
  |                | Buffer Connection              |                |
  |                | (client waits) |               |                |
  |                |                |               |                |
  |                |                |               | Pod Ready      |
  |                |                |               |<---------------|
  |                |                |               |                |
  |                | State Update   |               |                |
  |                |<---------------|               |                |
  |                | state=running  |               |                |
  |                |                |               |                |
  |                | Route Connection               |                |
  |                |----------------------------------------------->|
  |                |                |               |                |
  |  Connection    |                |               |                |
  |  Established   |                |               |                |
  |<---------------|                |               |                |
  |                |                |               |                |
```

### 9.3 Connection Router Implementation

```go
// pkg/router/connection_router.go

package router

import (
    "context"
    "crypto/tls"
    "net"
    "sync"
    "time"
)

type ConnectionRouter struct {
    stateStore    StateStore
    wakeUpService WakeUpService
    backendPool   *BackendPool

    // Connection buffering during wake-up
    pendingConns  map[string]*ConnectionBuffer
    pendingMu     sync.RWMutex

    config RouterConfig
}

type RouterConfig struct {
    ListenAddr          string
    TLSConfig           *tls.Config
    WakeUpTimeout       time.Duration // Max time to wait for wake-up
    ConnectionTimeout   time.Duration // Client connection timeout
    HealthCheckInterval time.Duration
}

func (r *ConnectionRouter) handleConnection(conn net.Conn) {
    defer conn.Close()

    // 1. TLS handshake to get SNI
    tlsConn, ok := conn.(*tls.Conn)
    if !ok {
        return
    }

    if err := tlsConn.Handshake(); err != nil {
        return
    }

    sni := tlsConn.ConnectionState().ServerName
    clusterID := r.parseClusterID(sni)

    // 2. Get cluster state
    state, err := r.stateStore.GetClusterState(clusterID)
    if err != nil {
        r.sendError(tlsConn, "Cluster not found")
        return
    }

    // 3. Handle based on state
    switch state.Status {
    case ClusterRunning:
        r.routeToBackend(tlsConn, state.Endpoint)

    case ClusterSuspended:
        r.handleSuspendedCluster(tlsConn, clusterID, state)

    case ClusterWakingUp:
        r.waitAndRoute(tlsConn, clusterID, state)

    default:
        r.sendError(tlsConn, "Cluster unavailable")
    }
}

func (r *ConnectionRouter) handleSuspendedCluster(
    conn *tls.Conn,
    clusterID string,
    state *ClusterState,
) {
    // 1. Initiate wake-up
    ctx, cancel := context.WithTimeout(context.Background(), r.config.WakeUpTimeout)
    defer cancel()

    wakeupStarted, err := r.wakeUpService.InitiateWakeUp(ctx, clusterID)
    if err != nil {
        r.sendError(conn, "Failed to wake cluster: "+err.Error())
        return
    }

    // 2. Send keepalive to client
    go r.sendKeepalive(conn, "Cluster is starting...")

    // 3. Wait for cluster to be ready
    if wakeupStarted {
        r.waitAndRoute(conn, clusterID, state)
    }
}

func (r *ConnectionRouter) waitAndRoute(
    conn *tls.Conn,
    clusterID string,
    state *ClusterState,
) {
    // 1. Subscribe to state updates
    updates := r.stateStore.WatchCluster(clusterID)

    // 2. Wait for running state
    timeout := time.NewTimer(r.config.WakeUpTimeout)
    defer timeout.Stop()

    for {
        select {
        case update := <-updates:
            if update.Status == ClusterRunning {
                // Route to backend
                r.routeToBackend(conn, update.Endpoint)
                return
            }
            if update.Status == ClusterFailed {
                r.sendError(conn, "Cluster failed to start")
                return
            }

        case <-timeout.C:
            r.sendError(conn, "Cluster wake-up timeout")
            return
        }
    }
}

func (r *ConnectionRouter) routeToBackend(clientConn *tls.Conn, endpoint string) {
    // Connect to backend PostgreSQL
    backendConn, err := r.backendPool.GetConnection(endpoint)
    if err != nil {
        r.sendError(clientConn, "Backend connection failed")
        return
    }
    defer backendConn.Close()

    // Bidirectional proxy
    var wg sync.WaitGroup
    wg.Add(2)

    go func() {
        defer wg.Done()
        io.Copy(backendConn, clientConn)
    }()

    go func() {
        defer wg.Done()
        io.Copy(clientConn, backendConn)
    }()

    wg.Wait()
}
```

### 9.4 Wake-Up Service

```go
// pkg/wakeup/service.go

package wakeup

import (
    "context"
    "fmt"
    "time"

    corev1 "k8s.io/api/core/v1"
    metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
    "sigs.k8s.io/controller-runtime/pkg/client"
)

type WakeUpService struct {
    k8sClient   client.Client
    stateStore  StateStore
    warmPool    *WarmPool

    config WakeUpConfig
}

type WakeUpConfig struct {
    DefaultImage        string
    WarmPoolEnabled     bool
    MaxConcurrentWakeUps int
    WakeUpTimeout       time.Duration
}

func (s *WakeUpService) InitiateWakeUp(ctx context.Context, clusterID string) (bool, error) {
    // 1. Check if already waking up (idempotent)
    state, err := s.stateStore.GetClusterState(clusterID)
    if err != nil {
        return false, err
    }

    if state.Status == ClusterWakingUp {
        return false, nil // Already in progress
    }

    if state.Status == ClusterRunning {
        return false, nil // Already running
    }

    // 2. Try to acquire from warm pool first
    if s.config.WarmPoolEnabled {
        if pod, ok := s.warmPool.Acquire(state.InstanceClass); ok {
            return s.attachWarmPod(ctx, clusterID, pod, state)
        }
    }

    // 3. Create new compute pod
    return s.createComputePod(ctx, clusterID, state)
}

func (s *WakeUpService) createComputePod(
    ctx context.Context,
    clusterID string,
    state *ClusterState,
) (bool, error) {
    // 1. Update state to waking up
    state.Status = ClusterWakingUp
    state.WakeUpStarted = time.Now()
    s.stateStore.UpdateClusterState(clusterID, state)

    // 2. Create the pod
    pod := s.buildComputePod(clusterID, state)
    if err := s.k8sClient.Create(ctx, pod); err != nil {
        state.Status = ClusterSuspended
        s.stateStore.UpdateClusterState(clusterID, state)
        return false, fmt.Errorf("failed to create pod: %w", err)
    }

    // 3. Watch for pod readiness
    go s.watchPodReadiness(ctx, clusterID, pod.Name)

    return true, nil
}

func (s *WakeUpService) buildComputePod(clusterID string, state *ClusterState) *corev1.Pod {
    resources := s.getResourcesForClass(state.InstanceClass)

    return &corev1.Pod{
        ObjectMeta: metav1.ObjectMeta{
            Name:      fmt.Sprintf("orochi-%s", clusterID),
            Namespace: state.Namespace,
            Labels: map[string]string{
                "orochi.io/cluster":   clusterID,
                "orochi.io/component": "compute",
            },
        },
        Spec: corev1.PodSpec{
            Containers: []corev1.Container{
                {
                    Name:  "postgresql",
                    Image: s.config.DefaultImage,
                    Resources: resources,
                    Env: []corev1.EnvVar{
                        {Name: "OROCHI_CLUSTER_ID", Value: clusterID},
                        {Name: "OROCHI_STATE_BUCKET", Value: state.StateBucket},
                        {Name: "OROCHI_RESTORE_LSN", Value: state.LastLSN},
                    },
                    Ports: []corev1.ContainerPort{
                        {Name: "postgresql", ContainerPort: 5432},
                    },
                    ReadinessProbe: &corev1.Probe{
                        ProbeHandler: corev1.ProbeHandler{
                            Exec: &corev1.ExecAction{
                                Command: []string{
                                    "pg_isready", "-U", "postgres",
                                },
                            },
                        },
                        InitialDelaySeconds: 2,
                        PeriodSeconds:       1,
                        SuccessThreshold:    1,
                        FailureThreshold:    30,
                    },
                    VolumeMounts: []corev1.VolumeMount{
                        {Name: "data", MountPath: "/var/lib/postgresql/data"},
                    },
                },
            },
            InitContainers: []corev1.Container{
                {
                    Name:  "state-restore",
                    Image: "orochi/state-restorer:latest",
                    Env: []corev1.EnvVar{
                        {Name: "S3_BUCKET", Value: state.StateBucket},
                        {Name: "CLUSTER_ID", Value: clusterID},
                        {Name: "TARGET_LSN", Value: state.LastLSN},
                    },
                    VolumeMounts: []corev1.VolumeMount{
                        {Name: "data", MountPath: "/var/lib/postgresql/data"},
                    },
                },
            },
            Volumes: []corev1.Volume{
                {
                    Name: "data",
                    VolumeSource: corev1.VolumeSource{
                        EmptyDir: &corev1.EmptyDirVolumeSource{
                            Medium: corev1.StorageMediumDefault,
                        },
                    },
                },
            },
            // Schedule on warm pool nodes for faster startup
            NodeSelector: map[string]string{
                "orochi.io/warm-pool": "true",
            },
            Tolerations: []corev1.Toleration{
                {
                    Key:      "orochi.io/serverless",
                    Operator: corev1.TolerationOpExists,
                },
            },
        },
    }
}
```

### 9.5 Warm Pool Management

```go
// pkg/warmpool/manager.go

package warmpool

import (
    "context"
    "sync"

    corev1 "k8s.io/api/core/v1"
)

// WarmPool manages pre-started PostgreSQL containers for fast wake-up
type WarmPool struct {
    mu       sync.RWMutex
    pools    map[string][]*WarmPod  // Keyed by instance class
    config   WarmPoolConfig

    k8sClient client.Client
}

type WarmPoolConfig struct {
    PoolSizes map[string]int  // Instance class -> pool size
    MaxAge    time.Duration   // Max time a warm pod sits unused
    Region    string
}

type WarmPod struct {
    Pod        *corev1.Pod
    InstanceClass string
    CreatedAt  time.Time
    Endpoint   string
}

func (p *WarmPool) Acquire(instanceClass string) (*WarmPod, bool) {
    p.mu.Lock()
    defer p.mu.Unlock()

    pool, exists := p.pools[instanceClass]
    if !exists || len(pool) == 0 {
        return nil, false
    }

    // Take the newest pod (LIFO for better cache locality)
    pod := pool[len(pool)-1]
    p.pools[instanceClass] = pool[:len(pool)-1]

    // Trigger replenishment
    go p.replenish(instanceClass)

    return pod, true
}

func (p *WarmPool) replenish(instanceClass string) {
    p.mu.RLock()
    currentSize := len(p.pools[instanceClass])
    targetSize := p.config.PoolSizes[instanceClass]
    p.mu.RUnlock()

    if currentSize >= targetSize {
        return
    }

    // Create new warm pods
    for i := currentSize; i < targetSize; i++ {
        pod, err := p.createWarmPod(instanceClass)
        if err != nil {
            log.Error(err, "Failed to create warm pod", "class", instanceClass)
            continue
        }

        p.mu.Lock()
        p.pools[instanceClass] = append(p.pools[instanceClass], pod)
        p.mu.Unlock()
    }
}

func (p *WarmPool) createWarmPod(instanceClass string) (*WarmPod, error) {
    resources := getResourcesForClass(instanceClass)

    pod := &corev1.Pod{
        ObjectMeta: metav1.ObjectMeta{
            GenerateName: fmt.Sprintf("orochi-warm-%s-", instanceClass),
            Namespace:    "orochi-warm-pool",
            Labels: map[string]string{
                "orochi.io/component":      "warm-pool",
                "orochi.io/instance-class": instanceClass,
            },
        },
        Spec: corev1.PodSpec{
            Containers: []corev1.Container{
                {
                    Name:      "postgresql",
                    Image:     "orochi/postgresql:17-orochi",
                    Resources: resources,
                    Env: []corev1.EnvVar{
                        {Name: "OROCHI_WARM_POOL", Value: "true"},
                        {Name: "POSTGRES_PASSWORD", Value: "warmpool-temp"},
                    },
                    Ports: []corev1.ContainerPort{
                        {ContainerPort: 5432},
                    },
                    ReadinessProbe: &corev1.Probe{
                        ProbeHandler: corev1.ProbeHandler{
                            Exec: &corev1.ExecAction{
                                Command: []string{"pg_isready"},
                            },
                        },
                        PeriodSeconds: 1,
                    },
                },
            },
            // Use spot instances for warm pool (cost optimization)
            NodeSelector: map[string]string{
                "orochi.io/warm-pool": "true",
            },
        },
    }

    ctx := context.Background()
    if err := p.k8sClient.Create(ctx, pod); err != nil {
        return nil, err
    }

    // Wait for ready
    if err := p.waitForReady(ctx, pod); err != nil {
        p.k8sClient.Delete(ctx, pod)
        return nil, err
    }

    return &WarmPod{
        Pod:          pod,
        InstanceClass: instanceClass,
        CreatedAt:    time.Now(),
        Endpoint:     fmt.Sprintf("%s.orochi-warm-pool.svc:5432", pod.Name),
    }, nil
}
```

---

## 10. Cost Optimization

### 10.1 Cost Model

```yaml
cost_model:
  compute:
    pricing_model: "per_second"

    instance_costs_per_hour:
      nano: 0.02
      micro: 0.04
      small: 0.10
      medium: 0.20
      large: 0.40
      xlarge: 0.80
      2xlarge: 1.60
      4xlarge: 3.20

    spot_discount: 0.70  # 70% off on-demand
    reserved_discount: 0.40  # 40% off with 1-year commit

  storage:
    hot_per_gb_month: 0.15
    warm_per_gb_month: 0.05
    cold_per_gb_month: 0.01
    archive_per_gb_month: 0.004

  network:
    ingress_free: true
    egress_per_gb: 0.09
    cross_region_per_gb: 0.02

  operations:
    iops_included: 3000
    iops_per_1000: 0.065
    throughput_included_mbps: 125
    throughput_per_mbps: 0.04
```

### 10.2 Cost Optimization Strategies

```yaml
cost_optimization_strategies:
  aggressive_scale_down:
    description: "Scale down quickly when load decreases"
    implementation:
      - "Shorter cooldown for scale-down (10m vs 15m default)"
      - "Lower CPU threshold for scale-down (25% vs 30%)"
      - "Proactive scale-down before predicted low periods"
    savings_estimate: "15-25%"

  scale_to_zero:
    description: "Suspend idle clusters to eliminate compute costs"
    implementation:
      - "10-minute idle timeout (5m for free tier)"
      - "Warm pool for fast wake-up (< 5s)"
      - "Connection proxy to handle wake-on-connect"
    savings_estimate: "40-60% for low-traffic clusters"

  spot_instances:
    description: "Use spot/preemptible instances for non-critical workloads"
    implementation:
      - "Warm pool runs on spot instances"
      - "Read replicas can use spot (with graceful replacement)"
      - "Serverless compute pools use spot"
    savings_estimate: "60-70% on compute"

  right_sizing:
    description: "Automatically adjust instance size to match actual usage"
    implementation:
      - "VPA recommendations analyzed weekly"
      - "Suggest downsizing if p95 utilization < 30%"
      - "Customer alerts with cost savings estimates"
    savings_estimate: "20-40%"

  storage_tiering:
    description: "Move cold data to cheaper storage tiers"
    implementation:
      - "Orochi automatic tiering (hot -> warm -> cold)"
      - "S3 Intelligent-Tiering for backups"
      - "Columnar compression for analytics data"
    savings_estimate: "50-80% on storage"

  reserved_capacity:
    description: "Commit to reserved instances for predictable workloads"
    implementation:
      - "Analyze usage patterns to identify stable baseline"
      - "Recommend reserved capacity for consistent usage"
      - "Mix on-demand for burst, reserved for baseline"
    savings_estimate: "30-50% on baseline compute"
```

### 10.3 Budget Controls

```yaml
budget_controls:
  organization_limits:
    - name: "monthly_spend_limit"
      type: "hard_cap"
      action: "prevent_scale_up"
      notification: "email, webhook"

    - name: "monthly_budget_warning"
      type: "soft_cap"
      threshold_percent: 80
      action: "notify_only"

  cluster_limits:
    - name: "max_instance_size"
      per_tier:
        free: "small"
        pro: "4xlarge"
        enterprise: "16xlarge"
      action: "prevent_scale_up"

    - name: "max_replicas"
      per_tier:
        free: 0
        pro: 5
        enterprise: 15
      action: "prevent_scale_up"

    - name: "max_storage"
      per_tier:
        free: "5Gi"
        pro: "1Ti"
        enterprise: "64Ti"
      action: "prevent_expansion"

  alerts:
    - condition: "projected_monthly_cost > budget * 1.1"
      severity: "warning"
      message: "Projected costs exceed budget by 10%"

    - condition: "scaling_blocked_by_budget"
      severity: "critical"
      message: "Autoscaling blocked due to budget limits"
```

### 10.4 Cost Dashboard Metrics

```sql
-- Cost analytics queries for dashboard

-- Current month spend by cluster
SELECT
    cluster_id,
    cluster_name,
    SUM(compute_cost) AS compute_cost,
    SUM(storage_cost) AS storage_cost,
    SUM(network_cost) AS network_cost,
    SUM(compute_cost + storage_cost + network_cost) AS total_cost
FROM billing.usage_records
WHERE org_id = $1
  AND billing_period = DATE_TRUNC('month', CURRENT_DATE)
GROUP BY cluster_id, cluster_name
ORDER BY total_cost DESC;

-- Scaling events and their cost impact
SELECT
    timestamp,
    cluster_id,
    scaling_type,
    from_state,
    to_state,
    hourly_cost_change,
    reason
FROM autoscaler.scaling_events
WHERE org_id = $1
  AND timestamp > NOW() - INTERVAL '7 days'
ORDER BY timestamp DESC;

-- Cost savings from scale-to-zero
SELECT
    cluster_id,
    SUM(EXTRACT(EPOCH FROM suspended_duration) / 3600) AS suspended_hours,
    SUM(EXTRACT(EPOCH FROM suspended_duration) / 3600) * instance_hourly_rate AS savings
FROM autoscaler.suspension_records
WHERE org_id = $1
  AND suspended_at > NOW() - INTERVAL '30 days'
GROUP BY cluster_id;
```

---

## 11. Operational Runbook

### 11.1 Monitoring Dashboard

```yaml
grafana_dashboards:
  autoscaler_overview:
    panels:
      - title: "Scaling Events (24h)"
        type: "stat"
        query: "sum(increase(orochi_scaling_events_total[24h]))"

      - title: "Active Clusters by State"
        type: "pie"
        query: "count by (state) (orochi_cluster_state)"

      - title: "Scale-Up Latency P99"
        type: "gauge"
        query: "histogram_quantile(0.99, orochi_scaling_duration_seconds_bucket{direction='up'})"
        thresholds: [60, 120, 300]

      - title: "Wake-Up Latency P99"
        type: "gauge"
        query: "histogram_quantile(0.99, orochi_wakeup_duration_seconds_bucket)"
        thresholds: [5, 10, 15]

      - title: "Scaling Events Timeline"
        type: "graph"
        queries:
          - "sum(rate(orochi_scaling_events_total{direction='up'}[5m])) * 300"
          - "sum(rate(orochi_scaling_events_total{direction='down'}[5m])) * 300"

      - title: "Cost Savings from Scale-to-Zero"
        type: "stat"
        query: "sum(orochi_suspended_hours_total) * avg(orochi_hourly_rate)"
```

### 11.2 Alert Rules

```yaml
alerting_rules:
  groups:
    - name: autoscaler_health
      rules:
        - alert: AutoscalerDown
          expr: up{job="orochi-autoscaler"} == 0
          for: 1m
          labels:
            severity: critical
          annotations:
            summary: "Autoscaler is down"
            runbook: "/runbooks/autoscaler-down"

        - alert: ScalingBacklog
          expr: orochi_scaling_queue_depth > 100
          for: 5m
          labels:
            severity: warning
          annotations:
            summary: "High scaling request backlog"

        - alert: ScalingFailureRate
          expr: rate(orochi_scaling_failures_total[15m]) / rate(orochi_scaling_attempts_total[15m]) > 0.1
          for: 10m
          labels:
            severity: warning
          annotations:
            summary: "High scaling failure rate (>10%)"

        - alert: WakeUpLatencyHigh
          expr: histogram_quantile(0.99, orochi_wakeup_duration_seconds_bucket) > 15
          for: 5m
          labels:
            severity: warning
          annotations:
            summary: "Wake-up latency exceeds 15s P99"

        - alert: WarmPoolDepleted
          expr: orochi_warm_pool_available == 0
          for: 2m
          labels:
            severity: warning
          annotations:
            summary: "Warm pool is depleted"

    - name: cluster_scaling
      rules:
        - alert: ClusterScalingStuck
          expr: orochi_cluster_scaling_in_progress == 1
          for: 10m
          labels:
            severity: warning
          annotations:
            summary: "Cluster scaling operation stuck"

        - alert: ClusterAtMaxSize
          expr: orochi_cluster_at_max_size == 1 AND orochi_cluster_cpu_percent > 80
          for: 5m
          labels:
            severity: critical
          annotations:
            summary: "Cluster at max size but still overloaded"

        - alert: FrequentScaling
          expr: increase(orochi_scaling_events_total[1h]) > 10
          for: 0m
          labels:
            severity: warning
          annotations:
            summary: "Cluster scaling frequently (>10/hour)"
```

### 11.3 Runbooks

#### Autoscaler Down

```markdown
## Runbook: Autoscaler Down

### Symptoms
- Alert: AutoscalerDown
- No scaling events being processed
- Clusters not responding to load changes

### Diagnosis
1. Check autoscaler pod status:
   ```bash
   kubectl get pods -n orochi-system -l app=orochi-autoscaler
   ```

2. Check pod logs:
   ```bash
   kubectl logs -n orochi-system -l app=orochi-autoscaler --tail=100
   ```

3. Check resource usage:
   ```bash
   kubectl top pods -n orochi-system -l app=orochi-autoscaler
   ```

### Resolution

#### If pod is CrashLooping:
1. Check for OOM:
   ```bash
   kubectl describe pod <pod-name> -n orochi-system | grep -A5 "Last State"
   ```
2. Increase memory limits if OOM
3. Check for panic in logs

#### If pod is Pending:
1. Check node resources:
   ```bash
   kubectl describe nodes | grep -A5 "Allocated resources"
   ```
2. Check for taints/tolerations issues

#### If pod is Running but not processing:
1. Check metrics endpoint:
   ```bash
   kubectl port-forward -n orochi-system svc/orochi-autoscaler 8080:8080
   curl localhost:8080/metrics | grep orochi_
   ```
2. Check etcd/Redis connectivity
3. Check Kubernetes API connectivity

### Escalation
If not resolved in 15 minutes, escalate to on-call SRE.
```

#### Cluster Scaling Stuck

```markdown
## Runbook: Cluster Scaling Stuck

### Symptoms
- Alert: ClusterScalingStuck
- Cluster in "Scaling" state for >10 minutes
- Customer reports slow performance

### Diagnosis
1. Get cluster status:
   ```bash
   kubectl get orochicluster <cluster-name> -n <namespace> -o yaml
   ```

2. Check scaling events:
   ```bash
   kubectl describe orochicluster <cluster-name> -n <namespace>
   ```

3. Check pod status:
   ```bash
   kubectl get pods -n <namespace> -l orochi.io/cluster=<cluster-name>
   ```

### Resolution

#### If new pod is Pending:
1. Check events:
   ```bash
   kubectl get events -n <namespace> --sort-by='.lastTimestamp'
   ```
2. Common issues:
   - Insufficient node resources -> scale node pool
   - PVC not binding -> check storage class
   - Image pull error -> check registry access

#### If new pod is Running but not Ready:
1. Check readiness probe:
   ```bash
   kubectl logs <pod-name> -n <namespace> -c postgresql
   ```
2. Common issues:
   - State restore failed -> check S3 access
   - WAL recovery stuck -> check WAL archive
   - Extension load failed -> check extension compatibility

#### If old pod not terminating:
1. Check for finalizers:
   ```bash
   kubectl get pod <pod-name> -n <namespace> -o yaml | grep finalizers
   ```
2. Force delete if safe:
   ```bash
   kubectl delete pod <pod-name> -n <namespace> --grace-period=0 --force
   ```

### Manual Recovery
If automated scaling is stuck:
1. Mark cluster for manual scaling:
   ```bash
   kubectl annotate orochicluster <cluster-name> orochi.io/manual-scaling=true
   ```
2. Manually create/update resources
3. Update cluster status
4. Remove annotation when complete
```

---

## 12. Testing Strategy

### 12.1 Unit Tests

```go
// pkg/decision/engine_test.go

func TestScalingDecisionEngine(t *testing.T) {
    tests := []struct {
        name           string
        metrics        Metrics
        currentClass   string
        expectedAction string
        expectedClass  string
    }{
        {
            name: "scale up on high CPU",
            metrics: Metrics{
                CPUPercent:    85,
                MemoryPercent: 50,
                Connections:   50,
            },
            currentClass:   "small",
            expectedAction: "scale_up",
            expectedClass:  "medium",
        },
        {
            name: "scale down on low utilization",
            metrics: Metrics{
                CPUPercent:    15,
                MemoryPercent: 20,
                Connections:   10,
                LowUtilizationDuration: 20 * time.Minute,
            },
            currentClass:   "large",
            expectedAction: "scale_down",
            expectedClass:  "medium",
        },
        {
            name: "no action in normal range",
            metrics: Metrics{
                CPUPercent:    50,
                MemoryPercent: 60,
                Connections:   100,
            },
            currentClass:   "medium",
            expectedAction: "no_action",
            expectedClass:  "medium",
        },
        {
            name: "respect cooldown",
            metrics: Metrics{
                CPUPercent:    85,
                MemoryPercent: 50,
                InCooldown:    true,
            },
            currentClass:   "small",
            expectedAction: "no_action",
            expectedClass:  "small",
        },
    }

    for _, tt := range tests {
        t.Run(tt.name, func(t *testing.T) {
            engine := NewDecisionEngine(DefaultConfig())
            cluster := &Cluster{CurrentClass: tt.currentClass}

            decision := engine.EvaluateVertical(tt.metrics, cluster)

            assert.Equal(t, tt.expectedAction, decision.Action)
            if decision.Action != "no_action" {
                assert.Equal(t, tt.expectedClass, decision.TargetClass)
            }
        })
    }
}
```

### 12.2 Integration Tests

```go
// test/integration/autoscaler_test.go

func TestAutoscalerIntegration(t *testing.T) {
    if testing.Short() {
        t.Skip("Skipping integration test")
    }

    ctx := context.Background()
    testEnv := NewTestEnvironment(t)
    defer testEnv.Cleanup()

    t.Run("vertical scaling on CPU spike", func(t *testing.T) {
        // Create cluster
        cluster := testEnv.CreateCluster("test-cluster", "small")

        // Simulate high CPU
        testEnv.InjectMetrics(cluster.ID, Metrics{CPUPercent: 90})

        // Wait for scaling
        require.Eventually(t, func() bool {
            c := testEnv.GetCluster(cluster.ID)
            return c.CurrentClass == "medium"
        }, 2*time.Minute, 5*time.Second)
    })

    t.Run("scale to zero on idle", func(t *testing.T) {
        cluster := testEnv.CreateCluster("idle-cluster", "small")

        // Simulate idle
        testEnv.InjectMetrics(cluster.ID, Metrics{
            Connections: 0,
            QPS:         0,
        })

        // Wait for suspension
        require.Eventually(t, func() bool {
            c := testEnv.GetCluster(cluster.ID)
            return c.Status == "suspended"
        }, 15*time.Minute, 10*time.Second)
    })

    t.Run("wake up on connection", func(t *testing.T) {
        // Ensure cluster is suspended
        cluster := testEnv.GetSuspendedCluster("idle-cluster")

        // Connect
        start := time.Now()
        conn, err := testEnv.Connect(cluster.Endpoint)
        require.NoError(t, err)
        defer conn.Close()

        wakeUpTime := time.Since(start)

        // Verify wake-up time
        assert.Less(t, wakeUpTime, 15*time.Second, "Wake-up should complete within 15s")

        // Verify cluster is running
        c := testEnv.GetCluster(cluster.ID)
        assert.Equal(t, "running", c.Status)
    })
}
```

### 12.3 Load Testing

```yaml
# k6 load test for autoscaling behavior
# test/load/autoscaling_test.js

import sql from 'k6/x/sql';
import { check, sleep } from 'k6';
import { Rate, Trend } from 'k6/metrics';

const connectionLatency = new Trend('connection_latency_ms');
const queryLatency = new Trend('query_latency_ms');
const scalingEvents = new Rate('scaling_events');

export const options = {
    scenarios: {
        // Gradual ramp-up to trigger scale-up
        ramp_up: {
            executor: 'ramping-vus',
            startVUs: 10,
            stages: [
                { duration: '2m', target: 10 },   // Warm-up
                { duration: '5m', target: 100 },  // Ramp-up
                { duration: '10m', target: 100 }, // Sustain
                { duration: '5m', target: 10 },   // Ramp-down
                { duration: '5m', target: 10 },   // Cool-down
            ],
        },

        // Spike test for burst handling
        spike: {
            executor: 'ramping-vus',
            startVUs: 10,
            stages: [
                { duration: '1m', target: 10 },
                { duration: '10s', target: 500 }, // Sudden spike
                { duration: '2m', target: 500 },
                { duration: '10s', target: 10 },  // Sudden drop
                { duration: '5m', target: 10 },
            ],
            startTime: '25m', // Start after ramp test
        },
    },
};

const CONN_STRING = __ENV.OROCHI_CONN_STRING || 'postgresql://test:test@localhost:5432/test';

export default function() {
    const start = Date.now();
    const db = sql.open('postgres', CONN_STRING);
    connectionLatency.add(Date.now() - start);

    // Run queries
    for (let i = 0; i < 10; i++) {
        const queryStart = Date.now();
        const result = db.query('SELECT pg_sleep(0.01), current_timestamp');
        queryLatency.add(Date.now() - queryStart);

        check(result, {
            'query successful': (r) => r.length > 0,
        });
    }

    db.close();
    sleep(1);
}

export function handleSummary(data) {
    return {
        'stdout': textSummary(data, { indent: '  ', enableColors: true }),
        'summary.json': JSON.stringify(data),
    };
}
```

### 12.4 Chaos Testing

```yaml
# Chaos experiments for autoscaler resilience
apiVersion: chaos-mesh.org/v1alpha1
kind: Schedule
metadata:
  name: autoscaler-chaos
  namespace: orochi-system
spec:
  schedule: "0 2 * * *"  # Daily at 2 AM
  type: "Workflow"
  historyLimit: 5
  concurrencyPolicy: Forbid
  workflowSpec:
    entry: chaos-suite
    templates:
      - name: chaos-suite
        templateType: Serial
        children:
          - pod-failure
          - network-delay
          - resource-stress

      - name: pod-failure
        templateType: PodChaos
        podChaos:
          selector:
            namespaces:
              - orochi-system
            labelSelectors:
              app: orochi-autoscaler
          mode: one
          action: pod-kill
          duration: "30s"

      - name: network-delay
        templateType: NetworkChaos
        networkChaos:
          selector:
            namespaces:
              - orochi-system
          mode: all
          action: delay
          delay:
            latency: "100ms"
            jitter: "50ms"
          duration: "2m"

      - name: resource-stress
        templateType: StressChaos
        stressChaos:
          selector:
            namespaces:
              - tenant-test
            labelSelectors:
              orochi.io/component: compute
          stressors:
            cpu:
              workers: 4
              load: 90
          duration: "5m"
```

---

## Appendix A: Metrics Reference

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `orochi_cluster_cpu_percent` | Gauge | cluster_id | Current CPU utilization |
| `orochi_cluster_memory_percent` | Gauge | cluster_id | Current memory utilization |
| `orochi_cluster_connections` | Gauge | cluster_id | Active connection count |
| `orochi_cluster_storage_used_bytes` | Gauge | cluster_id | Storage used |
| `orochi_cluster_state` | Gauge | cluster_id, state | Cluster state (1=active) |
| `orochi_scaling_events_total` | Counter | cluster_id, direction, type | Scaling event count |
| `orochi_scaling_duration_seconds` | Histogram | direction, type | Scaling operation duration |
| `orochi_wakeup_duration_seconds` | Histogram | instance_class | Wake-up latency |
| `orochi_suspended_hours_total` | Counter | cluster_id | Total suspended hours |
| `orochi_warm_pool_available` | Gauge | instance_class | Available warm pods |

---

## Appendix B: Configuration Reference

```yaml
# Complete autoscaler configuration reference
autoscaler:
  global:
    evaluation_interval: "15s"
    metrics_retention: "30d"

  vertical:
    enabled: true
    scale_up:
      cpu_threshold: 80
      memory_threshold: 85
      consecutive_breaches: 2
      evaluation_period: "5m"
      cooldown: "5m"
    scale_down:
      cpu_threshold: 30
      memory_threshold: 40
      consecutive_breaches: 3
      evaluation_period: "15m"
      cooldown: "15m"

  horizontal:
    enabled: true
    min_replicas: 0
    max_replicas: 15
    target_cpu_per_replica: 70
    target_qps_per_replica: 1000
    scale_up_cooldown: "3m"
    scale_down_cooldown: "10m"

  storage:
    auto_expand: true
    threshold_percent: 80
    expansion_percent: 25
    min_expansion_gb: 10
    max_size_tb: 64

  serverless:
    scale_to_zero: true
    idle_timeout: "10m"
    warm_pool:
      enabled: true
      sizes:
        nano: 5
        micro: 5
        small: 10
        medium: 5
      max_age: "30m"

  predictive:
    enabled: true
    model: "prophet"
    training_window: "30d"
    forecast_horizon: "1h"
    lookahead_minutes: 15
    confidence_threshold: 0.75

  connection_pooling:
    enabled: true
    mode: "transaction"
    min_size: 10
    max_size: 500
    scale_up_threshold: 80
    scale_down_threshold: 20

  budget_controls:
    enforce_limits: true
    alert_threshold_percent: 80
    block_threshold_percent: 100
```

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-16 | Architecture Team | Initial design document |

---

*This document is the authoritative reference for Orochi Cloud autoscaling architecture. It should be reviewed quarterly and updated as the system evolves.*
