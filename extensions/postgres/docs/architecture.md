# Orochi DB Architecture

## Overview

Orochi DB is designed as a PostgreSQL extension that adds HTAP (Hybrid Transactional/Analytical Processing) capabilities. It combines multiple storage engines, distributed query processing, and intelligent data lifecycle management into a unified system.

## Design Principles

1. **PostgreSQL Native**: Built as a standard extension using PostgreSQL's APIs
2. **Transparent Integration**: Existing PostgreSQL tools and ORMs work unchanged
3. **Separation of Concerns**: Modular components for storage, distribution, and query processing
4. **Adaptive Behavior**: Automatic optimization based on workload patterns

## Component Architecture

### 1. Core Layer (`src/core/`)

The foundation of Orochi DB:

- **init.c**: Extension initialization, GUC setup, hook registration
- **catalog.c/h**: Metadata management using PostgreSQL catalog tables

The catalog stores:
- Distributed table metadata
- Shard placement information
- Chunk boundaries
- Node registry
- Tiering policies

### 2. Storage Layer (`src/storage/`)

Multiple storage formats optimized for different workloads:

#### Row Store
- Standard PostgreSQL heap storage
- Best for: OLTP, point queries, updates

#### Columnar Store
- Column-oriented storage organized as Stripes → Chunk Groups → Column Chunks
- Multiple compression algorithms per column
- Skip lists for predicate pushdown
- Best for: Analytics, aggregations, scans

#### Compression (`compression.c`)
- LZ4: Fast compression for general use
- ZSTD: High compression ratio for cold data
- Delta: For monotonic integers/timestamps
- Gorilla: For floating-point time-series
- Dictionary: For low-cardinality strings
- RLE: For boolean/repeated values

### 3. Distribution Layer (`src/sharding/`)

Hash-based horizontal scaling:

```
Table → Shards (hash ranges) → Nodes
         ↓
    Shard 0: [INT_MIN, -1073741824)  → Node 1
    Shard 1: [-1073741824, 0)        → Node 2
    Shard 2: [0, 1073741824)         → Node 1
    Shard 3: [1073741824, INT_MAX]   → Node 2
```

Key concepts:
- **Distribution Column**: Hash key for routing
- **Co-location Groups**: Tables sharded together for local joins
- **Reference Tables**: Small tables replicated to all nodes
- **Shard Placements**: Primary/replica locations

### 4. Time-Series Layer (`src/timeseries/`)

Automatic time-based partitioning:

```
Hypertable → Chunks (time ranges) → Storage
              ↓
    Chunk 1: [2024-01-01, 2024-01-02)  → Hot tier
    Chunk 2: [2024-01-02, 2024-01-03)  → Hot tier
    Chunk 3: [2023-12-25, 2023-12-26)  → Warm tier (compressed)
    Chunk 4: [2023-06-01, 2023-06-02)  → Cold tier (S3)
```

Features:
- Automatic chunk creation
- Time bucketing functions
- Continuous aggregates with invalidation tracking
- Retention and compression policies

### 5. Tiered Storage Layer (`src/tiered/`)

Data lifecycle management:

```
HOT (Local SSD) → WARM (Local, Compressed) → COLD (S3) → FROZEN (Glacier)
     ↓                    ↓                      ↓              ↓
  1 day                7 days                30 days        Archive
```

Background workers automatically:
- Track access patterns
- Move data between tiers
- Compress on tier transition
- Upload/download from S3

### 6. Vector Layer (`src/vector/`)

AI/ML workload support:

- Custom `vector` data type
- SIMD-optimized distance calculations (AVX2/AVX-512)
- Distance functions: L2, cosine, inner product
- Index support: IVFFlat, HNSW

### 7. Query Processing (`src/planner/`, `src/executor/`)

Distributed query execution:

```
Query → Planner Hook → Distributed Plan → Custom Scan → Fragment Queries
                            ↓                              ↓
                       Shard Pruning              Parallel Execution
                            ↓                              ↓
                       Coordinator Query          Result Merging
```

Planner responsibilities:
- Identify distributed tables
- Analyze predicates for shard pruning
- Detect co-located joins
- Plan aggregate pushdown

Executor responsibilities:
- Adaptive parallelism (slow start)
- Connection pooling
- Result merging and sorting
- Error handling and retry

## Data Flow

### INSERT Path

```
INSERT → Distribution Column Hash → Shard Selection → Node Routing
                                         ↓
                              Columnar: Buffer → Chunk Group → Stripe
                              Row: Direct heap insert
```

### SELECT Path (Distributed)

```
SELECT → Planner Hook → Predicate Analysis → Shard Pruning
                              ↓
                      Fragment Query Generation
                              ↓
                      Parallel Worker Execution
                              ↓
                      Result Merging (Sort/Aggregate)
```

### SELECT Path (Columnar)

