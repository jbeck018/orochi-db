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

Four background workers handle maintenance:

1. **Tiering Worker**: Moves data between storage tiers
2. **Compression Worker**: Compresses cold chunks
3. **Stats Collector**: Gathers cluster statistics
4. **Rebalancer**: Rebalances shards across nodes

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

## Future Directions

- CDC (Change Data Capture) support
- Iceberg/Delta Lake format compatibility
- GPU acceleration for vector operations
- Kubernetes operator for deployment
- Read replicas with async replication
