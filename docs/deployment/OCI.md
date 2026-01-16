# Orochi DB - Oracle Cloud Infrastructure Deployment Guide

This guide provides comprehensive instructions for deploying Orochi DB on Oracle Cloud Infrastructure (OCI), covering compute instances, Kubernetes (OKE), managed PostgreSQL, and integration with OCI Object Storage for tiered data management.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [OCI Database Service](#2-oci-database-service)
3. [Compute Instance Deployment](#3-compute-instance-deployment)
4. [OKE (Kubernetes) Deployment](#4-oke-kubernetes-deployment)
5. [Object Storage Integration](#5-object-storage-integration)
6. [Autonomous Database Considerations](#6-autonomous-database-considerations)
7. [Resource Manager (Terraform)](#7-resource-manager-terraform)
8. [Monitoring and Observability](#8-monitoring-and-observability)
9. [Security Best Practices](#9-security-best-practices)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Prerequisites

### 1.1 OCI CLI Installation and Configuration

Install the OCI CLI on your workstation:

```bash
# Linux/macOS
bash -c "$(curl -L https://raw.githubusercontent.com/oracle/oci-cli/master/scripts/install/install.sh)"

# Verify installation
oci --version

# Configure CLI with your credentials
oci setup config
```

During setup, provide:
- User OCID (from OCI Console > Identity > Users)
- Tenancy OCID (from OCI Console > Tenancy Details)
- Region (e.g., `us-ashburn-1`, `eu-frankfurt-1`)
- API signing key path

Verify configuration:

```bash
# Test authentication
oci iam region list --output table

# List available compartments
oci iam compartment list --compartment-id-in-subtree true --output table
```

### 1.2 Environment Variables

Set up environment variables for consistent command execution:

```bash
# Core identifiers
export OCI_TENANCY_OCID="ocid1.tenancy.oc1..aaaa..."
export OCI_COMPARTMENT_OCID="ocid1.compartment.oc1..aaaa..."
export OCI_REGION="us-ashburn-1"

# Networking
export OCI_VCN_OCID="ocid1.vcn.oc1..."
export OCI_SUBNET_OCID="ocid1.subnet.oc1..."

# Availability Domain
export OCI_AD="Uocm:US-ASHBURN-AD-1"

# For Orochi DB
export OROCHI_DB_NAME="orochi-prod"
export OROCHI_DB_VERSION="16"
```

### 1.3 Required IAM Policies

Create a policy group for Orochi DB administrators:

```bash
# Create a group for database administrators
oci iam group create \
    --name "OrochDBAdmins" \
    --description "Administrators for Orochi DB deployments"

# Get the group OCID
export ADMIN_GROUP_OCID=$(oci iam group list \
    --name "OrochDBAdmins" \
    --query 'data[0].id' --raw-output)
```

Create IAM policies for the deployment:

```bash
# Create policy document
cat > /tmp/orochi-policy.json << 'EOF'
[
    "Allow group OrochDBAdmins to manage database-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage virtual-network-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage compute-management-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage volume-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage object-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage cluster-family in compartment OrochDB",
    "Allow group OrochDBAdmins to manage orm-stacks in compartment OrochDB",
    "Allow group OrochDBAdmins to manage orm-jobs in compartment OrochDB",
    "Allow group OrochDBAdmins to read metrics in compartment OrochDB",
    "Allow group OrochDBAdmins to manage alarms in compartment OrochDB",
    "Allow group OrochDBAdmins to manage notifications in compartment OrochDB",
    "Allow service objectstorage-${OCI_REGION} to manage object-family in compartment OrochDB"
]
EOF

# Create the policy
oci iam policy create \
    --compartment-id "$OCI_TENANCY_OCID" \
    --name "OrochDB-Policy" \
    --description "IAM policy for Orochi DB deployments" \
    --statements file:///tmp/orochi-policy.json
```

### 1.4 VCN and Network Configuration

Create a Virtual Cloud Network with proper security configuration:

```bash
# Create VCN
oci network vcn create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-vcn" \
    --cidr-blocks '["10.0.0.0/16"]' \
    --dns-label "orochivcn"

# Store VCN OCID
export OCI_VCN_OCID=$(oci network vcn list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-vcn" \
    --query 'data[0].id' --raw-output)

# Create Internet Gateway
oci network internet-gateway create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-igw" \
    --is-enabled true

export IGW_OCID=$(oci network internet-gateway list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --query 'data[0].id' --raw-output)

# Create NAT Gateway (for private subnets)
oci network nat-gateway create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-nat"

export NAT_OCID=$(oci network nat-gateway list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --query 'data[0].id' --raw-output)

# Create Service Gateway (for OCI services)
export SERVICE_OCID=$(oci network service list \
    --query "data[?contains(name, 'All') && contains(name, 'Services')].id | [0]" \
    --raw-output)

oci network service-gateway create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-sgw" \
    --services "[{\"serviceId\": \"$SERVICE_OCID\"}]"
```

Create route tables:

```bash
# Public route table (for bastion/load balancers)
oci network route-table create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-public-rt" \
    --route-rules "[{
        \"destination\": \"0.0.0.0/0\",
        \"destinationType\": \"CIDR_BLOCK\",
        \"networkEntityId\": \"$IGW_OCID\"
    }]"

# Private route table (for database instances)
export SGW_OCID=$(oci network service-gateway list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --query 'data[0].id' --raw-output)

oci network route-table create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-private-rt" \
    --route-rules "[
        {
            \"destination\": \"0.0.0.0/0\",
            \"destinationType\": \"CIDR_BLOCK\",
            \"networkEntityId\": \"$NAT_OCID\"
        },
        {
            \"destination\": \"all-${OCI_REGION}-services-in-oracle-services-network\",
            \"destinationType\": \"SERVICE_CIDR_BLOCK\",
            \"networkEntityId\": \"$SGW_OCID\"
        }
    ]"
```

Create security lists:

```bash
# Database security list
cat > /tmp/db-security-rules.json << 'EOF'
{
    "egressSecurityRules": [
        {
            "destination": "0.0.0.0/0",
            "protocol": "all",
            "isStateless": false
        }
    ],
    "ingressSecurityRules": [
        {
            "source": "10.0.0.0/16",
            "protocol": "6",
            "isStateless": false,
            "tcpOptions": {
                "destinationPortRange": {
                    "min": 5432,
                    "max": 5432
                }
            },
            "description": "PostgreSQL from VCN"
        },
        {
            "source": "10.0.0.0/16",
            "protocol": "6",
            "isStateless": false,
            "tcpOptions": {
                "destinationPortRange": {
                    "min": 22,
                    "max": 22
                }
            },
            "description": "SSH from VCN"
        },
        {
            "source": "10.0.0.0/16",
            "protocol": "6",
            "isStateless": false,
            "tcpOptions": {
                "destinationPortRange": {
                    "min": 6432,
                    "max": 6432
                }
            },
            "description": "PgBouncer from VCN"
        }
    ]
}
EOF

oci network security-list create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-seclist" \
    --egress-security-rules file:///tmp/db-security-rules.json#egressSecurityRules \
    --ingress-security-rules file:///tmp/db-security-rules.json#ingressSecurityRules
```

Create subnets:

```bash
export PRIVATE_RT_OCID=$(oci network route-table list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-private-rt" \
    --query 'data[0].id' --raw-output)

export DB_SECLIST_OCID=$(oci network security-list list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-seclist" \
    --query 'data[0].id' --raw-output)

# Database subnet (private)
oci network subnet create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-subnet" \
    --cidr-block "10.0.1.0/24" \
    --dns-label "orochidb" \
    --prohibit-public-ip-on-vnic true \
    --route-table-id "$PRIVATE_RT_OCID" \
    --security-list-ids "[\"$DB_SECLIST_OCID\"]"

# Application subnet (private)
oci network subnet create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-app-subnet" \
    --cidr-block "10.0.2.0/24" \
    --dns-label "orochiapp" \
    --prohibit-public-ip-on-vnic true \
    --route-table-id "$PRIVATE_RT_OCID"

# Public subnet (for load balancers, bastion)
export PUBLIC_RT_OCID=$(oci network route-table list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-public-rt" \
    --query 'data[0].id' --raw-output)

oci network subnet create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-public-subnet" \
    --cidr-block "10.0.0.0/24" \
    --dns-label "orochipu" \
    --prohibit-public-ip-on-vnic false \
    --route-table-id "$PUBLIC_RT_OCID"
```

---

## 2. OCI Database Service

### 2.1 Create PostgreSQL Database System

OCI Database with PostgreSQL provides managed PostgreSQL instances:

```bash
# List available PostgreSQL versions
oci psql db-system-shape list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --output table

# List available shapes
oci psql shape list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --output table

# Get subnet OCID
export DB_SUBNET_OCID=$(oci network subnet list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-subnet" \
    --query 'data[0].id' --raw-output)

# Create PostgreSQL DB System
oci psql db-system create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-postgres" \
    --db-version "16" \
    --shape "PostgreSQL.VM.Standard.E4.Flex.4.64GB" \
    --instance-count 2 \
    --instance-memory-size-in-gbs 64 \
    --instance-ocpu-count 4 \
    --storage-details '{
        "systemType": "OCI_OPTIMIZED_STORAGE",
        "isRegionallyDurable": true,
        "availabilityDomain": "'$OCI_AD'"
    }' \
    --network-details '{
        "subnetId": "'$DB_SUBNET_OCID'"
    }' \
    --credentials '{
        "username": "orochi_admin",
        "passwordDetails": {
            "passwordType": "PLAIN_TEXT",
            "password": "'"$(openssl rand -base64 24)"'"
        }
    }' \
    --config-id "$CONFIG_OCID" \
    --wait-for-state SUCCEEDED
```

### 2.2 PostgreSQL Configuration for Orochi DB

Create a custom configuration for Orochi DB workloads:

```bash
# Create configuration
oci psql configuration create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-config" \
    --shape "PostgreSQL.VM.Standard.E4.Flex.4.64GB" \
    --db-version "16" \
    --db-configuration-overrides '{
        "items": [
            {"configKey": "shared_preload_libraries", "overriddenConfigValue": "orochi"},
            {"configKey": "max_connections", "overriddenConfigValue": "500"},
            {"configKey": "shared_buffers", "overriddenConfigValue": "16GB"},
            {"configKey": "effective_cache_size", "overriddenConfigValue": "48GB"},
            {"configKey": "maintenance_work_mem", "overriddenConfigValue": "2GB"},
            {"configKey": "checkpoint_completion_target", "overriddenConfigValue": "0.9"},
            {"configKey": "wal_buffers", "overriddenConfigValue": "64MB"},
            {"configKey": "default_statistics_target", "overriddenConfigValue": "500"},
            {"configKey": "random_page_cost", "overriddenConfigValue": "1.1"},
            {"configKey": "effective_io_concurrency", "overriddenConfigValue": "200"},
            {"configKey": "work_mem", "overriddenConfigValue": "64MB"},
            {"configKey": "min_wal_size", "overriddenConfigValue": "4GB"},
            {"configKey": "max_wal_size", "overriddenConfigValue": "16GB"},
            {"configKey": "max_worker_processes", "overriddenConfigValue": "16"},
            {"configKey": "max_parallel_workers_per_gather", "overriddenConfigValue": "4"},
            {"configKey": "max_parallel_workers", "overriddenConfigValue": "8"},
            {"configKey": "max_parallel_maintenance_workers", "overriddenConfigValue": "4"},
            {"configKey": "jit", "overriddenConfigValue": "on"}
        ]
    }' \
    --wait-for-state SUCCEEDED

export CONFIG_OCID=$(oci psql configuration list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-config" \
    --query 'data.items[0].id' --raw-output)
```

### 2.3 High Availability with Read Replicas

Enable read replicas for high availability:

```bash
# Get DB System OCID
export DB_SYSTEM_OCID=$(oci psql db-system list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-postgres" \
    --query 'data.items[0].id' --raw-output)

# Create read replica
oci psql replica create \
    --db-system-id "$DB_SYSTEM_OCID" \
    --display-name "orochi-replica-1" \
    --wait-for-state SUCCEEDED

# Create second replica in different AD for HA
oci psql replica create \
    --db-system-id "$DB_SYSTEM_OCID" \
    --display-name "orochi-replica-2" \
    --wait-for-state SUCCEEDED
```

### 2.4 Connect and Install Orochi Extension

```bash
# Get connection details
oci psql db-system get \
    --db-system-id "$DB_SYSTEM_OCID" \
    --query 'data.{"endpoint": "networkDetails.primaryDbEndpointPrivateIp", "port": 5432}' \
    --output table

# Connect via bastion or private access
export PGHOST="<primary-endpoint-ip>"
export PGPORT="5432"
export PGUSER="orochi_admin"
export PGPASSWORD="<your-password>"

# Install Orochi extension (requires extension files in PostgreSQL lib directory)
psql -d postgres << 'EOF'
CREATE EXTENSION IF NOT EXISTS orochi;

-- Verify installation
SELECT * FROM pg_extension WHERE extname = 'orochi';

-- Create application database
CREATE DATABASE orochi_production;
\c orochi_production
CREATE EXTENSION orochi;

-- Configure Orochi
ALTER SYSTEM SET orochi.default_shard_count = 32;
ALTER SYSTEM SET orochi.enable_columnar_storage = on;
ALTER SYSTEM SET orochi.tiered_storage_enabled = on;
SELECT pg_reload_conf();
EOF
```

---

## 3. Compute Instance Deployment

### 3.1 Shape Selection Guide

| Workload Type | Recommended Shape | vCPUs | Memory | Use Case |
|--------------|-------------------|-------|--------|----------|
| Development | VM.Standard.E4.Flex | 2-4 | 16-32GB | Testing, dev |
| Production OLTP | VM.Standard.E4.Flex | 8-16 | 64-128GB | Transactional |
| Analytics | VM.Optimized3.Flex | 8-32 | 128-512GB | OLAP, columnar |
| Mixed HTAP | BM.Standard.E4.128 | 128 | 2TB | Full HTAP |
| High Memory | BM.HPC2.36 | 36 | 384GB | Large datasets |

### 3.2 Create Compute Instance

```bash
# List available images (Oracle Linux 8)
oci compute image list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --operating-system "Oracle Linux" \
    --operating-system-version "8" \
    --shape "VM.Standard.E4.Flex" \
    --sort-by TIMECREATED \
    --sort-order DESC \
    --query 'data[0].id' --raw-output

export IMAGE_OCID=$(oci compute image list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --operating-system "Oracle Linux" \
    --operating-system-version "8" \
    --shape "VM.Standard.E4.Flex" \
    --sort-by TIMECREATED --sort-order DESC \
    --query 'data[0].id' --raw-output)

# Get subnet
export APP_SUBNET_OCID=$(oci network subnet list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-subnet" \
    --query 'data[0].id' --raw-output)

# Create SSH key (if needed)
ssh-keygen -t rsa -b 4096 -f ~/.ssh/orochi-oci -N ""

# Create cloud-init script for PostgreSQL + Orochi installation
cat > /tmp/cloud-init.sh << 'CLOUDINIT'
#!/bin/bash
set -e

# Update system
dnf update -y
dnf install -y oracle-epel-release-el8

# Install PostgreSQL 16
dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-8-x86_64/pgdg-redhat-repo-latest.noarch.rpm
dnf -qy module disable postgresql
dnf install -y postgresql16-server postgresql16-devel postgresql16-contrib

# Install build dependencies
dnf install -y gcc make git lz4-devel libzstd-devel openssl-devel libcurl-devel librdkafka-devel

# Initialize PostgreSQL
/usr/pgsql-16/bin/postgresql-16-setup initdb
systemctl enable postgresql-16
systemctl start postgresql-16

# Clone and build Orochi DB
cd /opt
git clone https://github.com/orochi-db/orochi-db.git
cd orochi-db
export PATH="/usr/pgsql-16/bin:$PATH"
make
make install

# Configure PostgreSQL
cat >> /var/lib/pgsql/16/data/postgresql.conf << 'PGCONF'
# Orochi DB Configuration
shared_preload_libraries = 'orochi'
listen_addresses = '*'
max_connections = 500
shared_buffers = 16GB
effective_cache_size = 48GB
maintenance_work_mem = 2GB
checkpoint_completion_target = 0.9
wal_buffers = 64MB
default_statistics_target = 500
random_page_cost = 1.1
effective_io_concurrency = 200
work_mem = 64MB
min_wal_size = 4GB
max_wal_size = 16GB
max_worker_processes = 16
max_parallel_workers_per_gather = 4
max_parallel_workers = 8

# Orochi specific
orochi.default_shard_count = 32
orochi.enable_columnar_storage = on
orochi.tiered_storage_enabled = on
orochi.oci_object_storage_enabled = on
PGCONF

# Configure pg_hba.conf
cat >> /var/lib/pgsql/16/data/pg_hba.conf << 'PGHBA'
# Allow connections from VCN
host    all    all    10.0.0.0/16    scram-sha-256
PGHBA

# Restart PostgreSQL
systemctl restart postgresql-16

# Create Orochi extension
sudo -u postgres psql -c "CREATE EXTENSION orochi;"

echo "Orochi DB installation complete"
CLOUDINIT

# Create the instance
oci compute instance launch \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --availability-domain "$OCI_AD" \
    --display-name "orochi-db-primary" \
    --shape "VM.Standard.E4.Flex" \
    --shape-config '{
        "ocpus": 8,
        "memoryInGBs": 128,
        "baselineOcpuUtilization": "BASELINE_1_1"
    }' \
    --source-details '{
        "sourceType": "image",
        "imageId": "'$IMAGE_OCID'",
        "bootVolumeSizeInGBs": 100
    }' \
    --create-vnic-details '{
        "subnetId": "'$APP_SUBNET_OCID'",
        "assignPublicIp": false
    }' \
    --metadata '{
        "ssh_authorized_keys": "'"$(cat ~/.ssh/orochi-oci.pub)"'",
        "user_data": "'"$(base64 -w 0 /tmp/cloud-init.sh)"'"
    }' \
    --wait-for-state RUNNING
```

### 3.3 Block Volume Configuration

Create and attach high-performance block volumes for data:

```bash
# Get instance OCID
export INSTANCE_OCID=$(oci compute instance list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-db-primary" \
    --query 'data[0].id' --raw-output)

# Create data volume (Ultra High Performance)
oci bv volume create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --availability-domain "$OCI_AD" \
    --display-name "orochi-data-vol" \
    --size-in-gbs 2048 \
    --vpus-per-gb 30 \
    --wait-for-state AVAILABLE

export DATA_VOL_OCID=$(oci bv volume list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-data-vol" \
    --query 'data[0].id' --raw-output)

# Create WAL volume (Ultra High Performance)
oci bv volume create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --availability-domain "$OCI_AD" \
    --display-name "orochi-wal-vol" \
    --size-in-gbs 512 \
    --vpus-per-gb 30 \
    --wait-for-state AVAILABLE

export WAL_VOL_OCID=$(oci bv volume list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-wal-vol" \
    --query 'data[0].id' --raw-output)

# Attach data volume
oci compute volume-attachment attach \
    --instance-id "$INSTANCE_OCID" \
    --volume-id "$DATA_VOL_OCID" \
    --type paravirtualized \
    --device "/dev/oracleoci/oraclevdb" \
    --wait-for-state ATTACHED

# Attach WAL volume
oci compute volume-attachment attach \
    --instance-id "$INSTANCE_OCID" \
    --volume-id "$WAL_VOL_OCID" \
    --type paravirtualized \
    --device "/dev/oracleoci/oraclevdc" \
    --wait-for-state ATTACHED
```

Configure volumes on the instance:

```bash
# SSH to instance and configure storage
ssh -i ~/.ssh/orochi-oci opc@<instance-ip> << 'SSHCMD'
# Format and mount data volume
sudo mkfs.xfs /dev/oracleoci/oraclevdb
sudo mkdir -p /pgdata
sudo mount /dev/oracleoci/oraclevdb /pgdata
sudo chown postgres:postgres /pgdata

# Format and mount WAL volume
sudo mkfs.xfs /dev/oracleoci/oraclevdc
sudo mkdir -p /pgwal
sudo mount /dev/oracleoci/oraclevdc /pgwal
sudo chown postgres:postgres /pgwal

# Add to fstab
echo '/dev/oracleoci/oraclevdb /pgdata xfs defaults,_netdev,nofail 0 2' | sudo tee -a /etc/fstab
echo '/dev/oracleoci/oraclevdc /pgwal xfs defaults,_netdev,nofail 0 2' | sudo tee -a /etc/fstab

# Update PostgreSQL data directory
sudo systemctl stop postgresql-16
sudo -u postgres mv /var/lib/pgsql/16/data/* /pgdata/
sudo -u postgres mkdir -p /pgwal/pg_wal
sudo -u postgres mv /pgdata/pg_wal/* /pgwal/pg_wal/
sudo -u postgres ln -s /pgwal/pg_wal /pgdata/pg_wal

# Update postgresql.conf
sudo -u postgres sed -i "s|data_directory = .*|data_directory = '/pgdata'|" /pgdata/postgresql.conf
sudo systemctl start postgresql-16
SSHCMD
```

### 3.4 Instance Pool for High Availability

Create an instance pool for automatic recovery:

```bash
# Create instance configuration
oci compute-management instance-configuration create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-instance-config" \
    --instance-details '{
        "instanceType": "compute",
        "launchDetails": {
            "compartmentId": "'$OCI_COMPARTMENT_OCID'",
            "shape": "VM.Standard.E4.Flex",
            "shapeConfig": {
                "ocpus": 8,
                "memoryInGBs": 128
            },
            "sourceDetails": {
                "sourceType": "image",
                "imageId": "'$IMAGE_OCID'"
            },
            "createVnicDetails": {
                "subnetId": "'$APP_SUBNET_OCID'"
            }
        }
    }'

export INSTANCE_CONFIG_OCID=$(oci compute-management instance-configuration list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --query 'data[0].id' --raw-output)

# Create instance pool
oci compute-management instance-pool create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --instance-configuration-id "$INSTANCE_CONFIG_OCID" \
    --placement-configurations '[{
        "availabilityDomain": "'$OCI_AD'",
        "primarySubnetId": "'$APP_SUBNET_OCID'"
    }]' \
    --size 2 \
    --display-name "orochi-pool" \
    --wait-for-state RUNNING
```

---

## 4. OKE (Kubernetes) Deployment

### 4.1 Create OKE Cluster

```bash
# Create Kubernetes cluster
oci ce cluster create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-cluster" \
    --vcn-id "$OCI_VCN_OCID" \
    --kubernetes-version "v1.28.2" \
    --type "ENHANCED_CLUSTER" \
    --endpoint-config '{
        "isPublicIpEnabled": true,
        "subnetId": "'$PUBLIC_SUBNET_OCID'"
    }' \
    --wait-for-state SUCCEEDED

export CLUSTER_OCID=$(oci ce cluster list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-cluster" \
    --query 'data[0].id' --raw-output)

# Create node pool
oci ce node-pool create \
    --cluster-id "$CLUSTER_OCID" \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-nodepool" \
    --kubernetes-version "v1.28.2" \
    --node-shape "VM.Standard.E4.Flex" \
    --node-shape-config '{
        "ocpus": 8,
        "memoryInGBs": 64
    }' \
    --node-config-details '{
        "size": 3,
        "placementConfigs": [{
            "availabilityDomain": "'$OCI_AD'",
            "subnetId": "'$APP_SUBNET_OCID'"
        }]
    }' \
    --wait-for-state SUCCEEDED

# Get kubeconfig
oci ce cluster create-kubeconfig \
    --cluster-id "$CLUSTER_OCID" \
    --file $HOME/.kube/config \
    --region "$OCI_REGION" \
    --token-version 2.0.0

# Verify cluster access
kubectl get nodes
```

### 4.2 Kubernetes Manifests

Create the Orochi DB deployment manifests:

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
    listen_addresses = '*'
    max_connections = 500
    shared_buffers = 8GB
    effective_cache_size = 24GB
    maintenance_work_mem = 2GB
    checkpoint_completion_target = 0.9
    wal_buffers = 64MB
    default_statistics_target = 500
    random_page_cost = 1.1
    effective_io_concurrency = 200
    work_mem = 64MB
    min_wal_size = 2GB
    max_wal_size = 8GB
    max_worker_processes = 8
    max_parallel_workers_per_gather = 2
    max_parallel_workers = 4

    # Orochi settings
    orochi.default_shard_count = 32
    orochi.enable_columnar_storage = on
    orochi.tiered_storage_enabled = on
    orochi.oci_object_storage_namespace = 'your-namespace'
    orochi.oci_object_storage_bucket = 'orochi-tiered-storage'

  pg_hba.conf: |
    local   all   all                trust
    host    all   all   10.0.0.0/8   scram-sha-256
    host    all   all   0.0.0.0/0    scram-sha-256
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
  POSTGRES_PASSWORD: "your-secure-password-here"
  POSTGRES_DB: orochi_production
  # OCI credentials for Object Storage
  OCI_TENANCY_OCID: "ocid1.tenancy.oc1..."
  OCI_USER_OCID: "ocid1.user.oc1..."
  OCI_FINGERPRINT: "aa:bb:cc:..."
  OCI_REGION: "us-ashburn-1"
---
# orochi-pvc.yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-data-pvc
  namespace: orochi-db
spec:
  storageClassName: oci-bv
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: 500Gi
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-wal-pvc
  namespace: orochi-db
spec:
  storageClassName: oci-bv
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: 100Gi
---
# orochi-statefulset.yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: orochi-db
  namespace: orochi-db
spec:
  serviceName: orochi-db
  replicas: 3
  selector:
    matchLabels:
      app: orochi-db
  template:
    metadata:
      labels:
        app: orochi-db
    spec:
      securityContext:
        fsGroup: 999
      containers:
      - name: orochi-postgres
        image: orochi-db/orochi-postgres:16-latest
        ports:
        - containerPort: 5432
          name: postgres
        envFrom:
        - secretRef:
            name: orochi-credentials
        env:
        - name: PGDATA
          value: /var/lib/postgresql/data/pgdata
        resources:
          requests:
            memory: "32Gi"
            cpu: "4"
          limits:
            memory: "64Gi"
            cpu: "8"
        volumeMounts:
        - name: data
          mountPath: /var/lib/postgresql/data
        - name: wal
          mountPath: /var/lib/postgresql/wal
        - name: config
          mountPath: /etc/postgresql
        - name: oci-config
          mountPath: /root/.oci
          readOnly: true
        livenessProbe:
          exec:
            command:
            - pg_isready
            - -U
            - orochi_admin
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          exec:
            command:
            - pg_isready
            - -U
            - orochi_admin
          initialDelaySeconds: 5
          periodSeconds: 5
      volumes:
      - name: config
        configMap:
          name: orochi-config
      - name: oci-config
        secret:
          secretName: oci-api-key
  volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      storageClassName: oci-bv-encrypted
      accessModes:
      - ReadWriteOnce
      resources:
        requests:
          storage: 500Gi
  - metadata:
      name: wal
    spec:
      storageClassName: oci-bv-encrypted
      accessModes:
      - ReadWriteOnce
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
spec:
  type: ClusterIP
  ports:
  - port: 5432
    targetPort: 5432
    name: postgres
  selector:
    app: orochi-db
---
apiVersion: v1
kind: Service
metadata:
  name: orochi-db-read
  namespace: orochi-db
spec:
  type: ClusterIP
  ports:
  - port: 5432
    targetPort: 5432
    name: postgres
  selector:
    app: orochi-db
    role: replica
---
# orochi-loadbalancer.yaml
apiVersion: v1
kind: Service
metadata:
  name: orochi-db-lb
  namespace: orochi-db
  annotations:
    oci.oraclecloud.com/load-balancer-type: "lb"
    service.beta.kubernetes.io/oci-load-balancer-internal: "true"
    service.beta.kubernetes.io/oci-load-balancer-subnet1: "ocid1.subnet..."
spec:
  type: LoadBalancer
  ports:
  - port: 5432
    targetPort: 5432
    name: postgres
  selector:
    app: orochi-db
```

### 4.3 Storage Classes for OCI Block Volumes

```yaml
# storage-classes.yaml
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: oci-bv
provisioner: blockvolume.csi.oraclecloud.com
parameters:
  attachment-type: "paravirtualized"
reclaimPolicy: Retain
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
---
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: oci-bv-encrypted
provisioner: blockvolume.csi.oraclecloud.com
parameters:
  attachment-type: "paravirtualized"
  vpusPerGB: "20"
  kmsKeyId: "ocid1.key.oc1..."
reclaimPolicy: Retain
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
---
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: oci-bv-uhp
provisioner: blockvolume.csi.oraclecloud.com
parameters:
  attachment-type: "paravirtualized"
  vpusPerGB: "30"
reclaimPolicy: Retain
volumeBindingMode: WaitForFirstConsumer
allowVolumeExpansion: true
```

### 4.4 Helm Chart Configuration

Create Helm values for Orochi DB:

```yaml
# values-oci.yaml
replicaCount: 3

image:
  repository: orochi-db/orochi-postgres
  tag: "16-latest"
  pullPolicy: IfNotPresent

postgresql:
  version: "16"
  username: orochi_admin
  database: orochi_production

  resources:
    requests:
      memory: "32Gi"
      cpu: "4"
    limits:
      memory: "64Gi"
      cpu: "8"

  config:
    shared_preload_libraries: "orochi"
    max_connections: 500
    shared_buffers: "16GB"
    effective_cache_size: "48GB"
    maintenance_work_mem: "2GB"
    checkpoint_completion_target: 0.9
    wal_buffers: "64MB"
    work_mem: "64MB"
    min_wal_size: "4GB"
    max_wal_size: "16GB"
    max_worker_processes: 16
    max_parallel_workers_per_gather: 4
    max_parallel_workers: 8

orochi:
  shardCount: 32
  enableColumnar: true
  tieredStorage:
    enabled: true
    provider: "oci"
    namespace: "your-namespace"
    bucket: "orochi-tiered-storage"
    region: "us-ashburn-1"

persistence:
  enabled: true
  data:
    storageClass: "oci-bv-encrypted"
    size: "500Gi"
  wal:
    storageClass: "oci-bv-uhp"
    size: "100Gi"

service:
  type: LoadBalancer
  annotations:
    oci.oraclecloud.com/load-balancer-type: "lb"
    service.beta.kubernetes.io/oci-load-balancer-internal: "true"
    service.beta.kubernetes.io/oci-load-balancer-subnet1: "ocid1.subnet..."

podDisruptionBudget:
  enabled: true
  minAvailable: 2

affinity:
  podAntiAffinity:
    preferredDuringSchedulingIgnoredDuringExecution:
    - weight: 100
      podAffinityTerm:
        labelSelector:
          matchLabels:
            app: orochi-db
        topologyKey: topology.kubernetes.io/zone

monitoring:
  enabled: true
  serviceMonitor:
    enabled: true
```

Deploy with Helm:

```bash
# Add Orochi Helm repository
helm repo add orochi https://charts.orochi-db.io
helm repo update

# Install Orochi DB
helm install orochi-db orochi/orochi-db \
    --namespace orochi-db \
    --create-namespace \
    --values values-oci.yaml \
    --wait
```

---

## 5. Object Storage Integration

### 5.1 Create Object Storage Bucket

```bash
# Create namespace (use existing tenancy namespace)
export OCI_NAMESPACE=$(oci os ns get --query 'data' --raw-output)

# Create bucket for tiered storage
oci os bucket create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-tiered-storage" \
    --storage-tier "Standard" \
    --versioning "Enabled" \
    --auto-tiering "InfrequentAccess" \
    --object-events-enabled true

# Create bucket for backups
oci os bucket create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-backups" \
    --storage-tier "Standard" \
    --versioning "Enabled"

# Create bucket for cold storage (Archive)
oci os bucket create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-archive" \
    --storage-tier "Archive"
```

### 5.2 Configure Lifecycle Policies

```bash
# Create lifecycle policy for automatic tiering
cat > /tmp/lifecycle-policy.json << 'EOF'
{
    "items": [
        {
            "name": "archive-old-data",
            "action": "ARCHIVE",
            "objectNameFilter": {
                "inclusionPrefixes": ["cold/"]
            },
            "timeAmount": 30,
            "timeUnit": "DAYS",
            "isEnabled": true
        },
        {
            "name": "delete-temp-files",
            "action": "DELETE",
            "objectNameFilter": {
                "inclusionPrefixes": ["temp/"]
            },
            "timeAmount": 7,
            "timeUnit": "DAYS",
            "isEnabled": true
        },
        {
            "name": "infrequent-access-warm",
            "action": "INFREQUENT_ACCESS",
            "objectNameFilter": {
                "inclusionPrefixes": ["warm/"]
            },
            "timeAmount": 7,
            "timeUnit": "DAYS",
            "isEnabled": true
        }
    ]
}
EOF

oci os object-lifecycle-policy put \
    --bucket-name "orochi-tiered-storage" \
    --namespace "$OCI_NAMESPACE" \
    --items file:///tmp/lifecycle-policy.json
```

### 5.3 IAM Policies for Object Storage Access

```bash
# Create dynamic group for instances/pods
oci iam dynamic-group create \
    --compartment-id "$OCI_TENANCY_OCID" \
    --name "OrochDBInstances" \
    --description "Orochi DB compute instances and OKE pods" \
    --matching-rule "Any {instance.compartment.id = '$OCI_COMPARTMENT_OCID', resource.type = 'cluster', resource.compartment.id = '$OCI_COMPARTMENT_OCID'}"

# Create policy for Object Storage access
cat > /tmp/os-policy.json << 'EOF'
[
    "Allow dynamic-group OrochDBInstances to read buckets in compartment OrochDB",
    "Allow dynamic-group OrochDBInstances to manage objects in compartment OrochDB where target.bucket.name = 'orochi-tiered-storage'",
    "Allow dynamic-group OrochDBInstances to manage objects in compartment OrochDB where target.bucket.name = 'orochi-backups'",
    "Allow dynamic-group OrochDBInstances to manage objects in compartment OrochDB where target.bucket.name = 'orochi-archive'"
]
EOF

oci iam policy create \
    --compartment-id "$OCI_TENANCY_OCID" \
    --name "OrochDB-ObjectStorage-Policy" \
    --description "Allow Orochi DB instances to access Object Storage" \
    --statements file:///tmp/os-policy.json
```

### 5.4 Configure Orochi DB for OCI Object Storage

```sql
-- Configure OCI Object Storage as tiered storage backend
ALTER SYSTEM SET orochi.tiered_storage_enabled = on;
ALTER SYSTEM SET orochi.tiered_storage_provider = 'oci';
ALTER SYSTEM SET orochi.oci_object_storage_namespace = 'your-namespace';
ALTER SYSTEM SET orochi.oci_object_storage_region = 'us-ashburn-1';

-- Configure tier buckets
ALTER SYSTEM SET orochi.oci_warm_tier_bucket = 'orochi-tiered-storage';
ALTER SYSTEM SET orochi.oci_cold_tier_bucket = 'orochi-tiered-storage';
ALTER SYSTEM SET orochi.oci_frozen_tier_bucket = 'orochi-archive';

-- Configure tier thresholds
ALTER SYSTEM SET orochi.warm_tier_age_days = 7;
ALTER SYSTEM SET orochi.cold_tier_age_days = 30;
ALTER SYSTEM SET orochi.frozen_tier_age_days = 90;

-- Reload configuration
SELECT pg_reload_conf();

-- Create a table with tiered storage
SELECT orochi_create_hypertable(
    'sensor_data',
    'timestamp',
    chunk_time_interval => INTERVAL '1 day',
    tiered_storage => true
);

-- Manual tier movement
SELECT orochi_move_chunk_to_tier(
    'sensor_data',
    '2024-01-01'::timestamp,
    'cold'
);
```

### 5.5 Cross-Region Replication

```bash
# Enable replication on source bucket
oci os replication create-replication-policy \
    --bucket-name "orochi-tiered-storage" \
    --namespace "$OCI_NAMESPACE" \
    --name "orochi-cross-region-replication" \
    --destination-bucket-name "orochi-tiered-storage-dr" \
    --destination-region "us-phoenix-1"

# Create destination bucket in DR region
oci os bucket create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --name "orochi-tiered-storage-dr" \
    --storage-tier "Standard" \
    --versioning "Enabled" \
    --region "us-phoenix-1"
```

---

## 6. Autonomous Database Considerations

### 6.1 Autonomous Database Limitations

Oracle Autonomous Database (ADB) has restrictions on custom extensions. Consider these factors:

| Feature | ADB Support | Notes |
|---------|-------------|-------|
| Custom Extensions | Limited | Requires Oracle approval |
| `shared_preload_libraries` | Not configurable | Managed by Oracle |
| Background Workers | Not available | Service managed |
| Direct File Access | Not available | Managed storage |
| Custom C Functions | Restricted | Security sandbox |

### 6.2 Autonomous Database Serverless Connection

If using ADB-S for compatible workloads:

```bash
# Create Autonomous Database
oci db autonomous-database create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --db-name "OROCHIAPP" \
    --display-name "orochi-app-db" \
    --db-workload "OLTP" \
    --is-auto-scaling-enabled true \
    --cpu-core-count 2 \
    --data-storage-size-in-tbs 1 \
    --admin-password "YourSecurePassword123!" \
    --wait-for-state AVAILABLE

# Download wallet for mTLS connection
export ADB_OCID=$(oci db autonomous-database list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-app-db" \
    --query 'data[0].id' --raw-output)

oci db autonomous-database generate-wallet \
    --autonomous-database-id "$ADB_OCID" \
    --password "WalletPassword123!" \
    --file /tmp/adb-wallet.zip

unzip /tmp/adb-wallet.zip -d ~/.oracle/adb-wallet
```

### 6.3 Hybrid Architecture Pattern

For Orochi DB features with ADB, use a hybrid approach:

```
                    ┌─────────────────────────┐
                    │   Application Layer     │
                    └───────────┬─────────────┘
                                │
            ┌───────────────────┼───────────────────┐
            │                   │                   │
            ▼                   ▼                   ▼
    ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
    │  Orochi DB    │   │  Autonomous   │   │  OCI Object   │
    │  (OKE/Compute)│   │  Database     │   │  Storage      │
    │               │   │               │   │               │
    │ - HTAP        │   │ - OLTP        │   │ - Cold/Frozen │
    │ - Columnar    │   │ - Auto-scale  │   │ - Archive     │
    │ - Time-series │   │ - Self-tuning │   │ - Backups     │
    └───────────────┘   └───────────────┘   └───────────────┘
```

Configure foreign data wrapper for ADB access:

```sql
-- On Orochi DB instance
CREATE EXTENSION IF NOT EXISTS oracle_fdw;

CREATE SERVER adb_server
    FOREIGN DATA WRAPPER oracle_fdw
    OPTIONS (dbserver '//adb.us-ashburn-1.oraclecloud.com:1522/..._high');

CREATE USER MAPPING FOR orochi_admin
    SERVER adb_server
    OPTIONS (user 'ADMIN', password 'YourPassword');

-- Create foreign table for ADB data
CREATE FOREIGN TABLE adb_customers (
    customer_id INTEGER,
    name VARCHAR(100),
    email VARCHAR(255)
)
SERVER adb_server
OPTIONS (schema 'ADMIN', table 'CUSTOMERS');
```

---

## 7. Resource Manager (Terraform)

### 7.1 Terraform Stack Template

Create a comprehensive Terraform configuration for Orochi DB:

```hcl
# main.tf
terraform {
  required_providers {
    oci = {
      source  = "oracle/oci"
      version = ">= 5.0.0"
    }
  }
}

provider "oci" {
  region = var.region
}

# Variables
variable "compartment_ocid" {
  description = "Compartment OCID"
  type        = string
}

variable "region" {
  description = "OCI region"
  type        = string
  default     = "us-ashburn-1"
}

variable "vcn_cidr" {
  description = "VCN CIDR block"
  type        = string
  default     = "10.0.0.0/16"
}

variable "db_shape" {
  description = "Compute shape for database"
  type        = string
  default     = "VM.Standard.E4.Flex"
}

variable "db_ocpus" {
  description = "Number of OCPUs"
  type        = number
  default     = 8
}

variable "db_memory_gb" {
  description = "Memory in GB"
  type        = number
  default     = 128
}

variable "data_volume_size_gb" {
  description = "Data volume size in GB"
  type        = number
  default     = 1024
}

# Data sources
data "oci_identity_availability_domains" "ads" {
  compartment_id = var.compartment_ocid
}

data "oci_core_images" "oracle_linux" {
  compartment_id           = var.compartment_ocid
  operating_system         = "Oracle Linux"
  operating_system_version = "8"
  shape                    = var.db_shape
  sort_by                  = "TIMECREATED"
  sort_order               = "DESC"
}

# VCN
resource "oci_core_vcn" "orochi_vcn" {
  compartment_id = var.compartment_ocid
  cidr_blocks    = [var.vcn_cidr]
  display_name   = "orochi-vcn"
  dns_label      = "orochivcn"
}

# Internet Gateway
resource "oci_core_internet_gateway" "orochi_igw" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-igw"
  enabled        = true
}

# NAT Gateway
resource "oci_core_nat_gateway" "orochi_nat" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-nat"
}

# Service Gateway
data "oci_core_services" "all_services" {
  filter {
    name   = "name"
    values = ["All .* Services In Oracle Services Network"]
    regex  = true
  }
}

resource "oci_core_service_gateway" "orochi_sgw" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-sgw"

  services {
    service_id = data.oci_core_services.all_services.services[0].id
  }
}

# Route Tables
resource "oci_core_route_table" "orochi_public_rt" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-public-rt"

  route_rules {
    destination       = "0.0.0.0/0"
    destination_type  = "CIDR_BLOCK"
    network_entity_id = oci_core_internet_gateway.orochi_igw.id
  }
}

resource "oci_core_route_table" "orochi_private_rt" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-private-rt"

  route_rules {
    destination       = "0.0.0.0/0"
    destination_type  = "CIDR_BLOCK"
    network_entity_id = oci_core_nat_gateway.orochi_nat.id
  }

  route_rules {
    destination       = data.oci_core_services.all_services.services[0].cidr_block
    destination_type  = "SERVICE_CIDR_BLOCK"
    network_entity_id = oci_core_service_gateway.orochi_sgw.id
  }
}

# Security Lists
resource "oci_core_security_list" "orochi_db_seclist" {
  compartment_id = var.compartment_ocid
  vcn_id         = oci_core_vcn.orochi_vcn.id
  display_name   = "orochi-db-seclist"

  egress_security_rules {
    destination = "0.0.0.0/0"
    protocol    = "all"
  }

  ingress_security_rules {
    source   = var.vcn_cidr
    protocol = "6" # TCP

    tcp_options {
      min = 5432
      max = 5432
    }
  }

  ingress_security_rules {
    source   = var.vcn_cidr
    protocol = "6"

    tcp_options {
      min = 22
      max = 22
    }
  }
}

# Subnets
resource "oci_core_subnet" "orochi_db_subnet" {
  compartment_id             = var.compartment_ocid
  vcn_id                     = oci_core_vcn.orochi_vcn.id
  display_name               = "orochi-db-subnet"
  cidr_block                 = cidrsubnet(var.vcn_cidr, 8, 1)
  dns_label                  = "orochidb"
  prohibit_public_ip_on_vnic = true
  route_table_id             = oci_core_route_table.orochi_private_rt.id
  security_list_ids          = [oci_core_security_list.orochi_db_seclist.id]
}

resource "oci_core_subnet" "orochi_public_subnet" {
  compartment_id    = var.compartment_ocid
  vcn_id            = oci_core_vcn.orochi_vcn.id
  display_name      = "orochi-public-subnet"
  cidr_block        = cidrsubnet(var.vcn_cidr, 8, 0)
  dns_label         = "orochipu"
  route_table_id    = oci_core_route_table.orochi_public_rt.id
}

# Block Volumes
resource "oci_core_volume" "orochi_data_volume" {
  compartment_id      = var.compartment_ocid
  availability_domain = data.oci_identity_availability_domains.ads.availability_domains[0].name
  display_name        = "orochi-data-vol"
  size_in_gbs         = var.data_volume_size_gb
  vpus_per_gb         = 20 # Higher Performance
}

resource "oci_core_volume" "orochi_wal_volume" {
  compartment_id      = var.compartment_ocid
  availability_domain = data.oci_identity_availability_domains.ads.availability_domains[0].name
  display_name        = "orochi-wal-vol"
  size_in_gbs         = 256
  vpus_per_gb         = 30 # Ultra High Performance
}

# Compute Instance
resource "oci_core_instance" "orochi_db_instance" {
  compartment_id      = var.compartment_ocid
  availability_domain = data.oci_identity_availability_domains.ads.availability_domains[0].name
  display_name        = "orochi-db-primary"
  shape               = var.db_shape

  shape_config {
    ocpus         = var.db_ocpus
    memory_in_gbs = var.db_memory_gb
  }

  source_details {
    source_type             = "image"
    source_id               = data.oci_core_images.oracle_linux.images[0].id
    boot_volume_size_in_gbs = 100
  }

  create_vnic_details {
    subnet_id        = oci_core_subnet.orochi_db_subnet.id
    assign_public_ip = false
  }

  metadata = {
    ssh_authorized_keys = file("~/.ssh/orochi-oci.pub")
    user_data           = base64encode(file("${path.module}/cloud-init.sh"))
  }
}

# Volume Attachments
resource "oci_core_volume_attachment" "data_volume_attachment" {
  instance_id = oci_core_instance.orochi_db_instance.id
  volume_id   = oci_core_volume.orochi_data_volume.id
  attachment_type = "paravirtualized"
  device          = "/dev/oracleoci/oraclevdb"
}

resource "oci_core_volume_attachment" "wal_volume_attachment" {
  instance_id = oci_core_instance.orochi_db_instance.id
  volume_id   = oci_core_volume.orochi_wal_volume.id
  attachment_type = "paravirtualized"
  device          = "/dev/oracleoci/oraclevdc"
}

# Object Storage
resource "oci_objectstorage_bucket" "orochi_tiered_storage" {
  compartment_id = var.compartment_ocid
  namespace      = data.oci_objectstorage_namespace.ns.namespace
  name           = "orochi-tiered-storage"
  storage_tier   = "Standard"
  versioning     = "Enabled"
  auto_tiering   = "InfrequentAccess"
}

resource "oci_objectstorage_bucket" "orochi_backups" {
  compartment_id = var.compartment_ocid
  namespace      = data.oci_objectstorage_namespace.ns.namespace
  name           = "orochi-backups"
  storage_tier   = "Standard"
  versioning     = "Enabled"
}

data "oci_objectstorage_namespace" "ns" {
  compartment_id = var.compartment_ocid
}

# Outputs
output "db_instance_private_ip" {
  value = oci_core_instance.orochi_db_instance.private_ip
}

output "vcn_id" {
  value = oci_core_vcn.orochi_vcn.id
}

output "db_subnet_id" {
  value = oci_core_subnet.orochi_db_subnet.id
}

output "tiered_storage_bucket" {
  value = oci_objectstorage_bucket.orochi_tiered_storage.name
}
```

### 7.2 Deploy with Resource Manager

```bash
# Create a zip file of Terraform configuration
zip -r orochi-stack.zip *.tf cloud-init.sh

# Create Resource Manager stack
oci resource-manager stack create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-db-stack" \
    --description "Orochi DB Infrastructure Stack" \
    --config-source orochi-stack.zip \
    --variables '{
        "compartment_ocid": "'$OCI_COMPARTMENT_OCID'",
        "region": "'$OCI_REGION'",
        "db_ocpus": 8,
        "db_memory_gb": 128,
        "data_volume_size_gb": 1024
    }'

export STACK_OCID=$(oci resource-manager stack list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-db-stack" \
    --query 'data[0].id' --raw-output)

# Plan the deployment
oci resource-manager job create-plan-job \
    --stack-id "$STACK_OCID" \
    --display-name "orochi-plan" \
    --wait-for-state SUCCEEDED

# Apply the deployment
oci resource-manager job create-apply-job \
    --stack-id "$STACK_OCID" \
    --execution-plan-strategy AUTO_APPROVED \
    --display-name "orochi-apply" \
    --wait-for-state SUCCEEDED
```

### 7.3 Quick Deploy Button

For OCI Resource Manager quick deploy, create a button configuration:

```json
{
    "schemaVersion": "1.1.0",
    "title": "Orochi DB on OCI",
    "description": "Deploy Orochi DB HTAP database on Oracle Cloud Infrastructure",
    "stackType": "STANDARD",
    "packages": [],
    "logoUrl": "https://orochi-db.io/logo.png",
    "source": {
        "type": "git",
        "gitUrl": "https://github.com/orochi-db/orochi-db",
        "branch": "main",
        "path": "terraform/oci"
    },
    "variableGroups": [
        {
            "title": "Required Variables",
            "variables": [
                "compartment_ocid",
                "region"
            ]
        },
        {
            "title": "Database Configuration",
            "variables": [
                "db_shape",
                "db_ocpus",
                "db_memory_gb",
                "data_volume_size_gb"
            ]
        }
    ]
}
```

---

## 8. Monitoring and Observability

### 8.1 OCI Monitoring Configuration

Create custom metrics and alarms:

```bash
# Create metric namespace for Orochi DB
oci monitoring metric-data post \
    --metric-data '[{
        "namespace": "orochi_db",
        "compartmentId": "'$OCI_COMPARTMENT_OCID'",
        "name": "connections_active",
        "dimensions": {
            "resourceId": "'$INSTANCE_OCID'",
            "database": "orochi_production"
        },
        "datapoints": [{
            "timestamp": "'$(date -u +%Y-%m-%dT%H:%M:%SZ)'",
            "value": 25
        }]
    }]'

# Create alarm for high connection count
oci monitoring alarm create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-high-connections" \
    --metric-compartment-id "$OCI_COMPARTMENT_OCID" \
    --namespace "orochi_db" \
    --query 'connections_active[1m].mean() > 400' \
    --severity "WARNING" \
    --body "Orochi DB connection count is high" \
    --destinations '["'$NOTIFICATION_TOPIC_OCID'"]' \
    --is-enabled true

# Create alarm for replication lag
oci monitoring alarm create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-replication-lag" \
    --metric-compartment-id "$OCI_COMPARTMENT_OCID" \
    --namespace "orochi_db" \
    --query 'replication_lag_seconds[5m].max() > 30' \
    --severity "CRITICAL" \
    --body "Orochi DB replication lag exceeds 30 seconds" \
    --destinations '["'$NOTIFICATION_TOPIC_OCID'"]' \
    --is-enabled true

# Create alarm for storage utilization
oci monitoring alarm create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-storage-high" \
    --metric-compartment-id "$OCI_COMPARTMENT_OCID" \
    --namespace "oci_blockstore" \
    --query 'VolumeUtilization[5m].mean() > 85' \
    --severity "WARNING" \
    --body "Orochi DB storage utilization above 85%" \
    --destinations '["'$NOTIFICATION_TOPIC_OCID'"]' \
    --is-enabled true \
    --resource-group "orochi-db"
```

### 8.2 Database Management Service

Enable Database Management for PostgreSQL:

```bash
# Enable Database Management
oci database-management managed-database enable \
    --database-id "$DB_SYSTEM_OCID" \
    --management-type "ADVANCED"

# Create management agent
oci management-agent management-agent create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-agent" \
    --install-key-id "$AGENT_KEY_OCID"
```

### 8.3 Prometheus and Grafana Integration

Deploy monitoring stack on OKE:

```yaml
# prometheus-values.yaml
prometheus:
  prometheusSpec:
    serviceMonitorSelector:
      matchLabels:
        app: orochi-db
    additionalScrapeConfigs:
    - job_name: 'orochi-postgres'
      static_configs:
      - targets: ['orochi-db:9187']
      metrics_path: /metrics

grafana:
  dashboardProviders:
    dashboardproviders.yaml:
      apiVersion: 1
      providers:
      - name: 'orochi'
        orgId: 1
        folder: 'Orochi DB'
        type: file
        disableDeletion: false
        editable: true
        options:
          path: /var/lib/grafana/dashboards/orochi

  dashboards:
    orochi:
      orochi-overview:
        url: https://grafana.com/api/dashboards/9628/revisions/7/download
        datasource: Prometheus
```

```bash
# Deploy Prometheus stack
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts
helm install prometheus prometheus-community/kube-prometheus-stack \
    --namespace monitoring \
    --create-namespace \
    --values prometheus-values.yaml
```

### 8.4 Custom Metrics Collection Script

```bash
#!/bin/bash
# /opt/orochi/metrics-collector.sh

# Collect PostgreSQL metrics
METRICS=$(sudo -u postgres psql -d orochi_production -t -c "
SELECT json_build_object(
    'connections_active', (SELECT count(*) FROM pg_stat_activity WHERE state = 'active'),
    'connections_idle', (SELECT count(*) FROM pg_stat_activity WHERE state = 'idle'),
    'transactions_committed', (SELECT xact_commit FROM pg_stat_database WHERE datname = 'orochi_production'),
    'transactions_rolled_back', (SELECT xact_rollback FROM pg_stat_database WHERE datname = 'orochi_production'),
    'blocks_read', (SELECT blks_read FROM pg_stat_database WHERE datname = 'orochi_production'),
    'blocks_hit', (SELECT blks_hit FROM pg_stat_database WHERE datname = 'orochi_production'),
    'temp_files', (SELECT temp_files FROM pg_stat_database WHERE datname = 'orochi_production'),
    'deadlocks', (SELECT deadlocks FROM pg_stat_database WHERE datname = 'orochi_production'),
    'shard_count', (SELECT count(*) FROM orochi_shards WHERE table_id IN (SELECT id FROM orochi_tables)),
    'columnar_tables', (SELECT count(*) FROM orochi_tables WHERE storage_type = 'columnar'),
    'total_data_size_bytes', (SELECT pg_database_size('orochi_production'))
);
")

# Post to OCI Monitoring
TIMESTAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
INSTANCE_OCID=$(curl -s http://169.254.169.254/opc/v1/instance/ | jq -r '.id')

for metric in connections_active connections_idle transactions_committed deadlocks shard_count columnar_tables; do
    value=$(echo "$METRICS" | jq -r ".$metric")

    oci monitoring metric-data post \
        --metric-data "[{
            \"namespace\": \"orochi_db\",
            \"compartmentId\": \"$OCI_COMPARTMENT_OCID\",
            \"name\": \"$metric\",
            \"dimensions\": {
                \"resourceId\": \"$INSTANCE_OCID\",
                \"database\": \"orochi_production\"
            },
            \"datapoints\": [{
                \"timestamp\": \"$TIMESTAMP\",
                \"value\": $value
            }]
        }]" --auth instance_principal
