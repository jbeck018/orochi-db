# Orochi DB Implementation Plan

This plan file is designed for use with claude-flow swarm orchestration to complete the Orochi DB PostgreSQL extension.

## Project Overview

**Goal**: Complete the Orochi DB PostgreSQL extension - a unified HTAP system combining sharding, time-series, columnar storage, tiered storage, and vector operations.

**Current State**: Core infrastructure exists with 50+ TODOs/stubs that need implementation.

---

## Phase 1: Single-Node Distribution (Priority: Critical)

### Task 1.1: Shard Pruning Implementation
**File**: `src/sharding/physical_sharding.c:666`
**Description**: Implement actual shard pruning based on quals analysis
**Steps**:
1. Parse WHERE clause predicates for distribution column
2. Extract constant values from equality predicates
3. Compute hash of constant to determine target shard
4. Return only shards that match the predicate

### Task 1.2: Query Deparsing
**File**: `src/planner/distributed_planner.c:558`
**Description**: Convert Query tree back to SQL string for shard execution
**Steps**:
1. Implement SELECT clause deparsing (target list)
2. Implement FROM clause deparsing (table references)
3. Implement WHERE clause deparsing (predicates)
4. Handle table name substitution for shard tables
5. Add GROUP BY, ORDER BY, LIMIT clause handling

### Task 1.3: Coordinator Query Generation
**File**: `src/planner/distributed_planner.c:572`
**Description**: Create final aggregation query for coordinator
**Steps**:
1. Detect aggregate functions in original query
2. Generate partial aggregate collection query
3. Map partial to final aggregate functions (e.g., SUM of SUMs)

---

## Phase 2: Time-Series Completion (Priority: High)

### Task 2.1: Dimension Registration
**File**: `src/timeseries/hypertable.c:206`
**Description**: Register dimensions in catalog when adding to hypertable
**Steps**:
1. Create catalog entry for dimension
2. Store dimension type (time vs space)
3. Store interval/partition count

### Task 2.2: Dimension Loading
**File**: `src/timeseries/hypertable.c:251`
**Description**: Load dimension metadata when accessing hypertable
**Steps**:
1. Query catalog for dimensions
2. Populate HypertableDimension structures
3. Cache for repeated access

### Task 2.3: Policy Storage
**Files**: `src/timeseries/hypertable.c:667,679`
**Description**: Persist compression and retention policies to catalog
**Steps**:
1. Implement catalog insert for compression policies
2. Implement catalog insert for retention policies
3. Add policy retrieval and update functions

### Task 2.4: Maintenance Runner
**File**: `src/timeseries/hypertable.c:1112`
**Description**: Run maintenance tasks (compression, retention, refresh)
**Steps**:
1. Query active policies from catalog
2. Identify chunks eligible for compression
3. Identify chunks eligible for retention (drop)
4. Execute continuous aggregate refreshes

---

## Phase 3: Columnar Storage (Priority: Medium-High)

### Task 3.1: Storage Write Implementation
**File**: `src/storage/columnar.c:530`
**Description**: Actually write columnar data to disk
**Steps**:
1. Serialize chunk group to bytes
2. Write to PostgreSQL storage layer
3. Update stripe metadata in catalog

### Task 3.2: Skip List Loading
**File**: `src/storage/columnar.c:774`
**Description**: Load skip list from catalog for predicate pushdown
**Steps**:
1. Query column_chunks catalog table
2. Extract min/max values per chunk
3. Build SkipList structure

### Task 3.3: Predicate Evaluation
**File**: `src/storage/columnar.c:782`
**Description**: Evaluate predicates against min/max for chunk skipping
**Steps**:
1. Extract predicate from query quals
2. Compare against chunk min/max
3. Return true if chunk can be skipped

### Task 3.4: Enhanced Compression
**File**: `src/storage/compression.c:180,234,267`
**Description**: Complete compression algorithm implementations
**Steps**:
1. Add zigzag encoding to delta compression
2. Implement proper Gorilla bit-packing
3. Implement dictionary encoding with hash table

---

## Phase 4: Tiered Storage (Priority: Medium)

### Task 4.1: Chunk Data Upload
**File**: `src/tiered/tiered_storage.c:886`
**Description**: Read actual chunk data and upload to S3
**Steps**:
1. Read columnar data from local storage
2. Serialize to uploadable format
3. Use existing S3 client to upload
4. Update catalog with S3 location

### Task 4.2: Access Tracking
**File**: `src/tiered/tiered_storage.c:1040`
**Description**: Track chunk access patterns for intelligent tiering
**Steps**:
1. Increment access counter on read
2. Record timestamp of last access
3. Calculate access frequency

