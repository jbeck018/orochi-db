# Orochi DB User Guide

## Installation

### Prerequisites

- PostgreSQL 16, 17, or 18 (PostgreSQL 18 recommended for best performance)
- GCC or Clang compiler
- Development libraries:
  - liblz4-dev
  - libzstd-dev
  - libcurl4-openssl-dev

### Building from Source

```bash
# Clone the repository
git clone https://github.com/your-org/orochi-db.git
cd orochi-db

# Install dependencies (Ubuntu/Debian)
make install-deps

# Build
make

# Install (requires sudo)
sudo make install
```

### PostgreSQL Configuration

Add to `postgresql.conf`:

```
shared_preload_libraries = 'orochi'
```

Restart PostgreSQL, then create the extension:

```sql
CREATE EXTENSION orochi;
```

## Distributed Tables

### Creating a Distributed Table

```sql
-- Create your table normally
CREATE TABLE orders (
    id BIGSERIAL,
    customer_id INT NOT NULL,
    order_date TIMESTAMPTZ DEFAULT NOW(),
    total DECIMAL(10,2),
    status TEXT
);

-- Distribute it
SELECT create_distributed_table(
    'orders',
    'customer_id',          -- distribution column
    shard_count => 32       -- number of shards
);
```

### Distribution Column Selection

Choose a column that:
- Has high cardinality (many distinct values)
- Is frequently used in WHERE clauses
- Is used in JOIN conditions with other tables

Good choices:
- `customer_id` for customer-centric applications
- `tenant_id` for multi-tenant SaaS
- `user_id` for user-centric applications

### Co-located Tables

Tables that are frequently joined should be co-located:

```sql
-- Create orders distributed by customer_id
SELECT create_distributed_table('orders', 'customer_id');

-- Create order_items co-located with orders
SELECT create_distributed_table(
    'order_items',
    'customer_id',
    colocate_with => 'orders'
);

-- This join is efficient (no data movement)
SELECT o.*, oi.*
FROM orders o
JOIN order_items oi ON o.customer_id = oi.customer_id AND o.id = oi.order_id
WHERE o.customer_id = 12345;
```

### Reference Tables

Small dimension tables that need to be available on all nodes:

```sql
CREATE TABLE countries (
    code CHAR(2) PRIMARY KEY,
    name TEXT
);

-- Replicate to all nodes
SELECT create_reference_table('countries');

-- Joins with reference tables are always local
SELECT o.*, c.name as country_name
FROM orders o
JOIN countries c ON o.country_code = c.code;
```

## Hypertables (Time-Series)

### Creating a Hypertable

```sql
CREATE TABLE metrics (
    time TIMESTAMPTZ NOT NULL,
    device_id INT,
    metric_name TEXT,
    value DOUBLE PRECISION
);

-- Convert to hypertable with 1-day chunks
SELECT create_hypertable(
    'metrics',
    'time',
    chunk_time_interval => INTERVAL '1 day'
);

-- Add a secondary dimension for better query performance
SELECT add_dimension('metrics', 'device_id', number_partitions => 8);
```

### Time Bucketing

```sql
-- Hourly averages
SELECT
    time_bucket('1 hour', time) AS hour,
    device_id,
    AVG(value) AS avg_value
FROM metrics
WHERE time > NOW() - INTERVAL '1 day'
GROUP BY hour, device_id;

-- With custom origin
SELECT
    time_bucket('1 day', time, '2024-01-01'::timestamptz) AS day,
    COUNT(*)
FROM metrics
GROUP BY day;
```

### Continuous Aggregates

Pre-computed aggregations that refresh incrementally:

```sql
-- Create continuous aggregate
SELECT create_continuous_aggregate(
    'hourly_metrics',
    $$
    SELECT
        time_bucket('1 hour', time) AS hour,
        device_id,
        AVG(value) AS avg_value,
        MIN(value) AS min_value,
        MAX(value) AS max_value,
        COUNT(*) AS count
    FROM metrics
    GROUP BY hour, device_id
    $$,
    refresh_interval => INTERVAL '1 hour'
);

-- Query the aggregate (fast!)
SELECT * FROM hourly_metrics
WHERE hour > NOW() - INTERVAL '7 days';
```

### Chunk Management

```sql
-- View chunks
SELECT * FROM orochi.chunks WHERE hypertable = 'metrics';

-- Compress old chunks
SELECT compress_chunk(c.chunk_id)
FROM orochi.chunks c
WHERE c.hypertable = 'metrics'
  AND c.range_end < NOW() - INTERVAL '7 days'
  AND NOT c.is_compressed;

-- Drop old data
SELECT drop_chunks('metrics', older_than => INTERVAL '1 year');
```

## Columnar Storage

### Converting to Columnar

```sql
-- Set storage type for new hypertable chunks
ALTER TABLE metrics SET (
    orochi.storage_type = 'columnar',
    orochi.compression = 'zstd'
);
```

### Compression Settings

```sql
-- Per-table compression settings
SET orochi.compression_level = 3;  -- 1-19 for ZSTD

-- Column-specific compression is automatic based on type:
-- - Integers/timestamps: Delta encoding
-- - Floats: Gorilla compression
-- - Text: ZSTD or Dictionary
-- - Boolean: RLE
```

