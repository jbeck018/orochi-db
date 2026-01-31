# PgDog Integration Testing Strategy for Orochi-DB

A comprehensive testing strategy for integrating PgDog (Rust-based PostgreSQL proxy) with Orochi-DB's distributed sharding system.

## Overview

### Components Under Test

| Component | Technology | Purpose |
|-----------|------------|---------|
| PgDog Proxy | Rust | Connection pooling, query routing, failover |
| Custom Auth Plugin | Rust | JWT validation, session variable injection |
| Orochi Extension | C/PostgreSQL | Distributed sharding, time-series, columnar storage |
| Control Plane API | Go | Cluster management, configuration |
| Dashboard | React/TypeScript | User interface |

### Test Environment Architecture

```
                                    +-------------------+
                                    |    Dashboard      |
                                    |   (React/TS)      |
                                    +--------+----------+
                                             |
                                             v
+----------------+              +-------------------+
|  Test Client   | ----------> |   Control Plane   |
|  (Go/Rust)     |              |   API (Go)        |
+----------------+              +--------+----------+
        |                                |
        v                                v
+----------------+              +-------------------+
|    PgDog       | ----------> |   Provisioner     |
|    Proxy       |              |   (Go + K8s)      |
+----------------+              +-------------------+
        |
        v
+--------------------------------------------------+
|                  PostgreSQL Cluster               |
|  +----------+  +----------+  +----------+        |
|  | Primary  |  | Replica1 |  | Replica2 |        |
|  | + Orochi |  | + Orochi |  | + Orochi |        |
|  +----------+  +----------+  +----------+        |
+--------------------------------------------------+
```

---

## 1. Unit Tests (Rust Auth Module)

### 1.1 JWT Token Validation

**Framework**: `#[cfg(test)]` with `tokio::test` for async tests

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| JWT-001 | Valid HS256 token parsing | Token correctly parsed, claims extracted | P0 |
| JWT-002 | Valid RS256 token parsing | Token correctly parsed with RSA signature | P0 |
| JWT-003 | Valid ES256 token parsing | Token correctly parsed with ECDSA signature | P0 |
| JWT-004 | Expired token rejection | Returns `TokenExpired` error | P0 |
| JWT-005 | Invalid signature rejection | Returns `InvalidSignature` error | P0 |
| JWT-006 | Malformed token rejection | Returns `MalformedToken` error | P0 |
| JWT-007 | Missing required claims | Returns `MissingClaim` error | P1 |
| JWT-008 | Not-before (nbf) validation | Token rejected if `now < nbf` | P1 |
| JWT-009 | Issuer (iss) validation | Token rejected if issuer mismatch | P1 |
| JWT-010 | Audience (aud) validation | Token rejected if audience mismatch | P1 |
| JWT-011 | Algorithm substitution attack | Reject `alg: none` or algorithm switching | P0 |
| JWT-012 | Clock skew tolerance | Accept tokens within configurable skew | P2 |

**Completion Checklist**:
- [ ] JWT-001: Valid HS256 token parsing
- [ ] JWT-002: Valid RS256 token parsing
- [ ] JWT-003: Valid ES256 token parsing
- [ ] JWT-004: Expired token rejection
- [ ] JWT-005: Invalid signature rejection
- [ ] JWT-006: Malformed token rejection
- [ ] JWT-007: Missing required claims
- [ ] JWT-008: Not-before (nbf) validation
- [ ] JWT-009: Issuer (iss) validation
- [ ] JWT-010: Audience (aud) validation
- [ ] JWT-011: Algorithm substitution attack
- [ ] JWT-012: Clock skew tolerance

**Example Test Structure (Rust)**:
```rust
#[cfg(test)]
mod jwt_tests {
    use super::*;
    use jsonwebtoken::{encode, Header, EncodingKey};

    #[test]
    fn test_valid_hs256_token_parsing() {
        // Arrange
        let secret = "test-secret-key-32-bytes-long!!";
        let claims = Claims {
            sub: "user-123".to_string(),
            exp: (Utc::now() + Duration::hours(1)).timestamp() as usize,
            role: "authenticated".to_string(),
            ..Default::default()
        };
        let token = encode(&Header::default(), &claims, &EncodingKey::from_secret(secret.as_bytes())).unwrap();

        // Act
        let result = validate_jwt(&token, secret, Algorithm::HS256);

        // Assert
        assert!(result.is_ok());
        let parsed = result.unwrap();
        assert_eq!(parsed.sub, "user-123");
        assert_eq!(parsed.role, "authenticated");
    }

    #[test]
    fn test_expired_token_rejection() {
        // Arrange
        let secret = "test-secret-key-32-bytes-long!!";
        let claims = Claims {
            sub: "user-123".to_string(),
            exp: (Utc::now() - Duration::hours(1)).timestamp() as usize, // Expired
            ..Default::default()
        };
        let token = encode(&Header::default(), &claims, &EncodingKey::from_secret(secret.as_bytes())).unwrap();

        // Act
        let result = validate_jwt(&token, secret, Algorithm::HS256);

        // Assert
        assert!(matches!(result, Err(AuthError::TokenExpired)));
    }
}
```

