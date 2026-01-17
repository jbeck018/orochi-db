# Orochi DB - AI Agent Development Guide

A comprehensive guide for AI agents working with Orochi DB, a PostgreSQL HTAP extension with cloud platform.

## Project Overview

Orochi DB is a dual-component system:

1. **PostgreSQL Extension (C11)**: Core HTAP functionality built as a PostgreSQL extension
2. **Cloud Platform (Go + React)**: Management and orchestration layer for running Orochi DB clusters

### PostgreSQL Extension Features

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
- **Authentication**: JWT, WebAuthn, Magic Links, GoTrue integration
- **JSON Processing**: JSON query processing, columnar JSON storage, JSON indexing
- **Dynamic DDL**: Distributed schema changes and DDL workflow orchestration

### Cloud Platform Features

- **Control Plane**: REST/gRPC API for cluster lifecycle management
- **Web Dashboard**: React 19 UI with TanStack Router
- **CLI Tool**: Full-featured command-line interface
- **Autoscaler**: Automatic scaling based on metrics
- **Provisioner**: Cluster provisioning with CloudNativePG
- **Infrastructure**: Kubernetes manifests and Helm charts

**Version**: 1.0.0
**Supported PostgreSQL**: 16, 17, 18
**Minimum PostgreSQL**: 16+
**Extension Language**: C11
**Cloud Services Language**: Go 1.22+
**Dashboard**: React 19 + TypeScript

---

## Directory Structure (Monorepo)

```
/Users/jacob/projects/orochi-db/
├── extensions/
│   └── postgres/                  # PostgreSQL Extension (C11)
│       ├── src/
│       │   ├── orochi.h          # Main header - all types, enums, API declarations
│       │   ├── core/             # Init, catalog management
│       │   ├── storage/          # Columnar storage, compression
│       │   ├── sharding/         # Distribution, physical sharding
│       │   ├── timeseries/       # Hypertables, chunks
│       │   ├── tiered/           # Hot/warm/cold/frozen tiers
│       │   ├── vector/           # SIMD vector operations
│       │   ├── planner/          # Distributed query planning
│       │   ├── executor/         # Distributed execution, vectorized
│       │   ├── jit/              # JIT compilation
│       │   ├── consensus/        # Raft protocol
│       │   ├── pipelines/        # Data pipelines, Kafka
│       │   ├── cdc/              # Change data capture
│       │   ├── workload/         # Resource pools
│       │   ├── approx/           # HyperLogLog, T-Digest
│       │   ├── auth/             # Authentication (JWT, WebAuthn)
│       │   ├── json/             # JSON processing
│       │   ├── ddl/              # Dynamic DDL
│       │   └── utils/
│       ├── sql/                  # Extension SQL definitions
│       ├── test/                 # Unit and regression tests
│       ├── benchmark/            # Performance benchmarks
│       ├── docs/                 # Extension documentation
│       ├── Makefile              # PGXS build system
│       └── orochi.control
│
├── services/                      # Go Backend Services
│   ├── control-plane/            # Cluster management API
│   │   ├── cmd/server/           # Server entrypoint
│   │   ├── internal/
│   │   │   ├── api/              # HTTP handlers, middleware
│   │   │   ├── services/         # Business logic
│   │   │   ├── models/           # Data models
│   │   │   ├── db/               # Database layer
│   │   │   └── auth/             # JWT authentication
│   │   ├── pkg/                  # Public packages
│   │   └── go.mod
│   ├── provisioner/              # Cluster provisioning service
│   │   ├── cmd/provisioner/
│   │   ├── internal/
│   │   │   ├── grpc/             # gRPC server
│   │   │   ├── k8s/              # Kubernetes client
│   │   │   ├── provisioner/      # Service logic
│   │   │   └── cloudnativepg/    # CNPG integration
│   │   ├── templates/            # CloudNativePG templates
│   │   └── go.mod
│   ├── autoscaler/               # Automatic scaling service
│   │   ├── cmd/autoscaler/
│   │   ├── internal/
│   │   │   ├── metrics/          # Metrics collection
│   │   │   ├── grpc/             # gRPC server
│   │   │   ├── k8s/              # Kubernetes client
│   │   │   └── scaler/           # Scaling logic
│   │   └── go.mod
│   ├── Makefile                  # Services build
│   └── README.md
│
├── apps/
│   └── dashboard/                # Web UI (React 19 + TypeScript)
│       ├── src/
│       │   ├── routes/           # TanStack Router routes
│       │   └── client.tsx
│       ├── components/           # React components
│       │   ├── ui/               # shadcn/ui components
│       │   ├── auth/             # Auth forms
│       │   ├── clusters/         # Cluster components
│       │   └── layout/           # Layout components
│       ├── hooks/                # Custom React hooks
│       ├── lib/                  # Utilities
│       ├── types/                # TypeScript types
│       ├── package.json
│       └── vite.config.ts
│
├── tools/
│   └── cli/                      # CLI Tool (Go)
│       ├── cmd/orochi/           # CLI entrypoint
│       ├── internal/
│       │   ├── cmd/              # Command implementations
│       │   ├── api/              # API client
│       │   └── output/           # Output formatting
│       └── go.mod
│
├── packages/
│   └── go/
│       └── shared/               # Shared Go libraries
│           ├── types/            # Common type definitions
│           └── go.mod
│
├── infrastructure/               # Kubernetes infrastructure
│   ├── manifests/                # K8s manifests
│   ├── helm/                     # Helm charts
│   └── kustomize/                # Kustomize overlays
│
├── go.work                       # Go workspace file
├── package.json                  # npm workspaces
├── Makefile                      # Root Makefile (delegates to subdirs)
└── docs/                         # Project documentation
```

