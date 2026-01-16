# Orochi DB - AWS Deployment Guide

A comprehensive guide for deploying Orochi DB on Amazon Web Services (AWS), covering RDS, EC2, EKS, Aurora, and cloud-native integrations.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [RDS PostgreSQL Setup](#rds-postgresql-setup)
3. [EC2 Deployment](#ec2-deployment)
4. [EKS Deployment](#eks-deployment)
5. [S3 Integration for Tiered Storage](#s3-integration-for-tiered-storage)
6. [Aurora PostgreSQL](#aurora-postgresql)
7. [CloudFormation Templates](#cloudformation-templates)
8. [Monitoring and Observability](#monitoring-and-observability)
9. [Security Best Practices](#security-best-practices)
10. [Cost Optimization](#cost-optimization)
11. [Disaster Recovery](#disaster-recovery)

---

## Prerequisites

### AWS CLI Setup

Install and configure the AWS CLI:

```bash
# Install AWS CLI v2
curl "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o "awscliv2.zip"
unzip awscliv2.zip
sudo ./aws/install

# Configure credentials
aws configure
# AWS Access Key ID: AKIA...
# AWS Secret Access Key: ...
# Default region name: us-east-1
# Default output format: json

# Verify configuration
aws sts get-caller-identity
```

### Required IAM Permissions

Create an IAM policy with the following permissions:

```json
{
    "Version": "2012-10-17",
    "PolicyName": "OrochiDBDeployment",
    "Statement": [
        {
            "Sid": "EC2Permissions",
            "Effect": "Allow",
            "Action": [
                "ec2:RunInstances",
                "ec2:TerminateInstances",
                "ec2:DescribeInstances",
                "ec2:CreateVolume",
                "ec2:AttachVolume",
                "ec2:DetachVolume",
                "ec2:DeleteVolume",
                "ec2:DescribeVolumes",
                "ec2:CreateSecurityGroup",
                "ec2:AuthorizeSecurityGroupIngress",
                "ec2:DescribeSecurityGroups",
                "ec2:CreateKeyPair",
                "ec2:DescribeKeyPairs",
                "ec2:DescribeSubnets",
                "ec2:DescribeVpcs",
                "ec2:CreateTags"
            ],
            "Resource": "*"
        },
        {
            "Sid": "RDSPermissions",
            "Effect": "Allow",
            "Action": [
                "rds:CreateDBInstance",
                "rds:DeleteDBInstance",
                "rds:DescribeDBInstances",
                "rds:ModifyDBInstance",
                "rds:CreateDBParameterGroup",
                "rds:ModifyDBParameterGroup",
                "rds:DescribeDBParameterGroups",
                "rds:CreateDBSubnetGroup",
                "rds:DescribeDBSubnetGroups",
                "rds:CreateDBCluster",
                "rds:DeleteDBCluster",
                "rds:DescribeDBClusters",
                "rds:CreateOptionGroup",
                "rds:ModifyOptionGroup"
            ],
            "Resource": "*"
        },
        {
            "Sid": "S3Permissions",
            "Effect": "Allow",
            "Action": [
                "s3:CreateBucket",
                "s3:DeleteBucket",
                "s3:PutObject",
                "s3:GetObject",
                "s3:DeleteObject",
                "s3:ListBucket",
                "s3:PutBucketPolicy",
                "s3:GetBucketPolicy",
                "s3:PutBucketVersioning",
                "s3:PutLifecycleConfiguration",
                "s3:PutBucketEncryption",
                "s3:PutReplicationConfiguration"
            ],
            "Resource": [
                "arn:aws:s3:::orochi-*",
                "arn:aws:s3:::orochi-*/*"
            ]
        },
        {
            "Sid": "EKSPermissions",
            "Effect": "Allow",
            "Action": [
                "eks:CreateCluster",
                "eks:DeleteCluster",
                "eks:DescribeCluster",
                "eks:ListClusters",
                "eks:CreateNodegroup",
                "eks:DeleteNodegroup",
                "eks:DescribeNodegroup",
                "eks:UpdateNodegroupConfig"
            ],
            "Resource": "*"
        },
        {
            "Sid": "CloudWatchPermissions",
            "Effect": "Allow",
            "Action": [
                "cloudwatch:PutMetricData",
                "cloudwatch:GetMetricData",
                "cloudwatch:PutMetricAlarm",
                "cloudwatch:DescribeAlarms",
                "logs:CreateLogGroup",
                "logs:CreateLogStream",
                "logs:PutLogEvents",
                "logs:DescribeLogGroups"
            ],
            "Resource": "*"
        },
        {
            "Sid": "SecretsManagerPermissions",
            "Effect": "Allow",
            "Action": [
                "secretsmanager:CreateSecret",
                "secretsmanager:GetSecretValue",
                "secretsmanager:UpdateSecret",
                "secretsmanager:DeleteSecret"
            ],
            "Resource": "arn:aws:secretsmanager:*:*:secret:orochi-*"
        },
        {
            "Sid": "CloudFormationPermissions",
            "Effect": "Allow",
            "Action": [
                "cloudformation:CreateStack",
                "cloudformation:UpdateStack",
                "cloudformation:DeleteStack",
                "cloudformation:DescribeStacks",
                "cloudformation:DescribeStackEvents"
            ],
            "Resource": "*"
        }
    ]
}
```

### VPC and Security Group Requirements

#### VPC Configuration

Create a VPC with the following specifications:

```bash
# Create VPC
aws ec2 create-vpc \
    --cidr-block 10.0.0.0/16 \
    --tag-specifications 'ResourceType=vpc,Tags=[{Key=Name,Value=orochi-vpc}]'

# Enable DNS hostnames
aws ec2 modify-vpc-attribute \
    --vpc-id vpc-xxxxxxxxx \
    --enable-dns-hostnames

# Create subnets (at least 2 AZs for Multi-AZ deployments)
aws ec2 create-subnet \
    --vpc-id vpc-xxxxxxxxx \
    --cidr-block 10.0.1.0/24 \
    --availability-zone us-east-1a \
    --tag-specifications 'ResourceType=subnet,Tags=[{Key=Name,Value=orochi-private-1a}]'

aws ec2 create-subnet \
    --vpc-id vpc-xxxxxxxxx \
    --cidr-block 10.0.2.0/24 \
    --availability-zone us-east-1b \
    --tag-specifications 'ResourceType=subnet,Tags=[{Key=Name,Value=orochi-private-1b}]'

aws ec2 create-subnet \
    --vpc-id vpc-xxxxxxxxx \
    --cidr-block 10.0.101.0/24 \
    --availability-zone us-east-1a \
    --tag-specifications 'ResourceType=subnet,Tags=[{Key=Name,Value=orochi-public-1a}]'
```

#### Security Group for PostgreSQL

```bash
# Create security group
aws ec2 create-security-group \
    --group-name orochi-postgres-sg \
    --description "Security group for Orochi DB PostgreSQL" \
    --vpc-id vpc-xxxxxxxxx

# Allow PostgreSQL from within VPC
aws ec2 authorize-security-group-ingress \
    --group-id sg-xxxxxxxxx \
    --protocol tcp \
    --port 5432 \
    --cidr 10.0.0.0/16

# Allow SSH for management (optional, restrict to specific IPs)
aws ec2 authorize-security-group-ingress \
    --group-id sg-xxxxxxxxx \
    --protocol tcp \
    --port 22 \
    --cidr YOUR_IP/32
```

---

## RDS PostgreSQL Setup

### Create RDS DB Subnet Group

```bash
aws rds create-db-subnet-group \
    --db-subnet-group-name orochi-db-subnet \
    --db-subnet-group-description "Subnet group for Orochi DB" \
    --subnet-ids subnet-xxxxxxxxx subnet-yyyyyyyyy
```

### Create Custom Parameter Group

Orochi DB requires specific PostgreSQL settings. Create a custom parameter group:

```bash
# For PostgreSQL 16
aws rds create-db-parameter-group \
    --db-parameter-group-name orochi-pg16-params \
    --db-parameter-group-family postgres16 \
    --description "Orochi DB parameters for PostgreSQL 16"

# For PostgreSQL 17
aws rds create-db-parameter-group \
    --db-parameter-group-name orochi-pg17-params \
    --db-parameter-group-family postgres17 \
    --description "Orochi DB parameters for PostgreSQL 17"
```

Configure required parameters:

```bash
aws rds modify-db-parameter-group \
    --db-parameter-group-name orochi-pg16-params \
    --parameters \
        "ParameterName=shared_preload_libraries,ParameterValue=orochi,ApplyMethod=pending-reboot" \
        "ParameterName=max_worker_processes,ParameterValue=16,ApplyMethod=pending-reboot" \
        "ParameterName=max_parallel_workers_per_gather,ParameterValue=4,ApplyMethod=immediate" \
        "ParameterName=max_parallel_workers,ParameterValue=8,ApplyMethod=pending-reboot" \
        "ParameterName=max_parallel_maintenance_workers,ParameterValue=4,ApplyMethod=immediate" \
        "ParameterName=work_mem,ParameterValue=256MB,ApplyMethod=immediate" \
        "ParameterName=maintenance_work_mem,ParameterValue=1GB,ApplyMethod=immediate" \
        "ParameterName=effective_cache_size,ParameterValue=24GB,ApplyMethod=immediate" \
        "ParameterName=shared_buffers,ParameterValue={DBInstanceClassMemory/4},ApplyMethod=pending-reboot" \
        "ParameterName=random_page_cost,ParameterValue=1.1,ApplyMethod=immediate" \
        "ParameterName=effective_io_concurrency,ParameterValue=200,ApplyMethod=immediate" \
        "ParameterName=checkpoint_completion_target,ParameterValue=0.9,ApplyMethod=immediate" \
        "ParameterName=wal_buffers,ParameterValue=64MB,ApplyMethod=pending-reboot" \
        "ParameterName=default_statistics_target,ParameterValue=500,ApplyMethod=immediate" \
        "ParameterName=log_statement,ParameterValue=ddl,ApplyMethod=immediate" \
        "ParameterName=log_min_duration_statement,ParameterValue=1000,ApplyMethod=immediate"
```

### Create RDS Instance

#### Single-AZ Development Instance

```bash
aws rds create-db-instance \
    --db-instance-identifier orochi-dev \
    --db-instance-class db.r6g.xlarge \
    --engine postgres \
    --engine-version 16.4 \
    --master-username orochi_admin \
    --master-user-password "$(aws secretsmanager get-random-password --password-length 32 --query RandomPassword --output text)" \
    --allocated-storage 100 \
    --storage-type gp3 \
    --storage-throughput 125 \
    --iops 3000 \
    --db-subnet-group-name orochi-db-subnet \
    --vpc-security-group-ids sg-xxxxxxxxx \
    --db-parameter-group-name orochi-pg16-params \
    --backup-retention-period 7 \
    --preferred-backup-window "03:00-04:00" \
    --preferred-maintenance-window "sun:04:00-sun:05:00" \
    --auto-minor-version-upgrade \
    --publicly-accessible false \
    --enable-performance-insights \
    --performance-insights-retention-period 7 \
    --monitoring-interval 60 \
    --monitoring-role-arn arn:aws:iam::ACCOUNT_ID:role/rds-monitoring-role \
    --tags Key=Environment,Value=development Key=Application,Value=orochi
```

#### Multi-AZ Production Instance

```bash
aws rds create-db-instance \
    --db-instance-identifier orochi-prod \
    --db-instance-class db.r6g.4xlarge \
    --engine postgres \
    --engine-version 16.4 \
    --master-username orochi_admin \
    --master-user-password "SECURE_PASSWORD_HERE" \
    --allocated-storage 500 \
    --max-allocated-storage 2000 \
    --storage-type gp3 \
    --storage-throughput 500 \
    --iops 12000 \
    --db-subnet-group-name orochi-db-subnet \
    --vpc-security-group-ids sg-xxxxxxxxx \
    --db-parameter-group-name orochi-pg16-params \
    --multi-az \
    --backup-retention-period 35 \
    --preferred-backup-window "03:00-04:00" \
    --preferred-maintenance-window "sun:04:00-sun:05:00" \
    --auto-minor-version-upgrade \
    --publicly-accessible false \
    --storage-encrypted \
    --kms-key-id alias/aws/rds \
    --enable-performance-insights \
    --performance-insights-retention-period 731 \
    --monitoring-interval 1 \
    --monitoring-role-arn arn:aws:iam::ACCOUNT_ID:role/rds-monitoring-role \
    --deletion-protection \
    --copy-tags-to-snapshot \
    --enable-cloudwatch-logs-exports postgresql upgrade \
    --tags Key=Environment,Value=production Key=Application,Value=orochi
```

### Create Read Replicas

```bash
# Create read replica in same region
aws rds create-db-instance-read-replica \
    --db-instance-identifier orochi-prod-read-1 \
    --source-db-instance-identifier orochi-prod \
    --db-instance-class db.r6g.2xlarge \
    --availability-zone us-east-1b \
    --publicly-accessible false \
    --storage-type gp3

# Create cross-region read replica for DR
aws rds create-db-instance-read-replica \
    --db-instance-identifier orochi-prod-dr \
    --source-db-instance-identifier arn:aws:rds:us-east-1:ACCOUNT_ID:db:orochi-prod \
    --db-instance-class db.r6g.2xlarge \
    --region us-west-2 \
    --availability-zone us-west-2a \
    --publicly-accessible false \
    --storage-type gp3
```

### Install Orochi Extension on RDS

Note: RDS does not support custom extensions by default. For Orochi DB, use one of these approaches:

1. **Self-managed EC2** (recommended for full Orochi functionality)
2. **Aurora with custom extensions** (limited support)
3. **RDS Custom for PostgreSQL** (full OS access)

For RDS Custom:

```bash
# Create RDS Custom instance
aws rds create-db-instance \
    --db-instance-identifier orochi-custom \
    --db-instance-class db.r6g.xlarge \
    --engine custom-postgres-16 \
    --engine-version 16.4-cev1 \
    --custom-iam-instance-profile AWSRDSCustomInstanceProfile \
    --kms-key-id alias/aws/rds \
    --allocated-storage 100 \
    --db-subnet-group-name orochi-db-subnet \
    --vpc-security-group-ids sg-xxxxxxxxx
```

After RDS Custom instance is running, SSH to install Orochi:

```bash
# SSH to RDS Custom instance
ssh -i key.pem ec2-user@rds-custom-instance

# Install Orochi dependencies
sudo yum install -y lz4-devel libzstd-devel curl-devel

# Build and install Orochi
cd /tmp
git clone https://github.com/your-org/orochi-db.git
cd orochi-db
make
sudo make install

# Update parameter group via RDS API (shared_preload_libraries)
```

---

## EC2 Deployment

EC2 deployment provides full control and is recommended for production HTAP workloads.

### Instance Type Recommendations

| Workload Type | Instance Family | Recommended Size | Use Case |
|---------------|-----------------|------------------|----------|
| OLTP-heavy | r6g, r6i | r6g.2xlarge - r6g.8xlarge | Memory-intensive transactional |
| OLAP-heavy | c6g, c6i | c6g.4xlarge - c6g.12xlarge | Compute-intensive analytics |
| Mixed HTAP | r6g, r6i | r6g.4xlarge - r6g.16xlarge | Balanced workloads |
| Vector/AI | g4dn, p4d | g4dn.xlarge - g4dn.12xlarge | GPU-accelerated vectors |
| Cost-optimized | t3, m6i | m6i.xlarge - m6i.4xlarge | Development/testing |

### Launch EC2 Instance

```bash
# Create key pair
aws ec2 create-key-pair \
    --key-name orochi-key \
    --query 'KeyMaterial' \
    --output text > orochi-key.pem
chmod 400 orochi-key.pem

# Launch instance
aws ec2 run-instances \
    --image-id ami-0c55b159cbfafe1f0 \
    --instance-type r6g.4xlarge \
    --key-name orochi-key \
    --security-group-ids sg-xxxxxxxxx \
    --subnet-id subnet-xxxxxxxxx \
    --block-device-mappings '[
        {
            "DeviceName": "/dev/xvda",
            "Ebs": {
                "VolumeSize": 50,
                "VolumeType": "gp3",
                "Encrypted": true
            }
        }
    ]' \
    --iam-instance-profile Name=OrochiEC2Role \
    --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=orochi-primary}]' \
    --user-data file://user-data.sh
```

### EBS Volume Configuration

#### Create Optimized EBS Volumes

```bash
# Data volume (gp3 for balanced performance)
aws ec2 create-volume \
    --availability-zone us-east-1a \
    --size 500 \
    --volume-type gp3 \
    --iops 16000 \
    --throughput 1000 \
    --encrypted \
    --tag-specifications 'ResourceType=volume,Tags=[{Key=Name,Value=orochi-data}]'

# WAL volume (io2 for consistent low latency)
aws ec2 create-volume \
    --availability-zone us-east-1a \
    --size 100 \
    --volume-type io2 \
    --iops 64000 \
    --encrypted \
    --tag-specifications 'ResourceType=volume,Tags=[{Key=Name,Value=orochi-wal}]'

# Attach volumes
aws ec2 attach-volume \
    --volume-id vol-xxxxxxxxx \
    --instance-id i-xxxxxxxxx \
    --device /dev/xvdb

aws ec2 attach-volume \
    --volume-id vol-yyyyyyyyy \
    --instance-id i-xxxxxxxxx \
    --device /dev/xvdc
```

#### Volume Setup on Instance

```bash
#!/bin/bash
# user-data.sh - EC2 user data script

# Update system
yum update -y

# Install dependencies
yum install -y \
    postgresql16-server \
    postgresql16-devel \
    lz4-devel \
    libzstd-devel \
    libcurl-devel \
    openssl-devel \
    librdkafka-devel \
    git \
    gcc \
    make

# Format and mount data volume
mkfs.xfs /dev/xvdb
mkdir -p /var/lib/postgresql/data
mount /dev/xvdb /var/lib/postgresql/data
echo '/dev/xvdb /var/lib/postgresql/data xfs defaults,noatime 0 2' >> /etc/fstab

# Format and mount WAL volume
mkfs.xfs /dev/xvdc
mkdir -p /var/lib/postgresql/wal
mount /dev/xvdc /var/lib/postgresql/wal
echo '/dev/xvdc /var/lib/postgresql/wal xfs defaults,noatime 0 2' >> /etc/fstab

# Set ownership
chown -R postgres:postgres /var/lib/postgresql

# Initialize PostgreSQL
su - postgres -c "initdb -D /var/lib/postgresql/data --waldir=/var/lib/postgresql/wal"

# Clone and build Orochi
cd /tmp
git clone https://github.com/your-org/orochi-db.git
cd orochi-db
make
make install

# Configure PostgreSQL
cat >> /var/lib/postgresql/data/postgresql.conf << 'EOF'
# Orochi DB Settings
shared_preload_libraries = 'orochi'

# Memory settings (adjust based on instance size)
shared_buffers = 8GB
effective_cache_size = 24GB
work_mem = 256MB
maintenance_work_mem = 2GB

# WAL settings
wal_level = replica
max_wal_senders = 10
wal_keep_size = 1GB
wal_buffers = 64MB

# Parallel query
max_worker_processes = 16
max_parallel_workers_per_gather = 4
max_parallel_workers = 8
max_parallel_maintenance_workers = 4

# Connection settings
listen_addresses = '*'
max_connections = 200

# Performance
random_page_cost = 1.1
effective_io_concurrency = 200
checkpoint_completion_target = 0.9

# Orochi specific
orochi.default_shard_count = 32
orochi.compression = 'zstd'
orochi.s3_region = 'us-east-1'
EOF

# Configure pg_hba.conf
cat >> /var/lib/postgresql/data/pg_hba.conf << 'EOF'
# Allow connections from VPC
host    all    all    10.0.0.0/16    scram-sha-256
EOF

# Start PostgreSQL
systemctl enable postgresql
systemctl start postgresql

# Create extension
su - postgres -c "psql -c 'CREATE EXTENSION orochi;'"
```

### Production-Ready postgresql.conf

```ini
# /var/lib/postgresql/data/postgresql.conf
# Orochi DB Production Configuration

#------------------------------------------------------------------------------
# CONNECTIONS AND AUTHENTICATION
#------------------------------------------------------------------------------
listen_addresses = '*'
port = 5432
max_connections = 500
superuser_reserved_connections = 5

#------------------------------------------------------------------------------
# RESOURCE USAGE
#------------------------------------------------------------------------------
# Memory (for r6g.4xlarge with 128GB RAM)
shared_buffers = 32GB
huge_pages = try
effective_cache_size = 96GB
work_mem = 512MB
maintenance_work_mem = 4GB
autovacuum_work_mem = 2GB

# Disk
temp_buffers = 128MB
temp_file_limit = 100GB

#------------------------------------------------------------------------------
# WRITE-AHEAD LOG
#------------------------------------------------------------------------------
wal_level = replica
fsync = on
synchronous_commit = on
wal_sync_method = fdatasync
full_page_writes = on
wal_compression = zstd
wal_buffers = 128MB
checkpoint_timeout = 15min
checkpoint_completion_target = 0.9
max_wal_size = 8GB
min_wal_size = 2GB

#------------------------------------------------------------------------------
# REPLICATION
#------------------------------------------------------------------------------
max_wal_senders = 10
wal_keep_size = 2GB
max_replication_slots = 10
hot_standby = on

#------------------------------------------------------------------------------
# QUERY TUNING
#------------------------------------------------------------------------------
random_page_cost = 1.1
effective_io_concurrency = 200
default_statistics_target = 500
jit = on

#------------------------------------------------------------------------------
# PARALLEL QUERY
#------------------------------------------------------------------------------
max_worker_processes = 16
max_parallel_workers_per_gather = 4
max_parallel_workers = 12
max_parallel_maintenance_workers = 4
parallel_leader_participation = on

#------------------------------------------------------------------------------
# LOGGING
#------------------------------------------------------------------------------
logging_collector = on
log_directory = 'pg_log'
log_filename = 'postgresql-%Y-%m-%d_%H%M%S.log'
log_rotation_age = 1d
log_rotation_size = 100MB
log_min_duration_statement = 500
log_checkpoints = on
log_connections = on
log_disconnections = on
log_lock_waits = on
log_statement = 'ddl'
log_temp_files = 0

#------------------------------------------------------------------------------
# AUTOVACUUM
#------------------------------------------------------------------------------
autovacuum = on
autovacuum_max_workers = 4
autovacuum_naptime = 30s
autovacuum_vacuum_threshold = 50
autovacuum_vacuum_scale_factor = 0.05
autovacuum_analyze_threshold = 50
autovacuum_analyze_scale_factor = 0.05
autovacuum_vacuum_cost_delay = 2ms
autovacuum_vacuum_cost_limit = 1000

#------------------------------------------------------------------------------
# OROCHI DB EXTENSION
#------------------------------------------------------------------------------
shared_preload_libraries = 'orochi'

# Sharding defaults
orochi.default_shard_count = 64
orochi.enable_distributed_transactions = on

# Columnar storage
orochi.compression = 'zstd'
orochi.compression_level = 3
orochi.columnar_stripe_row_count = 150000
orochi.columnar_chunk_row_count = 10000

# Tiered storage
orochi.enable_tiering = on
orochi.hot_to_warm_after = '7 days'
orochi.warm_to_cold_after = '30 days'
orochi.tiering_check_interval = '1 hour'

# S3 configuration (use IAM roles, not keys)
orochi.s3_region = 'us-east-1'
orochi.s3_bucket = 'orochi-tiered-storage-prod'
orochi.s3_endpoint = 'https://s3.us-east-1.amazonaws.com'
orochi.s3_use_iam_role = on

# Vector operations
orochi.enable_simd = on
orochi.vector_default_dimensions = 1536

# Background workers
orochi.background_workers = 4
orochi.maintenance_workers = 2
```

---

## EKS Deployment

### Create EKS Cluster

```bash
# Install eksctl
curl --silent --location "https://github.com/weaveworks/eksctl/releases/latest/download/eksctl_$(uname -s)_amd64.tar.gz" | tar xz -C /tmp
sudo mv /tmp/eksctl /usr/local/bin

# Create cluster
eksctl create cluster \
    --name orochi-cluster \
    --version 1.29 \
    --region us-east-1 \
    --nodegroup-name orochi-workers \
    --node-type r6g.2xlarge \
    --nodes 3 \
    --nodes-min 3 \
    --nodes-max 10 \
    --managed \
    --asg-access \
    --with-oidc \
    --ssh-access \
    --ssh-public-key orochi-key \
    --vpc-private-subnets subnet-xxxxxxxxx,subnet-yyyyyyyyy \
    --vpc-public-subnets subnet-zzzzzzzzz,subnet-wwwwwwwww
```

### Install EBS CSI Driver

```bash
# Create IAM service account for EBS CSI
eksctl create iamserviceaccount \
    --name ebs-csi-controller-sa \
    --namespace kube-system \
    --cluster orochi-cluster \
    --attach-policy-arn arn:aws:iam::aws:policy/service-role/AmazonEBSCSIDriverPolicy \
    --approve \
    --override-existing-serviceaccounts

# Install EBS CSI driver
eksctl create addon \
    --name aws-ebs-csi-driver \
    --cluster orochi-cluster \
    --service-account-role-arn arn:aws:iam::ACCOUNT_ID:role/AmazonEKS_EBS_CSI_DriverRole \
    --force
```

### Kubernetes Manifests

#### Namespace and ConfigMap

```yaml
# orochi-namespace.yaml
apiVersion: v1
kind: Namespace
metadata:
  name: orochi
  labels:
    name: orochi
---
# orochi-config.yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: orochi-config
  namespace: orochi
data:
  postgresql.conf: |
    # Connection settings
    listen_addresses = '*'
    port = 5432
    max_connections = 200

    # Memory
    shared_buffers = 4GB
    effective_cache_size = 12GB
    work_mem = 256MB
    maintenance_work_mem = 1GB

    # Parallel query
    max_worker_processes = 8
    max_parallel_workers_per_gather = 2
    max_parallel_workers = 4

    # WAL
    wal_level = replica
    max_wal_senders = 5
    wal_buffers = 64MB

    # Orochi
    shared_preload_libraries = 'orochi'
    orochi.default_shard_count = 32
    orochi.compression = 'zstd'
    orochi.s3_region = 'us-east-1'
    orochi.s3_bucket = 'orochi-k8s-tiered'
    orochi.s3_use_iam_role = on

  pg_hba.conf: |
    local   all    all                    trust
    host    all    all    0.0.0.0/0       scram-sha-256
    host    replication    all    0.0.0.0/0    scram-sha-256
```

#### Storage Class and PVC

```yaml
# orochi-storage.yaml
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: orochi-gp3
provisioner: ebs.csi.aws.com
parameters:
  type: gp3
  iops: "16000"
  throughput: "1000"
  encrypted: "true"
reclaimPolicy: Retain
allowVolumeExpansion: true
volumeBindingMode: WaitForFirstConsumer
---
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: orochi-io2
provisioner: ebs.csi.aws.com
parameters:
  type: io2
  iops: "64000"
  encrypted: "true"
reclaimPolicy: Retain
allowVolumeExpansion: true
volumeBindingMode: WaitForFirstConsumer
```

#### StatefulSet

```yaml
# orochi-statefulset.yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: orochi
  namespace: orochi
spec:
  serviceName: orochi
  replicas: 3
  selector:
    matchLabels:
      app: orochi
  template:
    metadata:
      labels:
        app: orochi
    spec:
      serviceAccountName: orochi-sa
      terminationGracePeriodSeconds: 120
      initContainers:
        - name: init-permissions
          image: busybox:1.35
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
      containers:
        - name: postgres
          image: your-registry/orochi-db:1.0.0
          ports:
            - containerPort: 5432
              name: postgres
          env:
            - name: POSTGRES_USER
              valueFrom:
                secretKeyRef:
                  name: orochi-secrets
                  key: postgres-user
            - name: POSTGRES_PASSWORD
              valueFrom:
                secretKeyRef:
                  name: orochi-secrets
                  key: postgres-password
            - name: PGDATA
              value: /var/lib/postgresql/data/pgdata
          resources:
            requests:
              memory: "8Gi"
              cpu: "2"
            limits:
              memory: "16Gi"
              cpu: "4"
          volumeMounts:
            - name: data
              mountPath: /var/lib/postgresql/data
            - name: wal
              mountPath: /var/lib/postgresql/wal
            - name: config
              mountPath: /etc/postgresql/conf.d
          livenessProbe:
            exec:
              command:
                - pg_isready
                - -U
                - postgres
            initialDelaySeconds: 30
            periodSeconds: 10
            timeoutSeconds: 5
            failureThreshold: 6
          readinessProbe:
            exec:
              command:
                - pg_isready
                - -U
                - postgres
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 3
            failureThreshold: 3
      volumes:
        - name: config
          configMap:
            name: orochi-config
  volumeClaimTemplates:
    - metadata:
        name: data
      spec:
        accessModes: ["ReadWriteOnce"]
        storageClassName: orochi-gp3
        resources:
          requests:
            storage: 500Gi
    - metadata:
        name: wal
      spec:
        accessModes: ["ReadWriteOnce"]
        storageClassName: orochi-io2
        resources:
          requests:
            storage: 100Gi
```

#### Service

```yaml
# orochi-service.yaml
apiVersion: v1
kind: Service
metadata:
  name: orochi
  namespace: orochi
  labels:
    app: orochi
spec:
  type: ClusterIP
  ports:
    - port: 5432
      targetPort: 5432
      protocol: TCP
      name: postgres
  selector:
    app: orochi
---
apiVersion: v1
kind: Service
metadata:
  name: orochi-read
  namespace: orochi
  labels:
    app: orochi
spec:
  type: ClusterIP
  ports:
    - port: 5432
      targetPort: 5432
      protocol: TCP
      name: postgres
  selector:
    app: orochi
---
# Network Load Balancer for external access
apiVersion: v1
kind: Service
metadata:
  name: orochi-external
  namespace: orochi
  annotations:
    service.beta.kubernetes.io/aws-load-balancer-type: nlb
    service.beta.kubernetes.io/aws-load-balancer-internal: "true"
spec:
  type: LoadBalancer
  ports:
    - port: 5432
      targetPort: 5432
      protocol: TCP
  selector:
    app: orochi
```

#### IRSA for S3 Access

```yaml
# orochi-serviceaccount.yaml
apiVersion: v1
kind: ServiceAccount
metadata:
  name: orochi-sa
  namespace: orochi
  annotations:
    eks.amazonaws.com/role-arn: arn:aws:iam::ACCOUNT_ID:role/OrochiS3AccessRole
```

### Helm Chart Configuration

Create a Helm chart for simplified deployment:

```yaml
# Chart.yaml
apiVersion: v2
name: orochi-db
description: Orochi DB - HTAP PostgreSQL Extension
type: application
version: 1.0.0
appVersion: "1.0.0"

# values.yaml
replicaCount: 3

image:
  repository: your-registry/orochi-db
  tag: "1.0.0"
  pullPolicy: IfNotPresent

serviceAccount:
  create: true
  annotations:
    eks.amazonaws.com/role-arn: ""
  name: orochi-sa

resources:
  requests:
    memory: "8Gi"
    cpu: "2"
  limits:
    memory: "16Gi"
    cpu: "4"

persistence:
  data:
    enabled: true
    storageClass: orochi-gp3
    size: 500Gi
  wal:
    enabled: true
    storageClass: orochi-io2
    size: 100Gi

postgresql:
  maxConnections: 200
  sharedBuffers: "4GB"
  effectiveCacheSize: "12GB"
  workMem: "256MB"
  maintenanceWorkMem: "1GB"

orochi:
  shardCount: 32
  compression: zstd
  compressionLevel: 3
  enableTiering: true
  hotToWarmAfter: "7 days"
  warmToColdAfter: "30 days"
  s3:
    region: us-east-1
    bucket: orochi-tiered-storage
    useIAMRole: true

monitoring:
  enabled: true
  serviceMonitor:
    enabled: true
    interval: 30s

backup:
  enabled: true
  schedule: "0 2 * * *"
  retention: 7
  s3Bucket: orochi-backups
```

Deploy with Helm:

```bash
helm install orochi ./orochi-db \
    --namespace orochi \
    --create-namespace \
    --values custom-values.yaml
```

---

## S3 Integration for Tiered Storage

### Create S3 Buckets

```bash
# Create primary tiered storage bucket
aws s3api create-bucket \
    --bucket orochi-tiered-storage-prod \
    --region us-east-1

# Enable versioning
aws s3api put-bucket-versioning \
    --bucket orochi-tiered-storage-prod \
    --versioning-configuration Status=Enabled

# Enable server-side encryption
aws s3api put-bucket-encryption \
    --bucket orochi-tiered-storage-prod \
    --server-side-encryption-configuration '{
        "Rules": [{
            "ApplyServerSideEncryptionByDefault": {
                "SSEAlgorithm": "aws:kms",
                "KMSMasterKeyID": "alias/orochi-s3-key"
            },
            "BucketKeyEnabled": true
        }]
    }'

# Block public access
aws s3api put-public-access-block \
    --bucket orochi-tiered-storage-prod \
    --public-access-block-configuration '{
        "BlockPublicAcls": true,
        "IgnorePublicAcls": true,
        "BlockPublicPolicy": true,
        "RestrictPublicBuckets": true
    }'
```

### Lifecycle Policies

```bash
# Configure lifecycle policy for automatic Glacier transition
aws s3api put-bucket-lifecycle-configuration \
    --bucket orochi-tiered-storage-prod \
    --lifecycle-configuration '{
        "Rules": [
            {
                "ID": "MoveToGlacierAfter90Days",
                "Status": "Enabled",
                "Filter": {
                    "Prefix": "frozen/"
                },
                "Transitions": [
                    {
                        "Days": 0,
                        "StorageClass": "GLACIER"
                    }
                ]
            },
            {
                "ID": "IntelligentTieringForCold",
                "Status": "Enabled",
                "Filter": {
                    "Prefix": "cold/"
                },
                "Transitions": [
                    {
                        "Days": 0,
                        "StorageClass": "INTELLIGENT_TIERING"
                    }
                ]
            },
            {
                "ID": "ExpireOldVersions",
                "Status": "Enabled",
                "Filter": {},
                "NoncurrentVersionExpiration": {
                    "NoncurrentDays": 30
                }
            }
        ]
    }'
```

### IAM Role for S3 Access

```json
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Sid": "OrochiS3Access",
            "Effect": "Allow",
            "Action": [
                "s3:GetObject",
                "s3:PutObject",
                "s3:DeleteObject",
                "s3:ListBucket",
                "s3:GetObjectVersion",
                "s3:GetBucketLocation"
            ],
            "Resource": [
                "arn:aws:s3:::orochi-tiered-storage-prod",
                "arn:aws:s3:::orochi-tiered-storage-prod/*"
            ]
        },
        {
            "Sid": "OrochiGlacierRestore",
            "Effect": "Allow",
            "Action": [
                "s3:RestoreObject"
            ],
            "Resource": [
                "arn:aws:s3:::orochi-tiered-storage-prod/*"
            ]
        },
        {
            "Sid": "OrochiKMSAccess",
            "Effect": "Allow",
            "Action": [
                "kms:Decrypt",
                "kms:GenerateDataKey"
            ],
            "Resource": [
                "arn:aws:kms:us-east-1:ACCOUNT_ID:key/KEY_ID"
            ]
        }
    ]
}
```

### Cross-Region Replication

```bash
# Create destination bucket in DR region
aws s3api create-bucket \
    --bucket orochi-tiered-storage-dr \
    --region us-west-2 \
    --create-bucket-configuration LocationConstraint=us-west-2

# Enable versioning on destination
aws s3api put-bucket-versioning \
    --bucket orochi-tiered-storage-dr \
    --region us-west-2 \
    --versioning-configuration Status=Enabled

# Create replication role
cat > s3-replication-role.json << 'EOF'
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Principal": {
                "Service": "s3.amazonaws.com"
            },
            "Action": "sts:AssumeRole"
        }
    ]
}
EOF

aws iam create-role \
    --role-name OrochiS3ReplicationRole \
    --assume-role-policy-document file://s3-replication-role.json

# Configure replication
aws s3api put-bucket-replication \
    --bucket orochi-tiered-storage-prod \
    --replication-configuration '{
        "Role": "arn:aws:iam::ACCOUNT_ID:role/OrochiS3ReplicationRole",
        "Rules": [
            {
                "ID": "ReplicateAll",
                "Status": "Enabled",
                "Priority": 1,
                "Filter": {},
                "Destination": {
                    "Bucket": "arn:aws:s3:::orochi-tiered-storage-dr",
                    "StorageClass": "STANDARD_IA",
                    "EncryptionConfiguration": {
                        "ReplicaKmsKeyID": "arn:aws:kms:us-west-2:ACCOUNT_ID:key/DR_KEY_ID"
                    }
                },
                "SourceSelectionCriteria": {
                    "SseKmsEncryptedObjects": {
                        "Status": "Enabled"
                    }
                },
                "DeleteMarkerReplication": {
                    "Status": "Enabled"
                }
            }
        ]
    }'
```

### Configure Orochi for S3

```sql
-- Configure S3 credentials (using IAM role is recommended)
ALTER SYSTEM SET orochi.s3_endpoint = 'https://s3.us-east-1.amazonaws.com';
ALTER SYSTEM SET orochi.s3_bucket = 'orochi-tiered-storage-prod';
ALTER SYSTEM SET orochi.s3_region = 'us-east-1';
ALTER SYSTEM SET orochi.s3_use_iam_role = on;

-- Alternative: explicit credentials (not recommended for production)
-- ALTER SYSTEM SET orochi.s3_access_key = 'AKIA...';
-- ALTER SYSTEM SET orochi.s3_secret_key = 'secret...';

SELECT pg_reload_conf();

-- Set up tiering policy for a table
SELECT set_tiering_policy(
    'events',
    hot_after => INTERVAL '1 day',
    warm_after => INTERVAL '7 days',
    cold_after => INTERVAL '30 days'
);

-- Verify S3 connectivity
SELECT orochi.test_s3_connection();

-- Check tiering status
SELECT
    chunk_id,
    hypertable,
    range_start,
    range_end,
    storage_tier,
    size_bytes,
    is_compressed
FROM orochi.chunks
WHERE hypertable = 'events'
ORDER BY range_start DESC;
```

---

## Aurora PostgreSQL

### Create Aurora Cluster

Aurora supports custom extensions through parameter groups with limited functionality.

```bash
# Create Aurora cluster parameter group
aws rds create-db-cluster-parameter-group \
    --db-cluster-parameter-group-name orochi-aurora-cluster-params \
    --db-parameter-group-family aurora-postgresql16 \
    --description "Orochi DB Aurora cluster parameters"

# Create Aurora instance parameter group
aws rds create-db-parameter-group \
    --db-parameter-group-name orochi-aurora-instance-params \
    --db-parameter-group-family aurora-postgresql16 \
    --description "Orochi DB Aurora instance parameters"

# Configure parameters
aws rds modify-db-cluster-parameter-group \
    --db-cluster-parameter-group-name orochi-aurora-cluster-params \
    --parameters \
        "ParameterName=shared_preload_libraries,ParameterValue=orochi,ApplyMethod=pending-reboot" \
        "ParameterName=max_worker_processes,ParameterValue=16,ApplyMethod=pending-reboot"

# Create Aurora Serverless v2 cluster
aws rds create-db-cluster \
    --db-cluster-identifier orochi-aurora \
    --engine aurora-postgresql \
    --engine-version 16.4 \
    --master-username orochi_admin \
    --master-user-password "SECURE_PASSWORD" \
    --db-subnet-group-name orochi-db-subnet \
    --vpc-security-group-ids sg-xxxxxxxxx \
    --db-cluster-parameter-group-name orochi-aurora-cluster-params \
    --storage-encrypted \
    --kms-key-id alias/aws/rds \
    --enable-cloudwatch-logs-exports postgresql \
    --deletion-protection \
    --serverless-v2-scaling-configuration MinCapacity=0.5,MaxCapacity=16 \
    --tags Key=Environment,Value=production Key=Application,Value=orochi

# Create primary instance
aws rds create-db-instance \
    --db-instance-identifier orochi-aurora-1 \
    --db-cluster-identifier orochi-aurora \
    --db-instance-class db.serverless \
    --engine aurora-postgresql \
    --db-parameter-group-name orochi-aurora-instance-params \
    --enable-performance-insights \
    --performance-insights-retention-period 7 \
    --monitoring-interval 1 \
    --monitoring-role-arn arn:aws:iam::ACCOUNT_ID:role/rds-monitoring-role

# Create read replica
aws rds create-db-instance \
    --db-instance-identifier orochi-aurora-2 \
    --db-cluster-identifier orochi-aurora \
    --db-instance-class db.serverless \
    --engine aurora-postgresql \
    --db-parameter-group-name orochi-aurora-instance-params \
    --enable-performance-insights \
    --performance-insights-retention-period 7
```

### Aurora Global Database

```bash
# Create global database
aws rds create-global-cluster \
    --global-cluster-identifier orochi-global \
    --source-db-cluster-identifier arn:aws:rds:us-east-1:ACCOUNT_ID:cluster:orochi-aurora \
    --deletion-protection

# Add secondary region
aws rds create-db-cluster \
    --db-cluster-identifier orochi-aurora-dr \
    --engine aurora-postgresql \
    --engine-version 16.4 \
    --global-cluster-identifier orochi-global \
    --region us-west-2 \
    --db-subnet-group-name orochi-db-subnet-west \
    --vpc-security-group-ids sg-xxxxxxxxx \
    --storage-encrypted \
    --kms-key-id alias/aws/rds

# Add read replica in secondary region
aws rds create-db-instance \
    --db-instance-identifier orochi-aurora-dr-1 \
    --db-cluster-identifier orochi-aurora-dr \
    --db-instance-class db.r6g.xlarge \
    --engine aurora-postgresql \
    --region us-west-2
```

---

## CloudFormation Templates

### Quick Start Stack

```yaml
# orochi-quickstart.yaml
AWSTemplateFormatVersion: '2010-09-09'
Description: 'Orochi DB Quick Start - HTAP PostgreSQL Extension'

Parameters:
  Environment:
    Type: String
    Default: production
    AllowedValues:
      - development
      - staging
      - production

  DBInstanceClass:
    Type: String
    Default: db.r6g.xlarge
    AllowedValues:
      - db.r6g.large
      - db.r6g.xlarge
      - db.r6g.2xlarge
      - db.r6g.4xlarge

  DBAllocatedStorage:
    Type: Number
    Default: 100
    MinValue: 20
    MaxValue: 65536

  DBMasterUsername:
    Type: String
    Default: orochi_admin
    NoEcho: true

  DBMasterPassword:
    Type: String
    NoEcho: true
    MinLength: 16

  VpcId:
    Type: AWS::EC2::VPC::Id
    Description: VPC for Orochi DB deployment

  SubnetIds:
    Type: List<AWS::EC2::Subnet::Id>
    Description: Subnets for DB subnet group (minimum 2 AZs)

  MultiAZ:
    Type: String
    Default: 'true'
    AllowedValues:
      - 'true'
      - 'false'

Conditions:
  IsProduction: !Equals [!Ref Environment, production]
  EnableMultiAZ: !Equals [!Ref MultiAZ, 'true']

Resources:
  # Security Group
  OrochiSecurityGroup:
    Type: AWS::EC2::SecurityGroup
    Properties:
      GroupName: !Sub 'orochi-${Environment}-sg'
      GroupDescription: Security group for Orochi DB
      VpcId: !Ref VpcId
      SecurityGroupIngress:
        - IpProtocol: tcp
          FromPort: 5432
          ToPort: 5432
          CidrIp: 10.0.0.0/8
          Description: PostgreSQL from private networks
      Tags:
        - Key: Name
          Value: !Sub 'orochi-${Environment}-sg'
        - Key: Environment
          Value: !Ref Environment

  # DB Subnet Group
  OrochiDBSubnetGroup:
    Type: AWS::RDS::DBSubnetGroup
    Properties:
      DBSubnetGroupName: !Sub 'orochi-${Environment}-subnet-group'
      DBSubnetGroupDescription: Subnet group for Orochi DB
      SubnetIds: !Ref SubnetIds
      Tags:
        - Key: Environment
          Value: !Ref Environment

  # Parameter Group
  OrochiParameterGroup:
    Type: AWS::RDS::DBParameterGroup
    Properties:
      DBParameterGroupName: !Sub 'orochi-${Environment}-params'
      Description: Orochi DB parameter group
      Family: postgres16
      Parameters:
        shared_preload_libraries: orochi
        max_worker_processes: '16'
        max_parallel_workers_per_gather: '4'
        max_parallel_workers: '8'
        work_mem: '262144'
        maintenance_work_mem: '1048576'
        effective_cache_size: '25165824'
        random_page_cost: '1.1'
        effective_io_concurrency: '200'
        checkpoint_completion_target: '0.9'
        log_statement: ddl
        log_min_duration_statement: '1000'
      Tags:
        - Key: Environment
          Value: !Ref Environment

  # Secrets Manager Secret
  OrochiDBSecret:
    Type: AWS::SecretsManager::Secret
    Properties:
      Name: !Sub 'orochi-${Environment}-db-credentials'
      Description: Orochi DB master credentials
      SecretString: !Sub |
        {
          "username": "${DBMasterUsername}",
          "password": "${DBMasterPassword}",
          "engine": "postgres",
          "host": "${OrochiDBInstance.Endpoint.Address}",
          "port": 5432,
          "dbname": "orochi"
        }
      Tags:
        - Key: Environment
          Value: !Ref Environment

  # RDS Instance
  OrochiDBInstance:
    Type: AWS::RDS::DBInstance
    DeletionPolicy: Snapshot
    UpdateReplacePolicy: Snapshot
    Properties:
      DBInstanceIdentifier: !Sub 'orochi-${Environment}'
      DBInstanceClass: !Ref DBInstanceClass
      Engine: postgres
      EngineVersion: '16.4'
      MasterUsername: !Ref DBMasterUsername
      MasterUserPassword: !Ref DBMasterPassword
      AllocatedStorage: !Ref DBAllocatedStorage
      MaxAllocatedStorage: !If [IsProduction, 2000, 500]
      StorageType: gp3
      Iops: !If [IsProduction, 12000, 3000]
      StorageThroughput: !If [IsProduction, 500, 125]
      DBSubnetGroupName: !Ref OrochiDBSubnetGroup
      VPCSecurityGroups:
        - !Ref OrochiSecurityGroup
      DBParameterGroupName: !Ref OrochiParameterGroup
      MultiAZ: !If [EnableMultiAZ, true, false]
      PubliclyAccessible: false
      StorageEncrypted: true
      BackupRetentionPeriod: !If [IsProduction, 35, 7]
      PreferredBackupWindow: '03:00-04:00'
      PreferredMaintenanceWindow: 'sun:04:00-sun:05:00'
      AutoMinorVersionUpgrade: true
      DeletionProtection: !If [IsProduction, true, false]
      EnablePerformanceInsights: true
      PerformanceInsightsRetentionPeriod: !If [IsProduction, 731, 7]
      MonitoringInterval: !If [IsProduction, 1, 60]
      MonitoringRoleArn: !GetAtt RDSMonitoringRole.Arn
      EnableCloudwatchLogsExports:
        - postgresql
        - upgrade
      CopyTagsToSnapshot: true
      Tags:
        - Key: Name
          Value: !Sub 'orochi-${Environment}'
        - Key: Environment
          Value: !Ref Environment
        - Key: Application
          Value: orochi

  # Read Replica (Production only)
  OrochiReadReplica:
    Type: AWS::RDS::DBInstance
    Condition: IsProduction
    Properties:
      DBInstanceIdentifier: !Sub 'orochi-${Environment}-read-1'
      DBInstanceClass: !Ref DBInstanceClass
      SourceDBInstanceIdentifier: !Ref OrochiDBInstance
      StorageType: gp3
      PubliclyAccessible: false
      EnablePerformanceInsights: true
      PerformanceInsightsRetentionPeriod: 7
      Tags:
        - Key: Name
          Value: !Sub 'orochi-${Environment}-read-1'
        - Key: Environment
          Value: !Ref Environment

  # IAM Role for Enhanced Monitoring
  RDSMonitoringRole:
    Type: AWS::IAM::Role
    Properties:
      RoleName: !Sub 'orochi-${Environment}-rds-monitoring'
      AssumeRolePolicyDocument:
        Version: '2012-10-17'
        Statement:
          - Effect: Allow
            Principal:
              Service: monitoring.rds.amazonaws.com
            Action: sts:AssumeRole
      ManagedPolicyArns:
        - arn:aws:iam::aws:policy/service-role/AmazonRDSEnhancedMonitoringRole

  # S3 Bucket for Tiered Storage
  OrochiTieredStorageBucket:
    Type: AWS::S3::Bucket
    DeletionPolicy: Retain
    Properties:
      BucketName: !Sub 'orochi-${Environment}-tiered-${AWS::AccountId}'
      BucketEncryption:
        ServerSideEncryptionConfiguration:
          - ServerSideEncryptionByDefault:
              SSEAlgorithm: aws:kms
            BucketKeyEnabled: true
      VersioningConfiguration:
        Status: Enabled
      PublicAccessBlockConfiguration:
        BlockPublicAcls: true
        BlockPublicPolicy: true
        IgnorePublicAcls: true
        RestrictPublicBuckets: true
      LifecycleConfiguration:
        Rules:
          - Id: TransitionToIntelligentTiering
            Status: Enabled
            Prefix: cold/
            Transitions:
              - TransitionInDays: 0
                StorageClass: INTELLIGENT_TIERING
          - Id: TransitionToGlacier
            Status: Enabled
            Prefix: frozen/
            Transitions:
              - TransitionInDays: 0
                StorageClass: GLACIER
          - Id: ExpireOldVersions
            Status: Enabled
            NoncurrentVersionExpiration:
              NoncurrentDays: 30
      Tags:
        - Key: Environment
          Value: !Ref Environment
        - Key: Application
          Value: orochi

  # CloudWatch Alarms
  OrochiCPUAlarm:
    Type: AWS::CloudWatch::Alarm
    Properties:
      AlarmName: !Sub 'orochi-${Environment}-high-cpu'
      AlarmDescription: Orochi DB CPU utilization is high
      MetricName: CPUUtilization
      Namespace: AWS/RDS
      Statistic: Average
      Period: 300
      EvaluationPeriods: 3
      Threshold: 80
      ComparisonOperator: GreaterThanThreshold
      Dimensions:
        - Name: DBInstanceIdentifier
          Value: !Ref OrochiDBInstance

  OrochiStorageAlarm:
    Type: AWS::CloudWatch::Alarm
    Properties:
      AlarmName: !Sub 'orochi-${Environment}-low-storage'
      AlarmDescription: Orochi DB storage space is running low
      MetricName: FreeStorageSpace
      Namespace: AWS/RDS
      Statistic: Average
      Period: 300
      EvaluationPeriods: 1
      Threshold: 10737418240  # 10 GB
      ComparisonOperator: LessThanThreshold
      Dimensions:
        - Name: DBInstanceIdentifier
          Value: !Ref OrochiDBInstance

  OrochiConnectionsAlarm:
    Type: AWS::CloudWatch::Alarm
    Properties:
      AlarmName: !Sub 'orochi-${Environment}-high-connections'
      AlarmDescription: Orochi DB connections are high
      MetricName: DatabaseConnections
      Namespace: AWS/RDS
      Statistic: Average
      Period: 300
      EvaluationPeriods: 2
      Threshold: 180
      ComparisonOperator: GreaterThanThreshold
      Dimensions:
        - Name: DBInstanceIdentifier
          Value: !Ref OrochiDBInstance

Outputs:
  DBEndpoint:
    Description: Orochi DB endpoint
    Value: !GetAtt OrochiDBInstance.Endpoint.Address
    Export:
      Name: !Sub '${AWS::StackName}-DBEndpoint'

  DBPort:
    Description: Orochi DB port
    Value: !GetAtt OrochiDBInstance.Endpoint.Port
    Export:
      Name: !Sub '${AWS::StackName}-DBPort'

  ReadReplicaEndpoint:
    Description: Read replica endpoint
    Value: !If
      - IsProduction
      - !GetAtt OrochiReadReplica.Endpoint.Address
      - 'N/A'
    Export:
      Name: !Sub '${AWS::StackName}-ReadReplicaEndpoint'

  TieredStorageBucket:
    Description: S3 bucket for tiered storage
    Value: !Ref OrochiTieredStorageBucket
    Export:
      Name: !Sub '${AWS::StackName}-TieredStorageBucket'

  SecurityGroupId:
    Description: Security group ID
    Value: !Ref OrochiSecurityGroup
    Export:
      Name: !Sub '${AWS::StackName}-SecurityGroupId'

  SecretArn:
    Description: Secrets Manager secret ARN
    Value: !Ref OrochiDBSecret
    Export:
      Name: !Sub '${AWS::StackName}-SecretArn'
```

### Deploy Stack

```bash
# Validate template
aws cloudformation validate-template \
    --template-body file://orochi-quickstart.yaml

# Create stack
aws cloudformation create-stack \
    --stack-name orochi-production \
    --template-body file://orochi-quickstart.yaml \
    --parameters \
        ParameterKey=Environment,ParameterValue=production \
        ParameterKey=DBInstanceClass,ParameterValue=db.r6g.2xlarge \
        ParameterKey=DBAllocatedStorage,ParameterValue=500 \
        ParameterKey=DBMasterUsername,ParameterValue=orochi_admin \
        ParameterKey=DBMasterPassword,ParameterValue=YourSecurePassword123! \
        ParameterKey=VpcId,ParameterValue=vpc-xxxxxxxxx \
        ParameterKey=SubnetIds,ParameterValue="subnet-xxxxxxxxx,subnet-yyyyyyyyy" \
        ParameterKey=MultiAZ,ParameterValue=true \
    --capabilities CAPABILITY_NAMED_IAM \
    --tags Key=Application,Value=orochi Key=Environment,Value=production

# Monitor stack creation
aws cloudformation describe-stack-events \
    --stack-name orochi-production \
    --query 'StackEvents[*].[Timestamp,ResourceStatus,ResourceType,LogicalResourceId]' \
    --output table

# Get outputs
aws cloudformation describe-stacks \
    --stack-name orochi-production \
    --query 'Stacks[0].Outputs' \
    --output table
```

---

## Monitoring and Observability

### CloudWatch Metrics

#### Custom Orochi Metrics

Create a CloudWatch agent configuration for custom Orochi metrics:

```json
{
    "agent": {
        "metrics_collection_interval": 60,
        "run_as_user": "cwagent"
    },
    "metrics": {
        "namespace": "Orochi/Database",
        "metrics_collected": {
            "postgresql": {
                "metrics_collection_interval": 60,
                "resources": [
                    "*"
                ],
                "measurement": [
                    "sessions",
                    "transactions",
                    "blocks_read",
                    "blocks_hit",
                    "rows_inserted",
                    "rows_updated",
                    "rows_deleted"
                ]
            }
        },
        "append_dimensions": {
            "InstanceId": "${aws:InstanceId}",
            "Environment": "production"
        }
    },
    "logs": {
        "logs_collected": {
            "files": {
                "collect_list": [
                    {
                        "file_path": "/var/lib/postgresql/data/pg_log/postgresql-*.log",
                        "log_group_name": "/orochi/postgresql",
                        "log_stream_name": "{instance_id}/postgresql",
                        "timezone": "UTC"
                    }
                ]
            }
        }
    }
}
```

#### CloudWatch Dashboard

```json
{
    "widgets": [
        {
            "type": "metric",
            "x": 0,
            "y": 0,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "Database Connections",
                "metrics": [
                    ["AWS/RDS", "DatabaseConnections", "DBInstanceIdentifier", "orochi-prod"]
                ],
                "period": 60,
                "stat": "Average",
                "region": "us-east-1"
            }
        },
        {
            "type": "metric",
            "x": 12,
            "y": 0,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "CPU Utilization",
                "metrics": [
                    ["AWS/RDS", "CPUUtilization", "DBInstanceIdentifier", "orochi-prod"]
                ],
                "period": 60,
                "stat": "Average",
                "region": "us-east-1"
            }
        },
        {
            "type": "metric",
            "x": 0,
            "y": 6,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "Read/Write IOPS",
                "metrics": [
                    ["AWS/RDS", "ReadIOPS", "DBInstanceIdentifier", "orochi-prod"],
                    [".", "WriteIOPS", ".", "."]
                ],
                "period": 60,
                "stat": "Average",
                "region": "us-east-1"
            }
        },
        {
            "type": "metric",
            "x": 12,
            "y": 6,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "Free Storage Space",
                "metrics": [
                    ["AWS/RDS", "FreeStorageSpace", "DBInstanceIdentifier", "orochi-prod"]
                ],
                "period": 300,
                "stat": "Average",
                "region": "us-east-1"
            }
        },
        {
            "type": "metric",
            "x": 0,
            "y": 12,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "Read/Write Latency",
                "metrics": [
                    ["AWS/RDS", "ReadLatency", "DBInstanceIdentifier", "orochi-prod"],
                    [".", "WriteLatency", ".", "."]
                ],
                "period": 60,
                "stat": "Average",
                "region": "us-east-1"
            }
        },
        {
            "type": "metric",
            "x": 12,
            "y": 12,
            "width": 12,
            "height": 6,
            "properties": {
                "title": "Network Throughput",
                "metrics": [
                    ["AWS/RDS", "NetworkReceiveThroughput", "DBInstanceIdentifier", "orochi-prod"],
                    [".", "NetworkTransmitThroughput", ".", "."]
                ],
                "period": 60,
                "stat": "Average",
                "region": "us-east-1"
            }
        }
    ]
}
```

Create dashboard:

```bash
aws cloudwatch put-dashboard \
    --dashboard-name OrochiDB-Production \
    --dashboard-body file://dashboard.json
```

### RDS Performance Insights

Enable and configure Performance Insights:

```sql
-- Query Performance Insights data via API
-- Example: Get top SQL by wait time
aws pi get-resource-metrics \
    --service-type RDS \
    --identifier db-XXXXXXXXXXXXXXXXXXXXXXXXXX \
    --metric-queries '[
        {
            "Metric": "db.load.avg",
            "GroupBy": {
                "Group": "db.sql",
                "Limit": 10
            }
        }
    ]' \
    --start-time 2024-01-01T00:00:00Z \
    --end-time 2024-01-02T00:00:00Z \
    --period-in-seconds 3600
```

### Log Analysis

```bash
# Create CloudWatch Logs Insights query
aws logs start-query \
    --log-group-name /aws/rds/instance/orochi-prod/postgresql \
    --start-time $(date -d '1 hour ago' +%s) \
    --end-time $(date +%s) \
    --query-string '
        fields @timestamp, @message
        | filter @message like /ERROR|FATAL|PANIC/
        | sort @timestamp desc
        | limit 100
    '
```

---

## Security Best Practices

### Encryption

1. **At Rest**: Enable RDS encryption with AWS KMS
2. **In Transit**: Enforce SSL connections

```sql
-- Enforce SSL on PostgreSQL
ALTER SYSTEM SET ssl = on;
ALTER SYSTEM SET ssl_min_protocol_version = 'TLSv1.2';

-- Verify SSL certificate
SELECT ssl_is_used FROM pg_stat_ssl WHERE pid = pg_backend_pid();

-- Require SSL for specific users
ALTER USER app_user SET sslmode = 'require';
```

### Network Security

```bash
# Restrict security group to specific CIDR blocks
aws ec2 authorize-security-group-ingress \
    --group-id sg-xxxxxxxxx \
    --protocol tcp \
    --port 5432 \
    --source-group sg-application-sg

# Enable VPC Flow Logs
aws ec2 create-flow-logs \
    --resource-type VPC \
    --resource-ids vpc-xxxxxxxxx \
    --traffic-type ALL \
    --log-destination-type cloud-watch-logs \
    --log-group-name /vpc/orochi-flow-logs
```

### IAM Database Authentication

```bash
# Enable IAM authentication on RDS
aws rds modify-db-instance \
    --db-instance-identifier orochi-prod \
    --enable-iam-database-authentication \
    --apply-immediately

# Create database user for IAM
psql -h orochi-prod.xxxxxxxxx.us-east-1.rds.amazonaws.com -U orochi_admin -c "
CREATE USER iam_user WITH LOGIN;
GRANT rds_iam TO iam_user;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO iam_user;
"

# Connect using IAM authentication
PGPASSWORD=$(aws rds generate-db-auth-token \
    --hostname orochi-prod.xxxxxxxxx.us-east-1.rds.amazonaws.com \
    --port 5432 \
    --username iam_user \
    --region us-east-1) \
psql "host=orochi-prod.xxxxxxxxx.us-east-1.rds.amazonaws.com \
      dbname=orochi user=iam_user sslmode=require"
```

---

## Cost Optimization

### Reserved Instances

```bash
# Purchase Reserved Instance for production
aws rds purchase-reserved-db-instances-offering \
    --reserved-db-instances-offering-id offering-id \
    --reserved-db-instance-id orochi-prod-ri \
    --db-instance-count 1
```

### Instance Sizing Guidelines

| Workload | vCPU | Memory | Storage IOPS | Estimated Monthly Cost |
|----------|------|--------|--------------|------------------------|
| Dev/Test | 2 | 8GB | 3,000 | ~$150 |
| Small Production | 4 | 32GB | 6,000 | ~$500 |
| Medium Production | 8 | 64GB | 12,000 | ~$1,200 |
| Large Production | 16 | 128GB | 20,000 | ~$2,500 |
| Enterprise | 32+ | 256GB+ | 40,000+ | ~$5,000+ |

### Storage Optimization

```sql
-- Monitor storage usage
SELECT
    schemaname,
    relname,
    pg_size_pretty(pg_total_relation_size(relid)) AS total_size,
    pg_size_pretty(pg_relation_size(relid)) AS data_size,
    pg_size_pretty(pg_indexes_size(relid)) AS index_size
FROM pg_stat_user_tables
ORDER BY pg_total_relation_size(relid) DESC
LIMIT 20;

-- Identify candidates for tiering
SELECT
    chunk_id,
    hypertable,
    range_start,
    storage_tier,
    pg_size_pretty(size_bytes) AS size,
    last_accessed
FROM orochi.chunks
WHERE storage_tier = 'hot'
  AND last_accessed < NOW() - INTERVAL '7 days'
ORDER BY size_bytes DESC;
```

---

## Disaster Recovery

### Backup Strategy

```bash
# Enable automated backups with 35-day retention
aws rds modify-db-instance \
    --db-instance-identifier orochi-prod \
    --backup-retention-period 35 \
    --preferred-backup-window "03:00-04:00" \
    --apply-immediately

# Create manual snapshot before maintenance
aws rds create-db-snapshot \
    --db-instance-identifier orochi-prod \
    --db-snapshot-identifier orochi-prod-$(date +%Y%m%d)

# Copy snapshot to another region
aws rds copy-db-snapshot \
    --source-db-snapshot-identifier arn:aws:rds:us-east-1:ACCOUNT_ID:snapshot:orochi-prod-20240115 \
    --target-db-snapshot-identifier orochi-prod-dr-20240115 \
    --region us-west-2 \
    --source-region us-east-1 \
    --kms-key-id alias/aws/rds
```

### Point-in-Time Recovery

```bash
# Restore to specific point in time
aws rds restore-db-instance-to-point-in-time \
    --source-db-instance-identifier orochi-prod \
    --target-db-instance-identifier orochi-prod-pitr \
    --restore-time 2024-01-15T10:30:00Z \
    --db-instance-class db.r6g.xlarge \
    --db-subnet-group-name orochi-db-subnet \
    --vpc-security-group-ids sg-xxxxxxxxx
```

### Failover Testing

```bash
# Force failover for Multi-AZ instance
aws rds reboot-db-instance \
    --db-instance-identifier orochi-prod \
    --force-failover

# Monitor failover progress
aws rds describe-events \
    --source-identifier orochi-prod \
    --source-type db-instance \
    --duration 60
```

### Recovery Time Objectives

| Scenario | RTO | RPO | Method |
|----------|-----|-----|--------|
| AZ Failure | < 2 min | 0 | Multi-AZ automatic failover |
| Instance Failure | < 2 min | 0 | Multi-AZ automatic failover |
| Region Failure | < 1 hour | < 1 min | Cross-region read replica promotion |
| Data Corruption | < 4 hours | Varies | Point-in-time recovery |
| Complete Disaster | < 8 hours | < 24 hours | Snapshot restore |

---

## Appendix: Quick Reference Commands

### Common Operations

```bash
# Check RDS instance status
aws rds describe-db-instances \
    --db-instance-identifier orochi-prod \
    --query 'DBInstances[0].[DBInstanceStatus,Endpoint.Address]'

# Get connection string
echo "postgresql://orochi_admin@$(aws rds describe-db-instances \
    --db-instance-identifier orochi-prod \
    --query 'DBInstances[0].Endpoint.Address' \
    --output text):5432/orochi?sslmode=require"

# Scale instance
aws rds modify-db-instance \
    --db-instance-identifier orochi-prod \
    --db-instance-class db.r6g.4xlarge \
    --apply-immediately

# Increase storage
aws rds modify-db-instance \
    --db-instance-identifier orochi-prod \
    --allocated-storage 1000 \
    --apply-immediately

# Update parameter group
aws rds modify-db-parameter-group \
    --db-parameter-group-name orochi-pg16-params \
    --parameters "ParameterName=work_mem,ParameterValue=524288,ApplyMethod=immediate"

# Reboot to apply pending changes
aws rds reboot-db-instance \
    --db-instance-identifier orochi-prod
```

### Troubleshooting

```bash
# Check CloudWatch logs
aws logs get-log-events \
    --log-group-name /aws/rds/instance/orochi-prod/postgresql \
    --log-stream-name orochi-prod \
    --limit 50

# Check recent events
aws rds describe-events \
    --source-identifier orochi-prod \
    --source-type db-instance \
    --duration 1440

# Get Performance Insights metrics
aws pi get-resource-metrics \
    --service-type RDS \
    --identifier db-XXXXXXXXXXXXXXXXXXXXXXXXXX \
    --metric-queries '[{"Metric": "db.load.avg"}]' \
    --start-time $(date -d '1 hour ago' -u +%Y-%m-%dT%H:%M:%SZ) \
    --end-time $(date -u +%Y-%m-%dT%H:%M:%SZ) \
    --period-in-seconds 60
```

---

## Support and Resources

- **Orochi DB Documentation**: `/docs/` directory
- **GitHub Issues**: Report bugs and feature requests
- **AWS RDS Documentation**: https://docs.aws.amazon.com/rds/
- **PostgreSQL Documentation**: https://www.postgresql.org/docs/

For production deployments, consider engaging AWS Professional Services or an AWS Partner for architecture review.