### 1.2 Token Caching

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| CACHE-001 | Cache hit for valid token | Cached result returned, no re-validation | P0 |
| CACHE-002 | Cache miss for new token | Token validated and cached | P0 |
| CACHE-003 | Cache eviction on expiry | Expired tokens removed from cache | P0 |
| CACHE-004 | Cache size limits | LRU eviction when capacity exceeded | P1 |
| CACHE-005 | Cache invalidation on revocation | Revoked tokens not served from cache | P0 |
| CACHE-006 | Thread-safe concurrent access | No race conditions under load | P0 |
| CACHE-007 | Cache metrics collection | Hit/miss rates tracked accurately | P2 |

**Completion Checklist**:
- [ ] CACHE-001: Cache hit for valid token
- [ ] CACHE-002: Cache miss for new token
- [ ] CACHE-003: Cache eviction on expiry
- [ ] CACHE-004: Cache size limits
- [ ] CACHE-005: Cache invalidation on revocation
- [ ] CACHE-006: Thread-safe concurrent access
- [ ] CACHE-007: Cache metrics collection

### 1.3 Session Variable Injection

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| SESS-001 | Inject `request.jwt.claims` | Full claims JSON set as session var | P0 |
| SESS-002 | Inject `request.jwt.claim.sub` | Subject claim extracted correctly | P0 |
| SESS-003 | Inject `request.jwt.claim.role` | Role claim set for RLS policies | P0 |
| SESS-004 | Inject `request.jwt.claim.email` | Email claim available | P1 |
| SESS-005 | Inject custom claims | App-specific claims (tenant_id, org_id) | P0 |
| SESS-006 | Handle nested claims | `app_metadata.tenant_id` extracted | P1 |
| SESS-007 | Clear session on disconnect | Variables cleared between connections | P0 |
| SESS-008 | SQL injection in claim values | Values properly escaped | P0 |

**Completion Checklist**:
- [ ] SESS-001: Inject `request.jwt.claims`
- [ ] SESS-002: Inject `request.jwt.claim.sub`
- [ ] SESS-003: Inject `request.jwt.claim.role`
- [ ] SESS-004: Inject `request.jwt.claim.email`
- [ ] SESS-005: Inject custom claims
- [ ] SESS-006: Handle nested claims
- [ ] SESS-007: Clear session on disconnect
- [ ] SESS-008: SQL injection in claim values

### 1.4 Error Handling

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| ERR-001 | Missing Authorization header | Return 401 with clear message | P0 |
| ERR-002 | Invalid Bearer prefix | Return 401 with clear message | P1 |
| ERR-003 | Secret key rotation | Graceful handling of key changes | P1 |
| ERR-004 | OpenSSL errors | Wrapped with context, no panics | P0 |
| ERR-005 | Connection timeout during auth | Timeout error, connection cleaned up | P1 |
| ERR-006 | Logging of auth failures | Failures logged without exposing secrets | P0 |

**Completion Checklist**:
- [ ] ERR-001: Missing Authorization header
- [ ] ERR-002: Invalid Bearer prefix
- [ ] ERR-003: Secret key rotation
- [ ] ERR-004: OpenSSL errors
- [ ] ERR-005: Connection timeout during auth
- [ ] ERR-006: Logging of auth failures

---

## 2. Integration Tests

### 2.1 PgDog + PostgreSQL Connectivity

**Framework**: Docker Compose test environment, Go test suite with `pgx`

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| CONN-001 | Basic connection through proxy | Query executes, result returned | P0 |
| CONN-002 | Connection with TLS | TLS handshake successful | P0 |
| CONN-003 | Connection string parameters | All libpq params work through proxy | P1 |
| CONN-004 | Multiple concurrent connections | 100 connections handled without errors | P0 |
| CONN-005 | Connection reuse from pool | Same backend connection reused | P0 |
| CONN-006 | Connection release on client close | Backend connection returned to pool | P0 |
| CONN-007 | Prepared statement handling | Prepared statements work correctly | P0 |
| CONN-008 | Binary protocol support | Binary format data transmitted correctly | P1 |
| CONN-009 | Large result sets | Results > 10MB streamed correctly | P1 |
| CONN-010 | Connection timeout handling | Timeout after configurable period | P1 |

