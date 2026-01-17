# Orochi Cloud Services

Cloud-native management platform for Orochi DB clusters built with Go microservices.

## Overview

The Orochi Cloud platform provides cluster lifecycle management, automatic scaling, provisioning, and monitoring for Orochi DB PostgreSQL clusters running on Kubernetes with CloudNativePG.

## Services

| Service | Directory | Description | Port |
|---------|-----------|-------------|------|
| Control Plane | `control-plane/` | REST/gRPC API for cluster management | 8080 |
| Provisioner | `provisioner/` | Cluster provisioning with CloudNativePG | 8081 |
| Autoscaler | `autoscaler/` | Automatic scaling based on metrics | 8082 |
| CLI | `cli/` | Command-line interface | N/A |
| Dashboard | `dashboard/` | Web UI (React 19 + TanStack) | 3000 |

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Orochi Cloud Platform                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │  Dashboard   │  │   CLI Tool   │  │   Control Plane API  │  │
│  │  (React 19)  │  │ (orochi-cli) │  │        (Go)          │  │
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

## Quick Start

### Prerequisites

```bash
# Go 1.22+
go version

# Node.js 20+ (for dashboard)
node --version

# Kubernetes cluster
kubectl version

# CloudNativePG operator
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml
```

### Build All Services

```bash
cd orochi-cloud

# Build all Go services
make build

# Run tests
make test

# Build dashboard
cd dashboard && npm install && npm run build
```

### Run Locally

#### Terminal 1: Control Plane

```bash
cd orochi-cloud/control-plane

export POSTGRES_URL="postgres://user:pass@localhost:5432/orochi_cloud"
export KUBERNETES_CONFIG="$HOME/.kube/config"
export LOG_LEVEL="debug"
export PORT="8080"

go run ./cmd/control-plane
```

#### Terminal 2: Provisioner

```bash
cd orochi-cloud/provisioner

export KUBERNETES_CONFIG="$HOME/.kube/config"
export POSTGRES_URL="postgres://user:pass@localhost:5432/orochi_cloud"
export CONTROL_PLANE_URL="http://localhost:8080"

go run ./cmd/provisioner
```

#### Terminal 3: Autoscaler

```bash
cd orochi-cloud/autoscaler

export KUBERNETES_CONFIG="$HOME/.kube/config"
export CONTROL_PLANE_URL="http://localhost:8080"
export METRICS_INTERVAL="30s"

go run ./cmd/autoscaler
```

#### Terminal 4: Dashboard

```bash
cd orochi-cloud/dashboard

npm run dev
# Access at http://localhost:3000
```

### Using the CLI

```bash
cd orochi-cloud/cli

# Install CLI
go install ./...

# Configure
export OROCHI_API_URL="http://localhost:8080"

# Create cluster
orochi cluster create my-cluster --size small

# List clusters
orochi cluster list

# Get cluster details
orochi cluster get my-cluster

# Scale cluster
orochi cluster scale my-cluster --replicas 5

# Delete cluster
orochi cluster delete my-cluster
```

## Development

### Project Structure

```
orochi-cloud/
├── control-plane/          # Cluster management API
│   ├── cmd/
│   │   └── control-plane/  # Server entrypoint
│   ├── internal/
│   │   ├── api/            # HTTP/gRPC handlers
│   │   ├── services/       # Business logic
│   │   └── db/             # Database layer
│   ├── pkg/                # Public packages
│   └── go.mod
│
├── provisioner/            # Cluster provisioning
│   ├── cmd/
│   │   └── provisioner/
│   ├── internal/
│   │   ├── provisioner/    # Provisioning logic
│   │   ├── templates/      # CloudNativePG templates
│   │   └── k8s/            # Kubernetes client
│   └── go.mod
│
├── autoscaler/             # Automatic scaling
│   ├── cmd/
│   │   └── autoscaler/
│   ├── internal/
│   │   ├── metrics/        # Metrics collection
│   │   └── scaler/         # Scaling logic
│   └── go.mod
│
├── cli/                    # CLI tool
│   ├── cmd/
│   │   └── orochi-cli/
│   ├── internal/
│   │   ├── cmd/            # Command implementations
│   │   └── api/            # API client
│   └── go.mod
│
├── dashboard/              # Web UI
│   ├── src/
│   │   ├── routes/         # TanStack Router routes
│   │   └── components/     # React components
│   ├── components/         # shadcn/ui components
│   └── package.json
│
├── shared/                 # Shared Go libraries
│   ├── types/              # Common types
│   └── go.mod
│
└── infrastructure/         # Kubernetes manifests
    ├── manifests/          # K8s YAML
    ├── helm/               # Helm charts
    └── kustomize/          # Kustomize overlays
```

### Adding a New Service

1. **Create service directory**:
   ```bash
   mkdir -p myservice/cmd/myservice
   mkdir -p myservice/internal/service
   ```

2. **Initialize Go module**:
   ```bash
   cd myservice
   go mod init github.com/jbeck018/orochi-db/orochi-cloud/myservice
   ```

