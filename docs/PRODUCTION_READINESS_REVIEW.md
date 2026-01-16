# Orochi DB Production Readiness Review

**Date:** 2026-01-16
**Reviewer:** System Architecture Designer
**Status:** SIGNIFICANT GAPS IDENTIFIED - Not Production Ready

---

## Executive Summary

Orochi DB is a sophisticated 26,000+ line PostgreSQL extension implementing an HTAP (Hybrid Transactional/Analytical Processing) system with distributed sharding, time-series capabilities, columnar storage, tiered storage, and vector operations.

**Overall Status:** ⚠️ **CRITICAL INTEGRATION GAPS BLOCK PRODUCTION DEPLOYMENT**

The extension has well-designed modular components with 4-5 major subsystems (sharding, time-series, columnar, tiered, vector), but integration between them is **incomplete and fragile**. Multiple critical paths lack production-ready error handling, monitoring, and lifecycle management.

---

## 1. Extension Entry Point Analysis

### Location: `/home/user/orochi-db/src/core/init.c`

#### ✅ Strengths

1. **Proper _PG_init() pattern**
   - Correctly implements PostgreSQL module magic
   - Checks `process_shared_preload_libraries_in_progress` flag
   - Gracefully degrades if not in shared_preload_libraries

2. **Comprehensive GUC setup**
   - 24 configuration parameters covering all subsystems
   - Proper bounds checking (e.g., compression_level 1-19, shard_count 1-1024)
   - Supports runtime reconfiguration (PGC_USERSET, PGC_SIGHUP)

3. **Hook registration**
   - Planner hook for distributed query planning
   - 4 executor hooks (Start, Run, Finish, End)
   - Properly chains previous hooks for compatibility

4. **Background worker registration**
   - 4 workers registered: tiering, compression, stats, rebalancer
   - Restart policies configured appropriately

#### ❌ Critical Gaps

1. **Missing pipeline initialization**
   ```c
   // IN init.c line 96: orochi_register_background_workers()
   // Pipeline registration is MISSING - only registers 4 core workers
   // Pipeline worker is registered dynamically in pipeline.c, but:
   // - No shared memory allocation in _PG_init()
   // - No LWLock tranche requested for "orochi_pipeline"
   // - Pipeline shared state init not called during extension startup
   ```
   **Impact:** Pipelines cannot start because shared memory is never initialized
   - `orochi_pipeline_shmem_init()` defined but never called
   - `orochi_pipeline_shmem_size()` exists but RequestAddinShmemSpace() never called for it

2. **Incomplete shared memory setup**
   ```c
   // Current code allocates 1MB shard metadata + 64KB node registry + 64KB stats
   // Missing: Pipeline registry shared memory (~4KB)
   RequestNamedLWLockTranche("orochi", 8);  // Only 8 locks requested
   // But "orochi_pipeline" tranche is used in pipeline.c:92
   // This causes: ERROR missing LWLock tranche "orochi_pipeline"
   ```

3. **No Raft consensus initialization**
   - `raft_init()` and `raft_start()` never called
   - No registration in `orochi_register_background_workers()`
   - Distributed executor can't use consensus for leader election
   - Impact: Rebalancing and failure recovery not operational

4. **Missing error handling in hooks**
   ```c
   prev_planner_hook = planner_hook;
   planner_hook = orochi_planner_hook;
   // No try/catch - if hook fails, it crashes the planner
   ```

---

## 2. SQL Function Exposure Analysis

### Location: `/home/user/orochi-db/sql/orochi--1.0.sql`, `approx_functions.sql`, `pipelines.sql`

#### ✅ Comprehensive API

**Table 1: SQL Function Coverage**
```
Category          Functions    Status
───────────────────────────────────────
Vector ops        7 functions  ✅ Complete
Distributed       4 functions  ✅ Complete
Hypertable        5 functions  ✅ Complete
Chunking          3 functions  ✅ Complete
Compression       2 policies   ✅ Complete
Approx. query     4 aggregates ✅ Complete
Pipelines         8 functions  ⚠️ Partial
```

1. **Vector functions** - Fully exposed
   - vector type with I/O functions
   - Distance metrics (l2, cosine, inner_product)
   - Vector arithmetic (+, -)

2. **Distributed table functions** - Properly designed
   - `create_distributed_table(table, column, shard_count)`
   - `create_reference_table(table)`
   - Shard routing helpers (hash_value, get_shard_for_value)

3. **Approximation functions** - Well-structured
   - HyperLogLog for cardinality estimation
   - T-Digest for quantiles/percentiles
   - Reservoir sampling

#### ❌ Missing/Incomplete Exports