done
```

Add to crontab:
```bash
# Run metrics collection every minute
* * * * * /opt/orochi/metrics-collector.sh >> /var/log/orochi/metrics.log 2>&1
```

---

## 9. Security Best Practices

### 9.1 Network Security

```bash
# Create Network Security Group for database
oci network nsg create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-nsg"

export NSG_OCID=$(oci network nsg list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vcn-id "$OCI_VCN_OCID" \
    --display-name "orochi-db-nsg" \
    --query 'data[0].id' --raw-output)

# Add rules for PostgreSQL
oci network nsg rules add \
    --nsg-id "$NSG_OCID" \
    --security-rules '[
        {
            "direction": "INGRESS",
            "protocol": "6",
            "source": "10.0.2.0/24",
            "sourceType": "CIDR_BLOCK",
            "tcpOptions": {
                "destinationPortRange": {"min": 5432, "max": 5432}
            },
            "description": "PostgreSQL from app subnet"
        },
        {
            "direction": "EGRESS",
            "protocol": "all",
            "destination": "0.0.0.0/0",
            "destinationType": "CIDR_BLOCK"
        }
    ]'
```

### 9.2 Encryption Configuration

```bash
# Create KMS vault and key
oci kms management vault create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-vault" \
    --vault-type "DEFAULT"

