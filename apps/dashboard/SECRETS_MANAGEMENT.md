# Orochi Cloud Dashboard - Secrets Management Guide

Comprehensive guide for securely managing sensitive data and environment variables.

## Overview

Secrets are sensitive configuration values like API keys, tokens, and credentials. They must never be:
- Committed to version control
- Visible in logs
- Exposed in error messages
- Hardcoded in source code

---

## Environment Variables Types

### Build-Time Variables (Vite)

Variables needed during build process (prefixed with `VITE_`):

```bash
VITE_API_URL=https://api.example.com
VITE_SENTRY_DSN=https://xxx@sentry.io/123
```

**Storage**:
- Can be in `.env.local` during development
- Set via `--build-arg` or environment when building Docker image
- Never commit `.env.local` to git

**Usage in Code**:
```typescript
const apiUrl = import.meta.env.VITE_API_URL
const sentryDsn = import.meta.env.VITE_SENTRY_DSN
```

### Runtime Variables (Fly.io Secrets)

Variables needed when application is running (no `VITE_` prefix):

```bash
DATABASE_URL=postgresql://...
JWT_SECRET=xxx
API_KEY=yyy
```

**Storage**: Fly.io encrypted secrets
**Access**: Via `process.env` in server-side code only

---

## Secrets in Fly.io

### Step 1: Set Secrets

Set sensitive values in Fly.io:

```bash
# Set single secret
flyctl secrets set VITE_API_URL=https://api.example.com

# Set multiple secrets at once
flyctl secrets set \
  VITE_API_URL=https://api.example.com \
  JWT_SECRET=your-very-secret-key \
  DATABASE_URL=postgresql://user:pass@host/db
```

### Step 2: View Secrets

List secrets (values are masked):

```bash
flyctl secrets list
```

Output:
```
NAME           DIGEST                  CREATED AT
VITE_API_URL   2caa4a73f27d            1h ago
JWT_SECRET     8b4e1f2c9d3a            1h ago
DATABASE_URL   5f7c2e8b9a1d            2h ago
```

### Step 3: Update Secrets

Update a secret:

```bash
flyctl secrets set VITE_API_URL=https://new-api.example.com
```

The application automatically picks up new secrets without redeployment.

### Step 4: Remove Secrets

Remove a secret:

```bash
# Remove single secret
flyctl secrets unset VITE_API_URL

# Remove multiple secrets
flyctl secrets unset SECRET1 SECRET2

# Remove all secrets (careful!)
flyctl secrets unset --all
```

---

## Local Development

### Setup Local Environment

1. **Copy example file**:
   ```bash
   cp .env.example .env.local
   ```

2. **Update values** for local development:
   ```bash
   # .env.local (never commit this!)
   VITE_API_URL=http://localhost:8080
   VITE_SENTRY_DSN=
   ```

3. **Load in development**:
   ```bash
   npm run dev
   # Vite automatically loads .env.local
   ```

### Git Ignore

Ensure sensitive files are in `.gitignore`:

```bash
# .gitignore
.env
.env.local
.env.*.local
*.pem
*.key
secrets/
```

Verify these aren't tracked:

```bash
git status
# Should not show .env files
```

### Team Sharing

For sharing development secrets securely:

**Option 1: Shared 1Password/LastPass Vault**
- Store development `.env` in password manager
- Team members retrieve and use locally
- Rotate keys periodically

**Option 2: Encrypted `.env` file**
```bash
# Install tools
npm install --save-dev dotenv-vault

# Encrypt .env
npx dotenv-vault encrypt

# Commit .env.vault (never .env)
git add .env.vault

# Team members decrypt
npx dotenv-vault decrypt
```