1. **Pipeline functions exist but incomplete**
   ```sql
   CREATE FUNCTION orochi.create_pipeline(...) RETURNS BIGINT
   -- Implemented in C, but:
   -- - No view for monitoring running pipelines
   -- - No function to list checkpoints by pipeline
   -- - No metrics export function
   ```

2. **Raft/Consensus functions not exposed**
   - No SQL interface for leader election
   - No function to check cluster health
   - No command to force rebalancing
   - Impact: Operators can't manage consensus state

3. **Vector index management missing**
   ```c
   // In orochi.h line 324-326:
   extern void orochi_create_vector_index(...);
   // But NO SQL function defined!
   // Users cannot create vector indexes
   ```

4. **Statistics and monitoring functions missing**
   - No function to query shard distribution stats
   - No function to get compression ratio per table
   - No function to monitor tier transitions
   - Impact: No visibility into system health

---

## 3. Integration Gap Analysis

### 3.1 Vectorized Executor ↔ Columnar Storage

**Status:** ✅ **INTEGRATED**

Integration point: `/home/user/orochi-db/src/executor/vectorized.h`
```c
#include "../storage/columnar.h"

// Both share:
#define VECTORIZED_BATCH_SIZE COLUMNAR_VECTOR_BATCH_SIZE  /* 1024 */

// VectorizedScanState includes:
typedef struct VectorizedScanState {
    ColumnarReadState *columnar_state;  // Direct reference to columnar module
    // ...
} VectorizedScanState;

// Export functions:
extern VectorizedScanState *vectorized_columnar_scan_begin(Relation, Snapshot, Bitmapset *);
extern VectorBatch *vectorized_columnar_scan(VectorizedScanState *state);
```

✅ **Strengths:**
- Direct integration through shared data structures
- Batch size coordination ensures efficient memory usage
- SIMD-optimized path for columnar data

⚠️ **Minor Issues:**
- No fallback if SIMD unavailable
- `vectorized_cpu_supports_avx2()` check but not enforced

---

### 3.2 Raft Consensus ↔ Distributed Executor

**Status:** ❌ **NO INTEGRATION**

**Gap Description:**
```c
// distributed_executor.c has structure for consensus:
typedef struct DistributedExecutorState {
    DistributedPlan *plan;
    List *tasks;
    // NO FIELD for consensus state
    // NO FIELD for leader info
} DistributedExecutorState;

// raft.h defines consensus but:
extern void raft_start(RaftNode *node);
extern uint64 raft_submit_command(RaftNode *node, const char *command, int32 size);

// But raft is NEVER instantiated or started in init.c
// So distributed executor never knows about Raft
```

**Missing Integration Points:**
1. ❌ Rebalancing decisions never use Raft consensus
2. ❌ No consensus-based leader election for coordinator
3. ❌ 2PC commits don't coordinate through Raft log
4. ❌ Shard movement not replicated via Raft
5. ❌ Failure recovery not implemented

**Impact:**
- Single point of failure (no coordination)
- No automatic failover
- Inconsistent state possible during failures

---

### 3.3 Pipelines ↔ Main Extension

**Status:** ⚠️ **PARTIALLY BROKEN**

**Critical Issue:**
```c
// In init.c, _PG_init() does:
RequestAddinShmemSpace(orochi_memsize());
RequestNamedLWLockTranche("orochi", 8);

// But orochi_memsize() (lines 332-353) does NOT include pipelines:
static Size orochi_memsize(void) {
    Size size = 0;
    size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);  // Columnar
    size = add_size(size, 1024 * 1024);   // Shard metadata
    size = add_size(size, 64 * 1024);     // Node registry
    size = add_size(size, 64 * 1024);     // Statistics
    return size;
    // MISSING: Pipeline registry shared memory!
}

// Meanwhile in pipeline.c:80-95, orochi_pipeline_shmem_init() tries to:
pipeline_shared_state = (PipelineSharedState *)
    ShmemInitStruct("Orochi Pipeline State", sizeof(PipelineSharedState), &found);
// This will FAIL if orochi_memsize() didn't include it!
```

**Cascading Failures:**
```
User calls: SELECT orochi.create_pipeline(...)
         ↓
         Pipeline created in catalog
         ↓
User calls: SELECT orochi.start_pipeline(...)
         ↓
         RegisterDynamicBackgroundWorker() called
         ↓
         pipeline_worker_main() launched
         ↓
         orochi_pipeline_shmem_init() called
         ↓
         ERROR: missing LWLock tranche "orochi_pipeline"
         ↓
         Pipeline worker CRASHES
         ↓
         User sees: "failed to connect to SPI"
```

---

