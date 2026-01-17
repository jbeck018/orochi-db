# Orochi Cloud Dashboard - Deployment Checklist

Use this checklist before deploying to production on Fly.io.

## Pre-Deployment (Development)

### Code Quality
- [ ] All tests passing
  ```bash
  npm run type-check
  npm run lint
  npm run build
  ```
- [ ] No console errors in development
- [ ] No memory leaks detected (Chrome DevTools)
- [ ] All TypeScript errors resolved
- [ ] ESLint warnings addressed
- [ ] Dead code removed
- [ ] Dependencies up to date
  ```bash
  npm audit
  npm outdated
  ```

### Performance
- [ ] Bundle size acceptable
  ```bash
  npm run build
  # Check dist/ size
  du -sh dist/
  ```
- [ ] Page load time < 3 seconds
- [ ] Interactive content within 5 seconds
- [ ] No layout shifts or visual instability
- [ ] Images optimized (WebP where applicable)
- [ ] Code splitting configured
- [ ] Lazy loading implemented for routes
- [ ] CSS is minified
- [ ] JavaScript is minified

### Security
- [ ] No hardcoded secrets in code
- [ ] No API keys in environment variables file
- [ ] Content Security Policy headers configured
- [ ] CORS properly configured
- [ ] Input validation implemented
- [ ] XSS protection enabled
- [ ] CSRF protection if needed
- [ ] Dependency security audit passed
  ```bash
  npm audit
  ```
- [ ] No vulnerable dependencies
- [ ] Sensitive operations authenticated

### Documentation
- [ ] README.md updated
- [ ] DEPLOYMENT.md reviewed
- [ ] Environment variables documented
- [ ] API integration documented
- [ ] Known limitations documented
- [ ] Troubleshooting guide present
- [ ] Runbook for common issues created

---

## Pre-Deployment (Infrastructure)

### Fly.io Setup
- [ ] Fly.io account created
- [ ] Fly CLI installed and authenticated
  ```bash
  flyctl auth login
  flyctl whoami
  ```
- [ ] Project configured with fly.toml
- [ ] Dockerfile created and tested
  ```bash
  docker build -t orochi-dashboard .
  docker run -p 3000:3000 orochi-dashboard
  ```
- [ ] Docker image builds successfully
- [ ] No Docker build warnings

### Environment Configuration
- [ ] Environment variables defined
  - [ ] VITE_API_URL set
  - [ ] All required secrets identified
- [ ] Secrets securely configured
  ```bash
  flyctl secrets set VITE_API_URL=https://api.example.com
  flyctl secrets list
  ```
- [ ] .env.example file updated with all variables
- [ ] No sensitive data in fly.toml
- [ ] Region selected appropriate for user base
- [ ] VM size appropriate for expected load

### Health Checks
- [ ] Health check endpoint configured
- [ ] Health check path returns 200 OK
- [ ] Health check timeout appropriate
- [ ] Readiness probe configured
- [ ] Liveness probe configured

### Monitoring & Logging
- [ ] Logging configured
- [ ] Log retention policy set
- [ ] Alerts configured for:
  - [ ] High CPU usage (>80%)
  - [ ] High memory usage (>90%)
  - [ ] Application crashes
  - [ ] HTTP 5xx errors
- [ ] Error tracking service configured (Sentry/Datadog)
- [ ] Performance monitoring configured

---

## Deployment (Initial)

### Pre-Flight Checks
- [ ] Working directory clean
  ```bash
  git status
  ```
- [ ] All changes committed
- [ ] Latest code pulled
- [ ] Dependencies fresh
  ```bash
  npm ci
  ```
- [ ] Build verified locally
  ```bash
  npm run build
  npm run start
  ```
- [ ] Health endpoint responds
  ```bash
  curl http://localhost:3000
  ```

### Create Fly App
- [ ] App name globally unique
- [ ] Primary region selected
- [ ] fly.toml reviewed
  ```bash
  flyctl launch
  ```

### Initial Deployment
- [ ] No uncommitted changes
- [ ] Deployment command ready
  ```bash
  flyctl deploy
  ```
- [ ] Monitoring logs opened in second terminal
  ```bash
  flyctl logs --follow
  ```
- [ ] Deployment progress monitored
- [ ] No build errors
- [ ] No startup errors

### Post-Deployment Verification
- [ ] Application is running
  ```bash
  flyctl status
  ```
- [ ] Health checks passing
  ```bash
  flyctl checks list
  ```
- [ ] Application is accessible
  ```bash
  curl https://orochi-dashboard.fly.dev
  ```
- [ ] All pages load correctly
- [ ] API integration working
- [ ] No 5xx errors in logs
- [ ] No errors in browser console

---

## Deployment (Custom Domain)

### DNS Configuration
- [ ] Domain registered
- [ ] Domain ownership verified
- [ ] DNS nameservers changed (if using Fly DNS)
  OR
- [ ] CNAME record created pointing to Fly.dev domain
- [ ] DNS propagation verified
  ```bash
  nslookup dashboard.example.com
  dig dashboard.example.com
  ```

### SSL Certificate
- [ ] Certificate requested in Fly
  ```bash
  flyctl certs add dashboard.example.com
  ```
- [ ] Certificate status verified
  ```bash
  flyctl certs list
  ```
- [ ] Certificate issued (may take 1-10 minutes)
- [ ] HTTPS accessible
  ```bash
  curl https://dashboard.example.com
  ```
