# DigitalOcean Kubernetes Deployment

This directory contains DigitalOcean-specific Kubernetes manifests for Orochi DB.

## Prerequisites

1. **DigitalOcean Kubernetes (DOKS) Cluster**
   ```bash
   # Create cluster via doctl CLI
   doctl kubernetes cluster create orochi-production \
     --region nyc1 \
     --version 1.28 \
     --node-pool "name=orochi-workers;size=s-4vcpu-8gb;count=3;auto-scale=true;min-nodes=3;max-nodes=10"

   # Get kubeconfig
   doctl kubernetes cluster kubeconfig save orochi-production
   ```

2. **CSI Driver** (pre-installed on DOKS)
   ```bash
   # Verify DigitalOcean CSI driver is available
   kubectl get csidriver dobs.csi.digitalocean.com
   ```

3. **CloudNativePG Operator**
   ```bash
   kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml
   ```

## Storage Classes

Deploy the tiered storage classes:

```bash
# Deploy all storage classes
kubectl apply -f storage-classes.yaml

# Verify storage classes
kubectl get storageclasses | grep orochi
```

This creates four storage classes:
- `orochi-hot-tier` - High-performance SSD (immediate binding, delete policy)
- `orochi-warm-tier` - Standard SSD (deferred binding, retain policy)
- `orochi-cold-tier` - Cost-optimized for backups (deferred binding, retain policy)
- `orochi-default` - General-purpose SSD

## DigitalOcean Block Storage Characteristics

### Performance

| Volume Size | IOPS     | Throughput |
|-------------|----------|------------|
| 10 GB       | 400      | Limited    |
| 50 GB       | 2,000    | Limited    |
| 100 GB      | 4,000    | Limited    |
| 200 GB+     | 7,500    | Limited    |

**Notes:**
- IOPS formula: `min(size_in_gb × 40, 7500)`
- All volumes are SSD-based
- Throughput is not explicitly specified by DigitalOcean

### Pricing (as of 2024)

- **Block Storage**: $0.10/GB/month
- **Snapshots**: $0.05/GB/month
- **No additional IOPS charges** (included with volume)

### Limits

- **Min volume size**: 1 GB
- **Max volume size**: 16 TB
- **Max IOPS per volume**: 7,500
- **Max volumes per Droplet**: 7
- **Max total storage per Droplet**: 16 TB

## Example Deployments

### Hot Tier Example (OLTP Database)

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
      storage: 200Gi  # 7,500 IOPS
```

### Warm Tier Example (Historical Data)

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
      storage: 100Gi  # 4,000 IOPS
```

### Cold Tier Example (Backups)

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
      storage: 500Gi  # 7,500 IOPS (but sequential access)
```

## CloudNativePG Integration

### Using Hot Tier for Primary

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: orochi-production
spec:
  instances: 3

  storage:
    storageClass: orochi-hot-tier
    size: 200Gi

  walStorage:
    storageClass: orochi-cold-tier
    size: 100Gi

  # ... rest of cluster spec
```

### Multi-Tier Architecture

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: orochi-tiered
spec:
  instances: 3

  # Hot tier for primary data
  storage:
    storageClass: orochi-hot-tier
    size: 200Gi

  # Cold tier for WAL archives
  walStorage:
    storageClass: orochi-cold-tier
    size: 200Gi

  # Backup to cold tier
  backup:
    barmanObjectStore:
      destinationPath: s3://orochi-backups/
      s3Credentials:
        # DigitalOcean Spaces credentials

    # Local backup to cold tier (optional)
    volumeSnapshot:
      className: orochi-cold-tier
```

## Monitoring

### Check Volume Usage

```bash
# List all PVCs in cluster
kubectl get pvc -A -o wide

# Get specific PVC details
kubectl describe pvc orochi-oltp-primary

# Check volume usage in pod
kubectl exec -it <pod-name> -- df -h /pgdata
```

### Monitor IOPS

DigitalOcean provides metrics in the cloud console:
1. Navigate to Kubernetes → Clusters → Your Cluster
2. Select Volumes tab
3. View IOPS graphs for each volume

### Cost Monitoring

```bash
# Calculate monthly storage cost
# Formula: (total_gb × $0.10) + (snapshot_gb × $0.05)

# Example for production setup:
# Hot: 600 GB = $60
# Warm: 200 GB = $20
# Cold: 500 GB = $50
# Snapshots: 300 GB = $15
# Total: $145/month
```

## Best Practices

### 1. Volume Sizing for IOPS

Always provision at least 200 GB for hot tier volumes to get maximum IOPS (7,500):

```yaml
# Good - Maximum IOPS
storage:
  size: 200Gi  # 7,500 IOPS

