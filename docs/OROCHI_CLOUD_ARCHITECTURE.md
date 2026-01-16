# Orochi Cloud Architecture

## A Managed Database Service Platform for Orochi DB

**Version**: 1.0
**Last Updated**: January 2026
**Status**: Architecture Design Document

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [System Overview](#system-overview)
3. [Architecture Principles](#architecture-principles)
4. [Component Architecture](#component-architecture)
5. [Data Flow Diagrams](#data-flow-diagrams)
6. [Technology Stack](#technology-stack)
7. [Multi-Tenant Architecture](#multi-tenant-architecture)
8. [Security Architecture](#security-architecture)
9. [Deployment Topology](#deployment-topology)
10. [Cost Optimization Strategy](#cost-optimization-strategy)
11. [API and CLI Design](#api-and-cli-design)
12. [Monitoring and Observability](#monitoring-and-observability)
13. [Disaster Recovery](#disaster-recovery)
14. [Implementation Roadmap](#implementation-roadmap)

---

## 1. Executive Summary

Orochi Cloud is a fully managed database service platform that provides Orochi DB (PostgreSQL HTAP extension) as a cloud service. Inspired by industry leaders like TiDB Cloud, Neon, and Tigris, it offers:

- **Serverless compute** with scale-to-zero capability
- **Instant provisioning** of database clusters
- **Automatic scaling** based on workload
- **Usage-based billing** with transparent pricing
- **Enterprise security** with tenant isolation
- **Full PostgreSQL compatibility** with HTAP capabilities

### Target Personas

| Persona | Needs | Orochi Cloud Value |
|---------|-------|-------------------|
| **Startup Developer** | Low cost, fast setup, scale as needed | Serverless tier, pay-per-query |
| **Data Engineer** | Analytics on transactional data | Native HTAP, columnar storage |
| **Platform Team** | Multi-team database management | Organizations, RBAC, quotas |
| **Enterprise Architect** | Compliance, SLAs, support | Dedicated clusters, SOC2, SLAs |

---

## 2. System Overview

### High-Level Architecture Diagram

```
+-----------------------------------------------------------------------------------+
|                                 OROCHI CLOUD                                       |
+-----------------------------------------------------------------------------------+
|                                                                                    |
|  +------------------------+    +-------------------------+    +-----------------+ |
|  |     Web Dashboard      |    |       Public API        |    |      CLI        | |
|  |    (React + Next.js)   |    |   (REST + GraphQL)      |    |   (orochi-cli)  | |
|  +------------------------+    +-------------------------+    +-----------------+ |
|            |                            |                            |            |
|            +----------------------------+----------------------------+            |
|                                         |                                         |
|  +----------------------------------------------------------------------+        |
|  |                        API Gateway (Kong)                             |        |
|  |  - Rate Limiting  - Auth  - Request Routing  - Load Balancing         |        |
|  +----------------------------------------------------------------------+        |
|            |                    |                    |                            |
|  +---------+----------+   +----+-----+   +----------+---------+                  |
|  |   Control Plane    |   |  Billing |   |   Identity (IAM)   |                  |
|  |    Services        |   |  Service |   |      Service       |                  |
|  +---------+----------+   +----+-----+   +----------+---------+                  |
|            |                    |                    |                            |
|  +----------------------------------------------------------------------+        |
|  |                     Message Bus (NATS JetStream)                      |        |
|  +----------------------------------------------------------------------+        |
|            |                    |                    |                            |
|  +---------+----------+   +----+-----+   +----------+---------+                  |
|  |  Cluster Operator  |   | Metrics  |   |    Backup/PITR     |                  |
|  |    (Kubernetes)    |   | Collector|   |      Service       |                  |
|  +---------+----------+   +----+-----+   +----------+---------+                  |
|            |                    |                    |                            |
+------------+--------------------+--------------------+----------------------------+
             |                    |                    |
+------------+--------------------+--------------------+----------------------------+
|                              DATA PLANE                                           |
|                                                                                    |
|  Region: us-east-1              Region: eu-west-1              Region: ap-south-1 |
|  +------------------+           +------------------+           +------------------+|
|  | +-------------+  |           | +-------------+  |           | +-------------+  ||
|  | |  Tenant A   |  |           | |  Tenant C   |  |           | |  Tenant E   |  ||
|  | | Serverless  |  |           | | Dedicated   |  |           | | Serverless  |  ||
|  | +-------------+  |           | +-------------+  |           | +-------------+  ||
|  | +-------------+  |           | +-------------+  |           | +-------------+  ||
|  | |  Tenant B   |  |           | |  Tenant D   |  |           | |  Tenant F   |  ||
|  | | Dedicated   |  |           | | Serverless  |  |           | | Dedicated   |  ||
|  | +-------------+  |           | +-------------+  |           | +-------------+  ||
|  +------------------+           +------------------+           +------------------+|
|          |                              |                              |           |
|  +-------+-----+                +-------+-----+                +-------+-----+     |
|  | Shared S3   |                | Shared S3   |                | Shared S3   |     |
|  | (Tiered     |                | (Tiered     |                | (Tiered     |     |
|  |  Storage)   |                |  Storage)   |                |  Storage)   |     |
|  +-------------+                +-------------+                +-------------+     |
+-----------------------------------------------------------------------------------+
```

### Core Concepts

| Concept | Description |
|---------|-------------|
| **Organization** | Top-level account entity (company/team) |
| **Project** | Logical grouping of databases within an org |
| **Cluster** | A single Orochi DB deployment (serverless or dedicated) |
| **Branch** | Copy-on-write database clone (like Neon branches) |
| **Endpoint** | Connection endpoint for a cluster/branch |

---

## 3. Architecture Principles

### 3.1 Design Principles

1. **Serverless-First**: Default to serverless compute with scale-to-zero
2. **PostgreSQL Native**: Full wire protocol compatibility, no proprietary lock-in
3. **Cost Transparency**: Clear, predictable billing with no hidden costs
4. **Security by Default**: Zero-trust, encrypted by default, isolated tenants
5. **Operations Minimal**: Self-healing, auto-scaling, auto-maintenance
6. **Developer Experience**: Fast provisioning, instant branching, great tooling

### 3.2 Non-Functional Requirements

| Requirement | Target | Rationale |
|-------------|--------|-----------|
| **Availability** | 99.95% (Serverless), 99.99% (Dedicated) | Industry standard for managed DBaaS |
| **Latency** | < 10ms p95 within region | PostgreSQL-compatible performance |
| **Provisioning** | < 30 seconds (Serverless), < 5 min (Dedicated) | Competitive with Neon/PlanetScale |
| **Scale-to-Zero** | < 5 seconds cold start | Match serverless competitors |
| **Recovery Point** | < 1 second (PITR granularity) | Enterprise data protection |

---

## 4. Component Architecture

### 4.1 Control Plane Services

```yaml
control_plane:
  api_gateway:
    name: "API Gateway"
    technology: "Kong on Kubernetes"
    responsibilities:
      - Request routing and load balancing
      - Rate limiting per organization/API key
      - JWT/API key authentication
      - Request/response transformation
      - WAF and DDoS protection

  cluster_manager:
    name: "Cluster Manager Service"
    technology: "Go + gRPC"
    responsibilities:
      - Cluster lifecycle management (create, update, delete)
      - Scaling decisions and execution
      - Health monitoring and self-healing
      - Configuration management
    interfaces:
      - POST /v1/clusters
      - GET /v1/clusters/{id}
      - PATCH /v1/clusters/{id}
      - DELETE /v1/clusters/{id}
      - POST /v1/clusters/{id}/scale
      - POST /v1/clusters/{id}/branches

  identity_service:
    name: "Identity & Access Management"
    technology: "Go + OIDC"
    responsibilities:
      - User authentication (email, SSO, OAuth)
      - Organization and team management
      - Role-based access control (RBAC)
      - API key management
      - Audit logging
    external_integrations:
      - Auth0/Okta (SSO)
      - Google/GitHub (OAuth)

  billing_service:
    name: "Billing & Metering"
    technology: "Go + Stripe"
    responsibilities:
      - Usage metering (compute, storage, I/O)
      - Invoice generation
      - Payment processing
      - Quota enforcement
      - Cost alerts and budgets
    metrics_collected:
      - Compute units (CU-hours)
      - Storage (GB-hours)
      - Data transfer (GB)
      - Backup storage (GB-hours)
      - PITR retention (days)

  backup_service:
    name: "Backup & PITR Service"
    technology: "Go + WAL-G"
    responsibilities:
      - Continuous WAL archiving
      - Base backup scheduling
      - Point-in-time recovery execution
      - Cross-region backup replication
      - Backup retention management
```

### 4.2 Data Plane Components

```yaml
data_plane:
  compute_layer:
    serverless_pool:
      name: "Serverless Compute Pool"
      technology: "Kubernetes + Custom Scheduler"
      architecture:
        - Shared PostgreSQL process pools
        - Per-tenant connection routing via PgBouncer
        - Scale-to-zero with 5-second cold start
        - Neon-style compute/storage separation
      scaling:
        min_cu: 0.25  # Minimum compute units
        max_cu: 8     # Maximum for serverless
        scale_up_threshold: "CPU > 70% for 30s"
        scale_down_threshold: "No queries for 5 min"
        scale_to_zero_threshold: "No connections for 10 min"

    dedicated_clusters:
      name: "Dedicated Compute"
      technology: "Kubernetes StatefulSets"
      architecture:
        - Isolated PostgreSQL instances per tenant
        - Custom resource sizes (1-128 vCPU)
        - Optional read replicas
        - High availability with automatic failover
      instance_types:
        - name: "dev"
          vcpu: 1
          memory: "2GB"
          price_per_hour: 0.05
        - name: "small"
          vcpu: 2
          memory: "8GB"
          price_per_hour: 0.15
        - name: "medium"
          vcpu: 4
          memory: "16GB"
          price_per_hour: 0.30
        - name: "large"
          vcpu: 8
          memory: "32GB"
          price_per_hour: 0.60
        - name: "xlarge"
          vcpu: 16
          memory: "64GB"
          price_per_hour: 1.20

  storage_layer:
    primary_storage:
      name: "Hot Storage"
      technology: "EBS gp3 / Local NVMe"
      purpose: "Active data and recent WAL"
      replication: "Synchronous within AZ"

    tiered_storage:
      name: "Tiered Object Storage"
      technology: "S3 + Orochi Tiering"
      tiers:
        - name: "warm"
          storage_class: "S3 Standard"
          data_age: "> 7 days"
        - name: "cold"
          storage_class: "S3 Intelligent-Tiering"
          data_age: "> 30 days"
        - name: "archive"
          storage_class: "S3 Glacier Instant Retrieval"
          data_age: "> 90 days"

    backup_storage:
      name: "Backup Storage"
      technology: "S3 + WAL-G"
      retention:
        continuous_pitr: "7 days (default), up to 35 days"
        daily_snapshots: "30 days"
        monthly_snapshots: "12 months"

  network_layer:
    connection_pooler:
      name: "PgCat Connection Router"
      technology: "PgCat (Rust)"
      features:
        - Transaction-mode pooling
        - SSL termination
        - Connection multiplexing
        - Query routing (read/write split)

    load_balancer:
      name: "Database Load Balancer"
      technology: "AWS NLB / GCP TCP LB"
      features:
        - TCP passthrough for PostgreSQL
        - Health checking
        - Cross-AZ distribution
```

### 4.3 Kubernetes Operator Architecture

```yaml
orochi_operator:
  name: "Orochi Cloud Operator"
  technology: "Go + Kubebuilder"

  custom_resources:
    - kind: OrochiCluster
      spec:
        tier: "serverless | dedicated"
        compute:
          class: "dev | small | medium | large | xlarge"
          replicas: 1-3
          autoscaling:
            enabled: true
            minCU: 0.25
            maxCU: 8
        storage:
          size: "10Gi - 16Ti"
          class: "gp3 | io2"
          tiering:
            enabled: true
            warmAfter: "7d"
            coldAfter: "30d"
        extensions:
          - orochi
          - pgvector
          - postgis
        networking:
          publicAccess: false
          allowedCIDRs: []
          privateLink: false
      status:
        phase: "Pending | Provisioning | Running | Scaling | Failed"
        endpoint: "xxx.orochi.cloud"
        connectionString: "postgresql://..."

    - kind: OrochiBranch
      spec:
        parentCluster: "cluster-id"
        snapshot: "latest | timestamp | lsn"
      status:
        phase: "Creating | Ready | Deleting"
        endpoint: "branch-xxx.orochi.cloud"

    - kind: OrochiBackup
      spec:
        cluster: "cluster-id"
        type: "snapshot | pitr"
        retention: "30d"
      status:
        phase: "InProgress | Completed | Failed"
        size: "10GB"
        location: "s3://..."

  controllers:
    cluster_controller:
      reconcile_interval: "30s"
      actions:
        - provision_compute
        - configure_storage
        - setup_networking
        - install_extensions
        - configure_monitoring
        - health_checks

    autoscaler_controller:
      reconcile_interval: "10s"
      metrics_source: "Prometheus"
      actions:
        - collect_metrics
        - evaluate_scaling_rules
        - execute_scaling
        - update_billing_meters

    backup_controller:
      reconcile_interval: "1m"
      actions:
        - schedule_backups
        - archive_wal
        - verify_backups
        - cleanup_expired
```

---

## 5. Data Flow Diagrams

### 5.1 Cluster Provisioning Flow

```
User                   API Gateway          Cluster Manager       K8s Operator         Infrastructure
  |                        |                      |                     |                    |
  |  POST /clusters        |                      |                     |                    |
  |----------------------->|                      |                     |                    |
  |                        | Validate + Auth      |                     |                    |
  |                        |--------------------->|                     |                    |
  |                        |                      | Create OrochiCluster CR                  |
  |                        |                      |-------------------->|                    |
  |                        |                      |                     | Provision EBS      |
  |                        |                      |                     |------------------->|
  |                        |                      |                     |<-------------------|
  |                        |                      |                     | Create StatefulSet |
  |                        |                      |                     |------------------->|
  |                        |                      |                     |<-------------------|
  |                        |                      |                     | Configure Network  |
  |                        |                      |                     |------------------->|
  |                        |                      |                     |<-------------------|
  |                        |                      |                     | Install Orochi Ext |
  |                        |                      |                     |------------------->|
  |                        |                      |                     |<-------------------|
  |                        |                      |<--------------------|                    |
  |                        |                      | Status: Ready       |                    |
  |                        |<---------------------|                     |                    |
  | 201 Created            |                      |                     |                    |
  |<-----------------------|                      |                     |                    |
  |                        |                      |                     |                    |
  | Webhook: cluster.ready |                      |                     |                    |
  |<-----------------------|                      |                     |                    |
```

### 5.2 Query Execution Flow (Serverless)

```
Application          PgBouncer Pool       Compute Router        Serverless Pool       Storage
    |                     |                     |                      |                  |
    | PostgreSQL Connect  |                     |                      |                  |
    |-------------------->|                     |                      |                  |
    |                     | Route by tenant_id  |                      |                  |
    |                     |-------------------->|                      |                  |
    |                     |                     | Check warm instance  |                  |
    |                     |                     |--------------------->|                  |
    |                     |                     |                      |                  |
    |                     |                     | [If cold] Wake up    |                  |
    |                     |                     |--------------------->|                  |
    |                     |                     |                      | Load state       |
    |                     |                     |                      |----------------->|
    |                     |                     |                      |<-----------------|
    |                     |                     |<---------------------|                  |
    |                     |<--------------------|                      |                  |
    | Connection Ready    |                     |                      |                  |
    |<--------------------|                     |                      |                  |
    |                     |                     |                      |                  |
    | SELECT query        |                     |                      |                  |
    |-------------------->|                     |                      |                  |
    |                     |-------------------->|                      |                  |
    |                     |                     |--------------------->|                  |
    |                     |                     |                      | Execute query    |
    |                     |                     |                      |----------------->|
    |                     |                     |                      |<-----------------|
    |                     |                     |<---------------------|                  |
    |                     |<--------------------|                      |                  |
    | Query Result        |                     |                      |                  |
    |<--------------------|                     |                      |                  |
    |                     |                     |                      |                  |
    |                     | [Idle 10 min]       |                      |                  |
    |                     |                     | Scale to zero        |                  |
    |                     |                     |--------------------->|                  |
    |                     |                     |                      | Checkpoint + sleep|
    |                     |                     |                      |----------------->|
```

### 5.3 Billing Metering Flow

```
Compute Instance     Metrics Exporter      Prometheus         Billing Service        Stripe
       |                    |                   |                    |                  |
       | CPU/Memory/IO      |                   |                    |                  |
       |    metrics         |                   |                    |                  |
       |------------------->|                   |                    |                  |
       |                    | Scrape metrics    |                    |                  |
       |                    |<------------------|                    |                  |
       |                    |------------------>|                    |                  |
       |                    |                   |                    |                  |
       |                    |                   | Query aggregates   |                  |
       |                    |                   |<-------------------|                  |
       |                    |                   |------------------->|                  |
       |                    |                   |                    |                  |
       |                    |                   |                    | Calculate CU-hours|
       |                    |                   |                    |                  |
       |                    |                   |                    | [Hourly]         |
       |                    |                   |                    | Update usage     |
       |                    |                   |                    |----------------->|
       |                    |                   |                    |                  |
       |                    |                   |                    | [Monthly]        |
       |                    |                   |                    | Generate invoice |
       |                    |                   |                    |----------------->|
       |                    |                   |                    |<-----------------|
       |                    |                   |                    | Charge customer  |
       |                    |                   |                    |----------------->|
```

---

## 6. Technology Stack

### 6.1 Infrastructure Layer

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Cloud Provider** | AWS (primary), GCP, Azure | Multi-cloud for compliance, cost optimization |
| **Kubernetes** | EKS / GKE / AKS | Managed K8s reduces operational burden |
| **Container Runtime** | containerd | Industry standard, stable |
| **Service Mesh** | Linkerd (lightweight) | mTLS, observability without Istio overhead |
| **Secrets Management** | HashiCorp Vault | Industry standard, K8s integration |

### 6.2 Control Plane Stack

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **API Gateway** | Kong | Open-source, PostgreSQL backend, plugins |
| **Backend Services** | Go 1.22+ | Performance, concurrency, cloud-native ecosystem |
| **Service Communication** | gRPC + Protocol Buffers | Type safety, performance, code generation |
| **Message Queue** | NATS JetStream | Lightweight, persistent, multi-tenant |
| **API Documentation** | OpenAPI 3.1 | Industry standard, code generation |

### 6.3 Data Plane Stack

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Database Engine** | PostgreSQL 17 + Orochi | Latest PG with HTAP capabilities |
| **Connection Pooler** | PgCat | Rust-based, multi-tenant, sharding support, load balancing |
| **Backup Tool** | WAL-G | S3-native, encryption, compression |
| **Monitoring Agent** | Vector + Prometheus | Logs and metrics collection |

### 6.4 Frontend Stack

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **Web Framework** | Next.js 15 (App Router) | Server components, edge functions |
| **UI Components** | Shadcn/ui + Tailwind | Modern, accessible, customizable |
| **State Management** | TanStack Query | Server state, caching, optimistic updates |
| **Charts** | Recharts | Responsive, React-native |
| **Code Editor** | Monaco Editor | VS Code quality SQL editing |

### 6.5 DevOps & Tooling

| Component | Technology | Rationale |
|-----------|------------|-----------|
| **CI/CD** | GitHub Actions | Native GitHub integration |
| **Infrastructure as Code** | Terraform + Pulumi | AWS/GCP support, state management |
| **GitOps** | ArgoCD | K8s-native, declarative |
| **Observability** | Grafana Stack | LGTM (Loki, Grafana, Tempo, Mimir) |

---

## 7. Multi-Tenant Architecture

### 7.1 Isolation Model

```
+------------------------------------------------------------------+
|                     MULTI-TENANCY LAYERS                          |
+------------------------------------------------------------------+
|                                                                    |
|  Layer 1: Organization Isolation                                   |
|  +---------------------------+  +---------------------------+      |
|  |    Organization A         |  |    Organization B         |      |
|  |  - Separate billing       |  |  - Separate billing       |      |
|  |  - Own IAM policies       |  |  - Own IAM policies       |      |
|  |  - Audit isolation        |  |  - Audit isolation        |      |
|  +---------------------------+  +---------------------------+      |
|                                                                    |
|  Layer 2: Project Isolation                                        |
|  +-------------+  +-------------+  +-------------+                 |
|  | Project A1  |  | Project A2  |  | Project B1  |                 |
|  | (prod)      |  | (staging)   |  | (prod)      |                 |
|  +-------------+  +-------------+  +-------------+                 |
|                                                                    |
|  Layer 3: Cluster Isolation                                        |
|  +---------+  +---------+  +---------+  +---------+               |
|  |Cluster 1|  |Cluster 2|  |Cluster 3|  |Cluster 4|               |
|  |Dedicated|  |Serverless| |Dedicated|  |Serverless|              |
|  +---------+  +---------+  +---------+  +---------+               |
|       |            |            |            |                     |
|       |            |            |            |                     |
|  Layer 4: Network Isolation                                        |
|  +--------+  +--------+  +--------+  +--------+                   |
|  | VPC A1 |  |Shared   |  | VPC B1 |  |Shared   |                 |
|  |        |  |Pool VPC |  |        |  |Pool VPC |                 |
|  +--------+  +--------+  +--------+  +--------+                   |
|                                                                    |
+------------------------------------------------------------------+
```

### 7.2 Serverless Multi-Tenancy

For cost efficiency, serverless clusters share compute infrastructure while maintaining isolation:

```yaml
serverless_isolation:
  compute_isolation:
    method: "Process-level + Linux namespaces"
    details:
      - Each tenant gets separate PostgreSQL process
      - Linux namespaces for PID, network, mount isolation
      - cgroups for CPU/memory limits
      - Seccomp profiles for syscall filtering

  data_isolation:
    method: "Separate data directories + encryption"
    details:
      - Each tenant has isolated data directory
      - Separate encryption keys per tenant (AWS KMS)
      - File-level encryption at rest
      - TLS encryption in transit

  network_isolation:
    method: "Network policies + mTLS"
    details:
      - Kubernetes NetworkPolicy per tenant
      - Service mesh mTLS (Linkerd)
      - Unique connection credentials per cluster

  resource_isolation:
    method: "Kubernetes resource quotas"
    limits:
      cpu: "Enforced via cgroups"
      memory: "OOM killer with tenant priority"
      storage: "Per-PVC limits"
      connections: "PgBouncer max_client_conn per tenant"
```

### 7.3 Dedicated Multi-Tenancy

For higher isolation requirements:

```yaml
dedicated_isolation:
  compute_isolation:
    method: "Separate Kubernetes namespace + node pools"
    options:
      - Shared node pool (default): Network policies between namespaces
      - Dedicated node pool: Taints/tolerations for tenant-only nodes
      - Dedicated cluster: Separate EKS/GKE cluster

  data_isolation:
    method: "Separate EBS volumes + KMS keys"
    details:
      - Customer-managed encryption keys (BYOK)
      - Dedicated S3 buckets for backups (optional)

  network_isolation:
    method: "VPC peering or PrivateLink"
    options:
      - Shared VPC with security groups
      - VPC peering to customer VPC
      - AWS PrivateLink / GCP Private Service Connect
      - Direct Connect / Interconnect
```

---

## 8. Security Architecture

### 8.1 Security Layers

```
+------------------------------------------------------------------+
|                      SECURITY ARCHITECTURE                        |
+------------------------------------------------------------------+
|                                                                    |
|  +------------------------+                                        |
|  |   Perimeter Security   |                                        |
|  |  - AWS WAF / Shield    |                                        |
|  |  - DDoS protection     |                                        |
|  |  - Geo-blocking        |                                        |
|  +------------------------+                                        |
|             |                                                      |
|  +------------------------+                                        |
|  |   API Security         |                                        |
|  |  - OAuth 2.0 / OIDC    |                                        |
|  |  - API key rotation    |                                        |
|  |  - Rate limiting       |                                        |
|  |  - Request validation  |                                        |
|  +------------------------+                                        |
|             |                                                      |
|  +------------------------+                                        |
|  |   Network Security     |                                        |
|  |  - VPC isolation       |                                        |
|  |  - Security groups     |                                        |
|  |  - mTLS (Linkerd)      |                                        |
|  |  - Network policies    |                                        |
|  +------------------------+                                        |
|             |                                                      |
|  +------------------------+                                        |
|  |   Data Security        |                                        |
|  |  - Encryption at rest  |                                        |
|  |  - TLS 1.3 in transit  |                                        |
|  |  - Key management      |                                        |
|  |  - Backup encryption   |                                        |
|  +------------------------+                                        |
|             |                                                      |
|  +------------------------+                                        |
|  |   Access Control       |                                        |
|  |  - RBAC (org/project)  |                                        |
|  |  - DB-level GRANT      |                                        |
|  |  - Row-level security  |                                        |
|  |  - Audit logging       |                                        |
|  +------------------------+                                        |
|                                                                    |
+------------------------------------------------------------------+
```

### 8.2 Authentication & Authorization

```yaml
authentication:
  user_authentication:
    methods:
      - email_password:
          password_policy: "12+ chars, complexity requirements"
          mfa: "TOTP, WebAuthn (optional)"
      - sso:
          protocols: ["SAML 2.0", "OIDC"]
          providers: ["Okta", "Auth0", "Azure AD", "Google Workspace"]
      - oauth:
          providers: ["GitHub", "Google", "GitLab"]

  api_authentication:
    methods:
      - api_key:
          format: "orochi_sk_live_xxxxxxxxxxxx"
          rotation: "90 days recommended"
          scopes: ["read", "write", "admin"]
      - jwt:
          issuer: "https://auth.orochi.cloud"
          algorithm: "RS256"
          expiry: "1 hour"
      - service_account:
          for: "CI/CD, automation"
          format: "JSON key file"

authorization:
  rbac_model:
    organization_roles:
      - owner: "Full access, billing, delete org"
      - admin: "Manage members, projects, clusters"
      - member: "Access assigned projects"
      - billing: "View/manage billing only"

    project_roles:
      - admin: "Full project access"
      - developer: "Create/manage clusters"
      - viewer: "Read-only access"

    cluster_roles:
      - admin: "Full cluster access"
      - write: "Query, DML, DDL"
      - read: "Query only"

  database_security:
    postgres_roles:
      - admin: "SUPERUSER equivalent"
      - readwrite: "SELECT, INSERT, UPDATE, DELETE"
      - readonly: "SELECT only"
    row_level_security: "Supported via PostgreSQL RLS"
```

### 8.3 Compliance & Certifications

| Certification | Status | Timeline |
|---------------|--------|----------|
| **SOC 2 Type II** | Target | Month 12 |
| **GDPR** | Built-in | Launch |
| **HIPAA** | Dedicated tier | Month 18 |
| **PCI DSS** | Assessment | Month 24 |
| **ISO 27001** | Target | Month 24 |

---

## 9. Deployment Topology

### 9.1 Regional Architecture

```
                        Global
                          |
        +-----------------+-----------------+
        |                 |                 |
   US-EAST-1         EU-WEST-1        AP-SOUTH-1
        |                 |                 |
   +----+----+       +----+----+       +----+----+
   |         |       |         |       |         |
  AZ-A     AZ-B     AZ-A     AZ-B     AZ-A     AZ-B
   |         |       |         |       |         |
+--+--+   +--+--+ +--+--+   +--+--+ +--+--+   +--+--+
|     |   |     | |     |   |     | |     |   |     |
|Ctrl |   |Ctrl | |Ctrl |   |Ctrl | |Ctrl |   |Ctrl |
|Plane|   |Plane| |Plane|   |Plane| |Plane|   |Plane|
|     |   |     | |     |   |     | |     |   |     |
+--+--+   +--+--+ +--+--+   +--+--+ +--+--+   +--+--+
   |         |       |         |       |         |
+--+--+   +--+--+ +--+--+   +--+--+ +--+--+   +--+--+
|     |   |     | |     |   |     | |     |   |     |
|Data |   |Data | |Data |   |Data | |Data |   |Data |
|Plane|   |Plane| |Plane|   |Plane| |Plane|   |Plane|
|     |   |     | |     |   |     | |     |   |     |
+-----+   +-----+ +-----+   +-----+ +-----+   +-----+
```

### 9.2 Kubernetes Cluster Layout

```yaml
kubernetes_clusters:
  control_plane_cluster:
    name: "orochi-control-{region}"
    node_pools:
      - name: "control-plane"
        instance_type: "m6i.xlarge"
        min_nodes: 3
        max_nodes: 10
        labels:
          role: control-plane
        workloads:
          - api-gateway
          - cluster-manager
          - billing-service
          - identity-service

      - name: "observability"
        instance_type: "m6i.2xlarge"
        min_nodes: 2
        max_nodes: 5
        labels:
          role: observability
        workloads:
          - prometheus
          - grafana
          - loki
          - tempo

  data_plane_cluster:
    name: "orochi-data-{region}"
    node_pools:
      - name: "serverless-pool"
        instance_type: "r6i.2xlarge"
        min_nodes: 3
        max_nodes: 100
        labels:
          role: serverless
          tier: compute
        taints:
          - key: "dedicated"
            value: "serverless"
            effect: "NoSchedule"
        spot_instances: true  # Cost optimization
        spot_percentage: 70

      - name: "dedicated-small"
        instance_type: "r6i.xlarge"
        min_nodes: 0
        max_nodes: 50
        labels:
          role: dedicated
          size: small
        cluster_autoscaler: true

      - name: "dedicated-large"
        instance_type: "r6i.4xlarge"
        min_nodes: 0
        max_nodes: 20
        labels:
          role: dedicated
          size: large
        cluster_autoscaler: true
```

### 9.3 Global Services

```yaml
global_services:
  dns:
    provider: "Route 53 / Cloud DNS"
    domains:
      - "orochi.cloud" (main)
      - "*.db.orochi.cloud" (cluster endpoints)
      - "api.orochi.cloud" (API)
    features:
      - Latency-based routing
      - Health checks
      - Failover

  cdn:
    provider: "CloudFront / Cloud CDN"
    usage:
      - Static dashboard assets
      - API documentation
    caching:
      - Cache static assets: 1 year
      - Cache API docs: 1 day

  global_load_balancer:
    provider: "AWS Global Accelerator"
    purpose: "Anycast IP for consistent latency"
    backends:
      - us-east-1: weight 100
      - eu-west-1: weight 100
      - ap-south-1: weight 100
```

---

## 10. Cost Optimization Strategy

### 10.1 Pricing Model

```yaml
pricing_tiers:
  free_tier:
    name: "Free"
    limits:
      compute: "5 CU-hours/month"
      storage: "1 GB"
      branches: 1
      projects: 1
    features:
      - Serverless only
      - Community support
      - 7-day backup retention
    cost: "$0"

  pro_tier:
    name: "Pro"
    limits:
      compute: "Pay as you go"
      storage: "Pay as you go"
      branches: 10
      projects: 5
    features:
      - Serverless and dedicated
      - Email support
      - 30-day backup retention
      - Read replicas
    pricing:
      compute_cu_hour: "$0.10"
      storage_gb_month: "$0.15"
      data_transfer_gb: "$0.09"

  enterprise_tier:
    name: "Enterprise"
    limits:
      compute: "Unlimited"
      storage: "Unlimited"
      branches: "Unlimited"
      projects: "Unlimited"
    features:
      - Dedicated clusters
      - 24/7 support with SLA
      - HIPAA compliance
      - SSO/SAML
      - Audit logs
      - Private networking
    pricing: "Custom"
```

### 10.2 Infrastructure Cost Optimization

```yaml
cost_optimization:
  compute:
    strategies:
      - name: "Spot instances for serverless"
        savings: "60-70%"
        implementation: "Use spot for stateless compute pods"
        risk_mitigation: "Graceful shutdown, checkpointing"

      - name: "Scale-to-zero"
        savings: "Variable (idle time)"
        implementation: "Hibernate serverless clusters after 10 min"

      - name: "Right-sizing"
        savings: "20-40%"
        implementation: "Recommendations based on usage patterns"

      - name: "Reserved capacity"
        savings: "30-50%"
        implementation: "Reserved instances for baseline load"

  storage:
    strategies:
      - name: "Tiered storage"
        savings: "50-80% on cold data"
        implementation: "Orochi automatic tiering to S3"

      - name: "Compression"
        savings: "50-90%"
        implementation: "Orochi columnar compression"

      - name: "Intelligent tiering"
        savings: "Variable"
        implementation: "S3 Intelligent-Tiering for backups"

  network:
    strategies:
      - name: "Regional data processing"
        savings: "Avoid cross-region transfer"
        implementation: "Process data in same region as storage"

      - name: "Compression"
        savings: "60-80%"
        implementation: "Compress backup transfers"
```

### 10.3 Estimated Unit Economics

```
+------------------------------------------------------------------+
|                    UNIT ECONOMICS MODEL                           |
+------------------------------------------------------------------+
|                                                                    |
|  Cost per Serverless Cluster (idle):                              |
|  - Compute: $0 (scale-to-zero)                                    |
|  - Storage: $0.15/GB/month                                        |
|  - Backup: $0.03/GB/month                                         |
|  - Overhead: ~$1/month (monitoring, metadata)                     |
|                                                                    |
|  Cost per Serverless Cluster (active, 1 CU):                      |
|  - Compute: ~$0.02/hour (spot r6i.large)                          |
|  - Storage: $0.15/GB/month                                        |
|  - I/O: ~$0.01/1000 requests                                      |
|                                                                    |
|  Margin target:                                                    |
|  - Serverless: 60-70% gross margin                                |
|  - Dedicated: 40-50% gross margin                                 |
|                                                                    |
|  Break-even analysis:                                              |
|  - Fixed costs: ~$50K/month (team, infra baseline)                |
|  - Variable costs: ~$0.04/CU-hour (compute + overhead)            |
|  - Price: $0.10/CU-hour                                           |
|  - Break-even: ~830K CU-hours/month (~$83K revenue)               |
|                                                                    |
+------------------------------------------------------------------+
```

---

## 11. API and CLI Design

### 11.1 REST API Design

```yaml
api_design:
  base_url: "https://api.orochi.cloud/v1"

  authentication:
    header: "Authorization: Bearer <api_key>"
    api_key_format: "orochi_sk_live_xxxxxxxxxxxxxxxx"

  endpoints:
    organizations:
      - GET    /organizations
      - POST   /organizations
      - GET    /organizations/{org_id}
      - PATCH  /organizations/{org_id}
      - DELETE /organizations/{org_id}
      - GET    /organizations/{org_id}/members
      - POST   /organizations/{org_id}/members

    projects:
      - GET    /projects
      - POST   /projects
      - GET    /projects/{project_id}
      - PATCH  /projects/{project_id}
      - DELETE /projects/{project_id}

    clusters:
      - GET    /clusters
      - POST   /clusters
      - GET    /clusters/{cluster_id}
      - PATCH  /clusters/{cluster_id}
      - DELETE /clusters/{cluster_id}
      - POST   /clusters/{cluster_id}/start
      - POST   /clusters/{cluster_id}/stop
      - POST   /clusters/{cluster_id}/scale
      - GET    /clusters/{cluster_id}/metrics
      - GET    /clusters/{cluster_id}/logs

    branches:
      - GET    /clusters/{cluster_id}/branches
      - POST   /clusters/{cluster_id}/branches
      - GET    /branches/{branch_id}
      - DELETE /branches/{branch_id}
      - POST   /branches/{branch_id}/reset

    backups:
      - GET    /clusters/{cluster_id}/backups
      - POST   /clusters/{cluster_id}/backups
      - GET    /backups/{backup_id}
      - POST   /backups/{backup_id}/restore
      - DELETE /backups/{backup_id}

    connections:
      - GET    /clusters/{cluster_id}/connection-string
      - POST   /clusters/{cluster_id}/password/rotate

  example_requests:
    create_cluster: |
      POST /v1/clusters
      {
        "name": "production-db",
        "project_id": "proj_xxxxx",
        "region": "us-east-1",
        "tier": "serverless",
        "settings": {
          "compute": {
            "min_cu": 0.25,
            "max_cu": 4,
            "auto_scaling": true
          },
          "storage": {
            "size_gb": 10,
            "tiering_enabled": true
          },
          "extensions": ["orochi", "pgvector"],
          "postgresql_version": "17"
        }
      }

    response: |
      {
        "id": "cluster_xxxxx",
        "name": "production-db",
        "status": "provisioning",
        "endpoint": "cluster-xxxxx.us-east-1.db.orochi.cloud",
        "created_at": "2026-01-15T10:00:00Z"
      }
```

### 11.2 CLI Design

```bash
# orochi-cli - Command Line Interface for Orochi Cloud

# Installation
curl -fsSL https://orochi.cloud/install.sh | sh
# or
brew install orochi-cli
# or
npm install -g @orochi/cli

# Authentication
orochi auth login                    # Browser-based OAuth
orochi auth login --api-key          # API key authentication
orochi auth status                   # Show current auth status
orochi auth logout

# Organization management
orochi org list
orochi org create "My Company"
orochi org switch my-company

# Project management
orochi project list
orochi project create production
orochi project switch production

# Cluster management
orochi cluster list
orochi cluster create my-db \
  --tier serverless \
  --region us-east-1 \
  --compute-min 0.25 \
  --compute-max 4 \
  --storage 10

orochi cluster status my-db
orochi cluster scale my-db --compute-max 8
orochi cluster start my-db
orochi cluster stop my-db
orochi cluster delete my-db

# Connection
orochi connect my-db                 # Opens psql session
orochi connect my-db --url           # Print connection URL
orochi connect my-db --env           # Print as env vars

# Branching (Neon-style)
orochi branch list my-db
orochi branch create my-db feature-123
orochi branch delete my-db feature-123
orochi branch reset my-db feature-123 --to main

# Backups
orochi backup list my-db
orochi backup create my-db --name "pre-migration"
orochi backup restore my-db --backup backup_xxxxx
orochi backup restore my-db --point-in-time "2026-01-15T10:00:00Z"

# Monitoring
orochi metrics my-db                 # Show current metrics
orochi logs my-db                    # Stream logs
orochi logs my-db --query "ERROR"    # Filter logs

# SQL execution
orochi sql my-db "SELECT version()"
orochi sql my-db -f schema.sql
cat data.csv | orochi sql my-db "COPY table FROM STDIN CSV"

# Configuration
orochi config set default-region us-east-1
orochi config set output json        # json, table, yaml
orochi config list
```

### 11.3 SDK Support

```yaml
sdks:
  official:
    - language: "TypeScript/JavaScript"
      package: "@orochi/sdk"
      features:
        - Full API coverage
        - TypeScript types
        - Connection pooling helper
        - Serverless adapter (Vercel, Cloudflare)

    - language: "Python"
      package: "orochi-sdk"
      features:
        - Full API coverage
        - Async support
        - SQLAlchemy integration
        - Pandas DataFrame support

    - language: "Go"
      package: "github.com/orochi-cloud/orochi-go"
      features:
        - Full API coverage
        - Context support
        - pgx integration

  community:
    - language: "Ruby"
    - language: "Java"
    - language: "Rust"
    - language: ".NET"
```

---

## 12. Monitoring and Observability

### 12.1 Observability Stack

```
+------------------------------------------------------------------+
|                     OBSERVABILITY ARCHITECTURE                    |
+------------------------------------------------------------------+
|                                                                    |
|  +-----------------+  +-----------------+  +-----------------+    |
|  |    Metrics      |  |     Logs        |  |    Traces       |    |
|  |   (Prometheus)  |  |    (Loki)       |  |    (Tempo)      |    |
|  +-----------------+  +-----------------+  +-----------------+    |
|         |                    |                    |               |
|         +--------------------+--------------------+               |
|                              |                                    |
|                    +---------+---------+                          |
|                    |      Grafana      |                          |
|                    |   (Dashboards)    |                          |
|                    +-------------------+                          |
|                              |                                    |
|         +--------------------+--------------------+               |
|         |                    |                    |               |
|  +-----------------+  +-----------------+  +-----------------+    |
|  |   Alertmanager  |  |   PagerDuty     |  |     Slack       |    |
|  +-----------------+  +-----------------+  +-----------------+    |
|                                                                    |
+------------------------------------------------------------------+
```

### 12.2 Key Metrics

```yaml
platform_metrics:
  cluster_health:
    - orochi_cluster_status{cluster_id, status}
    - orochi_cluster_uptime_seconds{cluster_id}
    - orochi_cluster_connection_count{cluster_id}
    - orochi_cluster_connection_pool_available{cluster_id}

  compute_metrics:
    - orochi_compute_units_used{cluster_id}
    - orochi_cpu_utilization_percent{cluster_id}
    - orochi_memory_utilization_percent{cluster_id}
    - orochi_cold_start_duration_seconds{cluster_id}

  storage_metrics:
    - orochi_storage_used_bytes{cluster_id, tier}
    - orochi_storage_iops{cluster_id, type}
    - orochi_backup_size_bytes{cluster_id}
    - orochi_wal_size_bytes{cluster_id}

  query_metrics:
    - orochi_queries_total{cluster_id, status}
    - orochi_query_duration_seconds{cluster_id, quantile}
    - orochi_rows_returned_total{cluster_id}
    - orochi_rows_affected_total{cluster_id}

  billing_metrics:
    - orochi_compute_units_consumed{org_id, cluster_id}
    - orochi_storage_gb_hours{org_id, cluster_id}
    - orochi_data_transfer_bytes{org_id, direction}

customer_visible_metrics:
  dashboard:
    - CPU utilization
    - Memory utilization
    - Storage used
    - Active connections
    - Queries per second
    - Query latency (p50, p95, p99)
    - Data transfer
    - Compute units consumed

  alerts:
    - High CPU utilization (> 80% for 5 min)
    - High memory utilization (> 90%)
    - Storage approaching limit (> 80%)
    - Connection pool exhaustion (> 90%)
    - High query latency (p99 > 1s)
    - Replication lag (> 10s)
```

### 12.3 Alerting Rules

```yaml
alerting:
  platform_alerts:
    - name: ClusterDown
      severity: critical
      condition: "orochi_cluster_status != 1 for 1m"
      action: "Page on-call, auto-remediate"

    - name: HighErrorRate
      severity: warning
      condition: "rate(orochi_errors_total[5m]) > 0.01"
      action: "Notify Slack"

    - name: StorageNearLimit
      severity: warning
      condition: "orochi_storage_used_bytes / orochi_storage_limit_bytes > 0.9"
      action: "Notify customer, suggest upgrade"

  customer_alerts:
    - name: HighCPU
      threshold: "cpu_percent > 80 for 5m"
      default: enabled
      channels: ["email", "webhook"]

    - name: ConnectionPoolExhausted
      threshold: "connections / max_connections > 0.9"
      default: enabled
      channels: ["email", "webhook"]
```

---

## 13. Disaster Recovery

### 13.1 Backup Strategy

```yaml
backup_strategy:
  continuous_wal_archiving:
    frequency: "Continuous"
    retention: "7-35 days"
    storage: "S3 (same region)"
    encryption: "AES-256 (customer KMS optional)"

  base_backups:
    frequency: "Daily"
    retention: "30 days"
    storage: "S3 (same region)"
    method: "pg_basebackup via WAL-G"

  cross_region_replication:
    frequency: "Daily"
    destination: "Different region"
    retention: "30 days"
    use_case: "Regional disaster recovery"
```

### 13.2 Recovery Procedures

```yaml
recovery_scenarios:
  instance_failure:
    rto: "< 2 minutes"
    rpo: "0"
    method: "Automatic failover to standby"
    automation: "Kubernetes liveness probe + operator"

  availability_zone_failure:
    rto: "< 5 minutes"
    rpo: "0"
    method: "Failover to different AZ"
    automation: "Multi-AZ deployment, automatic DNS update"

  region_failure:
    rto: "< 1 hour"
    rpo: "< 5 minutes"
    method: "Restore from cross-region backup"
    automation: "Manual trigger, automated restore"

  data_corruption:
    rto: "< 30 minutes"
    rpo: "Point-in-time (1 second granularity)"
    method: "PITR restore to new cluster"
    automation: "Self-service via dashboard/API"

  accidental_deletion:
    rto: "< 15 minutes"
    rpo: "0 (soft delete)"
    method: "Restore from soft-deleted state"
    retention: "7 days for deleted clusters"
```

### 13.3 High Availability Configuration

```yaml
high_availability:
  serverless:
    replicas: "Stateless, auto-scaled"
    storage: "EBS Multi-AZ"
    failover: "Automatic (< 30 seconds)"

  dedicated_standard:
    replicas: 2
    synchronous_commit: "off"
    failover: "Automatic (< 60 seconds)"
    read_replicas: 0

  dedicated_ha:
    replicas: 3
    synchronous_commit: "remote_apply"
    failover: "Automatic (< 30 seconds)"
    read_replicas: "0-5"
    quorum: "2 of 3"
```

---

## 14. Implementation Roadmap

### 14.1 Phase 1: MVP (Months 1-4)

```yaml
phase_1_mvp:
  goal: "Launch basic managed Orochi DB service"

  deliverables:
    control_plane:
      - Basic API (create, read, delete clusters)
      - Simple web dashboard
      - Email/password authentication
      - Basic billing (Stripe integration)

    data_plane:
      - Single-region deployment (us-east-1)
      - Dedicated clusters only (no serverless)
      - Basic instance sizes (small, medium, large)
      - Automated backups (daily)
      - SSL/TLS encryption

    operations:
      - Basic monitoring (CloudWatch)
      - Manual scaling
      - Email support

  team:
    - 2 backend engineers
    - 1 frontend engineer
    - 1 DevOps/SRE
    - 1 product manager

  estimated_cost: "$30-50K/month"
```

### 14.2 Phase 2: Growth (Months 5-8)

```yaml
phase_2_growth:
  goal: "Add serverless, improve DX, expand regions"

  deliverables:
    control_plane:
      - Full RBAC (organizations, projects)
      - OAuth (GitHub, Google)
      - CLI tool (orochi-cli)
      - API v1 stable release
      - Usage-based billing

    data_plane:
      - Serverless compute (scale-to-zero)
      - Second region (eu-west-1)
      - Connection pooling (PgCat)
      - PITR (point-in-time recovery)
      - Auto-scaling (dedicated)

    operations:
      - Prometheus + Grafana
      - Alerting (PagerDuty)
      - Customer-facing metrics dashboard

  team: "+2 engineers, +1 SRE"
  estimated_cost: "$80-120K/month"
```

### 14.3 Phase 3: Enterprise (Months 9-12)

```yaml
phase_3_enterprise:
  goal: "Enterprise features, compliance, third region"

  deliverables:
    control_plane:
      - SSO/SAML integration
      - Audit logging
      - API rate limiting tiers
      - Terraform provider
      - SDKs (Python, TypeScript, Go)

    data_plane:
      - Third region (ap-south-1)
      - Private networking (VPC peering, PrivateLink)
      - Customer-managed encryption keys (BYOK)
      - Read replicas
      - Branching (Neon-style)

    compliance:
      - SOC 2 Type II audit
      - GDPR compliance
      - Security penetration testing

    operations:
      - 24/7 on-call rotation
      - SLA commitments (99.95%+)
      - Dedicated support tier

  team: "+3 engineers, +2 SRE, +1 security"
  estimated_cost: "$200-300K/month"
```

### 14.4 Phase 4: Scale (Months 13-18)

```yaml
phase_4_scale:
  goal: "Global scale, advanced features"

  deliverables:
    - Global database (cross-region replication)
    - Multi-cloud (GCP, Azure)
    - Advanced analytics (query insights)
    - HIPAA compliance
    - Marketplace integrations (Vercel, Supabase alternatives)
    - Partner program

  metrics_targets:
    - 1000+ active organizations
    - 10,000+ clusters
    - 99.99% availability
    - $1M+ ARR
```

---

## 15. Appendix

### A. Glossary

| Term | Definition |
|------|------------|
| **CU (Compute Unit)** | Billing unit for compute resources (1 CU = 1 vCPU + 4GB RAM) |
| **Cluster** | A single Orochi DB database instance |
| **Branch** | A copy-on-write clone of a cluster for development/testing |
| **Endpoint** | Network address for connecting to a cluster |
| **HTAP** | Hybrid Transactional/Analytical Processing |
| **PITR** | Point-in-Time Recovery |

### B. Competitive Comparison

| Feature | Orochi Cloud | Neon | PlanetScale | TiDB Cloud |
|---------|--------------|------|-------------|------------|
| PostgreSQL Compatible | Native | Native | No (MySQL) | No (MySQL) |
| Scale-to-Zero | Yes | Yes | Yes | No |
| Branching | Yes | Yes | Yes | No |
| HTAP | Native | No | No | Yes |
| Columnar Storage | Native | No | No | TiFlash |
| Time-Series | Native | Extension | No | No |
| Vector Search | Native | Extension | No | No |
| Pricing | $0.10/CU-hr | $0.10/CU-hr | $0.07/row-hr | $0.12/CU-hr |

### C. API Rate Limits

| Tier | Requests/min | Burst |
|------|--------------|-------|
| Free | 60 | 100 |
| Pro | 600 | 1000 |
| Enterprise | 6000 | 10000 |

### D. Instance Size Reference

| Size | vCPU | Memory | Max Connections | Price/hr |
|------|------|--------|-----------------|----------|
| dev | 0.25 | 1 GB | 25 | $0.02 |
| small | 1 | 4 GB | 100 | $0.10 |
| medium | 2 | 8 GB | 200 | $0.20 |
| large | 4 | 16 GB | 400 | $0.40 |
| xlarge | 8 | 32 GB | 800 | $0.80 |
| 2xlarge | 16 | 64 GB | 1500 | $1.60 |
| 4xlarge | 32 | 128 GB | 3000 | $3.20 |

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-16 | Architecture Team | Initial architecture design |

---

*This document serves as the foundational architecture for Orochi Cloud. It should be reviewed and updated quarterly as the platform evolves.*
