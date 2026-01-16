# Security Implementation Guide

## Document Information

| Field | Value |
|-------|-------|
| Version | 1.0.0 |
| Status | Design |
| Author | Security Architecture Team |
| Last Updated | 2026-01-16 |

---

## 1. Overview

This guide provides implementation details for securing the Orochi DB Managed Service multi-tenancy architecture. It covers authentication, authorization, encryption, network security, and operational security controls.

---

## 2. Authentication Security

### 2.1 Password Security

#### Password Hashing

**CRITICAL**: Never use SHA-256 or other fast hashes for password storage. Always use bcrypt with appropriate cost factors.

```python
# Python implementation using bcrypt
import bcrypt

BCRYPT_COST_FACTOR = 12  # Recommended minimum

def hash_password(plain_password: str) -> str:
    """Hash password using bcrypt with configurable cost factor."""
    salt = bcrypt.gensalt(rounds=BCRYPT_COST_FACTOR)
    hashed = bcrypt.hashpw(plain_password.encode('utf-8'), salt)
    return hashed.decode('utf-8')

def verify_password(plain_password: str, hashed_password: str) -> bool:
    """Verify password against stored hash."""
    return bcrypt.checkpw(
        plain_password.encode('utf-8'),
        hashed_password.encode('utf-8')
    )
```

```typescript
// TypeScript implementation
import * as bcrypt from 'bcrypt';

const BCRYPT_COST_FACTOR = 12;

async function hashPassword(plainPassword: string): Promise<string> {
  return bcrypt.hash(plainPassword, BCRYPT_COST_FACTOR);
}

async function verifyPassword(plainPassword: string, hashedPassword: string): Promise<boolean> {
  return bcrypt.compare(plainPassword, hashedPassword);
}
```

#### Password Policy Enforcement

```typescript
interface PasswordPolicy {
  minLength: number;
  requireUppercase: boolean;
  requireLowercase: boolean;
  requireNumbers: boolean;
  requireSpecialChars: boolean;
  maxConsecutiveChars: number;
  preventCommonPasswords: boolean;
  preventUserInfoInPassword: boolean;
  historyCount: number;  // Prevent reuse of last N passwords
}

const StandardPolicy: PasswordPolicy = {
  minLength: 12,
  requireUppercase: true,
  requireLowercase: true,
  requireNumbers: true,
  requireSpecialChars: true,
  maxConsecutiveChars: 3,
  preventCommonPasswords: true,
  preventUserInfoInPassword: true,
  historyCount: 5
};

const EnterprisePolicy: PasswordPolicy = {
  minLength: 16,
  requireUppercase: true,
  requireLowercase: true,
  requireNumbers: true,
  requireSpecialChars: true,
  maxConsecutiveChars: 2,
  preventCommonPasswords: true,
  preventUserInfoInPassword: true,
  historyCount: 12
};

function validatePassword(
  password: string,
  policy: PasswordPolicy,
  userInfo?: { email: string; displayName: string }
): { valid: boolean; errors: string[] } {
  const errors: string[] = [];

  if (password.length < policy.minLength) {
    errors.push(`Password must be at least ${policy.minLength} characters`);
  }

  if (policy.requireUppercase && !/[A-Z]/.test(password)) {
    errors.push('Password must contain at least one uppercase letter');
  }

  if (policy.requireLowercase && !/[a-z]/.test(password)) {
    errors.push('Password must contain at least one lowercase letter');
  }

  if (policy.requireNumbers && !/[0-9]/.test(password)) {
    errors.push('Password must contain at least one number');
  }

  if (policy.requireSpecialChars && !/[!@#$%^&*(),.?":{}|<>]/.test(password)) {
    errors.push('Password must contain at least one special character');
  }

  // Check consecutive characters
  const consecutivePattern = new RegExp(`(.)\\1{${policy.maxConsecutiveChars},}`);
  if (consecutivePattern.test(password)) {
    errors.push(`Password cannot contain more than ${policy.maxConsecutiveChars} consecutive identical characters`);
  }

  // Check for user info in password
  if (policy.preventUserInfoInPassword && userInfo) {
    const lowerPassword = password.toLowerCase();
    const emailPrefix = userInfo.email.split('@')[0].toLowerCase();

    if (lowerPassword.includes(emailPrefix)) {
      errors.push('Password cannot contain parts of your email');
    }

    if (userInfo.displayName) {
      const nameParts = userInfo.displayName.toLowerCase().split(/\s+/);
      for (const part of nameParts) {
        if (part.length >= 3 && lowerPassword.includes(part)) {
          errors.push('Password cannot contain parts of your name');
          break;
        }
      }
    }
  }

  return { valid: errors.length === 0, errors };
}
```

