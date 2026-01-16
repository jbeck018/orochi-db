# Orochi DB - AI Agent Development Guide

A comprehensive guide for AI agents working with this PostgreSQL HTAP extension.

## Project Overview

Orochi DB is a PostgreSQL extension providing HTAP (Hybrid Transactional/Analytical Processing) capabilities. It combines:

- **Automatic Sharding**: Hash-based horizontal distribution across nodes (inspired by Citus)
- **Time-Series Optimization**: Automatic time-based partitioning with chunks (inspired by TimescaleDB)
- **Columnar Storage**: Column-oriented format with compression for analytics (inspired by Hydra)
- **Tiered Storage**: Hot/warm/cold/frozen data lifecycle with S3 integration
- **Vector/AI Workloads**: SIMD-optimized vector operations and similarity search
- **Streaming Pipelines**: Real-time data ingestion from Kafka, S3, filesystems
- **CDC (Change Data Capture)**: Stream database changes to external systems
- **Raft Consensus**: Distributed coordination for cluster consistency
- **Workload Management**: Resource pools and query admission control
- **JIT Compilation**: Just-in-time query expression compilation

**Version**: 1.0.0
**Supported PostgreSQL**: 16, 17, 18
**Minimum PostgreSQL**: 16+
**Language**: C11

---

## Build Instructions

### Prerequisites

```bash
# Ubuntu/Debian - install all dependencies
make install-deps

# Or manually:
sudo apt-get install -y \
    postgresql-server-dev-all \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    librdkafka-dev \
    clang-format \
    cppcheck
```

### Building

```bash
# Standard build (optimized with -O3)
make

# Debug build (with symbols, no optimization)
make DEBUG=1

# Install to PostgreSQL
sudo make install

# Clean build artifacts
make clean

# Clean everything including test results
make clean-all
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

---

## Testing Instructions

### Unit Tests (Standalone - No PostgreSQL Required)

```bash
cd test/unit
make
./run_tests
```

The unit tests use a lightweight framework (`test_framework.h`) that runs without PostgreSQL.

**Test Files:**
| File | Module |
|------|--------|
| `test_columnar.c` | Columnar storage and compression |
| `test_distribution.c` | Sharding and hash distribution |
| `test_hypertable.c` | Time-series chunks |
| `test_executor.c` | Query execution |
| `test_catalog.c` | Metadata catalog |
| `test_raft.c` | Raft consensus protocol |
| `test_pipelines.c` | Data pipelines |
| `test_approx.c` | HyperLogLog, T-Digest |
| `test_vectorized.c` | SIMD vectorized execution |

### Regression Tests (Requires PostgreSQL)

```bash
# After installing the extension
make installcheck
```

Test SQL files: `test/sql/`
Expected output: `test/expected/`

### Static Analysis

```bash
make lint          # Run cppcheck
make check-format  # Check clang-format compliance
make format        # Auto-format all source code
```

---

## Directory Structure

```
/home/user/orochi-db/
├── src/
│   ├── orochi.h              # Main header - all types, enums, API declarations
│   ├── core/
│   │   ├── init.c            # _PG_init, GUCs, hook registration, background workers
│   │   └── catalog.c/h       # Metadata tables management
│   ├── storage/
│   │   ├── columnar.c/h      # Columnar storage (stripes, column chunks)
│   │   └── compression.c/h   # LZ4, ZSTD, Delta, Gorilla, RLE, Dictionary
│   ├── sharding/
│   │   ├── distribution.c/h  # Hash-based shard routing
│   │   └── physical_sharding.c/h
│   ├── timeseries/
│   │   └── hypertable.c/h    # Time partitioning, chunks, continuous aggregates
│   ├── tiered/
│   │   └── tiered_storage.c/h # Hot/warm/cold/frozen tiers, S3 integration
│   ├── vector/
│   │   └── vector_ops.c/h    # Vector type, SIMD distance functions
│   ├── planner/
│   │   └── distributed_planner.c/h # Planner hook, shard pruning
│   ├── executor/
│   │   ├── distributed_executor.c/h # Executor hooks, parallel execution
│   │   ├── vectorized.c/h    # Vectorized batch processing
│   │   └── vectorized_scan.c
│   ├── jit/
│   │   ├── jit.h             # JIT API
│   │   ├── jit_compile.c     # Expression compilation
│   │   └── jit_expr.c        # Expression tree building
│   ├── consensus/
│   │   ├── raft.c/h          # Raft protocol implementation
│   │   ├── raft_log.c        # Log management and persistence
│   │   └── raft_integration.c/h
│   ├── pipelines/
│   │   ├── pipeline.c/h      # Real-time ingestion framework
│   │   └── kafka_source.c    # Kafka consumer
│   ├── cdc/
│   │   └── cdc.c/h           # Change Data Capture
│   ├── workload/
│   │   └── workload.c/h      # Resource pools, admission control
│   ├── approx/
│   │   ├── hyperloglog.c/h   # Cardinality estimation
│   │   ├── tdigest.c/h       # Percentile approximation
│   │   └── sampling.c
│   └── utils/
│       └── utils.c
├── sql/
│   └── orochi--1.0.sql       # Extension SQL definitions
├── test/
│   ├── unit/
│   │   ├── Makefile
│   │   ├── test_framework.h  # Test macros
│   │   ├── mocks/            # PostgreSQL API mocks
│   │   └── test_*.c
│   ├── sql/                  # Regression SQL
│   └── expected/             # Expected output
├── docs/
│   ├── architecture.md
│   └── user-guide.md
└── Makefile                  # PGXS build system
```

---

## Architecture Overview

### Data Flow

```
INSERT → Distribution Column Hash → Shard/Chunk Selection → Storage
                                           │
                        ┌──────────────────┼──────────────────┐
                        │                  │                  │
                        ▼                  ▼                  ▼
                   Row Store        Columnar Store      Time-Series
                   (heap)           (stripes)           (chunks)
