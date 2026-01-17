# Fly.io Kubernetes (FKS) Deployment

This directory contains Kubernetes manifests optimized for Fly.io Kubernetes Service (FKS).

## Overview

FKS is used to deploy services that require Kubernetes API access:
- **Provisioner**: Manages CloudNativePG database clusters
- **Autoscaler**: Scales database clusters based on metrics

Services that don't need K8s are deployed on regular Fly.io:
- **Dashboard**: Deployed to `orochi-dashboard.fly.dev`
- **Control Plane**: Deployed to `orochi-control-plane.fly.dev`

## Prerequisites

1. **Fly.io CLI**:
   ```bash
   brew install flyctl
   fly auth login
   ```

2. **Enable FKS Beta** (if not already):
   ```bash
   fly orgs invite orochi-db --kubernetes
   ```

3. **Create FKS Cluster**:
   ```bash
   fly ext k8s create --name orochi-fks --region sjc
   ```

## Deployment

### 1. Get kubeconfig

```bash
fly ext k8s kubeconfig --name orochi-fks > ~/.kube/orochi-fks.yaml
export KUBECONFIG=~/.kube/orochi-fks.yaml
```

### 2. Install CloudNativePG Operator

```bash
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml
```

### 3. Create Namespace and Secrets

```bash
kubectl apply -f namespace.yaml
kubectl apply -f secrets.yaml
```

### 4. Deploy Services

```bash
# Deploy all FKS services
kubectl apply -f .

# Or deploy individually
kubectl apply -f provisioner/
kubectl apply -f autoscaler/
```

## Architecture

```
                    ┌─────────────────────────────────────────┐
                    │           Fly.io Platform               │
                    ├─────────────────┬───────────────────────┤
                    │  Regular Fly.io │        FKS            │
                    │                 │   (Kubernetes)        │
                    │  ┌───────────┐  │  ┌─────────────────┐  │
                    │  │ Dashboard │  │  │   Provisioner   │  │
Internet ──────────▶│  │   (Bun)   │  │  │      (Go)       │  │
                    │  └───────────┘  │  └────────┬────────┘  │
                    │        │        │           │           │
                    │        ▼        │           ▼           │
                    │  ┌───────────┐  │  ┌─────────────────┐  │
                    │  │  Control  │  │  │   Autoscaler    │  │
                    │  │  Plane    │◀─┼──│      (Go)       │  │
                    │  │   (Go)    │  │  └─────────────────┘  │
                    │  └─────┬─────┘  │           │           │
                    │        │        │           │           │
                    │        ▼        │           ▼           │
                    │  ┌───────────┐  │  ┌─────────────────┐  │
                    │  │    Fly    │  │  │  CloudNativePG  │  │
                    │  │  Postgres │  │  │    Operator     │  │
                    │  └───────────┘  │  └─────────────────┘  │
                    └─────────────────┴───────────────────────┘
```

## Connecting Services

FKS services communicate with Fly.io services via internal network:

- Control Plane: `orochi-control-plane.internal:8080`
- Dashboard: `orochi-dashboard.internal:3000`

Environment variables in FKS deployments use `.internal` hostnames.

## Monitoring

```bash
# View pods
kubectl get pods -n orochi-cloud

# View logs
kubectl logs -f deployment/provisioner -n orochi-cloud
kubectl logs -f deployment/autoscaler -n orochi-cloud

# Check events
kubectl get events -n orochi-cloud --sort-by=.metadata.creationTimestamp
```

## Troubleshooting

### Service Can't Reach Control Plane
FKS uses Fly.io's internal WireGuard mesh. Verify connectivity:
```bash
kubectl run -it --rm debug --image=alpine -- sh
# Inside pod:
apk add curl
curl -v http://orochi-control-plane.internal:8080/health
```

### CloudNativePG Issues
```bash
kubectl get clusters.postgresql.cnpg.io -n orochi-cloud
kubectl describe cluster <cluster-name> -n orochi-cloud
```
