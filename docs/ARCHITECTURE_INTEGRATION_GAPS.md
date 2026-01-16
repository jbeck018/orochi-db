# Orochi DB - Architecture Integration Gaps

**Document Purpose:** Detailed technical analysis of integration points, gaps, and required fixes
**Target Audience:** Systems architects, senior engineers
**Last Updated:** 2026-01-16

---

## Part 1: System Architecture Overview

### High-Level Component Map

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         PostgreSQL Server                                 │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │ Extension: Orochi (26,014 lines of C)                            │   │
│  │                                                                  │   │
│  ├─────────────────────────────────────────────────────────────────┤   │
│  │ Layer 1: Query Processing (Hooks into PG optimizer/executor)   │   │
│  ├─────────────────────────────────────────────────────────────────┤   │
│  │                                                                  │   │
│  │  ┌──────────────────────────┐  ┌──────────────────────────┐    │   │
│  │  │  Planner Hook            │  │  Executor Hooks (4)      │    │   │
│  │  │  ─────────────────────   │  │  ────────────────────    │    │   │
│  │  │  • Distributed table     │  │  • ExecutorStart         │    │   │
│  │  │    detection             │  │  • ExecutorRun           │    │   │
│  │  │  • Shard pruning         │  │  • ExecutorFinish        │    │   │
│  │  │  • Fragment generation   │  │  • ExecutorEnd           │    │   │
│  │  │  • Co-location checks    │  │                          │    │   │
│  │  │  • Cost estimation       │  │  Handles:                │    │   │
│  │  │                          │  │  - Task dispatch         │    │   │
│  │  │  Planner Output:         │  │  - Result merging        │    │   │
│  │  │  DistributedPlan struct  │  │  - Error handling        │    │   │
│  │  │  with FragmentQueries    │  │                          │    │   │
│  │  └──────────┬───────────────┘  └────────────┬─────────────┘    │   │
│  │             │                               │                  │   │
│  └─────────────┼───────────────────────────────┼──────────────────┘   │
│                │                               │                      │
│  ┌─────────────┼───────────────────────────────┼──────────────────┐   │
│  │ Layer 2: Storage Engines                  │                  │   │
│  ├─────────────┼───────────────────────────────┼──────────────────┤   │
│  │             │                               │                  │   │
│  │  ┌──────────▼──────────┐   ┌────────────────▼──────────┐      │   │
│  │  │ Columnar Storage    │   │ Distributed Executor      │      │   │
│  │  │ ────────────────    │   │ ──────────────────────    │      │   │
│  │  │ • Stripe layout     │   │ • Remote task execution   │      │   │
│  │  │ • Column chunks     │   │ • Worker connections      │      │   │
│  │  │ • Skip lists        │   │ • 2PC coordination        │      │   │
│  │  │ • Compression       │   │ • Result aggregation      │      │   │
│  │  │   (8 algorithms)    │   │                           │      │   │
│  │  │                     │   │ Potential Integration Gap │      │   │
│  │  │ ◄─────Integration──→│   │ (RAFT NOT INTEGRATED)     │      │   │
│  │  │ (via Vectorized)    │   │                           │      │   │
│  │  └──────────┬──────────┘   └────────────┬──────────────┘      │   │
│  │             │                           │                     │   │
│  └─────────────┼───────────────────────────┼─────────────────────┘   │
│                │                           │                        │
│  ┌─────────────┼───────────────────────────┼─────────────────────┐   │
│  │ Layer 3: Specialization Engines         │                    │   │
│  ├─────────────┼───────────────────────────┼─────────────────────┤   │
│  │             │                           │                    │   │
│  │  ┌──────────▼──────┐  ┌────────────┐   │   ┌────────────┐    │   │
│  │  │ Vectorized      │  │ Time-      │   │   │ Consensus  │    │   │
│  │  │ Executor (SIMD) │  │ series     │   │   │ (RAFT)     │    │   │
│  │  │ ────────────    │  │ ────────   │   │   │ ──────     │    │   │
│  │  │ • AVX2 ops      │  │ • Hyper-   │   │   │ • Leader   │    │   │
│  │  │ • Batch filter  │  │   tables   │   │   │   election │    │   │
│  │  │ • SIMD agg      │  │ • Chunking │   │   │ • Log      │    │   │
│  │  │ • Selection vec │  │ • Retention│   │   │   replication    │   │
│  │  │                 │  │ • Policies │   │   │ • State    │    │   │
│  │  │ Status: ✅      │  │            │   │   │   machine  │    │   │
│  │  │ Integrated      │  │ Status: ✅ │   │   │            │    │   │
│  │  │                 │  │ Integrated │   │   │ Status: ❌ │    │   │
│  │  │                 │  │            │   │   │ NOT used   │    │   │
│  │  └─────────────────┘  └────────────┘   │   └────────────┘    │   │
│  │                                         │                    │   │
│  └─────────────────────────────────────────┼────────────────────┘   │
│                                             │                       │
│  ┌──────────────────────────────────────────┼────────────────────┐   │
│  │ Layer 4: Data Movement & Lifecycle       │                    │   │
│  ├──────────────────────────────────────────┼────────────────────┤   │
│  │                                          │                    │   │
│  │  ┌──────────────┐  ┌──────────────┐    │   ┌──────────────┐  │   │
│  │  │ Tiered       │  │ Pipelines    │    │   │ Approximate  │  │   │
│  │  │ Storage      │  │              │    │   │ Query Engine │  │   │
│  │  │ ────────     │  │ ────────     │    │   │ ──────────   │  │   │
│  │  │ • S3 upload  │  │ • Kafka      │    │   │ • HyperLogLog│  │   │
│  │  │ • Tiering    │  │ • S3 sync    │    │   │ • T-Digest   │  │   │
│  │  │   policy     │  │ • File watch │    │   │ • Sampling   │  │   │
│  │  │ • Access     │  │                   │   │              │  │   │
│  │  │   tracking   │  │ Status: ⚠️ ❌    │   │ Status: ✅  │  │   │
│  │  │              │  │ Broken (no        │   │ Integrated  │  │   │
│  │  │ Status: ✅  │  │ shmem init)       │   │              │  │   │
│  │  │ Integrated   │  │                   │   └──────────────┘  │   │
│  │  └──────────────┘  └──────────────┘    │                    │   │
│  │                                         │                    │   │
│  └─────────────────────────────────────────┼────────────────────┘   │
│                                             │                       │
│  ┌──────────────────────────────────────────┼────────────────────┐   │
│  │ Layer 5: Catalog & Metadata              │                    │   │
│  ├──────────────────────────────────────────┼────────────────────┤   │
│  │  • Distributed table metadata            │                    │   │
│  │  • Shard locations & stats               │                    │   │
│  │  • Hypertable dimensions & policies      │                    │   │
│  │  • Columnar stripe & chunk metadata      │                    │   │
│  │  • Node registry (worker nodes)          │                    │   │
│  │  • Pipeline configuration & checkpoints  │                    │   │
│  │  • Vector index metadata                 │                    │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Background Workers (Maintenance)                             │   │
│  ├──────────────────────────────────────────────────────────────┤   │
│  │  ✅ Tiering Worker      - Moves chunks between S3/local     │   │
│  │  ✅ Compression Worker  - Applies compression policies      │   │
│  │  ✅ Stats Worker        - Collects aggregate statistics    │   │
│  │  ✅ Rebalancer Worker   - Moves shards for load balancing  │   │
│  │  ❌ Raft Worker         - NOT IMPLEMENTED (should exist)   │   │
│  │  ⚠️ Pipeline Workers    - Dynamically spawned (broken)     │   │
│  │                                                              │   │
│  │  Missing: Health check worker, Log compaction worker       │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

