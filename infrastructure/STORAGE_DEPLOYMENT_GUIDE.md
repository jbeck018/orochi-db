# Orochi DB Tiered Storage Deployment Guide

Complete guide for deploying Orochi DB with tiered storage on Kubernetes.

## Overview

Orochi DB implements a four-tier storage architecture optimized for different data access patterns and cost requirements:

| Tier    | Performance | Use Case                          | Reclaim Policy | Binding Mode           |
|---------|-------------|-----------------------------------|----------------|------------------------|
| Hot     | High IOPS   | Active OLTP data                  | Delete         | Immediate              |
| Warm    | Medium IOPS | Historical/analytical data        | Retain         | WaitForFirstConsumer   |
| Cold    | Low IOPS    | Backups, archives, WAL            | Retain         | WaitForFirstConsumer   |
| Default | Balanced    | General-purpose (non-tiered)      | Delete         | WaitForFirstConsumer   |

## Directory Structure

```
infrastructure/
├── manifests/
│   └── storage-classes/          # Individual StorageClass files
│       ├── fast-ssd.yaml         # Hot tier
│       ├── standard.yaml         # Warm tier
│       ├── cold-archive.yaml     # Cold tier
│       └── README.md             # Detailed documentation
│
├── digitalocean/                 # DigitalOcean-specific deployment
│   ├── storage-classes.yaml      # All tiers combined
│   ├── example-cluster.yaml      # Complete cluster example
│   ├── QUICKSTART.md             # 15-minute deployment guide
│   └── README.md                 # DigitalOcean-specific docs
│
└── helm/
    └── orochi-cloud/
        ├── templates/
        │   └── storage-classes.yaml  # Multi-cloud Helm template
        ├── values.yaml               # Default values with storage config
        └── values-production.yaml    # Production values
```

## Deployment Options

### Option 1: Individual StorageClass Files

Deploy specific tiers as needed:

```bash
# Deploy only hot tier
kubectl apply -f infrastructure/manifests/storage-classes/fast-ssd.yaml

# Deploy warm tier
kubectl apply -f infrastructure/manifests/storage-classes/standard.yaml

# Deploy cold tier
kubectl apply -f infrastructure/manifests/storage-classes/cold-archive.yaml
```

**Use Case:** Fine-grained control, testing individual tiers, or selective deployment.

### Option 2: Combined DigitalOcean Manifest

Deploy all tiers at once (DigitalOcean-optimized):

```bash
# Deploy all storage classes
kubectl apply -f infrastructure/digitalocean/storage-classes.yaml

# Deploy example cluster with tiered storage
kubectl apply -f infrastructure/digitalocean/example-cluster.yaml
```

**Use Case:** DigitalOcean deployments, quick start, all-in-one deployment.

### Option 3: Helm Chart (Multi-Cloud)

Deploy using Helm with cloud provider abstraction:

```bash
# Install with default values (DigitalOcean)
helm install orochi-cloud infrastructure/helm/orochi-cloud \
  --namespace orochi-cloud \
  --create-namespace

# Install for AWS
helm install orochi-cloud infrastructure/helm/orochi-cloud \
  --set storage.provider=aws \
  --set storage.hotTier.parameters.type=gp3 \
  --set storage.hotTier.parameters.iops=16000

# Install for GCP
helm install orochi-cloud infrastructure/helm/orochi-cloud \
  --set storage.provider=gcp \
  --set storage.hotTier.parameters.type=pd-ssd

# Install for production (DigitalOcean)
helm install orochi-cloud infrastructure/helm/orochi-cloud \
  -f infrastructure/helm/orochi-cloud/values-production.yaml
```

**Use Case:** Production deployments, multi-cloud support, GitOps workflows.

## Cloud Provider Support

### DigitalOcean (Default)

**Provisioner:** `dobs.csi.digitalocean.com`

**Configuration:**
```yaml
storage:
  provider: digitalocean
  hotTier:
    parameters:
      type: "ext4"
```

**Characteristics:**
- All volumes are SSD-based
- IOPS: 40 per GB (max 7,500)
- Volume size: 1 GB to 16 TB
- Pricing: ~$0.10/GB/month

**Quick Start:** See `infrastructure/digitalocean/QUICKSTART.md`

### AWS EBS

**Provisioner:** `ebs.csi.aws.com`

**Configuration:**
```yaml
storage:
  provider: aws
  hotTier:
    parameters:
      type: gp3
      iops: "16000"
      throughput: "1000"
      encrypted: "true"
  warmTier:
    parameters:
      type: gp3
      iops: "3000"
      throughput: "125"
      encrypted: "true"
  coldTier:
    parameters:
      type: st1  # Throughput-optimized HDD
      encrypted: "true"
```

**Storage Types:**
- Hot: `gp3` (general-purpose SSD, high IOPS)
- Warm: `gp3` (general-purpose SSD, moderate IOPS)
- Cold: `st1` (throughput-optimized HDD)

