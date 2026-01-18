# Orochi DB on DigitalOcean - Quick Start Guide

Deploy Orochi DB with tiered storage on DigitalOcean Kubernetes in under 15 minutes.

## Prerequisites

- DigitalOcean account with CLI (`doctl`) installed and configured
- `kubectl` CLI tool
- `helm` CLI tool (optional, for Helm deployment)

## Step 1: Create DigitalOcean Kubernetes Cluster

```bash
# Create a production-ready DOKS cluster
doctl kubernetes cluster create orochi-production \
  --region nyc1 \
  --version 1.28 \
  --node-pool "name=orochi-workers;size=s-4vcpu-8gb;count=3;auto-scale=true;min-nodes=3;max-nodes=10" \
  --wait

# Get cluster credentials
doctl kubernetes cluster kubeconfig save orochi-production
```

**Cluster Details:**
- **Region**: NYC1 (change as needed)
- **Nodes**: 3x s-4vcpu-8gb (4 vCPU, 8GB RAM each)
- **Auto-scaling**: 3-10 nodes
- **Cost**: ~$120/month for worker nodes (3x $40/month)

## Step 2: Install CloudNativePG Operator

```bash
# Install CloudNativePG operator
kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml

# Wait for operator to be ready
kubectl wait --for=condition=Available --timeout=300s \
  -n cnpg-system deployment/cnpg-controller-manager
```

## Step 3: Deploy Storage Classes

```bash
# Deploy tiered storage classes
kubectl apply -f storage-classes.yaml

# Verify storage classes
kubectl get storageclasses | grep orochi
```

**Expected Output:**
```
orochi-cold-tier    dobs.csi.digitalocean.com   Delete          WaitForFirstConsumer   true     1s
orochi-default      dobs.csi.digitalocean.com   Delete          WaitForFirstConsumer   true     1s
orochi-hot-tier     dobs.csi.digitalocean.com   Delete          Immediate              true     1s
orochi-warm-tier    dobs.csi.digitalocean.com   Retain          WaitForFirstConsumer   true     1s
```

## Step 4: Create DigitalOcean Spaces Bucket (for Backups)

```bash
# Create Spaces bucket via doctl
doctl spaces create orochi-backups --region nyc3

# Create access key for Spaces
# Go to: https://cloud.digitalocean.com/account/api/tokens
# Generate new Spaces access key
# Save the Access Key and Secret Key
```

**Alternative: Use DigitalOcean Console**
1. Navigate to Spaces → Create Space
2. Name: `orochi-backups`
3. Region: NYC3 (same as cluster region)
4. Create Space

## Step 5: Configure Backup Credentials

```bash
# Create namespace
kubectl create namespace orochi-production

# Create Spaces credentials secret
kubectl create secret generic orochi-backup-credentials \
  --namespace=orochi-production \
  --from-literal=ACCESS_KEY_ID='YOUR_SPACES_KEY' \
  --from-literal=ACCESS_SECRET_KEY='YOUR_SPACES_SECRET'
```

## Step 6: Deploy Orochi DB Cluster

### Option A: Using Manifest (Recommended)

```bash
# Edit example-cluster.yaml and update:
# 1. Spaces credentials (or use the secret from Step 5)
# 2. Backup destination path
# 3. Spaces endpoint URL (match your region)

# Deploy the cluster
kubectl apply -f example-cluster.yaml

# Wait for cluster to be ready
kubectl wait --for=condition=Ready --timeout=600s \
  -n orochi-example cluster/orochi-tiered-example
```

### Option B: Using Helm

```bash
# Add Orochi Helm repository (replace with actual repo)
helm repo add orochi https://charts.orochi-cloud.io
helm repo update

# Install with values
helm install orochi-db orochi/orochi-cloud \
  --namespace orochi-production \
  --create-namespace \
  --set storage.provider=digitalocean \
  --set storage.createStorageClasses=true \
  --wait
```

## Step 7: Verify Deployment

```bash
# Check cluster status
kubectl get cluster -n orochi-example

# Check pods
kubectl get pods -n orochi-example

# Check PVCs
kubectl get pvc -n orochi-example

# Get cluster details
kubectl describe cluster orochi-tiered-example -n orochi-example
```

**Expected Output:**
```
NAME                     AGE   INSTANCES   READY   STATUS                     PRIMARY
orochi-tiered-example    5m    3           3       Cluster in healthy state   orochi-tiered-example-1
```

## Step 8: Connect to Database

```bash
# Get the primary pod name
PRIMARY_POD=$(kubectl get pod -n orochi-example \
  -o jsonpath='{.items[?(@.metadata.labels.role=="primary")].metadata.name}')

# Port forward to local machine
kubectl port-forward -n orochi-example pod/$PRIMARY_POD 5432:5432

# Connect with psql (in another terminal)
psql -h localhost -U orochi -d orochi

# Or use password from secret
PGPASSWORD=$(kubectl get secret orochi-superuser -n orochi-example \
  -o jsonpath='{.data.password}' | base64 -d) \
  psql -h localhost -U orochi -d orochi
```

## Step 9: Enable Orochi Extension