**Completion Checklist**:
- [ ] CONN-001: Basic connection through proxy
- [ ] CONN-002: Connection with TLS
- [ ] CONN-003: Connection string parameters
- [ ] CONN-004: Multiple concurrent connections
- [ ] CONN-005: Connection reuse from pool
- [ ] CONN-006: Connection release on client close
- [ ] CONN-007: Prepared statement handling
- [ ] CONN-008: Binary protocol support
- [ ] CONN-009: Large result sets
- [ ] CONN-010: Connection timeout handling

### 2.2 PgDog + Orochi Extension Compatibility

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| OROCHI-001 | Create distributed table | `SELECT create_distributed_table()` works | P0 |
| OROCHI-002 | Insert into sharded table | Data routed to correct shard | P0 |
| OROCHI-003 | Query with shard key | Query pruned to single shard | P0 |
| OROCHI-004 | Query without shard key | Fan-out to all shards, results merged | P0 |
| OROCHI-005 | Create hypertable | Time-series partitioning works | P0 |
| OROCHI-006 | Insert time-series data | Data routed to correct chunk | P0 |
| OROCHI-007 | Columnar table operations | Columnar storage read/write works | P1 |
| OROCHI-008 | JWT auth context for RLS | `current_setting('request.jwt.claim.sub')` works | P0 |
| OROCHI-009 | RLS policy enforcement | Row-level security applied correctly | P0 |
| OROCHI-010 | Multi-statement transactions | `BEGIN/COMMIT` works across shards | P0 |

**Completion Checklist**:
- [ ] OROCHI-001: Create distributed table
- [ ] OROCHI-002: Insert into sharded table
- [ ] OROCHI-003: Query with shard key
- [ ] OROCHI-004: Query without shard key
- [ ] OROCHI-005: Create hypertable
- [ ] OROCHI-006: Insert time-series data
- [ ] OROCHI-007: Columnar table operations
- [ ] OROCHI-008: JWT auth context for RLS
- [ ] OROCHI-009: RLS policy enforcement
- [ ] OROCHI-010: Multi-statement transactions

### 2.3 Connection Pooling Behavior

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| POOL-001 | Pool initialization | Minimum connections pre-created | P0 |
| POOL-002 | Pool scaling up | New connections created under load | P0 |
| POOL-003 | Pool scaling down | Idle connections closed after timeout | P1 |
| POOL-004 | Pool exhaustion handling | Wait or error when pool full | P0 |
| POOL-005 | Connection health checks | Unhealthy connections removed | P0 |
| POOL-006 | Per-database pools | Separate pools for different databases | P0 |
| POOL-007 | Per-user pools | User-specific connection limits | P1 |
| POOL-008 | Session mode vs transaction mode | Correct pooling behavior per mode | P0 |
| POOL-009 | Statement mode (if supported) | Query-level pooling works | P2 |
| POOL-010 | Pool metrics exposed | Prometheus metrics accurate | P1 |

**Completion Checklist**:
- [ ] POOL-001: Pool initialization
- [ ] POOL-002: Pool scaling up
- [ ] POOL-003: Pool scaling down
- [ ] POOL-004: Pool exhaustion handling
- [ ] POOL-005: Connection health checks
- [ ] POOL-006: Per-database pools
- [ ] POOL-007: Per-user pools
- [ ] POOL-008: Session mode vs transaction mode
- [ ] POOL-009: Statement mode (if supported)
- [ ] POOL-010: Pool metrics exposed

### 2.4 Read/Write Splitting

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| RW-001 | Writes routed to primary | `INSERT/UPDATE/DELETE` go to primary | P0 |
| RW-002 | Reads routed to replica | `SELECT` queries go to replicas | P0 |
| RW-003 | Explicit primary routing | `-- primary` comment routes to primary | P1 |
| RW-004 | Transaction reads on primary | All queries in write transaction on primary | P0 |
| RW-005 | Read-only transaction on replica | `SET TRANSACTION READ ONLY` uses replica | P1 |
| RW-006 | Replica lag detection | Stale replicas excluded from routing | P0 |
| RW-007 | Replica failover | Queries rerouted when replica fails | P0 |
| RW-008 | Session stickiness | Consistent replica for session if needed | P2 |

**Completion Checklist**:
- [ ] RW-001: Writes routed to primary
- [ ] RW-002: Reads routed to replica
- [ ] RW-003: Explicit primary routing
- [ ] RW-004: Transaction reads on primary
- [ ] RW-005: Read-only transaction on replica
- [ ] RW-006: Replica lag detection
- [ ] RW-007: Replica failover
- [ ] RW-008: Session stickiness