### Google Cloud Platform

**Provisioner:** `pd.csi.storage.gke.io`

**Configuration:**
```yaml
storage:
  provider: gcp
  hotTier:
    parameters:
      type: pd-ssd
      replication-type: regional-pd
  warmTier:
    parameters:
      type: pd-balanced
      replication-type: regional-pd
  coldTier:
    parameters:
      type: pd-standard
      replication-type: regional-pd
```

**Storage Types:**
- Hot: `pd-ssd` (SSD persistent disk)
- Warm: `pd-balanced` (balanced persistent disk)
- Cold: `pd-standard` (standard persistent disk)

### Microsoft Azure

**Provisioner:** `disk.csi.azure.com`

**Configuration:**
```yaml
storage:
  provider: azure
  hotTier:
    parameters:
      storageaccounttype: Premium_LRS
      kind: Managed
  warmTier:
    parameters:
      storageaccounttype: StandardSSD_LRS
      kind: Managed
  coldTier:
    parameters:
      storageaccounttype: Standard_LRS
      kind: Managed
```

**Storage Types:**
- Hot: `Premium_LRS` (Premium SSD)
- Warm: `StandardSSD_LRS` (Standard SSD)
- Cold: `Standard_LRS` (Standard HDD)

## Usage Examples

### Hot Tier (OLTP Workload)

```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-oltp-primary
  labels:
    orochi.db/tier: hot
    orochi.db/workload: oltp
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-hot-tier
  resources:
    requests:
      storage: 200Gi
```

### Warm Tier (Analytics Workload)

```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-analytics-archive
  labels:
    orochi.db/tier: warm
    orochi.db/workload: analytics
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-warm-tier
  resources:
    requests:
      storage: 100Gi
```

### Cold Tier (Backup Storage)

```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: orochi-backup-storage
  labels:
    orochi.db/tier: cold
    orochi.db/purpose: backup
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: orochi-cold-tier
  resources:
    requests:
      storage: 500Gi
```

### CloudNativePG Integration

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: orochi-production
spec:
  instances: 3

  # Hot tier for primary data
  storage:
    storageClass: orochi-hot-tier
    size: 200Gi

  # Cold tier for WAL archives
  walStorage:
    storageClass: orochi-cold-tier
    size: 100Gi

  # Backup configuration
  backup:
    barmanObjectStore:
      destinationPath: s3://orochi-backups/
      # ... S3/Spaces credentials
```

## Performance Optimization

### IOPS Considerations

**DigitalOcean:**
- Formula: `IOPS = min(size_gb × 40, 7500)`
- Minimum 200 GB for maximum IOPS (7,500)

**AWS:**
- gp3: 3,000 baseline IOPS, configurable up to 16,000
- io2: Up to 64,000 IOPS per volume

**GCP:**
- pd-ssd: 30 IOPS/GB read, 30 IOPS/GB write

**Azure:**
- Premium_LRS: Up to 20,000 IOPS per disk

### Mount Options

All storage classes use optimized mount options:

```yaml
mountOptions:
  - noatime        # Don't update access times (reduces writes)
  - nodiratime     # Don't update directory access times
  - discard        # Enable TRIM for SSDs
  - barrier=0      # Disable barriers (hot tier only)
  - data=writeback # Optimize for sequential writes (cold tier only)
```

## Cost Estimation

### DigitalOcean Example (Production)

```
Storage Costs:
- Hot tier:  3 × 200 GB = 600 GB × $0.10 = $60/month
- Warm tier: 2 × 100 GB = 200 GB × $0.10 = $20/month
- Cold tier: 1 × 500 GB = 500 GB × $0.10 = $50/month
- Backups (Spaces): 500 GB × $0.02 = $10/month (after 250 GB free)

Total Storage: $140/month

Kubernetes Cluster:
- 3 nodes (s-4vcpu-8gb): 3 × $40 = $120/month

Total Monthly Cost: ~$260/month
```

### Cost Optimization Strategies

1. **Use Object Storage for Backups**
   - DigitalOcean Spaces: $0.02/GB vs $0.10/GB block storage
   - AWS S3: $0.023/GB (Standard) vs $0.08/GB (EBS)

2. **Right-Size Volumes**
   - Start with minimum needed size
   - Expand only when >80% full

3. **Lifecycle Management**
   - Move data: Hot → Warm → Cold → Object Storage
   - Delete old backups automatically

4. **Volume Snapshots**
   - Cheaper than maintaining duplicate volumes
   - DigitalOcean: $0.05/GB/month for snapshots

## Monitoring

### Check Storage Classes

```bash
# List all storage classes
kubectl get storageclasses

# Get details
kubectl describe storageclass orochi-hot-tier
```

### Monitor PVCs and PVs

```bash
# List all PVCs
kubectl get pvc -A