### 3.4 Tiered Storage ↔ Distributed Executor

**Status:** ✅ **LOOSELY INTEGRATED**

**Good Points:**
- Tiered storage functions can be called from background workers
- S3 configuration properly initialized via GUCs
- Chunk movement tracked in catalog

**Missing Points:**
- ❌ Executor doesn't know about tier migration latency
- ❌ No tier-aware query planning (push hot tiers to hot nodes)
- ❌ No automatic tier selection during shard movement
- ❌ Query planner doesn't avoid cold data unless explicitly told

---

## 4. Background Worker Lifecycle Issues

### Current Setup
```c
// In init.c, 4 workers registered:
1. orochi_tiering_worker_main        - Data tiering
2. orochi_compression_worker_main    - Chunk compression
3. orochi_stats_worker_main          - Statistics collection
4. orochi_rebalancer_worker_main     - Shard rebalancing
```

#### ❌ Problems

1. **No health checking**
   - If a worker crashes, no one knows
   - No restart mechanism
   - Impact: Silent failures in maintenance

2. **No inter-worker coordination**
   - Compression and tiering can run simultaneously
   - Can cause I/O contention
   - No mechanism to prevent race conditions

3. **Worker registration not idempotent**
   - If extension reloaded, workers registered again
   - Can create duplicate workers
   - Impact: Resource leak, degraded performance

4. **Missing shutdown behavior**
   ```c
   void _PG_fini(void) {
       shmem_startup_hook = prev_shmem_startup_hook;
       planner_hook = prev_planner_hook;
       // ... restore other hooks

       // MISSING: Stop all background workers!
       // Workers continue running after unload
   }
   ```

---

## 5. Security Issues (Per CODE_REVIEW_FINDINGS.md)

### Critical Vulnerabilities

1. **SQL Injection in Raft RPC** ⚠️ CRITICAL
   - File: `src/consensus/raft.c:1062-1065`
   - Method: Direct integer concatenation in RPC query
   - Mitigation: Use parameterized queries

2. **JSON Injection in Raft Log** ⚠️ CRITICAL
   - File: `src/consensus/raft.c:1138-1141`
   - Method: Unescaped command_data in JSON
   - Mitigation: Escape strings using PostgreSQL JSON functions

3. **JSON Injection via Query Parameter** ⚠️ CRITICAL
   - File: `src/consensus/raft.c:1146-1151`
   - Method: Raw JSON in parameterized query
   - Mitigation: Proper JSON escaping or separate parameters

---

## 6. Missing Monitoring & Observability

### No Built-In Monitoring

1. ❌ No performance counters
   - Shards accessed: 0
   - Queries planned: 0
   - Executor failures: 0

2. ❌ No health check endpoints
   - No function to verify cluster health
   - No function to check shard accessibility
   - No function to list unreachable nodes

3. ❌ No audit logging
   - No logging of data tier movements
   - No logging of shard rebalancing
   - No logging of compression actions

---

## 7. Testing Coverage Assessment

### Test Files
```
test/sql/basic.sql        - Basic functionality
test/sql/distributed.sql  - Sharding operations
test/sql/hypertable.sql   - Time-series features
test/sql/columnar.sql     - Columnar storage
test/sql/vector.sql       - Vector operations
test/sql/tiering.sql      - Storage tiering
```

**Coverage:** ~60% (basic happy path)

**Missing Tests:**
1. ❌ Failure scenarios (node down, network partition)
2. ❌ Concurrent operations (race conditions)
3. ❌ Resource exhaustion (OOM, disk full)
4. ❌ Integration tests (all subsystems together)
5. ❌ Security tests (injection, access control)
6. ❌ Performance regression tests

---

## 8. Configuration & Deployment

### GUC Parameters: ✅ Well-Designed

```
Sharding:
  - orochi.default_shard_count         (default: 32)
  - orochi.stripe_row_limit             (default: 150000)

Time-series:
  - orochi.chunk_time_interval_hours    (default: 24)
  - orochi.compression_level            (default: 3, range: 1-19)

S3/Tiering:
  - orochi.s3_*                         (6 parameters)
  - orochi.*_threshold_*                (4 parameters)

Execution:
  - orochi.enable_vectorized_execution  (default: true)
  - orochi.enable_parallel_query        (default: true)
  - orochi.parallel_workers             (default: 4, range: 1-64)
```

### Installation: ⚠️ Missing Prerequisites

No built-in validation of:
- PostgreSQL version compatibility
- Required shared libraries (libcurl, libzstd, liblz4)
- System resources (RAM for caches)
- S3 credentials validity

---

