# Orochi Cloud Implementation Roadmap

## Overview

This document provides a comprehensive implementation plan for Orochi Cloud - a managed database service for Orochi DB, inspired by PlanetScale, Neon, TiDB Cloud, and Tigris Data.

---

## 1. Project Structure (Monorepo)

```
orochi-cloud/
├── apps/
│   ├── web/                      # Dashboard UI (Next.js 15)
│   │   ├── app/                  # App Router pages
│   │   │   ├── (auth)/          # Auth routes (login, signup)
│   │   │   ├── (dashboard)/     # Protected dashboard routes
│   │   │   │   ├── clusters/    # Cluster management
│   │   │   │   ├── projects/    # Project management
│   │   │   │   ├── settings/    # Organization settings
│   │   │   │   └── billing/     # Billing dashboard
│   │   │   └── api/             # API routes
│   │   ├── components/          # App-specific components
│   │   └── lib/                 # App utilities
│   ├── api/                      # Control Plane API (Go)
│   │   ├── cmd/
│   │   │   └── server/          # Main entry point
│   │   ├── internal/
│   │   │   ├── auth/            # Authentication handlers
│   │   │   ├── cluster/         # Cluster management
│   │   │   ├── billing/         # Billing integration
│   │   │   └── middleware/      # HTTP middleware
│   │   ├── pkg/
│   │   │   ├── models/          # Domain models
│   │   │   └── database/        # Database access
│   │   └── proto/               # gRPC definitions
│   ├── cli/                      # CLI Tool (Go)
│   │   ├── cmd/                 # CLI commands
│   │   └── internal/            # CLI internals
│   └── docs/                     # Documentation site (Astro/Starlight)
│       ├── src/content/docs/    # Documentation content
│       └── astro.config.mjs
├── packages/
│   ├── ui/                       # Shared UI Components
│   │   ├── src/
│   │   │   ├── components/      # shadcn/ui based components
│   │   │   ├── hooks/           # Custom React hooks
│   │   │   └── styles/          # Tailwind config
│   │   └── package.json
│   ├── db/                       # Database Schema (Drizzle ORM)
│   │   ├── src/
│   │   │   ├── schema/          # Table definitions
│   │   │   │   ├── organizations.ts
│   │   │   │   ├── teams.ts
│   │   │   │   ├── users.ts
│   │   │   │   ├── projects.ts
│   │   │   │   ├── clusters.ts
│   │   │   │   ├── api-keys.ts
│   │   │   │   └── audit-logs.ts
│   │   │   ├── migrations/      # SQL migrations
│   │   │   └── seed/            # Seed data
│   │   └── drizzle.config.ts
│   ├── sdk/                      # Client SDKs
│   │   ├── typescript/          # TypeScript SDK
│   │   ├── python/              # Python SDK
│   │   └── go/                  # Go SDK
│   ├── config/                   # Shared configuration
│   │   ├── eslint/              # ESLint configs
│   │   └── typescript/          # TSConfig presets
│   └── validators/               # Shared validation schemas (Zod)
├── services/
│   ├── provisioner/              # Database Provisioning Service (Go)
│   │   ├── cmd/
│   │   ├── internal/
│   │   │   ├── kubernetes/      # K8s operator logic
│   │   │   ├── cloudnativepg/   # CNPG integration
│   │   │   └── provisioning/    # Provisioning workflows
│   │   └── deploy/              # Kubernetes manifests
│   ├── autoscaler/               # Auto-scaling Controller (Go)
│   │   ├── cmd/
│   │   ├── internal/
│   │   │   ├── metrics/         # Metrics aggregation
│   │   │   ├── decision/        # Scaling decision engine
│   │   │   ├── execution/       # Scaling execution
│   │   │   └── prediction/      # Predictive scaling
│   │   └── deploy/
│   ├── metrics/                  # Metrics Collection Service (Go)
│   ├── billing/                  # Billing & Metering Service (Go)
│   ├── connection-router/        # Connection Router (PgCat-based)
│   │   ├── pgcat.toml           # PgCat configuration
│   │   ├── src/
│   │   │   ├── wakeup/          # Scale-to-zero wake-up hook
│   │   │   └── metrics/         # Custom metrics integration
│   │   └── Dockerfile           # PgCat + custom plugins
│   └── backup/                   # Backup Service (Go)
├── infra/
│   ├── terraform/
│   │   ├── modules/
│   │   │   ├── vpc/             # VPC configuration
│   │   │   ├── eks/             # EKS cluster
│   │   │   ├── rds/             # Control plane RDS
│   │   │   ├── s3/              # Backup storage
│   │   │   └── monitoring/      # Observability stack
│   │   ├── environments/
│   │   │   ├── dev/
│   │   │   ├── staging/
│   │   │   └── production/
│   │   └── main.tf
│   ├── k8s/
│   │   ├── base/                # Base Kustomize configs
│   │   │   ├── control-plane/   # API, dashboard
│   │   │   ├── data-plane/      # Operator, CNPG
│   │   │   └── observability/   # Prometheus, Grafana
│   │   ├── overlays/
│   │   │   ├── dev/
│   │   │   ├── staging/
│   │   │   └── production/
│   │   └── crds/                # Custom Resource Definitions
│   └── helm/
│       ├── orochi-operator/     # Kubernetes operator chart
│       └── orochi-stack/        # Full stack chart
├── docker/
│   ├── postgresql/              # PostgreSQL + Orochi image
│   ├── api/
│   └── web/
├── scripts/
│   ├── bootstrap.sh             # Initial setup script
│   ├── dev-setup.sh             # Development environment
│   └── deploy.sh                # Deployment script
├── .github/
│   └── workflows/
│       ├── ci.yml               # CI pipeline
│       ├── deploy-staging.yml
│       └── deploy-production.yml
├── turbo.json                    # Turborepo config
├── pnpm-workspace.yaml
└── README.md
```

