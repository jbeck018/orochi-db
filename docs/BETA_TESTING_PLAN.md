# Orochi DB Beta Testing Plan

A comprehensive testing plan to validate all aspects of Orochi DB before beta release.

## Overview

**Test Environment:**
- Dashboard: https://orochi-dashboard.fly.dev
- Control Plane API: https://orochi-control-plane.fly.dev
- Region: sjc (San Jose)

**Test Categories:**
1. Dashboard UI/UX Testing
2. Authentication Flow Testing
3. Control Plane API Testing
4. Autoscaling Testing (Horizontal & Vertical)
5. PostgreSQL Extension Testing
6. End-to-End Integration Testing

---

## 1. Dashboard UI/UX Testing

### 1.1 Theme Validation
- [ ] Verify Fira Code font loads correctly
- [ ] Check primary color (amber/yellow) is applied throughout
- [ ] Test dark mode toggle functionality
- [ ] Verify pure black background in dark mode
- [ ] Ensure consistent styling across all pages
- [ ] Test responsive design on mobile/tablet viewports

### 1.2 Navigation Testing
- [ ] Sidebar navigation works correctly
- [ ] All routes load without errors
- [ ] Breadcrumb navigation accurate
- [ ] Deep linking works (direct URL access)

### 1.3 Page-Specific Tests

#### Home/Dashboard
- [ ] Landing page loads correctly
- [ ] Feature cards display properly
- [ ] Quick actions are functional

#### Clusters List
- [ ] Cluster list loads and displays
- [ ] Cluster cards show correct status
- [ ] Search/filter works
- [ ] Create new cluster button navigates correctly

#### Cluster Detail
- [ ] Overview tab displays metrics
- [ ] All tabs (Columnar, Time-Series, Sharding, Topology, CDC, Pipelines, Settings) load
- [ ] Charts render correctly
- [ ] Real-time updates work

#### Settings
- [ ] User preferences save correctly
- [ ] Theme settings persist
- [ ] API key management works

---

## 2. Authentication Flow Testing

### 2.1 Registration
- [ ] Email registration form validates input
- [ ] Password requirements enforced
- [ ] Email confirmation sent
- [ ] Successful account creation

### 2.2 Login
- [ ] Email/password login works
- [ ] Remember me functionality
- [ ] Error messages for invalid credentials
- [ ] Redirect to dashboard after login

### 2.3 Password Reset
- [ ] Forgot password sends email
- [ ] Reset link works
- [ ] New password saved successfully

### 2.4 Session Management
- [ ] JWT tokens refresh correctly
- [ ] Logout clears session
- [ ] Protected routes redirect to login
- [ ] Session timeout works

---

## 3. Control Plane API Testing

### 3.1 Health & Status Endpoints
```bash
# Health check
curl -s https://orochi-control-plane.fly.dev/health | jq

# Expected: {"status":"healthy","version":"..."}
```
- [ ] `/health` returns 200 with healthy status
- [ ] Response time < 500ms

### 3.2 Authentication Endpoints
```bash
# Register
curl -X POST https://orochi-control-plane.fly.dev/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"email":"test@example.com","password":"SecurePass123!"}'

# Login
curl -X POST https://orochi-control-plane.fly.dev/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"test@example.com","password":"SecurePass123!"}'
```
- [ ] Registration returns user and token
- [ ] Login returns valid JWT
- [ ] Invalid credentials return 401

### 3.3 Cluster Management Endpoints
```bash
# List clusters (authenticated)
curl -s https://orochi-control-plane.fly.dev/api/v1/clusters \
  -H "Authorization: Bearer $TOKEN" | jq

# Create cluster
curl -X POST https://orochi-control-plane.fly.dev/api/v1/clusters \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"test-cluster","region":"sjc","version":"16"}'

# Get cluster details
curl -s https://orochi-control-plane.fly.dev/api/v1/clusters/{id} \
  -H "Authorization: Bearer $TOKEN" | jq

# Delete cluster
curl -X DELETE https://orochi-control-plane.fly.dev/api/v1/clusters/{id} \
  -H "Authorization: Bearer $TOKEN"
```
- [ ] List clusters returns array
- [ ] Create cluster provisions successfully
- [ ] Get cluster returns full details
- [ ] Delete cluster cleans up resources

### 3.4 CORS Testing
- [ ] Dashboard origin allowed
- [ ] Preflight OPTIONS requests succeed
- [ ] Cross-origin POST/PUT/DELETE work

---

## 4. Autoscaling Testing

### 4.1 Horizontal Autoscaling (Fly.io Machines)