### 2.5 Sharding Key Routing

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| SHARD-001 | Extract shard key from query | Correctly identifies `WHERE tenant_id = X` | P0 |
| SHARD-002 | Route to correct shard | Query sent to appropriate backend | P0 |
| SHARD-003 | Multi-shard query detection | Query without shard key fans out | P0 |
| SHARD-004 | JOIN with same shard key | Colocated join handled correctly | P0 |
| SHARD-005 | Cross-shard JOIN handling | Appropriate error or distributed exec | P1 |
| SHARD-006 | Shard key from JWT claims | Use `tenant_id` from JWT for routing | P0 |
| SHARD-007 | INSERT shard key extraction | Extract from VALUES clause | P0 |
| SHARD-008 | UPDATE shard key extraction | Extract from WHERE clause | P0 |
| SHARD-009 | Parameterized query routing | Handle `$1` parameters correctly | P0 |
| SHARD-010 | Shard metadata caching | Shard map cached and refreshed | P1 |

**Completion Checklist**:
- [ ] SHARD-001: Extract shard key from query
- [ ] SHARD-002: Route to correct shard
- [ ] SHARD-003: Multi-shard query detection
- [ ] SHARD-004: JOIN with same shard key
- [ ] SHARD-005: Cross-shard JOIN handling
- [ ] SHARD-006: Shard key from JWT claims
- [ ] SHARD-007: INSERT shard key extraction
- [ ] SHARD-008: UPDATE shard key extraction
- [ ] SHARD-009: Parameterized query routing
- [ ] SHARD-010: Shard metadata caching

---

## 3. End-to-End Tests

### 3.1 Full Request Flow

**Framework**: Playwright/Cypress for UI, Go test client for API

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| E2E-001 | Login flow through dashboard | User authenticated, JWT issued | P0 |
| E2E-002 | API request with JWT | Request authenticated through PgDog | P0 |
| E2E-003 | Data query through full stack | Dashboard -> API -> PgDog -> Orochi -> DB | P0 |
| E2E-004 | Create resource via UI | New resource visible in database | P0 |
| E2E-005 | Real-time updates (WebSocket) | Changes propagated to UI | P1 |
| E2E-006 | Session timeout handling | User logged out after inactivity | P1 |
| E2E-007 | Error display in UI | Backend errors shown appropriately | P1 |

**Completion Checklist**:
- [ ] E2E-001: Login flow through dashboard
- [ ] E2E-002: API request with JWT
- [ ] E2E-003: Data query through full stack
- [ ] E2E-004: Create resource via UI
- [ ] E2E-005: Real-time updates (WebSocket)
- [ ] E2E-006: Session timeout handling
- [ ] E2E-007: Error display in UI

### 3.2 Multi-Tenant Isolation

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| MT-001 | Tenant A cannot see Tenant B data | RLS enforced via JWT claims | P0 |
| MT-002 | Tenant A cannot modify Tenant B data | Write RLS enforced | P0 |
| MT-003 | Cross-tenant query blocked | Error returned, no data leakage | P0 |
| MT-004 | Admin can access all tenants | Super-admin role bypasses RLS | P1 |
| MT-005 | Tenant ID injection prevented | Cannot override tenant from client | P0 |
| MT-006 | Connection pool isolation | Tenants use separate pools if configured | P2 |

**Completion Checklist**:
- [ ] MT-001: Tenant A cannot see Tenant B data
- [ ] MT-002: Tenant A cannot modify Tenant B data
- [ ] MT-003: Cross-tenant query blocked
- [ ] MT-004: Admin can access all tenants
- [ ] MT-005: Tenant ID injection prevented
- [ ] MT-006: Connection pool isolation

### 3.3 Branch-Aware Routing

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| BRANCH-001 | Route to main branch by default | Queries go to main database | P0 |
| BRANCH-002 | Route to feature branch | Branch header routes to branch DB | P0 |
| BRANCH-003 | Branch creation | New branch cloned from parent | P1 |
| BRANCH-004 | Branch deletion | Branch resources cleaned up | P1 |
| BRANCH-005 | Branch isolation | Branch data not visible from main | P0 |
| BRANCH-006 | Branch merge (if supported) | Changes merged to parent branch | P2 |

**Completion Checklist**:
- [ ] BRANCH-001: Route to main branch by default
- [ ] BRANCH-002: Route to feature branch
- [ ] BRANCH-003: Branch creation
- [ ] BRANCH-004: Branch deletion
- [ ] BRANCH-005: Branch isolation
- [ ] BRANCH-006: Branch merge (if supported)

---

## 4. Performance Tests

### 4.1 Connection Pool Efficiency

**Framework**: k6, wrk, or custom Go benchmark

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| PERF-001 | Connection acquisition latency | p99 < 10ms | P0 |
| PERF-002 | Query latency overhead | Proxy adds < 1ms to simple queries | P0 |
| PERF-003 | Connection reuse rate | > 95% reuse under steady load | P0 |
| PERF-004 | Pool scaling latency | New connections created in < 100ms | P1 |
| PERF-005 | Peak connection handling | Handle 10,000 concurrent connections | P0 |