### 2.2 Session Management

```typescript
interface SessionConfig {
  accessTokenTTL: number;      // seconds
  refreshTokenTTL: number;     // seconds
  absoluteTimeout: number;     // seconds
  idleTimeout: number;         // seconds
  maxConcurrentSessions: number;
  rotateRefreshToken: boolean;
}

const SessionConfigs: Record<string, SessionConfig> = {
  free: {
    accessTokenTTL: 3600,           // 1 hour
    refreshTokenTTL: 86400 * 7,     // 7 days
    absoluteTimeout: 86400 * 30,    // 30 days
    idleTimeout: 3600,              // 1 hour
    maxConcurrentSessions: 3,
    rotateRefreshToken: true
  },
  professional: {
    accessTokenTTL: 3600,           // 1 hour
    refreshTokenTTL: 86400 * 14,    // 14 days
    absoluteTimeout: 86400 * 90,    // 90 days
    idleTimeout: 7200,              // 2 hours
    maxConcurrentSessions: 10,
    rotateRefreshToken: true
  },
  enterprise: {
    accessTokenTTL: 1800,           // 30 minutes (configurable)
    refreshTokenTTL: 86400 * 7,     // 7 days (configurable)
    absoluteTimeout: 86400 * 30,    // 30 days (configurable)
    idleTimeout: 1800,              // 30 minutes (configurable)
    maxConcurrentSessions: -1,      // Unlimited (configurable)
    rotateRefreshToken: true
  }
};
```

### 2.3 Multi-Factor Authentication (MFA)

#### TOTP Implementation

```typescript
import * as speakeasy from 'speakeasy';
import * as qrcode from 'qrcode';

interface TOTPConfig {
  issuer: string;
  algorithm: 'sha1' | 'sha256' | 'sha512';
  digits: 6 | 8;
  period: number;
  window: number;  // Number of periods to check
}

const defaultTOTPConfig: TOTPConfig = {
  issuer: 'Orochi DB',
  algorithm: 'sha1',  // Most compatible
  digits: 6,
  period: 30,
  window: 1
};

async function setupTOTP(
  userId: string,
  email: string,
  config: TOTPConfig = defaultTOTPConfig
): Promise<{ secret: string; qrCodeUrl: string; backupCodes: string[] }> {
  // Generate secret
  const secret = speakeasy.generateSecret({
    name: `${config.issuer}:${email}`,
    issuer: config.issuer,
    length: 32
  });

  // Generate QR code
  const qrCodeUrl = await qrcode.toDataURL(secret.otpauth_url);

  // Generate backup codes
  const backupCodes = await generateBackupCodes(10);

  return {
    secret: secret.base32,
    qrCodeUrl,
    backupCodes
  };
}

function verifyTOTP(
  token: string,
  secret: string,
  config: TOTPConfig = defaultTOTPConfig
): boolean {
  return speakeasy.totp.verify({
    secret,
    encoding: 'base32',
    token,
    algorithm: config.algorithm,
    digits: config.digits,
    step: config.period,
    window: config.window
  });
}

async function generateBackupCodes(count: number): Promise<string[]> {
  const codes: string[] = [];
  for (let i = 0; i < count; i++) {
    // Generate 8-character alphanumeric code
    const buffer = await crypto.randomBytes(6);
    const code = buffer.toString('base64')
      .replace(/[+/=]/g, '')
      .substring(0, 8)
      .toUpperCase();
    codes.push(code);
  }
  return codes;
}
```

#### WebAuthn/FIDO2 Implementation

```typescript
import {
  generateRegistrationOptions,
  verifyRegistrationResponse,
  generateAuthenticationOptions,
  verifyAuthenticationResponse
} from '@simplewebauthn/server';

const rpName = 'Orochi DB';
const rpID = 'orochi.db';
const origin = `https://${rpID}`;

async function registerWebAuthnCredential(
  user: { id: string; email: string; displayName: string },
  existingCredentials: { id: Buffer; transports?: AuthenticatorTransport[] }[]
): Promise<PublicKeyCredentialCreationOptionsJSON> {
  return generateRegistrationOptions({
    rpName,
    rpID,
    userID: user.id,
    userName: user.email,
    userDisplayName: user.displayName,
    attestationType: 'none',
    excludeCredentials: existingCredentials.map(cred => ({
      id: cred.id,
      type: 'public-key',
      transports: cred.transports
    })),
    authenticatorSelection: {
      residentKey: 'preferred',
      userVerification: 'preferred',
      authenticatorAttachment: 'cross-platform'
    }
  });
}