**Current Configuration:**
- Control Plane: min=1, max=5 machines
- Dashboard: min=1, max=1 machine (auto_stop_machines=suspend)

**Test Procedure:**
```bash
# Check current machine count
fly status -a orochi-control-plane

# Generate load to trigger scaling
hey -n 1000 -c 50 https://orochi-control-plane.fly.dev/health

# Monitor machine scaling
watch -n 5 'fly status -a orochi-control-plane'
```

- [ ] Machines scale up under load (> soft_limit=400 connections)
- [ ] Machines scale down after load decreases
- [ ] Minimum of 1 machine always running
- [ ] Maximum of 5 machines respected

### 4.2 Vertical Autoscaling (Fly.io)

**Test Procedure:**
```bash
# Check current machine specs
fly machines list -a orochi-control-plane

# Scale vertically
fly scale memory 1024 -a orochi-control-plane

# Verify scaling
fly machines list -a orochi-control-plane
```

- [ ] Memory scaling works
- [ ] CPU scaling works
- [ ] Rolling restart maintains availability

### 4.3 Database Autoscaling (Future - CloudNativePG)

**Note:** Database autoscaling requires Kubernetes/CloudNativePG. Since FKS is in private beta, this is deferred.

When available, test:
- [ ] PostgreSQL replica scaling
- [ ] Connection pooling scales with load
- [ ] Storage autoscaling triggers at threshold

---

## 5. PostgreSQL Extension Testing

### 5.1 Extension Installation
```sql
-- Connect to PostgreSQL cluster
psql postgresql://user:pass@host:5432/dbname

-- Create extension
CREATE EXTENSION orochi;

-- Verify
SELECT * FROM pg_extension WHERE extname = 'orochi';
\dx orochi
```
- [ ] Extension creates without errors
- [ ] All SQL functions registered
- [ ] Shared library loads

### 5.2 Columnar Storage
```sql
-- Create columnar table
SELECT orochi_create_columnar_table(
  'test_columnar',
  'id INTEGER PRIMARY KEY, name TEXT, value NUMERIC, created_at TIMESTAMPTZ'
);

-- Insert data
INSERT INTO test_columnar VALUES
  (1, 'test1', 100.5, NOW()),
  (2, 'test2', 200.5, NOW());

-- Verify columnar storage
SELECT orochi_columnar_stats('test_columnar');
```
- [ ] Columnar table creation works
- [ ] Data inserts correctly
- [ ] Compression statistics available
- [ ] Query performance improved for analytics

### 5.3 Time-Series (Hypertables)
```sql
-- Create hypertable
SELECT orochi_create_hypertable(
  'metrics',
  'id SERIAL, device_id INT, value DOUBLE PRECISION, ts TIMESTAMPTZ',
  'ts',
  '1 day'
);

-- Insert time-series data
INSERT INTO metrics (device_id, value, ts)
SELECT
  (random() * 100)::int,
  random() * 1000,
  NOW() - (n || ' hours')::interval
FROM generate_series(1, 1000) n;

-- Verify chunks
SELECT orochi_hypertable_chunks('metrics');
```
- [ ] Hypertable creation works
- [ ] Automatic chunking by time interval
- [ ] Chunk pruning in queries

### 5.4 Sharding (Distribution)
```sql
-- Create distributed table
SELECT orochi_create_distributed_table(
  'orders',
  'id SERIAL PRIMARY KEY, customer_id INT, amount NUMERIC, created_at TIMESTAMPTZ',
  'customer_id',
  8  -- shard count
);

-- Insert and verify distribution
INSERT INTO orders (customer_id, amount, created_at)
SELECT
  (random() * 1000)::int,
  random() * 500,
  NOW()
FROM generate_series(1, 10000);

-- Check shard distribution
SELECT orochi_shard_stats('orders');
```
- [ ] Distributed table creation works
- [ ] Data distributed by hash
- [ ] Shard-level queries work

### 5.5 Vector Operations
```sql
-- Create vector column
CREATE TABLE embeddings (
  id SERIAL PRIMARY KEY,
  embedding FLOAT4[] NOT NULL
);

-- Insert vectors
INSERT INTO embeddings (embedding)
SELECT ARRAY(SELECT random()::float4 FROM generate_series(1, 1536))
FROM generate_series(1, 1000);

-- Similarity search
SELECT id, orochi_vector_cosine_similarity(
  embedding,
  ARRAY(SELECT random()::float4 FROM generate_series(1, 1536))
) as similarity
FROM embeddings
ORDER BY similarity DESC
LIMIT 10;
```
- [ ] Vector operations work
- [ ] SIMD acceleration active
- [ ] Similarity search returns results