---

## Part 2: Integration Gap Details

### Gap 1: Raft Consensus System

**Severity:** CRITICAL
**Impact:** No distributed coordination, single point of failure

#### Architecture

```
RAFT IMPLEMENTATION EXISTS:
├── Core Consensus (src/consensus/raft.h)
│   ├── Leader election with term-based voting
│   ├── Log replication across cluster nodes
│   ├── Heartbeat mechanism for failure detection
│   ├── State machine replication for consistency
│   └── 3 RPC message types (RequestVote, AppendEntries, InstallSnapshot)
│
├── Persistent Log (src/consensus/raft_log.c)
│   ├── In-memory log with optional WAL persistence
│   ├── Snapshots for log compaction
│   └── Recovery from disk on startup
│
└── Cluster Management
    ├── Peer tracking and connectivity
    ├── Quorum calculation
    └── Vote aggregation

RAFT IS NEVER STARTED:
├── _PG_init() doesn't call raft_init()
├── _PG_init() doesn't register a background worker for raft heartbeat
├── No Raft shared memory allocated
├── No Raft node created per cluster member
└── Distributed executor never checks Raft leader

CONSEQUENCE:
├── Distributed executor always assumes it's the leader (WRONG)
├── Rebalancer never uses Raft to coordinate shard movements
├── 2PC never coordinates through Raft log (no durability)
├── Node failures not detected or handled
└── System has single point of failure (coordinator node)
```

