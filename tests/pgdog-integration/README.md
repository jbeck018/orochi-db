# PgDog Integration Tests

Integration test suite for PgDog router with Orochi DB.

## Directory Structure

```
tests/pgdog-integration/
├── README.md                    # This file
├── docker-compose.test.yml      # Test environment
├── Dockerfile.test              # Test runner image
├── go.mod                       # Go module
├── config/
│   ├── pgdog.toml              # PgDog test config
│   └── prometheus.yml          # Prometheus config
├── init-scripts/
│   └── 01-setup-orochi.sql     # Database setup
├── connection_test.go           # Connection tests
├── sharding_test.go            # Sharding tests
├── auth_test.go                # Authentication tests
├── performance_test.go         # Performance benchmarks
└── chaos_test.go               # Chaos/resilience tests
```

## Prerequisites

- Docker and Docker Compose
- Go 1.22+
- Access to container registry

## Quick Start

```bash
# Start test environment
docker-compose -f docker-compose.test.yml up -d

# Wait for services to be ready
docker-compose -f docker-compose.test.yml ps

# Run all tests
go test -v ./...

# Run specific test category
go test -v -run TestConnection ./...
go test -v -run TestSharding ./...
go test -v -run TestAuth ./...

# Run benchmarks
go test -bench=. -benchmem ./...

# Cleanup
docker-compose -f docker-compose.test.yml down -v
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PGDOG_HOST` | `localhost` | PgDog hostname |
| `PGDOG_PORT` | `5432` | PgDog port |
| `POSTGRES_USER` | `postgres` | Database user |
| `POSTGRES_PASSWORD` | `testpassword` | Database password |
| `POSTGRES_DB` | `orochi_test` | Database name |
| `TEST_JWT_SECRET` | - | JWT signing secret |

## Test Categories

### Connection Tests (P0)
- Basic connectivity
- TLS/SSL verification
- Connection pooling behavior
- Prepared statements
- Transaction handling

### Sharding Tests (P0)
- Hash function compatibility with orochi_hash()
- Single-shard query routing
- Multi-shard query aggregation
- 2PC transaction handling

### Authentication Tests (P0)
- JWT token validation
- Token expiration handling
- Invalid token rejection
- Session variable injection

### Read/Write Split Tests (P0)
- SELECT routing to replicas
- Write routing to primary
- Transaction stickiness

### Performance Tests (P1)
- Connection latency
- Query overhead
- Throughput benchmarks
- Memory usage

### Chaos Tests (P2)
- Failover handling
- Network partition recovery
- Connection recovery

## CI/CD Integration

```yaml
# .github/workflows/pgdog-tests.yml
name: PgDog Integration Tests
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Start test environment
        run: docker-compose -f tests/pgdog-integration/docker-compose.test.yml up -d
      - name: Wait for services
        run: sleep 30
      - name: Run tests
        run: cd tests/pgdog-integration && go test -v ./...
      - name: Cleanup
        run: docker-compose -f tests/pgdog-integration/docker-compose.test.yml down -v
```

## Debugging

```bash
# View PgDog logs
docker logs pgdog-test-router -f

# Connect directly to PgDog
psql "host=localhost port=5432 user=postgres password=testpassword dbname=orochi_test"

# View metrics
curl http://localhost:9090/metrics

# Access PgDog admin
psql "host=localhost port=6432 user=admin dbname=pgdog"
```

## Adding New Tests

1. Create test file following naming convention: `*_test.go`
2. Use test IDs from the strategy document (e.g., `CONN-001`, `SHARD-005`)
3. Add completion marker to `PGDOG_IMPLEMENTATION_STRATEGY.md`
4. Run tests locally before committing
