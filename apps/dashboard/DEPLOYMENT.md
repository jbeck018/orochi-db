# Orochi Cloud Dashboard - Fly.io Deployment Guide

This guide provides complete instructions for deploying the Orochi Cloud Dashboard (React 19 + Vite application) to fly.io.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Fly.io Setup](#flyio-setup)
3. [Dockerfile Configuration](#dockerfile-configuration)
4. [fly.toml Configuration](#flytoml-configuration)
5. [Environment Variables & Secrets](#environment-variables--secrets)
6. [Build & Deployment](#build--deployment)
7. [Health Checks & Monitoring](#health-checks--monitoring)
8. [Custom Domain Setup](#custom-domain-setup)
9. [Scaling & Performance](#scaling--performance)
10. [Troubleshooting](#troubleshooting)

---

## Prerequisites

Before deploying to fly.io, ensure you have:

### Required Tools

- **Fly.io CLI**: Latest version installed
  ```bash
  # macOS
  brew install flyctl

  # Linux
  curl -L https://fly.io/install.sh | sh

  # Windows (with Windows Package Manager)
  winget install flyctl
  ```

- **Docker**: For local testing (optional but recommended)
  ```bash
  docker --version
  ```

- **Git**: Version control and CI/CD integration
  ```bash
  git --version
  ```

- **Node.js**: v18+ (for local development)
  ```bash
  node --version
  npm --version
  ```

### Fly.io Account

1. Create a fly.io account at https://fly.io
2. Authenticate with the CLI:
   ```bash
   flyctl auth login
   ```
3. Verify authentication:
   ```bash
   flyctl whoami
   ```

### DNS & Domain (Optional)

- Registered domain name (for custom domain setup)
- Access to domain registrar's DNS settings
- SSL/TLS certificate (automatically handled by fly.io via Let's Encrypt)

---

## Fly.io Setup

### Step 1: Create Fly App

Create a new Fly.io application:

```bash
# Navigate to dashboard directory
cd orochi-cloud/dashboard

# Initialize Fly app (interactive setup)
flyctl launch

# Or create with specific name
flyctl launch --name orochi-dashboard --region sjc
```

**Recommended Regions**:
- `sjc` - San Jose, California (US West)
- `iad` - Northern Virginia (US East)
- `ams` - Amsterdam (Europe)
- `syd` - Sydney (Australia)

Choose based on your primary user location for lowest latency.

### Step 2: Verify App Creation

```bash
# List your apps
flyctl apps list

# View app details
flyctl info

# View app status
flyctl status
```

---

## Dockerfile Configuration

Create a production-optimized Dockerfile for the React app.

### File: `Dockerfile`

```dockerfile
# Build stage
FROM node:18-alpine as builder

# Set working directory
WORKDIR /app

# Copy package files
COPY package*.json ./

# Install dependencies
RUN npm ci --only=production && \
    npm ci --only=development

# Copy source code
COPY . .

# Build the application
RUN npm run build

# Runtime stage
FROM node:18-alpine

# Install dumb-init for proper signal handling
RUN apk add --no-cache dumb-init

WORKDIR /app

# Copy built application from builder
COPY --from=builder /app/dist ./dist

# Copy package files for server dependencies
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/package*.json ./

# Create non-root user for security
RUN addgroup -g 1001 -S nodejs && \
    adduser -S nodejs -u 1001

# Change ownership to non-root user
RUN chown -R nodejs:nodejs /app

USER nodejs

# Expose port
EXPOSE 3000

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD node -e "require('http').get('http://localhost:3000', (r) => {if (r.statusCode !== 200) throw new Error(r.statusCode)})"

# Use dumb-init to handle signals properly
ENTRYPOINT ["dumb-init", "--"]

# Start application
CMD ["node", "dist/server/server.js"]
```

### Key Design Decisions

1. **Multi-stage build**: Reduces final image size by separating build and runtime
2. **Alpine Linux**: Minimal base image (~5MB) reduces deployment size and attack surface
3. **Non-root user**: Improves security by running container as non-privileged user
4. **dumb-init**: Ensures proper signal handling for graceful shutdowns
5. **Health check**: Enables fly.io to detect unhealthy instances and auto-restart

### Optimization Techniques

- `npm ci` instead of `npm install` for production consistency
- Separate production vs development dependency installation
- Layer caching: Dependencies installed before source code
- Minimal runtime dependencies (no dev tools in final image)

---

## fly.toml Configuration

Create the fly.toml configuration file for Fly.io deployment settings.

### File: `fly.toml`

```toml
# Fly.io Application Configuration for Orochi Cloud Dashboard

app = "orochi-dashboard"
primary_region = "sjc"

# Build configuration
[build]
dockerfile = "Dockerfile"
docker_build_args = {}

# Environment variables (non-sensitive)
[env]
NODE_ENV = "production"

# HTTP service configuration
[[services]]
internal_port = 3000
processes = ["app"]

# TCP configuration
[[services.ports]]
port = 80
handlers = ["http"]
force_https = true

[[services.ports]]
port = 443
handlers = ["tls", "http"]

# Health check configuration
[services.tcp_checks]
enabled = true

[services.http_checks]
enabled = true
interval = "30s"
timeout = "5s"
grace_period = "5s"
method = "GET"
path = "/"

# Auto-stop configuration
[[services.concurrency]]
type = "connections"
hard_limit = 1000
soft_limit = 900

# Metrics port (optional)
[[services.ports]]
port = 9090
handlers = ["metrics"]

# VM configuration
[env.production]
SIZE = "shared-cpu-2x"
MEMORY = 512

# Scaling configuration
[processes]
app = "node dist/server/server.js"

# Mounts (if using persistent storage for logs)
# [[mounts]]
# source = "logs"
# destination = "/app/logs"
# size_gb = 10

# Swap configuration
[swap]
size_mb = 512
```

### Configuration Sections Explained

#### `[build]`
- Specifies Dockerfile location and build arguments
- Supports docker build context configuration

#### `[env]`
- Global environment variables accessible to all processes
- Use this only for non-sensitive configuration
- Sensitive values go in fly.io secrets (see next section)

#### `[[services]]`
- Defines application services (web server, workers, etc.)
- `internal_port`: Port application listens on inside container
- Multiple services support multi-process applications

#### `[[services.ports]]`
- HTTP on port 80: Auto-redirects to HTTPS
- HTTPS on port 443: Primary endpoint
- Force HTTPS: Recommended for security

#### `[services.http_checks]`
- Health check configuration for load balancer
- Verifies application is running correctly
- Auto-restarts unhealthy instances
- Configurable intervals and grace periods

#### `[processes]`
- Names and commands for each process type
- `app`: Primary web process
- Additional processes for background workers, schedulers, etc.

#### `[env.production]`
- VM size: `shared-cpu-2x` (recommended for dashboards)
- Memory: 512MB (adjust based on performance testing)
- Options: `shared-cpu-1x` (256MB), `shared-cpu-2x` (512MB), `dedicated-cpu-1x` (2GB), etc.

---

## Environment Variables & Secrets

### Step 1: Define Environment Variables

The dashboard uses the following environment variables:

#### Build-Time Variables (in .env file)

```bash
# .env.production
VITE_API_URL=https://api.yourdomain.com
VITE_APP_ENV=production
```

#### Runtime Variables (via fly.io)

These are handled through Fly.io secrets and environment variables in fly.toml.

### Step 2: Set Secrets in Fly.io

Secrets are encrypted environment variables for sensitive data.

```bash
# Set single secret
flyctl secrets set VITE_API_URL=https://api.yourdomain.com

# Set multiple secrets at once
flyctl secrets set \
  VITE_API_URL=https://api.yourdomain.com \
  SECRET_API_KEY=your-secret-key

# View secrets (masked)
flyctl secrets list

# Remove a secret
flyctl secrets unset VITE_API_URL

# Unset all secrets
flyctl secrets unset --all
```

### Step 3: Build-Time Environment Variables

For variables needed during the Docker build process:

```bash
# Pass build args to Fly.io
flyctl deploy --build-arg VITE_API_URL=https://api.yourdomain.com

# Or update fly.toml:
[build]
docker_build_args = {
  VITE_API_URL = "https://api.yourdomain.com"
}
```

### Step 4: Environment Variable Reference

| Variable | Type | Purpose | Example |
|----------|------|---------|---------|
| `VITE_API_URL` | Build-time | Backend API endpoint | `https://api.example.com` |
| `NODE_ENV` | Runtime | Node.js environment | `production` |
| `PORT` | Runtime | Server port (usually 3000) | `3000` |

### Step 5: Local Development

For local testing with different environments:

```bash
# Development
cp .env.example .env
echo "VITE_API_URL=http://localhost:8080" >> .env
npm run dev

# Staging preview
echo "VITE_API_URL=https://api-staging.example.com" > .env.staging
npm run build -- --mode staging

# Production
echo "VITE_API_URL=https://api.example.com" > .env.production
npm run build
```

---

## Build & Deployment

### Step 1: Local Build Verification

Before deploying, test the build locally:

```bash
# Install dependencies
npm ci

# Type check
npm run type-check

# Lint code
npm run lint

# Build application
npm run build

# Verify build output
ls -lah dist/
```

### Step 2: Docker Build Testing (Optional)

Test the Docker image locally before deploying:

```bash
# Build image
docker build -t orochi-dashboard:latest .

# Run container locally
docker run -p 3000:3000 \
  -e VITE_API_URL=http://localhost:8080 \
  orochi-dashboard:latest

# Test the app
curl http://localhost:3000

# Stop container
docker stop <container-id>
```

### Step 3: Deploy to Fly.io

#### Initial Deployment

```bash
# Deploy with automatic image building
flyctl deploy

# Or specify build arguments
flyctl deploy --build-arg VITE_API_URL=https://api.yourdomain.com

# Deploy to specific region
flyctl deploy --region sjc

# Deploy with skip cache (full rebuild)
flyctl deploy --no-cache
```

#### Monitor Deployment

```bash
# Watch deployment progress
flyctl logs --follow

# Check app status
flyctl status

# View recent deployments
flyctl releases

# View specific deployment details
flyctl release info <release-number>
```

### Step 4: Verify Deployment

```bash
# Get app URL
flyctl info

# Test application
curl https://orochi-dashboard.fly.dev

# View logs
flyctl logs

# Check health status
flyctl checks list
```

### Step 5: Rollback if Needed

```bash
# View deployment history
flyctl releases

# Rollback to previous deployment
flyctl releases rollback <version>

# Or use shorthand
flyctl releases rollback
```

### Step 6: Update Deployment

```bash
# For code changes
git add .
git commit -m "Update dashboard"
flyctl deploy

# For environment variable changes only
flyctl secrets set VITE_API_URL=https://api.yourdomain.com
# No redeploy needed for secrets

# For infrastructure changes (fly.toml)
flyctl deploy

# For database/dependency upgrades
npm update
npm run build
flyctl deploy
```

---

## Health Checks & Monitoring

### Step 1: Configure Health Checks

The Dockerfile includes a built-in health check:

```dockerfile
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD node -e "require('http').get('http://localhost:3000', (r) => {if (r.statusCode !== 200) throw new Error(r.statusCode)})"
```

The fly.toml includes HTTP health checks:

```toml
[services.http_checks]
enabled = true
interval = "30s"
timeout = "5s"
grace_period = "5s"
method = "GET"
path = "/"
```

### Step 2: Monitor Application

#### Via Fly.io Dashboard

1. Go to https://fly.io/dashboard
2. Select your app `orochi-dashboard`
3. View metrics:
   - CPU usage
   - Memory usage
   - Request rate
   - Response time

#### Via Fly.io CLI

```bash
# View real-time logs
flyctl logs

# View logs with filters
flyctl logs --region sjc
flyctl logs --instance <instance-id>

# View metrics
flyctl metrics
```

### Step 3: Set Up Alerts

Configure notifications for deployment issues:

```bash
# View monitoring configuration
flyctl config env

# Set up log streaming to external service
# Via fly.io dashboard > Monitoring > Log Destinations
```

### Step 4: Application Monitoring

For deeper application monitoring, integrate with external services:

#### Option A: Sentry (Error Tracking)

```bash
# Install Sentry
npm install @sentry/react @sentry/vite-plugin

# Set Sentry DSN
flyctl secrets set SENTRY_DSN=https://your-sentry-dsn

# Update src/main.tsx
import * as Sentry from "@sentry/react"

Sentry.init({
  dsn: import.meta.env.VITE_SENTRY_DSN,
  environment: import.meta.env.MODE,
})
```

#### Option B: Datadog (APM)

```bash
# Install Datadog
npm install @datadog/browser-rum @datadog/browser-logs

# Set Datadog configuration
flyctl secrets set DATADOG_APPLICATION_ID=your-app-id
flyctl secrets set DATADOG_CLIENT_TOKEN=your-client-token
```

#### Option C: OpenTelemetry (Cloud-Agnostic)

```bash
# Install OpenTelemetry
npm install @opentelemetry/api @opentelemetry/sdk-node \
  @opentelemetry/auto @opentelemetry/exporter-trace-otlp-http

# Configure in application
# Details: https://opentelemetry.io/docs/instrumentation/js/
```

### Step 5: Create Custom Health Endpoint

For advanced health checks, create a dedicated endpoint:

```typescript
// src/routes/health.tsx (or similar)
export default function HealthRoute() {
  return Response.json({
    status: "ok",
    timestamp: new Date().toISOString(),
    version: "1.0.0",
    dependencies: {
      api: "checking", // Check backend connectivity
    },
  })
}
```

Update health check in fly.toml:

```toml
[services.http_checks]
path = "/api/health"
```

---

## Custom Domain Setup

### Step 1: Point Domain to Fly.io

#### For Apex Domain (example.com)

1. Get Fly nameservers:
   ```bash
   flyctl domains list
   ```

2. Update your domain registrar's nameservers to Fly.io's nameservers

Or use CNAME for www subdomain:

#### For Subdomain (dashboard.example.com)

1. Add CNAME record in your DNS provider:
   ```
   Name: dashboard
   Type: CNAME
   Value: orochi-dashboard.fly.dev
   ```

2. Update Fly app certificate:
   ```bash
   # Add domain to Fly app
   flyctl certs add dashboard.example.com

   # Verify certificate
   flyctl certs list

   # View certificate details
   flyctl certs show dashboard.example.com
   ```

### Step 2: Verify DNS Resolution

```bash
# Check DNS resolution
nslookup dashboard.example.com

# Verify SSL certificate
openssl s_client -connect dashboard.example.com:443

# Or use online tools
# https://www.nslookup.io/
# https://crt.sh/
```

### Step 3: Update Application Configuration

Update environment variables to use custom domain:

```bash
# If using custom domain, update API URL
flyctl secrets set VITE_API_URL=https://api.example.com

# Redeploy
flyctl deploy
```

### Step 4: Redirect HTTP to HTTPS

Already configured in fly.toml:

```toml
[[services.ports]]
port = 80
handlers = ["http"]
force_https = true
```

Verify redirect works:

```bash
curl -I http://dashboard.example.com
# Should return 301/302 redirect to HTTPS
```

---

## Scaling & Performance

### Step 1: Horizontal Scaling

Scale by adding more instances:

```bash
# Scale to 3 instances across regions
flyctl scale count 3

# Scale to specific region
flyctl scale count 2 --region sjc

# View current scaling
flyctl scale show
```

### Step 2: Vertical Scaling

Increase resources per instance:

```bash
# Update fly.toml [env.production]
SIZE = "shared-cpu-2x"  # Default: shared-cpu-1x
MEMORY = 512            # Default: 256

# Deploy changes
flyctl deploy

# Check available sizes
flyctl platform vm-sizes
```

### Step 3: Performance Optimization

#### Build Size Optimization

```bash
# Check bundle size
npm run build

# Analyze bundle
npm install -D rollup-plugin-visualizer

# In vite.config.ts:
import { visualizer } from 'rollup-plugin-visualizer'

plugins: [
  visualizer({
    emitFile: true,
    filename: 'dist/stats.html'
  })
]
```

#### Code Splitting

Vite automatically code-splits React Router routes. Ensure routes are lazy-loaded:

```typescript
// src/routes/dashboard.tsx
import { createFileRoute } from '@tanstack/react-router'
import { Suspense, lazy } from 'react'

const DashboardContent = lazy(() =>
  import('./dashboard-content').then(m => ({ default: m.DashboardContent }))
)

export const Route = createFileRoute('/dashboard')({
  component: () => (
    <Suspense fallback={<div>Loading...</div>}>
      <DashboardContent />
    </Suspense>
  ),
})
```

#### Caching Strategy

Configure HTTP caching headers in server:

```typescript
// dist/server/server.js or similar
app.use((req, res, next) => {
  // Static assets (CSS, JS, images)
  if (req.path.match(/\.(js|css|png|jpg|gif|ico|svg)$/)) {
    res.set('Cache-Control', 'public, max-age=31536000, immutable')
  }
  // HTML (no cache - always fetch latest)
  else if (req.path.endsWith('.html')) {
    res.set('Cache-Control', 'public, max-age=0, must-revalidate')
  }
  // Default
  else {
    res.set('Cache-Control', 'public, max-age=3600')
  }
  next()
})
```

### Step 4: Load Balancing

Fly.io automatically load-balances across instances.

Monitor load distribution:

```bash
# View instances
flyctl machines list

# Check instance metrics
flyctl machines status <machine-id>
```

### Step 5: Content Delivery Optimization

#### Use Fly's Regions

```toml
# fly.toml - Deploy to multiple regions for global coverage
[build]
dockerfile = "Dockerfile"

# Primary region (where state is maintained)
primary_region = "sjc"

# Use Fly's automatic failover and geo-routing
```

Deploy to multiple regions:

```bash
# Add regions
flyctl regions add iad  # Virginia
flyctl regions add ams  # Amsterdam

# View region configuration
flyctl regions list
```

---

## Troubleshooting

### Issue: Build Fails

#### Check Docker Build Logs

```bash
# Get detailed build logs
flyctl deploy --verbose

# Or rebuild locally
docker build -t orochi-dashboard:latest .
```

#### Common Causes

1. **Node version mismatch**: Ensure Dockerfile uses Node 18+
2. **Missing dependencies**: Run `npm ci` locally first
3. **Build errors**: Check `npm run build` locally

#### Solution

```bash
# Rebuild with cache cleared
flyctl deploy --no-cache

# Or redeploy with verbose output
flyctl deploy -v
```

### Issue: Application Won't Start

#### Check Application Logs

```bash
# View current logs
flyctl logs

# Follow logs in real-time
flyctl logs --follow

# View specific instance logs
flyctl logs --instance <instance-id>
```

#### Common Causes

1. **Port binding**: App must listen on port 3000
2. **Health check failures**: Verify endpoint responds with 200
3. **Environment variables**: Missing required VITE_* variables

#### Solution

```bash
# Verify app starts locally
npm run build
npm run start

# Check Dockerfile CMD runs correctly
docker run -p 3000:3000 orochi-dashboard:latest

# Verify port binding
flyctl ssh console
ps aux | grep node
```

### Issue: Memory/CPU Exhaustion

#### Monitor Resource Usage

```bash
# View real-time metrics
flyctl metrics

# Check instance resource usage
flyctl machines status <machine-id>
```

#### Common Causes

1. **Memory leak**: Check application code and dependencies
2. **Infinite loops**: Verify build completes locally
3. **Too much traffic**: Scale horizontally (add instances)

#### Solution

```bash
# Increase memory
# Update fly.toml:
[env.production]
MEMORY = 1024  # Increase from 512

# Or increase instances
flyctl scale count 3

# Redeploy
flyctl deploy
```

### Issue: Slow Response Times

#### Analyze Performance

```bash
# Check response times
curl -w "@curl-format.txt" https://orochi-dashboard.fly.dev

# Measure time to first byte
time curl -o /dev/null https://orochi-dashboard.fly.dev

# Check regional latency
# Use online tools: https://www.uptrends.com/tools/ping
```

#### Common Causes

1. **Large bundle**: Analyze with rollup-plugin-visualizer
2. **Network latency**: Deploy to region closer to users
3. **Backend API slow**: Check API response times

#### Solution

```bash
# Optimize bundle
npm run build  # Check output size

# Deploy closer to users
flyctl regions add <nearest-region>

# Enable gzip compression
# Add to fly.toml or server configuration
```

### Issue: SSL Certificate Issues

#### Check Certificate Status

```bash
# View certificates
flyctl certs list

# Check specific certificate
flyctl certs show dashboard.example.com

# View certificate details
openssl s_client -connect dashboard.example.com:443
```

#### Common Causes

1. **DNS not pointing to Fly**: Verify CNAME or nameservers
2. **Certificate pending**: Wait 10-30 minutes for provisioning
3. **Domain ownership**: Verify DNS records

#### Solution

```bash
# Force certificate renewal
flyctl certs remove dashboard.example.com
flyctl certs add dashboard.example.com

# Or check Let's Encrypt status
# https://crt.sh/?q=dashboard.example.com
```

### Issue: GitHub Actions Deployment Failing

#### Set Up Fly.io Token

```bash
# Generate deploy token
flyctl auth token

# Add to GitHub secrets
# Repository Settings > Secrets > New repository secret
# Name: FLY_API_TOKEN
# Value: <token from above>
```

#### GitHub Actions Workflow

```yaml
name: Deploy to Fly.io

on:
  push:
    branches: [main]

jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Deploy to Fly
        uses: superfly/flyctl-actions@master
        with:
          args: "deploy"
        env:
          FLY_API_TOKEN: ${{ secrets.FLY_API_TOKEN }}
```

---

## Performance Recommendations

### Recommended Configuration for Dashboard

Based on typical dashboard usage patterns:

```toml
# fly.toml - Recommended production configuration

[env.production]
SIZE = "shared-cpu-2x"      # Sufficient for interactive dashboard
MEMORY = 512                 # 512MB for React app

[[services]]
# Concurrency limits
[[services.concurrency]]
type = "connections"
hard_limit = 1000
soft_limit = 900

# Health checks
[services.http_checks]
interval = "30s"             # Check health every 30 seconds
timeout = "5s"              # 5 second timeout
grace_period = "5s"         # Wait 5s before checking on startup
```

### Recommended Scaling Strategy

1. **Development**: 1 shared-cpu-1x instance (256MB)
2. **Staging**: 2 shared-cpu-2x instances (512MB each)
3. **Production**: 2-3 shared-cpu-2x instances, scaled by region

Scale dynamically:

```bash
# Monitor during peak hours
flyctl metrics

# Scale if CPU > 80% consistently
flyctl scale count 3 --region sjc

# Or set up auto-scaling (via dashboard)
```

### Cost Optimization

- Use `shared-cpu-1x` for low-traffic environments
- Enable auto-suspend for non-production instances
- Use single instance for development
- Regional scaling (vs global) reduces costs

### Security Checklist

- [ ] Force HTTPS (configured in fly.toml)
- [ ] Use secrets for sensitive variables (not environment variables)
- [ ] Non-root user in Dockerfile
- [ ] Keep Node.js updated
- [ ] Scan dependencies: `npm audit`
- [ ] Enable authentication on admin endpoints
- [ ] Use custom domain with SSL certificate
- [ ] Regularly review logs for suspicious activity

---

## Advanced Configuration

### Using Fly Postgres

If you need a backend database:

```bash
# Create Postgres database
flyctl postgres create --name orochi-db

# Get connection string
flyctl postgres connect -a orochi-db

# Link to dashboard app
flyctl postgres attach orochi-db --app orochi-dashboard
```

### Using Fly Redis

For caching or sessions:

```bash
# Create Redis
flyctl redis create --name orochi-cache

# Link to app
flyctl redis attach orochi-cache --app orochi-dashboard
```

### Private Network

Connect dashboard to other Fly apps:

```bash
# Orochi dashboard and API on same private network
# Automatically available via app name as hostname

# In API config: database.example.com:5432
# In dashboard VITE_API_URL: http://orochi-api.internal:8080
```

---

## References

- [Fly.io Documentation](https://fly.io/docs/)
- [Fly.io CLI Reference](https://fly.io/docs/reference/flyctl/)
- [React 19 Deployment](https://react.dev/)
- [Vite Deployment Guide](https://vitejs.dev/guide/ssr.html)
- [TanStack Router](https://tanstack.com/router/latest)
- [Docker Best Practices](https://docs.docker.com/develop/dev-best-practices/)
- [Node.js Production Best Practices](https://nodejs.org/en/docs/guides/nodejs-performance-best-practices/)

---

## Support & Questions

- Fly.io Support: https://fly.io/docs/getting-help/
- GitHub Issues: Create issue in orochi-db repository
- Community Slack: Join Fly.io community
- Email: support@fly.io

---

## Deployment Checklist

Before deploying to production:

- [ ] All tests passing locally (`npm run lint && npm run type-check`)
- [ ] Build succeeds (`npm run build`)
- [ ] Docker image builds successfully (`docker build`)
- [ ] Environment variables configured (`flyctl secrets list`)
- [ ] Health checks configured in fly.toml
- [ ] Custom domain configured (if needed)
- [ ] SSL certificate provisioned
- [ ] Monitoring set up (logs, metrics, alerts)
- [ ] Rollback plan documented
- [ ] Database migrations completed (if applicable)
- [ ] Load testing completed for expected traffic
- [ ] Security audit completed
- [ ] Incident response plan prepared
- [ ] Team trained on deployment process

---

**Last Updated**: January 2026
**Version**: 1.0.0
**Status**: Production-Ready
