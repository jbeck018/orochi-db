# PgDog JWT Gateway

A lightweight TCP proxy that provides JWT authentication for PgDog PostgreSQL connection pooler.

## Overview

Since PgDog doesn't support custom authentication plugins, this gateway sits in front of PgDog and handles:

1. **JWT Extraction** - Extracts JWT token from the PostgreSQL password field
2. **Token Validation** - Validates RS256 signatures, expiration, issuer, and audience
3. **Claims Injection** - Sets PostgreSQL session variables based on JWT claims
4. **Connection Forwarding** - Transparently proxies the connection to PgDog

## Architecture

```
Client (psql/app)           JWT Gateway              PgDog              PostgreSQL
       |                         |                     |                    |
       |-- Startup Message ----->|                     |                    |
       |<- Auth Request ---------|                     |                    |
       |-- Password (JWT) ------>|                     |                    |
       |                         |-- Validate JWT      |                    |
       |                         |-- Connect --------->|                    |
       |                         |<- Auth OK ----------|                    |
       |                         |-- SET orochi.* ---->|                    |
       |<- Auth OK --------------|                     |                    |
       |<======= Bidirectional Proxy =================>|                    |
```

## Quick Start

### Build

```bash
# Build binary
go build -o jwt-proxy ./cmd/jwt-proxy

# Or use Docker
docker build -t pgdog-jwt-gateway .
```

### Run

```bash
# With config file
./jwt-proxy -config config.yaml

# With environment variables
export JWT_GATEWAY_PUBLIC_KEY_PATH=/path/to/public.pem
export JWT_GATEWAY_BACKEND_HOST=localhost
export JWT_GATEWAY_BACKEND_PORT=5432
./jwt-proxy
```

### Connect

```bash
# Use JWT as password
PGPASSWORD="eyJhbGciOiJSUzI1NiIs..." psql -h localhost -p 5433 -U myuser -d mydb
```

## Configuration

Configuration can be provided via YAML file or environment variables.

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `JWT_GATEWAY_LISTEN_ADDR` | Listen address | `:5433` |
| `JWT_GATEWAY_MAX_CONNECTIONS` | Max concurrent connections | `1000` |
| `JWT_GATEWAY_PUBLIC_KEY_PATH` | Path to RSA public key | - |
| `JWT_GATEWAY_PUBLIC_KEY_URL` | URL to fetch public key | - |
| `JWT_GATEWAY_ISSUER` | Expected token issuer | - |
| `JWT_GATEWAY_AUDIENCE` | Expected token audience | - |
| `JWT_GATEWAY_BACKEND_HOST` | PgDog host | `localhost` |
| `JWT_GATEWAY_BACKEND_PORT` | PgDog port | `5432` |
| `JWT_GATEWAY_LOG_LEVEL` | Log level (debug/info/warn/error) | `info` |

See `config.yaml.example` for full configuration options.

## JWT Token Format

The gateway expects JWT tokens with RS256 signature containing:

### Required Claims

- `iss` - Token issuer (if configured)
- `aud` - Token audience (if configured)
- `exp` - Expiration time
- `iat` - Issued at time

### Optional Orochi Claims

```json
{
  "user_id": "usr_abc123",
  "tenant_id": "tenant_xyz",
  "branch_id": "main",
  "permissions": ["read", "write"],
  "permission_level": 2,
  "database": "mydb",
  "read_only": false,
  "metadata": {
    "team": "engineering"
  }
}
```

## Session Variables

After authentication, the following PostgreSQL session variables are set:

| Variable | Description |
|----------|-------------|
| `orochi.user_id` | User identifier |
| `orochi.tenant_id` | Tenant identifier |
| `orochi.branch_id` | Database branch |
| `orochi.permission_level` | Permission level (0-3) |
| `orochi.read_only` | Read-only flag |

Access in SQL:
```sql
SELECT current_setting('orochi.tenant_id', true);
```

Use in Row-Level Security policies:
```sql
CREATE POLICY tenant_isolation ON orders
  USING (tenant_id = current_setting('orochi.tenant_id', true));
```

## Kubernetes Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: pgdog-jwt-gateway
spec:
  replicas: 2
  selector:
    matchLabels:
      app: pgdog-jwt-gateway
  template:
    metadata:
      labels:
        app: pgdog-jwt-gateway
    spec:
      containers:
      - name: jwt-gateway
        image: orochi/pgdog-jwt-gateway:latest
        ports:
        - containerPort: 5433
        env:
        - name: JWT_GATEWAY_BACKEND_HOST
          value: "pgdog-service"
        - name: JWT_GATEWAY_PUBLIC_KEY_PATH
          value: "/keys/public.pem"
        - name: JWT_GATEWAY_ISSUER
          value: "https://auth.orochi.cloud"
        volumeMounts:
        - name: jwt-keys
          mountPath: /keys
          readOnly: true
        resources:
          requests:
            memory: "64Mi"
            cpu: "100m"
          limits:
            memory: "256Mi"
            cpu: "500m"
        livenessProbe:
          tcpSocket:
            port: 5433
          initialDelaySeconds: 5
          periodSeconds: 10
        readinessProbe:
          tcpSocket:
            port: 5433
          initialDelaySeconds: 5
          periodSeconds: 5
      volumes:
      - name: jwt-keys
        secret:
          secretName: jwt-public-key
---
apiVersion: v1
kind: Service
metadata:
  name: pgdog-jwt-gateway
spec:
  selector:
    app: pgdog-jwt-gateway
  ports:
  - port: 5433
    targetPort: 5433
```

## Performance

- Token caching reduces validation overhead
- Connection pooling handled by PgDog
- Minimal latency overhead (~1ms for cached tokens)
- Supports thousands of concurrent connections

## Security Considerations

1. **Transport Security** - Use TLS between clients and gateway
2. **Key Rotation** - Configure `key_refresh_interval` for automatic key updates
3. **Token Lifetime** - Use short-lived tokens (15-60 minutes)
4. **Network Isolation** - Run gateway in same network as PgDog
5. **Secrets Management** - Use Kubernetes secrets or vault for keys

## Development

```bash
# Run tests
go test ./...

# Run with debug logging
JWT_GATEWAY_LOG_LEVEL=debug ./jwt-proxy

# Build for Linux
GOOS=linux GOARCH=amd64 go build -o jwt-proxy-linux ./cmd/jwt-proxy
```

## License

MIT License - See LICENSE file
