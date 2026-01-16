# Orochi DB - Google Cloud Platform Deployment Guide

This guide provides production-ready deployment instructions for Orochi DB on Google Cloud Platform (GCP). It covers Cloud SQL, Compute Engine, GKE, AlloyDB, and Cloud Storage integration.

**Version**: 1.0.0
**Last Updated**: January 2026
**Supported PostgreSQL**: 16, 17, 18

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Cloud SQL PostgreSQL](#2-cloud-sql-postgresql)
3. [Compute Engine Deployment](#3-compute-engine-deployment)
4. [GKE Deployment](#4-gke-deployment)
5. [Cloud Storage Integration](#5-cloud-storage-integration)
6. [AlloyDB Configuration](#6-alloydb-configuration)
7. [Terraform Templates](#7-terraform-templates)
8. [Monitoring and Observability](#8-monitoring-and-observability)
9. [Security Best Practices](#9-security-best-practices)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Prerequisites

### 1.1 Google Cloud CLI Setup

```bash
# Install gcloud CLI (Linux/macOS)
curl https://sdk.cloud.google.com | bash
exec -l $SHELL

# Initialize and authenticate
gcloud init
gcloud auth login
gcloud auth application-default login

# Set default project and region
export PROJECT_ID="your-project-id"
export REGION="us-central1"
export ZONE="us-central1-a"

gcloud config set project $PROJECT_ID
gcloud config set compute/region $REGION
gcloud config set compute/zone $ZONE

# Verify configuration
gcloud config list
```

### 1.2 Enable Required APIs

```bash
# Enable all required GCP APIs
gcloud services enable \
    compute.googleapis.com \
    sqladmin.googleapis.com \
    container.googleapis.com \
    storage.googleapis.com \
    monitoring.googleapis.com \
    logging.googleapis.com \
    cloudresourcemanager.googleapis.com \
    iam.googleapis.com \
    servicenetworking.googleapis.com \
    alloydb.googleapis.com \
    artifactregistry.googleapis.com \
    secretmanager.googleapis.com
```

### 1.3 Required IAM Roles

Create a service account for Orochi DB operations:

```bash
# Create service account
export SA_NAME="orochi-db-sa"
gcloud iam service-accounts create $SA_NAME \
    --display-name="Orochi DB Service Account" \
    --description="Service account for Orochi DB operations"

# Get the service account email
export SA_EMAIL="${SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"

# Assign required roles
ROLES=(
    "roles/cloudsql.admin"
    "roles/cloudsql.client"
    "roles/compute.instanceAdmin.v1"
    "roles/storage.admin"
    "roles/container.admin"
    "roles/monitoring.editor"
    "roles/logging.logWriter"
    "roles/secretmanager.secretAccessor"
    "roles/alloydb.admin"
    "roles/iam.serviceAccountUser"
)

for ROLE in "${ROLES[@]}"; do
    gcloud projects add-iam-policy-binding $PROJECT_ID \
        --member="serviceAccount:$SA_EMAIL" \
        --role="$ROLE"
done

# Create and download service account key (for local development)
gcloud iam service-accounts keys create ~/orochi-sa-key.json \
    --iam-account=$SA_EMAIL

# Set credentials environment variable
export GOOGLE_APPLICATION_CREDENTIALS=~/orochi-sa-key.json
```

### 1.4 VPC and Firewall Configuration

```bash
# Create a dedicated VPC for Orochi DB
export VPC_NAME="orochi-vpc"
export SUBNET_NAME="orochi-subnet"

gcloud compute networks create $VPC_NAME \
    --subnet-mode=custom \
    --bgp-routing-mode=regional

# Create subnet with private Google access
gcloud compute networks subnets create $SUBNET_NAME \
    --network=$VPC_NAME \
    --region=$REGION \
    --range=10.0.0.0/20 \
    --enable-private-ip-google-access

# Create secondary ranges for GKE (pods and services)
gcloud compute networks subnets update $SUBNET_NAME \
    --region=$REGION \
    --add-secondary-ranges=pods=10.4.0.0/14,services=10.8.0.0/20

# Configure firewall rules
# Allow internal communication
gcloud compute firewall-rules create orochi-allow-internal \
    --network=$VPC_NAME \
    --allow=tcp,udp,icmp \
    --source-ranges=10.0.0.0/8

# Allow PostgreSQL from specific IPs (replace with your CIDR)
gcloud compute firewall-rules create orochi-allow-postgres \
    --network=$VPC_NAME \
    --allow=tcp:5432 \
    --source-ranges=YOUR_OFFICE_CIDR/32 \
    --target-tags=orochi-db

# Allow health checks from GCP load balancers
gcloud compute firewall-rules create orochi-allow-health-checks \
    --network=$VPC_NAME \
    --allow=tcp:5432 \
    --source-ranges=130.211.0.0/22,35.191.0.0/16 \
    --target-tags=orochi-db

# Allow SSH via IAP (Identity-Aware Proxy)
gcloud compute firewall-rules create orochi-allow-iap-ssh \
    --network=$VPC_NAME \
    --allow=tcp:22 \
    --source-ranges=35.235.240.0/20 \
    --target-tags=orochi-db

# Configure Private Services Access for Cloud SQL
gcloud compute addresses create orochi-private-ip-range \
    --global \
    --purpose=VPC_PEERING \
    --prefix-length=16 \
    --network=$VPC_NAME

gcloud services vpc-peerings connect \
    --service=servicenetworking.googleapis.com \
    --ranges=orochi-private-ip-range \
    --network=$VPC_NAME
```

### 1.5 Cloud NAT for Private Instances

```bash
# Create Cloud Router
gcloud compute routers create orochi-router \
    --network=$VPC_NAME \
    --region=$REGION

# Create Cloud NAT
gcloud compute routers nats create orochi-nat \
    --router=orochi-router \
    --region=$REGION \
    --nat-all-subnet-ip-ranges \
    --auto-allocate-nat-external-ips
```

---

## 2. Cloud SQL PostgreSQL

Cloud SQL provides managed PostgreSQL with automatic backups, replication, and high availability.

### 2.1 Create Cloud SQL Instance

```bash
export INSTANCE_NAME="orochi-primary"
export DB_VERSION="POSTGRES_17"  # Options: POSTGRES_16, POSTGRES_17, POSTGRES_18
export MACHINE_TYPE="db-custom-8-32768"  # 8 vCPUs, 32GB RAM
export DISK_SIZE="500"  # GB

# Create primary instance with high availability
gcloud sql instances create $INSTANCE_NAME \
    --database-version=$DB_VERSION \
    --tier=$MACHINE_TYPE \
    --region=$REGION \
    --availability-type=REGIONAL \
    --storage-type=SSD \
    --storage-size=$DISK_SIZE \
    --storage-auto-increase \
    --storage-auto-increase-limit=2000 \
    --backup-start-time=02:00 \
    --enable-bin-log \
    --maintenance-window-day=SUN \
    --maintenance-window-hour=03 \
    --network=$VPC_NAME \
    --no-assign-ip \
    --root-password="$(openssl rand -base64 32)" \
    --insights-config-query-insights-enabled \
    --insights-config-query-plans-per-minute=5 \
    --insights-config-query-string-length=4500 \
    --insights-config-record-application-tags \
    --insights-config-record-client-address \
    --deletion-protection

# Store root password in Secret Manager
gcloud secrets create orochi-db-root-password \
    --replication-policy="automatic"

# Note: Save the root password securely before proceeding
```

### 2.2 Database Flags for Extensions

Configure PostgreSQL flags required for Orochi DB:

```bash
# Set database flags for Orochi DB
gcloud sql instances patch $INSTANCE_NAME \
    --database-flags=\
shared_preload_libraries=pg_stat_statements,\
max_connections=500,\
shared_buffers=8GB,\
effective_cache_size=24GB,\
maintenance_work_mem=2GB,\
checkpoint_completion_target=0.9,\
wal_buffers=64MB,\
default_statistics_target=100,\
random_page_cost=1.1,\
effective_io_concurrency=200,\
work_mem=64MB,\
huge_pages=try,\
min_wal_size=2GB,\
max_wal_size=8GB,\
max_worker_processes=16,\
max_parallel_workers_per_gather=4,\
max_parallel_workers=8,\
max_parallel_maintenance_workers=4,\
log_min_duration_statement=1000,\
log_checkpoints=on,\
log_connections=on,\
log_disconnections=on,\
log_lock_waits=on,\
log_temp_files=0,\
idle_in_transaction_session_timeout=300000,\
statement_timeout=3600000

# Wait for instance restart
gcloud sql instances describe $INSTANCE_NAME --format="value(state)"
```

### 2.3 Create Database and Users

```bash
# Create orochi database
gcloud sql databases create orochi \
    --instance=$INSTANCE_NAME \
    --charset=UTF8 \
    --collation=en_US.UTF8

# Create application user
gcloud sql users create orochi_app \
    --instance=$INSTANCE_NAME \
    --password="$(openssl rand -base64 24)"

# Create read-only user for analytics
gcloud sql users create orochi_readonly \
    --instance=$INSTANCE_NAME \
    --password="$(openssl rand -base64 24)"

# Store credentials in Secret Manager
gcloud secrets create orochi-app-credentials \
    --replication-policy="automatic"

gcloud secrets versions add orochi-app-credentials \
    --data-file=- <<EOF
{
  "username": "orochi_app",
  "password": "YOUR_GENERATED_PASSWORD",
  "database": "orochi",
  "instance": "$INSTANCE_NAME"
}
EOF
```

### 2.4 High Availability Configuration

The `--availability-type=REGIONAL` flag enables automatic failover. Additional HA considerations:

```bash
# Verify HA status
gcloud sql instances describe $INSTANCE_NAME \
    --format="table(name,state,settings.availabilityType,failoverReplica.name)"

# Configure failover replica (automatic with REGIONAL)
# Manual failover for testing:
gcloud sql instances failover $INSTANCE_NAME --async

# Monitor replication lag
gcloud sql instances describe $INSTANCE_NAME \
    --format="value(replicaConfiguration.failoverTarget)"
```

### 2.5 Read Replicas

```bash
# Create read replica in same region
gcloud sql instances create orochi-replica-1 \
    --master-instance-name=$INSTANCE_NAME \
    --tier=db-custom-4-16384 \
    --region=$REGION \
    --availability-type=ZONAL \
    --storage-type=SSD \
    --storage-size=500 \
    --no-assign-ip \
    --network=$VPC_NAME

# Create cross-region read replica for disaster recovery
gcloud sql instances create orochi-replica-dr \
    --master-instance-name=$INSTANCE_NAME \
    --tier=db-custom-4-16384 \
    --region=us-east1 \
    --availability-type=ZONAL \
    --storage-type=SSD \
    --storage-size=500 \
    --no-assign-ip \
    --network=$VPC_NAME

# List all replicas
gcloud sql instances list --filter="masterInstanceName:$INSTANCE_NAME"
```

### 2.6 Connect to Cloud SQL

```bash
# Get private IP address
export PRIVATE_IP=$(gcloud sql instances describe $INSTANCE_NAME \
    --format="value(ipAddresses[0].ipAddress)")

# Connect via Cloud SQL Auth Proxy (recommended)
# Download proxy
curl -o cloud-sql-proxy \
    https://storage.googleapis.com/cloud-sql-connectors/cloud-sql-proxy/v2.8.1/cloud-sql-proxy.linux.amd64
chmod +x cloud-sql-proxy

# Start proxy
./cloud-sql-proxy \
    --private-ip \
    --credentials-file=$GOOGLE_APPLICATION_CREDENTIALS \
    "${PROJECT_ID}:${REGION}:${INSTANCE_NAME}" &

# Connect via psql
PGPASSWORD='your_password' psql \
    -h 127.0.0.1 \
    -U orochi_app \
    -d orochi

# Install Orochi extension (if custom extensions enabled)
# Note: Cloud SQL has limited extension support
# For full Orochi functionality, use Compute Engine or GKE
```

---

## 3. Compute Engine Deployment

For full Orochi DB functionality including custom extensions, deploy on Compute Engine.

### 3.1 Machine Type Selection for HTAP

| Workload Type | Recommended Machine | vCPUs | Memory | Use Case |
|---------------|---------------------|-------|--------|----------|
| Development | n2-standard-4 | 4 | 16 GB | Testing, development |
| Production Small | n2-highmem-8 | 8 | 64 GB | Small HTAP workloads |
| Production Medium | n2-highmem-16 | 16 | 128 GB | Medium HTAP workloads |
| Production Large | n2-highmem-32 | 32 | 256 GB | Large HTAP workloads |
| Compute-Intensive | c2-standard-16 | 16 | 64 GB | Heavy analytical queries |
| Memory-Intensive | m2-ultramem-208 | 208 | 5.75 TB | Large in-memory workloads |

### 3.2 Create Compute Instance

```bash
export INSTANCE_NAME="orochi-db-primary"
export MACHINE_TYPE="n2-highmem-16"
export BOOT_DISK_SIZE="100"
export DATA_DISK_SIZE="1000"
export IMAGE_FAMILY="ubuntu-2404-lts"
export IMAGE_PROJECT="ubuntu-os-cloud"

# Create boot disk with Ubuntu 24.04 LTS
gcloud compute instances create $INSTANCE_NAME \
    --zone=$ZONE \
    --machine-type=$MACHINE_TYPE \
    --network=$VPC_NAME \
    --subnet=$SUBNET_NAME \
    --no-address \
    --tags=orochi-db \
    --service-account=$SA_EMAIL \
    --scopes=cloud-platform \
    --image-family=$IMAGE_FAMILY \
    --image-project=$IMAGE_PROJECT \
    --boot-disk-size=${BOOT_DISK_SIZE}GB \
    --boot-disk-type=pd-ssd \
    --metadata=enable-oslogin=TRUE \
    --shielded-secure-boot \
    --shielded-vtpm \
    --shielded-integrity-monitoring

# Create and attach SSD data disk
gcloud compute disks create orochi-data-disk \
    --zone=$ZONE \
    --size=${DATA_DISK_SIZE}GB \
    --type=pd-ssd

gcloud compute instances attach-disk $INSTANCE_NAME \
    --zone=$ZONE \
    --disk=orochi-data-disk \
    --device-name=orochi-data

# Create WAL disk (separate for performance)
gcloud compute disks create orochi-wal-disk \
    --zone=$ZONE \
    --size=200GB \
    --type=pd-ssd

gcloud compute instances attach-disk $INSTANCE_NAME \
    --zone=$ZONE \
    --disk=orochi-wal-disk \
    --device-name=orochi-wal
```

### 3.3 Persistent Disk Configuration

```bash
# SSH into the instance
gcloud compute ssh $INSTANCE_NAME --zone=$ZONE --tunnel-through-iap

# Format and mount data disk
sudo mkfs.ext4 -m 0 -E lazy_itable_init=0,lazy_journal_init=0,discard \
    /dev/disk/by-id/google-orochi-data
sudo mkdir -p /var/lib/postgresql/data
sudo mount -o discard,defaults,noatime \
    /dev/disk/by-id/google-orochi-data /var/lib/postgresql/data

# Format and mount WAL disk
sudo mkfs.ext4 -m 0 -E lazy_itable_init=0,lazy_journal_init=0,discard \
    /dev/disk/by-id/google-orochi-wal
sudo mkdir -p /var/lib/postgresql/wal
sudo mount -o discard,defaults,noatime \
    /dev/disk/by-id/google-orochi-wal /var/lib/postgresql/wal

# Add to fstab for persistence
echo "UUID=$(sudo blkid -s UUID -o value /dev/disk/by-id/google-orochi-data) /var/lib/postgresql/data ext4 discard,defaults,noatime 0 2" | sudo tee -a /etc/fstab
echo "UUID=$(sudo blkid -s UUID -o value /dev/disk/by-id/google-orochi-wal) /var/lib/postgresql/wal ext4 discard,defaults,noatime 0 2" | sudo tee -a /etc/fstab

# Set ownership
sudo chown -R postgres:postgres /var/lib/postgresql
```

### 3.4 Install PostgreSQL and Orochi DB

```bash
# Install PostgreSQL 17
sudo apt-get update
sudo apt-get install -y wget gnupg2 lsb-release

# Add PostgreSQL APT repository
sudo sh -c 'echo "deb http://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" > /etc/apt/sources.list.d/pgdg.list'
wget --quiet -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | sudo apt-key add -
sudo apt-get update

# Install PostgreSQL 17 and development packages
sudo apt-get install -y \
    postgresql-17 \
    postgresql-server-dev-17 \
    postgresql-contrib-17 \
    liblz4-dev \
    libzstd-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    librdkafka-dev \
    build-essential \
    git

# Clone and build Orochi DB
cd /opt
sudo git clone https://github.com/your-org/orochi-db.git
cd orochi-db

sudo make clean
sudo make
sudo make install

# Configure PostgreSQL
sudo -u postgres cat > /etc/postgresql/17/main/conf.d/orochi.conf << 'EOF'
# Orochi DB Configuration
shared_preload_libraries = 'orochi'

# Memory Settings
shared_buffers = 32GB
effective_cache_size = 96GB
maintenance_work_mem = 4GB
work_mem = 256MB
huge_pages = try

# WAL Configuration
wal_level = replica
max_wal_size = 16GB
min_wal_size = 4GB
wal_buffers = 256MB
checkpoint_completion_target = 0.9

# Parallelism
max_worker_processes = 32
max_parallel_workers = 16
max_parallel_workers_per_gather = 8
max_parallel_maintenance_workers = 4

# Connection Settings
max_connections = 500
listen_addresses = '*'

# Orochi Specific
orochi.default_shard_count = 64
orochi.columnar_stripe_row_count = 150000
orochi.enable_vectorized = on
orochi.enable_jit = on
orochi.hot_tier_threshold = '1 day'
orochi.warm_tier_threshold = '7 days'
orochi.cold_tier_threshold = '30 days'
EOF

# Configure pg_hba.conf for network access
sudo -u postgres cat >> /etc/postgresql/17/main/pg_hba.conf << 'EOF'
# Allow connections from VPC
host    all    all    10.0.0.0/8    scram-sha-256
EOF

# Move data directory to SSD
sudo systemctl stop postgresql
sudo rsync -av /var/lib/postgresql/17/main/ /var/lib/postgresql/data/
sudo sed -i "s|data_directory = '/var/lib/postgresql/17/main'|data_directory = '/var/lib/postgresql/data'|" /etc/postgresql/17/main/postgresql.conf

# Create symlink for WAL
sudo mv /var/lib/postgresql/data/pg_wal /var/lib/postgresql/wal/
sudo ln -s /var/lib/postgresql/wal/pg_wal /var/lib/postgresql/data/pg_wal

# Start PostgreSQL
sudo systemctl start postgresql
sudo systemctl enable postgresql

# Create extension
sudo -u postgres psql -c "CREATE EXTENSION orochi;"

# Verify installation
sudo -u postgres psql -c "SELECT * FROM pg_extension WHERE extname = 'orochi';"
```

### 3.5 Instance Group and Load Balancer

```bash
# Create instance template
gcloud compute instance-templates create orochi-template \
    --machine-type=$MACHINE_TYPE \
    --network=$VPC_NAME \
    --subnet=$SUBNET_NAME \
    --no-address \
    --tags=orochi-db \
    --service-account=$SA_EMAIL \
    --scopes=cloud-platform \
    --image-family=$IMAGE_FAMILY \
    --image-project=$IMAGE_PROJECT \
    --boot-disk-size=100GB \
    --boot-disk-type=pd-ssd \
    --metadata-from-file=startup-script=startup-script.sh

# Create managed instance group (for read replicas)
gcloud compute instance-groups managed create orochi-read-replicas \
    --template=orochi-template \
    --size=2 \
    --zone=$ZONE

# Create health check
gcloud compute health-checks create tcp orochi-health-check \
    --port=5432 \
    --check-interval=10s \
    --timeout=5s \
    --healthy-threshold=2 \
    --unhealthy-threshold=3

# Create internal TCP load balancer
gcloud compute backend-services create orochi-backend \
    --load-balancing-scheme=INTERNAL \
    --protocol=TCP \
    --region=$REGION \
    --health-checks=orochi-health-check \
    --health-checks-region=$REGION

gcloud compute backend-services add-backend orochi-backend \
    --instance-group=orochi-read-replicas \
    --instance-group-zone=$ZONE \
    --region=$REGION

# Create forwarding rule
gcloud compute forwarding-rules create orochi-ilb \
    --load-balancing-scheme=INTERNAL \
    --network=$VPC_NAME \
    --subnet=$SUBNET_NAME \
    --region=$REGION \
    --backend-service=orochi-backend \
    --ports=5432
```

---

## 4. GKE Deployment

Deploy Orochi DB on Google Kubernetes Engine for container orchestration and auto-scaling.

### 4.1 Create GKE Cluster

```bash
export CLUSTER_NAME="orochi-cluster"
export NODE_POOL_NAME="orochi-nodes"

# Create GKE cluster with VPC-native networking
gcloud container clusters create $CLUSTER_NAME \
    --zone=$ZONE \
    --num-nodes=3 \
    --machine-type=n2-highmem-8 \
    --disk-size=100 \
    --disk-type=pd-ssd \
    --enable-ip-alias \
    --network=$VPC_NAME \
    --subnetwork=$SUBNET_NAME \
    --cluster-secondary-range-name=pods \
    --services-secondary-range-name=services \
    --enable-autoscaling \
    --min-nodes=3 \
    --max-nodes=10 \
    --enable-autorepair \
    --enable-autoupgrade \
    --workload-pool=${PROJECT_ID}.svc.id.goog \
    --enable-vertical-pod-autoscaling \
    --release-channel=stable \
    --addons=GcePersistentDiskCsiDriver

# Get cluster credentials
gcloud container clusters get-credentials $CLUSTER_NAME --zone=$ZONE

# Create dedicated node pool for Orochi DB
gcloud container node-pools create $NODE_POOL_NAME \
    --cluster=$CLUSTER_NAME \
    --zone=$ZONE \
    --machine-type=n2-highmem-16 \
    --num-nodes=3 \
    --disk-size=200 \
    --disk-type=pd-ssd \
    --enable-autoscaling \
    --min-nodes=3 \
    --max-nodes=6 \
    --node-labels=workload=orochi-db \
    --node-taints=dedicated=orochi-db:NoSchedule
```

### 4.2 Kubernetes Manifests

Create namespace and configurations:

```yaml
# orochi-namespace.yaml
apiVersion: v1
kind: Namespace
metadata:
  name: orochi-db
  labels:
    app: orochi-db
---
# orochi-configmap.yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: orochi-config
  namespace: orochi-db
data:
  postgresql.conf: |
    shared_preload_libraries = 'orochi'
    shared_buffers = 16GB
    effective_cache_size = 48GB
    maintenance_work_mem = 2GB
    work_mem = 128MB
    max_connections = 500
    max_worker_processes = 16
    max_parallel_workers = 8
    max_parallel_workers_per_gather = 4
    wal_level = replica
    max_wal_size = 8GB
    min_wal_size = 2GB
    checkpoint_completion_target = 0.9
    orochi.default_shard_count = 32
    orochi.enable_vectorized = on
    orochi.enable_jit = on
    listen_addresses = '*'

  pg_hba.conf: |
    local   all   all                    trust
    host    all   all   127.0.0.1/32     scram-sha-256
    host    all   all   10.0.0.0/8       scram-sha-256
    host    all   all   ::1/128          scram-sha-256
    host    replication all 10.0.0.0/8   scram-sha-256
---
# orochi-secret.yaml
apiVersion: v1
kind: Secret
metadata:
  name: orochi-credentials
  namespace: orochi-db
type: Opaque
stringData:
  POSTGRES_USER: orochi_admin
  POSTGRES_PASSWORD: "CHANGE_ME_TO_SECURE_PASSWORD"
  POSTGRES_DB: orochi
  REPLICATION_USER: replicator
  REPLICATION_PASSWORD: "CHANGE_ME_TO_SECURE_PASSWORD"
```

StatefulSet for Orochi DB:

```yaml
# orochi-statefulset.yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: orochi-db
  namespace: orochi-db
spec:
  serviceName: orochi-db
  replicas: 3
  podManagementPolicy: OrderedReady
  selector:
    matchLabels:
      app: orochi-db
  template:
    metadata:
      labels:
        app: orochi-db
    spec:
      serviceAccountName: orochi-db-sa
      nodeSelector:
        workload: orochi-db
      tolerations:
        - key: "dedicated"
          operator: "Equal"
          value: "orochi-db"
          effect: "NoSchedule"
      securityContext:
        fsGroup: 999
        runAsUser: 999
        runAsGroup: 999
      initContainers:
        - name: init-permissions
          image: busybox:1.36
          command:
            - sh
            - -c
            - |
              chown -R 999:999 /var/lib/postgresql/data
              chown -R 999:999 /var/lib/postgresql/wal
          volumeMounts:
            - name: data
              mountPath: /var/lib/postgresql/data
            - name: wal
              mountPath: /var/lib/postgresql/wal
          securityContext:
            runAsUser: 0
      containers:
        - name: orochi-db
          image: gcr.io/${PROJECT_ID}/orochi-db:latest
          imagePullPolicy: Always
          ports:
            - containerPort: 5432
              name: postgres
          envFrom:
            - secretRef:
                name: orochi-credentials
          env:
            - name: PGDATA
              value: /var/lib/postgresql/data/pgdata
            - name: POD_NAME
              valueFrom:
                fieldRef:
                  fieldPath: metadata.name
          resources:
            requests:
              memory: "32Gi"
              cpu: "8"
            limits:
              memory: "64Gi"
              cpu: "16"
          volumeMounts:
            - name: data
              mountPath: /var/lib/postgresql/data
            - name: wal
              mountPath: /var/lib/postgresql/wal
            - name: config
              mountPath: /etc/postgresql/conf.d
            - name: shm
              mountPath: /dev/shm
          livenessProbe:
            exec:
              command:
                - pg_isready
                - -U
                - orochi_admin
                - -d
                - orochi
            initialDelaySeconds: 30
            periodSeconds: 10
            timeoutSeconds: 5
            failureThreshold: 6
          readinessProbe:
            exec:
              command:
                - pg_isready
                - -U
                - orochi_admin
                - -d
                - orochi
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 3
            failureThreshold: 3
      volumes:
        - name: config
          configMap:
            name: orochi-config
        - name: shm
          emptyDir:
            medium: Memory
            sizeLimit: 4Gi
  volumeClaimTemplates:
    - metadata:
        name: data
      spec:
        accessModes: ["ReadWriteOnce"]
        storageClassName: orochi-ssd
        resources:
          requests:
            storage: 500Gi
    - metadata:
        name: wal
      spec:
        accessModes: ["ReadWriteOnce"]
        storageClassName: orochi-ssd
        resources:
          requests:
            storage: 100Gi
---
# orochi-service.yaml
apiVersion: v1
kind: Service
metadata:
  name: orochi-db
  namespace: orochi-db
  labels:
    app: orochi-db
spec:
  type: ClusterIP
  ports:
    - port: 5432
      targetPort: postgres
      protocol: TCP
      name: postgres
  selector:
    app: orochi-db
---
# orochi-service-headless.yaml
apiVersion: v1
kind: Service
metadata:
  name: orochi-db-headless
  namespace: orochi-db
  labels:
    app: orochi-db
spec:
  type: ClusterIP
  clusterIP: None
  ports:
    - port: 5432
      targetPort: postgres
      protocol: TCP
      name: postgres
  selector:
    app: orochi-db
---
# orochi-service-primary.yaml
apiVersion: v1
kind: Service
metadata:
  name: orochi-db-primary
  namespace: orochi-db
  labels:
    app: orochi-db
    role: primary
spec:
  type: ClusterIP
  ports:
    - port: 5432
      targetPort: postgres
      protocol: TCP
      name: postgres
  selector:
    app: orochi-db
    role: primary
```

### 4.3 Storage Class for Regional Persistent Disks

```yaml
# orochi-storageclass.yaml
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: orochi-ssd
provisioner: pd.csi.storage.gke.io
parameters:
  type: pd-ssd
  replication-type: regional-pd
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
reclaimPolicy: Retain
---
# For balanced persistent disks (cost-optimized)
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: orochi-balanced
provisioner: pd.csi.storage.gke.io
parameters:
  type: pd-balanced
  replication-type: regional-pd
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
reclaimPolicy: Retain
```

### 4.4 Helm Chart Configuration

Create Helm chart structure:

```bash
mkdir -p charts/orochi-db/templates
```

```yaml
# charts/orochi-db/Chart.yaml
apiVersion: v2
name: orochi-db
description: Orochi DB - PostgreSQL HTAP Extension
type: application
version: 1.0.0
appVersion: "1.0.0"
keywords:
  - postgresql
  - htap
  - database
  - timeseries
  - columnar
maintainers:
  - name: Orochi DB Team
    email: orochi@example.com
```

```yaml
# charts/orochi-db/values.yaml
# Default values for orochi-db
replicaCount: 3

image:
  repository: gcr.io/your-project/orochi-db
  pullPolicy: IfNotPresent
  tag: "latest"

imagePullSecrets: []
nameOverride: ""
fullnameOverride: ""

serviceAccount:
  create: true
  annotations:
    iam.gke.io/gcp-service-account: orochi-db-sa@your-project.iam.gserviceaccount.com
  name: "orochi-db-sa"

podAnnotations:
  prometheus.io/scrape: "true"
  prometheus.io/port: "9187"

podSecurityContext:
  fsGroup: 999
  runAsUser: 999
  runAsGroup: 999

securityContext:
  runAsNonRoot: true
  runAsUser: 999
  allowPrivilegeEscalation: false

service:
  type: ClusterIP
  port: 5432

resources:
  requests:
    memory: "32Gi"
    cpu: "8"
  limits:
    memory: "64Gi"
    cpu: "16"

nodeSelector:
  workload: orochi-db

tolerations:
  - key: "dedicated"
    operator: "Equal"
    value: "orochi-db"
    effect: "NoSchedule"

affinity:
  podAntiAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      - labelSelector:
          matchExpressions:
            - key: app
              operator: In
              values:
                - orochi-db
        topologyKey: "kubernetes.io/hostname"

persistence:
  enabled: true
  storageClass: orochi-ssd
  dataSize: 500Gi
  walSize: 100Gi

postgresql:
  database: orochi
  username: orochi_admin
  existingSecret: orochi-credentials
  config:
    shared_buffers: "16GB"
    effective_cache_size: "48GB"
    max_connections: 500
    max_worker_processes: 16
    orochi_shard_count: 32

orochi:
  enableVectorized: true
  enableJit: true
  defaultShardCount: 32
  hotTierThreshold: "1 day"
  warmTierThreshold: "7 days"
  coldTierThreshold: "30 days"

gcs:
  enabled: true
  bucket: "orochi-tiered-storage"
  region: "us-central1"

monitoring:
  enabled: true
  serviceMonitor:
    enabled: true
    interval: 30s

backup:
  enabled: true
  schedule: "0 2 * * *"
  retention: 30
  bucket: "orochi-backups"
```

Deploy with Helm:

```bash
# Install Helm chart
helm install orochi-db ./charts/orochi-db \
    --namespace orochi-db \
    --create-namespace \
    --values values-production.yaml

# Upgrade existing installation
helm upgrade orochi-db ./charts/orochi-db \
    --namespace orochi-db \
    --values values-production.yaml

# Check status
helm status orochi-db -n orochi-db
kubectl get pods -n orochi-db
```

### 4.5 Workload Identity Setup

```bash
# Create Kubernetes service account
kubectl create serviceaccount orochi-db-sa -n orochi-db

# Bind GCP service account to Kubernetes service account
gcloud iam service-accounts add-iam-policy-binding $SA_EMAIL \
    --role=roles/iam.workloadIdentityUser \
    --member="serviceAccount:${PROJECT_ID}.svc.id.goog[orochi-db/orochi-db-sa]"

# Annotate Kubernetes service account
kubectl annotate serviceaccount orochi-db-sa \
    --namespace orochi-db \
    iam.gke.io/gcp-service-account=$SA_EMAIL
```

---

## 5. Cloud Storage Integration

Configure Orochi DB tiered storage with Google Cloud Storage.

### 5.1 Create Storage Buckets

```bash
export BUCKET_PREFIX="orochi-${PROJECT_ID}"

# Create buckets for different tiers
# Cold tier - Standard storage
gsutil mb -p $PROJECT_ID -c STANDARD -l $REGION \
    gs://${BUCKET_PREFIX}-cold/

# Frozen tier - Archive storage
gsutil mb -p $PROJECT_ID -c ARCHIVE -l $REGION \
    gs://${BUCKET_PREFIX}-frozen/

# Backup bucket - Nearline for cost optimization
gsutil mb -p $PROJECT_ID -c NEARLINE -l $REGION \
    gs://${BUCKET_PREFIX}-backups/

# Cross-region bucket for disaster recovery
gsutil mb -p $PROJECT_ID -c STANDARD -l US \
    gs://${BUCKET_PREFIX}-dr/
```

### 5.2 Bucket Configuration

```bash
# Enable versioning for data protection
gsutil versioning set on gs://${BUCKET_PREFIX}-cold/
gsutil versioning set on gs://${BUCKET_PREFIX}-frozen/
gsutil versioning set on gs://${BUCKET_PREFIX}-backups/

# Set lifecycle policies
cat > lifecycle-cold.json << 'EOF'
{
  "lifecycle": {
    "rule": [
      {
        "action": {"type": "Delete"},
        "condition": {"age": 365}
      },
      {
        "action": {"type": "SetStorageClass", "storageClass": "NEARLINE"},
        "condition": {"age": 90}
      }
    ]
  }
}
EOF

gsutil lifecycle set lifecycle-cold.json gs://${BUCKET_PREFIX}-cold/

cat > lifecycle-frozen.json << 'EOF'
{
  "lifecycle": {
    "rule": [
      {
        "action": {"type": "Delete"},
        "condition": {"age": 2555}
      }
    ]
  }
}
EOF

gsutil lifecycle set lifecycle-frozen.json gs://${BUCKET_PREFIX}-frozen/

# Set retention policy for compliance (optional)
gsutil retention set 365d gs://${BUCKET_PREFIX}-backups/

# Enable uniform bucket-level access
gsutil uniformbucketlevelaccess set on gs://${BUCKET_PREFIX}-cold/
gsutil uniformbucketlevelaccess set on gs://${BUCKET_PREFIX}-frozen/
gsutil uniformbucketlevelaccess set on gs://${BUCKET_PREFIX}-backups/
```

### 5.3 Service Account Permissions

```bash
# Grant storage permissions to service account
gsutil iam ch serviceAccount:${SA_EMAIL}:objectAdmin \
    gs://${BUCKET_PREFIX}-cold/

gsutil iam ch serviceAccount:${SA_EMAIL}:objectAdmin \
    gs://${BUCKET_PREFIX}-frozen/

gsutil iam ch serviceAccount:${SA_EMAIL}:objectAdmin \
    gs://${BUCKET_PREFIX}-backups/

gsutil iam ch serviceAccount:${SA_EMAIL}:objectAdmin \
    gs://${BUCKET_PREFIX}-dr/
```

### 5.4 Configure Orochi Tiered Storage

```sql
-- Connect to PostgreSQL and configure tiered storage
-- Set GCS credentials and bucket configuration

-- Configure tiered storage parameters
ALTER SYSTEM SET orochi.cold_storage_bucket = 'gs://orochi-project-cold';
ALTER SYSTEM SET orochi.frozen_storage_bucket = 'gs://orochi-project-frozen';
ALTER SYSTEM SET orochi.gcs_credentials_file = '/etc/orochi/gcs-credentials.json';

-- Or use Workload Identity (no credentials file needed)
ALTER SYSTEM SET orochi.use_workload_identity = on;

SELECT pg_reload_conf();

-- Create a table with tiered storage enabled
SELECT orochi_create_hypertable(
    'sensor_data',
    'timestamp',
    chunk_time_interval => INTERVAL '1 day',
    enable_tiered_storage => true
);

-- Configure tier thresholds
SELECT orochi_set_tier_policy(
    'sensor_data',
    hot_threshold => INTERVAL '1 day',
    warm_threshold => INTERVAL '7 days',
    cold_threshold => INTERVAL '30 days',
    frozen_threshold => INTERVAL '90 days'
);

-- View tier status
SELECT * FROM orochi_tier_status('sensor_data');

-- Manual tier movement (for testing)
SELECT orochi_move_to_tier('sensor_data', 'cold',
    older_than => INTERVAL '30 days');
```

### 5.5 Cross-Region Replication

```bash
# Enable cross-region replication for disaster recovery
gsutil rewrite -r -s STANDARD gs://${BUCKET_PREFIX}-cold/** \
    gs://${BUCKET_PREFIX}-dr/

# Set up ongoing replication with Transfer Service
gcloud transfer jobs create \
    gs://${BUCKET_PREFIX}-cold \
    gs://${BUCKET_PREFIX}-dr \
    --source-agent-pool=projects/${PROJECT_ID}/agentPools/default \
    --schedule-starts=$(date -u +%Y-%m-%dT%H:%M:%SZ) \
    --schedule-repeats-every=24h \
    --name="orochi-dr-replication"
```

---

## 6. AlloyDB Configuration

AlloyDB provides PostgreSQL-compatible managed service optimized for demanding workloads.

### 6.1 Create AlloyDB Cluster

```bash
export ALLOYDB_CLUSTER="orochi-alloydb"
export ALLOYDB_INSTANCE="orochi-primary"
export ALLOYDB_PASSWORD=$(openssl rand -base64 24)

# Create AlloyDB cluster
gcloud alloydb clusters create $ALLOYDB_CLUSTER \
    --region=$REGION \
    --password=$ALLOYDB_PASSWORD \
    --network=$VPC_NAME \
    --async

# Wait for cluster creation
gcloud alloydb operations list --region=$REGION

# Create primary instance
gcloud alloydb instances create $ALLOYDB_INSTANCE \
    --cluster=$ALLOYDB_CLUSTER \
    --region=$REGION \
    --instance-type=PRIMARY \
    --cpu-count=16 \
    --machine-type=n2-highmem-16 \
    --database-flags=\
max_connections=500,\
shared_buffers=16GB,\
effective_cache_size=48GB,\
maintenance_work_mem=2GB,\
work_mem=128MB,\
max_worker_processes=16,\
max_parallel_workers=8

# Create read pool for analytics
gcloud alloydb instances create orochi-read-pool \
    --cluster=$ALLOYDB_CLUSTER \
    --region=$REGION \
    --instance-type=READ_POOL \
    --cpu-count=8 \
    --read-pool-node-count=2
```

### 6.2 AlloyDB Columnar Engine

AlloyDB includes a built-in columnar engine for analytical queries:

```sql
-- Connect to AlloyDB
-- Enable columnar engine for specific tables

-- Check columnar engine status
SELECT google_columnar_engine_status();

-- Recommend columns for columnar storage
SELECT * FROM google_columnar_engine_recommend('my_table');

-- Add columns to columnar engine
ALTER TABLE sensor_data ADD COLUMN value
    USING google_columnar_engine;

-- Or enable auto-columnar
ALTER SYSTEM SET google_columnar_engine.auto_columnarization = on;
SELECT pg_reload_conf();

-- View columnar statistics
SELECT * FROM google_columnar_engine_stats();
```

### 6.3 AlloyDB AI Integration

```sql
-- Enable AI features (if available in your region)
CREATE EXTENSION IF NOT EXISTS google_ml_integration;

-- Use built-in embedding generation
SELECT google_ml.embedding(
    'text-embedding-004',
    'Sample text for embedding'
);

-- Create vector column
ALTER TABLE documents ADD COLUMN embedding vector(768);

-- Generate embeddings
UPDATE documents
SET embedding = google_ml.embedding('text-embedding-004', content);

-- Similarity search
SELECT content,
       embedding <-> google_ml.embedding('text-embedding-004', 'search query') AS distance
FROM documents
ORDER BY distance
LIMIT 10;
```

### 6.4 AlloyDB Backups

```bash
# Create on-demand backup
gcloud alloydb backups create orochi-backup-$(date +%Y%m%d) \
    --cluster=$ALLOYDB_CLUSTER \
    --region=$REGION \
    --async

# Configure automated backups
gcloud alloydb clusters update $ALLOYDB_CLUSTER \
    --region=$REGION \
    --automated-backup-enabled \
    --automated-backup-start-time=02:00 \
    --automated-backup-retention-period=14d

# List backups
gcloud alloydb backups list --region=$REGION
```

---

## 7. Terraform Templates

Infrastructure as Code for reproducible deployments.

### 7.1 Project Structure

```
terraform/
├── environments/
│   ├── dev/
│   │   ├── main.tf
│   │   ├── variables.tf
│   │   └── terraform.tfvars
│   ├── staging/
│   └── production/
├── modules/
│   ├── vpc/
│   ├── cloud-sql/
│   ├── compute/
│   ├── gke/
│   ├── storage/
│   └── alloydb/
├── main.tf
├── variables.tf
├── outputs.tf
└── versions.tf
```

### 7.2 Root Module

```hcl
# terraform/versions.tf
terraform {
  required_version = ">= 1.6.0"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 5.0"
    }
    google-beta = {
      source  = "hashicorp/google-beta"
      version = "~> 5.0"
    }
    kubernetes = {
      source  = "hashicorp/kubernetes"
      version = "~> 2.24"
    }
    helm = {
      source  = "hashicorp/helm"
      version = "~> 2.12"
    }
  }

  backend "gcs" {
    bucket = "orochi-terraform-state"
    prefix = "terraform/state"
  }
}

provider "google" {
  project = var.project_id
  region  = var.region
}

provider "google-beta" {
  project = var.project_id
  region  = var.region
}
```

```hcl
# terraform/variables.tf
variable "project_id" {
  description = "GCP Project ID"
  type        = string
}

variable "region" {
  description = "GCP Region"
  type        = string
  default     = "us-central1"
}

variable "zone" {
  description = "GCP Zone"
  type        = string
  default     = "us-central1-a"
}

variable "environment" {
  description = "Environment name (dev, staging, production)"
  type        = string
  default     = "production"
}

variable "vpc_cidr" {
  description = "VPC CIDR range"
  type        = string
  default     = "10.0.0.0/16"
}

variable "enable_cloud_sql" {
  description = "Enable Cloud SQL deployment"
  type        = bool
  default     = false
}

variable "enable_gke" {
  description = "Enable GKE deployment"
  type        = bool
  default     = true
}

variable "enable_alloydb" {
  description = "Enable AlloyDB deployment"
  type        = bool
  default     = false
}

variable "postgresql_version" {
  description = "PostgreSQL version"
  type        = string
  default     = "POSTGRES_17"
}

variable "machine_type" {
  description = "Machine type for database instances"
  type        = string
  default     = "n2-highmem-16"
}

variable "disk_size_gb" {
  description = "Data disk size in GB"
  type        = number
  default     = 500
}
```

### 7.3 VPC Module

```hcl
# terraform/modules/vpc/main.tf
resource "google_compute_network" "orochi_vpc" {
  name                    = "${var.name_prefix}-vpc"
  auto_create_subnetworks = false
  routing_mode            = "REGIONAL"
  project                 = var.project_id
}

resource "google_compute_subnetwork" "orochi_subnet" {
  name                     = "${var.name_prefix}-subnet"
  ip_cidr_range            = var.subnet_cidr
  region                   = var.region
  network                  = google_compute_network.orochi_vpc.id
  private_ip_google_access = true

  secondary_ip_range {
    range_name    = "pods"
    ip_cidr_range = var.pods_cidr
  }

  secondary_ip_range {
    range_name    = "services"
    ip_cidr_range = var.services_cidr
  }

  log_config {
    aggregation_interval = "INTERVAL_5_SEC"
    flow_sampling        = 0.5
    metadata             = "INCLUDE_ALL_METADATA"
  }
}

resource "google_compute_firewall" "allow_internal" {
  name    = "${var.name_prefix}-allow-internal"
  network = google_compute_network.orochi_vpc.name

  allow {
    protocol = "tcp"
  }
  allow {
    protocol = "udp"
  }
  allow {
    protocol = "icmp"
  }

  source_ranges = [var.subnet_cidr, var.pods_cidr, var.services_cidr]
}

resource "google_compute_firewall" "allow_postgres" {
  name    = "${var.name_prefix}-allow-postgres"
  network = google_compute_network.orochi_vpc.name

  allow {
    protocol = "tcp"
    ports    = ["5432"]
  }

  source_ranges = var.allowed_postgres_cidrs
  target_tags   = ["orochi-db"]
}

resource "google_compute_firewall" "allow_health_checks" {
  name    = "${var.name_prefix}-allow-health-checks"
  network = google_compute_network.orochi_vpc.name

  allow {
    protocol = "tcp"
    ports    = ["5432"]
  }

  source_ranges = ["130.211.0.0/22", "35.191.0.0/16"]
  target_tags   = ["orochi-db"]
}

resource "google_compute_global_address" "private_ip_range" {
  name          = "${var.name_prefix}-private-ip-range"
  purpose       = "VPC_PEERING"
  address_type  = "INTERNAL"
  prefix_length = 16
  network       = google_compute_network.orochi_vpc.id
}

resource "google_service_networking_connection" "private_vpc_connection" {
  network                 = google_compute_network.orochi_vpc.id
  service                 = "servicenetworking.googleapis.com"
  reserved_peering_ranges = [google_compute_global_address.private_ip_range.name]
}

resource "google_compute_router" "orochi_router" {
  name    = "${var.name_prefix}-router"
  region  = var.region
  network = google_compute_network.orochi_vpc.id
}

resource "google_compute_router_nat" "orochi_nat" {
  name                               = "${var.name_prefix}-nat"
  router                             = google_compute_router.orochi_router.name
  region                             = var.region
  nat_ip_allocate_option             = "AUTO_ONLY"
  source_subnetwork_ip_ranges_to_nat = "ALL_SUBNETWORKS_ALL_IP_RANGES"

  log_config {
    enable = true
    filter = "ERRORS_ONLY"
  }
}

output "vpc_id" {
  value = google_compute_network.orochi_vpc.id
}

output "vpc_name" {
  value = google_compute_network.orochi_vpc.name
}

output "subnet_id" {
  value = google_compute_subnetwork.orochi_subnet.id
}

output "subnet_name" {
  value = google_compute_subnetwork.orochi_subnet.name
}
```

### 7.4 Cloud SQL Module

```hcl
# terraform/modules/cloud-sql/main.tf
resource "google_sql_database_instance" "orochi_primary" {
  name                = "${var.name_prefix}-primary"
  database_version    = var.postgresql_version
  region              = var.region
  deletion_protection = var.deletion_protection

  depends_on = [var.private_vpc_connection]

  settings {
    tier              = var.machine_type
    availability_type = "REGIONAL"
    disk_type         = "PD_SSD"
    disk_size         = var.disk_size_gb
    disk_autoresize   = true
    disk_autoresize_limit = var.disk_autoresize_limit

    backup_configuration {
      enabled                        = true
      start_time                     = "02:00"
      location                       = var.backup_location
      point_in_time_recovery_enabled = true
      transaction_log_retention_days = 7

      backup_retention_settings {
        retained_backups = var.backup_retention_count
        retention_unit   = "COUNT"
      }
    }

    maintenance_window {
      day          = 7  # Sunday
      hour         = 3
      update_track = "stable"
    }

    ip_configuration {
      ipv4_enabled                                  = false
      private_network                               = var.vpc_id
      enable_private_path_for_google_cloud_services = true
    }

    insights_config {
      query_insights_enabled  = true
      query_plans_per_minute  = 5
      query_string_length     = 4500
      record_application_tags = true
      record_client_address   = true
    }

    database_flags {
      name  = "max_connections"
      value = "500"
    }
    database_flags {
      name  = "shared_buffers"
      value = "8589934592"  # 8GB in bytes
    }
    database_flags {
      name  = "effective_cache_size"
      value = "25769803776"  # 24GB in bytes
    }
    database_flags {
      name  = "maintenance_work_mem"
      value = "2147483648"  # 2GB in bytes
    }
    database_flags {
      name  = "checkpoint_completion_target"
      value = "0.9"
    }
    database_flags {
      name  = "wal_buffers"
      value = "67108864"  # 64MB in bytes
    }
    database_flags {
      name  = "max_worker_processes"
      value = "16"
    }
    database_flags {
      name  = "max_parallel_workers_per_gather"
      value = "4"
    }
    database_flags {
      name  = "max_parallel_workers"
      value = "8"
    }
    database_flags {
      name  = "log_min_duration_statement"
      value = "1000"
    }
  }
}

resource "google_sql_database" "orochi_db" {
  name     = "orochi"
  instance = google_sql_database_instance.orochi_primary.name
  charset  = "UTF8"
  collation = "en_US.UTF8"
}

resource "google_sql_user" "orochi_app" {
  name     = "orochi_app"
  instance = google_sql_database_instance.orochi_primary.name
  password = var.app_user_password
}

resource "google_sql_user" "orochi_readonly" {
  name     = "orochi_readonly"
  instance = google_sql_database_instance.orochi_primary.name
  password = var.readonly_user_password
}

# Read replicas
resource "google_sql_database_instance" "orochi_replica" {
  count                = var.replica_count
  name                 = "${var.name_prefix}-replica-${count.index + 1}"
  database_version     = var.postgresql_version
  region               = var.region
  master_instance_name = google_sql_database_instance.orochi_primary.name

  replica_configuration {
    failover_target = false
  }

  settings {
    tier              = var.replica_machine_type
    availability_type = "ZONAL"
    disk_type         = "PD_SSD"
    disk_size         = var.disk_size_gb
    disk_autoresize   = true

    ip_configuration {
      ipv4_enabled    = false
      private_network = var.vpc_id
    }
  }
}

output "instance_name" {
  value = google_sql_database_instance.orochi_primary.name
}

output "connection_name" {
  value = google_sql_database_instance.orochi_primary.connection_name
}

output "private_ip" {
  value = google_sql_database_instance.orochi_primary.private_ip_address
}

output "replica_ips" {
  value = google_sql_database_instance.orochi_replica[*].private_ip_address
}
```

### 7.5 GKE Module

```hcl
# terraform/modules/gke/main.tf
resource "google_container_cluster" "orochi_cluster" {
  name     = "${var.name_prefix}-cluster"
  location = var.zone

  # Use separately managed node pools
  remove_default_node_pool = true
  initial_node_count       = 1

  network    = var.vpc_name
  subnetwork = var.subnet_name

  networking_mode = "VPC_NATIVE"
  ip_allocation_policy {
    cluster_secondary_range_name  = "pods"
    services_secondary_range_name = "services"
  }

  private_cluster_config {
    enable_private_nodes    = true
    enable_private_endpoint = false
    master_ipv4_cidr_block  = "172.16.0.0/28"
  }

  master_authorized_networks_config {
    dynamic "cidr_blocks" {
      for_each = var.master_authorized_networks
      content {
        cidr_block   = cidr_blocks.value.cidr_block
        display_name = cidr_blocks.value.display_name
      }
    }
  }

  workload_identity_config {
    workload_pool = "${var.project_id}.svc.id.goog"
  }

  release_channel {
    channel = "STABLE"
  }

  addons_config {
    gce_persistent_disk_csi_driver_config {
      enabled = true
    }
    horizontal_pod_autoscaling {
      disabled = false
    }
    http_load_balancing {
      disabled = false
    }
  }

  vertical_pod_autoscaling {
    enabled = true
  }

  cluster_autoscaling {
    enabled = true
    resource_limits {
      resource_type = "cpu"
      minimum       = 16
      maximum       = 256
    }
    resource_limits {
      resource_type = "memory"
      minimum       = 64
      maximum       = 1024
    }
  }

  maintenance_policy {
    recurring_window {
      start_time = "2024-01-01T03:00:00Z"
      end_time   = "2024-01-01T07:00:00Z"
      recurrence = "FREQ=WEEKLY;BYDAY=SU"
    }
  }

  logging_config {
    enable_components = ["SYSTEM_COMPONENTS", "WORKLOADS"]
  }

  monitoring_config {
    enable_components = ["SYSTEM_COMPONENTS"]
    managed_prometheus {
      enabled = true
    }
  }
}

resource "google_container_node_pool" "orochi_nodes" {
  name       = "${var.name_prefix}-node-pool"
  location   = var.zone
  cluster    = google_container_cluster.orochi_cluster.name
  node_count = var.node_count

  autoscaling {
    min_node_count = var.min_node_count
    max_node_count = var.max_node_count
  }

  management {
    auto_repair  = true
    auto_upgrade = true
  }

  node_config {
    machine_type = var.node_machine_type
    disk_size_gb = var.node_disk_size
    disk_type    = "pd-ssd"

    labels = {
      workload = "orochi-db"
    }

    taint {
      key    = "dedicated"
      value  = "orochi-db"
      effect = "NO_SCHEDULE"
    }

    service_account = var.service_account_email
    oauth_scopes = [
      "https://www.googleapis.com/auth/cloud-platform"
    ]

    shielded_instance_config {
      enable_secure_boot          = true
      enable_integrity_monitoring = true
    }

    workload_metadata_config {
      mode = "GKE_METADATA"
    }
  }
}

output "cluster_name" {
  value = google_container_cluster.orochi_cluster.name
}

output "cluster_endpoint" {
  value     = google_container_cluster.orochi_cluster.endpoint
  sensitive = true
}

output "cluster_ca_certificate" {
  value     = google_container_cluster.orochi_cluster.master_auth[0].cluster_ca_certificate
  sensitive = true
}
```

### 7.6 Storage Module

```hcl
# terraform/modules/storage/main.tf
resource "google_storage_bucket" "orochi_cold" {
  name                        = "${var.name_prefix}-cold-${var.project_id}"
  location                    = var.region
  storage_class               = "STANDARD"
  uniform_bucket_level_access = true

  versioning {
    enabled = true
  }

  lifecycle_rule {
    condition {
      age = 90
    }
    action {
      type          = "SetStorageClass"
      storage_class = "NEARLINE"
    }
  }

  lifecycle_rule {
    condition {
      age = 365
    }
    action {
      type = "Delete"
    }
  }

  labels = {
    environment = var.environment
    tier        = "cold"
  }
}

resource "google_storage_bucket" "orochi_frozen" {
  name                        = "${var.name_prefix}-frozen-${var.project_id}"
  location                    = var.region
  storage_class               = "ARCHIVE"
  uniform_bucket_level_access = true

  versioning {
    enabled = true
  }

  lifecycle_rule {
    condition {
      age = 2555  # 7 years
    }
    action {
      type = "Delete"
    }
  }

  labels = {
    environment = var.environment
    tier        = "frozen"
  }
}

resource "google_storage_bucket" "orochi_backups" {
  name                        = "${var.name_prefix}-backups-${var.project_id}"
  location                    = var.region
  storage_class               = "NEARLINE"
  uniform_bucket_level_access = true

  versioning {
    enabled = true
  }

  retention_policy {
    retention_period = 2592000  # 30 days
  }

  lifecycle_rule {
    condition {
      age = var.backup_retention_days
    }
    action {
      type = "Delete"
    }
  }

  labels = {
    environment = var.environment
    purpose     = "backups"
  }
}

# Cross-region DR bucket
resource "google_storage_bucket" "orochi_dr" {
  name                        = "${var.name_prefix}-dr-${var.project_id}"
  location                    = "US"  # Multi-region
  storage_class               = "STANDARD"
  uniform_bucket_level_access = true

  versioning {
    enabled = true
  }

  labels = {
    environment = var.environment
    purpose     = "disaster-recovery"
  }
}

# IAM bindings
resource "google_storage_bucket_iam_member" "cold_admin" {
  bucket = google_storage_bucket.orochi_cold.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${var.service_account_email}"
}

resource "google_storage_bucket_iam_member" "frozen_admin" {
  bucket = google_storage_bucket.orochi_frozen.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${var.service_account_email}"
}

resource "google_storage_bucket_iam_member" "backups_admin" {
  bucket = google_storage_bucket.orochi_backups.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${var.service_account_email}"
}

resource "google_storage_bucket_iam_member" "dr_admin" {
  bucket = google_storage_bucket.orochi_dr.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${var.service_account_email}"
}

output "cold_bucket_name" {
  value = google_storage_bucket.orochi_cold.name
}

output "frozen_bucket_name" {
  value = google_storage_bucket.orochi_frozen.name
}

output "backups_bucket_name" {
  value = google_storage_bucket.orochi_backups.name
}

output "dr_bucket_name" {
  value = google_storage_bucket.orochi_dr.name
}
```

### 7.7 Main Deployment

```hcl
# terraform/main.tf
module "vpc" {
  source = "./modules/vpc"

  name_prefix             = "orochi-${var.environment}"
  project_id              = var.project_id
  region                  = var.region
  subnet_cidr             = "10.0.0.0/20"
  pods_cidr               = "10.4.0.0/14"
  services_cidr           = "10.8.0.0/20"
  allowed_postgres_cidrs  = var.allowed_postgres_cidrs
}

module "storage" {
  source = "./modules/storage"

  name_prefix            = "orochi-${var.environment}"
  project_id             = var.project_id
  region                 = var.region
  environment            = var.environment
  service_account_email  = google_service_account.orochi_sa.email
  backup_retention_days  = 30
}

module "cloud_sql" {
  source = "./modules/cloud-sql"
  count  = var.enable_cloud_sql ? 1 : 0

  name_prefix            = "orochi-${var.environment}"
  project_id             = var.project_id
  region                 = var.region
  postgresql_version     = var.postgresql_version
  machine_type           = "db-custom-8-32768"
  disk_size_gb           = var.disk_size_gb
  disk_autoresize_limit  = 2000
  vpc_id                 = module.vpc.vpc_id
  private_vpc_connection = module.vpc.private_vpc_connection
  backup_location        = var.region
  backup_retention_count = 30
  deletion_protection    = true
  replica_count          = 2
  replica_machine_type   = "db-custom-4-16384"
  app_user_password      = var.app_user_password
  readonly_user_password = var.readonly_user_password
}

module "gke" {
  source = "./modules/gke"
  count  = var.enable_gke ? 1 : 0

  name_prefix                = "orochi-${var.environment}"
  project_id                 = var.project_id
  region                     = var.region
  zone                       = var.zone
  vpc_name                   = module.vpc.vpc_name
  subnet_name                = module.vpc.subnet_name
  service_account_email      = google_service_account.orochi_sa.email
  node_machine_type          = var.machine_type
  node_disk_size             = 200
  node_count                 = 3
  min_node_count             = 3
  max_node_count             = 10
  master_authorized_networks = var.master_authorized_networks
}

# Service Account
resource "google_service_account" "orochi_sa" {
  account_id   = "orochi-db-sa"
  display_name = "Orochi DB Service Account"
  description  = "Service account for Orochi DB operations"
}

resource "google_project_iam_member" "orochi_sa_roles" {
  for_each = toset([
    "roles/cloudsql.client",
    "roles/storage.objectAdmin",
    "roles/monitoring.metricWriter",
    "roles/logging.logWriter",
  ])

  project = var.project_id
  role    = each.value
  member  = "serviceAccount:${google_service_account.orochi_sa.email}"
}

# Workload Identity binding for GKE
resource "google_service_account_iam_member" "workload_identity" {
  count              = var.enable_gke ? 1 : 0
  service_account_id = google_service_account.orochi_sa.name
  role               = "roles/iam.workloadIdentityUser"
  member             = "serviceAccount:${var.project_id}.svc.id.goog[orochi-db/orochi-db-sa]"
}
```

### 7.8 Terraform Usage

```bash
# Initialize Terraform
cd terraform/environments/production
terraform init

# Plan deployment
terraform plan -var-file=terraform.tfvars -out=tfplan

# Apply
terraform apply tfplan

# Destroy (caution!)
terraform destroy -var-file=terraform.tfvars
```

---

## 8. Monitoring and Observability

### 8.1 Cloud Monitoring Dashboard

Create a custom monitoring dashboard:

```bash
# Create dashboard from JSON
cat > orochi-dashboard.json << 'EOF'
{
  "displayName": "Orochi DB Monitoring",
  "gridLayout": {
    "columns": 2,
    "widgets": [
      {
        "title": "CPU Utilization",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/cpu/utilization\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_MEAN",
                  "crossSeriesReducer": "REDUCE_MEAN"
                }
              }
            }
          }]
        }
      },
      {
        "title": "Memory Utilization",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/memory/utilization\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_MEAN"
                }
              }
            }
          }]
        }
      },
      {
        "title": "Active Connections",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/postgresql/num_backends\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_MEAN"
                }
              }
            }
          }]
        }
      },
      {
        "title": "Disk IOPS",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/disk/read_ops_count\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_RATE"
                }
              }
            }
          }]
        }
      },
      {
        "title": "Replication Lag",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/replication/replica_lag\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_MEAN"
                }
              }
            }
          }]
        }
      },
      {
        "title": "Query Latency",
        "xyChart": {
          "dataSets": [{
            "timeSeriesQuery": {
              "timeSeriesFilter": {
                "filter": "resource.type=\"cloudsql_database\" AND metric.type=\"cloudsql.googleapis.com/database/postgresql/transaction_count\"",
                "aggregation": {
                  "perSeriesAligner": "ALIGN_RATE"
                }
              }
            }
          }]
        }
      }
    ]
  }
}
EOF

gcloud monitoring dashboards create --config-from-file=orochi-dashboard.json
```

### 8.2 Alerting Policies

```bash
# High CPU alert
gcloud alpha monitoring policies create \
    --display-name="Orochi DB High CPU" \
    --condition-display-name="CPU > 80%" \
    --condition-filter='resource.type="cloudsql_database" AND metric.type="cloudsql.googleapis.com/database/cpu/utilization"' \
    --condition-threshold-value=0.8 \
    --condition-threshold-duration=300s \
    --condition-threshold-comparison=COMPARISON_GT \
    --notification-channels=YOUR_NOTIFICATION_CHANNEL_ID

# High memory alert
gcloud alpha monitoring policies create \
    --display-name="Orochi DB High Memory" \
    --condition-display-name="Memory > 90%" \
    --condition-filter='resource.type="cloudsql_database" AND metric.type="cloudsql.googleapis.com/database/memory/utilization"' \
    --condition-threshold-value=0.9 \
    --condition-threshold-duration=300s \
    --condition-threshold-comparison=COMPARISON_GT \
    --notification-channels=YOUR_NOTIFICATION_CHANNEL_ID

# Replication lag alert
gcloud alpha monitoring policies create \
    --display-name="Orochi DB Replication Lag" \
    --condition-display-name="Lag > 60s" \
    --condition-filter='resource.type="cloudsql_database" AND metric.type="cloudsql.googleapis.com/database/replication/replica_lag"' \
    --condition-threshold-value=60 \
    --condition-threshold-duration=300s \
    --condition-threshold-comparison=COMPARISON_GT \
    --notification-channels=YOUR_NOTIFICATION_CHANNEL_ID

# Disk utilization alert
gcloud alpha monitoring policies create \
    --display-name="Orochi DB Disk Space" \
    --condition-display-name="Disk > 85%" \
    --condition-filter='resource.type="cloudsql_database" AND metric.type="cloudsql.googleapis.com/database/disk/utilization"' \
    --condition-threshold-value=0.85 \
    --condition-threshold-duration=300s \
    --condition-threshold-comparison=COMPARISON_GT \
    --notification-channels=YOUR_NOTIFICATION_CHANNEL_ID
```

### 8.3 Query Insights

Enable and use Query Insights for Cloud SQL:

```bash
# Query Insights is enabled during instance creation
# Access via Cloud Console or API

# View top queries by execution time
gcloud sql operations list \
    --instance=$INSTANCE_NAME \
    --filter="operationType=QUERY_INSIGHTS"

# Export Query Insights data to BigQuery
gcloud sql instances patch $INSTANCE_NAME \
    --insights-config-query-string-length=4500 \
    --insights-config-record-application-tags \
    --insights-config-record-client-address
```

### 8.4 Custom Metrics with Prometheus

For GKE deployments, export PostgreSQL metrics:

```yaml
# postgres-exporter.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: postgres-exporter
  namespace: orochi-db
spec:
  replicas: 1
  selector:
    matchLabels:
      app: postgres-exporter
  template:
    metadata:
      labels:
        app: postgres-exporter
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "9187"
    spec:
      containers:
        - name: postgres-exporter
          image: prometheuscommunity/postgres-exporter:v0.15.0
          ports:
            - containerPort: 9187
          env:
            - name: DATA_SOURCE_URI
              value: "orochi-db:5432/orochi?sslmode=disable"
            - name: DATA_SOURCE_USER
              valueFrom:
                secretKeyRef:
                  name: orochi-credentials
                  key: POSTGRES_USER
            - name: DATA_SOURCE_PASS
              valueFrom:
                secretKeyRef:
                  name: orochi-credentials
                  key: POSTGRES_PASSWORD
            - name: PG_EXPORTER_EXTEND_QUERY_PATH
              value: /etc/postgres-exporter/queries.yaml
          volumeMounts:
            - name: queries
              mountPath: /etc/postgres-exporter
      volumes:
        - name: queries
          configMap:
            name: postgres-exporter-queries
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: postgres-exporter-queries
  namespace: orochi-db
data:
  queries.yaml: |
    orochi_shard_count:
      query: "SELECT count(*) as count FROM orochi_shards"
      master: true
      metrics:
        - count:
            usage: "GAUGE"
            description: "Number of Orochi shards"

    orochi_chunk_count:
      query: "SELECT count(*) as count FROM orochi_chunks"
      master: true
      metrics:
        - count:
            usage: "GAUGE"
            description: "Number of time-series chunks"

    orochi_tier_sizes:
      query: |
        SELECT tier, sum(size_bytes) as bytes
        FROM orochi_tier_status
        GROUP BY tier
      master: true
      metrics:
        - tier:
            usage: "LABEL"
            description: "Storage tier"
        - bytes:
            usage: "GAUGE"
            description: "Size in bytes per tier"
---
apiVersion: v1
kind: Service
metadata:
  name: postgres-exporter
  namespace: orochi-db
  labels:
    app: postgres-exporter
spec:
  ports:
    - port: 9187
      targetPort: 9187
  selector:
    app: postgres-exporter
```

### 8.5 Log-Based Metrics

```bash
# Create log-based metric for slow queries
gcloud logging metrics create orochi-slow-queries \
    --description="Count of slow queries over 1 second" \
    --log-filter='resource.type="cloudsql_database"
                  AND textPayload=~"duration:"
                  AND textPayload=~"ms"' \
    --bucket-options='{"explicit_buckets":{"bounds":[1000,5000,10000,30000,60000]}}'

# Create log-based metric for errors
gcloud logging metrics create orochi-errors \
    --description="Count of PostgreSQL errors" \
    --log-filter='resource.type="cloudsql_database"
                  AND severity>=ERROR'
```

---

## 9. Security Best Practices

### 9.1 Network Security

```bash
# Enable VPC Service Controls
gcloud access-context-manager perimeters create orochi-perimeter \
    --title="Orochi DB Perimeter" \
    --resources="projects/$PROJECT_ID" \
    --restricted-services="sqladmin.googleapis.com,storage.googleapis.com" \
    --access-levels="accessPolicies/YOUR_POLICY/accessLevels/corp-network"

# Enable Private Google Access
gcloud compute networks subnets update $SUBNET_NAME \
    --region=$REGION \
    --enable-private-ip-google-access

# Configure Cloud Armor (for external access)
gcloud compute security-policies create orochi-security-policy \
    --description="Security policy for Orochi DB"

gcloud compute security-policies rules create 1000 \
    --security-policy=orochi-security-policy \
    --expression="origin.region_code == 'US'" \
    --action=allow

gcloud compute security-policies rules create 2147483647 \
    --security-policy=orochi-security-policy \
    --action=deny-403
```

### 9.2 Encryption

```bash
# Create Cloud KMS key for encryption
gcloud kms keyrings create orochi-keyring \
    --location=$REGION

gcloud kms keys create orochi-key \
    --location=$REGION \
    --keyring=orochi-keyring \
    --purpose=encryption \
    --rotation-period=90d

# Grant Cloud SQL service account access to KMS key
gcloud kms keys add-iam-policy-binding orochi-key \
    --location=$REGION \
    --keyring=orochi-keyring \
    --member="serviceAccount:service-${PROJECT_NUMBER}@gcp-sa-cloud-sql.iam.gserviceaccount.com" \
    --role="roles/cloudkms.cryptoKeyEncrypterDecrypter"

# Create Cloud SQL instance with CMEK
gcloud sql instances create $INSTANCE_NAME \
    --disk-encryption-key=projects/$PROJECT_ID/locations/$REGION/keyRings/orochi-keyring/cryptoKeys/orochi-key \
    # ... other flags
```

### 9.3 IAM Best Practices

```bash
# Create custom role with minimal permissions
gcloud iam roles create orochiAppRole \
    --project=$PROJECT_ID \
    --title="Orochi Application Role" \
    --description="Minimal permissions for Orochi application" \
    --permissions=cloudsql.instances.connect,cloudsql.instances.get

# Use IAM database authentication
gcloud sql instances patch $INSTANCE_NAME \
    --database-flags=cloudsql.iam_authentication=on

# Create IAM user
gcloud sql users create orochi_iam_user \
    --instance=$INSTANCE_NAME \
    --type=CLOUD_IAM_USER

# Grant connect permission
gcloud projects add-iam-policy-binding $PROJECT_ID \
    --member="user:orochi_iam_user@example.com" \
    --role="roles/cloudsql.instanceUser"
```

### 9.4 Secret Management

```bash
# Store database credentials in Secret Manager
gcloud secrets create orochi-db-credentials \
    --replication-policy="automatic"

gcloud secrets versions add orochi-db-credentials --data-file=- << EOF
{
  "host": "10.0.0.5",
  "port": 5432,
  "database": "orochi",
  "username": "orochi_app",
  "password": "secure_password_here"
}
EOF

# Grant access to service account
gcloud secrets add-iam-policy-binding orochi-db-credentials \
    --member="serviceAccount:$SA_EMAIL" \
    --role="roles/secretmanager.secretAccessor"

# Access from application
gcloud secrets versions access latest --secret=orochi-db-credentials
```

---

## 10. Troubleshooting

### 10.1 Common Issues

**Connection Issues**

```bash
# Check Cloud SQL instance status
gcloud sql instances describe $INSTANCE_NAME --format="value(state)"

# Verify VPC peering
gcloud services vpc-peerings list --network=$VPC_NAME

# Test connectivity from Compute Engine
gcloud compute ssh $INSTANCE_NAME --zone=$ZONE --tunnel-through-iap -- \
    "pg_isready -h $PRIVATE_IP -p 5432"

# Check firewall rules
gcloud compute firewall-rules list --filter="network:$VPC_NAME"
```

**Performance Issues**

```bash
# Check instance metrics
gcloud monitoring metrics list --filter="resource.type=cloudsql_database"

# View slow query log
gcloud logging read 'resource.type="cloudsql_database" AND textPayload=~"duration"' \
    --limit=100

# Check connection count
gcloud sql operations describe OPERATION_ID --format="value(status)"
```

**Replication Issues**

```bash
# Check replica status
gcloud sql instances describe orochi-replica-1 \
    --format="table(name,state,replicaConfiguration)"

# View replication lag
gcloud monitoring read \
    'metric.type="cloudsql.googleapis.com/database/replication/replica_lag"' \
    --start-time=$(date -u -d '1 hour ago' +%Y-%m-%dT%H:%M:%SZ)

# Promote replica (DR scenario)
gcloud sql instances promote-replica orochi-replica-1
```

### 10.2 Diagnostic Queries

```sql
-- Check Orochi extension status
SELECT * FROM pg_extension WHERE extname = 'orochi';

-- View distributed tables
SELECT * FROM orochi_distributed_tables;

-- Check shard distribution
SELECT shard_id, node_id, row_count, size_bytes
FROM orochi_shard_stats
ORDER BY shard_id;

-- Monitor chunk status
SELECT chunk_id, start_time, end_time, tier, compressed
FROM orochi_chunk_info
WHERE table_name = 'sensor_data'
ORDER BY start_time DESC
LIMIT 20;

-- Check tiered storage usage
SELECT tier, count(*) as chunks,
       pg_size_pretty(sum(size_bytes)) as total_size
FROM orochi_tier_status
GROUP BY tier;

-- View active queries
SELECT pid, now() - pg_stat_activity.query_start AS duration, query
FROM pg_stat_activity
WHERE (now() - pg_stat_activity.query_start) > interval '5 seconds'
  AND state != 'idle'
ORDER BY duration DESC;

-- Check for locks
SELECT blocked.pid AS blocked_pid,
       blocked.query AS blocked_query,
       blocking.pid AS blocking_pid,
       blocking.query AS blocking_query
FROM pg_stat_activity blocked
JOIN pg_locks blocked_locks ON blocked.pid = blocked_locks.pid
JOIN pg_locks blocking_locks ON blocked_locks.locktype = blocking_locks.locktype
  AND blocked_locks.database IS NOT DISTINCT FROM blocking_locks.database
  AND blocked_locks.relation IS NOT DISTINCT FROM blocking_locks.relation
  AND blocked_locks.page IS NOT DISTINCT FROM blocking_locks.page
  AND blocked_locks.tuple IS NOT DISTINCT FROM blocking_locks.tuple
  AND blocked_locks.transactionid IS NOT DISTINCT FROM blocking_locks.transactionid
  AND blocked_locks.classid IS NOT DISTINCT FROM blocking_locks.classid
  AND blocked_locks.objid IS NOT DISTINCT FROM blocking_locks.objid
  AND blocked_locks.objsubid IS NOT DISTINCT FROM blocking_locks.objsubid
  AND blocked_locks.pid != blocking_locks.pid
JOIN pg_stat_activity blocking ON blocking_locks.pid = blocking.pid
WHERE NOT blocked_locks.granted;
```

### 10.3 Support Resources

- **Google Cloud Support**: https://cloud.google.com/support
- **Cloud SQL Documentation**: https://cloud.google.com/sql/docs/postgres
- **GKE Documentation**: https://cloud.google.com/kubernetes-engine/docs
- **AlloyDB Documentation**: https://cloud.google.com/alloydb/docs
- **Orochi DB Issues**: https://github.com/your-org/orochi-db/issues

---

## Appendix A: Quick Reference Commands

```bash
# Cloud SQL
gcloud sql instances list
gcloud sql instances describe $INSTANCE_NAME
gcloud sql connect $INSTANCE_NAME --user=postgres

# Compute Engine
gcloud compute instances list
gcloud compute ssh $INSTANCE_NAME --zone=$ZONE --tunnel-through-iap

# GKE
gcloud container clusters get-credentials $CLUSTER_NAME --zone=$ZONE
kubectl get pods -n orochi-db
kubectl logs -f deployment/orochi-db -n orochi-db

# Storage
gsutil ls gs://${BUCKET_PREFIX}-cold/
gsutil du -s gs://${BUCKET_PREFIX}-cold/

# Monitoring
gcloud monitoring dashboards list
gcloud logging read 'resource.type="cloudsql_database"' --limit=50

# Secrets
gcloud secrets list
gcloud secrets versions access latest --secret=orochi-db-credentials
```

---

## Appendix B: Cost Optimization

| Component | Cost Optimization Strategy |
|-----------|---------------------------|
| Cloud SQL | Use committed use discounts, rightsize instances |
| Compute Engine | Use preemptible VMs for replicas, sustained use discounts |
| GKE | Enable cluster autoscaler, use spot VMs for non-critical workloads |
| Cloud Storage | Use lifecycle policies, appropriate storage classes |
| Network | Use Private Google Access, minimize egress |

```bash
# View cost recommendations
gcloud recommender recommendations list \
    --project=$PROJECT_ID \
    --location=$REGION \
    --recommender=google.cloudsql.instance.CostRecommender

# Export billing to BigQuery for analysis
gcloud billing export bigquery enable \
    --billing-account=YOUR_BILLING_ACCOUNT \
    --dataset=billing_export
```

---

**Document Version**: 1.0.0
**Last Updated**: January 2026
**Maintainer**: Orochi DB Team