async function verifyWebAuthnRegistration(
  response: RegistrationResponseJSON,
  expectedChallenge: string
): Promise<VerifiedRegistrationResponse> {
  return verifyRegistrationResponse({
    response,
    expectedChallenge,
    expectedOrigin: origin,
    expectedRPID: rpID
  });
}
```

### 2.4 Rate Limiting and Brute Force Protection

```typescript
interface RateLimitConfig {
  windowMs: number;
  maxAttempts: number;
  lockoutDurationMs: number;
  lockoutThreshold: number;
}

const AuthRateLimits: Record<string, RateLimitConfig> = {
  login: {
    windowMs: 15 * 60 * 1000,     // 15 minutes
    maxAttempts: 5,
    lockoutDurationMs: 15 * 60 * 1000,  // 15 minutes
    lockoutThreshold: 5
  },
  passwordReset: {
    windowMs: 60 * 60 * 1000,    // 1 hour
    maxAttempts: 3,
    lockoutDurationMs: 60 * 60 * 1000,
    lockoutThreshold: 5
  },
  mfaVerification: {
    windowMs: 5 * 60 * 1000,     // 5 minutes
    maxAttempts: 5,
    lockoutDurationMs: 5 * 60 * 1000,
    lockoutThreshold: 10
  },
  apiKeyValidation: {
    windowMs: 60 * 1000,         // 1 minute
    maxAttempts: 100,
    lockoutDurationMs: 60 * 1000,
    lockoutThreshold: 500
  }
};

class RateLimiter {
  private redis: Redis;

  constructor(redis: Redis) {
    this.redis = redis;
  }

  async checkRateLimit(
    key: string,
    config: RateLimitConfig
  ): Promise<{ allowed: boolean; remaining: number; resetAt: Date }> {
    const now = Date.now();
    const windowKey = `ratelimit:${key}:${Math.floor(now / config.windowMs)}`;
    const lockoutKey = `ratelimit:lockout:${key}`;

    // Check for lockout
    const lockoutUntil = await this.redis.get(lockoutKey);
    if (lockoutUntil && parseInt(lockoutUntil) > now) {
      return {
        allowed: false,
        remaining: 0,
        resetAt: new Date(parseInt(lockoutUntil))
      };
    }

    // Increment counter
    const count = await this.redis.incr(windowKey);
    if (count === 1) {
      await this.redis.pexpire(windowKey, config.windowMs);
    }

    // Check if over limit
    if (count > config.maxAttempts) {
      // Apply lockout if threshold reached
      const totalKey = `ratelimit:total:${key}`;
      const totalCount = await this.redis.incr(totalKey);

      if (totalCount >= config.lockoutThreshold) {
        await this.redis.set(
          lockoutKey,
          (now + config.lockoutDurationMs).toString(),
          'PX',
          config.lockoutDurationMs
        );
      }

      return {
        allowed: false,
        remaining: 0,
        resetAt: new Date(now + config.windowMs)
      };
    }

    return {
      allowed: true,
      remaining: config.maxAttempts - count,
      resetAt: new Date(now + config.windowMs)
    };
  }
}
```

---

## 3. Authorization Security

### 3.1 Permission Check Middleware

```typescript
import { Request, Response, NextFunction } from 'express';

interface AuthContext {
  userId: string;
  orgId?: string;
  apiKeyId?: string;
  permissions: Set<string>;
}

declare global {
  namespace Express {
    interface Request {
      auth: AuthContext;
    }
  }
}

function requirePermission(permission: string) {
  return async (req: Request, res: Response, next: NextFunction) => {
    const { userId, orgId } = req.auth;
    const resourceType = getResourceTypeFromPath(req.path);
    const resourceId = getResourceIdFromParams(req.params);

    try {
      const hasPermission = await checkPermission(
        userId,
        resourceType,
        resourceId,
        permission
      );

      if (!hasPermission) {
        // Log authorization failure
        await logAuditEvent({
          orgId,
          userId,
          category: 'authorization',
          action: `${resourceType}.access_denied`,
          outcome: 'denied',
          resourceType,
          resourceId,
          eventData: {
            permission,
            path: req.path,
            method: req.method
          }
        });

        return res.status(403).json({
          error: 'Forbidden',
          message: `Missing required permission: ${permission}`
        });
      }

      next();
    } catch (error) {
      next(error);
    }
  };
}

// Usage example
app.delete(
  '/api/v1/databases/:databaseId',
  authenticate,
  requirePermission('project.databases.delete'),
  async (req, res) => {
    // Handler implementation
  }
);
```

### 3.2 Resource Hierarchy Authorization

```typescript
interface ResourceHierarchy {
  organization?: string;
  team?: string;
  project?: string;
  database?: string;
}