export VAULT_OCID=$(oci kms management vault list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --query 'data[0].id' --raw-output)

export VAULT_ENDPOINT=$(oci kms management vault get \
    --vault-id "$VAULT_OCID" \
    --query 'data."management-endpoint"' --raw-output)

# Create encryption key
oci kms management key create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --display-name "orochi-db-key" \
    --key-shape '{"algorithm": "AES", "length": 256}' \
    --endpoint "$VAULT_ENDPOINT"

export KEY_OCID=$(oci kms management key list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --endpoint "$VAULT_ENDPOINT" \
    --query 'data[0].id' --raw-output)

# Encrypt block volumes with customer-managed key
oci bv volume update \
    --volume-id "$DATA_VOL_OCID" \
    --kms-key-id "$KEY_OCID"
```

### 9.3 Secrets Management

```bash
# Create secret for database credentials
oci vault secret create-base64 \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --vault-id "$VAULT_OCID" \
    --key-id "$KEY_OCID" \
    --secret-name "orochi-db-password" \
    --secret-content-content "$(echo -n 'YourSecurePassword' | base64)"

# Retrieve secret in application
oci secrets secret-bundle get \
    --secret-id "$SECRET_OCID" \
    --stage CURRENT \
    --query 'data."secret-bundle-content".content' \
    --raw-output | base64 -d
