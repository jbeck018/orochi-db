# Provisioner Tiering Integration

This document describes how tiered storage configuration is wired through the provisioner to CloudNativePG clusters.

## Overview

The provisioner now automatically configures tiered storage (hot/warm/cold/frozen) when creating PostgreSQL clusters with the Orochi extension. This includes:

1. Setting PostgreSQL GUCs for tiering configuration
2. Creating S3 credentials as Kubernetes secrets
3. Injecting environment variables for S3 access
4. Automatically creating the Orochi extension on cluster bootstrap

## Architecture

```
ClusterSpec (API Request)
    │
    ├─> Tiering GUCs ──────────────────────> postgresql.parameters
    │                                         (orochi.tiering_enabled, etc.)
    │
    ├─> S3 Credentials Secret ─────────────> Kubernetes Secret
    │                                         (validated before cluster creation)
    │
    ├─> Environment Variables ──────────────> spec.env
    │                                         (AWS_ACCESS_KEY_ID, etc.)
    │
    └─> Extension Bootstrap ─────────────────> spec.bootstrap.initdb.postInitSQL
                                              (CREATE EXTENSION, initialize())
```

## Configuration Parameters

### Tiering GUCs

The following PostgreSQL GUCs are automatically set when tiering is enabled:

| GUC | Description | Example |
|-----|-------------|---------|
| `orochi.tiering_enabled` | Enable tiered storage | `on` |
| `orochi.tiering_hot_duration` | Duration for hot tier | `7 days` |
| `orochi.tiering_warm_duration` | Duration for warm tier | `30 days` |
| `orochi.tiering_cold_duration` | Duration for cold tier | `90 days` |
| `orochi.s3_endpoint` | S3-compatible endpoint | `s3.amazonaws.com` |
| `orochi.s3_bucket` | S3 bucket name | `orochi-cold-storage` |
| `orochi.s3_region` | S3 region | `us-east-1` |

### ClusterSpec Structure

```json
{
  "name": "my-cluster",
  "namespace": "default",
  "instances": 3,
  "orochiConfig": {
    "enabled": true,
    "tiering": {
      "enabled": true,
      "hotDuration": "7 days",
      "warmDuration": "30 days",
      "coldDuration": "90 days",
      "s3Config": {
        "endpoint": "s3.amazonaws.com",
        "bucket": "orochi-cold-storage",
        "region": "us-east-1",
        "accessKeySecret": "orochi-s3-credentials"
      }
    }
  }
}
```

## S3 Credentials Secret

Before creating a cluster with tiering enabled, you must create a Kubernetes secret with S3 credentials:

```bash
kubectl create secret generic orochi-s3-credentials \
  --namespace=default \
  --from-literal=AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE \
  --from-literal=AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY
```

### Secret Validation

The provisioner validates that:

1. The secret exists in the target namespace
2. The secret contains required keys: `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`
3. The secret is accessible before cluster creation begins

If validation fails, cluster creation is aborted with a descriptive error.

### IAM Role-Based Authentication

If you prefer IAM role-based authentication (e.g., using AWS IRSA or GKE Workload Identity), omit the `accessKeySecret` field:

```json
{
  "orochiConfig": {
    "tiering": {
      "enabled": true,
      "s3Config": {
        "endpoint": "s3.amazonaws.com",
        "bucket": "orochi-cold-storage",
        "region": "us-east-1"
        // No accessKeySecret - uses IAM role
      }
    }
  }
}
```

## Environment Variable Injection

When `accessKeySecret` is specified, the provisioner injects environment variables into the PostgreSQL pods:

```yaml
spec:
  env:
    - name: AWS_ACCESS_KEY_ID
      valueFrom:
        secretKeyRef:
          name: orochi-s3-credentials
          key: AWS_ACCESS_KEY_ID
    - name: AWS_SECRET_ACCESS_KEY
      valueFrom:
        secretKeyRef:
          name: orochi-s3-credentials
          key: AWS_SECRET_ACCESS_KEY
```