async function getResourceHierarchy(
  resourceType: string,
  resourceId: string
): Promise<ResourceHierarchy> {
  switch (resourceType) {
    case 'organization':
      return { organization: resourceId };

    case 'team': {
      const team = await db.query(
        'SELECT org_id FROM platform.teams WHERE team_id = $1',
        [resourceId]
      );
      return {
        organization: team.rows[0]?.org_id,
        team: resourceId
      };
    }

    case 'project': {
      const project = await db.query(
        'SELECT org_id, team_id FROM platform.projects WHERE project_id = $1',
        [resourceId]
      );
      return {
        organization: project.rows[0]?.org_id,
        team: project.rows[0]?.team_id,
        project: resourceId
      };
    }

    case 'database': {
      const database = await db.query(`
        SELECT p.org_id, p.team_id, d.project_id
        FROM platform.databases d
        JOIN platform.projects p ON d.project_id = p.project_id
        WHERE d.database_id = $1
      `, [resourceId]);
      return {
        organization: database.rows[0]?.org_id,
        team: database.rows[0]?.team_id,
        project: database.rows[0]?.project_id,
        database: resourceId
      };
    }

    default:
      throw new Error(`Unknown resource type: ${resourceType}`);
  }
}

async function checkPermission(
  userId: string,
  resourceType: string,
  resourceId: string,
  permission: string
): Promise<boolean> {
  // Use the database function for permission checking
  const result = await db.query(
    'SELECT platform.check_permission($1, $2, $3, $4) as has_permission',
    [userId, resourceType, resourceId, permission]
  );

  return result.rows[0]?.has_permission === true;
}
```

---

## 4. Input Validation and Sanitization

### 4.1 Zod Schema Validation

```typescript
import { z } from 'zod';

// Organization schemas
const OrganizationCreateSchema = z.object({
  name: z.string()
    .min(2, 'Organization name must be at least 2 characters')
    .max(255, 'Organization name must be at most 255 characters')
    .regex(/^[a-zA-Z0-9\s\-_.]+$/, 'Organization name contains invalid characters'),
  slug: z.string()
    .min(3, 'Slug must be at least 3 characters')
    .max(63, 'Slug must be at most 63 characters')
    .regex(/^[a-z][a-z0-9-]*[a-z0-9]$/, 'Slug must start with a letter and contain only lowercase letters, numbers, and hyphens'),
  billingEmail: z.string().email('Invalid email address'),
  planTier: z.enum(['free', 'starter', 'professional', 'enterprise']).optional()
});

// Database schemas
const DatabaseCreateSchema = z.object({
  name: z.string()
    .min(1, 'Database name is required')
    .max(63, 'Database name must be at most 63 characters')
    .regex(/^[a-z][a-z0-9_]*$/, 'Database name must start with a letter and contain only lowercase letters, numbers, and underscores'),
  projectId: z.string().uuid('Invalid project ID'),
  instanceType: z.string()
    .regex(/^(small|medium|large|xlarge|2xlarge|4xlarge|8xlarge)$/, 'Invalid instance type'),
  storageSizeGb: z.number()
    .int('Storage size must be an integer')
    .min(10, 'Minimum storage is 10 GB')
    .max(65536, 'Maximum storage is 65536 GB'),
  postgresVersion: z.enum(['16', '17', '18']).default('17'),
  highAvailability: z.boolean().default(false),
  extensions: z.array(z.string().regex(/^[a-z][a-z0-9_]*$/, 'Invalid extension name')).default([])
});

// API Key schemas
const ApiKeyCreateSchema = z.object({
  name: z.string()
    .min(1, 'API key name is required')
    .max(255, 'API key name must be at most 255 characters'),
  scope: z.enum(['user', 'project', 'database', 'service']),
  permissions: z.array(z.string()).default([]),
  resourceIds: z.array(z.string().uuid()).optional(),
  ipAllowlist: z.array(
    z.string().regex(
      /^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)(?:\/(?:3[0-2]|[12]?[0-9]))?$/,
      'Invalid IP address or CIDR notation'
    )
  ).optional(),
  expiresIn: z.string()
    .regex(/^(\d+)(h|d|w|m|y)$/, 'Invalid expiration format (e.g., 30d, 1y)')
    .optional()
});

// Validation middleware
function validateBody<T>(schema: z.ZodSchema<T>) {
  return (req: Request, res: Response, next: NextFunction) => {
    try {
      req.body = schema.parse(req.body);
      next();
    } catch (error) {
      if (error instanceof z.ZodError) {
        return res.status(400).json({
          error: 'Validation Error',
          details: error.errors.map(e => ({
            field: e.path.join('.'),
            message: e.message
          }))
        });
      }
      next(error);
    }
  };
}
```

### 4.2 SQL Injection Prevention

```typescript
// NEVER do this - vulnerable to SQL injection
// const query = `SELECT * FROM users WHERE email = '${userInput}'`;