```

### 9.4 Audit Logging

```bash
# Enable audit logging
oci audit config update \
    --compartment-id "$OCI_TENANCY_OCID" \
    --retention-period-days 365

# Query audit logs
oci audit event list \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --start-time "2024-01-01T00:00:00Z" \
    --end-time "2024-01-31T23:59:59Z" \
    --query 'data[?contains("event-type", `database`) || contains("event-type", `compute`)]'
```

---

## 10. Troubleshooting

### 10.1 Common Issues

#### Instance Not Starting

```bash
# Check instance console history
oci compute instance get-windows-initial-creds \
    --instance-id "$INSTANCE_OCID"

# View console connection
oci compute instance-console-connection create \
    --instance-id "$INSTANCE_OCID"

# Check cloud-init logs
ssh opc@<bastion> "ssh opc@<instance-ip> 'sudo cat /var/log/cloud-init-output.log'"
```

#### Database Connection Issues

```bash
# Test connectivity from bastion
ssh opc@<bastion> "nc -zv <db-ip> 5432"

# Check PostgreSQL logs
ssh opc@<instance-ip> "sudo tail -100 /var/lib/pgsql/16/data/log/postgresql-*.log"

# Verify security list rules
oci network security-list get \
    --security-list-id "$DB_SECLIST_OCID" \
    --query 'data."ingress-security-rules"'
