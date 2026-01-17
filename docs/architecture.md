# Orochi DB System Architecture

Comprehensive architecture documentation for Orochi DB - a PostgreSQL HTAP extension with cloud platform.

## Table of Contents

- [System Overview](#system-overview)
- [PostgreSQL Extension Architecture](#postgresql-extension-architecture)
- [Cloud Platform Architecture](#cloud-platform-architecture)
- [Data Flow](#data-flow)
- [Component Interactions](#component-interactions)
- [Storage Architecture](#storage-architecture)
- [Query Processing](#query-processing)
- [Distributed Execution](#distributed-execution)
- [Security Architecture](#security-architecture)
- [Scalability](#scalability)
- [Performance Characteristics](#performance-characteristics)

## System Overview

Orochi DB is a dual-component system designed to provide HTAP capabilities for PostgreSQL:

```
┌─────────────────────────────────────────────────────────────────┐
│                         Orochi DB System                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │          PostgreSQL Extension (C11)                        │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │ │
│  │  │ Sharding │  │   Time   │  │ Columnar │  │  Vector  │  │ │
│  │  │          │  │  Series  │  │ Storage  │  │   Ops    │  │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │ │
│  │  │  Tiered  │  │   CDC    │  │ Pipelines│  │   Raft   │  │ │
│  │  │ Storage  │  │          │  │          │  │ Consensus│  │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │ │
│  └────────────────────────────────────────────────────────────┘ │
│                             │                                    │
│                             │                                    │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │          Cloud Platform (Go + React)                       │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │ │
│  │  │ Control  │  │   CLI    │  │Dashboard │  │Autoscaler│  │ │
│  │  │  Plane   │  │          │  │          │  │          │  │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │ │
│  │  ┌──────────┐  ┌──────────┐                               │ │
│  │  │Provision │  │  Infra   │                               │ │
│  │  │   er     │  │structure │                               │ │
│  │  └──────────┘  └──────────┘                               │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Separation of Concerns**: Extension handles data processing, cloud platform handles orchestration
2. **PostgreSQL Native**: Built as a proper PostgreSQL extension, not a fork
3. **Cloud Native**: Kubernetes-first deployment with CloudNativePG
4. **Performance First**: SIMD optimization, JIT compilation, columnar storage
5. **Scalability**: Horizontal scaling via sharding, vertical via resource pools
6. **Flexibility**: Multiple storage formats, compression algorithms, and workload patterns

## PostgreSQL Extension Architecture

### High-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                    PostgreSQL Core                               │
├─────────────────────────────────────────────────────────────────┤
│                    Orochi Extension Hooks                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │ Planner  │  │ Executor │  │ Utility  │  │ProcessUtil│       │
│  │  Hook    │  │  Hooks   │  │  Hook    │  │   Hook    │       │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘       │
├───────┼─────────────┼─────────────┼─────────────┼──────────────┤
│       ▼             ▼             ▼             ▼               │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                   Core Subsystems                          │ │
│  ├────────────────────────────────────────────────────────────┤ │
│  │  Catalog Manager  │  GUC Variables  │  Shared Memory      │ │
│  │  Background Workers │  Memory Contexts │  Error Handling  │ │
│  └────────────────────────────────────────────────────────────┘ │
│                              │                                  │
│       ┌──────────────────────┼──────────────────────┐          │
│       ▼                      ▼                      ▼           │
│  ┌─────────┐          ┌─────────┐          ┌─────────┐        │
│  │Distribution        │ Time-Series        │ Storage │         │
│  │  Layer  │          │  Layer  │          │ Engines │         │
│  └────┬────┘          └────┬────┘          └────┬────┘        │
│       │                    │                    │              │
│       └────────────┬───────┴────────────────────┘              │
│                    ▼                                            │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              Data Access Layer                             │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │ │
│  │  │   Heap   │  │ Columnar │  │  Vector  │  │  Tiered  │  │ │
│  │  │  Tables  │  │  Stripes │  │  Store   │  │  S3/Disk │  │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Module Organization

#### Core Modules

**src/core/init.c**
- Extension initialization (`_PG_init`, `_PG_fini`)
- GUC variable registration
- Hook installation
- Background worker registration
- Shared memory allocation

**src/core/catalog.c**
- Metadata table management
- Table/shard/chunk tracking
- Schema version management
- Distributed transaction log

#### Distribution Layer

**src/sharding/distribution.c**
- Hash-based sharding
- Shard key extraction
- Co-location groups
- Reference table replication
- Shard pruning logic

**src/sharding/physical_sharding.c**
- Physical shard placement
- Shard rebalancing
- Node management
- Shard migration

#### Time-Series Layer

**src/timeseries/hypertable.c**
- Hypertable creation
- Chunk management
- Time-based partitioning
- Space partitioning
- Continuous aggregates
- Retention policies

#### Storage Engines

**src/storage/columnar.c**
- Columnar table access methods
- Stripe management
- Column chunk encoding
- Skip list indexing
- Projection pushdown

**src/storage/compression.c**
- LZ4 general compression
- ZSTD high-ratio compression
- Delta encoding for monotonic data
- Gorilla compression for floats
- Dictionary encoding for strings
- RLE for repeated values

**src/tiered/tiered_storage.c**
- Hot/warm/cold/frozen tiers
- S3 integration
- Data lifecycle management
- Transparent retrieval
- Tiering policies

#### Vector Operations

**src/vector/vector_ops.c**
- Vector data type
- Distance functions (L2, cosine, inner product)
- SIMD optimization (AVX2, AVX-512)
- Vector indexing (IVFFlat, HNSW)
- Batch operations

#### Query Processing

**src/planner/distributed_planner.c**
- Query plan analysis
- Shard pruning
- Join co-location detection
- Aggregate pushdown
- Fragment generation

**src/executor/distributed_executor.c**
- Distributed query execution
- Fragment routing
- Result merging
- 2PC coordination
- Adaptive parallelism

**src/executor/vectorized.c**
- Batch-oriented execution
- Column-wise processing
- SIMD vectorization
- Cache-friendly algorithms

**src/jit/jit_compile.c**
- Expression compilation
- LLVM code generation
- Runtime optimization
- Cache management

#### Consensus & Coordination

**src/consensus/raft.c**
- Raft protocol implementation
- Leader election
- Log replication
- Snapshot management
- Membership changes

#### Streaming & CDC

**src/pipelines/pipeline.c**
- Pipeline framework
- Source/sink abstraction
- Backpressure handling
- Error recovery

**src/pipelines/kafka_source.c**
- Kafka consumer integration
- Offset management
- Partition assignment

**src/cdc/cdc.c**
- Logical replication slot
- Change event encoding
- Filtering and transformation
- Output plugin API

#### Authentication

**src/auth/auth.c**
- Authentication framework
- Session management
- Permission checking

**src/auth/jwt.c**
- JWT token validation
- Claims extraction
- Key rotation

**src/auth/webauthn.c**
- WebAuthn/FIDO2 support
- Credential storage
- Challenge generation

### Extension Lifecycle

```
PostgreSQL Startup
    ├──> _PG_init()
    │     ├──> Define GUC variables
    │     ├──> Request shared memory
    │     ├──> Install hooks
    │     │     ├──> planner_hook
    │     │     ├──> executor_start_hook
    │     │     ├──> executor_run_hook
    │     │     ├──> executor_finish_hook
    │     │     ├──> executor_end_hook
    │     │     └──> ProcessUtility_hook
    │     └──> Register background workers
    │           ├──> Tiering worker
    │           ├──> Compression worker
    │           ├──> Stats collector
    │           ├──> Rebalancer
    │           └──> Raft consensus
    │
    ├──> Shared memory initialization
    │     ├──> orochi_shmem_startup()
    │     ├──> Initialize shared state
    │     ├──> Initialize locks
    │     └──> Initialize worker queues
    │
    └──> Extension ready

Query Execution
    ├──> Parser (PostgreSQL)
    ├──> Analyzer (PostgreSQL)
    ├──> Planner
    │     └──> orochi_planner_hook()
    │           ├──> Detect distributed tables
    │           ├──> Perform shard pruning
    │           ├──> Detect co-located joins
    │           ├──> Push down aggregates
    │           └──> Generate fragment plans
    │
    ├──> Executor
    │     ├──> orochi_executor_start_hook()
    │     │     ├──> Initialize distributed state
    │     │     └──> Allocate fragment slots
    │     │
    │     ├──> orochi_executor_run_hook()
    │     │     ├──> Route fragments to shards
    │     │     ├──> Execute in parallel
    │     │     ├──> Merge results
    │     │     └──> Apply limit/offset
    │     │
    │     ├──> orochi_executor_finish_hook()
    │     │     └──> Finalize execution
    │     │
    │     └──> orochi_executor_end_hook()
    │           └──> Cleanup resources
    │
    └──> Return results

PostgreSQL Shutdown
    └──> _PG_fini()
          ├──> Restore hooks
          ├──> Signal workers to exit
          └──> Cleanup resources
```

## Cloud Platform Architecture

### Component Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Kubernetes Cluster                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                  Control Plane Service                     │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │ │
│  │  │   HTTP/REST  │  │     gRPC     │  │   WebSocket     │  │ │
│  │  │     API      │  │     API      │  │  (real-time)    │  │ │
│  │  └──────┬───────┘  └──────┬───────┘  └────────┬────────┘  │ │
│  │         └──────────────────┼──────────────────┬┘           │ │
│  │  ┌─────────────────────────▼──────────────────▼─────────┐  │ │
│  │  │              Business Logic Layer                     │  │ │
│  │  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────────┐ │  │ │
│  │  │  │Cluster │  │  User  │  │ Backup │  │ Monitoring │ │  │ │
│  │  │  │Service │  │Service │  │Service │  │  Service   │ │  │ │
│  │  │  └────────┘  └────────┘  └────────┘  └────────────┘ │  │ │
│  │  └──────────────────────┬─────────────────────────────── │  │ │
│  │                         ▼                                  │ │
│  │  ┌────────────────────────────────────────────────────────┤ │
│  │  │               Data Access Layer                        │ │
│  │  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │ │
│  │  │  │  PostgreSQL  │  │  Kubernetes  │  │    Cache    │ │ │
│  │  │  │   Metadata   │  │     API      │  │    (Redis)  │ │ │
│  │  │  └──────────────┘  └──────────────┘  └─────────────┘ │ │
│  │  └────────────────────────────────────────────────────────┘ │
│  └────────────────────────────────────────────────────────────┘ │
│                             │                                    │
│              ┌──────────────┼──────────────┐                    │
│              ▼              ▼              ▼                     │
│  ┌────────────────┐  ┌─────────────┐  ┌────────────────┐       │
│  │  Provisioner   │  │ Autoscaler  │  │  Dashboard     │       │
│  │    Service     │  │   Service   │  │  (React App)   │       │
│  │                │  │             │  │                │       │
│  │  - Cluster     │  │  - Metrics  │  │  - Web UI      │       │
│  │    creation    │  │    monitor  │  │  - Real-time   │       │
│  │  - CNPG mgmt   │  │  - Scale    │  │    updates     │       │
│  │  - Templates   │  │    decisions│  │  - Auth        │       │
│  └───────┬────────┘  └──────┬──────┘  └────────────────┘       │
│          │                  │                                    │
│          ▼                  ▼                                    │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │            CloudNativePG Operator                          │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │ │
│  │  │   Cluster    │  │   Backup     │  │   Pooler        │  │ │
│  │  │  Controller  │  │  Controller  │  │   Controller    │  │ │
│  │  └──────────────┘  └──────────────┘  └─────────────────┘  │ │
│  └────────────────────────────────────────────────────────────┘ │
│                             │                                    │
│                             ▼                                    │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              Orochi DB Clusters                            │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │ Cluster 1                                            │  │ │
│  │  │  ┌──────────┐  ┌──────────┐  ┌──────────┐           │  │ │
│  │  │  │ Primary  │  │ Replica  │  │ Replica  │           │  │ │
│  │  │  │   Pod    │  │   Pod    │  │   Pod    │           │  │ │
│  │  │  └──────────┘  └──────────┘  └──────────┘           │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │ Cluster 2                                            │  │ │
│  │  │  ┌──────────┐  ┌──────────┐                         │  │ │
│  │  │  │ Primary  │  │ Replica  │                         │  │ │
│  │  │  └──────────┘  └──────────┘                         │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Control Plane Service

**Responsibilities:**
- Cluster lifecycle management (create, update, delete)
- User authentication and authorization
- Backup and restore operations
- Monitoring and alerting
- Billing and metering
- API Gateway

**Technology Stack:**
- Go 1.22+
- Chi router for HTTP
- gRPC for inter-service communication
- PostgreSQL for metadata
- Redis for caching

**API Endpoints:**
```
POST   /api/v1/clusters                 # Create cluster
GET    /api/v1/clusters                 # List clusters
GET    /api/v1/clusters/:id             # Get cluster details
PATCH  /api/v1/clusters/:id             # Update cluster
DELETE /api/v1/clusters/:id             # Delete cluster
POST   /api/v1/clusters/:id/scale       # Scale cluster
GET    /api/v1/clusters/:id/metrics     # Get metrics
POST   /api/v1/clusters/:id/backups     # Create backup
GET    /api/v1/clusters/:id/backups     # List backups
POST   /api/v1/clusters/:id/restore     # Restore backup
WS     /ws/clusters/:id                 # Real-time updates
```

### Provisioner Service

**Responsibilities:**
- CloudNativePG cluster provisioning
- Template management
- Resource allocation
- Network configuration
- Volume management

**Workflow:**
```
1. Receive provisioning request
2. Validate configuration
3. Generate CloudNativePG manifest
4. Apply to Kubernetes
5. Wait for cluster ready
6. Configure networking
7. Initialize extension
8. Return cluster details
```

### Autoscaler Service

**Responsibilities:**
- Metrics collection
- Scaling decisions
- Resource optimization
- Cost management

**Scaling Algorithm:**
```
1. Collect metrics (CPU, memory, connections, query latency)
2. Calculate resource utilization
3. Apply scaling rules
   - Scale up if: CPU > 80% or Memory > 85% or Connections > 90%
   - Scale down if: CPU < 30% and Memory < 40% and Connections < 50%
4. Execute scaling action via control plane API
5. Monitor scaling operation
```

### Dashboard

**Technology Stack:**
- React 19
- TypeScript
- TanStack Router
- TanStack Query
- shadcn/ui components
- Tailwind CSS
- Recharts for visualization

**Features:**
- Cluster management UI
- Real-time metrics visualization
- Query monitoring
- Backup management
- User management
- Cost tracking

## Data Flow

### Write Path

```
Application
    │
    ├──> SQL INSERT/UPDATE/DELETE
    │
    ▼
PostgreSQL Parser/Analyzer
    │
    ▼
Orochi Planner Hook
    ├──> Determine target shard(s)
    ├──> Calculate hash of distribution column
    ├──> Lookup shard placement
    │
    ▼
Orochi Executor Hook
    ├──> For single shard:
    │     └──> Direct execution
    │
    ├──> For multiple shards (e.g., UPDATE without dist column):
    │     ├──> Generate fragment for each shard
    │     ├──> Execute in parallel
    │     └──> 2PC if needed
    │
    ▼
Storage Selection
    ├──> Row Store (heap)
    │     └──> Standard PostgreSQL heap
    │
    ├──> Columnar Store
    │     ├──> Buffer in memory
    │     ├──> Batch into stripe (100k rows or 1GB)
    │     ├──> Compress columns
    │     └──> Write to disk
    │
    ├──> Time-Series (hypertable)
    │     ├──> Determine chunk based on time column
    │     ├──> Create chunk if needed
    │     └──> Insert into chunk
    │
    ▼
Tiering (async background worker)
    ├──> Monitor data age
    ├──> Hot → Warm (7 days)
    │     └──> Convert to columnar + compress
    ├──> Warm → Cold (30 days)
    │     └──> Upload to S3
    ├──> Cold → Frozen (90 days)
    │     └──> Migrate to S3 Glacier
```

### Read Path

```
Application
    │
    ├──> SQL SELECT
    │
    ▼
PostgreSQL Parser/Analyzer
    │
    ▼
Orochi Planner Hook
    ├──> Analyze WHERE clause
    ├──> Shard pruning
    │     ├──> If dist column in WHERE: prune to specific shards
    │     └──> If time column in WHERE: prune to specific chunks
    │
    ├──> Join optimization
    │     ├──> Co-located join: execute on same node
    │     └──> Broadcast join: replicate small table
    │
    ├──> Aggregate pushdown
    │     ├──> Push COUNT/SUM/MIN/MAX to shards
    │     └──> Combine partial results
    │
    ▼
Orochi Executor Hook
    ├──> Generate fragment queries
    ├──> Route to shards in parallel
    ├──> Execute with adaptive concurrency
    │     ├──> Start with 2 parallel fragments
    │     ├──> Ramp up to max_parallel_workers
    │
    ▼
Storage Access
    ├──> Row Store
    │     └──> Sequential scan or index scan
    │
    ├──> Columnar Store
    │     ├──> Read only needed columns (projection)
    │     ├──> Apply skip list predicates
    │     ├──> Decompress column chunks
    │     └──> Vectorized processing
    │
    ├──> Tiered Store
    │     ├──> Check local cache
    │     ├──> If not cached:
    │     │     ├──> Fetch from S3
    │     │     ├──> Decompress
    │     │     └──> Cache locally
    │     └──> Process data
    │
    ▼
Result Merging
    ├──> Combine results from shards
    ├──> Apply sort/limit/offset
    ├──> Compute final aggregates
    │
    ▼
Return to Application
```

### Vector Similarity Search

```
Application
    │
    ├──> SELECT * FROM table ORDER BY embedding <-> query_vector LIMIT k
    │
    ▼
Orochi Planner Hook
    ├──> Detect vector similarity operator (<->, <#>, <=>)
    ├──> Choose index strategy
    │     ├──> IVFFlat: cluster-based approximate search
    │     └──> HNSW: graph-based approximate search
    │
    ▼
Vector Index Scan
    ├──> IVFFlat:
    │     ├──> Find nearest clusters (nprobe)
    │     ├──> Scan vectors in those clusters
    │     └──> Use SIMD for distance calculation
    │
    ├──> HNSW:
    │     ├──> Start at entry point
    │     ├──> Navigate graph layers
    │     └──> Find k nearest neighbors
    │
    ▼
Distance Calculation (SIMD)
    ├──> L2 distance: AVX2 vectorized sqrt(sum((a-b)^2))
    ├──> Cosine: AVX2 vectorized (dot(a,b) / (norm(a) * norm(b)))
    └──> Inner product: AVX2 vectorized dot(a,b)
    │
    ▼
Return top k results
```

## Component Interactions

### Cluster Creation Flow

```
User (CLI/Dashboard)
    │
    ├──> POST /api/v1/clusters { name, size, region }
    │
    ▼
Control Plane API
    ├──> Validate request
    ├──> Check quotas
    ├──> Generate cluster ID
    ├──> Store in metadata DB
    │
    ▼
Provisioner Service (gRPC call)
    ├──> Load cluster template
    ├──> Substitute parameters
    │     ├──> Name
    │     ├──> Size (small/medium/large)
    │     ├──> Region
    │     ├──> Orochi extension config
    │
    ├──> Generate CloudNativePG manifest:
    │     ```yaml
    │     apiVersion: postgresql.cnpg.io/v1
    │     kind: Cluster
    │     metadata:
    │       name: orochi-cluster-abc123
    │     spec:
    │       instances: 3
    │       postgresql:
    │         shared_preload_libraries: ["orochi"]
    │       storage:
    │         size: 100Gi
    │     ```
    │
    ├──> Apply to Kubernetes
    │     └──> kubectl apply -f cluster.yaml
    │
    ▼
CloudNativePG Operator
    ├──> Watch for new Cluster resource
    ├──> Create primary instance
    ├──> Wait for primary ready
    ├──> Create replica instances
    ├──> Configure replication
    ├──> Create services
    │
    ▼
Orochi Extension Initialization
    ├──> _PG_init() runs on each pod
    ├──> Initialize distributed metadata
    ├──> Join Raft cluster
    ├──> Synchronize shard map
    │
    ▼
Provisioner monitors readiness
    ├──> Wait for all pods ready
    ├──> Run initialization SQL:
    │     CREATE EXTENSION orochi;
    │     SELECT orochi_init_cluster();
    │
    ├──> Update cluster status: READY
    │
    ▼
Return cluster details to Control Plane
    │
    ▼
Control Plane
    ├──> Update metadata DB
    ├──> Send WebSocket notification
    │
    ▼
Dashboard updates in real-time
```

### Autoscaling Flow

```
Autoscaler Service (runs every 30s)
    │
    ├──> Query Prometheus for metrics
    │     ├──> CPU utilization
    │     ├──> Memory utilization
    │     ├──> Active connections
    │     ├──> Query latency (p95, p99)
    │     └──> Disk I/O
    │
    ▼
Analyze metrics
    ├──> Calculate resource pressure
    ├──> Apply scaling rules
    ├──> Determine action: scale_up, scale_down, none
    │
    ▼
If scale_up:
    ├──> Calculate target instance count
    ├──> Call Control Plane API:
    │     PATCH /api/v1/clusters/:id/scale
    │     { instances: new_count }
    │
    ▼
Control Plane
    ├──> Validate scaling request
    ├──> Update CloudNativePG Cluster spec
    │     ```yaml
    │     spec:
    │       instances: 5  # was 3
    │     ```
    │
    ▼
CloudNativePG Operator
    ├──> Detect spec change
    ├──> Create new replica pods
    ├──> Wait for pods ready
    ├──> Update service endpoints
    │
    ▼
Orochi Extension
    ├──> New instances join Raft cluster
    ├──> Receive shard map
    ├──> Start accepting queries
    │
    ▼
Autoscaler monitors scaling operation
    ├──> Wait for metrics to stabilize
    ├──> Record scaling event
```

## Storage Architecture

### Columnar Storage Layout

```
Stripe (100k rows or 1GB, whichever comes first)
┌─────────────────────────────────────────────────────────┐
│ Stripe Header                                           │
│  - Magic number: 0x4F524348 ('ORCH')                   │
│  - Version: 1                                           │
│  - Row count: 100,000                                   │
│  - Column count: 5                                      │
│  - Compression: ZSTD                                    │
│  - Checksum: CRC32                                      │
└─────────────────────────────────────────────────────────┘
│
├─ Column 0: id (INT8)
│  ┌──────────────────────────────────────────────────┐
│  │ Column Metadata                                  │
│  │  - Type: INT8                                    │
│  │  - Compression: DELTA                            │
│  │  - Null count: 0                                 │
│  │  - Min: 1                                        │
│  │  - Max: 100000                                   │
│  │  - Offset: 1024                                  │
│  │  - Length: 50000 (compressed)                    │
│  └──────────────────────────────────────────────────┘
│  │
│  ├─ Delta-encoded values (base=1, deltas=[1,1,1,...])
│  └─ Skip list index (every 1000 rows)
│
├─ Column 1: timestamp (TIMESTAMPTZ)
│  ┌──────────────────────────────────────────────────┐
│  │ Column Metadata                                  │
│  │  - Type: TIMESTAMPTZ                             │
│  │  - Compression: DELTA                            │
│  │  - Min: 2024-01-01 00:00:00                      │
│  │  - Max: 2024-01-01 23:59:59                      │
│  └──────────────────────────────────────────────────┘
│  │
│  └─ Delta-encoded microseconds since epoch
│
├─ Column 2: price (FLOAT8)
│  ┌──────────────────────────────────────────────────┐
│  │ Column Metadata                                  │
│  │  - Type: FLOAT8                                  │
│  │  - Compression: GORILLA                          │
│  └──────────────────────────────────────────────────┘
│  │
│  └─ Gorilla-encoded values (XOR compression)
│
├─ Column 3: status (TEXT)
│  ┌──────────────────────────────────────────────────┐
│  │ Column Metadata                                  │
│  │  - Type: TEXT                                    │
│  │  - Compression: DICTIONARY                       │
│  │  - Cardinality: 4                                │
│  │  - Dictionary: ["pending", "active", "done", ...] │
│  └──────────────────────────────────────────────────┘
│  │
│  ├─ Dictionary (4 unique values)
│  └─ Index array (2 bits per value, 4 values possible)
│
└─ Column 4: description (TEXT)
   ┌──────────────────────────────────────────────────┐
   │ Column Metadata                                  │
   │  - Type: TEXT                                    │
   │  - Compression: ZSTD                             │
   │  - Avg length: 120                               │
   └──────────────────────────────────────────────────┘
   │
   ├─ Offset array (INT32[100000])
   └─ Compressed text blob
```

### Tiered Storage Layout

```
Data Lifecycle
┌──────────────────────────────────────────────────────────┐
│ Hot Tier (0-1 day)                                       │
│  - Storage: Local NVMe SSD                              │
│  - Format: PostgreSQL heap                              │
│  - Indexes: All indexes active                          │
│  - Compression: None                                    │
│  - Access: Sub-millisecond                              │
└──────────────────────────────────────────────────────────┘
            │ (age > 1 day)
            ▼
┌──────────────────────────────────────────────────────────┐
│ Warm Tier (1-7 days)                                     │
│  - Storage: Local SSD                                   │
│  - Format: Columnar stripes                             │
│  - Indexes: Primary index only                          │
│  - Compression: LZ4 (fast)                              │
│  - Access: Milliseconds                                 │
└──────────────────────────────────────────────────────────┘
            │ (age > 7 days)
            ▼
┌──────────────────────────────────────────────────────────┐
│ Cold Tier (7-30 days)                                    │
│  - Storage: S3 Standard                                 │
│  - Format: Columnar stripes                             │
│  - Indexes: Metadata only                               │
│  - Compression: ZSTD (high ratio)                       │
│  - Access: Seconds (with local cache)                   │
└──────────────────────────────────────────────────────────┘
            │ (age > 30 days)
            ▼
┌──────────────────────────────────────────────────────────┐
│ Frozen Tier (30+ days)                                   │
│  - Storage: S3 Glacier                                  │
│  - Format: Columnar stripes                             │
│  - Indexes: None                                        │
│  - Compression: ZSTD level 19                           │
│  - Access: Minutes to hours                             │
└──────────────────────────────────────────────────────────┘
```

## Query Processing

### Query Plan Example

SQL Query:
```sql
SELECT
  device_id,
  time_bucket('1 hour', time) AS hour,
  AVG(temperature) AS avg_temp,
  MAX(temperature) AS max_temp
FROM metrics
WHERE time >= NOW() - INTERVAL '7 days'
  AND device_id IN (1, 2, 3)
GROUP BY device_id, hour
ORDER BY device_id, hour;
```

Execution Plan:
```
Sort (cost=5000..5100 rows=1000)
  Sort Key: device_id, hour
  ->  HashAggregate (cost=4000..4500 rows=1000)
        Group Key: device_id, time_bucket('1 hour', time)
        ->  Custom Scan (Orochi Distributed Scan)
              Shards: 3 of 32 (pruned by device_id IN (1,2,3))
              Chunks: 168 of 365 (pruned by time >= NOW() - '7 days')
              Storage: Columnar (warm tier)
              Pushdown:
                - Projection: device_id, time, temperature
                - Filter: device_id IN (1,2,3) AND time >= ...
                - Partial Aggregate: AVG(temperature), MAX(temperature)
              Fragments:
                1. Shard 5, Chunks [100-123]: 24 chunks
                2. Shard 12, Chunks [100-123]: 24 chunks
                3. Shard 19, Chunks [100-123]: 24 chunks
              ->  Vectorized Scan on metrics_columnar
                    Filter: ...
                    Vectorized ops: AVX2
```

### Distributed Execution

```
Coordinator Node
    │
    ├──> Generate fragment plans
    │     Fragment 1: Shard 5, Chunks 100-123
    │     Fragment 2: Shard 12, Chunks 100-123
    │     Fragment 3: Shard 19, Chunks 100-123
    │
    ├──> Establish connections to shard nodes
    │
    ├──> Execute fragments in parallel (adaptive)
    │     ├──> Start with 2 parallel
    │     ├──> Ramp up to max_parallel_workers
    │     ├──> Monitor latency
    │     └──> Adjust parallelism dynamically
    │
    ▼
Worker Nodes (in parallel)
    │
    ├──> Receive fragment query
    ├──> Execute on local shards/chunks
    ├──> Apply filters
    ├──> Compute partial aggregates
    ├──> Stream results back
    │
    ▼
Coordinator Node
    │
    ├──> Receive partial results
    ├──> Merge results
    ├──> Compute final aggregates
    ├──> Apply sort/limit
    │
    ▼
Return to client
```

## Security Architecture

### Authentication Flow

```
Client
    │
    ├──> Connect to PostgreSQL
    │
    ▼
Orochi Auth Hook
    ├──> Check authentication method
    │
    ├──> JWT Token
    │     ├──> Extract token from connection param
    │     ├──> Verify signature (RS256/ES256)
    │     ├──> Check expiration
    │     ├──> Extract claims (user_id, roles)
    │     └──> Map to PostgreSQL role
    │
    ├──> WebAuthn
    │     ├──> Challenge-response flow
    │     ├──> Verify credential
    │     └──> Map to PostgreSQL role
    │
    ├──> Magic Link
    │     ├──> Verify token
    │     ├──> Check single-use
    │     └──> Map to PostgreSQL role
    │
    ▼
PostgreSQL Role Assignment
    ├──> SET ROLE <mapped_role>
    ├──> Apply RLS policies
    └──> Grant permissions
```

### Row-Level Security

```sql
-- Example: Multi-tenant table with RLS
CREATE TABLE tenant_data (
    id BIGSERIAL PRIMARY KEY,
    tenant_id INT NOT NULL,
    data JSONB
);

-- Enable RLS
ALTER TABLE tenant_data ENABLE ROW LEVEL SECURITY;

-- Policy: Users can only see their own tenant's data
CREATE POLICY tenant_isolation ON tenant_data
    USING (tenant_id = current_setting('app.tenant_id')::int);

-- Set tenant context after authentication
SELECT orochi_set_tenant(123);  -- Sets app.tenant_id = 123

-- All queries automatically filtered
SELECT * FROM tenant_data;  -- Only sees rows WHERE tenant_id = 123
```

## Scalability

### Horizontal Scaling

**Shard Addition:**
```
1. Add new nodes to cluster
2. Raft leader detects new members
3. Trigger rebalancing:
   - Calculate new shard distribution
   - Select shards to move
   - Copy data to new nodes
   - Update shard map atomically
   - Redirect queries to new locations
4. Background cleanup of old shards
```

**Read Scaling:**
```
1. Add read replicas via CloudNativePG
2. Configure connection pooling
3. Route read queries to replicas
4. Route write queries to primary
```

### Vertical Scaling

**Resource Pools:**
```sql
-- Create resource pools for different workloads
SELECT orochi_create_resource_pool(
    'analytics',
    memory_limit => '4GB',
    cpu_limit => 4,
    max_connections => 20
);

SELECT orochi_create_resource_pool(
    'oltp',
    memory_limit => '2GB',
    cpu_limit => 2,
    max_connections => 100
);

-- Assign queries to pools
SET orochi.resource_pool = 'analytics';
-- Heavy analytical query runs here
```

## Performance Characteristics

### Latency Targets

| Operation | Target Latency (p95) |
|-----------|---------------------|
| Point query (single shard) | < 1ms |
| Point query (multi-shard) | < 5ms |
| Range scan (1M rows, columnar) | < 100ms |
| Aggregate (1B rows, pushdown) | < 1s |
| Vector similarity (1M vectors, HNSW) | < 10ms |
| Write (single row) | < 2ms |
| Write (batch 10k rows) | < 100ms |

### Throughput Targets

| Workload | Target Throughput |
|----------|------------------|
| OLTP (point queries) | > 100k QPS per node |
| OLAP (analytical queries) | > 1k QPS per node |
| Time-series ingestion | > 1M rows/sec per node |
| Vector similarity search | > 10k QPS per node |

### Compression Ratios

| Data Type | Compression Algorithm | Typical Ratio |
|-----------|----------------------|---------------|
| Monotonic integers | Delta + LZ4 | 10-20x |
| Timestamps | Delta + LZ4 | 15-25x |
| Floats | Gorilla + LZ4 | 5-10x |
| Low-cardinality strings | Dictionary | 20-50x |
| High-cardinality strings | ZSTD | 3-5x |
| Boolean/flags | RLE | 50-100x |

### Storage Efficiency

| Tier | Compression | Size Reduction |
|------|-------------|----------------|
| Hot (heap) | None | 1x (baseline) |
| Warm (columnar + LZ4) | LZ4 | 5-8x |
| Cold (columnar + ZSTD) | ZSTD | 10-15x |
| Frozen (columnar + ZSTD-19) | ZSTD max | 15-25x |

---

This architecture enables Orochi DB to provide:
- **HTAP Capabilities**: Simultaneous OLTP and OLAP workloads
- **Horizontal Scalability**: Linear scaling via sharding
- **Cost Efficiency**: Tiered storage reduces storage costs by 10-20x
- **High Performance**: Columnar storage + vectorization + JIT
- **Cloud Native**: Kubernetes-first deployment model
- **Operational Simplicity**: Automated scaling, backup, monitoring