- [ ] Certificate valid and not expired
  ```bash
  openssl s_client -connect dashboard.example.com:443
  ```
- [ ] HTTP redirects to HTTPS
  ```bash
  curl -I http://dashboard.example.com
  ```

---

## Post-Deployment (24 Hours)

### Monitor Application
- [ ] Check error rates
  ```bash
  flyctl logs
  ```
- [ ] Monitor performance metrics
  ```bash
  flyctl metrics
  ```
- [ ] CPU usage normal (<30% average)
- [ ] Memory usage stable
- [ ] Response times acceptable (<500ms p95)
- [ ] No memory leaks
- [ ] No unusual error patterns

### User Testing
- [ ] User acceptance testing completed
- [ ] No reported issues from testers
- [ ] All features working as expected
- [ ] Performance acceptable to users
- [ ] Mobile experience verified
- [ ] Cross-browser testing passed

### Analytics (if applicable)
- [ ] Page views tracked correctly
- [ ] User actions tracked
- [ ] Error tracking working
- [ ] Performance metrics collected

---

## Ongoing Maintenance

### Weekly
- [ ] Review error logs
- [ ] Check performance metrics
- [ ] Verify backups (if applicable)
- [ ] Monitor disk usage

### Monthly
- [ ] Update dependencies
  ```bash
  npm update
  npm audit fix
  ```
- [ ] Review security advisories
- [ ] Check for deprecated libraries
- [ ] Review and optimize performance
- [ ] Check SSL certificate expiration
- [ ] Review cost/usage metrics

### Quarterly
- [ ] Major dependency updates
- [ ] Security audit
- [ ] Performance optimization review
- [ ] Capacity planning
- [ ] Disaster recovery drill

---

## Rollback Procedures

### Scenario: Deployment Issues (First 10 Minutes)

```bash
# Stop deployment (if still running)
# Check logs immediately
flyctl logs

# View recent deployments
flyctl releases

# Rollback to previous version
flyctl releases rollback

# Verify rollback
flyctl status
curl https://orochi-dashboard.fly.dev
```

### Scenario: Health Check Failures

```bash
# View health check details
flyctl checks list

# View application logs
flyctl logs --follow

# Check if health endpoint is responding
curl https://orochi-dashboard.fly.dev/

# If needed, rollback
flyctl releases rollback
```

### Scenario: Performance Degradation

```bash
# Check metrics
flyctl metrics

# If CPU/Memory high:
# Option 1: Scale horizontally
flyctl scale count 3

# Option 2: Scale vertically
# Edit fly.toml, increase SIZE/MEMORY
flyctl deploy

# Monitor improvement
flyctl metrics
```

### Scenario: Production Bug Found

```bash
# Immediate actions:
# 1. Acknowledge issue
# 2. Determine scope and severity
# 3. Notify team

# Decision: Fix or rollback?
# If quick fix available:
git checkout -b hotfix/issue-name
# Make fix
npm run build
flyctl deploy

# If rollback needed:
flyctl releases rollback

# Post-incident:
# 1. Root cause analysis
# 2. Fix implementation
# 3. Prevent recurrence
# 4. Update runbooks
```

---

## Monitoring Dashboard Setup

### Create Metrics Dashboard

Monitor these key metrics:

1. **Application Health**
   - Request rate
   - Error rate (4xx, 5xx)
   - Response time (p50, p95, p99)
   - Uptime percentage

2. **Infrastructure**
   - CPU usage
   - Memory usage
   - Disk I/O
   - Network I/O

3. **Business**
   - User activity
   - Feature usage
   - Conversion rates (if applicable)
   - User retention

### Set Up Alerts

```bash
# Alert when error rate > 5%
# Alert when response time p95 > 1 second
# Alert when CPU > 80%
# Alert when memory > 90%
# Alert when application down > 1 minute
```

---

## Communication Plan

### Before Deployment
- [ ] Team notified of deployment window
- [ ] Stakeholders informed of changes
- [ ] On-call engineer assigned
- [ ] Rollback procedures documented

### During Deployment
- [ ] Real-time updates in team chat
- [ ] Status page updated (if public)
- [ ] Issues escalated immediately

### After Deployment
- [ ] Success announced
- [ ] Metrics summary shared
- [ ] Lessons learned documented (if issues)

---

## Success Criteria

Deployment is successful when:

- [x] Application is running without errors
- [x] Health checks passing
- [x] All features working correctly
- [x] Performance metrics within acceptable range
- [x] No increase in error rate vs previous version
- [x] SSL certificate valid
- [x] Monitoring alerts configured
- [x] Team trained on new deployment process
- [x] Documentation updated
- [x] Stakeholders notified

---

## Emergency Contacts

| Role | Name | Contact |
|------|------|---------|
| DevOps Lead | | |
| Team Lead | | |
| Backend Lead | | |
| Frontend Lead | | |
| Fly.io Support | | https://fly.io/docs/getting-help/ |

---

## Additional Resources

- [DEPLOYMENT.md](./DEPLOYMENT.md) - Full deployment guide
- [FLY_QUICKSTART.md](./FLY_QUICKSTART.md) - Quick reference
- [Fly.io Documentation](https://fly.io/docs/)
- [React Best Practices](https://react.dev/)
- [Security Checklist](https://owasp.org/)

---

**Version**: 1.0.0
**Last Updated**: January 2026
**Next Review**: April 2026
