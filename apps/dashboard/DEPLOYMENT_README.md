# Orochi Cloud Dashboard - Fly.io Deployment Documentation

Complete production-ready deployment guide for the Orochi Cloud Dashboard on Fly.io.

## Documentation Structure

This deployment package includes comprehensive documentation for deploying the React 19 dashboard to Fly.io:

### Core Documentation

1. **[DEPLOYMENT.md](./DEPLOYMENT.md)** - Full Deployment Guide
   - Complete step-by-step deployment instructions
   - Dockerfile configuration and optimization
   - fly.toml configuration with all options
   - Environment variables and secrets management
   - Health checks and monitoring setup
   - Custom domain configuration
   - Scaling and performance tuning
   - Comprehensive troubleshooting guide
   - ~6,000 words, production-ready

2. **[FLY_QUICKSTART.md](./FLY_QUICKSTART.md)** - Quick Start Guide
   - Get to production in 10 minutes
   - Quick reference for common commands
   - Rapid deployment steps
   - Essential troubleshooting
   - Perfect for first-time Fly.io deployment

3. **[DEPLOYMENT_CHECKLIST.md](./DEPLOYMENT_CHECKLIST.md)** - Pre/Post Deployment Checklist
   - Pre-deployment code quality checks
   - Infrastructure verification
   - Deployment procedure validation
   - Post-deployment monitoring
   - Rollback procedures
   - Emergency contact information
   - Success criteria

4. **[SECRETS_MANAGEMENT.md](./SECRETS_MANAGEMENT.md)** - Secrets & Environment Variables
   - Local development setup
   - Fly.io secrets management
   - GitHub Actions secrets
   - Docker build secrets
   - Secret rotation procedures
   - Common secret types and examples
   - Security best practices
   - Troubleshooting secret issues

### Configuration Files

1. **[Dockerfile](./Dockerfile)** - Production Docker Image
   - Multi-stage build (builder + runtime)
   - Alpine Linux base (minimal size)
   - Security hardening (non-root user, dumb-init)
   - Health checks configured
   - Layer caching optimization
   - Ready to deploy immediately

2. **[fly.toml](./fly.toml)** - Fly.io Configuration
   - App settings and regions
   - Service configuration
   - HTTP/HTTPS setup with auto-redirect
   - Health check configuration
   - VM sizing and memory allocation
   - Process definitions
   - Concurrency limits
   - Production-optimized defaults

3. **[.env.example](./.env.example)** - Environment Variable Template
   - All required variables documented
   - Example values for each environment
   - Comments explaining each variable
   - Copy to .env and customize for your setup

### Supporting Files

1. **[src/server.ts.example](./src/server.ts.example)** - Example Server Configuration
   - Express.js setup for production
   - Security headers (Helmet.js)
   - Compression and gzip
   - Rate limiting
   - Error handling
   - Graceful shutdown
   - Health endpoints

2. **[.github/workflows/deploy-dashboard-fly.yml](../.github/workflows/deploy-dashboard-fly.yml)**
   - GitHub Actions CI/CD workflow
   - Automated deployment on push to main/develop
   - Build verification and testing
   - Post-deployment health checks
   - Automatic rollback on failure
   - Deployment notifications

---

## Quick Navigation

### I want to...

**Deploy to Fly.io for the first time**
1. Read: [FLY_QUICKSTART.md](./FLY_QUICKSTART.md) (10 min)
2. Follow: Steps 1-5 in that guide
3. Reference: [DEPLOYMENT_CHECKLIST.md](./DEPLOYMENT_CHECKLIST.md)

**Set up production environment**
1. Read: [DEPLOYMENT.md](./DEPLOYMENT.md) - Sections 1-5
2. Configure: Environment variables and secrets
3. Reference: [SECRETS_MANAGEMENT.md](./SECRETS_MANAGEMENT.md)
4. Verify: All checks in [DEPLOYMENT_CHECKLIST.md](./DEPLOYMENT_CHECKLIST.md)

**Configure custom domain**
1. Read: [DEPLOYMENT.md](./DEPLOYMENT.md) - Section 8
2. Update: DNS records
3. Verify: SSL certificate provisioning

**Set up monitoring and alerts**
1. Read: [DEPLOYMENT.md](./DEPLOYMENT.md) - Section 7
2. Configure: Health checks
3. Integrate: Error tracking and APM services

**Scale application for more traffic**
1. Read: [DEPLOYMENT.md](./DEPLOYMENT.md) - Section 9
2. Monitor: Current metrics with `flyctl metrics`
3. Scale: Horizontally (add instances) or vertically (increase resources)

**Debug deployment issues**
1. Check: [DEPLOYMENT.md](./DEPLOYMENT.md) - Section 10 (Troubleshooting)
2. Run: `flyctl logs --follow`
3. Follow: Procedures for your specific issue