// ALWAYS use parameterized queries
async function getUserByEmail(email: string): Promise<User | null> {
  const result = await db.query(
    'SELECT * FROM platform.users WHERE email = $1',
    [email]
  );
  return result.rows[0] || null;
}

// For dynamic table/column names, use allowlists
const ALLOWED_SORT_COLUMNS = new Set(['created_at', 'updated_at', 'name', 'email']);
const ALLOWED_SORT_DIRECTIONS = new Set(['ASC', 'DESC']);

async function listUsers(
  orgId: string,
  sortBy: string = 'created_at',
  sortDir: string = 'DESC'
): Promise<User[]> {
  // Validate sort column
  if (!ALLOWED_SORT_COLUMNS.has(sortBy)) {
    throw new Error('Invalid sort column');
  }
  if (!ALLOWED_SORT_DIRECTIONS.has(sortDir.toUpperCase())) {
    throw new Error('Invalid sort direction');
  }

  // Safe to use in query since validated against allowlist
  const result = await db.query(
    `SELECT u.* FROM platform.users u
     JOIN platform.organization_members om ON u.user_id = om.user_id
     WHERE om.org_id = $1 AND om.status = 'active'
     ORDER BY u.${sortBy} ${sortDir.toUpperCase()}`,
    [orgId]
  );
  return result.rows;
}
```

---

## 5. Data Protection

### 5.1 Encryption at Rest

```typescript
import * as crypto from 'crypto';

interface EncryptionConfig {
  algorithm: string;
  keyLength: number;
  ivLength: number;
}

const defaultEncryptionConfig: EncryptionConfig = {
  algorithm: 'aes-256-gcm',
  keyLength: 32,
  ivLength: 16
};

class DataEncryption {
  private masterKey: Buffer;
  private config: EncryptionConfig;

  constructor(masterKeyBase64: string, config: EncryptionConfig = defaultEncryptionConfig) {
    this.masterKey = Buffer.from(masterKeyBase64, 'base64');
    this.config = config;

    if (this.masterKey.length !== config.keyLength) {
      throw new Error(`Master key must be ${config.keyLength} bytes`);
    }
  }

  async encrypt(plaintext: string): Promise<string> {
    const iv = crypto.randomBytes(this.config.ivLength);
    const cipher = crypto.createCipheriv(
      this.config.algorithm,
      this.masterKey,
      iv
    ) as crypto.CipherGCM;

    let encrypted = cipher.update(plaintext, 'utf8', 'base64');
    encrypted += cipher.final('base64');

    const authTag = cipher.getAuthTag();

    // Format: iv:authTag:ciphertext
    return `${iv.toString('base64')}:${authTag.toString('base64')}:${encrypted}`;
  }

  async decrypt(ciphertext: string): Promise<string> {
    const [ivBase64, authTagBase64, encryptedBase64] = ciphertext.split(':');

    const iv = Buffer.from(ivBase64, 'base64');
    const authTag = Buffer.from(authTagBase64, 'base64');
    const encrypted = Buffer.from(encryptedBase64, 'base64');

    const decipher = crypto.createDecipheriv(
      this.config.algorithm,
      this.masterKey,
      iv
    ) as crypto.DecipherGCM;

    decipher.setAuthTag(authTag);

    let decrypted = decipher.update(encrypted);
    decrypted = Buffer.concat([decrypted, decipher.final()]);

    return decrypted.toString('utf8');
  }
}

// Key derivation for per-tenant encryption
async function deriveKeyForTenant(
  masterKey: Buffer,
  tenantId: string,
  purpose: string
): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    crypto.hkdf(
      'sha256',
      masterKey,
      tenantId,  // salt
      purpose,   // info
      32,        // key length
      (err, derivedKey) => {
        if (err) reject(err);
        else resolve(Buffer.from(derivedKey));
      }
    );
  });
}
```

### 5.2 Sensitive Data Masking

```typescript
interface MaskingRule {
  pattern: RegExp;
  replacement: (match: string) => string;
}