#### Design Intent vs. Reality

**What Should Happen:**
```
User starts cluster with 3 PostgreSQL + Orochi nodes:
  Node A: coordinator + raft leader + data holder
  Node B: worker node + raft follower + data holder
  Node C: worker node + raft follower + data holder

Raft state machine:
  Commands: CREATE SHARD, MOVE_SHARD, DROP_SHARD, UPDATE_REPLICA_SET

When Node A crashes:
  1. Raft heartbeat timeout triggers election in Node B/C
  2. New leader elected (say Node B)
  3. Queries now route to new leader
  4. Shard replication continues under new leader
  5. Node A rejoins as follower when it recovers
```

**What Actually Happens:**
```
User starts cluster with 3 PostgreSQL + Orochi nodes:
  Node A: coordinator (hardcoded as leader)
  Node B: worker node (no raft state)
  Node C: worker node (no raft state)

When Node A crashes:
  1. Raft system doesn't exist - no election
  2. Queries fail because "coordinator is down"
  3. Shards can't be rebalanced without coordinator
  4. Users must manually promote Node B to coordinator
  5. Data inconsistency possible during transition
```

#### Required Integration

1. **In `init.c` _PG_init():**
   ```c
   if (process_shared_preload_libraries_in_progress) {
       // Request Raft shared memory
       RequestAddinShmemSpace(sizeof(RaftNode) + RAFT_MAX_LOG_ENTRIES * sizeof(RaftLogEntry));

       // Register Raft LWLock tranche
       RequestNamedLWLockTranche("orochi_raft", 2);

       // Initialize Raft in shmem startup
       prev_shmem_startup_hook = shmem_startup_hook;
       shmem_startup_hook = orochi_shmem_startup;  // this will init Raft
   }
   ```

2. **In `init.c` orochi_shmem_startup():**
   ```c
   static void
   orochi_shmem_startup(void) {
       // ... existing code ...

       // Initialize Raft cluster node
       raft_node = raft_init(
           my_node_id,  // From config or pg_nodeinfo
           raft_wal_path
       );
       raft_start(raft_node);  // Start heartbeat/election timer
   }
   ```

