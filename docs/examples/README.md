# Orochi DB Cluster Configuration Examples

This directory contains example cluster configurations for creating Orochi DB clusters with various features enabled.

## Prerequisites

Before creating clusters with tiering enabled, you need:

1. **Kubernetes cluster** with CloudNativePG operator installed
2. **Orochi provisioner** service running
3. **S3-compatible storage** (AWS S3, MinIO, Ceph, etc.)
4. **S3 credentials secret** (if not using IAM roles)

### Creating S3 Credentials Secret

For AWS S3:

```bash
kubectl create secret generic orochi-s3-credentials \
  --namespace=production \
  --from-literal=AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID \
  --from-literal=AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY
```

For MinIO:

```bash
kubectl create secret generic minio-credentials \
  --namespace=development \
  --from-literal=AWS_ACCESS_KEY_ID=minioadmin \
  --from-literal=AWS_SECRET_ACCESS_KEY=minioadmin
```

## Examples

### 1. Full Production Cluster with Tiering

**File**: `create-cluster-with-tiering.json`

Complete production configuration with all features:
- 5 PostgreSQL instances (1 primary + 4 replicas)
- Tiered storage (hot/warm/cold/frozen)
- Automatic backups to S3
- PgBouncer connection pooler
- Prometheus monitoring
- Pod anti-affinity for high availability
- Custom resource allocation

**Create the cluster**:

```bash
# Via gRPC API
grpcurl -d @create-cluster-with-tiering.json \
  -plaintext localhost:50051 \
  provisioner.v1.ProvisionerService/CreateCluster

# Via REST API (if control plane is running)
curl -X POST http://localhost:8080/api/v1/clusters \
  -H "Content-Type: application/json" \
  -d @create-cluster-with-tiering.json
```

**Key Features**:
- Hot tier: 7 days (fast SSD storage)
- Warm tier: 30 days (columnar format, compressed)
- Cold tier: 180 days (S3 with local cache)
- Frozen tier: 180+ days (S3 only)
- Automated backups at 2 AM daily
- Connection pooling with PgBouncer

### 2. Minimal Development Cluster

**File**: `create-cluster-with-tiering-minimal.json`

Minimal configuration for development/testing:
- Single PostgreSQL instance
- Basic tiering to MinIO
- Minimal resource allocation
- No high availability

**Use Cases**:
- Local development with Minikube/Kind
- CI/CD testing environments
- Development with MinIO for S3 simulation

**Create the cluster**:

```bash
# Start MinIO in development namespace (if needed)
kubectl apply -f - <<EOF
apiVersion: v1
kind: Pod
metadata:
  name: minio
  namespace: development
spec:
  containers:
  - name: minio
    image: minio/minio:latest
    args:
      - server
      - /data
    ports:
    - containerPort: 9000
    env:
    - name: MINIO_ROOT_USER
      value: minioadmin
    - name: MINIO_ROOT_PASSWORD
      value: minioadmin
---
apiVersion: v1
kind: Service
metadata:
  name: minio
  namespace: development
spec:
  selector:
    name: minio
  ports:
  - port: 9000
EOF

# Create MinIO credentials
kubectl create secret generic minio-credentials \
  --namespace=development \
  --from-literal=AWS_ACCESS_KEY_ID=minioadmin \
  --from-literal=AWS_SECRET_ACCESS_KEY=minioadmin

# Create cluster
grpcurl -d @create-cluster-with-tiering-minimal.json \
  -plaintext localhost:50051 \
  provisioner.v1.ProvisionerService/CreateCluster
```

### 3. Production Cluster with IAM Role Authentication

**File**: `create-cluster-with-tiering-iam-role.json`

Production cluster using IAM roles instead of static credentials:
- AWS IRSA (IAM Roles for Service Accounts) for EKS
- GKE Workload Identity for GKE
- No S3 credentials secret needed

**Prerequisites**:

For AWS EKS:

```bash
# Create IAM policy for S3 access
cat > orochi-s3-policy.json <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "s3:GetObject",
        "s3:PutObject",
        "s3:DeleteObject",
        "s3:ListBucket"
      ],
      "Resource": [
        "arn:aws:s3:::production-orochi-cold-us-west-2",
        "arn:aws:s3:::production-orochi-cold-us-west-2/*"
      ]
    }
  ]
}
EOF

aws iam create-policy \
  --policy-name OrochiPostgresS3Access \
  --policy-document file://orochi-s3-policy.json

# Create service account with IAM role
eksctl create iamserviceaccount \
  --name orochi-postgres \
  --namespace production \
  --cluster my-eks-cluster \
  --attach-policy-arn arn:aws:iam::123456789012:policy/OrochiPostgresS3Access \
  --approve
```

**Create the cluster**:

```bash
grpcurl -d @create-cluster-with-tiering-iam-role.json \
  -plaintext localhost:50051 \
  provisioner.v1.ProvisionerService/CreateCluster
```

**Key Features**:
- No static credentials required
- IAM role automatically provides S3 access
- Follows AWS security best practices
- Automatic credential rotation by AWS

## Configuration Reference

### Tiering Configuration

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `enabled` | boolean | Yes | false | Enable tiered storage |
| `hotDuration` | string | No | "7 days" | Duration for hot tier |
| `warmDuration` | string | No | "30 days" | Duration for warm tier |
| `coldDuration` | string | No | "90 days" | Duration for cold tier |
| `s3Config` | object | Yes | - | S3 configuration |

### S3 Configuration

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `endpoint` | string | Yes | - | S3 endpoint URL |
| `bucket` | string | Yes | - | S3 bucket name |
| `region` | string | No | us-east-1 | AWS region |
| `accessKeySecret` | string | No | - | Kubernetes secret name with credentials |

**Note**: If `accessKeySecret` is omitted, the cluster will use IAM role-based authentication.

### Duration Formats

Durations can be specified in PostgreSQL interval format:

- `"3 days"`
- `"1 week"`
- `"30 days"`
- `"90 days"`
- `"6 months"`
- `"1 year"`

## Verification

After creating a cluster, verify the configuration:

```bash
# 1. Check cluster status
kubectl get cluster <cluster-name> -n <namespace>

# 2. Verify Orochi extension is loaded
kubectl exec -it <cluster-pod> -n <namespace> -- \
  psql -U postgres -c "SELECT * FROM pg_extension WHERE extname = 'orochi'"

# 3. Check tiering configuration
kubectl exec -it <cluster-pod> -n <namespace> -- \
  psql -U postgres -c "SHOW orochi.tiering_enabled"

kubectl exec -it <cluster-pod> -n <namespace> -- \
  psql -U postgres -c "SHOW orochi.s3_endpoint"

kubectl exec -it <cluster-pod> -n <namespace> -- \
  psql -U postgres -c "SHOW orochi.s3_bucket"

# 4. Verify environment variables (if using static credentials)
kubectl exec -it <cluster-pod> -n <namespace> -- env | grep AWS

# 5. Test tiering functionality
kubectl exec -it <cluster-pod> -n <namespace> -- \
  psql -U postgres -c "SELECT orochi.tiering_status()"
```

## Monitoring Tiering

Once the cluster is running, you can monitor tiering activity:

```sql
-- View tiering statistics
SELECT * FROM orochi.tiering_stats();

-- View data distribution across tiers
SELECT
  tier,
  COUNT(*) as chunks,
  pg_size_pretty(SUM(size)) as total_size
FROM orochi.chunk_tiering_status
GROUP BY tier;

-- View recent tier transitions
SELECT * FROM orochi.tier_transitions
ORDER BY transitioned_at DESC
LIMIT 10;
```

## Cost Optimization

Tiering can significantly reduce storage costs:

| Tier | Storage Type | Relative Cost | Use Case |
|------|-------------|---------------|----------|
| Hot | SSD (PVC) | 10x | Recent data, frequent queries |
| Warm | Columnar (PVC) | 2x | Week-old data, analytics |
| Cold | S3 + Cache | 1x | Month-old data, occasional access |
| Frozen | S3 only | 0.5x | Archive data, rare access |

**Example Savings**:
- 1 TB of data in hot tier: $100/month
- 1 TB of data in cold tier: $10/month
- 1 TB of data in frozen tier: $5/month

With automatic tiering, a 1 TB dataset with typical access patterns can cost:
- Without tiering: $100/month
- With tiering: $30-40/month (60-70% savings)

## Troubleshooting

### Secret Not Found

```
Error: tiering S3 credentials secret not found
```

**Solution**:
```bash
kubectl create secret generic <secret-name> \
  --namespace=<namespace> \
  --from-literal=AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID \
  --from-literal=AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY
```

### Extension Not Created

```
ERROR: extension "orochi" does not exist
```

**Solution**: Check cluster logs:
```bash
kubectl logs <cluster-pod> -n <namespace> | grep -i "orochi\|extension"
```

Look for bootstrap errors in the PostgreSQL logs.

### S3 Connection Errors

```
ERROR: failed to connect to S3
```

**Solution**: Verify:
1. S3 endpoint is accessible from cluster
2. Bucket exists and is in the correct region
3. Credentials are valid
4. Network policies allow S3 access

```bash
# Test S3 connectivity from pod
kubectl exec -it <cluster-pod> -n <namespace> -- \
  curl -v https://<s3-endpoint>/<bucket>
```

## Best Practices

1. **Use IAM Roles in Production**: Prefer IAM role-based authentication over static credentials
2. **Separate Buckets**: Use different buckets for different environments (dev, staging, prod)
3. **Enable Versioning**: Enable S3 bucket versioning for data protection
4. **Enable Encryption**: Use S3 server-side encryption (SSE-S3 or SSE-KMS)
5. **Monitor Costs**: Monitor S3 storage and transfer costs regularly
6. **Tune Durations**: Adjust tiering durations based on your access patterns
7. **Test Restore**: Regularly test restoring data from cold/frozen tiers
8. **Use Lifecycle Policies**: Combine with S3 lifecycle policies for archival to Glacier

## Advanced Scenarios

### Multi-Region Tiering

For multi-region deployments, use region-specific buckets:

```json
{
  "s3Config": {
    "endpoint": "s3.${REGION}.amazonaws.com",
    "bucket": "orochi-cold-${REGION}",
    "region": "${REGION}"
  }
}
```

### Custom Tiering Policies per Table

Override default tiering for specific tables:

```sql
-- Faster tiering for logs table
SELECT orochi.set_tiering_policy('logs',
    hot_duration => '1 day',
    warm_duration => '7 days',
    cold_duration => '30 days'
);

-- Slower tiering for historical data
SELECT orochi.set_tiering_policy('historical_metrics',
    hot_duration => '30 days',
    warm_duration => '90 days',
    cold_duration => '365 days'
);
```

### Hybrid Storage (S3 + GCS)

You can configure different tables to use different S3-compatible endpoints:

```sql
-- Use AWS S3 for production data
SELECT orochi.set_s3_config('production_metrics',
    endpoint => 's3.amazonaws.com',
    bucket => 'prod-metrics-cold'
);

-- Use Google Cloud Storage for analytics data
SELECT orochi.set_s3_config('analytics_events',
    endpoint => 'storage.googleapis.com',
    bucket => 'analytics-cold'
);
```

## Related Documentation

- [Provisioner Tiering Integration Guide](/docs/provisioner-tiering-integration.md)
- [Implementation Summary](/docs/IMPLEMENTATION_SUMMARY_TIERING.md)
- [CloudNativePG Documentation](https://cloudnative-pg.io/)
- [AWS IRSA Documentation](https://docs.aws.amazon.com/eks/latest/userguide/iam-roles-for-service-accounts.html)