---

## 2. Implementation Phases

### Phase 1: MVP - Core Platform (Months 1-4)

**Goal:** Launch basic managed Orochi DB service with single-region dedicated clusters

#### Month 1: Foundation Setup

**Week 1-2: Infrastructure Bootstrap**
- [ ] Set up monorepo with Turborepo + pnpm
- [ ] Configure CI/CD pipeline (GitHub Actions)
- [ ] Deploy initial Terraform for Hetzner/Fly.io or AWS
- [ ] Set up Kubernetes cluster (EKS/k3s)
- [ ] Configure secrets management (HashiCorp Vault or Doppler)

**Week 3-4: Control Plane Database**
- [ ] Design and implement database schema (packages/db)
  - Organizations, Users, Teams tables
  - Projects, Clusters tables
  - API Keys table
  - Audit Logs table
- [ ] Set up PostgreSQL for control plane (RDS or self-managed)
- [ ] Implement Drizzle ORM migrations

#### Month 2: Authentication & Core API

**Week 1-2: Authentication System**
- [ ] Implement user registration/login (email + password)
- [ ] bcrypt password hashing (cost factor 12)
- [ ] JWT token generation (RS256, 1-hour expiry)
- [ ] Refresh token rotation
- [ ] Rate limiting for auth endpoints (5 attempts / 15 min)

**Week 3-4: Core API Endpoints**
- [ ] POST /v1/organizations - Create organization
- [ ] POST /v1/projects - Create project
- [ ] POST /v1/clusters - Create cluster (dedicated only)
- [ ] GET /v1/clusters/:id - Get cluster details
- [ ] DELETE /v1/clusters/:id - Delete cluster
- [ ] GET /v1/clusters/:id/connection-string

#### Month 3: Provisioning & Dashboard

**Week 1-2: Cluster Provisioning**
- [ ] Build provisioner service (Go)
- [ ] Integrate CloudNativePG operator
- [ ] Build PostgreSQL + Orochi Docker image
- [ ] Implement cluster creation workflow:
  1. Create namespace
  2. Deploy PgBouncer ConfigMap
  3. Deploy CNPG Cluster resource
  4. Wait for readiness
  5. Install Orochi extension
  6. Return connection info

**Week 3-4: Web Dashboard MVP**
- [ ] Set up Next.js 15 with App Router
- [ ] Implement authentication UI (login, signup)
- [ ] Dashboard layout with navigation
- [ ] Cluster list view with status indicators
- [ ] Cluster create modal (region, size selection)
- [ ] Cluster detail page (connection string, metrics placeholder)

#### Month 4: Billing & Polish

**Week 1-2: Basic Billing**
- [ ] Integrate Stripe for payments
- [ ] Implement plan tiers (Free, Pro)
- [ ] Usage metering (CU-hours, storage GB-hours)
- [ ] Billing dashboard in UI
- [ ] Upgrade/downgrade flows

**Week 3-4: MVP Polish**
- [ ] SSL/TLS for all connections
- [ ] Basic monitoring (CloudWatch/Prometheus)
- [ ] Email notifications (cluster ready, issues)
- [ ] Documentation site setup
- [ ] Landing page
- [ ] Beta launch preparation

**MVP Deliverables:**
- Single-region deployment (us-east-1 or European equivalent)
- Dedicated clusters only (small, medium, large sizes)
- Email/password authentication
- Basic web dashboard
- Connection string display
- Stripe billing integration
- Daily automated backups