**Option 3: GitHub Secrets (for CI/CD only)**
See [GitHub Actions Setup](#github-actions-setup)

---

## Build-Time Secrets (Docker Build)

### Method 1: Docker Build Arguments

Pass secrets at build time:

```bash
docker build \
  --build-arg VITE_API_URL=https://api.example.com \
  -t orochi-dashboard .
```

In `Dockerfile`:
```dockerfile
ARG VITE_API_URL=http://localhost:8080
ENV VITE_API_URL=$VITE_API_URL
```

### Method 2: Docker Build Secrets (Experimental)

For more sensitive values:

```dockerfile
# Enable BuildKit
# syntax=docker/dockerfile:1.4

FROM node:18-alpine as builder

# Mount secret (not copied into image)
RUN --mount=type=secret,id=npm_token \
    npm ci --registry=https://registry.npmjs.org/
```

Build:
```bash
docker build \
  --secret npm_token=/path/to/token \
  -t orochi-dashboard .
```

### Method 3: Multi-Stage Build

Keep secrets out of final image:

```dockerfile
FROM node:18-alpine as builder

ARG VITE_API_URL
RUN VITE_API_URL=$VITE_API_URL npm run build

FROM node:18-alpine
# Final image only contains built files, not secrets
COPY --from=builder /app/dist ./dist
```

---

## GitHub Actions Setup

### Step 1: Add Secrets to GitHub

1. Go to Repository Settings > Secrets and variables > Actions
2. Click "New repository secret"
3. Add each secret:

```
Name: FLY_API_TOKEN
Value: <your-fly-api-token>

Name: VITE_API_URL
Value: https://api.example.com
```

### Step 2: Use in Workflow

```yaml
name: Deploy

on: [push]

jobs:
  deploy:
    runs-on: ubuntu-latest
    environment: production
    steps:
      - uses: actions/checkout@v4

      - name: Deploy
        env:
          FLY_API_TOKEN: ${{ secrets.FLY_API_TOKEN }}
        run: flyctl deploy --build-arg VITE_API_URL=${{ secrets.VITE_API_URL }}
```

### Step 3: Environment Secrets

For per-environment secrets:

1. Create environment in Settings > Environments
2. Add secrets to each environment
3. Reference in workflow:

```yaml
jobs:
  deploy:
    environment:
      name: production
    steps:
      - name: Deploy
        env:
          API_URL: ${{ secrets.VITE_API_URL }}
```

---

## Secret Rotation

### Automatic Rotation Strategy

1. **Monthly rotation** for API keys
2. **Quarterly rotation** for long-lived tokens
3. **Immediately rotate** if compromised

### Rotation Procedure

```bash
# 1. Generate new secret
NEW_KEY=$(openssl rand -hex 32)

# 2. Set new secret in Fly.io
flyctl secrets set API_KEY=$NEW_KEY

# 3. Update application to handle both old and new
# (implement grace period in code)

# 4. Wait for deployment and monitoring
flyctl deploy
flyctl logs --follow

# 5. Remove old secret
flyctl secrets unset OLD_API_KEY

# 6. Document rotation
# Log in change management system
```

---

## Secret Leakage Prevention

### Code Review Checklist

Before merging code:

- [ ] No hardcoded secrets
  ```bash
  git diff | grep -E "password|secret|key|token"
  ```
- [ ] No credentials in error messages
- [ ] `.env` files not committed
- [ ] API keys not in logs
- [ ] Secrets marked as sensitive in config

### Automated Detection

Install secret scanning:

```bash
# GitHub: Automatically enabled
# Or use:
npm install --save-dev @secretlint/secretlint

# Scan for secrets
npx secretlint "**/*"
```

### Pre-Commit Hook

Prevent accidental commits:

```bash
# Install pre-commit hooks
npm install --save-dev husky

# Add to pre-commit hook
npx husky add .husky/pre-commit 'npx secretlint'
```

---

## Production Secret Management

### Secrets in Fly.io

Most secure for Fly.io:

```bash
# 1. Set secrets
flyctl secrets set \
  VITE_API_URL=https://api.prod.example.com \
  DATABASE_URL=postgresql://... \
  JWT_SECRET=... \
  API_KEY=...

# 2. No need to redeploy
# App picks up secrets automatically

# 3. Verify secrets are set
flyctl secrets list

# 4. Check in logs (won't show values)
flyctl logs
```

### Backup & Disaster Recovery

**Do NOT:**
- Store secrets in backups
- Email secrets
- Commit to version control

**Do:**
- Keep secrets in Fly.io only
- Document which services have which secrets
- Maintain secure backup of secret rotation logs
- Test recovery procedures quarterly

### Secrets in External Services

For external service integrations:

```bash
# 1. Create service-specific credentials
# (not your primary credentials)

# 2. Set minimum required permissions
# (principle of least privilege)

# 3. Store in Fly.io secrets
flyctl secrets set SENDGRID_API_KEY=...

# 4. Rotate quarterly
# 5. Use different key for different services
```

---

## Secret Access Audit

### View Secret Access Logs

```bash
# Fly.io logs show when secrets are set
flyctl logs | grep secrets

# GitHub Actions logs show secret usage
# (secrets are masked in logs)
```

### Access Control

Limit who can manage secrets:

**GitHub**:
1. Settings > Environments
2. Required reviewers: Enable
3. Deployment branches: Limit to main

**Fly.io**:
```bash
# Grant team member access
flyctl orgs invite team@example.com

# Remove access
flyctl orgs remove-member user@example.com
```

---

## Common Secret Types

### API Keys

```bash
# Store in Fly.io
flyctl secrets set OPENAI_API_KEY=sk-...

# Use in code
const apiKey = process.env.OPENAI_API_KEY
```

### Database URLs

```bash
# PostgreSQL
flyctl secrets set DATABASE_URL=postgresql://user:pass@host:5432/db

# MongoDB
flyctl secrets set MONGODB_URI=mongodb+srv://user:pass@cluster.mongodb.net/db
```

### JWT Secrets

```bash
# Generate secret
openssl rand -hex 32

# Set in Fly.io
flyctl secrets set JWT_SECRET=abcd1234...

# Use for signing tokens
import jwt from 'jsonwebtoken'
const token = jwt.sign(payload, process.env.JWT_SECRET)
```

### OAuth Credentials

```bash
# GitHub OAuth
flyctl secrets set GITHUB_CLIENT_ID=...
flyctl secrets set GITHUB_CLIENT_SECRET=...

# Google OAuth
flyctl secrets set GOOGLE_CLIENT_ID=...
flyctl secrets set GOOGLE_CLIENT_SECRET=...
```

### Certificates & Keys

For `.pem` or `.key` files:

```bash
# Read file content
cat path/to/private.key

# Set as secret (replace newlines)
flyctl secrets set PRIVATE_KEY="-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQE...
-----END PRIVATE KEY-----"

# Or use base64
cat path/to/private.key | base64 | tr -d '\n'
flyctl secrets set PRIVATE_KEY_B64=...
```

---

## Troubleshooting

### Secret Not Available at Runtime

```bash
# 1. Verify secret is set
flyctl secrets list | grep SECRET_NAME

# 2. Check environment variable name
# Must not have VITE_ prefix for runtime secrets

# 3. Verify deployment picked up secret
flyctl logs | grep SECRET_NAME

# 4. Redeploy if needed
flyctl deploy
```

### Secret Visible in Logs

```bash
# 1. Check code for logging secrets
grep -r "process.env" src/

# 2. Never log secret values
// Bad
console.log('API Key:', process.env.API_KEY)

// Good
console.log('API configured')

# 3. Use secret masking
// Mask in logs
const masked = `${secret.substring(0, 4)}****${secret.substring(-4)}`
console.log('Using secret:', masked)
```

### Secret Rotation Issues

```bash
# 1. Verify new secret is set
flyctl secrets list

# 2. Check application is using correct secret
flyctl logs --follow

# 3. Verify dependent systems accept new secret
# (test API connection, etc.)

# 4. Only then remove old secret
flyctl secrets unset OLD_SECRET
```

---

## Security Best Practices

### General Rules

1. **Never commit secrets to git**
2. **Never hardcode secrets in code**
3. **Never share secrets via email or chat**
4. **Rotate secrets regularly** (monthly minimum)
5. **Use unique secrets per environment**
6. **Monitor secret access logs**
7. **Audit secret permissions quarterly**
8. **Document secret purposes**
9. **Test secret rotation procedures**
10. **Have incident response plan**

### Code Review

Checklist for every code review:

- [ ] No hardcoded credentials
- [ ] No secrets in config files
- [ ] Error messages don't expose secrets
- [ ] Logging doesn't include secrets
- [ ] Environment variables used correctly
- [ ] External API calls use secrets properly

### Deployment Verification

Before deployment:

```bash
# 1. Verify secrets are set
flyctl secrets list

# 2. Check Dockerfile doesn't expose secrets
grep -v "ENV.*SECRET" Dockerfile

# 3. Verify .env not committed
git ls-files | grep -E "\.env|\.key|\.pem"

# 4. Check logs for exposed values
npm run build 2>&1 | grep -i "secret\|key\|token\|password"
```

---

## References

- [Fly.io Secrets Documentation](https://fly.io/docs/reference/secrets/)
- [OWASP Secret Management](https://owasp.org/)
- [12-Factor App - Config](https://12factor.net/config)
- [GitHub Actions Secrets](https://docs.github.com/en/actions/security-guides/encrypted-secrets)
- [Node.js Security Best Practices](https://nodejs.org/en/docs/guides/security/)

---

**Last Updated**: January 2026
**Version**: 1.0.0
**Status**: Production-Ready
