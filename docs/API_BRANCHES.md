# Database Branching API

OrochiDB supports instant database branching, allowing you to create isolated copies of your database for development, testing, and staging environments in seconds.

## Overview

Database branching creates a copy-on-write clone of your parent database. Changes to the branch do not affect the parent cluster, making it perfect for:

- **Development environments**: Each developer can have their own isolated database copy
- **Testing**: Run integration tests against a production-like dataset
- **Staging**: Preview changes before deploying to production
- **Feature branches**: Match your Git workflow with database branches

## Branching Methods

OrochiDB supports multiple branching methods, automatically selecting the fastest available option:

| Method | Speed | Description |
|--------|-------|-------------|
| `volumeSnapshot` | Instant (~seconds) | Uses CSI volume snapshots with copy-on-write. Recommended. |
| `clone` | Fast (~seconds) | PostgreSQL 18's native `CLONE` feature with XFS reflinks |
| `pg_basebackup` | Slower (~minutes) | Traditional physical backup. Most compatible. |
| `pitr` | Variable | Point-in-time recovery to a specific timestamp or LSN |

## Authentication

All endpoints require authentication via Bearer token:

```
Authorization: Bearer <your-jwt-token>
```

## Endpoints

### List Branches

```
GET /api/v1/clusters/{clusterId}/branches
```

Returns all branches for a cluster.

**Response:**

```json
{
  "branches": [
    {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "name": "feature-auth",
      "parentClusterId": "123e4567-e89b-12d3-a456-426614174000",
      "parentCluster": "production-db",
      "status": "ready",
      "method": "volumeSnapshot",
      "connectionString": "postgresql://user:pass@branch-host:5432/postgres",
      "poolerConnection": "postgresql://user:pass@pooler-host:6432/postgres",
      "createdAt": "2024-01-15T10:30:00Z",
      "labels": {
        "orochi.io/branch": "true",
        "orochi.io/parent-cluster": "production-db"
      }
    }
  ],
  "totalCount": 1
}
```

**Status Codes:**

- `200 OK` - Success
- `401 Unauthorized` - Missing or invalid authentication
- `403 Forbidden` - No access to cluster
- `404 Not Found` - Cluster not found

### Create Branch

```
POST /api/v1/clusters/{clusterId}/branches
```

Creates a new database branch from the parent cluster.

**Request Body:**

```json
{
  "name": "feature-auth",
  "method": "volumeSnapshot",
  "inherit": true
}
```

**Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | Yes | Branch name (alphanumeric, hyphens allowed) |
| `method` | string | No | Branching method (`volumeSnapshot`, `clone`, `pg_basebackup`, `pitr`). Defaults to fastest available. |
| `pointInTime` | string | No | RFC3339 timestamp for PITR branches |
| `lsn` | string | No | PostgreSQL LSN for PITR branches |
| `instances` | integer | No | Number of instances (default: 1) |
| `inherit` | boolean | No | Inherit parent cluster configuration (default: true) |

**Response:**

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "name": "feature-auth",
  "parentClusterId": "123e4567-e89b-12d3-a456-426614174000",
  "parentCluster": "production-db",
  "status": "creating",
  "method": "volumeSnapshot",
  "createdAt": "2024-01-15T10:30:00Z",
  "labels": {
    "orochi.io/branch": "true",
    "orochi.io/parent-cluster": "production-db"
  }
}
```

**Status Codes:**

- `202 Accepted` - Branch creation started
- `400 Bad Request` - Invalid request body
- `401 Unauthorized` - Missing or invalid authentication
- `403 Forbidden` - No access to cluster
- `404 Not Found` - Cluster not found
- `409 Conflict` - Cluster must be running

### Get Branch

```
GET /api/v1/clusters/{clusterId}/branches/{branchId}
```

Returns details for a specific branch.

**Response:**

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "name": "feature-auth",
  "parentClusterId": "123e4567-e89b-12d3-a456-426614174000",
  "parentCluster": "production-db",
  "status": "ready",
  "method": "volumeSnapshot",
  "connectionString": "postgresql://user:pass@branch-host:5432/postgres",
  "poolerConnection": "postgresql://user:pass@pooler-host:6432/postgres",
  "createdAt": "2024-01-15T10:30:00Z",
  "labels": {
    "orochi.io/branch": "true",
    "orochi.io/parent-cluster": "production-db"
  }
}
```

**Status Codes:**

- `200 OK` - Success
- `401 Unauthorized` - Missing or invalid authentication
- `403 Forbidden` - No access to cluster
- `404 Not Found` - Cluster or branch not found

### Delete Branch

```
DELETE /api/v1/clusters/{clusterId}/branches/{branchId}
```

Deletes a branch and releases its resources.

**Status Codes:**

- `204 No Content` - Branch deleted
- `401 Unauthorized` - Missing or invalid authentication
- `403 Forbidden` - No access to cluster
- `404 Not Found` - Cluster or branch not found

### Promote Branch

```
POST /api/v1/clusters/{clusterId}/branches/{branchId}/promote
```

Promotes a branch to become a standalone cluster. The branch will be removed from the parent cluster and become an independent cluster.

**Response:**

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "name": "feature-auth",
  "parentClusterId": "123e4567-e89b-12d3-a456-426614174000",
  "parentCluster": "production-db",
  "status": "promoting",
  "method": "volumeSnapshot",
  "createdAt": "2024-01-15T10:30:00Z"
}
```

**Status Codes:**

- `202 Accepted` - Promotion started
- `401 Unauthorized` - Missing or invalid authentication
- `403 Forbidden` - No access to cluster
- `404 Not Found` - Cluster or branch not found
- `409 Conflict` - Branch must be in ready state

## Branch Status

| Status | Description |
|--------|-------------|
| `creating` | Branch is being created |
| `ready` | Branch is ready for connections |
| `failed` | Branch creation failed |
| `deleting` | Branch is being deleted |
| `promoting` | Branch is being promoted to a standalone cluster |

## CLI Usage

```bash
# List branches
orochi cluster branches list <cluster-id>

# Create a branch
orochi cluster branch create <cluster-id> --name feature-auth

# Create a branch with specific method
orochi cluster branch create <cluster-id> --name staging --method pg_basebackup

# Create a PITR branch
orochi cluster branch create <cluster-id> --name recovery --method pitr --point-in-time "2024-01-15T10:00:00Z"

# Delete a branch
orochi cluster branch delete <cluster-id> <branch-id>

# Promote a branch
orochi cluster branch promote <cluster-id> <branch-id>
```

## Dashboard

Database branches can be managed through the web dashboard:

1. Navigate to your cluster
2. Click "Branches" in the features section
3. Click "Create Branch" to create a new branch
4. Use the dropdown menu on each branch card for actions (delete, promote, copy connection string)

## Best Practices

1. **Use descriptive names**: Name branches after features or purposes (e.g., `feature-auth`, `staging`, `dev-john`)
2. **Clean up unused branches**: Delete branches when no longer needed to free resources
3. **Use volume snapshots**: When available, volume snapshots provide the fastest branching
4. **Consider storage**: Branches share storage with the parent until divergent data is written
5. **Monitor branch count**: Keep the number of active branches manageable for your tier

## Limitations

- Maximum branches per cluster depends on your tier
- Branches inherit the parent's PostgreSQL version
- Branches cannot be created while the parent cluster is stopped
- PITR branches require WAL archiving to be enabled