```

#### Object Storage Access Issues

```bash
# Test instance principal authentication
ssh opc@<instance-ip> << 'EOF'
curl -s http://169.254.169.254/opc/v1/identity/
oci os ns get --auth instance_principal
oci os bucket list --compartment-id $OCI_COMPARTMENT_OCID --auth instance_principal
EOF

# Check IAM policies
oci iam policy list \
    --compartment-id "$OCI_TENANCY_OCID" \
    --query 'data[?contains(name, `OrochDB`)]'
```

#### Performance Issues

```bash
# Check instance metrics
oci monitoring metric-data summarize-metrics-data \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --namespace "oci_computeagent" \
    --query-text "CpuUtilization[1h]{resourceId = \"$INSTANCE_OCID\"}.mean()"

# Check block volume performance
oci monitoring metric-data summarize-metrics-data \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --namespace "oci_blockstore" \
    --query-text "VolumeReadThroughput[1h]{resourceId = \"$DATA_VOL_OCID\"}.max()"

# PostgreSQL performance queries
sudo -u postgres psql -d orochi_production << 'EOF'
-- Long running queries
SELECT pid, now() - pg_stat_activity.query_start AS duration, query, state
FROM pg_stat_activity
WHERE (now() - pg_stat_activity.query_start) > interval '5 minutes'
ORDER BY duration DESC;

