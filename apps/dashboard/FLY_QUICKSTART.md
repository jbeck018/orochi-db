# Orochi Cloud Dashboard - Fly.io Quick Start Guide

Fast-track deployment to production on Fly.io in 10 minutes.

## Prerequisites

```bash
# 1. Install Fly CLI
brew install flyctl  # macOS
# Or see: https://fly.io/docs/getting-started/installing-flyctl/

# 2. Create Fly account
flyctl auth login

# 3. Verify authentication
flyctl whoami
```

## Deployment Steps

### Step 1: Configure Application (2 min)

```bash
# Navigate to dashboard directory
cd orochi-cloud/dashboard

# Ensure all deployment files are present
ls -la Dockerfile fly.toml DEPLOYMENT.md
```

### Step 2: Create Fly App (1 min)

```bash
# Create app (one-time setup)
flyctl launch

# When prompted:
# - App name: orochi-dashboard (or your choice)
# - Copy configuration to fly.toml: yes
# - Existing fly.toml: yes (use our pre-configured one)
# - Deploy now: no (we'll do it next)
```

### Step 3: Set Environment Variables (1 min)

```bash
# Set backend API URL (required)
flyctl secrets set VITE_API_URL=https://api.yourdomain.com

# Verify secrets
flyctl secrets list
```

### Step 4: Deploy (3 min)

```bash
# Deploy application
flyctl deploy

# Monitor deployment progress
flyctl logs --follow
```

### Step 5: Verify Deployment (2 min)

```bash
# Get application URL
flyctl info

# Test the application
curl https://orochi-dashboard.fly.dev

# View status
flyctl status
```

## Set Up Custom Domain (Optional)

```bash
# Add domain
flyctl certs add dashboard.example.com

# Verify certificate
flyctl certs list

# Wait for SSL certificate to be issued (1-10 minutes)
```

## Common Commands

### View Logs

```bash
# Real-time logs
flyctl logs --follow

# Logs for specific instance
flyctl logs --instance <instance-id>

# Last 100 lines
flyctl logs --lines 100
```

### Update Application

```bash
# After code changes
git add .
git commit -m "Update dashboard"
flyctl deploy

# Update API URL only (no redeploy)
flyctl secrets set VITE_API_URL=https://new-api.example.com
```

### Scale Application

```bash
# Add instances
flyctl scale count 3

# View instances
flyctl machines list

# Check metrics
flyctl metrics
```

### Rollback Deployment

```bash
# View releases
flyctl releases

# Rollback to previous version
flyctl releases rollback

# Or specific version
flyctl releases rollback <version-number>
```

## Troubleshooting

### Application won't start?

```bash
# Check logs
flyctl logs

# Verify build locally
npm run build
npm run start

# Rebuild without cache
flyctl deploy --no-cache
```

### Slow performance?

```bash
# Check metrics
flyctl metrics

# Increase resources
# Edit fly.toml: SIZE = "shared-cpu-2x" -> "dedicated-cpu-1x"
flyctl deploy

# Or add instances
flyctl scale count 3
```

### Health checks failing?

```bash
# Check health
flyctl checks list

# Review logs
flyctl logs --follow

# Verify app responds on port 3000
# Should return 200 OK on GET /
```

## Environment Variables Reference

| Variable | Required | Example |
|----------|----------|---------|
| `VITE_API_URL` | Yes | `https://api.example.com` |
| `VITE_SENTRY_DSN` | No | `https://xxx@sentry.io/123` |
| `NODE_ENV` | Auto | `production` |

## Next Steps

1. **Configure monitoring**: See DEPLOYMENT.md > Health Checks & Monitoring
2. **Set up alerts**: Configure notifications in Fly dashboard
3. **Custom domain**: Add DNS records pointing to Fly.io
4. **CI/CD**: Set up GitHub Actions auto-deployment
5. **Scaling**: Monitor metrics and scale based on load

## Full Documentation

See [DEPLOYMENT.md](./DEPLOYMENT.md) for comprehensive guide covering:
- Dockerfile configuration
- fly.toml settings
- Secrets management
- Custom domains
- Monitoring & alerting
- Scaling strategies
- Troubleshooting

## Need Help?

```bash
# Fly.io documentation
flyctl help

# Check app status
flyctl status

# SSH into instance
flyctl ssh console

# Support
# https://fly.io/docs/getting-help/
```

---

**Time to Production**: ~10 minutes
**Cost**: ~$5/month (shared CPU) + data transfer
**Support**: 24/7 Fly.io support included