---

## Build Instructions

### Quick Start (Monorepo)

```bash
# Build everything
make build-all

# Or build individual components
make extension        # PostgreSQL extension
make services         # All Go services
make cli              # CLI tool
make dashboard        # Dashboard

# Run all tests
make test

# Format and lint
make format
make lint
```

### PostgreSQL Extension

#### Prerequisites

```bash
# Ubuntu/Debian - install all dependencies
cd extensions/postgres && make install-deps

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

#### Building

```bash
# Standard build (optimized with -O3)
make extension

# Debug build (with symbols, no optimization)
cd extensions/postgres && make DEBUG=1

# Install to PostgreSQL
make extension-install

# Clean build artifacts
make extension-clean
```

#### PostgreSQL Configuration

Add to `postgresql.conf`:
```
shared_preload_libraries = 'orochi'
```

Restart PostgreSQL, then create the extension:
```sql
CREATE EXTENSION orochi;
```

### Cloud Platform

#### Prerequisites

```bash
# Go 1.22+
go version

# Node.js 20+ and npm
node --version
npm --version

# Kubernetes cluster (for deployment)
kubectl version

# CloudNativePG operator (for cluster provisioning)
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml
```

#### Building Services

```bash
# Build all services (uses go.work)
make services

# Build specific service
make services-control-plane
make services-provisioner
make services-autoscaler

# Run tests
make services-test

# Tidy Go modules
make go-tidy
```

#### Building CLI

```bash
# Build CLI
make cli

# Install globally
make cli-install

# Test CLI
make cli-test
```

#### Building Dashboard

```bash
# Build for production
make dashboard

# Run development server
make dashboard-dev

# Or manually:
cd apps/dashboard
npm install
npm run dev
```

---

## Testing Instructions

### PostgreSQL Extension Tests

#### Unit Tests (Standalone - No PostgreSQL Required)

```bash
# Via root Makefile
make extension-unit-test

# Or directly
cd extensions/postgres/test/unit
make
./run_tests
```

The unit tests use a lightweight framework (`test_framework.h`) that runs without PostgreSQL.

**Test Files (in `extensions/postgres/test/unit/`):**
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
| `test_auth.c` | Authentication system |
| `test_jwt.c` | JWT token handling |
| `test_webauthn.c` | WebAuthn/FIDO2 |
| `test_magic_link.c` | Magic link authentication |
| `test_branch_access.c` | Branch access control |

#### Regression Tests (Requires PostgreSQL)

```bash
# After installing the extension
make extension-test