These environment variables are used by the Orochi extension's S3 client (libcurl-based) to authenticate with S3.

## Automatic Extension Creation

The provisioner configures CloudNativePG to automatically create the Orochi extension on cluster bootstrap:

```yaml
spec:
  bootstrap:
    initdb:
      postInitSQL:
        - "CREATE EXTENSION IF NOT EXISTS orochi"
        - "SELECT orochi.initialize()"
```

This ensures the extension is created and initialized immediately after the cluster is ready, without manual intervention.

## Complete Example

### Step 1: Create S3 Credentials Secret

```bash
kubectl create secret generic orochi-s3-credentials \
  --namespace=production \
  --from-literal=AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID \
  --from-literal=AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY
```

### Step 2: Create Cluster with Tiering

```json
{
  "name": "production-analytics",
  "namespace": "production",
  "instances": 5,
  "postgresVersion": "16",
  "resources": {
    "cpuRequest": "2",
    "cpuLimit": "4",
    "memoryRequest": "8Gi",
    "memoryLimit": "16Gi"
  },
  "storage": {
    "size": "100Gi",
    "storageClass": "fast-ssd"
  },
  "orochiConfig": {
    "enabled": true,
    "defaultShardCount": 64,
    "chunkInterval": "1 day",
    "enableColumnar": true,
    "defaultCompression": "zstd",
    "tiering": {
      "enabled": true,
      "hotDuration": "7 days",
      "warmDuration": "30 days",
      "coldDuration": "180 days",
      "s3Config": {
        "endpoint": "s3.us-east-1.amazonaws.com",
        "bucket": "production-orochi-cold",
        "region": "us-east-1",
        "accessKeySecret": "orochi-s3-credentials"
      }
    }
  },
  "backupConfig": {
    "retentionPolicy": "30d",
    "barmanObjectStore": {
      "destinationPath": "s3://production-orochi-backups/analytics",
      "s3Credentials": {
        "accessKeyIdSecret": "orochi-s3-credentials",
        "secretAccessKeySecret": "orochi-s3-credentials"
      }
    }
  },
  "monitoring": {
    "enablePodMonitor": true
  }
}
```

### Step 3: Verify Configuration

```bash
# Check cluster status
kubectl get cluster production-analytics -n production

# Verify Orochi extension is loaded
kubectl exec -it production-analytics-1 -n production -- \
  psql -U postgres -c "SELECT * FROM pg_extension WHERE extname = 'orochi'"

# Check tiering configuration
kubectl exec -it production-analytics-1 -n production -- \
  psql -U postgres -c "SHOW orochi.tiering_enabled"

kubectl exec -it production-analytics-1 -n production -- \
  psql -U postgres -c "SHOW orochi.s3_endpoint"
```

## Implementation Details

### Files Modified

1. **`/services/provisioner/internal/cloudnativepg/cluster.go`**
   - `buildPostgresParameters()`: Added tiering GUC configuration
   - `buildClusterSpec()`: Added bootstrap SQL and environment variable injection

2. **`/services/provisioner/templates/cluster.yaml.tmpl`**
   - Added tiering parameters section
   - Added environment variable template
   - Added bootstrap SQL section

3. **`/services/provisioner/internal/provisioner/service.go`**
   - `CreateCluster()`: Calls `createTieringSecrets()` before cluster creation
   - `createTieringSecrets()`: Enhanced with validation and logging

### Code Flow

1. **Cluster Creation Request**
   ```
   API Request → CreateCluster(spec)
   ```

2. **Secret Validation**
   ```
   if spec.OrochiConfig.Tiering.S3Config != nil {
       createTieringSecrets(ctx, spec)  // Validates secret exists
   }
   ```