---

## 6. End-to-End Integration Testing

### 6.1 Full User Journey
1. [ ] Register new account via dashboard
2. [ ] Receive confirmation email
3. [ ] Login to dashboard
4. [ ] Create new cluster
5. [ ] Wait for cluster provisioning
6. [ ] Connect to cluster via psql
7. [ ] Create orochi extension
8. [ ] Create columnar and time-series tables
9. [ ] Insert and query data
10. [ ] View metrics in dashboard
11. [ ] Scale cluster
12. [ ] Delete cluster
13. [ ] Logout

### 6.2 Error Handling
- [ ] API errors display user-friendly messages
- [ ] Network failures show retry options
- [ ] Invalid input shows validation errors
- [ ] 404 pages work correctly

### 6.3 Performance Benchmarks
```bash
# API response times
hey -n 1000 -c 10 https://orochi-control-plane.fly.dev/health

# Dashboard load time
lighthouse https://orochi-dashboard.fly.dev --output=json
```

**Targets:**
- [ ] API p95 latency < 200ms
- [ ] Dashboard TTI < 3s
- [ ] First Contentful Paint < 1.5s

---

## 7. Load Testing Script

```bash
#!/bin/bash
# save as test_autoscaling.sh

API_URL="https://orochi-control-plane.fly.dev"
DASHBOARD_URL="https://orochi-dashboard.fly.dev"

echo "=== Orochi DB Beta Load Test ==="

# Install hey if not present
command -v hey >/dev/null 2>&1 || { echo "Installing hey..."; brew install hey; }

# Phase 1: Baseline
echo "Phase 1: Baseline (10 req/s for 30s)"
hey -z 30s -q 10 $API_URL/health
fly status -a orochi-control-plane

# Phase 2: Moderate Load
echo "Phase 2: Moderate Load (50 req/s for 60s)"
hey -z 60s -q 50 $API_URL/health
fly status -a orochi-control-plane

# Phase 3: High Load (trigger scaling)
echo "Phase 3: High Load (200 req/s for 120s)"
hey -z 120s -q 200 -c 100 $API_URL/health
fly status -a orochi-control-plane

# Phase 4: Cooldown
echo "Phase 4: Cooldown (monitor scale-down)"
for i in {1..12}; do
  sleep 30
  echo "Check $i/12:"
  fly status -a orochi-control-plane | grep -E "(PROCESS|app)"
done

echo "=== Load Test Complete ==="
```

---

## 8. Test Results Summary

| Category | Total Tests | Passed | Failed | Blocked |
|----------|-------------|--------|--------|---------|
| Dashboard UI | 15 | - | - | - |
| Authentication | 8 | - | - | - |
| Control Plane API | 12 | - | - | - |
| Autoscaling | 8 | - | - | FKS blocked |
| PostgreSQL Extension | 20 | - | - | - |
| E2E Integration | 15 | - | - | - |
| **Total** | **78** | - | - | - |

---

## 9. Known Issues & Limitations

1. **FKS Private Beta**: Fly Kubernetes Service is in private beta - cannot provision K8s clusters yet
2. **Database Autoscaling**: Requires CloudNativePG on K8s, blocked by FKS availability
3. **gRPC Service**: TCP service on port 9090 requires WireGuard VPN for private network access

---

## 10. Beta Release Checklist

### Must Have (P0)
- [ ] Dashboard loads and is functional
- [ ] User registration and login work
- [ ] Control plane health checks pass
- [ ] Basic cluster lifecycle (create/list/delete) works
- [ ] PostgreSQL extension installs
- [ ] Horizontal autoscaling works for control plane

### Should Have (P1)
- [ ] All dashboard pages functional
- [ ] Full authentication flow (including password reset)
- [ ] Cluster scaling works
- [ ] Columnar, time-series, and sharding features work

### Nice to Have (P2)
- [ ] Vertical autoscaling tested
- [ ] Vector operations validated
- [ ] CDC and pipelines functional
- [ ] Performance benchmarks meet targets

---

## Execution Commands

```bash
# Quick smoke test
curl -s https://orochi-control-plane.fly.dev/health | jq
curl -s https://orochi-dashboard.fly.dev/ -o /dev/null -w "%{http_code}"

# Full API test suite (using httpie)
http GET https://orochi-control-plane.fly.dev/health
http POST https://orochi-control-plane.fly.dev/api/v1/auth/register \
  email=test@example.com password=Test123!

# Monitor autoscaling
watch -n 10 'fly machines list -a orochi-control-plane'
```

---

*Created: 2026-01-17*
*Version: 1.0*
*Author: Claude Code*