```

```
SELECT → Planner Hook → Shard/Chunk Pruning → Fragment Queries
                              │
                              ▼
                     Distributed Executor
                              │
              ┌───────────────┼───────────────┐
              │               │               │
              ▼               ▼               ▼
         Vectorized      JIT-Compiled     Standard
         Execution       Expressions      Execution
```

### Module Dependencies

```
core/init.c
    ├── planner/distributed_planner.c
    ├── executor/distributed_executor.c
    ├── pipelines/pipeline.c
    └── consensus/raft.c

sharding/distribution.c ←→ timeseries/hypertable.c
         │                          │
         └──────────┬───────────────┘
                    ▼
            storage/columnar.c
                    │
                    ▼
            tiered/tiered_storage.c
```

---

## Coding Conventions

### PostgreSQL Extension Patterns

**Memory Management** - Use PostgreSQL memory contexts:
```c
MemoryContext old_ctx = MemoryContextSwitchTo(orochi_context);
// allocations here use orochi_context
result = palloc(size);
MemoryContextSwitchTo(old_ctx);
```

**Error Handling** - Use ereport/elog:
```c
ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("shard count must be between 1 and 1024"),
         errdetail("Received: %d", shard_count)));

elog(LOG, "Orochi initialized with %d shards", count);
```

**GUC Variables** - Define in `init.c`:
```c
DefineCustomIntVariable("orochi.default_shard_count",
                        "Default number of shards",
                        NULL,
                        &orochi_default_shard_count,
                        32,    /* default */
                        1,     /* min */
                        1024,  /* max */
                        PGC_USERSET,
                        0,
                        NULL, NULL, NULL);
```

**Hook Chaining** - Always preserve previous hooks:
```c
prev_planner_hook = planner_hook;
planner_hook = orochi_planner_hook;

// In _PG_fini:
planner_hook = prev_planner_hook;
```

**SQL Functions** - Use PG_FUNCTION_ARGS:
```c
PG_FUNCTION_INFO_V1(orochi_my_function);
Datum
orochi_my_function(PG_FUNCTION_ARGS)
{
    int32 arg1 = PG_GETARG_INT32(0);
    text *arg2 = PG_GETARG_TEXT_PP(1);
    // ...
    PG_RETURN_BOOL(true);
}
```

### Code Style

- C11 standard (`-std=c11`)
- 4-space indentation
- Use `make format` before committing
- Prefix public symbols: `orochi_`, `raft_`, `cdc_`, etc.
- Use `static` for file-local functions
- Document functions in header files

### Unit Test Pattern

```c
#include "test_framework.h"

