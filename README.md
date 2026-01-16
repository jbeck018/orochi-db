# Orochi DB

**A Modern HTAP PostgreSQL Extension for AI, Analytics, and Massive Scale**

Orochi DB transforms PostgreSQL into a powerful Hybrid Transactional/Analytical Processing (HTAP) database, combining the best features of Citus (distributed sharding), TimescaleDB (time-series optimization), and Hydra (columnar analytics) with intelligent tiered storage and native AI/vector support.

## Features

### Automatic Sharding
- **Hash-based distribution** for horizontal scaling across nodes
- **Co-location groups** for efficient distributed joins
- **Reference tables** for dimension data replication
- **Automatic rebalancing** with near-zero-downtime shard movement
- **Smart query routing** with predicate-based shard pruning

### Time-Series Optimization
- **Hypertables** with automatic time-based partitioning
- **Chunk management** for efficient time-range queries
- **Continuous aggregates** with incremental refresh
- **Time bucketing functions** for analytics
- **Retention policies** for automatic data lifecycle

### Columnar Storage
- **Hybrid row-columnar storage** for HTAP workloads
- **Multiple compression algorithms**: ZSTD, LZ4, Delta, Gorilla, Dictionary, RLE
- **Skip lists** for predicate pushdown
- **Vectorized execution** for analytics queries
- **Per-column compression** selection based on data type

### Tiered Storage
- **Hot tier**: Local SSD, row format, fully indexed
- **Warm tier**: Local disk, compressed columnar
- **Cold tier**: S3/object storage, heavily compressed
- **Frozen tier**: S3 Glacier for long-term archive
- **Automatic data movement** based on access patterns

### AI/Vector Support
- **Native vector data type** up to 16,000 dimensions
- **Distance functions**: L2, cosine, inner product
- **SIMD optimization**: AVX2/AVX-512 acceleration
- **Vector indexing**: IVFFlat, HNSW
- **Batch operations** for efficient similarity search

### Distributed Query Execution
- **Adaptive executor** with slow-start parallelism
- **Partial aggregate pushdown** for analytics
- **Distributed transaction coordination** (2PC)
- **Automatic retry** for transient failures

## Quick Start

### Installation

```bash
# Install dependencies (Ubuntu/Debian)
# For PostgreSQL 18 (recommended)
sudo apt-get install postgresql-server-dev-18 liblz4-dev libzstd-dev libcurl4-openssl-dev

# Or for PostgreSQL 16/17
sudo apt-get install postgresql-server-dev-all liblz4-dev libzstd-dev libcurl4-openssl-dev

# Build and install
make
sudo make install

# Enable extension in PostgreSQL
psql -c "CREATE EXTENSION orochi"
```

### Basic Usage

#### Create a Distributed Table

```sql
-- Create a regular table
CREATE TABLE orders (
    id BIGSERIAL,
    customer_id INT,
    order_date TIMESTAMPTZ,
    total DECIMAL(10,2),
    status TEXT
);

-- Distribute by customer_id with 32 shards
SELECT create_distributed_table('orders', 'customer_id', shard_count => 32);
```

#### Create a Hypertable (Time-Series)

```sql
-- Create a metrics table
CREATE TABLE metrics (
    time TIMESTAMPTZ NOT NULL,
    device_id INT,
    temperature FLOAT,
    humidity FLOAT
);

-- Convert to hypertable with daily chunks
SELECT create_hypertable('metrics', 'time', chunk_time_interval => INTERVAL '1 day');

-- Add space partitioning
SELECT add_dimension('metrics', 'device_id', number_partitions => 4);
```

#### Time Bucketing

```sql
-- Aggregate by hour
SELECT
    time_bucket('1 hour', time) AS hour,
    device_id,
    AVG(temperature) AS avg_temp
FROM metrics
WHERE time > NOW() - INTERVAL '1 day'
GROUP BY hour, device_id
ORDER BY hour;
```

#### Vector Similarity Search

```sql
-- Create embeddings table
CREATE TABLE embeddings (
    id BIGSERIAL PRIMARY KEY,
    content TEXT,
    embedding vector(1536)
);

-- Find similar items
SELECT id, content, embedding <-> '[0.1, 0.2, ...]'::vector AS distance
FROM embeddings
ORDER BY embedding <-> '[0.1, 0.2, ...]'::vector
LIMIT 10;
```

#### Tiered Storage

```sql
-- Set tiering policy
SELECT set_tiering_policy(
    'metrics',
    hot_after => INTERVAL '1 day',
    warm_after => INTERVAL '7 days',
    cold_after => INTERVAL '30 days'
);

-- Manually move chunk to cold storage
SELECT move_chunk_to_tier('_orochi_chunk_12345', 'cold');
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Orochi DB                                │
├─────────────────────────────────────────────────────────────────┤
│  SQL Interface                                                   │
│  ├── create_distributed_table()  ├── create_hypertable()        │
│  ├── time_bucket()               ├── vector operations          │
│  └── compression/tiering APIs    └── continuous aggregates      │
├─────────────────────────────────────────────────────────────────┤
│  Query Planner                    │  Distributed Executor        │
│  ├── Shard pruning               │  ├── Adaptive parallelism    │
│  ├── Join co-location            │  ├── Result merging          │
│  └── Aggregate pushdown          │  └── 2PC coordination        │
├─────────────────────────────────────────────────────────────────┤
│  Storage Engines                                                 │
│  ├── Row Store (PostgreSQL heap)                                │
│  ├── Columnar Store (stripes, chunks, compression)              │
│  ├── Vector Store (SIMD-optimized)                              │
│  └── Tiered Storage (hot/warm/cold/frozen)                      │
├─────────────────────────────────────────────────────────────────┤
│  Distribution Layer                                              │
│  ├── Hash/Range sharding         ├── Node management            │
│  ├── Co-location groups          ├── Rebalancing                │
│  └── Reference tables            └── Logical replication        │
├─────────────────────────────────────────────────────────────────┤
│  PostgreSQL Core                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Comparison

| Feature | PostgreSQL | Citus | TimescaleDB | Orochi DB |
|---------|------------|-------|-------------|-----------|
| Sharding | ❌ | ✅ | Partial | ✅ |
| Time-series | ❌ | ❌ | ✅ | ✅ |
| Columnar | ❌ | ❌ | ✅ (Hypercore) | ✅ |
| Tiered Storage | ❌ | ❌ | ❌ | ✅ |
| Vector/AI | Via pgvector | Via pgvector | ❌ | ✅ Native |
| Continuous Aggregates | ❌ | ❌ | ✅ | ✅ |
| S3 Integration | ❌ | ❌ | ❌ | ✅ |

## Use Cases

- **Real-Time Analytics**: Dashboards, reporting, ad-hoc queries
- **AI/ML Workloads**: Embedding storage, similarity search, RAG applications
- **IoT and Telemetry**: Sensor data, time-series metrics
- **Multi-Tenant SaaS**: Tenant isolation, scalable storage

## Documentation

- [Architecture Overview](docs/architecture.md)
- [User Guide](docs/user-guide.md)

## Contributing

Contributions welcome! Please open issues for proposals and RFCs before large changes.

## License

TBD

## Acknowledgments

Inspired by [Citus](https://github.com/citusdata/citus), [TimescaleDB](https://github.com/timescale/timescaledb), and [Hydra](https://github.com/hydradatabase/columnar).

---

**Orochi DB** - Unleash the power of PostgreSQL for modern data workloads.