**Completion Checklist**:
- [ ] PERF-001: Connection acquisition latency
- [ ] PERF-002: Query latency overhead
- [ ] PERF-003: Connection reuse rate
- [ ] PERF-004: Pool scaling latency
- [ ] PERF-005: Peak connection handling

**Benchmark Script (k6)**:
```javascript
import sql from 'k6/x/sql';
import { check, sleep } from 'k6';
import { Trend, Counter } from 'k6/metrics';

const connectionLatency = new Trend('connection_latency');
const queryLatency = new Trend('query_latency');
const queryErrors = new Counter('query_errors');

export const options = {
    scenarios: {
        connection_pool_test: {
            executor: 'ramping-vus',
            startVUs: 10,
            stages: [
                { duration: '30s', target: 100 },
                { duration: '1m', target: 100 },
                { duration: '30s', target: 500 },
                { duration: '1m', target: 500 },
                { duration: '30s', target: 10 },
            ],
        },
    },
    thresholds: {
        'connection_latency': ['p(99)<10'],   // 10ms
        'query_latency': ['p(99)<50'],         // 50ms
        'query_errors': ['count<10'],
    },
};

export default function () {
    const connStart = Date.now();
    const db = sql.open('postgres', __ENV.PGDOG_URL);
    connectionLatency.add(Date.now() - connStart);

    const queryStart = Date.now();
    try {
        const result = db.query('SELECT 1');
        check(result, { 'query succeeded': (r) => r.length > 0 });
    } catch (e) {
        queryErrors.add(1);
    }
    queryLatency.add(Date.now() - queryStart);

    db.close();
    sleep(0.1);
}
```

### 4.2 Query Latency Overhead

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| LAT-001 | Simple SELECT overhead | < 0.5ms added latency | P0 |
| LAT-002 | Parameterized query overhead | < 1ms added latency | P0 |
| LAT-003 | Transaction overhead | < 2ms total overhead for BEGIN/COMMIT | P0 |
| LAT-004 | Auth overhead per request | < 1ms with token caching | P0 |
| LAT-005 | Shard routing overhead | < 2ms for shard key extraction | P1 |

**Completion Checklist**:
- [ ] LAT-001: Simple SELECT overhead
- [ ] LAT-002: Parameterized query overhead
- [ ] LAT-003: Transaction overhead
- [ ] LAT-004: Auth overhead per request
- [ ] LAT-005: Shard routing overhead

### 4.3 Throughput Under Load

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| THRU-001 | Queries per second (read) | > 50,000 QPS for simple SELECTs | P0 |
| THRU-002 | Queries per second (write) | > 10,000 QPS for simple INSERTs | P0 |
| THRU-003 | Transactions per second | > 5,000 TPS for simple transactions | P0 |
| THRU-004 | Sustained load (1 hour) | No degradation over time | P1 |
| THRU-005 | Burst handling | Handle 10x normal load for 60s | P1 |

**Completion Checklist**:
- [ ] THRU-001: Queries per second (read)
- [ ] THRU-002: Queries per second (write)
- [ ] THRU-003: Transactions per second
- [ ] THRU-004: Sustained load (1 hour)
- [ ] THRU-005: Burst handling

### 4.4 Memory Usage

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| MEM-001 | Baseline memory usage | < 100MB for idle proxy | P0 |
| MEM-002 | Memory per connection | < 50KB per pooled connection | P0 |
| MEM-003 | Memory under load | < 1GB at 10,000 connections | P0 |
| MEM-004 | Memory leak detection | No growth over 24h sustained load | P0 |
| MEM-005 | Token cache memory | Bounded by configured limit | P1 |

**Completion Checklist**:
- [ ] MEM-001: Baseline memory usage
- [ ] MEM-002: Memory per connection
- [ ] MEM-003: Memory under load
- [ ] MEM-004: Memory leak detection
- [ ] MEM-005: Token cache memory

---

## 5. Chaos/Resilience Tests

### 5.1 Failover Behavior

**Framework**: Chaos Mesh or Litmus for Kubernetes, manual for Docker Compose

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| FAIL-001 | Primary PostgreSQL failure | Automatic failover to new primary | P0 |
| FAIL-002 | Replica failure | Queries rerouted to remaining replicas | P0 |
| FAIL-003 | PgDog instance failure | Load balancer routes to healthy instance | P0 |
| FAIL-004 | Graceful PgDog restart | In-flight queries completed or retried | P0 |
| FAIL-005 | All replicas fail | Reads degraded to primary | P1 |
| FAIL-006 | Split-brain prevention | Only one primary accepts writes | P0 |

**Completion Checklist**:
- [ ] FAIL-001: Primary PostgreSQL failure
- [ ] FAIL-002: Replica failure
- [ ] FAIL-003: PgDog instance failure
- [ ] FAIL-004: Graceful PgDog restart
- [ ] FAIL-005: All replicas fail
- [ ] FAIL-006: Split-brain prevention

