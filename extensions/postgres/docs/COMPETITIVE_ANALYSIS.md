# Orochi DB Competitive Analysis

## Executive Summary

This document analyzes leading HTAP databases to identify features Orochi DB should consider implementing to remain competitive in the 2025 market.

## Competitor Overview

### TiDB (PingCAP)
- **Architecture**: TiKV (row-store) + TiFlash (column-store) with Raft consensus
- **License**: Apache 2.0 (fully open source)
- **Compatibility**: MySQL wire protocol
- **Strengths**: Real-time HTAP, automatic sharding, placement rules

### CockroachDB (Cockroach Labs)
- **Architecture**: Unified storage with ColFlow vectorized engine
- **License**: Enterprise (changed from open source in 2024)
- **Compatibility**: PostgreSQL wire protocol
- **Strengths**: Global consistency, geo-distribution, follower reads

### SingleStore (formerly MemSQL)
- **Architecture**: Memory-first rowstores + compressed columnstores
- **License**: Proprietary
- **Compatibility**: MySQL wire protocol
- **Strengths**: Sub-millisecond latency, code-gen vectorization, pipelines

### TimescaleDB (Timescale)
- **Architecture**: PostgreSQL extension with hypertables
- **License**: Apache 2.0
- **Compatibility**: Native PostgreSQL
- **Strengths**: Time-series optimization, 90%+ compression, Hypercore engine

### ClickHouse (ClickHouse Inc)
- **Architecture**: Columnar MergeTree with LSM-like storage
- **License**: Apache 2.0
- **Compatibility**: Custom SQL dialect
- **Strengths**: Compression codecs, approximate algorithms, projections

---

## Feature Gap Analysis

### Priority 1: Critical Missing Features

| Feature | Competitors | Orochi Status | Impact |
|---------|-------------|---------------|--------|
| **Raft/Paxos Consensus** | TiDB, CockroachDB | ❌ Missing | High - Required for true distributed consistency |
| **Real-time Pipelines** | SingleStore | ❌ Missing | High - Kafka/S3 ingestion without middleware |
| **Vectorized Execution** | All | ⚠️ Partial (SIMD vectors only) | High - Query performance |
| **JIT Query Compilation** | SingleStore, ClickHouse | ❌ Missing | High - 10x+ query speedup |
| **Follower Reads** | CockroachDB, TiDB | ❌ Missing | Medium - Read scalability |

### Priority 2: High-Value Features

| Feature | Competitors | Orochi Status | Impact |
|---------|-------------|---------------|--------|
| **Placement Rules** | TiDB | ❌ Missing | High - Data locality control |
| **MPP (Massively Parallel Processing)** | TiDB TiFlash | ⚠️ Basic | High - Analytics performance |
| **Global Secondary Indexes** | TiDB, CockroachDB | ❌ Missing | Medium - Query flexibility |
| **Resource Pools/Workload Management** | SingleStore | ❌ Missing | Medium - Multi-tenant support |
| **Native JSON with Columnar Storage** | SingleStore | ⚠️ Basic | Medium - Semi-structured data |

### Priority 3: Competitive Differentiators

| Feature | Competitors | Orochi Status | Impact |
|---------|-------------|---------------|--------|
| **Continuous Aggregates (real-time)** | TimescaleDB | ✅ Implemented | - |
| **Approximate Algorithms (HyperLogLog)** | ClickHouse | ❌ Missing | Medium - Cardinality estimation |
| **Materialized Views with Refresh** | ClickHouse, TimescaleDB | ⚠️ Basic | Medium - Pre-computed analytics |
| **Projections** | ClickHouse | ❌ Missing | Medium - Pre-sorted data views |
| **Full-Text Search** | SingleStore | ❌ Missing | Low - Text analytics |
| **Geospatial Functions** | SingleStore, PostGIS | ⚠️ Via PostGIS | Low - Location analytics |

---

## Detailed Feature Recommendations

### 1. Raft Consensus Protocol

**What competitors do:**
- TiDB uses Multi-Raft with Region-based sharding
- CockroachDB uses Raft with range-based replication

**Recommendation for Orochi:**
```
- Implement Raft consensus for shard replicas
- Add leader election and log replication
- Support learner replicas (like TiDB's Raft Learner for TiFlash)
```

**Implementation complexity:** High (3-6 months)

---

### 2. Real-Time Data Pipelines

**What SingleStore does:**
```sql
CREATE PIPELINE kafka_events
AS LOAD DATA KAFKA 'broker:9092/events'
INTO TABLE events;
```

**Recommendation for Orochi:**
```sql
-- Proposed syntax
CREATE PIPELINE sensor_data
AS LOAD DATA FROM KAFKA 'broker:9092/sensors'
INTO HYPERTABLE sensor_readings
WITH (
    format = 'json',
    batch_size = 10000,
    exactly_once = true
);
```