3. **In `init.c` orochi_register_background_workers():**
   ```c
   BackgroundWorker raft_worker;
   memset(&raft_worker, 0, sizeof(BackgroundWorker));
   snprintf(raft_worker.bgw_name, BGW_MAXLEN, "orochi raft consensus");
   snprintf(raft_worker.bgw_type, BGW_MAXLEN, "orochi raft");
   raft_worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
   raft_worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
   raft_worker.bgw_restart_time = 60;
   snprintf(raft_worker.bgw_library_name, BGW_MAXLEN, "orochi");
   snprintf(raft_worker.bgw_function_name, BGW_MAXLEN, "orochi_raft_worker_main");
   RegisterBackgroundWorker(&raft_worker);
   ```

4. **In `distributed_executor.c` ExecutorRun hook:**
   ```c
   // Before executing distributed plan:
   if (!raft_is_leader(raft_node)) {
       int32 leader_id = raft_get_leader(raft_node);
       ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("This node is not the query coordinator. "
                  "Connect to node %d instead", leader_id)));
   }
   ```

5. **In `rebalancer_worker_main()`:**
   ```c
   // Coordinate shard movement through Raft:
   uint64 log_index = raft_submit_command(
       raft_node,
       "MOVE_SHARD from_node=X to_node=Y shard_id=Z",
       command_size
   );
   // Wait for replication to quorum
   raft_wait_for_commit(raft_node, log_index, REBALANCE_TIMEOUT_MS);
   // Now apply locally
   execute_shard_movement(...);
   ```

---

### Gap 2: Pipeline Shared Memory Initialization

**Severity:** CRITICAL
**Impact:** Pipelines completely non-functional, workers crash

#### Problem Flow

```
Timeline of Failure:

T0: Extension loads
    ├─ _PG_init() called
    ├─ orochi_memsize() returns size
    │  └─ MISSING: Pipeline shared state size
    ├─ RequestAddinShmemSpace() called
    │  └─ Doesn't include pipeline memory
    ├─ orochi_shmem_startup() called
    │  └─ Initializes columnar, shard, node, stats caches
    │     └─ MISSING: orochi_pipeline_shmem_init() call

T1: User creates pipeline
    ├─ SELECT orochi.create_pipeline(...)
    ├─ Catalog entry created ✅
    └─ Pipeline ID returned ✅

T2: User starts pipeline
    ├─ SELECT orochi.start_pipeline(pipeline_id)
    ├─ RegisterDynamicBackgroundWorker() called ✅
    ├─ pipeline_worker_main() spawned
    ├─ SPI_connect() ✅
    ├─ orochi_pipeline_shmem_init() called
    │  └─ ShmemInitStruct("Orochi Pipeline State", ...)
    │     └─ CRASH: ERROR: no shared memory available
    │         Tried to allocate 3200 bytes, but only 0 available
    │         (no memory was reserved in RequestAddinShmemSpace)

T3: User sees error
    ├─ ERROR: failed to connect to SPI
    └─ Pipeline worker crashed ❌

T4: User tries again
    └─ Same error (worker can't initialize shared memory)
```

#### Code Locations

**Problem 1: Missing allocation in init.c**
```c
// src/core/init.c:332-353
static Size
orochi_memsize(void) {
    Size size = 0;
    size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);
    size = add_size(size, 1024 * 1024);   // Shard metadata
    size = add_size(size, 64 * 1024);     // Node registry
    size = add_size(size, 64 * 1024);     // Statistics

    // BUG: Missing pipeline registry
    // size = add_size(size, sizeof(PipelineSharedState));

    return size;
}
```

**Problem 2: Missing initialization call**
```c
// src/core/init.c:359-393
static void
orochi_shmem_startup(void) {
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    ShmemInitStruct("orochi_columnar_cache", ...);
    ShmemInitStruct("orochi_shard_metadata", ...);
    ShmemInitStruct("orochi_node_registry", ...);
    ShmemInitStruct("orochi_statistics", ...);

    // BUG: Missing pipeline state init
    // orochi_pipeline_shmem_init();

    LWLockRelease(AddinShmemInitLock);
}
```