TEST_SUITE(test_my_module)
{
    TEST_BEGIN("descriptive_test_name")
        // Setup
        MyStruct *obj = create_object();

        // Action
        int result = function_under_test(obj);

        // Assert
        TEST_ASSERT_EQ(42, result);
        TEST_ASSERT_NOT_NULL(obj->data);
        TEST_ASSERT_STR_EQ("expected", obj->name);

        // Cleanup
        free_object(obj);
    TEST_END()

    TEST_BEGIN("another_test")
        TEST_ASSERT_NEAR(3.14159, calculate_pi(), 0.0001);
    TEST_END()
}
```

---

## Important Entry Points

### Extension Lifecycle

| Function | File | Purpose |
|----------|------|---------|
| `_PG_init()` | `src/core/init.c` | Extension load - GUCs, hooks, shared memory |
| `_PG_fini()` | `src/core/init.c` | Extension unload - restore hooks |
| `orochi_shmem_startup()` | `src/core/init.c` | Initialize shared memory structures |

### Hooks

| Hook | File | Purpose |
|------|------|---------|
| `orochi_planner_hook` | `distributed_planner.c` | Query planning for distributed tables |
| `orochi_executor_start_hook` | `distributed_executor.c` | Initialize execution state |
| `orochi_executor_run_hook` | `distributed_executor.c` | Execute distributed fragments |
| `orochi_executor_finish_hook` | `distributed_executor.c` | Finalize execution |
| `orochi_executor_end_hook` | `distributed_executor.c` | Cleanup execution state |

### Background Workers

| Worker | Function | Purpose |
|--------|----------|---------|
| Tiering | `orochi_tiering_worker_main` | Move data between storage tiers |
| Compression | `orochi_compression_worker_main` | Compress cold chunks |
| Stats | `orochi_stats_worker_main` | Gather cluster statistics |
| Rebalancer | `orochi_rebalancer_worker_main` | Rebalance shards across nodes |
| Raft | `orochi_raft_worker_main` | Consensus protocol |
| Pipeline | `pipeline_worker_main` | Data pipeline processing |
| CDC | `cdc_worker_main` | Change data capture streaming |

### Core APIs

| Function | Purpose |
|----------|---------|
| `orochi_create_distributed_table()` | Shard a table across nodes |
| `orochi_create_hypertable()` | Enable time-series partitioning |
| `orochi_convert_to_columnar()` | Convert to columnar storage |
| `orochi_move_to_tier()` | Move data between tiers |
| `orochi_create_pipeline()` | Create data ingestion pipeline |
| `orochi_create_cdc_subscription()` | Set up CDC streaming |
| `orochi_create_resource_pool()` | Create workload resource pool |

---

## Key Data Structures

| Structure | Header | Purpose |
|-----------|--------|---------|
| `OrochiTableInfo` | `orochi.h` | Distributed/hypertable metadata |
| `OrochiShardInfo` | `orochi.h` | Shard placement information |
| `OrochiChunkInfo` | `orochi.h` | Time-series chunk boundaries |
| `OrochiStripeInfo` | `orochi.h` | Columnar stripe metadata |
| `OrochiColumnChunk` | `orochi.h` | Column data within stripe |
| `VectorBatch` | `vectorized.h` | Vectorized execution batch |
| `RaftNode` | `raft.h` | Raft consensus state |
| `RaftLog` | `raft.h` | Raft log entries |
| `Pipeline` | `pipeline.h` | Pipeline configuration |
| `CDCSubscription` | `cdc.h` | CDC subscription config |
| `ResourcePool` | `workload.h` | Workload resource limits |
| `JitContext` | `jit.h` | JIT compilation context |
| `JitExprNode` | `jit.h` | JIT expression tree node |

---

## Compression Types

| Type | Enum | Best For |
|------|------|----------|
| None | `OROCHI_COMPRESS_NONE` | Already compressed data |
| LZ4 | `OROCHI_COMPRESS_LZ4` | Fast general compression |
| ZSTD | `OROCHI_COMPRESS_ZSTD` | High ratio for cold data |
| Delta | `OROCHI_COMPRESS_DELTA` | Monotonic integers/timestamps |
| Gorilla | `OROCHI_COMPRESS_GORILLA` | Floating-point time-series |
| Dictionary | `OROCHI_COMPRESS_DICTIONARY` | Low-cardinality strings |
| RLE | `OROCHI_COMPRESS_RLE` | Boolean/repeated values |

---

## Storage Tiers

| Tier | Enum | Default Age | Storage |
|------|------|-------------|---------|
| Hot | `OROCHI_TIER_HOT` | 0-1 day | Local SSD |
| Warm | `OROCHI_TIER_WARM` | 1-7 days | Local, compressed |
| Cold | `OROCHI_TIER_COLD` | 7-30 days | S3 |
| Frozen | `OROCHI_TIER_FROZEN` | 30+ days | S3 Glacier |

---

## Common Tasks

### Adding a New Feature

1. **Declare in header** (`src/module/module.h`)
2. **Implement** (`src/module/module.c`)
3. **Add to Makefile OBJS** if new `.c` file
4. **Add SQL interface** in `sql/orochi--1.0.sql` if user-facing
5. **Write unit tests** in `test/unit/test_module.c`
6. **Update runner** in `test/unit/run_tests.c`

### Adding a GUC Parameter

```c
// In src/core/init.c, orochi_define_gucs():