**Chaos Test Script (Chaos Mesh)**:
```yaml
apiVersion: chaos-mesh.org/v1alpha1
kind: PodChaos
metadata:
  name: primary-failure-test
spec:
  action: pod-failure
  mode: one
  selector:
    namespaces:
      - orochi-cloud
    labelSelectors:
      cnpg.io/cluster: test-cluster
      role: primary
  duration: "60s"
  scheduler:
    cron: "@every 5m"
```

### 5.2 Network Partition Handling

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| NET-001 | Network partition between proxy and DB | Connections closed, clients notified | P0 |
| NET-002 | Network partition between primary and replicas | Replicas promoted if quorum lost | P0 |
| NET-003 | Partial network degradation | Queries routed to healthy nodes | P1 |
| NET-004 | DNS resolution failure | Cached IPs used, graceful degradation | P1 |
| NET-005 | Network restored after partition | Connections re-established | P0 |

**Completion Checklist**:
- [ ] NET-001: Network partition between proxy and DB
- [ ] NET-002: Network partition between primary and replicas
- [ ] NET-003: Partial network degradation
- [ ] NET-004: DNS resolution failure
- [ ] NET-005: Network restored after partition

### 5.3 Connection Recovery

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| REC-001 | Backend connection reset | New connection established transparently | P0 |
| REC-002 | Pool connection poisoning | Bad connections detected and removed | P0 |
| REC-003 | Mass disconnection recovery | Pool rebuilt without thundering herd | P0 |
| REC-004 | Connection timeout recovery | Timed out connections cleaned up | P0 |
| REC-005 | Client reconnection storm | Rate limiting prevents overload | P1 |

**Completion Checklist**:
- [ ] REC-001: Backend connection reset
- [ ] REC-002: Pool connection poisoning
- [ ] REC-003: Mass disconnection recovery
- [ ] REC-004: Connection timeout recovery
- [ ] REC-005: Client reconnection storm

### 5.4 Two-Phase Commit (2PC) Failure Scenarios

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| 2PC-001 | Coordinator failure during prepare | Transaction rolled back on all shards | P0 |
| 2PC-002 | Participant failure during prepare | Transaction rolled back | P0 |
| 2PC-003 | Coordinator failure after prepare | Transaction recoverable on restart | P0 |
| 2PC-004 | Participant failure after prepare | Prepared transaction persists | P0 |
| 2PC-005 | Network failure during commit | Timeout and rollback or recovery | P0 |
| 2PC-006 | Prepared transaction cleanup | Orphaned prepared txns cleaned up | P1 |
| 2PC-007 | Heuristic decisions | Manual resolution for stuck transactions | P2 |

**Completion Checklist**:
- [ ] 2PC-001: Coordinator failure during prepare
- [ ] 2PC-002: Participant failure during prepare
- [ ] 2PC-003: Coordinator failure after prepare
- [ ] 2PC-004: Participant failure after prepare
- [ ] 2PC-005: Network failure during commit
- [ ] 2PC-006: Prepared transaction cleanup
- [ ] 2PC-007: Heuristic decisions

---

## 6. Security Tests

### 6.1 Token Validation

**Framework**: OWASP ZAP, custom security test suite

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| SEC-001 | Reject unsigned tokens | `alg: none` rejected | P0 |
| SEC-002 | Reject tokens with wrong secret | Invalid signature error | P0 |
| SEC-003 | Token replay prevention (if jti) | Same token rejected twice | P1 |
| SEC-004 | Token scope validation | Limited scope tokens restricted | P1 |
| SEC-005 | Key confusion attack prevention | RSA/HMAC confusion blocked | P0 |
| SEC-006 | JWT header injection | Malformed headers rejected | P0 |

**Completion Checklist**:
- [ ] SEC-001: Reject unsigned tokens
- [ ] SEC-002: Reject tokens with wrong secret
- [ ] SEC-003: Token replay prevention (if jti)
- [ ] SEC-004: Token scope validation
- [ ] SEC-005: Key confusion attack prevention
- [ ] SEC-006: JWT header injection

### 6.2 SQL Injection Through Proxy

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| SQLI-001 | Injection in shard key | No SQL execution from shard key | P0 |
| SQLI-002 | Injection in session variables | Variables properly escaped | P0 |
| SQLI-003 | Injection in query comments | Comments sanitized if parsed | P1 |
| SQLI-004 | Second-order injection | Stored values don't execute as SQL | P0 |
| SQLI-005 | Parameterized query bypass | All input treated as data | P0 |

**Completion Checklist**:
- [ ] SQLI-001: Injection in shard key
- [ ] SQLI-002: Injection in session variables
- [ ] SQLI-003: Injection in query comments
- [ ] SQLI-004: Second-order injection
- [ ] SQLI-005: Parameterized query bypass