**Problem 3: Missing LWLock tranche**
```c
// src/core/init.c:85-89
void _PG_init(void) {
    orochi_define_gucs();

    if (process_shared_preload_libraries_in_progress) {
        RequestAddinShmemSpace(orochi_memsize());
        RequestNamedLWLockTranche("orochi", 8);

        // BUG: Missing "orochi_pipeline" tranche
        // RequestNamedLWLockTranche("orochi_pipeline", 1);

        prev_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = orochi_shmem_startup;

        orochi_register_background_workers();
    }
}
```

**Where it fails in pipeline.c**
```c
// src/pipelines/pipeline.c:80-95
void
orochi_pipeline_shmem_init(void) {
    bool found;

    pipeline_shared_state = (PipelineSharedState *)
        ShmemInitStruct("Orochi Pipeline State",
                        sizeof(PipelineSharedState),  // 3200 bytes
                        &found);  // BUG: No memory available!

    if (!found) {
        memset(pipeline_shared_state, 0, sizeof(PipelineSharedState));

        // BUG: Tranche never requested in init.c
        pipeline_shared_state->lock =
            &(GetNamedLWLockTranche("orochi_pipeline"))->lock;
            //  ^ Crashes: LWLock tranche "orochi_pipeline" not found

        pipeline_shared_state->num_active_pipelines = 0;
    }
}
```

#### Required Fix

1. **Fix orochi_memsize():**
   ```c
   static Size
   orochi_memsize(void) {
       Size size = 0;
       size = add_size(size, (Size) orochi_cache_size_mb * 1024 * 1024);
       size = add_size(size, 1024 * 1024);
       size = add_size(size, 64 * 1024);
       size = add_size(size, 64 * 1024);
       size = add_size(size, sizeof(PipelineSharedState));  // ADD THIS
       return size;
   }
   ```

2. **Fix _PG_init():**
   ```c
   if (process_shared_preload_libraries_in_progress) {
       RequestAddinShmemSpace(orochi_memsize());
       RequestNamedLWLockTranche("orochi", 8);
       RequestNamedLWLockTranche("orochi_pipeline", 1);  // ADD THIS
       // ... rest of init
   }
   ```

3. **Fix orochi_shmem_startup():**
   ```c
   LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

   ShmemInitStruct("orochi_columnar_cache", ...);
   ShmemInitStruct("orochi_shard_metadata", ...);
   ShmemInitStruct("orochi_node_registry", ...);
   ShmemInitStruct("orochi_statistics", ...);
   orochi_pipeline_shmem_init();  // ADD THIS

   LWLockRelease(AddinShmemInitLock);
   ```

---

### Gap 3: Missing Vector Index SQL Functions

**Severity:** MEDIUM
**Impact:** Users cannot create vector indexes

#### Problem

```c
// In orochi.h, the function is declared:
extern void orochi_create_vector_index(Oid relid, const char *column,
                                       int dimensions, const char *index_type,
                                       const char *distance_type);

// But in sql/orochi--1.0.sql, there's NO SQL wrapper function!
// Users cannot call it from SQL.
```

#### Missing Function

```sql
-- Should be added to sql/orochi--1.0.sql

CREATE FUNCTION orochi.create_vector_index(
    table_name regclass,
    column_name text,
    dimensions integer,
    index_type text DEFAULT 'hnsw',
    distance_type text DEFAULT 'l2'
) RETURNS void
AS 'MODULE_PATHNAME', 'orochi_create_vector_index_sql'
LANGUAGE C STRICT;

COMMENT ON FUNCTION orochi.create_vector_index IS
'Create a vector index for similarity search.
Index types: hnsw (default), ivfflat
Distance types: l2 (default), cosine, inner_product';
```

---

## Part 3: Integration Dependency Graph