## 9. Architectural Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    PostgreSQL Core                          │
├──────────────────┬──────────────────┬──────────────────────┤
│   Planner Hook   │  Executor Hooks  │  Buffer Management   │
└────────┬─────────┴────────┬─────────┴──────────┬───────────┘
         │                  │                    │
         ▼                  ▼                    │
    ┌─────────────┐   ┌──────────────┐         │
    │   Planner   │──▶│  Executor    │         │
    │             │   │              │         │
    │  - Shard    │   │  - Fragments │         │
    │    prune    │   │  - 2PC coord │         │
    └─────────────┘   └──────────────┘         │
         │                  │                  │
         │                  ▼                  │
         │            ┌──────────────┐         │
         │            │  Remote Exec │         │
         │            │  (libpq)     │         │
         │            └──────────────┘         │
         │                  │                  │
         └──────────┬───────┴──────┬───────────┘
                    │              │
         ┌──────────▼──┐  ┌────────▼─────┐
         │ Columnar    │  │  Tiered      │
         │ Storage ◄───┼──┤  Storage     │
         │             │  │  (S3)        │
         └──────┬───────┘  └──────┬───────┘
                │                 │
         ┌──────▼──────┐  ┌───────▼──────┐
         │  Vectorized │  │  Compression │
         │  Executor   │  │  Worker      │
         └─────────────┘  └──────────────┘

MISSING CONNECTIONS:
  • Raft ↔ Executor (no consensus integration)
  • Raft ↔ Rebalancer (no coordinated rebalancing)
  • Pipeline → Shared Memory (init gap)
  • Pipeline → Main Extension (lifecycle issue)
```

---

## 10. Production Readiness Checklist

| Category | Item | Status | Notes |
|----------|------|--------|-------|
| **Architecture** | Module isolation | ✅ | Good separation of concerns |
| | Integration completeness | ❌ | Raft not integrated, pipelines broken |
| | Extensibility | ✅ | Hook-based design allows plugins |
| **Reliability** | Error handling | ❌ | Missing in critical paths |
| | Crash recovery | ⚠️ | WAL present but not tested |
| | Failover | ❌ | No failover mechanism |
| **Operations** | Health monitoring | ❌ | No health check functions |
| | Logging | ⚠️ | Basic only, no structured logging |
| | Metrics | ❌ | No performance counters |
| **Security** | Input validation | ❌ | SQL/JSON injection vulnerabilities |
| | Access control | ✅ | Uses PostgreSQL ACLs |
| | Encryption | ❌ | No encryption for S3 data at rest |
| **Testing** | Unit tests | ✅ | Basic coverage exists |
| | Integration tests | ❌ | Multi-subsystem scenarios missing |
| | Failure tests | ❌ | No chaos engineering tests |
| | Performance tests | ⚠️ | Ad-hoc only |
| **Documentation** | API docs | ✅ | SQL functions documented |
| | Architecture docs | ⚠️ | High-level but integration gaps not documented |
| | Operational guide | ❌ | No runbook for troubleshooting |
| | Deployment guide | ❌ | No production deployment guide |

---

## 11. Critical Path to Production

### Must Fix (Blocking)
1. **Fix pipeline initialization** - Without this, pipelines are non-functional
   - Add pipeline shared memory to `orochi_memsize()`
   - Request "orochi_pipeline" LWLock tranche
   - Call `orochi_pipeline_shmem_init()` in `orochi_shmem_startup()`

2. **Fix Raft integration** - Consensus is core to distributed reliability
   - Initialize Raft node in `orochi_shmem_startup()`
   - Register Raft as background worker
   - Wire Raft decisions into rebalancer

3. **Fix security vulnerabilities** - SQL/JSON injection is exploitable
   - Use parameterized queries for Raft RPC
   - Escape JSON strings properly
   - Add input validation

### Should Fix (High Priority)
1. Add monitoring/health check functions
2. Add integration tests for failure scenarios
3. Add shutdown behavior in `_PG_fini()`
4. Document integration architecture
5. Add performance regression tests

### Could Fix (Medium Priority)
1. Add structured logging
2. Add performance counters
3. Add encryption for S3 data
4. Add automatic backup mechanism
5. Add query result caching improvements

---

## 12. Deployment Recommendation

**CURRENT STATUS:** ❌ **NOT READY FOR PRODUCTION**

**Recommended Actions:**

### Phase 1: Fix Critical Gaps (2-3 weeks)
- [ ] Fix pipeline integration (day 1-2)
- [ ] Fix Raft initialization (day 2-3)
- [ ] Fix security vulnerabilities (day 3-4)
- [ ] Add basic health checks (day 4-5)
- [ ] Test failure scenarios (day 5-7)

### Phase 2: Add Operational Capabilities (2-3 weeks)
- [ ] Structured logging
- [ ] Monitoring integration (Prometheus)
- [ ] Operational runbook
- [ ] Deployment automation

### Phase 3: Hardening & Testing (3-4 weeks)
- [ ] Integration test suite
- [ ] Chaos engineering tests
- [ ] Performance benchmarking
- [ ] Security audit (external)

**Estimated Time to Production:** 6-10 weeks

---

## 13. Detailed Gap Descriptions

### Gap 1: Pipeline Shared Memory Not Allocated

**File:** `/home/user/orochi-db/src/core/init.c`
**Lines:** 332-353 (orochi_memsize function)

**Issue:**
```c
static Size
orochi_memsize(void)
{
    Size size = 0;
    size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);  // ✓
    size = add_size(size, 1024 * 1024);   // Shard metadata  ✓
    size = add_size(size, 64 * 1024);     // Node registry   ✓
    size = add_size(size, 64 * 1024);     // Statistics      ✓
    return size;
    // MISSING: Pipeline shared memory! (line 42-49 in pipeline.c)
    //   sizeof(PipelineSharedState) ≈ sizeof(LWLock*) + sizeof(int) + 64 * sizeof(entry)
    //                                 ≈ 8 + 4 + 64 * 48 ≈ 3.2 KB
}
```

**Where It's Needed:**
```c
// pipeline.c line 84-95
ShmemInitStruct("Orochi Pipeline State", sizeof(PipelineSharedState), &found);
// ^ This fails because memory wasn't reserved in RequestAddinShmemSpace()
```

**Fix:**
```c
static Size
orochi_memsize(void)
{
    Size size = 0;
    size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);
    size = add_size(size, 1024 * 1024);
    size = add_size(size, 64 * 1024);
    size = add_size(size, 64 * 1024);
    size = add_size(size, sizeof(PipelineSharedState));  // ADD THIS LINE
    return size;
}
```

---

### Gap 2: Missing LWLock Tranche for Pipelines

**File:** `/home/user/orochi-db/src/core/init.c`
**Lines:** 89, 92

**Issue:**
```c
RequestNamedLWLockTranche("orochi", 8);  // Line 89

