# Orochi DB - Implementation Status and TODO List

This document tracks the implementation status, TODOs, and planned work for the Orochi DB PostgreSQL extension.

## Executive Summary

Orochi DB is a PostgreSQL extension that combines:
- **Sharding** - Hash-based distribution with co-location
- **Time-series** - Hypertables with automatic chunking
- **Columnar storage** - Multiple compression algorithms
- **Tiered storage** - S3 integration for cold data
- **Vector operations** - SIMD-optimized distance functions

**Current Status**: Core infrastructure is in place with comprehensive SQL APIs. Many backend implementations are stubbed/incomplete.

---

## Priority 1: Critical Path (Blocking Core Functionality)

### 1.1 Distributed Query Executor (`src/executor/distributed_executor.c`)

| Line | TODO | Status |
|------|------|--------|
| 125 | Implement actual remote execution (connection to worker, send query, wait for result) | **Stub** |
| 144 | Implement async wait for task completion | **Stub** |
| 153 | Send cancel to worker when task needs cancellation | **Stub** |
| 258 | Implement result iteration (get next tuple from results) | **Stub** |
| 348 | Establish connection and send query asynchronously | **Stub** |
| 360 | Check async connection for results | **Stub** |
| 386 | Actually execute on worker (aggregate pushdown) | **Stub** |
| 607 | Implement actual result streaming | **Stub** |
| 639-658 | 2PC transaction coordination (begin, prepare, commit, rollback) | **Stub** |

**Impact**: Without these, distributed queries cannot actually execute on remote workers.

### 1.2 Distributed Query Planner (`src/planner/distributed_planner.c`)

| Line | TODO | Status |
|------|------|--------|
| 558 | Implement proper query deparsing (SQL text generation) | **Stub** |
| 572 | Create final aggregation query for coordinator | **Stub** |
| 730 | Send query to worker node and collect results | **Stub** |
| 742 | Return next tuple from results | **Stub** |
| 867 | Create partial aggregation query | **Stub** |
| 880 | Create final aggregation query | **Stub** |

**Impact**: Fragment queries are placeholders; actual SQL generation missing.

### 1.3 Physical Sharding (`src/sharding/physical_sharding.c`)

| Line | TODO | Status |
|------|------|--------|
| 666 | Implement actual shard pruning based on quals analysis | **Stub** |

**Impact**: Query routing cannot prune unnecessary shards.

---

## Priority 2: High Priority (Core Features Incomplete)

### 2.1 Sharding/Distribution (`src/sharding/distribution.c`)

| Line | TODO | Status |
|------|------|--------|
| 94 | Implement proper co-location tracking in catalog | **Stub** |
| 102 | Implement co-location group creation in catalog | **Stub** |
| 377 | Implement shard movement (rebalancing) | **Stub** |

### 2.2 Time-Series/Hypertables (`src/timeseries/hypertable.c`)

| Line | TODO | Status |
|------|------|--------|
| 206 | Register dimension in catalog | **Stub** |
| 231 | Implement dimension addition | **Stub** |
| 251 | Load dimensions and other metadata | **Stub** |
| 650 | Drop physical chunk table | **Stub** |
| 667 | Store compression policy in catalog | **Stub** |
| 679 | Store retention policy in catalog | **Stub** |
| 1112 | Run all maintenance tasks | **Stub** |
| 1248 | Implement compression policy removal | **Stub** |
| 1268 | Implement retention policy removal | **Stub** |

### 2.3 Tiered Storage (`src/tiered/tiered_storage.c`)

| Line | TODO | Status |
|------|------|--------|
| 307 | Test S3 connection with HEAD bucket request | **Stub** |
| 886 | Read chunk data and upload to S3 | **Stub** |
| 1040 | Implement access tracking | **Stub** |
| 1303 | Implement proper node removal with shard migration | **Stub** |
| 1313 | Implement shard migration to other nodes | **Stub** |
| 1325 | Implement shard rebalancing | **Stub** |

---

## Priority 3: Medium Priority (Advanced Features)

### 3.1 Columnar Storage (`src/storage/columnar.c`)

| Line | TODO | Status |
|------|------|--------|
| 187 | Analyze cardinality for dictionary encoding | **Stub** |
| 311 | Compare and update min/max statistics | **Stub** |
| 530 | Implement actual storage write | **Stub** |
| 632 | Load chunk group data from storage | **Stub** |
| 747 | Find correct chunk group | **Stub** |
| 774 | Load skip list from catalog | **Stub** |
| 782 | Implement predicate evaluation against min/max | **Stub** |