const maskingRules: MaskingRule[] = [
  // Email addresses
  {
    pattern: /([a-zA-Z0-9._%+-]+)@([a-zA-Z0-9.-]+\.[a-zA-Z]{2,})/g,
    replacement: (match) => {
      const [local, domain] = match.split('@');
      return `${local.substring(0, 2)}***@${domain}`;
    }
  },
  // Credit card numbers
  {
    pattern: /\b\d{4}[- ]?\d{4}[- ]?\d{4}[- ]?\d{4}\b/g,
    replacement: (match) => `****-****-****-${match.slice(-4)}`
  },
  // API keys
  {
    pattern: /orochi_[A-Za-z0-9+/]{32,}/g,
    replacement: (match) => `${match.substring(0, 12)}****`
  },
  // Connection strings
  {
    pattern: /postgresql:\/\/([^:]+):([^@]+)@/g,
    replacement: (match) => match.replace(/:([^:]+)@/, ':****@')
  },
  // IP addresses (optional - for privacy)
  {
    pattern: /\b(?:\d{1,3}\.){3}\d{1,3}\b/g,
    replacement: (match) => {
      const parts = match.split('.');
      return `${parts[0]}.${parts[1]}.xxx.xxx`;
    }
  }
];

function maskSensitiveData(text: string): string {
  let masked = text;
  for (const rule of maskingRules) {
    masked = masked.replace(rule.pattern, rule.replacement);
  }
  return masked;
}

// Apply masking to logs
function sanitizeLogData(data: Record<string, any>): Record<string, any> {
  const sensitiveFields = new Set([
    'password', 'secret', 'token', 'apiKey', 'api_key',
    'authorization', 'cookie', 'creditCard', 'ssn'
  ]);

  const sanitized: Record<string, any> = {};

  for (const [key, value] of Object.entries(data)) {
    if (sensitiveFields.has(key.toLowerCase())) {
      sanitized[key] = '[REDACTED]';
    } else if (typeof value === 'string') {
      sanitized[key] = maskSensitiveData(value);
    } else if (typeof value === 'object' && value !== null) {
      sanitized[key] = sanitizeLogData(value);
    } else {
      sanitized[key] = value;
    }
  }

  return sanitized;
}
```

---

## 6. Network Security

### 6.1 TLS Configuration

```nginx
# nginx.conf - TLS best practices

ssl_protocols TLSv1.2 TLSv1.3;
ssl_prefer_server_ciphers on;
ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305;

ssl_session_timeout 1d;
ssl_session_cache shared:SSL:50m;
ssl_session_tickets off;

# OCSP Stapling
ssl_stapling on;
ssl_stapling_verify on;

# Security headers
add_header Strict-Transport-Security "max-age=63072000; includeSubDomains; preload" always;
add_header X-Content-Type-Options "nosniff" always;
add_header X-Frame-Options "DENY" always;
add_header X-XSS-Protection "1; mode=block" always;
add_header Content-Security-Policy "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self' wss:;" always;
add_header Referrer-Policy "strict-origin-when-cross-origin" always;
```

### 6.2 IP Allowlisting

```typescript
import { Request, Response, NextFunction } from 'express';
import * as ipaddr from 'ipaddr.js';

interface IpAllowlistConfig {
  enabled: boolean;
  allowedRanges: string[];  // CIDR notation
  bypassForServiceAccounts: boolean;
}

function isIpAllowed(
  clientIp: string,
  allowedRanges: string[]
): boolean {
  try {
    const parsedIp = ipaddr.parse(clientIp);

    for (const range of allowedRanges) {
      const [rangeIp, prefixLength] = ipaddr.parseCIDR(range);

      if (parsedIp.kind() === rangeIp.kind()) {
        if (parsedIp.match(ipaddr.parseCIDR(range))) {
          return true;
        }
      }
    }

    return false;
  } catch {
    return false;
  }
}

function ipAllowlistMiddleware(getConfig: (orgId: string) => Promise<IpAllowlistConfig>) {
  return async (req: Request, res: Response, next: NextFunction) => {
    const orgId = req.auth?.orgId;

    if (!orgId) {
      return next();
    }

    const config = await getConfig(orgId);

    if (!config.enabled) {
      return next();
    }

    // Bypass for service accounts if configured
    if (config.bypassForServiceAccounts && req.auth?.apiKeyId) {
      return next();
    }

    const clientIp = req.ip || req.connection.remoteAddress;

    if (!clientIp || !isIpAllowed(clientIp, config.allowedRanges)) {
      await logAuditEvent({
        orgId,
        userId: req.auth?.userId,
        category: 'security',
        action: 'ip_blocked',
        outcome: 'denied',
        eventData: {
          clientIp,
          allowedRanges: config.allowedRanges
        }
      });

      return res.status(403).json({
        error: 'Forbidden',
        message: 'Access denied: IP address not in allowlist'
      });
    }

    next();
  };
}
```

---

## 7. Audit Logging Best Practices

### 7.1 Comprehensive Event Logging

```typescript
interface AuditEvent {
  eventTime?: Date;
  orgId: string;
  userId?: string;
  apiKeyId?: string;
  category: 'authentication' | 'authorization' | 'data_access' |
            'data_modification' | 'configuration' | 'security' |
            'billing' | 'system';
  action: string;
  outcome: 'success' | 'failure' | 'error' | 'denied';
  resourceType?: string;
  resourceId?: string;
  resourceName?: string;
  ipAddress?: string;
  userAgent?: string;
  requestId?: string;
  sessionId?: string;
  eventData?: Record<string, any>;
  previousState?: Record<string, any>;
  newState?: Record<string, any>;
  authenticationMethod?: string;
  mfaUsed?: boolean;
  riskScore?: number;
}