### Task 4.3: S3 Connection Testing
**File**: `src/tiered/tiered_storage.c:307`
**Description**: Validate S3 configuration with HEAD bucket request
**Steps**:
1. Build HEAD request with AWS Signature V4
2. Execute request via CURL
3. Return success/failure based on response

---

## Phase 5: Multi-Node Support (Priority: Medium-Low)

### Task 5.1: Remote Execution
**File**: `src/executor/distributed_executor.c:125`
**Description**: Execute queries on remote worker nodes
**Steps**:
1. Establish libpq connection to worker
2. Send query via PQsendQuery
3. Collect results via PQgetResult
4. Handle errors and timeouts

### Task 5.2: Async Task Handling
**Files**: `src/executor/distributed_executor.c:144,348,360`
**Description**: Implement asynchronous task execution
**Steps**:
1. Use PQsendQuery for non-blocking sends
2. Use PQconsumeInput + PQisBusy for async polling
3. Implement proper timeout handling

### Task 5.3: 2PC Transaction Coordination
**Files**: `src/executor/distributed_executor.c:639-658`
**Description**: Implement two-phase commit for distributed transactions
**Steps**:
1. BEGIN on all participants
2. PREPARE TRANSACTION on all participants
3. COMMIT PREPARED on success, ROLLBACK on failure

### Task 5.4: Result Streaming
**File**: `src/executor/distributed_executor.c:607`
**Description**: Stream results from workers to coordinator
**Steps**:
1. Read rows from each worker connection
2. Merge sorted results if ORDER BY present
3. Apply LIMIT at coordinator level

---

## Phase 6: Advanced Features (Priority: Low)

### Task 6.1: Shard Rebalancing
**Files**: `src/tiered/tiered_storage.c:1313,1325`
**Description**: Move shards between nodes for load balancing
**Steps**:
1. Calculate optimal shard distribution
2. Copy shard data to target node
3. Update shard placement in catalog
4. Remove shard from source node

### Task 6.2: Dynamic Parallelism
**File**: `src/executor/distributed_executor.c:227`
**Description**: Adjust parallelism based on runtime performance
**Steps**:
1. Monitor task completion rates
2. Increase parallelism if tasks complete quickly
3. Decrease if workers are overloaded

### Task 6.3: Cache Invalidation
**File**: `src/executor/distributed_executor.c:569`
**Description**: Track cache dependencies for proper invalidation
**Steps**:
1. Record table OIDs used by cached queries
2. On table modification, invalidate dependent caches
3. Implement LRU eviction policy

---

## Testing Requirements

### Unit Tests
- [ ] Shard pruning with various predicates
- [ ] Query deparsing for SELECT/INSERT/UPDATE/DELETE
- [ ] Compression/decompression round-trip
- [ ] Skip list evaluation

### Integration Tests
- [ ] Distributed table creation and querying
- [ ] Hypertable with automatic chunking
- [ ] Columnar storage with compression
- [ ] Tier transitions (hot -> warm -> cold)

### Performance Tests
- [ ] Query latency with shard pruning
- [ ] Compression ratios for different data types
- [ ] Parallel query execution scaling

---

## Success Criteria

1. **Phase 1 Complete**: Single-node distributed queries execute correctly
2. **Phase 2 Complete**: Hypertables auto-chunk and policies execute
3. **Phase 3 Complete**: Columnar storage writes and reads with compression
4. **Phase 4 Complete**: Cold data moves to S3 and recalls on access
5. **Phase 5 Complete**: Multi-node queries work with 2PC
6. **Phase 6 Complete**: Production-ready features implemented

---

## Swarm Configuration

```yaml
topology: hierarchical
strategy: development
maxAgents: 8

agents:
  - type: architect
    name: System Architect
    focus: [planning, design-review, integration]

  - type: coder
    name: Core Developer
    focus: [sharding, distribution, query-planning]

  - type: coder
    name: Storage Developer
    focus: [columnar, compression, tiered-storage]

  - type: coder
    name: Time-Series Developer
    focus: [hypertables, chunking, policies]

  - type: tester
    name: Test Engineer
    focus: [unit-tests, integration-tests]

  - type: reviewer
    name: Code Reviewer
    focus: [security, performance, PostgreSQL-best-practices]
```

---

## Notes

- All implementations must use PostgreSQL memory contexts
- Follow PostgreSQL coding conventions
- Use ereport/elog for error handling
- Update catalog after any metadata changes
- Test with various PostgreSQL versions (14, 15, 16)