// Declare extern in orochi.h
int orochi_new_param;

DefineCustomIntVariable("orochi.new_param",
                        "Description",
                        "Long description",
                        &orochi_new_param,
                        default_value,
                        min_value,
                        max_value,
                        PGC_USERSET,  // or PGC_SIGHUP, PGC_POSTMASTER
                        0,
                        NULL, NULL, NULL);
```

### Adding a SQL Function

```c
// 1. Declare in header
extern Datum orochi_my_func(PG_FUNCTION_ARGS);

// 2. Implement
PG_FUNCTION_INFO_V1(orochi_my_func);
Datum
orochi_my_func(PG_FUNCTION_ARGS)
{
    int32 arg = PG_GETARG_INT32(0);
    // implementation
    PG_RETURN_INT32(result);
}

// 3. Register in sql/orochi--1.0.sql
CREATE FUNCTION my_func(arg integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'orochi_my_func'
LANGUAGE C STRICT;
```

### Adding Unit Tests

```c
// test/unit/test_new_module.c
#include "test_framework.h"

TEST_SUITE(test_new_module)
{
    TEST_BEGIN("test_basic_functionality")
        // test code
        TEST_ASSERT(condition);
    TEST_END()
}

// Add to test/unit/run_tests.c:
extern void test_new_module(void);
// In main():
RUN_TEST_SUITE(test_new_module);

// Add to test/unit/Makefile if new file
```

### Debugging

```bash
# Build with debug symbols
make DEBUG=1

# Attach GDB to PostgreSQL backend
gdb -p <postgres_backend_pid>

# Check PostgreSQL logs for elog output
tail -f /var/log/postgresql/postgresql-*.log

# Memory debugging
MALLOC_CHECK_=3 ./test/unit/run_tests
```

---

## Quick Reference

```bash
# Build and install
make && sudo make install

# Run unit tests
cd test/unit && make && ./run_tests

# Run regression tests (after install)
make installcheck

# Format code
make format

# Static analysis
make lint

# Create release tarball
make dist
```

---

## Test Framework Macros

| Macro | Usage |
|-------|-------|
| `TEST_BEGIN(name)` | Start a test case |
| `TEST_END()` | End a test case |
| `TEST_ASSERT(cond)` | Assert condition is true |
| `TEST_ASSERT_EQ(exp, act)` | Assert integer equality |
| `TEST_ASSERT_NEQ(exp, act)` | Assert integer inequality |
| `TEST_ASSERT_NEAR(exp, act, eps)` | Assert float equality within epsilon |
| `TEST_ASSERT_GT(val, thresh)` | Assert greater than |
| `TEST_ASSERT_LT(val, thresh)` | Assert less than |
| `TEST_ASSERT_NOT_NULL(ptr)` | Assert pointer not null |
| `TEST_ASSERT_NULL(ptr)` | Assert pointer is null |
| `TEST_ASSERT_STR_EQ(exp, act)` | Assert string equality |
| `TEST_ASSERT_MEM_EQ(exp, act, sz)` | Assert memory equality |
| `TEST_SKIP(reason)` | Skip test with reason |
| `RUN_TEST_SUITE(func)` | Run a test suite function |

---

## File Organization Rules

- Source code: `src/`
- Tests: `test/unit/` or `test/sql/`
- Documentation: `docs/`
- SQL definitions: `sql/`
- Never save working files to root folder