async function logAuditEvent(event: AuditEvent): Promise<string> {
  // Sanitize sensitive data
  const sanitizedEvent = {
    ...event,
    eventData: event.eventData ? sanitizeLogData(event.eventData) : undefined,
    previousState: event.previousState ? sanitizeLogData(event.previousState) : undefined,
    newState: event.newState ? sanitizeLogData(event.newState) : undefined
  };

  const result = await db.query(`
    INSERT INTO platform.audit_logs (
      event_time, org_id, user_id, api_key_id,
      event_category, event_action, event_outcome,
      resource_type, resource_id, resource_name,
      ip_address, user_agent, request_id, session_id,
      event_data, previous_state, new_state,
      authentication_method, mfa_used, risk_score
    ) VALUES (
      COALESCE($1, NOW()), $2, $3, $4,
      $5, $6, $7,
      $8, $9, $10,
      $11, $12, $13, $14,
      $15, $16, $17,
      $18, $19, $20
    )
    RETURNING log_id
  `, [
    sanitizedEvent.eventTime,
    sanitizedEvent.orgId,
    sanitizedEvent.userId,
    sanitizedEvent.apiKeyId,
    sanitizedEvent.category,
    sanitizedEvent.action,
    sanitizedEvent.outcome,
    sanitizedEvent.resourceType,
    sanitizedEvent.resourceId,
    sanitizedEvent.resourceName,
    sanitizedEvent.ipAddress,
    sanitizedEvent.userAgent,
    sanitizedEvent.requestId,
    sanitizedEvent.sessionId,
    sanitizedEvent.eventData ? JSON.stringify(sanitizedEvent.eventData) : '{}',
    sanitizedEvent.previousState ? JSON.stringify(sanitizedEvent.previousState) : null,
    sanitizedEvent.newState ? JSON.stringify(sanitizedEvent.newState) : null,
    sanitizedEvent.authenticationMethod,
    sanitizedEvent.mfaUsed,
    sanitizedEvent.riskScore
  ]);

  return result.rows[0].log_id;
}

// Events to always log
const CRITICAL_EVENTS = [
  // Authentication
  'user.login',
  'user.login_failed',
  'user.logout',
  'user.password_changed',
  'user.password_reset',
  'user.mfa_enabled',
  'user.mfa_disabled',

  // Authorization
  'permission.granted',
  'permission.revoked',
  'role.assigned',
  'role.removed',

  // Security
  'api_key.created',
  'api_key.revoked',
  'ip_blocked',
  'suspicious_activity',

  // Data access
  'database.connected',
  'database.query_executed',

  // Configuration
  'organization.settings_changed',
  'database.created',
  'database.deleted',
  'user.invited',
  'user.removed'
];
```

---

## 8. Security Monitoring and Alerting

### 8.1 Security Metrics

```typescript
interface SecurityMetrics {
  failedLoginAttempts: number;
  successfulLogins: number;
  mfaChallenges: number;
  mfaFailures: number;
  apiKeyUsage: number;
  deniedRequests: number;
  suspiciousActivities: number;
  dataExfiltrationAttempts: number;
}

async function collectSecurityMetrics(
  orgId: string,
  windowMinutes: number = 60
): Promise<SecurityMetrics> {
  const result = await db.query(`
    SELECT
      COUNT(*) FILTER (WHERE event_action = 'user.login_failed') as failed_logins,
      COUNT(*) FILTER (WHERE event_action = 'user.login' AND event_outcome = 'success') as successful_logins,
      COUNT(*) FILTER (WHERE event_action LIKE 'mfa.%') as mfa_challenges,
      COUNT(*) FILTER (WHERE event_action LIKE 'mfa.%' AND event_outcome = 'failure') as mfa_failures,
      COUNT(*) FILTER (WHERE api_key_id IS NOT NULL) as api_key_usage,
      COUNT(*) FILTER (WHERE event_outcome = 'denied') as denied_requests,
      COUNT(*) FILTER (WHERE event_category = 'security' AND risk_score > 0.7) as suspicious_activities
    FROM platform.audit_logs
    WHERE org_id = $1
      AND event_time > NOW() - ($2 || ' minutes')::interval
  `, [orgId, windowMinutes]);

  return {
    failedLoginAttempts: result.rows[0].failed_logins,
    successfulLogins: result.rows[0].successful_logins,
    mfaChallenges: result.rows[0].mfa_challenges,
    mfaFailures: result.rows[0].mfa_failures,
    apiKeyUsage: result.rows[0].api_key_usage,
    deniedRequests: result.rows[0].denied_requests,
    suspiciousActivities: result.rows[0].suspicious_activities,
    dataExfiltrationAttempts: 0  // Calculated separately
  };
}
```

### 8.2 Anomaly Detection

```typescript
interface AnomalyDetectionRule {
  name: string;
  description: string;
  query: string;
  threshold: number;
  severity: 'low' | 'medium' | 'high' | 'critical';
}

