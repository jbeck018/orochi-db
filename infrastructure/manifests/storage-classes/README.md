# Orochi DB Tiered Storage Classes

This directory contains Kubernetes StorageClass definitions for Orochi DB's tiered storage architecture.

## Overview

Orochi DB implements a four-tier storage system optimized for different data access patterns and cost requirements:

1. **Hot Tier**: High-performance SSD storage for frequently accessed data
2. **Warm Tier**: Standard SSD storage for moderately accessed data
3. **Cold Tier**: Cost-optimized storage for backups, archives, and WAL files
4. **Default**: General-purpose storage for clusters not using tiered storage

## Storage Classes

### Hot Tier (`orochi-hot-tier`)

**Use Cases:**
- Active database tables with frequent reads/writes
- Primary data storage for OLTP workloads
- Real-time analytics data
- High-performance indexes

**Characteristics:**
- **Volume Binding**: Immediate (volume created when PVC is created)
- **Reclaim Policy**: Delete (volume deleted when PVC is deleted)
- **Expansion**: Enabled
- **IOPS**: 40 per GB (up to 7,500 IOPS on DigitalOcean)
- **Mount Options**: `noatime`, `nodiratime`, `discard`, `barrier=0`

**Recommended Minimum Size**: 100 GB (for optimal IOPS: 4,000)

**Example Usage:**
```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-hot-data
  labels:
    orochi.db/tier: hot
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-hot-tier
  resources:
    requests:
      storage: 100Gi
```

### Warm Tier (`orochi-warm-tier`)

**Use Cases:**
- Historical data accessed less frequently
- Reporting and analytics datasets
- Intermediate storage for data migration
- Archived operational data

**Characteristics:**
- **Volume Binding**: WaitForFirstConsumer (volume created when pod is scheduled)
- **Reclaim Policy**: Retain (manual deletion required)
- **Expansion**: Enabled
- **IOPS**: 40 per GB on DigitalOcean
- **Mount Options**: `noatime`, `nodiratime`, `discard`

**Recommended Minimum Size**: 50 GB

**Example Usage:**
```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-warm-data
  labels:
    orochi.db/tier: warm
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-warm-tier
  resources:
    requests:
      storage: 50Gi
```

### Cold Tier (`orochi-cold-tier`)

**Use Cases:**
- Long-term backups
- PostgreSQL WAL archives
- Disaster recovery data
- Compliance and audit logs
- Rarely accessed historical data

**Characteristics:**
- **Volume Binding**: WaitForFirstConsumer
- **Reclaim Policy**: Retain (prevents accidental deletion)
- **Expansion**: Enabled
- **IOPS**: Standard (sufficient for sequential backup workloads)
- **Mount Options**: `noatime`, `nodiratime`, `discard`, `data=writeback`

**Recommended Minimum Size**: 200 GB

**Example Usage:**
```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-wal-archive
  labels:
    orochi.db/tier: cold
    orochi.db/purpose: wal-archive
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-cold-tier
  resources:
    requests:
      storage: 200Gi
```

### Default (`orochi-default`)

**Use Cases:**
- Single-tier PostgreSQL clusters
- Development and testing environments
- Workloads without specific tiering requirements

**Characteristics:**
- **Volume Binding**: WaitForFirstConsumer
- **Reclaim Policy**: Delete
- **Expansion**: Enabled
- **Mount Options**: `noatime`, `nodiratime`, `discard`

## Deployment

### Individual Files

Deploy individual storage classes:

```bash
# Hot tier only
kubectl apply -f fast-ssd.yaml

# Warm tier only
kubectl apply -f standard.yaml

# Cold tier only
kubectl apply -f cold-archive.yaml
```

### All at Once

Deploy all storage classes:

```bash
# DigitalOcean (includes all tiers + default)
kubectl apply -f ../digitalocean/storage-classes.yaml
```

### Via Helm

Deploy with the Helm chart:

```bash
# Development
helm install orochi-cloud ../../helm/orochi-cloud \
  --set storage.createStorageClasses=true \
  --set storage.provider=digitalocean

# Production
helm install orochi-cloud ../../helm/orochi-cloud \
  -f ../../helm/orochi-cloud/values-production.yaml
```

## Cloud Provider Support

### DigitalOcean (Default)

**Provisioner**: `dobs.csi.digitalocean.com`

**Pricing** (as of 2024):
- All volumes: ~$0.10/GB/month
- IOPS: 40 per GB (included)
- Snapshots: Additional cost

**Volume Limits**:
- Min size: 1 GB
- Max size: 16 TB
- Max IOPS: 7,500

**Prerequisites**:
```bash
# DigitalOcean CSI driver is pre-installed on DOKS clusters
kubectl get csidriver dobs.csi.digitalocean.com
```

### AWS EBS

**Provisioner**: `ebs.csi.aws.com`

**Storage Types**:
- Hot: `gp3` (16,000 IOPS, 1,000 MB/s throughput)
- Warm: `gp3` (3,000 IOPS, 125 MB/s throughput)
- Cold: `st1` (Throughput-optimized HDD)

**Configuration**:
```yaml
storage:
  provider: aws
  hotTier:
    parameters:
      type: gp3
      iops: "16000"
      throughput: "1000"
      encrypted: "true"
```

### Google Cloud (GCP)

**Provisioner**: `pd.csi.storage.gke.io`

**Storage Types**:
- Hot: `pd-ssd` (regional)
- Warm: `pd-balanced` (regional)
- Cold: `pd-standard` (regional)