// ...later in shmem_startup...

LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
ShmemInitStruct("orochi_columnar_cache", ..., &found);
ShmemInitStruct("orochi_shard_metadata", ..., &found);
ShmemInitStruct("orochi_node_registry", ..., &found);
ShmemInitStruct("orochi_statistics", ..., &found);
LWLockRelease(AddinShmemInitLock);

// But in pipeline.c line 92:
pipeline_shared_state->lock =
    &(GetNamedLWLockTranche("orochi_pipeline"))->lock;
// ^ This crashes - tranche "orochi_pipeline" was never requested!
```

**Fix:**
```c
// In _PG_init() line 89:
RequestNamedLWLockTranche("orochi", 8);
RequestNamedLWLockTranche("orochi_pipeline", 1);  // ADD THIS LINE
```

---

### Gap 3: Raft Not Initialized

**File:** `/home/user/orochi-db/src/core/init.c`
**Lines:** 90-97

**Issue:**
```c
if (process_shared_preload_libraries_in_progress)
{
    RequestAddinShmemSpace(orochi_memsize());
    RequestNamedLWLockTranche("orochi", 8);

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = orochi_shmem_startup;

    orochi_register_background_workers();
}

// But orochi_register_background_workers() does NOT:
// 1. Initialize Raft node
// 2. Register Raft background worker
// 3. Request shared memory for Raft log
```

**Where It Should Be:**
- In `orochi_shmem_startup()`: Initialize Raft node with `raft_init()`
- In `orochi_register_background_workers()`: Register Raft heartbeat worker
- In `_PG_init()`: Add Raft shared memory size to RequestAddinShmemSpace()

---

## 14. Conclusion

Orochi DB demonstrates excellent **modular architecture and feature completeness** for a single-node PostgreSQL extension. The core subsystems (sharding, columnar, tiered, vector) are well-engineered and largely functional.

However, **production deployment is blocked by three critical integration gaps:**

1. **Pipelines are non-functional** due to missing shared memory initialization
2. **Raft consensus is unused** despite being fully implemented
3. **Security vulnerabilities** in Raft RPC handling

These gaps indicate the codebase is in an **incomplete mid-development state** rather than a finalized product. With 6-10 weeks of focused engineering on the critical path, Orochi DB could become production-ready.

**Key Recommendation:** Before any production deployment, execute the Phase 1 fixes (section 11) and comprehensive failure scenario testing.

