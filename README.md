# Orochi DB

**A Modern HTAP PostgreSQL Extension for AI, Analytics, and Massive Scale**

[![CI](https://github.com/jbeck018/orochi-db/actions/workflows/ci.yml/badge.svg)](https://github.com/jbeck018/orochi-db/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-16%2B-blue.svg)](https://www.postgresql.org/)
[![Go Version](https://img.shields.io/badge/Go-1.22%2B-00ADD8.svg)](https://golang.org/)
[![React](https://img.shields.io/badge/React-19-61DAFB.svg)](https://reactjs.org/)

Orochi DB transforms PostgreSQL into a powerful Hybrid Transactional/Analytical Processing (HTAP) database, combining distributed sharding, time-series optimization, columnar analytics, intelligent tiered storage, and native AI/vector support.

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [Project Structure](#project-structure)
- [Documentation](#documentation)
- [Use Cases](#use-cases)
- [Architecture Overview](#architecture-overview)
- [Development](#development)
- [Contributing](#contributing)
- [License](#license)

## Features

### PostgreSQL Extension (C11)

- **Automatic Sharding**: Hash-based distribution for horizontal scaling across nodes
- **Time-Series Optimization**: Hypertables with automatic time-based partitioning
- **Columnar Storage**: Hybrid row-columnar storage with advanced compression (ZSTD, LZ4, Delta, Gorilla, Dictionary, RLE)
- **Tiered Storage**: Intelligent data lifecycle management (hot/warm/cold/frozen) with S3 integration
- **AI/Vector Support**: Native vector data type with SIMD-optimized distance functions
- **Distributed Query Execution**: Adaptive parallelism with aggregate pushdown
- **JIT Compilation**: Just-in-time expression compilation for analytics
- **Streaming Pipelines**: Real-time data ingestion from Kafka, S3, filesystems
- **CDC (Change Data Capture)**: Stream database changes to external systems
- **Raft Consensus**: Distributed coordination for cluster consistency
- **Advanced Authentication**: JWT, WebAuthn, Magic Links, GoTrue integration
- **JSON Processing**: Columnar JSON storage with indexing
- **Dynamic DDL**: Distributed schema changes with workflow orchestration

### Cloud Platform (Go + React)

- **Control Plane**: REST/gRPC API for cluster lifecycle management
- **Web Dashboard**: Modern React 19 UI with TanStack Router and real-time monitoring
- **CLI Tool**: Full-featured command-line interface for cluster operations
- **Autoscaler**: Automatic scaling based on metrics and workload patterns
- **Provisioner**: Cluster provisioning with CloudNativePG operator
- **Infrastructure**: Kubernetes manifests and Helm charts

## Quick Start

### PostgreSQL Extension

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install postgresql-server-dev-18 liblz4-dev libzstd-dev libcurl4-openssl-dev libssl-dev librdkafka-dev

# Build and install
make
sudo make install

# Enable in PostgreSQL
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
```

### Cloud Platform

```bash
# Install CLI
cd orochi-cloud/cli
go install ./...

# Login to Orochi Cloud
orochi login

# Create a new cluster
orochi cluster create my-cluster --size small --region us-east-1

# Check cluster status
orochi cluster status my-cluster
```

## Project Structure

```
orochi-db/
├── src/                          # PostgreSQL Extension (C11)
│   ├── orochi.h                  # Main header with all types and API declarations
│   ├── core/                     # Extension initialization and catalog
│   ├── storage/                  # Columnar storage and compression
│   ├── sharding/                 # Distribution and physical sharding
│   ├── timeseries/               # Hypertables and time partitioning
│   ├── tiered/                   # Tiered storage with S3
│   ├── vector/                   # Vector operations and SIMD
│   ├── planner/                  # Distributed query planner
│   ├── executor/                 # Distributed and vectorized execution
│   ├── jit/                      # JIT expression compilation
│   ├── consensus/                # Raft consensus protocol
│   ├── pipelines/                # Data ingestion pipelines
│   ├── cdc/                      # Change Data Capture
│   ├── workload/                 # Resource pools and admission control
│   ├── auth/                     # Authentication (JWT, WebAuthn, Magic Links)
│   ├── json/                     # JSON processing and indexing
│   └── ddl/                      # Dynamic DDL workflow
│
├── sql/                          # SQL extension definitions
│   └── orochi--1.0.sql
│
├── test/                         # Testing infrastructure
│   ├── unit/                     # Standalone unit tests (no PostgreSQL)
│   ├── sql/                      # Regression tests
│   └── expected/                 # Expected test output
│
├── benchmark/                    # Performance benchmarks
│   ├── columnar/                 # Columnar storage benchmarks
│   ├── distributed/              # Distributed query benchmarks
│   ├── timeseries/               # Time-series benchmarks
│   ├── tpch/                     # TPC-H standard benchmarks
│   └── vectorized/               # Vectorized execution benchmarks
│
├── orochi-cloud/                 # Cloud Platform (Go + React)
│   ├── control-plane/            # Cluster management API (Go)
│   ├── dashboard/                # Web UI (React 19 + TanStack)
│   ├── cli/                      # Command-line tool (Go)
│   ├── autoscaler/               # Automatic scaling service (Go)
│   ├── provisioner/              # Cluster provisioning (Go)
│   ├── infrastructure/           # Kubernetes manifests and Helm charts
│   └── shared/                   # Shared Go libraries
│
└── docs/                         # Documentation
    ├── DEVELOPMENT.md            # Development environment setup
    ├── ARCHITECTURE.md           # System architecture
    ├── architecture.md           # Extension architecture
    ├── user-guide.md             # User guide
    └── design/                   # Design documents
```

See [ARCHITECTURE.md](docs/ARCHITECTURE.md) for detailed component interactions.

## Documentation

### Getting Started

- [README.md](README.md) - This file
- [Quick Start Guide](docs/user-guide.md) - Basic usage and tutorials
- [Development Setup](docs/DEVELOPMENT.md) - Full development environment setup

### PostgreSQL Extension

- [Extension Architecture](docs/architecture.md) - Technical architecture
- [PostgreSQL Extension README](extensions/postgres/README.md) - Build and test instructions (PLANNED)
- [CLAUDE.md](CLAUDE.md) - AI agent development guide

### Cloud Platform

- [Cloud Architecture](docs/OROCHI_CLOUD_ARCHITECTURE.md) - Cloud platform design
- [Dashboard README](orochi-cloud/dashboard/README.md) - Web UI development (PLANNED)
- [CLI Documentation](orochi-cloud/cli/README.md) - Command-line tool usage (PLANNED)
- [Services README](orochi-cloud/README.md) - Go services overview

### Design Documents

- [Authentication Architecture](docs/AUTH_ARCHITECTURE_DESIGN.md)
- [Autoscaling Design](docs/AUTOSCALING-DESIGN.md)
- [JSON Indexing Design](docs/JSON_INDEXING_DESIGN.md)
- [UI Dashboard Design](docs/UI-DASHBOARD-DESIGN.md)

### Operations

- [Production Readiness Review](docs/PRODUCTION_READINESS_REVIEW.md)
- [Deployment Guide](orochi-cloud/dashboard/DEPLOYMENT.md)
- [Secrets Management](orochi-cloud/dashboard/SECRETS_MANAGEMENT.md)

## Use Cases

### Real-Time Analytics
Dashboards, reporting, and ad-hoc queries with columnar storage and vectorized execution.

### AI/ML Workloads
Embedding storage, similarity search, and RAG applications with native vector support and SIMD optimization.

### IoT and Telemetry
Sensor data, time-series metrics with automatic partitioning, compression, and tiered storage.

### Multi-Tenant SaaS
Tenant isolation via sharding, scalable storage, and resource pool management.

### Event Streaming
Real-time data ingestion from Kafka, S3, and CDC for streaming analytics.

## Architecture Overview

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

### Cloud Platform Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Orochi Cloud Platform                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │  Dashboard   │  │   CLI Tool   │  │   Control Plane API  │  │
│  │ (React 19)   │  │  (orochi-cli)│  │        (Go)          │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘  │
│         │                 │                      │               │
│         └─────────────────┼──────────────────────┘               │
│                           │                                      │
│                           ▼                                      │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                    Kubernetes Cluster                       │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │ │
│  │  │  Autoscaler  │  │  Provisioner │  │  CloudNativePG  │  │ │
│  │  │   Service    │  │   Service    │  │    Operator     │  │ │
│  │  └──────────────┘  └──────────────┘  └─────────────────┘  │ │
│  │                           │                                 │ │
│  │                           ▼                                 │ │
│  │  ┌────────────────────────────────────────────────────┐   │ │
│  │  │              Orochi DB Clusters                     │   │ │
│  │  │  ┌─────────┐  ┌─────────┐  ┌─────────┐            │   │ │
│  │  │  │ Primary │  │ Replica │  │ Replica │  ...       │   │ │
│  │  │  └─────────┘  └─────────┘  └─────────┘            │   │ │
│  │  └────────────────────────────────────────────────────┘   │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

## Development

### PostgreSQL Extension

```bash
# Install dependencies
make install-deps

# Build (optimized)
make

# Build (debug)
make DEBUG=1

# Install
sudo make install

# Run unit tests (standalone)
cd test/unit && make && ./run_tests

# Run regression tests (requires PostgreSQL)
make installcheck

# Format code
make format

# Static analysis
make lint

# Run benchmarks
cd benchmark && make && ./run_all.sh
```

### Cloud Platform

```bash
# Start all services locally
cd orochi-cloud
make dev

# Run tests
make test

# Build all components
make build

# Build CLI only
cd cli && go build -o orochi-cli ./cmd/orochi-cli

# Run dashboard locally
cd dashboard && npm install && npm run dev
```

See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for detailed setup instructions.

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
| Streaming Pipelines | ❌ | ❌ | ❌ | ✅ |
| Cloud Management | ❌ | Limited | ✅ | ✅ Full Platform |

## Contributing

We welcome contributions! Please see our contributing guidelines:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Development Guidelines

- **C Code**: Follow PostgreSQL coding conventions, use `make format`
- **Go Code**: Follow standard Go conventions, use `gofmt` and `golangci-lint`
- **React Code**: Use TypeScript, follow ESLint rules
- **Tests**: Add tests for all new features
- **Documentation**: Update docs for user-facing changes

See [CLAUDE.md](CLAUDE.md) for AI agent development patterns.

## Roadmap

- [ ] Complete monorepo restructure (extensions/, services/, apps/, tools/, packages/)
- [ ] Production-ready cloud platform deployment
- [ ] Enhanced autoscaling with ML-based predictions
- [ ] Multi-cloud support (AWS, GCP, Azure)
- [ ] Advanced query optimization with learned indexes
- [ ] Extended vector operations (quantization, filtering)
- [ ] GraphQL API support
- [ ] WebAssembly extensions

See [TODO.md](TODO.md) and [PLAN.md](PLAN.md) for detailed roadmap.

## License

Apache License 2.0 (TBD - pending license file)

## Acknowledgments

Inspired by:
- [Citus](https://github.com/citusdata/citus) - Distributed PostgreSQL
- [TimescaleDB](https://github.com/timescale/timescaledb) - Time-series optimization
- [Hydra](https://github.com/hydradatabase/columnar) - Columnar storage
- [pgvector](https://github.com/pgvector/pgvector) - Vector similarity search

## Support

- **Documentation**: [docs/](docs/)
- **Issues**: [GitHub Issues](https://github.com/jbeck018/orochi-db/issues)
- **Discussions**: [GitHub Discussions](https://github.com/jbeck018/orochi-db/discussions)

---

**Orochi DB** - Unleash the power of PostgreSQL for modern data workloads.