-- Lock contention
SELECT blocked_locks.pid AS blocked_pid,
       blocked_activity.usename AS blocked_user,
       blocking_locks.pid AS blocking_pid,
       blocking_activity.usename AS blocking_user,
       blocked_activity.query AS blocked_statement
FROM pg_catalog.pg_locks blocked_locks
JOIN pg_catalog.pg_stat_activity blocked_activity ON blocked_activity.pid = blocked_locks.pid
JOIN pg_catalog.pg_locks blocking_locks ON blocking_locks.locktype = blocked_locks.locktype
JOIN pg_catalog.pg_stat_activity blocking_activity ON blocking_activity.pid = blocking_locks.pid
WHERE NOT blocked_locks.granted;

-- Orochi shard statistics
SELECT * FROM orochi_shard_stats();
EOF
```

### 10.2 Support Commands

```bash
# Collect diagnostic bundle
oci db autonomous-database generate-awr-snapshot \
    --autonomous-database-id "$ADB_OCID"

# Create support request
oci support incident create \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --problem-type "technical" \
    --severity "high" \
    --title "Orochi DB Performance Issue" \
    --description "Database experiencing high latency..."
```

---

## Quick Reference

### Essential Commands

```bash
# Check cluster status
oci ce cluster get --cluster-id "$CLUSTER_OCID" --query 'data."lifecycle-state"'