### 6.3 TLS/SSL Verification

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| TLS-001 | Require TLS for client connections | Plaintext connections rejected | P0 |
| TLS-002 | TLS version enforcement | TLS 1.2+ only | P0 |
| TLS-003 | Certificate validation (client to proxy) | Invalid certs rejected | P0 |
| TLS-004 | Certificate validation (proxy to DB) | Proxy validates DB certs | P0 |
| TLS-005 | mTLS support (if enabled) | Client certs validated | P1 |
| TLS-006 | Cipher suite restrictions | Weak ciphers disabled | P1 |
| TLS-007 | Certificate rotation | Hot reload of certificates | P1 |

**Completion Checklist**:
- [ ] TLS-001: Require TLS for client connections
- [ ] TLS-002: TLS version enforcement
- [ ] TLS-003: Certificate validation (client to proxy)
- [ ] TLS-004: Certificate validation (proxy to DB)
- [ ] TLS-005: mTLS support (if enabled)
- [ ] TLS-006: Cipher suite restrictions
- [ ] TLS-007: Certificate rotation

### 6.4 Unauthorized Access Attempts

| Test ID | Test Case | Pass Criteria | Priority |
|---------|-----------|---------------|----------|
| AUTH-001 | Access without token | 401 Unauthorized | P0 |
| AUTH-002 | Access with expired token | 401 Unauthorized | P0 |
| AUTH-003 | Access with wrong role | 403 Forbidden | P0 |
| AUTH-004 | Rate limiting on auth failures | Throttle after N failures | P0 |
| AUTH-005 | Brute force detection | Account lockout or CAPTCHA | P1 |
| AUTH-006 | Audit logging of failures | All failures logged with context | P0 |

**Completion Checklist**:
- [ ] AUTH-001: Access without token
- [ ] AUTH-002: Access with expired token
- [ ] AUTH-003: Access with wrong role
- [ ] AUTH-004: Rate limiting on auth failures
- [ ] AUTH-005: Brute force detection
- [ ] AUTH-006: Audit logging of failures

---

## 7. Test Infrastructure

### 7.1 Docker Compose Test Environment

```yaml
# docker-compose.test.yml
version: '3.8'

services:
  postgres-primary:
    image: ghcr.io/orochi-db/orochi-pg:18
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: testpass
      POSTGRES_DB: orochi_test
    volumes:
      - ./init-scripts:/docker-entrypoint-initdb.d
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres"]
      interval: 5s
      timeout: 5s
      retries: 5

  postgres-replica:
    image: ghcr.io/orochi-db/orochi-pg:18
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: testpass
      POSTGRES_PRIMARY_HOST: postgres-primary
    depends_on:
      postgres-primary:
        condition: service_healthy

  pgdog:
    image: pgdog/pgdog:latest
    ports:
      - "6432:6432"
    environment:
      PGDOG_LISTEN_ADDRESS: "0.0.0.0:6432"
      PGDOG_PRIMARY_HOST: postgres-primary
      PGDOG_REPLICA_HOST: postgres-replica
      PGDOG_JWT_SECRET: "test-jwt-secret-32-bytes-long!!"
    volumes:
      - ./pgdog.toml:/etc/pgdog/pgdog.toml
    depends_on:
      - postgres-primary
      - postgres-replica

  test-runner:
    build:
      context: .
      dockerfile: Dockerfile.test
    environment:
      PGDOG_URL: "postgres://postgres:testpass@pgdog:6432/orochi_test"
      TEST_JWT_SECRET: "test-jwt-secret-32-bytes-long!!"
    depends_on:
      - pgdog
    command: ["go", "test", "-v", "./..."]
```

### 7.2 CI/CD Pipeline Integration

```yaml
# .github/workflows/pgdog-integration-tests.yml
name: PgDog Integration Tests

on:
  push:
    branches: [main, 'feature/pgdog-*']
  pull_request:
    branches: [main]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install Rust
        uses: actions-rs/toolchain@v1
        with:
          toolchain: stable

      - name: Run Rust Unit Tests
        run: |
          cd pgdog-auth-plugin
          cargo test --all-features

  integration-tests:
    runs-on: ubuntu-latest
    needs: unit-tests
    steps:
      - uses: actions/checkout@v4

      - name: Start Test Environment
        run: docker-compose -f docker-compose.test.yml up -d

      - name: Wait for Services
        run: |
          ./scripts/wait-for-services.sh

      - name: Run Integration Tests
        run: |
          go test -v -tags=integration ./tests/integration/...

      - name: Run E2E Tests
        run: |
          npm run test:e2e

      - name: Collect Logs
        if: failure()
        run: docker-compose -f docker-compose.test.yml logs > test-logs.txt

      - name: Upload Logs
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: test-logs
          path: test-logs.txt

  performance-tests:
    runs-on: ubuntu-latest
    needs: integration-tests
    if: github.ref == 'refs/heads/main'
    steps:
      - uses: actions/checkout@v4

      - name: Start Test Environment
        run: docker-compose -f docker-compose.perf.yml up -d

      - name: Run k6 Performance Tests
        run: |
          k6 run tests/performance/connection_pool.js
          k6 run tests/performance/query_latency.js

      - name: Upload Performance Report
        uses: actions/upload-artifact@v3
        with:
          name: performance-report
          path: performance-results/
```