---

### Phase 2: Essential Features (Months 5-8)

**Goal:** Add serverless compute, monitoring, backup/restore, and team management

#### Month 5: Serverless Foundation

**Week 1-2: Connection Router (PgCat)**
- [ ] Deploy PgCat as connection pooler/router
- [ ] Configure multi-tenant routing via SNI
- [ ] Set up connection pooling (transaction mode)
- [ ] Implement wake-on-connect hook for scale-to-zero
- [ ] Configure read/write splitting for replicas
- [ ] Enable Prometheus metrics endpoint

**Week 3-4: Scale-to-Zero**
- [ ] Implement idle detection (5 min no connections)
- [ ] Cluster suspension workflow
- [ ] Wake-up workflow (target: <5s cold start)

#### Month 6: Auto-Scaling & Metrics

**Week 1-2: Metrics Collection**
- [ ] Deploy VictoriaMetrics for time-series storage
- [ ] PostgreSQL metrics exporter
- [ ] PgBouncer metrics collection
- [ ] Kubernetes metrics (CPU, memory)
- [ ] Custom metrics API for UI

**Week 3-4: Auto-Scaling Controller**
- [ ] Build autoscaler service (Go)
- [ ] Vertical scaling decision engine
- [ ] Zero-downtime vertical scaling
- [ ] Cooldown management
- [ ] Budget limit enforcement

#### Month 7: Backup & Restore

**Week 1-2: Automated Backups**
- [ ] WAL-G integration for continuous archiving
- [ ] Daily base backup scheduling
- [ ] S3 bucket per organization (encrypted)
- [ ] Backup retention policy

**Week 3-4: Point-in-Time Recovery**
- [ ] PITR restore API endpoint
- [ ] Restore workflow
- [ ] Backup list in UI
- [ ] One-click restore functionality

#### Month 8: Team Management & Multi-Region Prep

**Week 1-2: Team Management**
- [ ] Team CRUD operations
- [ ] Role-based access control implementation
- [ ] Invitation system (email invites)
- [ ] Team management UI

**Week 3-4: CLI & Second Region**
- [ ] Build orochi-cli (Go)
- [ ] Deploy second region (eu-west-1)
- [ ] Region selector in UI
- [ ] Cross-region backup replication prep

**Phase 2 Deliverables:**
- Serverless compute with scale-to-zero
- Auto-scaling (vertical)
- Second region (EU)
- PgCat connection pooling/routing
- PITR backup/restore
- Team management with RBAC
- CLI tool
- Customer-facing metrics dashboard

---

### Phase 3: Advanced Features (Months 9-12)

**Goal:** Enterprise features, compliance, third region, advanced security

#### Month 9: SSO & Advanced Auth
- [ ] SAML 2.0 provider support
- [ ] OIDC provider support
- [ ] Google/GitHub OAuth
- [ ] MFA support (TOTP, WebAuthn)
- [ ] IP allowlisting per organization

#### Month 10: Private Networking & Audit Logs
- [ ] VPC peering support (AWS, GCP)
- [ ] AWS PrivateLink integration
- [ ] Comprehensive audit logging
- [ ] Audit log UI with search/filter

#### Month 11: Enterprise Security & Branching
- [ ] Customer-managed encryption keys (BYOK)
- [ ] Database branching (Neon-style)
- [ ] Branch lifecycle management

#### Month 12: Compliance & Third Region
- [ ] SOC 2 Type II preparation
- [ ] Deploy third region (Asia-Pacific)
- [ ] Global load balancer setup
- [ ] GA launch preparation

---

### Phase 4: Enterprise & Scale (Months 13-18)

- [ ] Horizontal scaling (read replicas)
- [ ] Predictive scaling
- [ ] Built-in SQL editor
- [ ] Multi-cloud deployment (GCP, Azure)
- [ ] Terraform provider
- [ ] HIPAA compliance
- [ ] Advanced query insights

---

## 3. Technology Decisions

### Backend Services

| Service | Language | Framework | Rationale |
|---------|----------|-----------|-----------|
| **API Gateway** | - | Kong | Open-source, PostgreSQL-backed, plugins |
| **Control Plane API** | Go | Chi/Echo | Performance, K8s ecosystem, gRPC support |
| **Provisioner** | Go | Kubebuilder | Native K8s operator framework |
| **Autoscaler** | Go | Custom | Tight K8s integration, metrics access |
| **Connection Router** | Rust | **PgCat** | Multi-tenant pooling, sharding, load balancing |
| **Billing Service** | Go | Custom | Stripe SDK, reliability |

