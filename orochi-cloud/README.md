# Orochi Cloud

Cloud-native management platform for Orochi DB - a PostgreSQL HTAP extension.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Orochi Cloud Platform                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │  Dashboard   │  │   CLI Tool   │  │   Control Plane API  │  │
│  │  (Next.js)   │  │  (orochi-cli)│  │        (Go)          │  │
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

## Components

| Component | Directory | Technology | Description |
|-----------|-----------|------------|-------------|
| Control Plane | `/control-plane` | Go 1.22+ | REST/gRPC API for cluster management |
| Dashboard | `/dashboard` | Next.js 15 | Web UI for monitoring and management |
| Autoscaler | `/autoscaler` | Go 1.22+ | Automatic scaling based on metrics |
| Provisioner | `/provisioner` | Go 1.22+ | Cluster provisioning with CloudNativePG |
| Infrastructure | `/infrastructure` | Helm/K8s | Kubernetes manifests and Helm charts |
| CLI | `/cli` | Go 1.22+ | Command-line interface (orochi-cli) |
| Shared | `/shared` | Various | Shared libraries and types |

## Quick Start

```bash
# Install CLI
go install ./cli/...

# Login to Orochi Cloud
orochi login

# Create a new cluster
orochi cluster create my-cluster --size small --region us-east-1

# Check cluster status
orochi cluster status my-cluster
```

## Development

```bash
# Start all services locally
make dev

# Run tests
make test

# Build all components
make build
```

## License

Apache 2.0