const anomalyRules: AnomalyDetectionRule[] = [
  {
    name: 'excessive_failed_logins',
    description: 'More than 10 failed login attempts in 5 minutes',
    query: `
      SELECT COUNT(*) as count
      FROM platform.audit_logs
      WHERE org_id = $1
        AND event_action = 'user.login_failed'
        AND event_time > NOW() - INTERVAL '5 minutes'
    `,
    threshold: 10,
    severity: 'high'
  },
  {
    name: 'unusual_data_access',
    description: 'Unusually high data access volume',
    query: `
      SELECT COALESCE(SUM((event_data->>'rows_returned')::bigint), 0) as count
      FROM platform.audit_logs
      WHERE org_id = $1
        AND event_action = 'database.query_executed'
        AND event_time > NOW() - INTERVAL '1 hour'
    `,
    threshold: 1000000,  // 1M rows
    severity: 'medium'
  },
  {
    name: 'admin_action_spike',
    description: 'Unusual spike in administrative actions',
    query: `
      SELECT COUNT(*) as count
      FROM platform.audit_logs
      WHERE org_id = $1
        AND event_category = 'configuration'
        AND event_time > NOW() - INTERVAL '15 minutes'
    `,
    threshold: 50,
    severity: 'high'
  },
  {
    name: 'api_key_abuse',
    description: 'Single API key used from multiple IPs',
    query: `
      SELECT COUNT(DISTINCT ip_address) as count
      FROM platform.audit_logs
      WHERE org_id = $1
        AND api_key_id IS NOT NULL
        AND event_time > NOW() - INTERVAL '1 hour'
      GROUP BY api_key_id
      HAVING COUNT(DISTINCT ip_address) > 5
    `,
    threshold: 1,  // Any key with >5 IPs triggers
    severity: 'high'
  }
];

async function runAnomalyDetection(orgId: string): Promise<void> {
  for (const rule of anomalyRules) {
    const result = await db.query(rule.query, [orgId]);
    const count = result.rows[0]?.count || 0;

    if (count > rule.threshold) {
      await createSecurityAlert({
        orgId,
        ruleName: rule.name,
        severity: rule.severity,
        description: rule.description,
        actualValue: count,
        threshold: rule.threshold
      });
    }
  }
}
```

---

## 9. Security Checklist

### 9.1 Pre-Production Checklist

- [ ] All passwords hashed with bcrypt (cost factor >= 12)
- [ ] No hardcoded credentials in codebase
- [ ] All secrets stored in secure vault
- [ ] TLS 1.2+ enforced for all connections
- [ ] Input validation on all user inputs
- [ ] Parameterized queries for all database operations
- [ ] Rate limiting configured for authentication endpoints
- [ ] MFA available for all users
- [ ] Audit logging enabled and tested
- [ ] RBAC permissions verified for all endpoints
- [ ] Error messages do not leak sensitive information
- [ ] Security headers configured (HSTS, CSP, etc.)
- [ ] Dependencies scanned for vulnerabilities
- [ ] Penetration testing completed

### 9.2 Ongoing Security Operations

- [ ] Weekly vulnerability scans
- [ ] Monthly access reviews
- [ ] Quarterly penetration tests
- [ ] Annual security training
- [ ] Incident response plan tested
- [ ] Backup restore procedures tested
- [ ] Key rotation executed on schedule
- [ ] Audit log retention verified

---

## 10. References

- [OWASP Authentication Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html)
- [NIST Digital Identity Guidelines](https://pages.nist.gov/800-63-3/)
- [PostgreSQL Security Best Practices](https://www.postgresql.org/docs/current/security.html)
- [WebAuthn Specification](https://www.w3.org/TR/webauthn/)
- [TOTP RFC 6238](https://tools.ietf.org/html/rfc6238)