# Or directly
cd extensions/postgres && make installcheck
```

Test SQL files: `extensions/postgres/test/sql/`
Expected output: `extensions/postgres/test/expected/`

#### Static Analysis

```bash
make lint          # Run all linters
make format        # Auto-format all code

# Extension only
cd extensions/postgres
make lint          # Run cppcheck
make check-format  # Check clang-format compliance
make format        # Auto-format C code
```

#### Performance Benchmarks

```bash
# Build and run all benchmarks
cd extensions/postgres/benchmark
make
./run_all.sh

# Run specific benchmark suite
cd extensions/postgres/benchmark/tpch && ./tpch_bench
cd extensions/postgres/benchmark/vectorized && ./vectorized_bench
```

### Cloud Platform Tests

#### Go Service Tests

```bash
# Test all services
make services-test

# Test specific service
cd services/control-plane && go test ./...
cd services/autoscaler && go test ./...
cd services/provisioner && go test ./...

# Test CLI
make cli-test

# Test with coverage
cd services/control-plane && go test -coverprofile=coverage.out ./...
go tool cover -html=coverage.out

# Run integration tests
cd services/control-plane && go test -tags=integration ./...
```

#### Dashboard Tests

```bash
cd apps/dashboard

# Lint TypeScript
npm run lint

# Type check
npm run type-check
```

---

## Cross-Component Development

### Working on Extension + Dashboard

When developing features that span both the PostgreSQL extension and dashboard:

```bash
# Terminal 1: PostgreSQL with extension
make extension-install && sudo systemctl restart postgresql
psql -c "CREATE EXTENSION IF NOT EXISTS orochi"

# Terminal 2: Control plane API
cd services/control-plane
go run ./cmd/server

# Terminal 3: Dashboard
make dashboard-dev
```

### Working on Extension + CLI

```bash
# Terminal 1: Build and install extension
cd extensions/postgres && make DEBUG=1 && sudo make install

# Terminal 2: Test with CLI
cd tools/cli
go run ./cmd/orochi cluster status my-cluster
```

### Local Development with Kubernetes

```bash
# Start minikube
minikube start

# Install CloudNativePG
kubectl apply -f infrastructure/manifests/cnpg-operator.yaml

# Deploy control plane
kubectl apply -f infrastructure/manifests/control-plane.yaml

# Port forward for local access
kubectl port-forward svc/orochi-control-plane 8080:8080

# Deploy dashboard
kubectl apply -f infrastructure/manifests/dashboard.yaml
kubectl port-forward svc/orochi-dashboard 3000:3000
```

---

## Architecture Overview

### PostgreSQL Extension Data Flow

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

### Cloud Platform Component Interaction

```
┌──────────────┐
│   Dashboard  │
│  (React 19)  │
└──────┬───────┘
       │ HTTP/REST
       ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│    CLI       │────▶│ Control      │────▶│ PostgreSQL   │
│  (Go)        │     │ Plane API    │     │ Metadata DB  │
└──────────────┘     │  (Go)        │     └──────────────┘
                     └──────┬───────┘
                            │ K8s API
                            ▼
                     ┌──────────────┐     ┌──────────────┐
                     │ Provisioner  │────▶│ CloudNativePG│
                     │   Service    │     │   Operator   │
                     └──────────────┘     └──────┬───────┘
                                                 │
                     ┌──────────────┐            │
                     │ Autoscaler   │            │
                     │   Service    │            │
                     └──────┬───────┘            │
                            │ Metrics            │
                            ▼                    ▼
                     ┌────────────────────────────────┐
                     │      Orochi DB Clusters        │
                     │  (PostgreSQL + Extension)      │
                     └────────────────────────────────┘
```

### Module Dependencies

```
Extension (extensions/postgres/src/):
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

Cloud Platform:
apps/dashboard → services/control-plane → services/provisioner → CloudNativePG
       │                    │                       │
       │                    └───────────────────────┴─→ services/autoscaler
       │
       └─→ tools/cli → services/control-plane