# Get PVC usage
kubectl exec -it <pod-name> -- df -h /pgdata

# Check PV details
kubectl get pv
kubectl describe pv <pv-name>
```

### Set Up Alerts

```yaml
# Example Prometheus alert
- alert: HighStorageUsage
  expr: kubelet_volume_stats_used_bytes / kubelet_volume_stats_capacity_bytes > 0.8
  for: 5m
  annotations:
    summary: "PVC {{ $labels.persistentvolumeclaim }} is {{ $value }}% full"
```

## Troubleshooting

### Volume Not Binding

```bash
# Check PVC events
kubectl describe pvc <pvc-name>

# Check CSI driver logs
kubectl logs -n kube-system -l app=csi-controller
```

### Performance Issues

```bash
# Check IOPS allocation
kubectl describe pv <pv-name> | grep -E "Capacity|Type|IOPS"

# Expand volume for more IOPS
kubectl patch pvc <pvc-name> -p '{"spec":{"resources":{"requests":{"storage":"300Gi"}}}}'
```

### Volume Expansion Stuck

```bash
# Check filesystem resize
kubectl exec -it <pod-name> -- resize2fs /dev/vdb

# Monitor resize operation
kubectl get pvc <pvc-name> --watch
```

## Migration Between Tiers

### Manual Data Movement

```sql
-- Move data from hot to warm tier
-- 1. Create new table in warm tier
CREATE TABLE users_archive (LIKE users INCLUDING ALL);

-- 2. Move old data
INSERT INTO users_archive
SELECT * FROM users WHERE created_at < NOW() - INTERVAL '30 days';

-- 3. Delete from hot tier
DELETE FROM users WHERE created_at < NOW() - INTERVAL '30 days';

-- 4. Verify and swap (optional)
-- ALTER TABLE users RENAME TO users_temp;
-- ALTER TABLE users_archive RENAME TO users;
```

### Automated Tiering (Future)

Orochi DB will support automatic data tiering:

```sql
-- Configure automatic tiering
ALTER TABLE users SET (
    orochi.enable_tiering = true,
    orochi.hot_tier_threshold = '7 days',
    orochi.warm_tier_threshold = '30 days',
    orochi.cold_tier_threshold = '90 days'
);
```

## Best Practices

1. **Use Retain Policy for Critical Data**
   - Warm and cold tiers use `Retain` by default
   - Prevents accidental data loss on PVC deletion

2. **Enable Volume Expansion**
   - All storage classes have `allowVolumeExpansion: true`
   - No downtime required for expansion

3. **Plan for Growth**
   - Provision hot tier with 200 GB minimum (max IOPS)
   - Monitor usage and expand proactively

4. **Use Labels**
   - Tag PVCs with `orochi.db/tier` and `orochi.db/purpose`
   - Easier management and cost tracking

5. **Test Disaster Recovery**
   - Regularly test backup restoration
   - Verify backup integrity

6. **Monitor Performance**
   - Set up alerts for storage usage >80%
   - Track IOPS utilization

## Security Considerations

### Encryption at Rest

**AWS:**
```yaml
parameters:
  encrypted: "true"
  kmsKeyId: "arn:aws:kms:..."
```

**GCP:**
```yaml
parameters:
  disk-encryption-kms-key: "projects/.../keyRings/.../cryptoKeys/..."
```

**Azure:**
```yaml
parameters:
  diskEncryptionSetID: "/subscriptions/.../diskEncryptionSets/..."
```

### RBAC

```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: orochi-storage-admin
rules:
  - apiGroups: [""]
    resources: ["persistentvolumeclaims"]
    verbs: ["create", "delete", "get", "list", "update", "patch"]
```

## Documentation

- **Detailed Storage Guide**: `infrastructure/manifests/storage-classes/README.md`
- **DigitalOcean Quick Start**: `infrastructure/digitalocean/QUICKSTART.md`
- **DigitalOcean Docs**: `infrastructure/digitalocean/README.md`
- **Helm Values**: `infrastructure/helm/orochi-cloud/values.yaml`

## Support

For issues, questions, or contributions:

- GitHub Issues: Report bugs or request features
- Documentation: https://docs.orochi-db.io
- Community: Join Discord/Slack for discussions
- Commercial Support: Contact Orochi DB team

## References

- [Kubernetes Storage Classes](https://kubernetes.io/docs/concepts/storage/storage-classes/)
- [CloudNativePG Storage](https://cloudnative-pg.io/documentation/current/storage/)
- [DigitalOcean Block Storage](https://docs.digitalocean.com/products/volumes/)
- [AWS EBS CSI Driver](https://github.com/kubernetes-sigs/aws-ebs-csi-driver)
- [GCP Persistent Disk CSI](https://github.com/kubernetes-sigs/gcp-compute-persistent-disk-csi-driver)
- [Azure Disk CSI Driver](https://github.com/kubernetes-sigs/azuredisk-csi-driver)