```
SELECT → Skip List Check → Chunk Elimination → Decompress Needed Columns
                                                      ↓
                                              Vectorized Evaluation
```

## Background Workers

Core background workers handle maintenance:

1. **Tiering Worker**: Moves data between storage tiers
2. **Compression Worker**: Compresses cold chunks
3. **Stats Collector**: Gathers cluster statistics
4. **Rebalancer**: Rebalances shards across nodes

When built with `RAFT_INTEGRATION=1`, two additional workers are registered:

5. **Raft Health Check Worker** (`raft_health_worker.c`): Pings cluster nodes every 30s (configurable via `orochi.health_check_interval`), updates `orochi.orochi_node_health` table, marks nodes unhealthy after configurable failure threshold
6. **Raft Log Compaction Worker** (`raft_compaction_worker.c`): Runs every 60s (configurable via `orochi.raft_compaction_interval`), triggers snapshot + log compaction when entries exceed `orochi.raft_log_max_entries` (default 10,000)

## Catalog Schema

```sql
orochi.orochi_tables          -- Distributed/hypertable metadata
orochi.orochi_shards          -- Shard definitions and placement
orochi.orochi_shard_placements -- Replica locations
orochi.orochi_chunks          -- Time-series chunk metadata
orochi.orochi_nodes           -- Cluster node registry
orochi.orochi_stripes         -- Columnar stripe metadata
orochi.orochi_column_chunks   -- Column chunk statistics
orochi.orochi_tiering_policies -- Data lifecycle policies
orochi.orochi_vector_indexes  -- Vector index metadata
orochi.orochi_continuous_aggregates -- Materialized aggregate views
orochi.orochi_invalidation_log -- Aggregate invalidation tracking
orochi.orochi_node_health      -- Node health status (Raft mode)
orochi.orochi_pipeline_queue   -- Table-based pipeline queue (non-Kafka fallback)
orochi.orochi_pipeline_queue_cursors -- Pipeline consumer offsets
orochi.orochi_cdc_events       -- CDC events (non-Kafka fallback)
orochi.orochi_webhook_queue    -- CDC webhook delivery queue
orochi.orochi_auth_webhook_queue -- Auth webhook delivery queue
```

## Extension Points

Orochi DB uses PostgreSQL's extension APIs:

- **Planner Hook**: Intercept query planning
- **Executor Hooks**: Start/Run/Finish/End
- **Table Access Method (TAM)**: Custom storage format
- **Custom Scan Nodes**: Distributed execution
- **Background Workers**: Maintenance tasks
- **Shared Memory**: Caching and coordination
- **GUC Variables**: Configuration

## PostgreSQL Version Compatibility

Orochi DB supports PostgreSQL versions 16, 17, and 18. The extension uses a compatibility layer (`src/compat.h`) to abstract API differences between versions.

### PostgreSQL 18 Features

When running on PostgreSQL 18, Orochi DB can leverage several new features:

1. **PG_MODULE_MAGIC_EXT**: Reports extension name and version via `pg_get_loaded_modules()`
2. **Asynchronous I/O (AIO)**: Improved performance for sequential scans and bulk operations
3. **Skip Scan for B-tree**: Better utilization of multicolumn indexes in distributed queries
4. **Temporal Constraints**: Enhanced time-series constraint validation
5. **UUIDv7**: Timestamp-ordered UUID generation for distributed ID assignment
6. **EXPLAIN Hooks**: Custom EXPLAIN output for distributed query plans
7. **Cumulative Statistics API**: Custom statistics tracking for Orochi operations

### Compatibility Macros

The compatibility header provides feature detection:

```c
#include "compat.h"

#if OROCHI_HAS_AIO
    // Use async I/O for better performance
#endif

#if OROCHI_HAS_TEMPORAL_CONSTRAINTS
    // Leverage temporal constraints for hypertables
#endif
```

### Building for Different Versions

```bash
# Build for PostgreSQL 18 (recommended)
PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config make

# Build for PostgreSQL 16
PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config make
```

## Optional Library Fallbacks

Orochi DB gracefully handles missing optional libraries at compile time:

- **Without librdkafka**: Pipeline and CDC operations fall back to PostgreSQL table-based queues (`orochi.orochi_pipeline_queue`, `orochi.orochi_cdc_events`). Data is stored in tables rather than streamed via Kafka.
- **Without libcurl**: S3 tiered storage functions return warnings. Auth webhook delivery queues events to `orochi.orochi_auth_webhook_queue` for external processing.
- **Without RAFT_INTEGRATION**: Single-node stubs in `raft_stubs.c` provide no-op consensus. All distributed functions return success (single-node mode).

## Future Directions

- Iceberg/Delta Lake format compatibility
- GPU acceleration for vector operations
- Read replicas with async replication
- External webhook queue processor for non-curl deployments