```
Extension Startup Sequence:

┌────────────────────────────────────────────────────────────────┐
│                     _PG_init()                                  │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. orochi_define_gucs()                                        │
│     ├─ All 24 configuration parameters                          │
│     └─ No dependencies                                          │
│                                                                 │
│  2. if (process_shared_preload_libraries_in_progress)          │
│     │                                                           │
│     ├─ RequestAddinShmemSpace()  ❌ MISSING PIPELINE SIZE     │
│     │  └─ Should include: pipeline + raft shared memory        │
│     │                                                           │
│     ├─ RequestNamedLWLockTranche("orochi", 8)  ✅             │
│     │  └─ MISSING: "orochi_pipeline" tranche                  │
│     │  └─ MISSING: "orochi_raft" tranche                      │
│     │                                                           │
│     ├─ prev_shmem_startup_hook = shmem_startup_hook           │
│     │                                                           │
│     ├─ shmem_startup_hook = orochi_shmem_startup  ✅          │
│     │  └─ Will be called during DB startup                    │
│     │  └─ Initializes all shared memory structures            │
│     │                                                           │
│     └─ orochi_register_background_workers()  ⚠️ INCOMPLETE   │
│        ├─ Tiering worker        ✅                            │
│        ├─ Compression worker    ✅                            │
│        ├─ Stats worker          ✅                            │
│        ├─ Rebalancer worker     ✅                            │
│        └─ MISSING: Raft worker                                │
│           └─ MISSING: Raft node creation                      │
│                                                                 │
│  3. prev_planner_hook = planner_hook                           │
│     planner_hook = orochi_planner_hook  ✅                    │
│                                                                 │
│  4. prev_executor_start_hook = ExecutorStart_hook              │
│     ExecutorStart_hook = orochi_executor_start_hook  ✅       │
│                                                                 │
│  5. prev_executor_run_hook = ExecutorRun_hook                  │
│     ExecutorRun_hook = orochi_executor_run_hook  ✅           │
│                                                                 │
│  6. prev_executor_finish_hook = ExecutorFinish_hook            │
│     ExecutorFinish_hook = orochi_executor_finish_hook  ✅     │
│                                                                 │
│  7. prev_executor_end_hook = ExecutorEnd_hook                  │
│     ExecutorEnd_hook = orochi_executor_end_hook  ✅           │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
                          │
                          ▼
            ┌─────────────────────────────┐
            │  During Database Startup    │
            │  orochi_shmem_startup()     │
            ├─────────────────────────────┤
            │                             │
            │  1. prev_shmem_startup_hook()  (chain)
            │                             │
            │  2. LWLockAcquire()        │
            │                             │
            │  3. ShmemInitStruct()x4    │
            │     ├─ columnar_cache      │
            │     ├─ shard_metadata      │
            │     ├─ node_registry       │
            │     └─ statistics          │
            │                             │
            │  ❌ MISSING:               │
            │     ├─ orochi_pipeline_shmem_init()
            │     └─ raft_init() + raft_start()
            │                             │
            │  4. LWLockRelease()        │
            │                             │
            └─────────────────────────────┘
                          │
                          ▼
            ┌─────────────────────────────┐
            │  Background Workers Start   │
            │  (in parallel)              │
            ├─────────────────────────────┤
            │                             │
            ├─ Tiering Worker       ✅  │
            │  └─ Monitors tier policies  │
            │                             │
            ├─ Compression Worker   ✅  │
            │  └─ Applies compression    │
            │                             │
            ├─ Stats Worker         ✅  │
            │  └─ Collects statistics    │
            │                             │
            ├─ Rebalancer Worker    ✅  │
            │  └─ Balances shards        │
            │  └─ ❌ Doesn't use Raft   │
            │                             │
            └─────────────────────────────┘
                          │
                          ▼
                Extension Ready
              (Ready for queries, but:
               - Pipelines non-functional
               - No distributed consensus
               - Single point of failure)
```

---

## Part 4: Fix Priority and Implementation Order

### Priority 1: Critical (Days 1-3)