Go Workspace (go.work):
├── services/control-plane
├── services/provisioner
├── services/autoscaler
├── tools/cli
└── packages/go/shared
```

---

## Coding Conventions

### PostgreSQL Extension (C11)

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

**Code Style**:
- C11 standard (`-std=c11`)
- 4-space indentation
- Use `make format` before committing
- Prefix public symbols: `orochi_`, `raft_`, `cdc_`, etc.
- Use `static` for file-local functions
- Document functions in header files

### Go Services

**Project Structure** - Follow standard Go project layout:
```
service/
├── cmd/
│   └── service/
│       └── main.go
├── internal/
│   ├── api/
│   ├── service/
│   └── db/
├── pkg/
│   └── client/
├── api/
│   └── openapi.yaml
└── go.mod
```

**Error Handling** - Use structured errors:
```go
import "errors"

var (
    ErrClusterNotFound = errors.New("cluster not found")
    ErrInvalidConfig   = errors.New("invalid configuration")
)

func (s *Service) GetCluster(id string) (*Cluster, error) {
    cluster, err := s.db.FindCluster(id)
    if err != nil {
        return nil, fmt.Errorf("failed to get cluster: %w", err)
    }
    return cluster, nil
}
```

**Code Style**:
- Run `gofmt` and `goimports`
- Use `golangci-lint run`
- Follow [Effective Go](https://golang.org/doc/effective_go.html)
- Use context for cancellation
- Handle errors explicitly
- Use structured logging (slog)

### React Dashboard (TypeScript)

**Component Structure**:
```typescript
// components/ClusterCard.tsx
import { Card, CardHeader, CardTitle, CardContent } from "@/components/ui/card"
import { type Cluster } from "@/types/cluster"

interface ClusterCardProps {
  cluster: Cluster
  onSelect?: (id: string) => void
}

export function ClusterCard({ cluster, onSelect }: ClusterCardProps) {
  return (
    <Card onClick={() => onSelect?.(cluster.id)}>
      <CardHeader>
        <CardTitle>{cluster.name}</CardTitle>
      </CardHeader>
      <CardContent>
        {/* ... */}
      </CardContent>
    </Card>
  )
}
```

**TanStack Router Usage**:
```typescript
// routes/clusters.$clusterId.tsx
import { createFileRoute } from '@tanstack/react-router'
import { ClusterDetail } from '@/components/ClusterDetail'

export const Route = createFileRoute('/clusters/$clusterId')({
  component: ClusterDetailPage,
  loader: async ({ params }) => {
    const cluster = await fetchCluster(params.clusterId)
    return { cluster }
  }
})

function ClusterDetailPage() {
  const { cluster } = Route.useLoaderData()
  return <ClusterDetail cluster={cluster} />
}
```

**Code Style**:
- Use TypeScript strict mode
- Follow ESLint rules
- Use Tailwind CSS for styling
- Prefer shadcn/ui components
- Use TanStack Query for data fetching
- Use TanStack Router for routing

---

## Important Entry Points

### PostgreSQL Extension

| Function | File | Purpose |
|----------|------|---------|
| `_PG_init()` | `extensions/postgres/src/core/init.c` | Extension load - GUCs, hooks, shared memory |
| `_PG_fini()` | `extensions/postgres/src/core/init.c` | Extension unload - restore hooks |
| `orochi_shmem_startup()` | `extensions/postgres/src/core/init.c` | Initialize shared memory structures |

### Go Services

| Service | Entrypoint | Purpose |
|---------|------------|---------|
| Control Plane | `services/control-plane/cmd/server/main.go` | Cluster management API |
| CLI | `tools/cli/cmd/orochi/main.go` | Command-line interface |
| Autoscaler | `services/autoscaler/cmd/autoscaler/main.go` | Automatic scaling service |
| Provisioner | `services/provisioner/cmd/provisioner/main.go` | Cluster provisioning service |

### Dashboard

| File | Purpose |
|------|---------|
| `apps/dashboard/src/client.tsx` | Application entrypoint |
| `apps/dashboard/src/routes/` | TanStack Router routes |

---

## Common Tasks

### Adding a PostgreSQL Extension Feature

1. **Declare in header** (`extensions/postgres/src/module/module.h`)
2. **Implement** (`extensions/postgres/src/module/module.c`)
3. **Add to Makefile OBJS** if new `.c` file (`extensions/postgres/Makefile`)
4. **Add SQL interface** in `extensions/postgres/sql/orochi--1.0.sql` if user-facing
5. **Write unit tests** in `extensions/postgres/test/unit/test_module.c`
6. **Update runner** in `extensions/postgres/test/unit/run_tests.c`

Example:
```c
// extensions/postgres/src/my_module/my_module.h
extern Datum orochi_my_function(PG_FUNCTION_ARGS);

