# Orochi DB Development Guide

Complete development environment setup and workflow guide for Orochi DB.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Initial Setup](#initial-setup)
- [PostgreSQL Extension Development](#postgresql-extension-development)
- [Go Services Development](#go-services-development)
- [Dashboard Development](#dashboard-development)
- [Testing](#testing)
- [Debugging](#debugging)
- [Performance Profiling](#performance-profiling)
- [Deployment](#deployment)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### System Requirements

- **OS**: Linux (Ubuntu 22.04+), macOS (12+), or WSL2
- **Memory**: 8GB+ RAM (16GB+ recommended)
- **Disk**: 20GB+ free space
- **CPU**: 4+ cores recommended

### Required Software

#### PostgreSQL Extension

```bash
# PostgreSQL 16, 17, or 18
sudo apt-get install postgresql-18 postgresql-server-dev-18

# Build dependencies
sudo apt-get install -y \
    build-essential \
    git \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    librdkafka-dev \
    clang-format \
    cppcheck

# Or use the makefile target
make install-deps
```

#### Go Services

```bash
# Go 1.22+
wget https://go.dev/dl/go1.22.0.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.22.0.linux-amd64.tar.gz
export PATH=$PATH:/usr/local/go/bin

# Verify
go version
```

#### Dashboard

```bash
# Node.js 20+ and npm
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs

# Verify
node --version
npm --version
```

#### Kubernetes (Optional - for cloud platform development)

```bash
# kubectl
curl -LO "https://dl.k8s.io/release/$(curl -L -s https://dl.k8s.io/release/stable.txt)/bin/linux/amd64/kubectl"
sudo install -o root -g root -m 0755 kubectl /usr/local/bin/kubectl

# minikube (for local testing)
curl -LO https://storage.googleapis.com/minikube/releases/latest/minikube-linux-amd64
sudo install minikube-linux-amd64 /usr/local/bin/minikube

# CloudNativePG operator
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml
```

## Initial Setup

### Clone Repository

```bash
git clone https://github.com/jbeck018/orochi-db.git
cd orochi-db
```

### PostgreSQL Configuration

1. **Configure PostgreSQL**:

```bash
sudo -u postgres psql << 'EOF'
CREATE DATABASE orochi_dev;
CREATE USER orochi_dev WITH PASSWORD 'dev_password';
GRANT ALL PRIVILEGES ON DATABASE orochi_dev TO orochi_dev;
EOF
```

2. **Enable extension loading**:

Edit `/etc/postgresql/18/main/postgresql.conf`:
```
shared_preload_libraries = 'orochi'
max_worker_processes = 16
```

3. **Restart PostgreSQL**:

```bash
sudo systemctl restart postgresql
```

### Environment Setup

Create `.env` file in project root:

```bash
# PostgreSQL
PGHOST=localhost
PGPORT=5432
PGDATABASE=orochi_dev
PGUSER=orochi_dev
PGPASSWORD=dev_password

# Go Services
OROCHI_API_URL=http://localhost:8080
OROCHI_LOG_LEVEL=debug

# Dashboard
VITE_API_URL=http://localhost:8080/api/v1
VITE_WS_URL=ws://localhost:8080/ws
```

## PostgreSQL Extension Development

### Building

```bash
# Standard optimized build
make

# Debug build with symbols
make DEBUG=1

# Install extension
sudo make install

# Create extension in database
psql -d orochi_dev -c "CREATE EXTENSION orochi;"
```

### Development Workflow

1. **Make code changes** in `src/`
2. **Rebuild**:
   ```bash
   make clean && make DEBUG=1
   ```
3. **Reinstall**:
   ```bash
   sudo make install
   ```
4. **Reload in PostgreSQL**:
   ```bash
   psql -d orochi_dev -c "DROP EXTENSION IF EXISTS orochi CASCADE;"
   psql -d orochi_dev -c "CREATE EXTENSION orochi;"
   ```

### Code Formatting

```bash
# Check formatting
make check-format

# Auto-format all C code
make format
```

### Static Analysis

```bash
# Run cppcheck
make lint

# Check for common issues
cppcheck --enable=all --inconclusive --std=c11 src/
```

### Adding New Module

```bash
# 1. Create module directory
mkdir -p src/mymodule

# 2. Create header file
cat > src/mymodule/mymodule.h << 'EOF'
#ifndef OROCHI_MYMODULE_H
#define OROCHI_MYMODULE_H

#include "postgres.h"

/* Function declarations */
extern void orochi_mymodule_init(void);
extern Datum orochi_my_function(PG_FUNCTION_ARGS);

#endif /* OROCHI_MYMODULE_H */
EOF

# 3. Create implementation
cat > src/mymodule/mymodule.c << 'EOF'
#include "postgres.h"
#include "fmgr.h"
#include "mymodule/mymodule.h"

PG_FUNCTION_INFO_V1(orochi_my_function);

void
orochi_mymodule_init(void)
{
    /* Initialization code */
}

Datum
orochi_my_function(PG_FUNCTION_ARGS)
{
    int32 arg = PG_GETARG_INT32(0);
    /* Implementation */
    PG_RETURN_INT32(arg * 2);
}
EOF

# 4. Add to Makefile
# Edit Makefile and add to OBJS:
# OBJS += src/mymodule/mymodule.o

# 5. Add SQL function
# Edit sql/orochi--1.0.sql:
# CREATE FUNCTION my_function(arg integer)
# RETURNS integer
# AS 'MODULE_PATHNAME', 'orochi_my_function'
# LANGUAGE C STRICT;

# 6. Rebuild
make clean && make DEBUG=1 && sudo make install
```

## Go Services Development

### Setup

```bash
cd orochi-cloud

# Initialize Go workspace (if not exists)
go work init
go work use ./shared
go work use ./cli
go work use ./control-plane
go work use ./autoscaler
go work use ./provisioner
```

### Building Services

```bash
# Build all services
make build

# Build specific service
cd control-plane
go build -o control-plane ./cmd/control-plane

# Install CLI globally
cd cli
go install ./...
```

### Running Services Locally

#### Control Plane

```bash
cd orochi-cloud/control-plane

# Set environment variables
export POSTGRES_URL="postgres://orochi_dev:dev_password@localhost:5432/orochi_dev"
export KUBERNETES_CONFIG="$HOME/.kube/config"
export LOG_LEVEL="debug"
export PORT="8080"

# Run
go run ./cmd/control-plane
```

#### Autoscaler

```bash
cd orochi-cloud/autoscaler

export POSTGRES_URL="postgres://orochi_dev:dev_password@localhost:5432/orochi_dev"
export KUBERNETES_CONFIG="$HOME/.kube/config"
export CONTROL_PLANE_URL="http://localhost:8080"

go run ./cmd/autoscaler
```

#### Provisioner

```bash
cd orochi-cloud/provisioner

export KUBERNETES_CONFIG="$HOME/.kube/config"
export POSTGRES_URL="postgres://orochi_dev:dev_password@localhost:5432/orochi_dev"

go run ./cmd/provisioner
```

### Development Workflow

1. **Make code changes**
2. **Run tests**:
   ```bash
   go test ./...
   ```
3. **Run service**:
   ```bash
   go run ./cmd/<service>
   ```
4. **Test with curl**:
   ```bash
   curl http://localhost:8080/api/v1/health
   ```

### Adding New Endpoint

```bash
# 1. Define handler in internal/api/
cat > internal/api/example_handler.go << 'EOF'
package api

import (
    "encoding/json"
    "net/http"
)

func (h *Handler) GetExample(w http.ResponseWriter, r *http.Request) {
    // Implementation
    json.NewEncoder(w).Encode(map[string]string{
        "message": "Hello from example endpoint",
    })
}
EOF

# 2. Register route in internal/api/routes.go
# r.Get("/api/v1/example", h.GetExample)

# 3. Add tests in internal/api/example_handler_test.go
cat > internal/api/example_handler_test.go << 'EOF'
package api

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestGetExample(t *testing.T) {
    handler := &Handler{}
    req := httptest.NewRequest("GET", "/api/v1/example", nil)
    w := httptest.NewRecorder()

    handler.GetExample(w, req)

    if w.Code != http.StatusOK {
        t.Errorf("expected status 200, got %d", w.Code)
    }
}
EOF

# 4. Run tests
go test ./internal/api/...
```

### Code Quality

```bash
# Format code
gofmt -w .
goimports -w .

# Lint
golangci-lint run

# Vet
go vet ./...

# Test with coverage
go test -coverprofile=coverage.out ./...
go tool cover -html=coverage.out
```

## Dashboard Development

### Setup

```bash
cd orochi-cloud/dashboard

# Install dependencies
npm install

# Start development server
npm run dev
```

The dashboard will be available at http://localhost:3000

### Project Structure

```
dashboard/
├── src/
│   ├── routes/              # TanStack Router routes
│   │   ├── __root.tsx       # Root layout
│   │   ├── index.tsx        # Home page
│   │   └── clusters/        # Cluster routes
│   ├── components/          # React components
│   ├── lib/                 # Utilities and API clients
│   └── types/               # TypeScript types
├── components/              # shadcn/ui components
├── hooks/                   # Custom React hooks
└── public/                  # Static assets
```

### Adding New Route

```bash
# 1. Create route file
cat > src/routes/example.tsx << 'EOF'
import { createFileRoute } from '@tanstack/react-router'

export const Route = createFileRoute('/example')({
  component: ExamplePage,
})

function ExamplePage() {
  return (
    <div>
      <h1>Example Page</h1>
    </div>
  )
}
EOF

# 2. Route is automatically registered by TanStack Router
# Access at http://localhost:3000/example
```

### Adding New Component

```bash
# 1. Create component file
cat > src/components/ExampleCard.tsx << 'EOF'
import { Card, CardHeader, CardTitle, CardContent } from "@/components/ui/card"

interface ExampleCardProps {
  title: string
  content: string
}

export function ExampleCard({ title, content }: ExampleCardProps) {
  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <p>{content}</p>
      </CardContent>
    </Card>
  )
}
EOF

# 2. Use in route
# import { ExampleCard } from '@/components/ExampleCard'
# <ExampleCard title="Hello" content="World" />
```

### Development Workflow

1. **Make code changes**
2. **Hot reload** updates automatically
3. **Check browser console** for errors
4. **Test in browser**

### Code Quality

```bash
# Type check
npm run type-check

# Lint
npm run lint

# Fix lint issues
npm run lint -- --fix

# Build for production
npm run build
```

## Testing

### PostgreSQL Extension Tests

#### Unit Tests

```bash
cd test/unit

# Build tests
make

# Run all tests
./run_tests

# Run specific test
./run_tests test_columnar
```

#### Regression Tests

```bash
# After installing extension
make installcheck

# Run specific test
pg_regress --inputdir=test --dbname=orochi_dev columnar
```

#### Adding Unit Test

```bash
# 1. Create test file
cat > test/unit/test_example.c << 'EOF'
#include "test_framework.h"

TEST_SUITE(test_example)
{
    TEST_BEGIN("basic_test")
        int result = 2 + 2;
        TEST_ASSERT_EQ(4, result);
    TEST_END()

    TEST_BEGIN("string_test")
        const char *str = "hello";
        TEST_ASSERT_STR_EQ("hello", str);
    TEST_END()
}
EOF

# 2. Add to run_tests.c
# extern void test_example(void);
# RUN_TEST_SUITE(test_example);

# 3. Add to Makefile OBJS
# OBJS += test_example.o

# 4. Rebuild and run
make clean && make && ./run_tests
```

### Go Service Tests

```bash
# Run all tests
cd orochi-cloud
make test

# Test specific service
cd control-plane
go test ./...

# Test with verbose output
go test -v ./...

# Test with coverage
go test -coverprofile=coverage.out ./...
go tool cover -html=coverage.out

# Run integration tests
go test -tags=integration ./...

# Run tests in parallel
go test -p 4 ./...
```

#### Adding Go Test

```bash
cat > internal/service/example_service_test.go << 'EOF'
package service

import (
    "context"
    "testing"
)

func TestExampleService(t *testing.T) {
    svc := NewExampleService()

    result, err := svc.DoSomething(context.Background())
    if err != nil {
        t.Fatalf("unexpected error: %v", err)
    }

    if result != "expected" {
        t.Errorf("expected 'expected', got '%s'", result)
    }
}
EOF
```

### Dashboard Tests

```bash
cd orochi-cloud/dashboard

# Type check (primary testing method)
npm run type-check

# Lint
npm run lint
```

### Integration Tests

```bash
# 1. Start all services
# Terminal 1: PostgreSQL with extension
sudo make install && sudo systemctl restart postgresql

# Terminal 2: Control plane
cd orochi-cloud/control-plane && go run ./cmd/control-plane

# Terminal 3: Dashboard
cd orochi-cloud/dashboard && npm run dev

# 2. Run integration tests
cd test/integration
go test -v ./...
```

## Debugging

### PostgreSQL Extension Debugging

#### Using GDB

```bash
# 1. Build with debug symbols
make DEBUG=1 && sudo make install

# 2. Find PostgreSQL backend PID
psql -d orochi_dev -c "SELECT pg_backend_pid();"

# 3. Attach GDB
sudo gdb -p <backend_pid>

# 4. Set breakpoints
(gdb) break orochi_create_distributed_table
(gdb) continue

# 5. Execute SQL in another terminal
psql -d orochi_dev -c "SELECT create_distributed_table('my_table', 'id');"

# 6. Debug in GDB
(gdb) step
(gdb) print variable_name
(gdb) backtrace
```

#### Logging

```bash
# Enable verbose logging
# Edit postgresql.conf:
log_min_messages = debug1
log_statement = 'all'

# Restart PostgreSQL
sudo systemctl restart postgresql

# Watch logs
tail -f /var/log/postgresql/postgresql-18-main.log

# In code, use elog:
elog(DEBUG1, "Variable value: %d", value);
elog(LOG, "Important information");
elog(WARNING, "Warning message");
elog(ERROR, "Error occurred");
```

### Go Service Debugging

#### Using Delve

```bash
# Install delve
go install github.com/go-delve/delve/cmd/dlv@latest

# Debug service
cd orochi-cloud/control-plane
dlv debug ./cmd/control-plane

# Set breakpoints
(dlv) break main.main
(dlv) break internal/api/cluster_handler.go:42
(dlv) continue

# Debug commands
(dlv) step         # Step into
(dlv) next         # Step over
(dlv) print var    # Print variable
(dlv) goroutines   # List goroutines
(dlv) stack        # Show stack trace
```

#### Logging

```bash
# Enable debug logging
export LOG_LEVEL=debug
go run ./cmd/control-plane

# In code, use slog:
import "log/slog"

slog.Debug("Debug message", "key", "value")
slog.Info("Info message", "key", "value")
slog.Warn("Warning message", "key", "value")
slog.Error("Error message", "key", "value", "error", err)
```

### Dashboard Debugging

```bash
# Run with source maps
npm run dev

# Browser DevTools
# - Console: Check for errors
# - Network: Inspect API calls
# - React DevTools: Inspect component state

# Add console logging
console.log('Debug:', value)
console.error('Error:', error)
console.table(data)
```

## Performance Profiling

### PostgreSQL Extension

```bash
# 1. Build with profiling enabled
make CFLAGS="-pg -O2"

# 2. Run queries
psql -d orochi_dev -f benchmark.sql

# 3. Analyze with gprof
gprof /usr/lib/postgresql/18/lib/orochi.so gmon.out > analysis.txt
```

### Go Services

```bash
# CPU profiling
go run ./cmd/control-plane -cpuprofile=cpu.prof
# Run load tests
go tool pprof cpu.prof

# Memory profiling
go run ./cmd/control-plane -memprofile=mem.prof
go tool pprof mem.prof

# Live profiling
go run ./cmd/control-plane &
# Service exposes /debug/pprof endpoints
go tool pprof http://localhost:8080/debug/pprof/profile
go tool pprof http://localhost:8080/debug/pprof/heap
```

### Benchmarking

```bash
# Extension benchmarks
cd benchmark
make
./run_all.sh

# Go benchmarks
cd orochi-cloud/control-plane
go test -bench=. -benchmem ./...

# Dashboard performance
cd orochi-cloud/dashboard
npm run build
# Use Lighthouse in Chrome DevTools
```

## Deployment

### Local Development Deployment

```bash
# 1. Build everything
make && sudo make install
cd orochi-cloud && make build
cd dashboard && npm run build

# 2. Start services
docker-compose up -d

# 3. Access
# - PostgreSQL: localhost:5432
# - Control Plane: localhost:8080
# - Dashboard: localhost:3000
```

### Kubernetes Deployment

```bash
# 1. Start minikube
minikube start

# 2. Build images
cd orochi-cloud
make docker-build

# 3. Deploy
kubectl apply -f infrastructure/manifests/

# 4. Access services
kubectl port-forward svc/orochi-control-plane 8080:8080
kubectl port-forward svc/orochi-dashboard 3000:3000
```

## Troubleshooting

### Extension Won't Load

```bash
# Check PostgreSQL logs
sudo tail -100 /var/log/postgresql/postgresql-18-main.log

# Verify extension file exists
ls -la /usr/lib/postgresql/18/lib/orochi.so

# Check postgresql.conf
grep shared_preload_libraries /etc/postgresql/18/main/postgresql.conf

# Verify extension installed correctly
psql -d orochi_dev -c "SELECT * FROM pg_available_extensions WHERE name = 'orochi';"
```

### Go Service Won't Start

```bash
# Check environment variables
env | grep OROCHI

# Verify PostgreSQL connection
psql "$POSTGRES_URL" -c "SELECT 1;"

# Check port availability
netstat -tuln | grep 8080

# Run with verbose logging
LOG_LEVEL=debug go run ./cmd/control-plane
```

### Dashboard Build Errors

```bash
# Clear cache
rm -rf node_modules package-lock.json
npm cache clean --force
npm install

# Check Node version
node --version  # Should be 20+

# Update dependencies
npm update
```

### Common Issues

#### "Extension not found"
```bash
# Solution: Reinstall extension
sudo make install
psql -d orochi_dev -c "CREATE EXTENSION orochi;"
```

#### "Port already in use"
```bash
# Find process using port
lsof -i :8080
kill -9 <pid>
```

#### "Cannot connect to Kubernetes"
```bash
# Verify kubeconfig
kubectl cluster-info

# Check minikube status
minikube status

# Restart minikube
minikube delete && minikube start
```

## Best Practices

1. **Always use DEBUG=1 for development builds**
2. **Run tests before committing**
3. **Format code before committing**
4. **Use feature branches**
5. **Write tests for new features**
6. **Document new APIs**
7. **Update CLAUDE.md for AI agents**
8. **Keep dependencies up to date**
9. **Use semantic commit messages**
10. **Review logs regularly**

## Additional Resources

- [PostgreSQL Extension Development](https://www.postgresql.org/docs/current/extend-extensions.html)
- [Go Best Practices](https://go.dev/doc/effective_go)
- [React Documentation](https://react.dev/)
- [TanStack Router](https://tanstack.com/router)
- [CloudNativePG](https://cloudnative-pg.io/documentation/)