## Tiered Storage

### Setting Up S3

```sql
-- Configure S3 credentials (superuser only)
ALTER SYSTEM SET orochi.s3_endpoint = 'https://s3.amazonaws.com';
ALTER SYSTEM SET orochi.s3_bucket = 'my-data-bucket';
ALTER SYSTEM SET orochi.s3_access_key = 'AKIA...';
ALTER SYSTEM SET orochi.s3_secret_key = 'secret...';
ALTER SYSTEM SET orochi.s3_region = 'us-east-1';
SELECT pg_reload_conf();
```

### Tiering Policies

```sql
-- Enable automatic tiering
SELECT set_tiering_policy(
    'metrics',
    hot_after => INTERVAL '1 day',    -- Keep recent data hot
    warm_after => INTERVAL '7 days',  -- Compress after 7 days
    cold_after => INTERVAL '30 days'  -- Move to S3 after 30 days
);

-- View current tier status
SELECT * FROM orochi.chunks
WHERE hypertable = 'metrics'
ORDER BY range_start DESC;
```

### Manual Tier Control

```sql
-- Move specific chunk to cold storage
SELECT move_chunk_to_tier('_orochi_chunk_12345', 'cold');

-- Recall from cold storage (may take time)
SELECT move_chunk_to_tier('_orochi_chunk_12345', 'warm');
```

## Vector Operations

### Creating Vector Tables

```sql
CREATE TABLE documents (
    id BIGSERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    embedding vector(1536)  -- OpenAI ada-002 dimension
);

-- Insert vectors
INSERT INTO documents (title, content, embedding)
VALUES ('Document 1', 'Content...', '[0.1, 0.2, ...]'::vector);
```

### Similarity Search

```sql
-- Find nearest neighbors (L2 distance)
SELECT id, title, embedding <-> query_embedding AS distance
FROM documents
ORDER BY embedding <-> query_embedding
LIMIT 10;

-- Cosine similarity
SELECT id, title, 1 - (embedding <=> query_embedding) AS similarity
FROM documents
ORDER BY embedding <=> query_embedding
LIMIT 10;
```

### Vector Indexes

```sql
-- Create IVFFlat index
CREATE INDEX ON documents USING ivfflat (embedding vector_l2_ops)
WITH (lists = 100);

-- Or HNSW for better recall
CREATE INDEX ON documents USING hnsw (embedding vector_l2_ops)
WITH (m = 16, ef_construction = 64);
```

## Cluster Management

### Adding Nodes

```sql
-- Add worker nodes
SELECT add_node('worker1.example.com', 5432);
SELECT add_node('worker2.example.com', 5432);

-- View cluster
SELECT * FROM orochi.nodes;
```

### Rebalancing

```sql
-- Rebalance all tables
SELECT rebalance_shards();

-- Rebalance specific table
SELECT rebalance_shards('orders');

-- Drain a node before removal
SELECT drain_node(2);
SELECT remove_node(2);
```

## Maintenance

### Running Maintenance

```sql
-- Run all maintenance tasks
SELECT run_maintenance();

-- This executes:
-- 1. Retention policies (drop old chunks)
-- 2. Compression policies (compress eligible chunks)
-- 3. Tiering policies (move data between tiers)
-- 4. Continuous aggregate refresh
```

### Monitoring

```sql
-- Table statistics
SELECT * FROM orochi.tables;

-- Shard distribution
SELECT
    table_name,
    COUNT(*) as shards,
    SUM(row_count) as total_rows,
    pg_size_pretty(SUM(size_bytes)) as total_size
FROM orochi.shards
GROUP BY table_name;

-- Node health
SELECT * FROM orochi.nodes;
```

## Best Practices

### Schema Design

1. **Choose distribution columns carefully** - Should be in most queries
2. **Co-locate related tables** - Avoids cross-shard joins
3. **Use reference tables for dimensions** - Small, frequently joined tables
4. **Partition time-series by time** - Enables efficient chunk pruning

### Query Optimization

1. **Include distribution column in WHERE** - Enables single-shard queries
2. **Use time filters for hypertables** - Enables chunk elimination
3. **Push aggregations down** - Let workers do partial aggregation
4. **Avoid cross-shard transactions** - They require 2PC

### Storage Optimization

1. **Enable compression for cold data** - 5-10x space savings
2. **Use tiering for historical data** - 90%+ cost reduction
3. **Choose chunk size wisely** - Balance query speed vs. management overhead
4. **Monitor storage tiers** - Ensure proper data movement

## Troubleshooting

### Common Issues

**Query is slow:**
```sql
-- Check if shards are being pruned
EXPLAIN (ANALYZE, VERBOSE) SELECT ...;
```

**Chunk not compressing:**
```sql
-- Check chunk status
SELECT * FROM orochi.chunks WHERE chunk_id = 12345;
```

**S3 upload failing:**
```sql
-- Check S3 configuration
SHOW orochi.s3_endpoint;
SHOW orochi.s3_bucket;
```

### Getting Help

- GitHub Issues: [Report bugs and feature requests]
- Documentation: See `docs/` directory