3. **Create main.go**:
   ```go
   package main

   import (
       "log"
       "github.com/jbeck018/orochi-db/orochi-cloud/myservice/internal/service"
   )

   func main() {
       svc := service.New()
       if err := svc.Run(); err != nil {
           log.Fatal(err)
       }
   }
   ```

4. **Add to Makefile**:
   ```makefile
   .PHONY: build-myservice
   build-myservice:
       cd myservice && go build -o myservice ./cmd/myservice
   ```

### Running Tests

```bash
# Test all services
make test

# Test specific service
cd control-plane && go test ./...

# Test with coverage
cd control-plane && go test -coverprofile=coverage.out ./...
go tool cover -html=coverage.out

# Integration tests
cd control-plane && go test -tags=integration ./...
```

### Code Quality

```bash
# Format code
make fmt

# Lint code
make lint

# Vet code
make vet

# Run all checks
make check
```

## API Documentation

### Control Plane API

**Base URL**: `http://localhost:8080/api/v1`

#### Clusters

```bash
# Create cluster
POST /clusters
{
  "name": "my-cluster",
  "size": "small",
  "region": "us-east-1",
  "replicas": 3
}

# List clusters
GET /clusters

# Get cluster
GET /clusters/:id

# Update cluster
PATCH /clusters/:id
{
  "replicas": 5
}

# Delete cluster
DELETE /clusters/:id

# Scale cluster
POST /clusters/:id/scale
{
  "replicas": 5
}

# Get metrics
GET /clusters/:id/metrics?from=2024-01-01&to=2024-01-02
```

#### Backups

```bash
# Create backup
POST /clusters/:id/backups
{
  "type": "full"
}

# List backups
GET /clusters/:id/backups

# Restore backup
POST /clusters/:id/restore
{
  "backupId": "backup-123"
}
```

#### Users

```bash
# Create user
POST /users
{
  "email": "user@example.com",
  "name": "John Doe"
}

# List users
GET /users

# Get user
GET /users/:id

# Update user
PATCH /users/:id
```

### WebSocket API

```javascript
// Connect to WebSocket
const ws = new WebSocket('ws://localhost:8080/ws/clusters/cluster-123')

ws.onmessage = (event) => {
  const data = JSON.parse(event.data)
  console.log('Cluster update:', data)
}

// Message types:
// - cluster.created
// - cluster.updated
// - cluster.deleted
// - cluster.metrics
// - cluster.scaling
// - cluster.backup.created
// - cluster.backup.completed
```

## Deployment

### Kubernetes Deployment

```bash
# Deploy all services
kubectl apply -f infrastructure/manifests/

# Or use Helm
helm install orochi-cloud infrastructure/helm/orochi-cloud

# Or use Kustomize
kubectl apply -k infrastructure/kustomize/production
```

### Environment Variables

#### Control Plane

```bash
POSTGRES_URL=postgres://user:pass@postgres:5432/orochi_cloud
KUBERNETES_CONFIG=/path/to/kubeconfig
LOG_LEVEL=info
PORT=8080
JWT_SECRET=your-secret-key
CORS_ORIGINS=https://dashboard.orochi.cloud
```

#### Provisioner

```bash
KUBERNETES_CONFIG=/path/to/kubeconfig
POSTGRES_URL=postgres://user:pass@postgres:5432/orochi_cloud
CONTROL_PLANE_URL=http://orochi-control-plane:8080
CNPG_NAMESPACE=orochi-clusters
```

#### Autoscaler

```bash
KUBERNETES_CONFIG=/path/to/kubeconfig
CONTROL_PLANE_URL=http://orochi-control-plane:8080
METRICS_INTERVAL=30s
SCALE_UP_THRESHOLD_CPU=0.8
SCALE_UP_THRESHOLD_MEMORY=0.85
SCALE_DOWN_THRESHOLD_CPU=0.3
SCALE_DOWN_THRESHOLD_MEMORY=0.4
COOLDOWN_PERIOD=5m
```

#### Dashboard

```bash
VITE_API_URL=https://api.orochi.cloud/api/v1
VITE_WS_URL=wss://api.orochi.cloud/ws
```

## Monitoring

### Prometheus Metrics

All services expose Prometheus metrics at `/metrics`:

```bash
# Control plane metrics
curl http://localhost:8080/metrics

# Provisioner metrics
curl http://localhost:8081/metrics

# Autoscaler metrics
curl http://localhost:8082/metrics
```

### Health Checks

```bash
# Health check
GET /health

# Readiness check
GET /ready
```

## Troubleshooting

### Service Won't Start

```bash
# Check logs
docker logs orochi-control-plane

# Check environment variables
env | grep OROCHI

# Test database connection
psql "$POSTGRES_URL" -c "SELECT 1"

# Test Kubernetes connection
kubectl cluster-info
```

### CLI Not Working

```bash
# Check API URL
echo $OROCHI_API_URL

# Test connection
curl $OROCHI_API_URL/health

# Enable debug logging
orochi --debug cluster list
```

### Dashboard Not Loading

```bash
# Check API connection
# Open browser console, check Network tab

# Verify environment variables
cat apps/dashboard/.env

# Check CORS settings
# API must allow dashboard origin
```

## Contributing

See [../docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md) for development setup and guidelines.

## License

Apache License 2.0