**Implementation complexity:** Medium (2-3 months)

---

### 3. Vectorized Query Execution

**What competitors do:**
- SingleStore: Code-gen vectorization with LLVM
- ClickHouse: Batch-oriented columnar processing
- CockroachDB: ColFlow engine

**Current Orochi state:**
- SIMD for vector distance operations only

**Recommendation:**
```
- Extend SIMD to filter/aggregate operations
- Implement batch-oriented tuple processing
- Consider LLVM JIT for hot query paths
```

**Implementation complexity:** High (4-6 months)

---

### 4. Placement Rules

**What TiDB does:**
```sql
ALTER TABLE orders SET TIFLASH REPLICA 2;
ALTER PLACEMENT POLICY hot_data
  PRIMARY_REGION="us-east" REGIONS="us-east,us-west";
```

**Recommendation for Orochi:**
```sql
-- Proposed syntax
CREATE PLACEMENT POLICY hot_tier
  STORAGE_TIER = 'hot'
  TABLESPACE = 'nvme_fast'
  REPLICA_COUNT = 3;

ALTER TABLE events SET PLACEMENT POLICY hot_tier
  WHERE time > NOW() - INTERVAL '7 days';
```

**Implementation complexity:** Medium (2-3 months)

---

### 5. Approximate Query Processing

**What ClickHouse does:**
```sql
SELECT uniqHLL12(user_id) FROM events;  -- HyperLogLog
SELECT quantileTDigest(0.99)(latency) FROM requests;  -- T-Digest
SELECT * FROM events SAMPLE 0.1;  -- 10% sampling
```

**Recommendation for Orochi:**
```sql
-- Proposed functions
SELECT approx_count_distinct(user_id) FROM events;
SELECT approx_percentile(latency, 0.99) FROM requests;
SELECT * FROM events TABLESAMPLE BERNOULLI(10);
```

**Implementation complexity:** Medium (1-2 months)

---

### 6. Native JSON Columnar Storage

**What SingleStore does:**
- Schema inference from JSON keys
- Columnar storage by JSON path
- Parquet-like encoding

**Recommendation for Orochi:**
```sql
CREATE TABLE events (
    id BIGINT,
    data JSONB USING COLUMNAR,  -- Store JSON columnar
    time TIMESTAMPTZ
) USING orochi_columnar;

-- Automatic path extraction
SELECT data::user::name, data::metrics::latency
FROM events
WHERE data::type = 'request';
```

**Implementation complexity:** Medium (2-3 months)

---

## Competitive Positioning

### Orochi DB Strengths (vs Competitors)
1. **PostgreSQL Native** - Full compatibility, no migration
2. **Unified Extension** - Single install for sharding + time-series + columnar
3. **Open Source** - Apache 2.0 license (unlike CockroachDB's change)
4. **Tiered Storage** - Built-in S3 cold tier (like TiDB placement rules)
5. **Vector Operations** - Native SIMD (ahead of most HTAP DBs)

### Orochi DB Weaknesses (to Address)
1. **No Consensus Protocol** - Limited to single-coordinator
2. **No Real-Time Ingestion** - Requires external ETL
3. **Basic Vectorization** - Query engine not fully vectorized
4. **No JIT Compilation** - Interpreter overhead
5. **No Approximate Algorithms** - Must scan all data

---

## Recommended Roadmap

### Phase 1: Foundation (Q1)
- [ ] Implement Raft consensus for distributed commits
- [ ] Add follower read support
- [ ] Enhance MPP with shuffle operations

### Phase 2: Performance (Q2)
- [ ] Vectorized filter/aggregate operations
- [ ] Approximate algorithms (HyperLogLog, T-Digest)
- [ ] Query result caching with invalidation

### Phase 3: Usability (Q3)
- [ ] Real-time pipelines (Kafka, S3)
- [ ] Placement rules for data tiering
- [ ] Enhanced JSON columnar storage

### Phase 4: Enterprise (Q4)
- [ ] Resource pools and workload isolation
- [ ] Global secondary indexes
- [ ] JIT query compilation (optional)

---

## Sources

- [TiDB vs SingleStore Architecture](https://www.pingcap.com/blog/real-world-htap-a-look-at-tidb-and-singlestore-and-their-architectures/)
- [Best HTAP Databases 2025](https://www.getgalaxy.io/learn/data-tools/best-htap-databases-2025)
- [Distributed SQL Comparison](https://sanj.dev/post/distributed-sql-databases-comparison)
- [TiDB vs CockroachDB](https://www.bytebase.com/blog/tidb-vs-cockroachdb/)
- [TimescaleDB Releases](https://github.com/timescale/timescaledb/releases)