#### 1.1 Pipeline Shared Memory (2 hours)
```
Files to modify:
- src/core/init.c: 3 lines added to orochi_memsize()
- src/core/init.c: 1 line added to _PG_init()
- src/core/init.c: 1 line added to orochi_shmem_startup()

Total effort: 30 minutes
Testing: Create/start pipeline, verify no crash
```

#### 1.2 Raft Node Initialization (4 hours)
```
Files to modify:
- src/core/init.c: Add raft_init() call
- src/consensus/raft.c: Create orochi_raft_worker_main()
- src/core/init.c: Register raft worker

Total effort: 2-3 hours
Testing: Cluster startup, verify Raft state machine
```

#### 1.3 Security Fixes (6 hours)
```
Files to modify:
- src/consensus/raft.c: 3 locations (SQL injection, JSON injection fixes)

Changes:
- Use PQexecParams instead of string concatenation
- Escape JSON strings properly
- Add input validation

Total effort: 3-4 hours
Testing: Security audit, penetration testing
```

### Priority 2: High (Days 4-7)

#### 2.1 Vector Index SQL Functions (1 hour)
```
Files to modify:
- sql/orochi--1.0.sql: Add SQL wrapper

Total effort: 30 minutes
Testing: Test vector index creation
```

#### 2.2 Health Check Functions (4 hours)
```
New functions to add:
- orochi.cluster_health() → table
- orochi.shard_status(table_name) → table
- orochi.replication_status() → table

Total effort: 2-3 hours
Testing: Manual verification
```

#### 2.3 Integration Tests (8 hours)
```
Test scenarios:
- Node failure and recovery
- Shard rebalancing coordination
- Concurrent pipeline operations
- Cross-subsystem interactions

Total effort: 4-5 hours
Testing: Test suite execution
```

### Priority 3: Medium (Days 8-14)

#### 3.1 Monitoring Integration (6 hours)
```
Add Prometheus metrics:
- shards_accessed_total
- distributed_queries_total
- executor_failures_total
- pipeline_messages_processed_total
```

#### 3.2 Operational Documentation (8 hours)
```
Create:
- Troubleshooting guide
- Deployment runbook
- Architecture integration guide
- Performance tuning guide
```

---

## Part 5: Testing Strategy

### Pre-Fix Validation

```bash
# 1. Verify failures exist
psql -c "CREATE EXTENSION orochi;"
psql -c "CREATE TABLE t (id int);"
psql -c "SELECT orochi.create_pipeline('test', 'kafka', 't'::regclass, 'json', '{}'::jsonb);"
# Expected: ERROR: missing LWLock tranche

# 2. Verify Raft unused
psql -c "SELECT * FROM orochi.raft_node_status();"
# Expected: ERROR: function does not exist
```

### Post-Fix Validation

```bash
# 1. Pipeline lifecycle
SELECT pipeline_id FROM orochi.create_pipeline(...);
SELECT orochi.start_pipeline(1);
SELECT orochi.pause_pipeline(1);
SELECT orochi.resume_pipeline(1);
SELECT orochi.stop_pipeline(1);
# Expected: All succeed

# 2. Raft operations
SELECT orochi.cluster_health();
# Expected: Returns leader info and quorum status

# 3. Distributed queries on cluster
CREATE TABLE orders (id int, customer_id int);
SELECT orochi.create_distributed_table('orders', 'customer_id', 8);
INSERT INTO orders SELECT 1, i FROM generate_series(1, 1000) i;
SELECT COUNT(*), AVG(customer_id) FROM orders;
# Expected: Correct results from shards
```

---

## Conclusion

Orochi DB demonstrates excellent system design with well-segregated components. Three critical integration gaps prevent production deployment:

1. **Raft not initialized** - System lacks distributed consensus
2. **Pipeline shared memory missing** - Pipeline feature is non-functional
3. **Security vulnerabilities** - Potential for exploitation

Fixing these gaps requires ~20-30 hours of engineering effort and would move the system from "architectural mockup" to "production-ready foundation."