```sql
-- Connect to database and enable extension
CREATE EXTENSION IF NOT EXISTS orochi;

-- Verify extension
SELECT * FROM orochi_version();

-- Create a distributed table
CREATE TABLE users (
    id BIGSERIAL PRIMARY KEY,
    email TEXT NOT NULL,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Distribute the table
SELECT orochi_create_distributed_table('users', 'id');

-- Create a hypertable (time-series)
CREATE TABLE metrics (
    time TIMESTAMPTZ NOT NULL,
    sensor_id INTEGER,
    value DOUBLE PRECISION
);

SELECT orochi_create_hypertable('metrics', 'time');
```

## Step 10: Test Backup and Restore

```bash
# Trigger an on-demand backup
kubectl cnpg backup orochi-tiered-example -n orochi-example

# Check backup status
kubectl get backup -n orochi-example

# View backup details
kubectl describe backup -n orochi-example <backup-name>

# Verify backup in DigitalOcean Spaces
doctl spaces ls orochi-backups --region nyc3
```

## Monitoring and Management

### Check Cluster Health

```bash
# Get cluster status
kubectl cnpg status orochi-tiered-example -n orochi-example

# View logs
kubectl logs -n orochi-example -l postgresql=orochi-tiered-example -f

# Check replication
kubectl exec -n orochi-example $PRIMARY_POD -- \
  psql -U orochi -c "SELECT * FROM pg_stat_replication;"
```

### Scale Storage

```bash
# Expand hot tier volume (200Gi → 300Gi)
kubectl patch pvc orochi-tiered-example-1 -n orochi-example \
  -p '{"spec":{"resources":{"requests":{"storage":"300Gi"}}}}'

# Monitor resize
kubectl get pvc -n orochi-example --watch
```

### Scale Instances

```bash
# Scale to 5 instances
kubectl patch cluster orochi-tiered-example -n orochi-example \
  --type=merge -p '{"spec":{"instances":5}}'

# Monitor scaling
kubectl get pods -n orochi-example --watch
```

## Cost Breakdown

### Monthly Costs (Estimated)

**Kubernetes Cluster:**
- 3x s-4vcpu-8gb nodes: 3 × $40 = $120/month

**Storage:**
- Hot tier (200Gi × 3 instances): 600 GB × $0.10 = $60/month
- WAL/Cold tier (100Gi × 3 instances): 300 GB × $0.10 = $30/month
- Backups in Spaces (500 GB): 250 GB × $0.02 = $5/month (250 GB free)

**Load Balancer (optional):**
- DigitalOcean Load Balancer: $12/month

**Total:** ~$227/month (base configuration)

### Cost Optimization

1. **Use Spaces for backups** ($0.02/GB vs $0.10/GB block storage)
2. **Enable auto-scaling** (scale down during off-peak)
3. **Right-size volumes** (start with minimum needed)
4. **Delete old backups** (retention policy: 30 days)

## Troubleshooting

### Cluster Not Starting

```bash
# Check operator logs
kubectl logs -n cnpg-system deployment/cnpg-controller-manager

# Check cluster events
kubectl describe cluster orochi-tiered-example -n orochi-example

# Check pod events
kubectl describe pod -n orochi-example <pod-name>
```

### Volume Issues

```bash
# Check PVC status
kubectl get pvc -n orochi-example
kubectl describe pvc <pvc-name> -n orochi-example

# Check CSI driver
kubectl logs -n kube-system -l app=csi-do-controller
```

### Backup Failures

```bash
# Check backup status
kubectl describe backup -n orochi-example <backup-name>

# Check Spaces credentials
kubectl get secret orochi-backup-credentials -n orochi-example -o yaml

# Test Spaces connectivity
kubectl run -it --rm debug --image=amazon/aws-cli --restart=Never -- \
  s3 ls s3://orochi-backups/ \
  --endpoint-url=https://nyc3.digitaloceanspaces.com
```

## Next Steps

1. **Configure Monitoring**: Install Prometheus and Grafana
2. **Set up Alerts**: Configure alerting for cluster health
3. **Implement CI/CD**: Automate deployments with GitHub Actions
4. **Load Testing**: Test performance with pgbench
5. **Disaster Recovery**: Test backup restoration procedures
6. **Security Hardening**: Enable network policies and RBAC

## Cleanup

```bash
# Delete cluster
kubectl delete -f example-cluster.yaml

# Delete storage classes
kubectl delete -f storage-classes.yaml

# Delete CloudNativePG operator
kubectl delete -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml

# Delete Kubernetes cluster (THIS DELETES EVERYTHING)
doctl kubernetes cluster delete orochi-production

# Delete Spaces bucket (CAUTION: Deletes all backups)
doctl spaces delete orochi-backups --region nyc3 --force
```

## Support

- **Documentation**: See `README.md` and `../manifests/storage-classes/README.md`
- **Issues**: GitHub Issues
- **Community**: Discord/Slack (if available)
- **Commercial Support**: Contact Orochi DB team

## References

- [CloudNativePG Documentation](https://cloudnative-pg.io/)
- [DigitalOcean Kubernetes](https://docs.digitalocean.com/products/kubernetes/)
- [DigitalOcean Spaces](https://docs.digitalocean.com/products/spaces/)
- [Orochi DB Documentation](https://docs.orochi-db.io/)
