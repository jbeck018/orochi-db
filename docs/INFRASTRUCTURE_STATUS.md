# Orochi DB Infrastructure Status

## Deployed Services (DigitalOcean App Platform)

### Control Plane
- **URL**: https://orochi-control-plane-npk4n.ondigitalocean.app
- **Status**: ✅ Healthy
- **Features**:
  - User registration with organization creation
  - JWT-based authentication
  - Cluster management API (simulated provisioning)
  - Metrics endpoint
  
### Dashboard
- **URL**: https://orochi-dashboard-vrhqn.ondigitalocean.app
- **Status**: ✅ Healthy
- **Built with**: Dockerfile.nginx-node (Node.js build, nginx serve)
- **Notes**: Uses nginx for cross-platform AMD64 compatibility

## Working Features

1. **Authentication**
   - User registration with organization
   - Login/logout with JWT tokens
   - Token refresh
   - CORS properly configured for dashboard origin

2. **Cluster Management (Simulated)**
   - Create cluster
   - List clusters
   - Get cluster details
   - Delete cluster
   - Scale cluster
   - **Note**: Provisioning is simulated - clusters transition to "running" but no actual PostgreSQL instances are created

3. **Organization Management**
   - Create organization on registration
   - Invite users (planned)
   - Membership management

## Pending for Production

### 1. Provisioner Service (Requires Kubernetes)
The provisioner service cannot run on DO App Platform - it requires:
- Kubernetes cluster (DOKS recommended)
- CloudNativePG operator installed
- gRPC connection from control-plane

**Required Environment**:
```yaml
services:
  provisioner:
    requires:
      - kubernetes_cluster: DOKS
      - operators:
          - cloudnative-pg v1.21+
      - grpc_port: 8080
      - health_port: 8081
```

### 2. S3/Tiered Storage
- **Current**: S3 config is per-cluster (customers provide credentials)
- **Not Required**: Global Spaces bucket for platform operation
- **Customer Config**: Customers provide their own S3-compatible storage for cold/frozen tiers

### 3. Control-Plane → Provisioner Integration
The control-plane currently uses simulated provisioning. To enable real cluster creation:
1. Deploy provisioner to Kubernetes
2. Add `PROVISIONER_GRPC_ADDR` to control-plane config
3. Update `startProvisioning()` to call provisioner via gRPC

### 4. Autoscaler Service
- Monitors cluster metrics
- Triggers scaling via provisioner
- Also requires Kubernetes deployment

## Environment Variables

### Control Plane (Configured)
```
DATABASE_URL=<configured>
JWT_SECRET=<configured>
ENCRYPTION_KEY=<configured>
ALLOWED_ORIGINS=https://orochi-dashboard-vrhqn.ondigitalocean.app,http://localhost:3000,http://localhost:5173
```

### Dashboard (Configured)
```
VITE_API_URL=https://orochi-control-plane-npk4n.ondigitalocean.app
```

## Architecture Summary

```
┌─────────────────────────────────────────────────────────┐
│           DigitalOcean App Platform (PaaS)             │
├───────────────────────┬─────────────────────────────────┤
│     Dashboard         │        Control Plane            │
│   (nginx/static)      │     (Go HTTP/REST API)          │
│                       │                                 │
│  React 19 + Vite      │  - Auth (JWT)                   │
│  TanStack Router      │  - Cluster CRUD                 │
│  Tailwind + shadcn    │  - Organization mgmt            │
│                       │  - Simulated provisioning       │
└───────────────────────┴─────────────────────────────────┘
                              │
                              │ (Future: gRPC)
                              ▼
┌─────────────────────────────────────────────────────────┐
│           DigitalOcean Kubernetes (DOKS)                │
│                    (Not yet deployed)                   │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │ Provisioner │  │  Autoscaler  │  │ CloudNativePG│   │
│  │   (gRPC)    │  │   (metrics)  │  │   Operator   │   │
│  └─────────────┘  └──────────────┘  └──────────────┘   │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │            Customer Orochi DB Clusters           │   │
│  │  (PostgreSQL + Orochi Extension via CNPG)        │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## Next Steps for Production

1. **Create DOKS cluster** in DigitalOcean
2. **Deploy CloudNativePG operator**
3. **Deploy provisioner service** with gRPC endpoint
4. **Update control-plane** to call provisioner instead of simulating
5. **Deploy autoscaler** for automatic scaling
6. **Configure monitoring** (Prometheus/Grafana)

---
*Last updated: 2026-01-18*