### Frontend

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Dashboard** | Next.js 15 (App Router) | Server components, edge functions |
| **UI Library** | shadcn/ui + Tailwind | Modern, accessible, customizable |
| **State** | TanStack Query | Server state caching, optimistic updates |
| **Charts** | Recharts | Responsive, React-native |
| **SQL Editor** | Monaco Editor | VS Code quality, syntax highlighting |

### Infrastructure

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Kubernetes** | EKS (AWS) / k3s (Hetzner) | Managed K8s reduces ops burden |
| **PostgreSQL Operator** | CloudNativePG | Production-ready, well-maintained |
| **Service Mesh** | Linkerd | Lightweight mTLS, observability |
| **Secrets** | HashiCorp Vault | Industry standard, K8s integration |
| **IaC** | Terraform | Multi-cloud, state management |
| **GitOps** | ArgoCD | K8s-native, declarative |

### Databases & Storage

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Control Plane DB** | PostgreSQL (RDS) | Reliability, familiarity |
| **Time-Series Metrics** | VictoriaMetrics | High-cardinality, long retention |
| **Message Queue** | NATS JetStream | Lightweight, persistent, multi-tenant |
| **Object Storage** | S3 / GCS | Backups, tiered storage |
| **Cache** | Redis (ElastiCache) | Session store, rate limiting |

### Observability

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Metrics** | Prometheus + Grafana | Industry standard |
| **Logs** | Loki | Grafana ecosystem |
| **Traces** | Tempo | Grafana ecosystem |
| **Alerting** | Alertmanager + PagerDuty | On-call integration |

---

## 4. MVP Feature List (Prioritized)

### Must-Have (Launch Blockers)

1. **User Authentication**
   - Email/password registration and login
   - JWT-based sessions
   - Password reset via email

2. **Organization Management**
   - Create organization on signup
   - Basic organization settings

3. **Cluster Provisioning**
   - Create dedicated PostgreSQL + Orochi cluster
   - Instance sizes: small (1 vCPU, 2GB), medium (2 vCPU, 4GB), large (4 vCPU, 8GB)
   - Single region selection

4. **Cluster Lifecycle**
   - View cluster status and details
   - Display connection string securely
   - Delete cluster with confirmation

5. **Basic Dashboard**
   - List all clusters with status
   - Cluster detail view
   - Navigation and responsive layout

6. **Billing Integration**
   - Stripe checkout for plan selection
   - Usage display (even if metering is basic)
   - Upgrade/downgrade plans

7. **SSL/TLS Security**
   - All API endpoints over HTTPS
   - Database connections over TLS

8. **Automated Backups**
   - Daily backups to S3
   - 7-day retention

### Should-Have (Post-Launch Priorities)

1. Serverless compute tier
2. Auto-scaling (vertical)
3. Team invitations
4. Basic RBAC
5. CLI tool (basic commands)
6. Metrics dashboard (CPU, memory, connections)

---

## 5. Cost-Optimized Infrastructure Options

### Option A: Hetzner + k3s (Lowest Cost)

**Estimated Monthly Cost: ~$200-400/month for MVP**

```hcl
# Hetzner Cloud Servers
# Control Plane (3x CPX31): 3 × €15.59 = €46.77/month
# Data Plane (3x CPX51): 3 × €30.59 = €91.77/month
# Total Compute: ~€140/month (~$150)

# Storage
# Hetzner Volumes: ~$50/month
# Hetzner Object Storage (S3-compatible): ~$10/month

# Networking
# Load Balancer: ~$5/month
```

**Setup:**
```bash
# 1. Provision servers via Terraform
cd infra/terraform/environments/dev

# 2. Install k3s cluster
curl -sfL https://get.k3s.io | sh -s - server --cluster-init

# 3. Install CloudNativePG operator
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.22/releases/cnpg-1.22.0.yaml

# 4. Deploy services
kubectl apply -k infra/k8s/overlays/dev
```

### Option B: Fly.io (Simpler Operations)

**Estimated Monthly Cost: ~$300-500/month for MVP**

```bash
# Control Plane
fly postgres create --name orochi-control-db --region ord  # ~$30/month

# API Service
fly launch --name orochi-api --region ord  # ~$20/month

# Web Dashboard
fly launch --name orochi-web --region ord  # ~$10/month

# Data Plane (Fly Machines for PostgreSQL)
# Variable based on usage
```

### Option C: AWS (Production-Grade)

**Estimated Monthly Cost: ~$800-1500/month for MVP**