# Scale node pool
oci ce node-pool update --node-pool-id "$NODEPOOL_OCID" --size 5

# Resize block volume
oci bv volume update --volume-id "$DATA_VOL_OCID" --size-in-gbs 2048

# Create manual backup
oci os object put \
    --bucket-name "orochi-backups" \
    --file /tmp/backup.sql.gz \
    --name "manual/backup-$(date +%Y%m%d).sql.gz"

# List recent alarms
oci monitoring alarm-status list-alarms-status \
    --compartment-id "$OCI_COMPARTMENT_OCID" \
    --query 'data[?status==`FIRING`]'
```

### Resource Sizing Guide

| Workload | Shape | OCPUs | Memory | Data Volume | WAL Volume |
|----------|-------|-------|--------|-------------|------------|
| Dev/Test | VM.Standard.E4.Flex | 2 | 16GB | 100GB | 50GB |
| Small Prod | VM.Standard.E4.Flex | 4 | 32GB | 500GB | 100GB |
| Medium Prod | VM.Standard.E4.Flex | 8 | 128GB | 1TB | 256GB |
| Large Prod | VM.Optimized3.Flex | 16 | 256GB | 4TB | 512GB |
| Enterprise | BM.Standard.E4.128 | 128 | 2TB | 10TB+ | 1TB |

---

## Additional Resources

- [OCI Documentation](https://docs.oracle.com/en-us/iaas/Content/home.htm)
- [OCI CLI Reference](https://docs.oracle.com/en-us/iaas/tools/oci-cli/latest/oci_cli_docs/)
- [OKE Documentation](https://docs.oracle.com/en-us/iaas/Content/ContEng/home.htm)
- [Database Service for PostgreSQL](https://docs.oracle.com/en-us/iaas/Content/postgresql/home.htm)
- [Orochi DB Documentation](/home/user/orochi-db/docs/user-guide.md)