**Configuration**:
```yaml
storage:
  provider: gcp
  hotTier:
    parameters:
      type: pd-ssd
      replication-type: regional-pd
```

### Azure

**Provisioner**: `disk.csi.azure.com`

**Storage Types**:
- Hot: `Premium_LRS` (Premium SSD)
- Warm: `StandardSSD_LRS` (Standard SSD)
- Cold: `Standard_LRS` (Standard HDD)

**Configuration**:
```yaml
storage:
  provider: azure
  hotTier:
    parameters:
      storageaccounttype: Premium_LRS
      kind: Managed
```

## Performance Optimization

### Mount Options

The storage classes use optimized mount options:

- **`noatime`**: Don't update file access times (reduces writes)
- **`nodiratime`**: Don't update directory access times
- **`discard`**: Enable TRIM for SSD performance and space reclamation
- **`barrier=0`**: Disable write barriers (safe with modern storage)
- **`data=writeback`**: Optimize for sequential writes (cold tier only)

### IOPS Calculation (DigitalOcean)

IOPS = min(volume_size_gb × 40, 7500)

Examples:
- 10 GB → 400 IOPS
- 50 GB → 2,000 IOPS
- 100 GB → 4,000 IOPS
- 200 GB → 7,500 IOPS (maxed out)

For optimal hot tier performance, provision at least 200 GB.

## Monitoring

### Check Storage Classes

```bash
# List all storage classes
kubectl get storageclasses

# Get details for hot tier
kubectl describe storageclass orochi-hot-tier
```

### Check PVCs and PVs

```bash
# List all PVCs
kubectl get pvc -n orochi-cloud

# Get PVC details
kubectl describe pvc orochi-hot-data -n orochi-cloud

# List all PVs
kubectl get pv

# Get PV details
kubectl describe pv <pv-name>
```

### Storage Metrics

```bash
# Check volume usage in pod
kubectl exec -it <pod-name> -n orochi-cloud -- df -h /pgdata

# Get PVC events
kubectl get events --field-selector involvedObject.name=<pvc-name> -n orochi-cloud
```

## Troubleshooting

### Volume Not Binding

If a PVC remains in `Pending` state:

```bash
# Check PVC events
kubectl describe pvc <pvc-name> -n orochi-cloud

# Common issues:
# 1. No available storage in the zone
# 2. CSI driver not installed
# 3. Insufficient permissions

# Check CSI driver
kubectl get csidriver
kubectl logs -n kube-system -l app=csi-do-controller
```

### Insufficient IOPS

If database performance is poor:

```bash
# Expand the volume to increase IOPS
kubectl patch pvc orochi-hot-data -n orochi-cloud -p '{"spec":{"resources":{"requests":{"storage":"200Gi"}}}}'

# Monitor the resize operation
kubectl get pvc orochi-hot-data -n orochi-cloud --watch
```

### Volume Expansion Stuck

```bash
# Check if filesystem resize is needed
kubectl exec -it <pod-name> -n orochi-cloud -- df -h /pgdata

# Manually trigger filesystem resize (if needed)
kubectl exec -it <pod-name> -n orochi-cloud -- resize2fs /dev/vdb
```

### Reclaim Policy Issues

If you need to change reclaim policy on existing volumes:

```bash
# Change from Delete to Retain
kubectl patch pv <pv-name> -p '{"spec":{"persistentVolumeReclaimPolicy":"Retain"}}'

# Change from Retain to Delete (dangerous!)
kubectl patch pv <pv-name> -p '{"spec":{"persistentVolumeReclaimPolicy":"Delete"}}'
```

## Best Practices

1. **Use Hot Tier for Active Data**: Provision at least 200 GB for maximum IOPS
2. **Retain Important Data**: Use `Retain` reclaim policy for warm/cold tiers
3. **Monitor Storage Growth**: Set up alerts for PVC usage > 80%
4. **Plan for Expansion**: Enable `allowVolumeExpansion` on all storage classes
5. **Backup Cold Tier**: Regularly snapshot cold tier volumes containing backups
6. **Cost Optimization**: Move inactive data from hot → warm → cold tiers
7. **Test Disaster Recovery**: Regularly test backup restoration from cold tier
8. **Use Labels**: Tag PVCs with `orochi.db/tier` and `orochi.db/purpose` labels

## Cost Estimation

### DigitalOcean Example

Monthly cost for a typical production cluster:

```
Hot Tier:  3 x 200 GB = 600 GB × $0.10 = $60/month
Warm Tier: 2 x 100 GB = 200 GB × $0.10 = $20/month
Cold Tier: 1 x 500 GB = 500 GB × $0.10 = $50/month
---------------------------------------------------
Total:                               $130/month
```

### Cost Optimization Tips

1. **Lifecycle Management**: Automate data movement between tiers
2. **Compression**: Use Orochi DB's columnar compression for warm/cold data
3. **Retention Policies**: Delete old backups from cold tier
4. **Right-Sizing**: Start small and expand volumes as needed
5. **Monitoring**: Track storage utilization and IOPS usage

## References

- [DigitalOcean Block Storage Pricing](https://www.digitalocean.com/pricing/block-storage)
- [Kubernetes Storage Classes](https://kubernetes.io/docs/concepts/storage/storage-classes/)
- [CSI Volume Expansion](https://kubernetes.io/docs/concepts/storage/persistent-volumes/#expanding-persistent-volumes-claims)
- [CloudNativePG Storage](https://cloudnative-pg.io/documentation/current/storage/)