### 7.3 Test Data Generators

```go
// tests/generators/jwt.go
package generators

import (
    "time"
    "github.com/golang-jwt/jwt/v5"
)

type ClaimsGenerator struct {
    Secret string
}

func (g *ClaimsGenerator) GenerateValidToken(userID, role, tenantID string) string {
    claims := jwt.MapClaims{
        "sub":       userID,
        "role":      role,
        "tenant_id": tenantID,
        "exp":       time.Now().Add(time.Hour).Unix(),
        "iat":       time.Now().Unix(),
    }
    token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
    tokenString, _ := token.SignedString([]byte(g.Secret))
    return tokenString
}

func (g *ClaimsGenerator) GenerateExpiredToken(userID string) string {
    claims := jwt.MapClaims{
        "sub": userID,
        "exp": time.Now().Add(-time.Hour).Unix(), // Expired
        "iat": time.Now().Add(-2 * time.Hour).Unix(),
    }
    token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
    tokenString, _ := token.SignedString([]byte(g.Secret))
    return tokenString
}

func (g *ClaimsGenerator) GenerateTokenWithWrongSignature(userID string) string {
    claims := jwt.MapClaims{
        "sub": userID,
        "exp": time.Now().Add(time.Hour).Unix(),
    }
    token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
    tokenString, _ := token.SignedString([]byte("wrong-secret"))
    return tokenString
}
```

---

## 8. Test Execution Schedule

### Pre-Commit (Local)
- All unit tests (Rust + Go)
- Fast integration tests (< 5 minutes)

### CI Pipeline (PR)
- All unit tests
- All integration tests
- Basic E2E tests

### Nightly Build
- Full test suite including performance tests
- Chaos tests (subset)
- Security scan

### Weekly
- Full chaos test suite
- Extended performance tests (1-hour sustained load)
- Memory leak detection tests

### Pre-Release
- Complete test suite
- Manual security review
- Performance regression comparison

---

## 9. Test Metrics and Reporting

### Key Metrics to Track

| Metric | Target | Alert Threshold |
|--------|--------|-----------------|
| Unit Test Coverage | > 80% | < 70% |
| Integration Test Pass Rate | 100% | < 95% |
| P99 Latency (proxy overhead) | < 2ms | > 5ms |
| Connection Pool Efficiency | > 95% | < 85% |
| Memory Usage (peak) | < 1GB | > 2GB |
| Test Suite Duration | < 30 min | > 60 min |

### Reporting Dashboard

Tests should output results in JUnit XML format for CI integration:

```bash
# Go tests
go test -v ./... -json | go-junit-report > report.xml

# Rust tests
cargo test -- --format=json | cargo2junit > report.xml

# k6 performance tests
k6 run --out json=results.json script.js
```

---

## 10. Appendix

### A. Required Dependencies

| Tool | Version | Purpose |
|------|---------|---------|
| Docker | 24+ | Container runtime |
| Docker Compose | 2.20+ | Multi-container orchestration |
| Go | 1.22+ | Integration tests |
| Rust | 1.75+ | Unit tests for auth plugin |
| k6 | 0.48+ | Performance tests |
| Chaos Mesh | 2.6+ | Chaos engineering |
| OWASP ZAP | 2.14+ | Security scanning |

### B. Environment Variables

```bash
# Test configuration
export PGDOG_URL="postgres://user:pass@localhost:6432/testdb"
export TEST_JWT_SECRET="your-32-byte-test-secret-here!!"
export TEST_TENANT_ID="test-tenant-001"
export PGDOG_ADMIN_URL="http://localhost:9090"

# Performance test configuration
export K6_VUS=100
export K6_DURATION="5m"
export PERF_TARGET_QPS=10000
```

### C. Test Data Setup

```sql
-- init-scripts/01-setup-orochi.sql
CREATE EXTENSION orochi;

-- Create distributed table for tests
SELECT create_distributed_table('users', 'tenant_id');

-- Create test tenants
INSERT INTO tenants (id, name) VALUES
    ('tenant-001', 'Test Tenant 1'),
    ('tenant-002', 'Test Tenant 2');

-- Create RLS policies
ALTER TABLE users ENABLE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON users
    USING (tenant_id = current_setting('request.jwt.claim.tenant_id', true));
```

---

## Document Control

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-01-31 | QA Team | Initial strategy document |