// extensions/postgres/src/my_module/my_module.c
PG_FUNCTION_INFO_V1(orochi_my_function);
Datum
orochi_my_function(PG_FUNCTION_ARGS)
{
    int32 arg = PG_GETARG_INT32(0);
    // implementation
    PG_RETURN_INT32(result);
}

// extensions/postgres/sql/orochi--1.0.sql
CREATE FUNCTION my_function(arg integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'orochi_my_function'
LANGUAGE C STRICT;
```

### Adding a Go Service Endpoint

```go
// services/control-plane/internal/api/handlers/cluster_handler.go
func (h *Handler) GetCluster(w http.ResponseWriter, r *http.Request) {
    id := chi.URLParam(r, "id")

    cluster, err := h.service.GetCluster(r.Context(), id)
    if err != nil {
        http.Error(w, err.Error(), http.StatusInternalServerError)
        return
    }

    json.NewEncoder(w).Encode(cluster)
}

// Register in router
r.Get("/api/v1/clusters/{id}", h.GetCluster)
```

### Adding a Dashboard Route

```typescript
// apps/dashboard/src/routes/clusters.create.tsx
import { createFileRoute } from '@tanstack/react-router'
import { ClusterCreateForm } from '@/components/ClusterCreateForm'

export const Route = createFileRoute('/clusters/create')({
  component: CreateClusterPage,
})

function CreateClusterPage() {
  const navigate = Route.useNavigate()

  const handleCreate = async (data: ClusterConfig) => {
    const cluster = await createCluster(data)
    navigate({ to: '/clusters/$clusterId', params: { clusterId: cluster.id } })
  }

  return <ClusterCreateForm onSubmit={handleCreate} />
}
```

### Cross-Component Feature Development

When adding a feature that spans extension and cloud platform:

1. **Define the extension API** (SQL functions, GUCs)
2. **Implement extension logic** (C code)
3. **Add control plane API** (Go handlers)
4. **Update CLI commands** (Go CLI)
5. **Create dashboard UI** (React components)
6. **Write integration tests** (across all layers)

Example: Adding cluster backup feature

```bash
# 1. Extension: Add backup API
# extensions/postgres/src/backup/backup.c - implement orochi_create_backup()
# extensions/postgres/sql/orochi--1.0.sql - CREATE FUNCTION create_backup()

# 2. Control plane: Add backup endpoint
# services/control-plane/internal/api/handlers/backup_handler.go
# POST /api/v1/clusters/{id}/backups

# 3. CLI: Add backup command
# tools/cli/internal/cmd/backup.go
# orochi cluster backup create <cluster-id>

# 4. Dashboard: Add backup UI
# apps/dashboard/src/routes/clusters.$clusterId.backups.tsx
# apps/dashboard/components/clusters/BackupList.tsx
```

---

## Quick Reference

### Root Makefile Commands

```bash
# Build commands
make build-all           # Build everything
make extension           # Build PostgreSQL extension
make services            # Build all Go services
make cli                 # Build CLI tool
make dashboard           # Build dashboard

# Test commands
make test                # Run all tests
make extension-test      # Run extension regression tests
make extension-unit-test # Run extension unit tests
make services-test       # Run Go service tests
make cli-test            # Run CLI tests

# Clean commands
make clean               # Clean all build artifacts
make extension-clean     # Clean extension artifacts
make services-clean      # Clean service binaries
make cli-clean           # Clean CLI binary
make dashboard-clean     # Clean dashboard artifacts

# Code quality
make lint                # Lint all code
make format              # Format all code

# Go workspace
make go-sync             # Sync go.work
make go-tidy             # Tidy all Go modules

# Development
make dashboard-dev       # Run dashboard dev server
```

### PostgreSQL Extension

```bash
# Build and install
make extension && sudo make extension-install

# Run unit tests
make extension-unit-test

# Run regression tests (after install)
make extension-test

# Format code
cd extensions/postgres && make format

# Static analysis
cd extensions/postgres && make lint

# Run benchmarks
cd extensions/postgres/benchmark && make && ./run_all.sh
```

### Go Services

```bash
# Build all services (using go.work)
make services

# Build specific service
make services-control-plane
make services-provisioner
make services-autoscaler

# Run tests
make services-test

# Run service locally
cd services/control-plane && go run ./cmd/server
cd services/provisioner && go run ./cmd/provisioner
cd services/autoscaler && go run ./cmd/autoscaler
```

### CLI Tool

```bash
# Build CLI
make cli

# Install CLI
make cli-install

# Run CLI tests
make cli-test

# Run CLI directly
cd tools/cli && go run ./cmd/orochi --help
```

### Dashboard

```bash
# Install dependencies and run dev server
make dashboard-dev

# Build for production
make dashboard

# Or manually
cd apps/dashboard
npm install
npm run dev          # Development
npm run build        # Production build
npm run type-check   # Type check
```

---

## Debugging

### PostgreSQL Extension

```bash
# Build with debug symbols
cd extensions/postgres && make DEBUG=1

# Attach GDB to PostgreSQL backend
gdb -p <postgres_backend_pid>

# Check PostgreSQL logs for elog output
tail -f /var/log/postgresql/postgresql-*.log

# Memory debugging
cd extensions/postgres/test/unit
MALLOC_CHECK_=3 ./run_tests
```

### Go Services

```bash
# Run with delve debugger
cd services/control-plane
dlv debug ./cmd/server

# Enable verbose logging
export LOG_LEVEL=debug
go run ./cmd/server

# Profile CPU usage
go run ./cmd/server -cpuprofile=cpu.prof
go tool pprof cpu.prof
```

### Dashboard

```bash
# Run with source maps
cd apps/dashboard && npm run dev

# Use React DevTools in browser
# Install: https://react.dev/learn/react-developer-tools

# Check network requests
# Browser DevTools → Network tab
```

---

## Environment Variables

### Control Plane

```bash
POSTGRES_URL=postgres://user:pass@localhost:5432/orochi_cloud
KUBERNETES_CONFIG=/path/to/kubeconfig
LOG_LEVEL=info
PORT=8080
```

### Dashboard

```bash
VITE_API_URL=http://localhost:8080/api/v1
VITE_WS_URL=ws://localhost:8080/ws
```

### CLI

```bash
OROCHI_API_URL=https://api.orochi.cloud
OROCHI_AUTH_TOKEN=<jwt-token>
```

---

## File Organization Rules

- **Extension source**: `extensions/postgres/src/`
- **Extension tests**: `extensions/postgres/test/unit/` or `extensions/postgres/test/sql/`
- **Extension SQL**: `extensions/postgres/sql/`
- **Extension docs**: `extensions/postgres/docs/`
- **Go services**: `services/<service>/`
- **CLI tool**: `tools/cli/`
- **Shared Go code**: `packages/go/shared/`
- **Dashboard**: `apps/dashboard/`
- **Infrastructure**: `infrastructure/`
- **Project documentation**: `docs/`
- **Root config files**: `go.work`, `package.json`, `Makefile`
- Never save working files to root folder

---

## Additional Resources

- [PostgreSQL Extension Documentation](https://www.postgresql.org/docs/current/extend.html)
- [Go Project Layout](https://github.com/golang-standards/project-layout)
- [TanStack Router Docs](https://tanstack.com/router)
- [shadcn/ui Components](https://ui.shadcn.com/)
- [CloudNativePG](https://cloudnative-pg.io/)