# Suboptimal - Limited IOPS
storage:
  size: 50Gi   # Only 2,000 IOPS
```

### 2. Use Snapshots for Backups

DigitalOcean Spaces (S3-compatible) is cheaper for long-term backups:

```yaml
backup:
  barmanObjectStore:
    destinationPath: s3://my-bucket/backups/
    endpointURL: https://nyc3.digitaloceanspaces.com
    s3Credentials:
      accessKeyId:
        name: do-spaces-credentials
        key: access-key-id
      secretAccessKey:
        name: do-spaces-credentials
        key: secret-access-key
```

**Cost Comparison:**
- Block Storage: $0.10/GB/month
- Spaces: $0.02/GB/month (after first 250 GB free)

### 3. Regional Considerations

Place volumes in the same region as your cluster:

```bash
# Check cluster region
doctl kubernetes cluster get orochi-production --format Region

# All volumes must be in the same region
# DigitalOcean automatically handles this with WaitForFirstConsumer
```

### 4. Volume Expansion

Expand volumes without downtime:

```bash
# Edit PVC
kubectl patch pvc orochi-oltp-primary -p '{"spec":{"resources":{"requests":{"storage":"300Gi"}}}}'

# CloudNativePG will automatically handle the resize
# Monitor progress
kubectl get pvc orochi-oltp-primary --watch
```

## Troubleshooting

### Volume Stuck in Pending

```bash
# Check PVC events
kubectl describe pvc <pvc-name>

# Common issues:
# 1. Quota exceeded - check DigitalOcean account limits
# 2. Region mismatch - ensure pod and volume in same region
# 3. CSI driver issues - check driver logs

# Check CSI driver
kubectl logs -n kube-system -l app=csi-do-controller
kubectl logs -n kube-system -l app=csi-do-node
```

### Low IOPS Performance

```bash
# Check current volume size
kubectl get pvc <pvc-name> -o jsonpath='{.spec.resources.requests.storage}'

# If < 200Gi, expand to get max IOPS
kubectl patch pvc <pvc-name> -p '{"spec":{"resources":{"requests":{"storage":"200Gi"}}}}'
```

### Volume Not Detaching

If a volume won't detach from a deleted pod:

```bash
# Force delete the pod
kubectl delete pod <pod-name> --force --grace-period=0

# If still stuck, check DigitalOcean console
# Manually detach volume from Droplet
```

## Migration from Other Providers

### AWS to DigitalOcean

```bash
# 1. Create snapshot/backup on AWS
# 2. Export to S3
# 3. Copy to DigitalOcean Spaces
# 4. Restore to new cluster on DigitalOcean

# Example: pg_dump/pg_restore
kubectl exec -it aws-cluster-1 -- pg_dump -Fc mydb > backup.dump
kubectl cp backup.dump do-cluster-1:/tmp/
kubectl exec -it do-cluster-1 -- pg_restore -d mydb /tmp/backup.dump
```

## Cost Optimization

### Right-Sizing Strategy

1. **Start Conservative**: Begin with 200 GB hot tier volumes
2. **Monitor Usage**: Track actual storage usage over 2-4 weeks
3. **Adjust**: Expand only when needed (>80% full)
4. **Tier Data**: Move inactive data to warm tier manually or via automation

### Backup Strategy

1. **Use Spaces for Long-Term**: Configure barman to use DigitalOcean Spaces
2. **Limit Block Storage Snapshots**: Keep only last 7 days on block storage
3. **Retention Policies**: Delete old backups automatically

```yaml
backup:
  retentionPolicy: "7d"  # Keep only 7 days
  barmanObjectStore:
    destinationPath: s3://orochi-backups/
    wal:
      compression: gzip
      maxParallel: 4
```

## Security

### Volume Encryption

DigitalOcean Block Storage is **not encrypted by default**. For encryption at rest:

1. Use PostgreSQL's native encryption (pgcrypto)
2. Implement application-level encryption
3. Consider DigitalOcean Spaces with encryption for backups

### Access Control

```yaml
# Use RBAC to restrict PVC access
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: orochi-storage-admin
rules:
  - apiGroups: [""]
    resources: ["persistentvolumeclaims"]
    verbs: ["create", "delete", "get", "list", "update", "patch"]
```

## References

- [DigitalOcean Block Storage Docs](https://docs.digitalocean.com/products/volumes/)
- [DigitalOcean Kubernetes CSI](https://github.com/digitalocean/csi-digitalocean)
- [CloudNativePG Storage Configuration](https://cloudnative-pg.io/documentation/current/storage/)
- [Kubernetes CSI Documentation](https://kubernetes-csi.github.io/docs/)