**Manage secrets safely**
1. Read: [SECRETS_MANAGEMENT.md](./SECRETS_MANAGEMENT.md)
2. Set: Secrets in Fly.io: `flyctl secrets set KEY=value`
3. Verify: `flyctl secrets list`

**Prepare for production deployment**
1. Review: [DEPLOYMENT_CHECKLIST.md](./DEPLOYMENT_CHECKLIST.md)
2. Complete: All pre-deployment checks
3. Execute: Deployment procedure
4. Verify: Post-deployment verification steps

**Set up automated CI/CD**
1. File: `.github/workflows/deploy-dashboard-fly.yml` (already included)
2. Configure: GitHub Secrets (FLY_API_TOKEN, VITE_API_URL)
3. Enable: Actions in repository settings
4. Deploy: Automatically on push to main

---

## File Sizes and Complexity

| File | Size | Complexity | Time to Read |
|------|------|-----------|--------------|
| FLY_QUICKSTART.md | 2.2K | Low | 5 min |
| DEPLOYMENT.md | 25K | High | 30 min |
| DEPLOYMENT_CHECKLIST.md | 9.5K | Medium | 15 min |
| SECRETS_MANAGEMENT.md | 12K | Medium | 20 min |
| Dockerfile | 2.2K | Low | 5 min |
| fly.toml | 3.7K | Low | 10 min |
| Total Documentation | ~54K | Varies | 85 min |

---

## Key Features

### Security
- Non-root Docker user
- Secrets encrypted in Fly.io
- HTTPS enforced (HTTP redirects)
- Content Security Policy headers
- No hardcoded credentials
- Regular rotation procedures

### Performance
- Multi-stage Docker build (optimized image size)
- Alpine Linux base (~150MB final image)
- Gzip compression configured
- Static asset caching
- Code splitting for React Router
- Health checks for instant restart

### Reliability
- Automatic health checks
- Graceful shutdown handling
- Automatic instance restart on crash
- Deployment rollback procedures
- Monitoring and alerting
- Post-deployment verification

### Operations
- One-command deployment: `flyctl deploy`
- Real-time logs: `flyctl logs --follow`
- Horizontal scaling: `flyctl scale count 3`
- Zero-downtime updates
- Automatic SSL certificate renewal
- Regional failover available

---

## Deployment Workflow

### Local Development
```bash
npm install              # Install dependencies
cp .env.example .env.local # Create local config
npm run dev             # Start development server
```

### Before Deployment
```bash
npm run type-check      # Type checking
npm run lint            # Linting
npm run build           # Build Vite app
docker build .          # Test Docker build
```

### Deploy to Staging
```bash
git checkout develop
flyctl deploy --app orochi-dashboard-staging
```

### Deploy to Production
```bash
git checkout main
flyctl deploy --app orochi-dashboard
```

### Post-Deployment
```bash
flyctl status           # Check status
flyctl logs --follow    # Monitor logs
flyctl metrics          # View metrics
flyctl checks list      # Verify health
```

---

## Directory Structure

```
orochi-cloud/dashboard/
├── DEPLOYMENT.md                    # Full deployment guide
├── FLY_QUICKSTART.md               # Quick start (10 min)
├── DEPLOYMENT_CHECKLIST.md         # Pre/post checks
├── SECRETS_MANAGEMENT.md           # Secrets guide
├── DEPLOYMENT_README.md            # This file
├── Dockerfile                      # Production Docker image
├── fly.toml                        # Fly.io configuration
├── .env.example                    # Environment template
├── src/
│   ├── server.ts.example          # Example server config
│   └── ...
├── package.json                    # Dependencies
├── tsconfig.json                   # TypeScript config
├── vite.config.ts                 # Vite build config
└── ...

.github/
└── workflows/
    └── deploy-dashboard-fly.yml   # GitHub Actions workflow
```

---

## Prerequisites Checklist

Before starting deployment:

- [ ] Fly.io account created (https://fly.io)
- [ ] Fly CLI installed and authenticated
  ```bash
  brew install flyctl && flyctl auth login
  ```
- [ ] GitHub account (for CI/CD)
- [ ] Domain name (optional, for custom domain)
- [ ] Backend API URL (for VITE_API_URL)
- [ ] All dependencies installed
  ```bash
  npm ci
  ```

---

## Environment Variables

### Required (Build-Time)
- `VITE_API_URL`: Backend API endpoint (e.g., `https://api.example.com`)

### Optional (Build-Time)
- `VITE_SENTRY_DSN`: Sentry error tracking
- `VITE_DATADOG_APPLICATION_ID`: Datadog APM
- `VITE_FEATURE_FLAGS_URL`: Feature flags service

### Runtime (Fly.io)
Set via `flyctl secrets set`:
```bash
flyctl secrets set VITE_API_URL=https://api.example.com
flyctl secrets set DATABASE_URL=postgresql://...  # If needed
flyctl secrets set JWT_SECRET=...                 # If needed
```

---

## Common Commands Reference

```bash
# Initial Setup
flyctl launch                           # Create app
flyctl secrets set KEY=value            # Set secrets

# Deployment
flyctl deploy                           # Deploy application
flyctl deploy --no-cache                # Force rebuild

# Monitoring
flyctl logs                             # View recent logs
flyctl logs --follow                    # Stream logs
flyctl metrics                          # View metrics
flyctl status                           # App status

# Scaling
flyctl scale count 3                    # Add instances
flyctl scale count 1 --region sjc       # Scale region

# Troubleshooting
flyctl ssh console                      # SSH into instance
flyctl releases                         # Deployment history
flyctl releases rollback                # Rollback deployment
flyctl checks list                      # Health status

# Management
flyctl secrets list                     # View secrets
flyctl secrets unset KEY                # Remove secret
flyctl certs list                       # View certificates
flyctl certs add domain.com             # Add domain
```

---

## Deployment Timeline

### Initial Setup (First Time)
- Prerequisites: 15 minutes
- Reading documentation: 30 minutes
- Fly.io account & CLI: 5 minutes
- Deployment: 5 minutes
- Verification: 5 minutes
- **Total: ~60 minutes**

### Subsequent Deployments
- Code changes: varies
- Build: 1-2 minutes
- Deploy: 1-2 minutes
- Verification: 1-2 minutes
- **Total: ~5-10 minutes**

---

## Cost Estimate

### Minimum (Development/Staging)
- 1x shared-cpu-1x: ~$5/month
- Data transfer: ~$0.15/GB
- **Total: ~$5-10/month**

### Production (Recommended)
- 2-3x shared-cpu-2x: ~$15-22/month
- Data transfer: ~$1-5/month
- Domain (optional): ~$10/year
- **Total: ~$15-30/month**

See [Fly.io Pricing](https://fly.io/docs/about/pricing/) for current rates.

---

## Support & Resources

### Official Documentation
- [Fly.io Docs](https://fly.io/docs/)
- [Fly CLI Reference](https://fly.io/docs/reference/flyctl/)
- [React Documentation](https://react.dev/)
- [Vite Documentation](https://vitejs.dev/)

### This Project
- Report issues: GitHub Issues
- Ask questions: GitHub Discussions
- View related: Orochi DB repository

### Community
- Fly.io Slack Community
- Fly.io Discord
- Stack Overflow tags: `fly.io`, `react`, `vite`

---

## Next Steps

1. **Start here**: Read [FLY_QUICKSTART.md](./FLY_QUICKSTART.md)
2. **Then read**: [DEPLOYMENT.md](./DEPLOYMENT.md) for comprehensive guide
3. **Before deploying**: Complete [DEPLOYMENT_CHECKLIST.md](./DEPLOYMENT_CHECKLIST.md)
4. **For secrets**: Follow [SECRETS_MANAGEMENT.md](./SECRETS_MANAGEMENT.md)
5. **Deploy**: Use `flyctl deploy`
6. **Monitor**: Watch `flyctl logs --follow`

---

## FAQ

**Q: How much does Fly.io cost?**
A: Starting at ~$5/month for shared CPU. See Fly.io pricing page.

**Q: Can I use a custom domain?**
A: Yes! See DEPLOYMENT.md Section 8 for setup instructions.

**Q: How do I rollback a deployment?**
A: Use `flyctl releases rollback` to revert to previous version.

**Q: Can I scale horizontally?**
A: Yes! Use `flyctl scale count 3` to add instances.

**Q: Where do I store secrets?**
A: Use Fly.io secrets: `flyctl secrets set KEY=value`

**Q: How do I automate deployments?**
A: Use the GitHub Actions workflow included in `.github/workflows/`

**Q: What if deployment fails?**
A: Check logs with `flyctl logs --follow` and see troubleshooting section.

**Q: Can I use a database?**
A: Yes! Fly.io offers Postgres and Redis. See DEPLOYMENT.md advanced section.

---

## Change Log

### Version 1.0.0 (January 2026)
- Initial comprehensive deployment documentation
- Dockerfile with production optimizations
- fly.toml with recommended settings
- GitHub Actions CI/CD workflow
- Secrets management guide
- Deployment checklist
- Quick start guide

---

## Feedback & Improvements

This documentation aims to be complete and production-ready. If you find:
- Missing information
- Unclear instructions
- Outdated content
- Broken links

Please report issues or submit PRs to improve this guide.

---

**Created**: January 2026
**Version**: 1.0.0
**Status**: Production-Ready
**Maintenance**: Actively maintained
**Last Updated**: January 2026