3. **Cluster Spec Building**
   ```
   buildClusterSpec(spec)
       ├─> buildPostgresParameters(spec)  // Add tiering GUCs
       ├─> buildEnvironmentVariables(spec) // Add S3 credentials
       └─> buildBootstrap(spec)            // Add extension creation SQL
   ```

4. **CloudNativePG Cluster Creation**
   ```
   clusterManager.CreateCluster(ctx, spec)
       └─> CloudNativePG Operator provisions cluster
           └─> PostgreSQL starts with Orochi extension loaded
               └─> Bootstrap SQL creates and initializes extension
   ```

## Tiering Lifecycle

Once configured, the Orochi extension manages data lifecycle automatically:

1. **Hot Tier** (0 - 7 days): Data in PostgreSQL heap storage (fastest)
2. **Warm Tier** (7 - 30 days): Data in columnar format (compressed)
3. **Cold Tier** (30 - 180 days): Data in S3 with local cache
4. **Frozen Tier** (180+ days): Data in S3 only (cheapest)

Transitions are automatic based on data age and access patterns.

## Troubleshooting

### Secret Not Found

```
Error: tiering S3 credentials secret not found: secrets "orochi-s3-credentials" not found
```

**Solution**: Create the secret in the target namespace before creating the cluster.

### Missing Secret Keys

```
Error: S3 credentials secret orochi-s3-credentials missing required key: AWS_ACCESS_KEY_ID
```

**Solution**: Ensure the secret contains both `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` keys.

### Extension Not Created

```
ERROR:  extension "orochi" does not exist
```

**Solution**: Check the cluster logs for bootstrap errors:

```bash
kubectl logs production-analytics-1 -n production | grep -i "orochi\|extension"
```

### S3 Connection Errors

```
ERROR:  failed to connect to S3: The AWS Access Key Id you provided does not exist
```

**Solution**: Verify the secret contains valid AWS credentials and the bucket exists.

## Advanced Configuration

### Custom Tiering Policies

You can override default tiering durations per table:

```sql
-- Override tiering for specific hypertable
SELECT orochi.set_tiering_policy('metrics',
    hot_duration => '3 days',
    warm_duration => '14 days',
    cold_duration => '90 days'
);
```

### S3-Compatible Storage (MinIO, Ceph)

```json
{
  "s3Config": {
    "endpoint": "minio.internal.company.com:9000",
    "bucket": "orochi-cold",
    "region": "us-east-1",
    "accessKeySecret": "minio-credentials"
  }
}
```

### Multi-Region Tiering

For multi-region deployments, use region-specific buckets:

```json
{
  "s3Config": {
    "endpoint": "s3.us-west-2.amazonaws.com",
    "bucket": "orochi-cold-us-west-2",
    "region": "us-west-2",
    "accessKeySecret": "orochi-s3-credentials-west"
  }
}
```

## Performance Considerations

1. **Hot Tier**: Use fast SSDs for best query performance
2. **Warm Tier**: Columnar format reduces storage by 5-10x
3. **Cold Tier**: S3 access adds 50-200ms latency per query
4. **Network**: Ensure sufficient network bandwidth for S3 transfers

## Security Best Practices

1. **Least Privilege**: Grant S3 credentials only `s3:GetObject`, `s3:PutObject`, `s3:DeleteObject` permissions
2. **Bucket Policies**: Use bucket policies to restrict access by IP or VPC
3. **Encryption**: Enable S3 server-side encryption (SSE-S3 or SSE-KMS)
4. **Secret Rotation**: Rotate S3 credentials regularly
5. **IAM Roles**: Prefer IAM role-based authentication over static credentials

## References

- [CloudNativePG Documentation](https://cloudnative-pg.io/)
- [PostgreSQL Extension Development](https://www.postgresql.org/docs/current/extend.html)
- [AWS S3 Best Practices](https://docs.aws.amazon.com/AmazonS3/latest/userguide/best-practices.html)
- [Kubernetes Secrets](https://kubernetes.io/docs/concepts/configuration/secret/)
