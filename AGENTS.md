# Orochi DB Agent Specifications

This document defines specialized agents for working on the Orochi DB codebase. Each agent has a focused area of responsibility, specific files they work with, and defined dependencies on other agents.

---

## Table of Contents

1. [Security Agent](#1-security-agent)
2. [Consensus Agent](#2-consensus-agent)
3. [Storage Agent](#3-storage-agent)
4. [Executor Agent](#4-executor-agent)
5. [Pipeline Agent](#5-pipeline-agent)
6. [CDC Agent](#6-cdc-agent)
7. [JIT Agent](#7-jit-agent)
8. [Workload Agent](#8-workload-agent)
9. [TimeSeries Agent](#9-timeseries-agent)
10. [Test Agent](#10-test-agent)

---

## 1. Security Agent

### Responsibility

Ensures security across all Orochi DB components. Focuses on SQL injection prevention, input validation, authentication mechanisms, and secure RPC communication in the Raft consensus layer.

### Key Files

- `src/consensus/raft.c` - Secure RPC handling and peer authentication
- `src/consensus/raft.h` - Security-related type definitions
- `src/pipelines/kafka_source.c` - SASL authentication validation
- `src/pipelines/s3_source.c` - AWS credential handling
- `src/cdc/cdc_sink.c` - Secure sink configurations
- `src/workload/workload.c` - Session authentication and authorization

### Common Tasks

- Audit RPC message serialization/deserialization for buffer overflows
- Validate input parameters in all public API functions
- Review SASL mechanism implementations in Kafka/CDC sinks
- Ensure proper credential storage (no hardcoded secrets)
- Validate SSL/TLS configurations for external connections
- Implement parameterized query patterns to prevent SQL injection
- Review peer connection authentication in Raft protocol
- Audit memory allocation for potential vulnerabilities

### Testing Requirements

- Write fuzz tests for RPC deserialization functions
- Create unit tests for input validation edge cases
- Test authentication failure scenarios
- Verify credential sanitization in logs
- Test SSL certificate validation paths

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Consensus Agent | Coordinate on secure Raft RPC implementation |
| Pipeline Agent | Review Kafka/S3 authentication flows |
| CDC Agent | Validate sink credential handling |
| Test Agent | Request security-focused test coverage |

---

## 2. Consensus Agent

### Responsibility

Maintains the Raft consensus protocol implementation for distributed coordination. Handles leader election, log replication, heartbeat mechanisms, and state machine consistency across cluster nodes.

### Key Files

- `src/consensus/raft.c` - Core Raft protocol state machine
- `src/consensus/raft.h` - Protocol types and API definitions
- `src/consensus/raft_log.c` - Log entry management and WAL persistence
- `src/consensus/raft_integration.c` - PostgreSQL integration layer
- `src/consensus/raft_integration.h` - Integration API

### Common Tasks

- Implement leader election timeout handling
- Manage log replication to follower nodes
- Handle AppendEntries and RequestVote RPCs
- Implement snapshot creation and transfer (InstallSnapshot)
- Maintain commit index advancement and state machine application
- Implement linearizable reads via `raft_read_index()`
- Handle cluster membership changes (add/remove peer)
- Optimize heartbeat intervals for network conditions

### Testing Requirements

- Test leader election under various failure scenarios
- Verify log replication correctness with network partitions
- Test snapshot transfer for lagging followers
- Validate commit index advancement with quorum
- Test cluster reconfiguration safety
- Benchmark election timeout tuning

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Security Agent | Secure RPC communication between peers |
| Storage Agent | Coordinate WAL persistence mechanisms |
| CDC Agent | Raft log entries feed CDC event capture |
| Test Agent | Request `test/unit/test_raft.c` coverage |

---

## 3. Storage Agent

### Responsibility

Manages the columnar storage engine including compression, stripe/chunk organization, and PostgreSQL Table Access Method (TAM) integration. Optimizes storage for analytical workloads.

### Key Files

- `src/storage/columnar.c` - Columnar storage implementation
- `src/storage/columnar.h` - Storage structures and API
- `src/storage/compression.c` - Compression algorithms (LZ4, ZSTD, Snappy)
- `src/storage/compression.h` - Compression interfaces
- `src/tiered/tiered_storage.c` - Tiered storage management (hot/warm/cold)
- `src/tiered/tiered_storage.h` - Tiered storage API

### Common Tasks

- Implement stripe creation and flushing
- Manage chunk group organization within stripes
- Optimize compression algorithm selection per column type
- Implement skip list for predicate pushdown
- Handle columnar tuple insert/update/delete operations
- Implement TAM callbacks (scan, insert, delete, update)
- Manage storage tiering between local disk and S3
- Optimize decompression for vectorized reads

### Testing Requirements

- Test compression ratios for different data types
- Verify stripe/chunk boundary handling
- Test concurrent read/write operations
- Validate skip list predicate elimination
- Test tiered storage migration policies
- Benchmark compression/decompression performance

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Executor Agent | Provide vectorized column data for scans |
| JIT Agent | Support JIT-compiled decompression paths |
| TimeSeries Agent | Coordinate chunk compression policies |
| Test Agent | Request `test/unit/test_columnar.c` coverage |

---

## 4. Executor Agent

### Responsibility

Implements the vectorized query execution engine with SIMD-accelerated operations. Handles batch processing, filter evaluation, aggregation, and integration with columnar storage.

### Key Files

- `src/executor/vectorized.c` - Vectorized execution engine
- `src/executor/vectorized.h` - Vector batch structures and operations
- `src/executor/vectorized_scan.c` - Columnar scan with vectorization
- `src/executor/distributed_executor.c` - Distributed query execution
- `src/executor/distributed_executor.h` - Distributed execution API

### Common Tasks

- Implement SIMD-accelerated filter operations (AVX2/AVX512)
- Manage VectorBatch creation and selection vectors
- Implement vectorized aggregations (SUM, COUNT, AVG, MIN, MAX)
- Handle null bitmap processing in batch operations
- Optimize batch size tuning for CPU cache efficiency
- Implement distributed query coordination across nodes
- Integrate with columnar storage read path
- Handle type-specific vectorized operations (int32/64, float32/64)

### Testing Requirements

- Test SIMD operations on various CPU architectures
- Verify selection vector correctness after filtering
- Test aggregation accuracy with null values
- Benchmark vectorized vs. tuple-at-a-time execution
- Test distributed execution with node failures
- Validate memory alignment for SIMD operations

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Storage Agent | Receive columnar data for vectorized scans |
| JIT Agent | Use JIT-compiled filters and aggregations |
| Workload Agent | Respect resource limits during execution |
| Test Agent | Request `test/unit/test_vectorized.c` and `test_executor.c` coverage |

---

## 5. Pipeline Agent

### Responsibility

Manages real-time data ingestion pipelines from external sources (Kafka, S3, filesystem). Handles data parsing, transformation, and batch insertion into target tables.

### Key Files

- `src/pipelines/pipeline.c` - Core pipeline management
- `src/pipelines/pipeline.h` - Pipeline structures and lifecycle API
- `src/pipelines/kafka_source.c` - Kafka consumer implementation
- `src/pipelines/s3_source.c` - S3 file source implementation

### Common Tasks

- Implement pipeline lifecycle (create, start, pause, stop)
- Handle Kafka consumer offset management and commits
- Implement S3 file discovery and processing
- Parse JSON, CSV, Avro, and line protocol formats
- Apply field transformations and mappings
- Manage pipeline checkpointing for exactly-once semantics
- Handle batch insertion with configurable batch sizes
- Implement error thresholds and retry logic

### Testing Requirements

- Test pipeline state transitions
- Verify Kafka offset commit correctness
- Test S3 file processing with various formats
- Validate exactly-once delivery semantics
- Test error handling and retry mechanisms
- Benchmark throughput for high-volume ingestion

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Security Agent | Validate authentication configurations |
| Storage Agent | Coordinate bulk insert operations |
| TimeSeries Agent | Target hypertable ingestion paths |
| Test Agent | Request `test/unit/test_pipelines.c` coverage |

---

## 6. CDC Agent

### Responsibility

Implements Change Data Capture for streaming database changes to external systems. Captures events from WAL and Raft log, serializes them, and delivers to configured sinks (Kafka, webhook, file).

### Key Files

- `src/cdc/cdc.c` - CDC event capture and processing
- `src/cdc/cdc.h` - CDC structures, event types, subscription API
- `src/cdc/cdc_sink.c` - Sink implementations (Kafka, webhook, file)

### Common Tasks

- Capture INSERT/UPDATE/DELETE events from WAL
- Process Raft log entries for distributed changes
- Serialize events in JSON, Debezium, or Avro format
- Manage CDC subscriptions and filtering
- Implement Kafka sink with batching and acknowledgments
- Handle webhook delivery with retry logic
- Maintain confirmed LSN for delivery tracking
- Support table and event type filtering

### Testing Requirements

- Test event capture for all DML operations
- Verify serialization format correctness
- Test sink delivery with network failures
- Validate LSN tracking and resume behavior
- Test subscription filtering accuracy
- Benchmark event throughput

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Consensus Agent | Receive Raft log entries for capture |
| Security Agent | Validate sink credential handling |
| Pipeline Agent | Share Kafka sink infrastructure |
| Test Agent | Request CDC-specific test coverage |

---

## 7. JIT Agent

### Responsibility

Implements Just-In-Time compilation for query expressions. Compiles filter predicates and aggregations to specialized code for improved performance in vectorized execution.

### Key Files

- `src/jit/jit_compile.c` - JIT compilation engine
- `src/jit/jit.h` - JIT context, expression nodes, compiled types
- `src/jit/jit_expr.c` - Expression tree building and evaluation
- `src/jit/jit_expr.h` - Expression-specific definitions

### Common Tasks

- Build expression trees from PostgreSQL nodes
- Compile filter predicates to specialized functions
- Implement SIMD-optimized compilation paths
- Manage expression cache for repeated queries
- Compile aggregation update functions
- Optimize common subexpression elimination
- Integrate with vectorized executor
- Implement cost-based JIT decision logic

### Testing Requirements

- Test expression compilation for all operators
- Verify SIMD path correctness
- Test cache hit/miss behavior
- Benchmark JIT vs. interpreted execution
- Test with complex nested expressions
- Validate null handling in compiled code

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Executor Agent | Provide JIT-compiled functions for execution |
| Storage Agent | Optimize for columnar data access patterns |
| Test Agent | Request JIT-specific test coverage |

---

## 8. Workload Agent

### Responsibility

Manages resource pools, workload classes, and query admission control. Enforces CPU, memory, and I/O limits per session and implements query queuing with priority scheduling.

### Key Files

- `src/workload/workload.c` - Workload class and pool management
- `src/workload/workload.h` - Resource structures and API
- `src/workload/resource_governor.c` - Resource enforcement and tracking

### Common Tasks

- Create and manage resource pools with limits
- Implement workload class assignment for sessions
- Handle query admission with concurrency limits
- Implement priority-based query queuing
- Track CPU time usage per session
- Enforce memory allocation limits
- Implement I/O throttling for pools
- Handle query timeout enforcement

### Testing Requirements

- Test resource pool creation and limits
- Verify query admission under load
- Test priority queue ordering
- Validate resource tracking accuracy
- Test timeout enforcement
- Benchmark overhead of resource tracking

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Executor Agent | Enforce limits during query execution |
| Security Agent | Validate session authorization for pools |
| Test Agent | Request workload management tests |

---

## 9. TimeSeries Agent

### Responsibility

Implements time-series specific features including hypertables, automatic time-based chunking, continuous aggregates, and retention/compression policies.

### Key Files

- `src/timeseries/hypertable.c` - Hypertable implementation
- `src/timeseries/hypertable.h` - Hypertable structures and API

### Common Tasks

- Create hypertables with time-based partitioning
- Implement automatic chunk creation for time ranges
- Support space partitioning dimensions
- Implement time bucketing functions
- Create and maintain continuous aggregates
- Execute retention policies (drop old chunks)
- Apply compression policies to cold chunks
- Optimize chunk exclusion for time-range queries
- Support distributed hypertables across nodes

### Testing Requirements

- Test chunk creation at partition boundaries
- Verify continuous aggregate refresh correctness
- Test retention policy execution
- Validate time bucket calculations
- Test distributed hypertable coordination
- Benchmark chunk exclusion optimization

### Dependencies

| Agent | Interaction |
|-------|-------------|
| Storage Agent | Use columnar storage for chunks |
| Pipeline Agent | Support hypertable as ingestion target |
| Consensus Agent | Coordinate distributed hypertable metadata |
| Test Agent | Request `test/unit/test_hypertable.c` coverage |

---

## 10. Test Agent

### Responsibility

Creates, maintains, and runs the test suite for Orochi DB. Works with the standalone unit test framework and SQL regression tests. Ensures comprehensive coverage across all modules.

### Key Files

- `test/unit/test_framework.h` - Unit test framework macros and utilities
- `test/unit/run_tests.c` - Test runner entry point
- `test/unit/test_raft.c` - Raft consensus tests
- `test/unit/test_columnar.c` - Columnar storage tests
- `test/unit/test_vectorized.c` - Vectorized executor tests
- `test/unit/test_hypertable.c` - Hypertable tests
- `test/unit/test_pipelines.c` - Pipeline tests
- `test/unit/test_approx.c` - Approximate query tests
- `test/unit/test_executor.c` - Executor tests
- `test/unit/test_catalog.c` - Catalog tests
- `test/unit/test_distribution.c` - Distribution tests
- `test/unit/Makefile` - Build configuration for tests
- `test/unit/mocks/postgres_mock.h` - PostgreSQL mock functions
- `test/sql/*.sql` - SQL regression test scripts
- `test/expected/*.out` - Expected test outputs

### Common Tasks

- Write unit tests using `TEST_BEGIN`/`TEST_END` macros
- Create test suites with `test_suite_begin`/`test_suite_end`
- Use assertion macros: `TEST_ASSERT`, `TEST_ASSERT_EQ`, `TEST_ASSERT_NEAR`
- Add mock implementations for PostgreSQL dependencies
- Create SQL regression tests for end-to-end validation
- Maintain expected output files for regression tests
- Run tests with `make -C test/unit && ./test/unit/run_tests`
- Track code coverage and identify gaps
- Create fuzz tests for security-critical paths

### Testing Requirements

- Ensure all new code has corresponding unit tests
- Maintain minimum 80% code coverage
- Tests must pass without PostgreSQL running (standalone)
- SQL tests must match expected output exactly
- Performance benchmarks should not regress

### Dependencies

| Agent | Interaction |
|-------|-------------|
| All Agents | Write tests for code from all agents |
| Security Agent | Prioritize security-related test coverage |
| Consensus Agent | Test distributed scenarios thoroughly |
| Executor Agent | Benchmark vectorized operations |

---

## Agent Coordination Matrix

| Agent | Security | Consensus | Storage | Executor | Pipeline | CDC | JIT | Workload | TimeSeries |
|-------|:--------:|:---------:|:-------:|:--------:|:--------:|:---:|:---:|:--------:|:----------:|
| Security | - | RPC | - | - | Auth | Auth | - | Session | - |
| Consensus | RPC | - | WAL | - | - | Log | - | - | Metadata |
| Storage | - | WAL | - | Data | Bulk | - | Decomp | - | Chunks |
| Executor | - | - | Data | - | - | - | JIT | Limits | - |
| Pipeline | Auth | - | Bulk | - | - | Kafka | - | - | Target |
| CDC | Auth | Log | - | - | Kafka | - | - | - | - |
| JIT | - | - | Decomp | JIT | - | - | - | - | - |
| Workload | Session | - | - | Limits | - | - | - | - | - |
| TimeSeries | - | Metadata | Chunks | - | Target | - | - | - | - |

---

## Quick Reference: File to Agent Mapping

| Directory | Primary Agent | Secondary Agent |
|-----------|---------------|-----------------|
| `src/consensus/` | Consensus Agent | Security Agent |
| `src/storage/` | Storage Agent | - |
| `src/tiered/` | Storage Agent | - |
| `src/executor/` | Executor Agent | JIT Agent |
| `src/pipelines/` | Pipeline Agent | Security Agent |
| `src/cdc/` | CDC Agent | Security Agent |
| `src/jit/` | JIT Agent | Executor Agent |
| `src/workload/` | Workload Agent | Security Agent |
| `src/timeseries/` | TimeSeries Agent | Storage Agent |
| `test/unit/` | Test Agent | All Agents |
| `test/sql/` | Test Agent | All Agents |