- EKS cluster: ~$150/month
- RDS for control plane: ~$50/month
- EC2 instances (data plane): ~$300-500/month
- S3, networking, monitoring: ~$100-200/month

---

## 6. Pricing Model

### Recommended Tiers

| Tier | Price | Included | Overage |
|------|-------|----------|---------|
| **Free** | $0 | 1 DB, 1 GB storage, 10 compute hrs | N/A (paused) |
| **Pro** | $29/mo | Unlimited DBs, 50 GB, 1000 hrs | $0.10/hr, $0.25/GB |
| **Team** | $99/mo | Pro + Teams, 200 GB, 3000 hrs | $0.08/hr, $0.20/GB |
| **Enterprise** | Custom | Dedicated, VPC, SSO, SLA | Custom |

### Unit Economics

- **Compute Unit (CU)**: 1 vCPU + 2GB RAM for 1 hour
- **Storage**: Per GB-month, includes backups
- **Data Transfer**: Free inbound, $0.05/GB outbound

---

## 7. Key Implementation Patterns

### Cluster Provisioning Workflow

```go
func (s *ProvisionerService) CreateCluster(ctx context.Context, req CreateClusterRequest) (*Cluster, error) {
    // 1. Validate request
    if err := s.validator.Validate(req); err != nil {
        return nil, err
    }

    // 2. Check quota
    if err := s.quotaService.CheckQuota(ctx, req.OrgID, req); err != nil {
        return nil, err
    }

    // 3. Create cluster record
    cluster := &Cluster{
        ID:        uuid.New(),
        OrgID:     req.OrgID,
        Name:      req.Name,
        Region:    req.Region,
        Size:      req.Size,
        Status:    "provisioning",
    }
    if err := s.db.CreateCluster(ctx, cluster); err != nil {
        return nil, err
    }

    // 4. Trigger async provisioning
    if err := s.queue.Publish(ctx, "cluster.provision", cluster.ID); err != nil {
        return nil, err
    }

    return cluster, nil
}

func (s *ProvisionerService) provisionCluster(ctx context.Context, clusterID uuid.UUID) error {
    cluster, err := s.db.GetCluster(ctx, clusterID)
    if err != nil {
        return err
    }

    // 1. Create Kubernetes namespace
    ns := &corev1.Namespace{
        ObjectMeta: metav1.ObjectMeta{
            Name: fmt.Sprintf("tenant-%s", cluster.ID),
            Labels: map[string]string{
                "orochi.cloud/tenant": cluster.OrgID.String(),
            },
        },
    }
    if err := s.k8s.Create(ctx, ns); err != nil {
        return err
    }

    // 2. Deploy CNPG Cluster
    cnpgCluster := s.buildCNPGCluster(cluster)
    if err := s.k8s.Create(ctx, cnpgCluster); err != nil {
        return err
    }

    // 3. Wait for readiness
    if err := s.waitForClusterReady(ctx, cluster.ID); err != nil {
        return err
    }

    // 4. Install Orochi extension
    if err := s.installOrochiExtension(ctx, cluster); err != nil {
        return err
    }

    // 5. Update status
    cluster.Status = "running"
    cluster.Endpoint = fmt.Sprintf("%s.orochi.cloud", cluster.ID)
    return s.db.UpdateCluster(ctx, cluster)
}
```

---

## 8. Getting Started

### Prerequisites

```bash
# Install required tools
brew install go node pnpm terraform kubectl helm

# Clone repository
git clone https://github.com/orochi/orochi-cloud.git
cd orochi-cloud

# Install dependencies
pnpm install

# Set up environment
cp .env.example .env
```

### Local Development

```bash
# Start local infrastructure (Docker Compose)
docker-compose up -d

# Run database migrations
pnpm db:migrate

# Start development servers
pnpm dev

# Access:
# - Dashboard: http://localhost:3000
# - API: http://localhost:8080
# - Docs: http://localhost:4321
```

### Deploy to Staging

```bash
# Configure cloud credentials
export HCLOUD_TOKEN=xxx  # or AWS credentials

# Deploy infrastructure
cd infra/terraform/environments/staging
terraform init
terraform apply

# Deploy services
kubectl apply -k infra/k8s/overlays/staging
```

---

## Related Documents

- [Cloud Architecture](./OROCHI_CLOUD_ARCHITECTURE.md)
- [Auto-Scaling Design](./AUTOSCALING-DESIGN.md)
- [Multi-Tenancy Architecture](./security/MULTI-TENANCY-ARCHITECTURE.md)
- [Security Implementation Guide](./security/SECURITY-IMPLEMENTATION-GUIDE.md)
- [UI/Dashboard Design](./UI-DASHBOARD-DESIGN.md)