### 3.2 Compression (`src/storage/compression.c`)

| Line | TODO | Status |
|------|------|--------|
| 180 | Apply zigzag encoding and simple8b_rle for delta compression | **Partial** |
| 234 | Implement proper Gorilla bit-packing | **Partial** |
| 267 | Implement dictionary encoding for low-cardinality strings | **Stub** |

### 3.3 Query Planning (`src/planner/distributed_planner.c`)

| Line | TODO | Status |
|------|------|--------|
| 278 | Implement proper shard narrowing check | **Stub** |
| 836 | Check if join is on distribution column | **Stub** |

### 3.4 Executor Features (`src/executor/distributed_executor.c`)

| Line | TODO | Status |
|------|------|--------|
| 227 | Implement dynamic parallelism adjustment | **Stub** |
| 569 | Track which cache entries depend on which tables | **Stub** |

### 3.5 Catalog (`src/core/catalog.c`)

| Line | TODO | Status |
|------|------|--------|
| 310 | Proper interval formatting | Minor |
| 891 | Proper interval formatting for tiering policies | Minor |
| 1402 | Implement column chunk creation | **Stub** |

---

## Test Coverage Analysis

### Existing Tests
| Test Suite | File | Coverage |
|------------|------|----------|
| Basic | `test/sql/basic.sql` | Extension loading, catalog, types |
| Distributed | `test/sql/distributed.sql` | Sharding, co-location, reference tables |
| Hypertable | `test/sql/hypertable.sql` | Time-series, chunking, policies |
| Columnar | `test/sql/columnar.sql` | Columnar storage, compression |
| Vector | `test/sql/vector.sql` | Vector type, distance functions |
| Tiering | `test/sql/tiering.sql` | Tier transitions, policies |

### Missing Test Coverage
1. **Multi-node distributed queries** - No actual remote execution
2. **S3 integration tests** - Need mock or local S3
3. **Continuous aggregates refresh** - Not tested
4. **Shard rebalancing** - Not implemented
5. **Connection pooling** - Not tested
6. **2PC transactions** - Not implemented
7. **SIMD vector operations** - Basic tests only

---

## Recommended Implementation Order

### Phase 1: Make Single-Node Distribution Work
1. Fix shard pruning (`physical_sharding.c:666`)
2. Implement proper query deparsing (`distributed_planner.c:558`)
3. Complete local shard execution flow

### Phase 2: Complete Time-Series
1. Dimension registration and loading (`hypertable.c:206,251`)
2. Policy storage in catalog (`hypertable.c:667,679`)
3. Maintenance task runner (`hypertable.c:1112`)

### Phase 3: Complete Columnar Storage
1. Storage write implementation (`columnar.c:530`)
2. Skip list loading and predicate pushdown (`columnar.c:774,782`)
3. Enhanced compression (delta zigzag, Gorilla bit-packing)

### Phase 4: Tiered Storage
1. Read chunk data for S3 upload (`tiered_storage.c:886`)
2. Access tracking (`tiered_storage.c:1040`)
3. S3 connection testing

### Phase 5: Multi-Node Support
1. Remote execution (`distributed_executor.c:125`)
2. Connection pooling and async I/O
3. 2PC transaction coordination
4. Result streaming

### Phase 6: Advanced Features
1. Shard rebalancing
2. Dynamic parallelism
3. Query result caching with invalidation

---

## Quick Wins (Easy Fixes)

1. Interval formatting in catalog (`catalog.c:310,891`) - String formatting
2. Co-location tracking - Add catalog table entries
3. Dictionary encoding - Simple hash table implementation
4. Access tracking - Increment counter in catalog

---

## Architecture Notes

### Current Limitations
- Distributed queries fall back to local execution
- S3 upload uses placeholder data
- No actual multi-node communication
- 2PC transactions are no-ops

### Key Design Decisions
- Uses PostgreSQL PGXS build system
- Integrates via planner/executor hooks
- Custom Scan nodes for distributed plans
- Background workers for maintenance

---

## Build Requirements

```bash
# Dependencies
sudo apt-get install -y \
    postgresql-server-dev-16 \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev

# Build
make
make install

# Test
make installcheck
```
