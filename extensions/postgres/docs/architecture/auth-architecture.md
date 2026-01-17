# Orochi DB Authentication Architecture

## Executive Summary

This document defines the foundational authentication architecture for Orochi DB, designed to unify Supabase-style (JWT-based, multi-tenant auth-as-a-service) and Neon-style (connection pooling, compute isolation) authentication patterns into a cohesive system that integrates deeply with PostgreSQL.

## 1. Design Principles

1. **PostgreSQL-Native**: Leverage PostgreSQL's authentication infrastructure while extending it
2. **Zero-Trust Security**: Every request authenticated, no implicit trust boundaries
3. **Performance-First**: Sub-millisecond token validation via shared memory caching
4. **Compliance-Ready**: Built-in audit logging for SOC2, HIPAA, GDPR
5. **Multi-Tenant**: Isolated authentication contexts per tenant/project
6. **Extensible**: Plugin architecture for custom authentication providers

## 2. Core Auth Schema Design

### 2.1 Schema Overview

```
orochi_auth
  |-- identities          (User identity, provider-agnostic)
  |-- users               (User profile and metadata)
  |-- credentials         (Password hashes, API keys)
  |-- sessions            (Active sessions)
  |-- tokens              (Access/refresh tokens)
  |-- api_keys            (Service API keys)
  |-- mfa_factors         (MFA enrollment)
  |-- providers           (OAuth providers config)
  |-- audit_log           (Security audit trail)
  |-- rate_limits         (Per-user rate limiting)
  |-- signing_keys        (JWT signing keys)
```

### 2.2 Identity Model

The identity model separates authentication (who you are) from authorization (what you can do).

```
                    +------------------+
                    |    Identity      |
                    |------------------|
                    | id (UUID)        |
                    | tenant_id        |
                    | created_at       |
                    +--------+---------+
                             |
            +----------------+----------------+
            |                |                |
    +-------v------+  +------v-------+  +----v--------+
    |   Password   |  |    OAuth     |  |   API Key   |
    |   Provider   |  |   Provider   |  |   Provider  |
    +--------------+  +--------------+  +-------------+
            |                |                |
            +----------------+----------------+
                             |
                    +--------v---------+
                    |      User        |
                    |------------------|
                    | Profile data     |
                    | Roles/Claims     |
                    | Metadata         |
                    +------------------+
```

## 3. C Data Structures (Shared Memory)

```c
/*-------------------------------------------------------------------------
 * auth.h
 *    Orochi DB Authentication System - Core Data Structures
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_AUTH_H
#define OROCHI_AUTH_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

/* ============================================================
 * Authentication Constants
 * ============================================================ */

#define OROCHI_AUTH_MAX_CACHED_TOKENS       10000
#define OROCHI_AUTH_MAX_CACHED_SESSIONS     5000
#define OROCHI_AUTH_MAX_SIGNING_KEYS        16
#define OROCHI_AUTH_MAX_API_KEYS_PER_USER   100
#define OROCHI_AUTH_TOKEN_HASH_SIZE         64
#define OROCHI_AUTH_SESSION_ID_SIZE         32
#define OROCHI_AUTH_API_KEY_PREFIX_SIZE     8
#define OROCHI_AUTH_USER_ID_SIZE            36      /* UUID string */
#define OROCHI_AUTH_TENANT_ID_SIZE          36
#define OROCHI_AUTH_PROVIDER_NAME_SIZE      64
#define OROCHI_AUTH_ROLE_NAME_SIZE          64
#define OROCHI_AUTH_MAX_ROLES               32
#define OROCHI_AUTH_JWT_MAX_SIZE            8192
#define OROCHI_AUTH_PASSWORD_HASH_SIZE      97      /* Argon2id */
#define OROCHI_AUTH_HMAC_KEY_SIZE           64
#define OROCHI_AUTH_AES_KEY_SIZE            32

/* Default timeouts and intervals */
#define OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC    900     /* 15 minutes */
#define OROCHI_AUTH_REFRESH_TOKEN_TTL_SEC   604800  /* 7 days */
#define OROCHI_AUTH_SESSION_TTL_SEC         86400   /* 24 hours */
#define OROCHI_AUTH_API_KEY_CLEANUP_SEC     3600    /* 1 hour */
#define OROCHI_AUTH_TOKEN_CLEANUP_SEC       300     /* 5 minutes */

/* ============================================================
 * Authentication Method Types
 * ============================================================ */

typedef enum OrochiAuthMethod
{
    OROCHI_AUTH_NONE = 0,
    OROCHI_AUTH_PASSWORD,           /* Email/password */
    OROCHI_AUTH_API_KEY,            /* Service API key */
    OROCHI_AUTH_JWT,                /* JWT bearer token */
    OROCHI_AUTH_OAUTH,              /* OAuth 2.0 provider */
    OROCHI_AUTH_MAGIC_LINK,         /* Passwordless email */
    OROCHI_AUTH_PHONE_OTP,          /* SMS OTP */
    OROCHI_AUTH_SAML,               /* Enterprise SAML */
    OROCHI_AUTH_CERTIFICATE,        /* Client certificate */
    OROCHI_AUTH_CONNECTION_STRING   /* Pooler connection string */
} OrochiAuthMethod;

/* ============================================================
 * Token Types
 * ============================================================ */

typedef enum OrochiTokenType
{
    OROCHI_TOKEN_ACCESS = 0,        /* Short-lived access token */
    OROCHI_TOKEN_REFRESH,           /* Long-lived refresh token */
    OROCHI_TOKEN_API_KEY,           /* API key (no expiry) */
    OROCHI_TOKEN_CONNECTION,        /* Connection pooler token */
    OROCHI_TOKEN_INVITATION,        /* User invitation token */
    OROCHI_TOKEN_PASSWORD_RESET,    /* Password reset token */
    OROCHI_TOKEN_EMAIL_VERIFY       /* Email verification token */
} OrochiTokenType;

/* ============================================================
 * Identity Provider Types
 * ============================================================ */

typedef enum OrochiProviderType
{
    OROCHI_PROVIDER_EMAIL = 0,      /* Email/password */
    OROCHI_PROVIDER_PHONE,          /* Phone/SMS */
    OROCHI_PROVIDER_GOOGLE,         /* Google OAuth */
    OROCHI_PROVIDER_GITHUB,         /* GitHub OAuth */
    OROCHI_PROVIDER_GITLAB,         /* GitLab OAuth */
    OROCHI_PROVIDER_AZURE,          /* Azure AD */
    OROCHI_PROVIDER_OKTA,           /* Okta */
    OROCHI_PROVIDER_SAML,           /* Generic SAML */
    OROCHI_PROVIDER_OIDC,           /* Generic OIDC */
    OROCHI_PROVIDER_CUSTOM          /* Custom provider */
} OrochiProviderType;

/* ============================================================
 * Session State
 * ============================================================ */

typedef enum OrochiSessionState
{
    OROCHI_SESSION_ACTIVE = 0,
    OROCHI_SESSION_IDLE,
    OROCHI_SESSION_EXPIRED,
    OROCHI_SESSION_REVOKED,
    OROCHI_SESSION_MFA_REQUIRED
} OrochiSessionState;

/* ============================================================
 * MFA Factor Types
 * ============================================================ */

typedef enum OrochiMfaType
{
    OROCHI_MFA_NONE = 0,
    OROCHI_MFA_TOTP,                /* Time-based OTP (Authenticator apps) */
    OROCHI_MFA_SMS,                 /* SMS OTP */
    OROCHI_MFA_EMAIL,               /* Email OTP */
    OROCHI_MFA_WEBAUTHN,            /* WebAuthn/FIDO2 */
    OROCHI_MFA_RECOVERY_CODE        /* Recovery codes */
} OrochiMfaType;

/* ============================================================
 * Audit Event Types
 * ============================================================ */

typedef enum OrochiAuditEvent
{
    OROCHI_AUDIT_LOGIN_SUCCESS = 0,
    OROCHI_AUDIT_LOGIN_FAILED,
    OROCHI_AUDIT_LOGOUT,
    OROCHI_AUDIT_TOKEN_ISSUED,
    OROCHI_AUDIT_TOKEN_REVOKED,
    OROCHI_AUDIT_TOKEN_REFRESHED,
    OROCHI_AUDIT_PASSWORD_CHANGED,
    OROCHI_AUDIT_PASSWORD_RESET,
    OROCHI_AUDIT_MFA_ENROLLED,
    OROCHI_AUDIT_MFA_VERIFIED,
    OROCHI_AUDIT_MFA_FAILED,
    OROCHI_AUDIT_API_KEY_CREATED,
    OROCHI_AUDIT_API_KEY_REVOKED,
    OROCHI_AUDIT_USER_CREATED,
    OROCHI_AUDIT_USER_UPDATED,
    OROCHI_AUDIT_USER_DELETED,
    OROCHI_AUDIT_ROLE_ASSIGNED,
    OROCHI_AUDIT_ROLE_REVOKED,
    OROCHI_AUDIT_PERMISSION_DENIED,
    OROCHI_AUDIT_RATE_LIMITED,
    OROCHI_AUDIT_SUSPICIOUS_ACTIVITY
} OrochiAuditEvent;

/* ============================================================
 * Core Identity Structure
 * ============================================================ */

typedef struct OrochiIdentity
{
    char                identity_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char                provider_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    OrochiProviderType  provider_type;
    char                provider_user_id[256];      /* External user ID */
    char                email[256];
    bool                email_verified;
    char                phone[32];
    bool                phone_verified;
    TimestampTz         created_at;
    TimestampTz         updated_at;
    TimestampTz         last_sign_in;
    bool                is_active;
    bool                is_admin;
} OrochiIdentity;

/* ============================================================
 * User Profile Structure
 * ============================================================ */

typedef struct OrochiUser
{
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char                identity_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                email[256];
    char                display_name[256];
    char                avatar_url[512];
    char                roles[OROCHI_AUTH_MAX_ROLES][OROCHI_AUTH_ROLE_NAME_SIZE];
    int                 role_count;
    char                metadata[4096];             /* JSON metadata */
    char                app_metadata[4096];         /* Admin-only metadata */
    TimestampTz         created_at;
    TimestampTz         updated_at;
    TimestampTz         last_activity;
    TimestampTz         banned_until;               /* NULL if not banned */
    bool                is_super_admin;
    bool                is_sso_user;
} OrochiUser;

/* ============================================================
 * Credential Structure (Password/API Key)
 * ============================================================ */

typedef struct OrochiCredential
{
    int64               credential_id;
    char                identity_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    OrochiAuthMethod    auth_method;
    char                password_hash[OROCHI_AUTH_PASSWORD_HASH_SIZE + 1];
    char                api_key_prefix[OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1];
    char                api_key_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    char                api_key_name[128];
    int64               api_key_scopes;             /* Bitmask of permissions */
    TimestampTz         created_at;
    TimestampTz         last_used;
    TimestampTz         expires_at;                 /* NULL for no expiry */
    bool                is_active;
} OrochiCredential;

/* ============================================================
 * Session Structure
 * ============================================================ */

typedef struct OrochiSession
{
    char                session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    OrochiSessionState  state;
    OrochiAuthMethod    auth_method;
    char                ip_address[64];
    char                user_agent[512];
    char                device_id[64];
    char                geo_country[4];
    char                geo_city[128];
    TimestampTz         created_at;
    TimestampTz         last_activity;
    TimestampTz         expires_at;
    int                 refresh_count;
    bool                mfa_verified;
    char                aal[16];                    /* Authenticator Assurance Level */
} OrochiSession;

/* ============================================================
 * Token Structure (Cached in Shared Memory)
 * ============================================================ */

typedef struct OrochiToken
{
    char                token_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    OrochiTokenType     token_type;
    char                session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    TimestampTz         issued_at;
    TimestampTz         expires_at;
    TimestampTz         not_before;
    int64               scopes;                     /* Bitmask */
    bool                is_revoked;
    char                jti[64];                    /* JWT ID for revocation */
} OrochiToken;

/* ============================================================
 * Signing Key Structure
 * ============================================================ */

typedef struct OrochiSigningKey
{
    char                key_id[64];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    int                 algorithm;                  /* RS256, ES256, etc. */
    char                public_key[4096];
    char                private_key_encrypted[8192];
    TimestampTz         created_at;
    TimestampTz         expires_at;
    TimestampTz         rotated_at;
    bool                is_current;
    bool                is_active;
    int                 version;
} OrochiSigningKey;

/* ============================================================
 * MFA Factor Structure
 * ============================================================ */

typedef struct OrochiMfaFactor
{
    int64               factor_id;
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    OrochiMfaType       factor_type;
    char                friendly_name[128];
    char                secret_encrypted[512];      /* TOTP secret */
    char                phone_number[32];           /* For SMS */
    bool                is_verified;
    bool                is_default;
    TimestampTz         created_at;
    TimestampTz         last_used;
} OrochiMfaFactor;

/* ============================================================
 * Rate Limit Entry
 * ============================================================ */

typedef struct OrochiRateLimit
{
    char                key[256];                   /* user_id:action or ip:action */
    int                 count;
    int                 limit;
    TimestampTz         window_start;
    int                 window_seconds;
    bool                is_blocked;
    TimestampTz         blocked_until;
} OrochiRateLimit;

/* ============================================================
 * Audit Log Entry
 * ============================================================ */

typedef struct OrochiAuditEntry
{
    int64               audit_id;
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    OrochiAuditEvent    event_type;
    char                ip_address[64];
    char                user_agent[512];
    char                resource_type[64];
    char                resource_id[256];
    char                old_value[4096];
    char                new_value[4096];
    char                metadata[4096];             /* Additional JSON context */
    TimestampTz         created_at;
    bool                is_sensitive;               /* Mask in logs */
} OrochiAuditEntry;

/* ============================================================
 * Connection Context (for pooler auth)
 * ============================================================ */

typedef struct OrochiConnectionContext
{
    char                connection_id[64];
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char                database[64];
    char                role[64];
    OrochiAuthMethod    auth_method;
    int                 backend_pid;
    bool                is_pooled;
    bool                is_readonly;
    int64               max_connections;
    int64               current_connections;
    TimestampTz         connected_at;
    TimestampTz         last_query;
} OrochiConnectionContext;

/* ============================================================
 * Shared Memory State for Auth Cache
 * ============================================================ */

typedef struct OrochiAuthSharedState
{
    LWLock             *main_lock;
    LWLock             *token_cache_lock;
    LWLock             *session_cache_lock;
    LWLock             *rate_limit_lock;
    LWLock             *signing_key_lock;

    /* Token cache (hash table in shared memory) */
    int                 token_cache_count;
    OrochiToken         token_cache[OROCHI_AUTH_MAX_CACHED_TOKENS];

    /* Session cache */
    int                 session_cache_count;
    OrochiSession       session_cache[OROCHI_AUTH_MAX_CACHED_SESSIONS];

    /* Signing keys (rotated periodically) */
    int                 signing_key_count;
    OrochiSigningKey    signing_keys[OROCHI_AUTH_MAX_SIGNING_KEYS];

    /* Rate limiting state */
    int                 rate_limit_count;
    OrochiRateLimit     rate_limits[1024];

    /* Statistics */
    int64               total_auth_requests;
    int64               cache_hits;
    int64               cache_misses;
    int64               failed_authentications;
    int64               tokens_issued;
    int64               tokens_revoked;

    /* Timestamps */
    TimestampTz         last_key_rotation;
    TimestampTz         last_cleanup;
    TimestampTz         startup_time;

    bool                is_initialized;
} OrochiAuthSharedState;

/* ============================================================
 * Authentication Result
 * ============================================================ */

typedef struct OrochiAuthResult
{
    bool                success;
    char                error_code[32];
    char                error_message[256];
    OrochiUser         *user;
    OrochiSession      *session;
    char                access_token[OROCHI_AUTH_JWT_MAX_SIZE];
    char                refresh_token[256];
    int                 access_token_expires_in;
    int                 refresh_token_expires_in;
    bool                mfa_required;
    OrochiMfaType       mfa_type;
} OrochiAuthResult;

/* ============================================================
 * Token Validation Result
 * ============================================================ */

typedef struct OrochiTokenValidation
{
    bool                is_valid;
    char                error_code[32];
    char                error_message[256];
    char                user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char                tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char                session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    int64               scopes;
    TimestampTz         expires_at;
    bool                is_expired;
    bool                is_revoked;
    char                roles[OROCHI_AUTH_MAX_ROLES][OROCHI_AUTH_ROLE_NAME_SIZE];
    int                 role_count;
} OrochiTokenValidation;

#endif /* OROCHI_AUTH_H */
```

## 4. SQL Schema Definition

```sql
-- ============================================================
-- Orochi Auth Schema
-- ============================================================

CREATE SCHEMA IF NOT EXISTS orochi_auth;

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

-- ============================================================
-- Core Identity Tables
-- ============================================================

-- Identities: Provider-agnostic user identity
CREATE TABLE orochi_auth.identities (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    tenant_id UUID NOT NULL,
    provider_type INTEGER NOT NULL DEFAULT 0,
    provider_id TEXT NOT NULL,
    email TEXT,
    email_verified BOOLEAN NOT NULL DEFAULT FALSE,
    phone TEXT,
    phone_verified BOOLEAN NOT NULL DEFAULT FALSE,
    identity_data JSONB NOT NULL DEFAULT '{}',
    last_sign_in_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT identities_provider_unique UNIQUE (tenant_id, provider_type, provider_id)
);

CREATE INDEX idx_identities_tenant ON orochi_auth.identities(tenant_id);
CREATE INDEX idx_identities_email ON orochi_auth.identities(tenant_id, email) WHERE email IS NOT NULL;
CREATE INDEX idx_identities_phone ON orochi_auth.identities(tenant_id, phone) WHERE phone IS NOT NULL;

-- Users: User profiles and metadata
CREATE TABLE orochi_auth.users (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    tenant_id UUID NOT NULL,
    identity_id UUID NOT NULL REFERENCES orochi_auth.identities(id) ON DELETE CASCADE,
    email TEXT,
    encrypted_password TEXT,
    email_confirmed_at TIMESTAMPTZ,
    phone TEXT,
    phone_confirmed_at TIMESTAMPTZ,
    confirmation_token TEXT,
    confirmation_sent_at TIMESTAMPTZ,
    recovery_token TEXT,
    recovery_sent_at TIMESTAMPTZ,
    email_change_token_new TEXT,
    email_change TEXT,
    email_change_sent_at TIMESTAMPTZ,
    last_sign_in_at TIMESTAMPTZ,
    raw_app_meta_data JSONB NOT NULL DEFAULT '{}',
    raw_user_meta_data JSONB NOT NULL DEFAULT '{}',
    is_super_admin BOOLEAN NOT NULL DEFAULT FALSE,
    is_sso_user BOOLEAN NOT NULL DEFAULT FALSE,
    role TEXT NOT NULL DEFAULT 'authenticated',
    aal TEXT CHECK (aal IN ('aal1', 'aal2', 'aal3')),
    banned_until TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT users_email_unique UNIQUE (tenant_id, email)
);

CREATE INDEX idx_users_tenant ON orochi_auth.users(tenant_id);
CREATE INDEX idx_users_identity ON orochi_auth.users(identity_id);
CREATE INDEX idx_users_email ON orochi_auth.users(tenant_id, email);
CREATE INDEX idx_users_created ON orochi_auth.users(created_at);

-- ============================================================
-- Credentials Table
-- ============================================================

CREATE TABLE orochi_auth.credentials (
    id BIGSERIAL PRIMARY KEY,
    identity_id UUID NOT NULL REFERENCES orochi_auth.identities(id) ON DELETE CASCADE,
    credential_type INTEGER NOT NULL,  -- 0=password, 1=api_key, 2=oauth

    -- Password credentials
    password_hash TEXT,
    password_salt TEXT,
    password_algorithm TEXT DEFAULT 'argon2id',

    -- API Key credentials
    api_key_prefix TEXT,
    api_key_hash TEXT,
    api_key_name TEXT,
    api_key_scopes BIGINT DEFAULT 0,

    -- Common fields
    last_used_at TIMESTAMPTZ,
    expires_at TIMESTAMPTZ,
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT credentials_api_key_unique UNIQUE (api_key_prefix)
);

CREATE INDEX idx_credentials_identity ON orochi_auth.credentials(identity_id);
CREATE INDEX idx_credentials_api_key ON orochi_auth.credentials(api_key_prefix) WHERE api_key_prefix IS NOT NULL;

-- ============================================================
-- Sessions Table
-- ============================================================

CREATE TABLE orochi_auth.sessions (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES orochi_auth.users(id) ON DELETE CASCADE,
    tenant_id UUID NOT NULL,
    factor_id UUID,
    aal TEXT CHECK (aal IN ('aal1', 'aal2', 'aal3')),
    not_after TIMESTAMPTZ,
    refreshed_at TIMESTAMPTZ,
    user_agent TEXT,
    ip TEXT,
    tag TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
) PARTITION BY RANGE (created_at);

-- Create monthly partitions (automated by background worker)
CREATE TABLE orochi_auth.sessions_default PARTITION OF orochi_auth.sessions DEFAULT;

CREATE INDEX idx_sessions_user ON orochi_auth.sessions(user_id);
CREATE INDEX idx_sessions_tenant ON orochi_auth.sessions(tenant_id);
CREATE INDEX idx_sessions_created ON orochi_auth.sessions(created_at);
CREATE INDEX idx_sessions_not_after ON orochi_auth.sessions(not_after) WHERE not_after IS NOT NULL;

-- ============================================================
-- Refresh Tokens Table
-- ============================================================

CREATE TABLE orochi_auth.refresh_tokens (
    id BIGSERIAL PRIMARY KEY,
    token TEXT NOT NULL,
    user_id UUID NOT NULL REFERENCES orochi_auth.users(id) ON DELETE CASCADE,
    session_id UUID REFERENCES orochi_auth.sessions(id) ON DELETE CASCADE,
    parent TEXT,
    revoked BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT refresh_tokens_token_unique UNIQUE (token)
) PARTITION BY RANGE (created_at);

CREATE TABLE orochi_auth.refresh_tokens_default PARTITION OF orochi_auth.refresh_tokens DEFAULT;

CREATE INDEX idx_refresh_tokens_token ON orochi_auth.refresh_tokens(token);
CREATE INDEX idx_refresh_tokens_session ON orochi_auth.refresh_tokens(session_id);
CREATE INDEX idx_refresh_tokens_user ON orochi_auth.refresh_tokens(user_id);
CREATE INDEX idx_refresh_tokens_parent ON orochi_auth.refresh_tokens(parent) WHERE parent IS NOT NULL;

-- ============================================================
-- MFA Factors Table
-- ============================================================

CREATE TABLE orochi_auth.mfa_factors (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES orochi_auth.users(id) ON DELETE CASCADE,
    friendly_name TEXT,
    factor_type TEXT NOT NULL CHECK (factor_type IN ('totp', 'phone', 'webauthn')),
    status TEXT NOT NULL CHECK (status IN ('unverified', 'verified')) DEFAULT 'unverified',
    secret_encrypted TEXT,
    phone TEXT,
    last_challenged_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_mfa_factors_user ON orochi_auth.mfa_factors(user_id);
CREATE INDEX idx_mfa_factors_status ON orochi_auth.mfa_factors(user_id, status);

-- MFA Challenges (for verification flow)
CREATE TABLE orochi_auth.mfa_challenges (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    factor_id UUID NOT NULL REFERENCES orochi_auth.mfa_factors(id) ON DELETE CASCADE,
    ip_address TEXT NOT NULL,
    verified_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    otp_code TEXT  -- Hashed OTP for SMS/email
);

CREATE INDEX idx_mfa_challenges_factor ON orochi_auth.mfa_challenges(factor_id);
CREATE INDEX idx_mfa_challenges_created ON orochi_auth.mfa_challenges(created_at);

-- ============================================================
-- API Keys Table
-- ============================================================

CREATE TABLE orochi_auth.api_keys (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    tenant_id UUID NOT NULL,
    user_id UUID REFERENCES orochi_auth.users(id) ON DELETE SET NULL,
    name TEXT NOT NULL,
    description TEXT,
    key_prefix TEXT NOT NULL,
    key_hash TEXT NOT NULL,
    scopes BIGINT NOT NULL DEFAULT 0,
    rate_limit INTEGER DEFAULT 1000,
    allowed_ips TEXT[],
    allowed_origins TEXT[],
    last_used_at TIMESTAMPTZ,
    expires_at TIMESTAMPTZ,
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT api_keys_prefix_unique UNIQUE (key_prefix)
);

CREATE INDEX idx_api_keys_tenant ON orochi_auth.api_keys(tenant_id);
CREATE INDEX idx_api_keys_prefix ON orochi_auth.api_keys(key_prefix);
CREATE INDEX idx_api_keys_user ON orochi_auth.api_keys(user_id) WHERE user_id IS NOT NULL;

-- ============================================================
-- OAuth Providers Configuration
-- ============================================================

CREATE TABLE orochi_auth.sso_providers (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    tenant_id UUID NOT NULL,
    provider_type INTEGER NOT NULL,
    name TEXT NOT NULL,
    client_id TEXT NOT NULL,
    client_secret_encrypted TEXT NOT NULL,
    redirect_uri TEXT NOT NULL,
    scopes TEXT[] NOT NULL DEFAULT '{}',
    metadata JSONB NOT NULL DEFAULT '{}',
    attribute_mapping JSONB NOT NULL DEFAULT '{}',
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT sso_providers_unique UNIQUE (tenant_id, provider_type)
);

CREATE INDEX idx_sso_providers_tenant ON orochi_auth.sso_providers(tenant_id);

-- SSO Domains (for enterprise SAML/OIDC)
CREATE TABLE orochi_auth.sso_domains (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    provider_id UUID NOT NULL REFERENCES orochi_auth.sso_providers(id) ON DELETE CASCADE,
    domain TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT sso_domains_unique UNIQUE (domain)
);

CREATE INDEX idx_sso_domains_domain ON orochi_auth.sso_domains(domain);

-- ============================================================
-- Signing Keys Table
-- ============================================================

CREATE TABLE orochi_auth.signing_keys (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    tenant_id UUID NOT NULL,
    key_id TEXT NOT NULL,
    algorithm TEXT NOT NULL DEFAULT 'RS256',
    public_key TEXT NOT NULL,
    private_key_encrypted TEXT NOT NULL,
    is_current BOOLEAN NOT NULL DEFAULT FALSE,
    version INTEGER NOT NULL DEFAULT 1,
    expires_at TIMESTAMPTZ,
    rotated_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT signing_keys_kid_unique UNIQUE (tenant_id, key_id)
);

CREATE INDEX idx_signing_keys_tenant ON orochi_auth.signing_keys(tenant_id);
CREATE INDEX idx_signing_keys_current ON orochi_auth.signing_keys(tenant_id, is_current) WHERE is_current = TRUE;

-- ============================================================
-- Audit Log Table (Partitioned)
-- ============================================================

CREATE TABLE orochi_auth.audit_log (
    id BIGSERIAL,
    tenant_id UUID NOT NULL,
    actor_id UUID,
    actor_type TEXT NOT NULL DEFAULT 'user',
    event_type INTEGER NOT NULL,
    ip_address TEXT,
    user_agent TEXT,
    resource_type TEXT,
    resource_id TEXT,
    old_data JSONB,
    new_data JSONB,
    metadata JSONB,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    PRIMARY KEY (created_at, id)
) PARTITION BY RANGE (created_at);

-- Automated monthly partition creation
CREATE TABLE orochi_auth.audit_log_default PARTITION OF orochi_auth.audit_log DEFAULT;

CREATE INDEX idx_audit_log_tenant ON orochi_auth.audit_log(tenant_id, created_at);
CREATE INDEX idx_audit_log_actor ON orochi_auth.audit_log(actor_id, created_at) WHERE actor_id IS NOT NULL;
CREATE INDEX idx_audit_log_event ON orochi_auth.audit_log(event_type, created_at);
CREATE INDEX idx_audit_log_resource ON orochi_auth.audit_log(resource_type, resource_id, created_at);

-- ============================================================
-- Rate Limiting Table
-- ============================================================

CREATE TABLE orochi_auth.rate_limits (
    id BIGSERIAL PRIMARY KEY,
    key TEXT NOT NULL,
    action TEXT NOT NULL,
    count INTEGER NOT NULL DEFAULT 0,
    window_start TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    window_seconds INTEGER NOT NULL DEFAULT 60,
    is_blocked BOOLEAN NOT NULL DEFAULT FALSE,
    blocked_until TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT rate_limits_unique UNIQUE (key, action)
);

CREATE INDEX idx_rate_limits_key ON orochi_auth.rate_limits(key);
CREATE INDEX idx_rate_limits_blocked ON orochi_auth.rate_limits(blocked_until) WHERE is_blocked = TRUE;

-- ============================================================
-- Flow State (for OAuth flows)
-- ============================================================

CREATE TABLE orochi_auth.flow_state (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID,
    auth_code TEXT NOT NULL,
    code_challenge TEXT,
    code_challenge_method TEXT,
    provider_type INTEGER NOT NULL,
    provider_access_token TEXT,
    provider_refresh_token TEXT,
    authentication_method TEXT NOT NULL,
    auth_code_issued_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_flow_state_auth_code ON orochi_auth.flow_state(auth_code);
CREATE INDEX idx_flow_state_created ON orochi_auth.flow_state(created_at);

-- ============================================================
-- One-Time Tokens
-- ============================================================

CREATE TABLE orochi_auth.one_time_tokens (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES orochi_auth.users(id) ON DELETE CASCADE,
    token_type TEXT NOT NULL CHECK (token_type IN (
        'confirmation', 'recovery', 'email_change',
        'phone_change', 'reauthentication', 'invite'
    )),
    token_hash TEXT NOT NULL,
    relates_to TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT one_time_tokens_hash_unique UNIQUE (token_hash)
);

CREATE INDEX idx_one_time_tokens_user ON orochi_auth.one_time_tokens(user_id);
CREATE INDEX idx_one_time_tokens_type ON orochi_auth.one_time_tokens(user_id, token_type);
CREATE INDEX idx_one_time_tokens_relates ON orochi_auth.one_time_tokens(relates_to) WHERE relates_to IS NOT NULL;
```

## 5. Authentication Flow Architecture

### 5.1 Request Authentication Pipeline

```
                                  +------------------+
                                  |  Incoming Request |
                                  +--------+---------+
                                           |
                                  +--------v---------+
                                  |  Extract Token   |
                                  |  (Header/Cookie) |
                                  +--------+---------+
                                           |
                    +----------------------+----------------------+
                    |                      |                      |
            +-------v-------+      +-------v-------+      +-------v-------+
            |  JWT Bearer   |      |   API Key     |      |  Connection   |
            |    Token      |      |  (X-API-Key)  |      |    String     |
            +-------+-------+      +-------+-------+      +-------+-------+
                    |                      |                      |
                    v                      v                      v
            +------------------+   +------------------+   +------------------+
            | Check SHM Cache  |   | Hash & Lookup    |   | Parse Pooler    |
            | (Token Hash)     |   | API Key          |   | Connection      |
            +--------+---------+   +--------+---------+   +--------+---------+
                     |                      |                      |
         +-----------+-----------+          |                      |
         | Cache Hit | Cache Miss|          |                      |
         v           v           |          |                      |
    +--------+  +--------+       |          |                      |
    |Validate|  |  Full  |       |          |                      |
    |Expiry  |  |JWT Parse|      |          |                      |
    +---+----+  +----+---+       |          |                      |
        |            |           |          |                      |
        +------+-----+           |          |                      |
               |                 |          |                      |
               v                 v          v                      v
        +------+-----------------+----------+----------------------+-----+
        |                    Rate Limit Check                            |
        +--------------------------------+-------------------------------+
                                         |
                              +----------v-----------+
                              |   Build Auth Context |
                              |   (User, Roles, etc.)|
                              +----------+-----------+
                                         |
                              +----------v-----------+
                              |   Set Session Vars   |
                              | (orochi_auth.user_id)|
                              +----------+-----------+
                                         |
                              +----------v-----------+
                              |   Execute Query      |
                              +----------------------+
```

### 5.2 Token Validation Flow

```c
/* ============================================================
 * Token Validation Flow (Pseudo-code)
 * ============================================================ */

/*
 * orochi_auth_validate_token
 *    Main entry point for token validation
 *
 * Steps:
 *    1. Hash the token for cache lookup
 *    2. Check shared memory cache (hot path)
 *    3. If cache miss, perform full JWT validation
 *    4. Verify signature against signing keys
 *    5. Check expiration, not-before, revocation
 *    6. Extract claims and roles
 *    7. Cache result for future requests
 *
 * Returns OrochiTokenValidation with user context
 */
OrochiTokenValidation
orochi_auth_validate_token(const char *token, const char *tenant_id)
{
    OrochiTokenValidation result = {0};
    char                  token_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    OrochiToken          *cached_token;

    /* Step 1: Hash token for lookup */
    orochi_auth_hash_token(token, token_hash);

    /* Step 2: Check shared memory cache */
    LWLockAcquire(AuthSharedState->token_cache_lock, LW_SHARED);
    cached_token = orochi_auth_cache_lookup(token_hash);

    if (cached_token != NULL)
    {
        /* Cache hit - validate expiry only */
        AuthSharedState->cache_hits++;

        if (cached_token->expires_at < GetCurrentTimestamp())
        {
            result.is_valid = false;
            result.is_expired = true;
            strcpy(result.error_code, "TOKEN_EXPIRED");
        }
        else if (cached_token->is_revoked)
        {
            result.is_valid = false;
            result.is_revoked = true;
            strcpy(result.error_code, "TOKEN_REVOKED");
        }
        else
        {
            result.is_valid = true;
            memcpy(result.user_id, cached_token->user_id, OROCHI_AUTH_USER_ID_SIZE);
            memcpy(result.tenant_id, cached_token->tenant_id, OROCHI_AUTH_TENANT_ID_SIZE);
            result.scopes = cached_token->scopes;
            result.expires_at = cached_token->expires_at;
        }

        LWLockRelease(AuthSharedState->token_cache_lock);
        return result;
    }

    LWLockRelease(AuthSharedState->token_cache_lock);
    AuthSharedState->cache_misses++;

    /* Step 3-6: Full JWT validation */
    result = orochi_auth_validate_jwt_full(token, tenant_id);

    /* Step 7: Cache valid tokens */
    if (result.is_valid)
    {
        orochi_auth_cache_token(&result, token_hash);
    }

    return result;
}
```

### 5.3 Session Establishment Flow

```
User                    Auth Endpoint                   Database
  |                           |                             |
  |--- Login Request -------->|                             |
  |    (email, password)      |                             |
  |                           |--- Lookup User ------------>|
  |                           |<-- User Record -------------|
  |                           |                             |
  |                           |--- Verify Password         |
  |                           |    (Argon2id)              |
  |                           |                             |
  |                           |--- Check MFA Required ---->|
  |                           |<-- MFA Factors -------------|
  |                           |                             |
  |<-- MFA Challenge ---------|                             |
  |    (if required)          |                             |
  |                           |                             |
  |--- MFA Response --------->|                             |
  |    (TOTP code)            |                             |
  |                           |--- Verify MFA Code         |
  |                           |                             |
  |                           |--- Create Session -------->|
  |                           |<-- Session ID --------------|
  |                           |                             |
  |                           |--- Generate Tokens         |
  |                           |    (Access + Refresh)      |
  |                           |                             |
  |                           |--- Cache Session -------->SHM
  |                           |                             |
  |                           |--- Audit Log ------------->|
  |                           |                             |
  |<-- Auth Response ---------|                             |
  |    (tokens, session)      |                             |
```

## 6. PostgreSQL Extension Integration

### 6.1 Background Workers

```c
/* ============================================================
 * Authentication Background Workers
 * ============================================================ */

/*
 * Auth Token Cleanup Worker
 *    Runs every 5 minutes to:
 *    - Remove expired tokens from cache
 *    - Update token statistics
 *    - Vacuum old audit log partitions
 */
void
orochi_auth_token_cleanup_worker_main(Datum main_arg)
{
    /* Worker initialization */
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("orochi", NULL, 0);

    while (!ShutdownRequestPending)
    {
        int rc = WaitLatch(MyLatch,
                          WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                          OROCHI_AUTH_TOKEN_CLEANUP_SEC * 1000L,
                          PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        /* Cleanup expired tokens from shared memory cache */
        orochi_auth_cleanup_expired_tokens();

        /* Cleanup expired sessions */
        orochi_auth_cleanup_expired_sessions();

        /* Process token revocation queue */
        orochi_auth_process_revocation_queue();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * Signing Key Rotation Worker
 *    Runs daily to:
 *    - Check for keys approaching expiration
 *    - Generate new signing keys
 *    - Update key cache in shared memory
 *    - Archive old keys (keep for validation period)
 */
void
orochi_auth_key_rotation_worker_main(Datum main_arg)
{
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("orochi", NULL, 0);

    while (!ShutdownRequestPending)
    {
        int rc = WaitLatch(MyLatch,
                          WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                          86400000L, /* 24 hours */
                          PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        /* Check and rotate signing keys */
        orochi_auth_check_key_rotation();

        /* Refresh signing key cache */
        orochi_auth_refresh_signing_key_cache();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * Session Management Worker
 *    Runs every minute to:
 *    - Update idle session states
 *    - Enforce session limits
 *    - Send session expiry warnings
 */
void
orochi_auth_session_worker_main(Datum main_arg)
{
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("orochi", NULL, 0);

    while (!ShutdownRequestPending)
    {
        int rc = WaitLatch(MyLatch,
                          WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                          60000L, /* 1 minute */
                          PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        /* Update session activity states */
        orochi_auth_update_session_states();

        /* Enforce per-user session limits */
        orochi_auth_enforce_session_limits();

        /* Create audit log partition if needed */
        orochi_auth_ensure_audit_partitions();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * Audit Log Archival Worker
 *    Runs weekly to:
 *    - Archive old audit log partitions
 *    - Compress archived partitions
 *    - Export to S3 if configured
 */
void
orochi_auth_audit_archival_worker_main(Datum main_arg)
{
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("orochi", NULL, 0);

    while (!ShutdownRequestPending)
    {
        int rc = WaitLatch(MyLatch,
                          WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                          604800000L, /* 7 days */
                          PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        /* Archive old partitions */
        orochi_auth_archive_audit_partitions();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}
```

### 6.2 Shared Memory Initialization

```c
/* ============================================================
 * Shared Memory Setup
 * ============================================================ */

/* Global pointer to auth shared state */
OrochiAuthSharedState *AuthSharedState = NULL;

/*
 * orochi_auth_shmem_size
 *    Calculate shared memory requirements
 */
Size
orochi_auth_shmem_size(void)
{
    Size size = 0;

    size = add_size(size, sizeof(OrochiAuthSharedState));
    size = add_size(size, MAXALIGN(size));

    return size;
}

/*
 * orochi_auth_shmem_init
 *    Initialize auth shared memory structures
 */
void
orochi_auth_shmem_init(void)
{
    bool found;

    AuthSharedState = ShmemInitStruct("orochi_auth_shared_state",
                                       orochi_auth_shmem_size(),
                                       &found);

    if (!found)
    {
        /* First time initialization */
        memset(AuthSharedState, 0, sizeof(OrochiAuthSharedState));

        /* Initialize locks */
        AuthSharedState->main_lock = &(GetNamedLWLockTranche("orochi_auth")[0]);
        AuthSharedState->token_cache_lock = &(GetNamedLWLockTranche("orochi_auth")[1]);
        AuthSharedState->session_cache_lock = &(GetNamedLWLockTranche("orochi_auth")[2]);
        AuthSharedState->rate_limit_lock = &(GetNamedLWLockTranche("orochi_auth")[3]);
        AuthSharedState->signing_key_lock = &(GetNamedLWLockTranche("orochi_auth")[4]);

        AuthSharedState->startup_time = GetCurrentTimestamp();
        AuthSharedState->is_initialized = true;

        elog(LOG, "Orochi Auth shared memory initialized");
    }
}
```

### 6.3 Authentication Hook

```c
/* ============================================================
 * Authentication Hook (ClientAuthentication_hook)
 * ============================================================ */

static ClientAuthentication_hook_type prev_client_auth_hook = NULL;

/*
 * orochi_auth_client_authentication_hook
 *    Intercept client authentication for custom methods
 */
void
orochi_auth_client_authentication_hook(Port *port, int status)
{
    /* Call previous hook if exists */
    if (prev_client_auth_hook)
        prev_client_auth_hook(port, status);

    /* Only process successful authentications */
    if (status != STATUS_OK)
        return;

    /* Check for Orochi-specific authentication */
    if (port->hba->auth_method == uaPassword ||
        port->hba->auth_method == uaMD5 ||
        port->hba->auth_method == uaSCRAM)
    {
        /* Extract tenant and user info from connection options */
        const char *tenant_id = GetConfigOption("orochi_auth.tenant_id", true, false);
        const char *api_key = GetConfigOption("orochi_auth.api_key", true, false);

        if (api_key != NULL)
        {
            /* API key authentication */
            OrochiTokenValidation result = orochi_auth_validate_api_key(api_key);

            if (!result.is_valid)
            {
                ereport(FATAL,
                        (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                         errmsg("invalid API key")));
            }

            /* Set session variables */
            orochi_auth_set_session_context(&result);
        }
    }
}

/*
 * orochi_auth_set_session_context
 *    Set PostgreSQL session variables with auth context
 */
void
orochi_auth_set_session_context(OrochiTokenValidation *auth)
{
    /* Set user ID */
    SetConfigOption("orochi_auth.user_id", auth->user_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set tenant ID */
    SetConfigOption("orochi_auth.tenant_id", auth->tenant_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set session ID */
    SetConfigOption("orochi_auth.session_id", auth->session_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set role for RLS */
    SetConfigOption("orochi_auth.role", auth->roles[0],
                    PGC_USERSET, PGC_S_SESSION);
}
```

### 6.4 GUC Variables

```c
/* ============================================================
 * GUC Configuration Variables
 * ============================================================ */

/* JWT Configuration */
int     orochi_auth_access_token_ttl = 900;         /* 15 min */
int     orochi_auth_refresh_token_ttl = 604800;     /* 7 days */
char   *orochi_auth_jwt_secret = NULL;              /* Deprecated, use signing keys */
char   *orochi_auth_jwt_algorithm = "RS256";
int     orochi_auth_jwt_leeway = 60;                /* Clock skew tolerance */

/* Session Configuration */
int     orochi_auth_session_ttl = 86400;            /* 24 hours */
int     orochi_auth_max_sessions_per_user = 10;
bool    orochi_auth_single_session = false;
int     orochi_auth_idle_timeout = 3600;            /* 1 hour */

/* MFA Configuration */
bool    orochi_auth_mfa_required = false;
char   *orochi_auth_mfa_providers = "totp,sms";
int     orochi_auth_mfa_code_ttl = 300;             /* 5 min */
int     orochi_auth_mfa_max_attempts = 5;

/* Password Configuration */
int     orochi_auth_password_min_length = 8;
bool    orochi_auth_password_require_uppercase = true;
bool    orochi_auth_password_require_number = true;
bool    orochi_auth_password_require_special = false;
int     orochi_auth_password_hash_cost = 12;        /* Argon2 iterations */

/* Rate Limiting */
bool    orochi_auth_rate_limit_enabled = true;
int     orochi_auth_rate_limit_login = 5;           /* per minute */
int     orochi_auth_rate_limit_api = 100;           /* per minute */
int     orochi_auth_rate_limit_window = 60;         /* seconds */

/* Audit Configuration */
bool    orochi_auth_audit_enabled = true;
int     orochi_auth_audit_retention_days = 90;
bool    orochi_auth_audit_pii = false;              /* Log PII data */

/* Key Management */
int     orochi_auth_key_rotation_days = 90;
int     orochi_auth_key_overlap_days = 30;          /* Keep old keys valid */

/* Connection Pooler Auth */
bool    orochi_auth_pooler_enabled = true;
int     orochi_auth_pooler_token_ttl = 3600;        /* 1 hour */

/*
 * orochi_auth_define_gucs
 *    Register all authentication GUC variables
 */
static void
orochi_auth_define_gucs(void)
{
    /* JWT Configuration */
    DefineCustomIntVariable("orochi_auth.access_token_ttl",
                           "Access token time-to-live in seconds",
                           NULL,
                           &orochi_auth_access_token_ttl,
                           900, 60, 86400,
                           PGC_SIGHUP, 0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi_auth.refresh_token_ttl",
                           "Refresh token time-to-live in seconds",
                           NULL,
                           &orochi_auth_refresh_token_ttl,
                           604800, 3600, 2592000,
                           PGC_SIGHUP, 0,
                           NULL, NULL, NULL);

    DefineCustomStringVariable("orochi_auth.jwt_algorithm",
                              "JWT signing algorithm (RS256, ES256, HS256)",
                              NULL,
                              &orochi_auth_jwt_algorithm,
                              "RS256",
                              PGC_SIGHUP, 0,
                              orochi_auth_check_algorithm, NULL, NULL);

    /* Session Configuration */
    DefineCustomIntVariable("orochi_auth.max_sessions_per_user",
                           "Maximum concurrent sessions per user",
                           NULL,
                           &orochi_auth_max_sessions_per_user,
                           10, 1, 1000,
                           PGC_SIGHUP, 0,
                           NULL, NULL, NULL);

    DefineCustomBoolVariable("orochi_auth.mfa_required",
                            "Require MFA for all users",
                            NULL,
                            &orochi_auth_mfa_required,
                            false,
                            PGC_SIGHUP, 0,
                            NULL, NULL, NULL);

    /* Rate Limiting */
    DefineCustomBoolVariable("orochi_auth.rate_limit_enabled",
                            "Enable authentication rate limiting",
                            NULL,
                            &orochi_auth_rate_limit_enabled,
                            true,
                            PGC_SIGHUP, 0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi_auth.rate_limit_login",
                           "Maximum login attempts per minute",
                           NULL,
                           &orochi_auth_rate_limit_login,
                           5, 1, 100,
                           PGC_SIGHUP, 0,
                           NULL, NULL, NULL);

    /* Audit Configuration */
    DefineCustomBoolVariable("orochi_auth.audit_enabled",
                            "Enable authentication audit logging",
                            NULL,
                            &orochi_auth_audit_enabled,
                            true,
                            PGC_SIGHUP, 0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi_auth.audit_retention_days",
                           "Audit log retention period in days",
                           NULL,
                           &orochi_auth_audit_retention_days,
                           90, 7, 3650,
                           PGC_SIGHUP, 0,
                           NULL, NULL, NULL);

    /* Session variables (read by RLS policies) */
    DefineCustomStringVariable("orochi_auth.user_id",
                              "Current authenticated user ID",
                              NULL,
                              &orochi_auth_current_user_id,
                              "",
                              PGC_USERSET, GUC_NOT_IN_SAMPLE,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi_auth.tenant_id",
                              "Current tenant ID",
                              NULL,
                              &orochi_auth_current_tenant_id,
                              "",
                              PGC_USERSET, GUC_NOT_IN_SAMPLE,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi_auth.role",
                              "Current user role",
                              NULL,
                              &orochi_auth_current_role,
                              "anon",
                              PGC_USERSET, GUC_NOT_IN_SAMPLE,
                              NULL, NULL, NULL);
}
```

## 7. Security Architecture

### 7.1 Key Management

```
+------------------+     +-------------------+     +------------------+
|   JWT Signing    |     |   Encryption      |     |   HMAC Keys      |
|   Keys (RS256)   |     |   Keys (AES-256)  |     |   (SHA-256)      |
+--------+---------+     +--------+----------+     +--------+---------+
         |                        |                         |
         v                        v                         v
+--------+---------+     +--------+----------+     +--------+---------+
| Access Tokens    |     | Secrets Vault     |     | API Key Hashing  |
| Refresh Tokens   |     | Password Storage  |     | Token Hashing    |
| ID Tokens        |     | API Key Secrets   |     | Session IDs      |
+------------------+     +-------------------+     +------------------+
```

**Key Hierarchy:**

1. **Master Key** (stored in HSM or Vault)
   - Used to encrypt all other keys
   - Never exposed to application layer

2. **Data Encryption Keys (DEK)**
   - Rotate every 30 days
   - Encrypted by master key

3. **JWT Signing Keys**
   - RS256: 2048-bit RSA key pairs
   - ES256: P-256 ECDSA key pairs
   - Rotate every 90 days with 30-day overlap

### 7.2 Secret Rotation Procedures

```sql
-- ============================================================
-- Key Rotation Functions
-- ============================================================

-- Generate new signing key pair
CREATE OR REPLACE FUNCTION orochi_auth.rotate_signing_key(
    p_tenant_id UUID
) RETURNS UUID AS $$
DECLARE
    v_key_id UUID;
    v_key_pair RECORD;
BEGIN
    -- Generate new RSA key pair
    SELECT * INTO v_key_pair FROM orochi_auth._generate_rsa_keypair(2048);

    -- Mark old keys as non-current
    UPDATE orochi_auth.signing_keys
    SET is_current = FALSE,
        rotated_at = NOW()
    WHERE tenant_id = p_tenant_id AND is_current = TRUE;

    -- Insert new key
    INSERT INTO orochi_auth.signing_keys (
        tenant_id, key_id, algorithm, public_key,
        private_key_encrypted, is_current, version
    )
    VALUES (
        p_tenant_id,
        'kid_' || encode(gen_random_bytes(16), 'hex'),
        'RS256',
        v_key_pair.public_key,
        orochi_auth._encrypt_secret(v_key_pair.private_key),
        TRUE,
        (SELECT COALESCE(MAX(version), 0) + 1
         FROM orochi_auth.signing_keys
         WHERE tenant_id = p_tenant_id)
    )
    RETURNING id INTO v_key_id;

    -- Notify cache refresh
    PERFORM pg_notify('orochi_auth_key_rotation',
                      json_build_object('tenant_id', p_tenant_id,
                                       'key_id', v_key_id)::text);

    -- Audit log
    PERFORM orochi_auth._audit_log(
        p_tenant_id, NULL, 'system',
        20, -- KEY_ROTATED
        NULL, NULL,
        'signing_key', v_key_id::text,
        NULL, json_build_object('key_id', v_key_id)
    );

    RETURN v_key_id;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

### 7.3 Compliance Considerations

**SOC2 Type II Requirements:**

| Control | Implementation |
|---------|---------------|
| CC6.1 - Logical Access | Role-based access control (RBAC) via PostgreSQL roles and RLS |
| CC6.2 - Authentication | MFA support, strong password policies, API key management |
| CC6.3 - User Management | User provisioning/deprovisioning audit trail |
| CC7.2 - Monitoring | Comprehensive audit logging with tamper protection |
| CC7.3 - Evaluation | Automated compliance checks via background workers |

**HIPAA Requirements:**

| Requirement | Implementation |
|-------------|---------------|
| Access Control | Per-tenant isolation, RLS policies |
| Audit Controls | Immutable audit log with encryption |
| Transmission Security | TLS 1.3 required, mTLS for internal |
| Authentication | MFA for PHI access, session timeouts |

**GDPR Requirements:**

| Requirement | Implementation |
|-------------|---------------|
| Right to Access | `orochi_auth.export_user_data()` function |
| Right to Erasure | `orochi_auth.delete_user()` with cascade |
| Data Portability | JSON export of all user data |
| Consent Management | Consent tracking in user metadata |

## 8. Performance and Scalability

### 8.1 Auth Caching Strategy

```
                    +-----------------------+
                    |    Shared Memory      |
                    |    Token Cache        |
                    |   (10,000 entries)    |
                    +----------+------------+
                               |
              +----------------+----------------+
              |                |                |
     +--------v------+  +------v-------+  +----v--------+
     |   Hot Path    |  |  Warm Path   |  |  Cold Path  |
     |   (< 1ms)     |  |   (< 5ms)    |  |  (< 50ms)   |
     +---------------+  +--------------+  +-------------+
     | - Token hash  |  | - Full JWT   |  | - Database  |
     |   lookup      |  |   validation |  |   lookup    |
     | - Expiry      |  | - Signature  |  | - Key fetch |
     |   check       |  |   verify     |  | - Session   |
     +---------------+  +--------------+  +-------------+
```

**Cache Eviction Policy:**
- LRU with TTL
- Immediate eviction on revocation (via pg_notify)
- Periodic cleanup by background worker

### 8.2 Token Validation Optimization

```c
/* ============================================================
 * Optimized Token Validation
 * ============================================================ */

/*
 * orochi_auth_validate_token_fast
 *    Optimized hot path for token validation
 *
 * Optimizations:
 *    1. SHA-256 hardware acceleration for hashing
 *    2. Lock-free read from token cache
 *    3. Batch signature verification
 *    4. SIMD-accelerated base64 decoding
 */
bool
orochi_auth_validate_token_fast(const char *token,
                                 int token_len,
                                 OrochiTokenValidation *result)
{
    char            token_hash[65];
    uint32_t        hash_slot;
    OrochiToken    *cached;

    /* Hardware-accelerated SHA-256 */
    orochi_sha256_fast(token, token_len, token_hash);

    /* Calculate cache slot (power-of-2 for fast modulo) */
    hash_slot = *(uint32_t *)token_hash % OROCHI_AUTH_MAX_CACHED_TOKENS;

    /* Lock-free read with memory barrier */
    pg_memory_barrier();
    cached = &AuthSharedState->token_cache[hash_slot];

    /* Fast comparison */
    if (likely(memcmp(cached->token_hash, token_hash, 64) == 0))
    {
        /* Validate expiry with cached timestamp */
        if (likely(cached->expires_at > GetCurrentTimestamp()))
        {
            if (likely(!cached->is_revoked))
            {
                /* Hot path: return cached result */
                result->is_valid = true;
                memcpy(result->user_id, cached->user_id, 37);
                memcpy(result->tenant_id, cached->tenant_id, 37);
                result->scopes = cached->scopes;
                result->expires_at = cached->expires_at;

                /* Increment stats atomically */
                pg_atomic_fetch_add_u64(&AuthSharedState->cache_hits, 1);

                return true;
            }
            result->is_revoked = true;
        }
        result->is_expired = true;
    }

    /* Cache miss - fall back to full validation */
    pg_atomic_fetch_add_u64(&AuthSharedState->cache_misses, 1);
    return false;
}
```

### 8.3 Connection Pooling Integration

```
                         +-----------------+
                         |   Application   |
                         +--------+--------+
                                  |
                         +--------v--------+
                         |   PgBouncer /   |
                         |   PgCat         |
                         +--------+--------+
                                  |
                    +-------------+-------------+
                    |                           |
           +--------v--------+         +--------v--------+
           |  Auth Endpoint  |         |  Auth Endpoint  |
           |  (Validates)    |         |  (Validates)    |
           +--------+--------+         +--------+--------+
                    |                           |
           +--------v--------+         +--------v--------+
           |  Connection     |         |  Connection     |
           |  with Context   |         |  with Context   |
           +--------+--------+         +--------+--------+
                    |                           |
                    +-------------+-------------+
                                  |
                         +--------v--------+
                         |   PostgreSQL    |
                         |   (Orochi)      |
                         +-----------------+
```

**Connection String Authentication:**

```
postgres://[user].[tenant_id]:[api_key]@pooler.orochi.db:5432/[database]?options=-c%20orochi_auth.token%3D[jwt_token]
```

### 8.4 High-Availability Patterns

```
                    +------------------+
                    |   Load Balancer  |
                    +--------+---------+
                             |
            +----------------+----------------+
            |                |                |
    +-------v------+  +------v-------+  +-----v--------+
    |   Primary    |  |   Replica    |  |   Replica    |
    |   (Writes)   |  |   (Reads)    |  |   (Reads)    |
    +-------+------+  +------+-------+  +------+-------+
            |                |                |
            +----------------+----------------+
                             |
                    +--------v---------+
                    |   Shared Auth    |
                    |   Cache (Redis)  |
                    +------------------+
```

**Token Revocation Propagation:**
1. Revocation written to primary
2. pg_notify broadcasts to all nodes
3. Each node invalidates local cache
4. Redis cluster also invalidated (if configured)

## 9. SQL Function Interface

### 9.1 Core Authentication Functions

```sql
-- ============================================================
-- Authentication Functions
-- ============================================================

-- Sign up new user
CREATE OR REPLACE FUNCTION orochi_auth.sign_up(
    p_email TEXT,
    p_password TEXT,
    p_data JSONB DEFAULT '{}'
) RETURNS orochi_auth.auth_response AS $$
    SELECT orochi_auth._sign_up(p_email, p_password, p_data);
$$ LANGUAGE sql SECURITY DEFINER;

-- Sign in with email/password
CREATE OR REPLACE FUNCTION orochi_auth.sign_in_with_password(
    p_email TEXT,
    p_password TEXT
) RETURNS orochi_auth.auth_response AS $$
    SELECT orochi_auth._sign_in_password(p_email, p_password);
$$ LANGUAGE sql SECURITY DEFINER;

-- Sign in with OAuth provider
CREATE OR REPLACE FUNCTION orochi_auth.sign_in_with_oauth(
    p_provider TEXT,
    p_access_token TEXT,
    p_id_token TEXT DEFAULT NULL
) RETURNS orochi_auth.auth_response AS $$
    SELECT orochi_auth._sign_in_oauth(p_provider, p_access_token, p_id_token);
$$ LANGUAGE sql SECURITY DEFINER;

-- Refresh access token
CREATE OR REPLACE FUNCTION orochi_auth.refresh_token(
    p_refresh_token TEXT
) RETURNS orochi_auth.auth_response AS $$
    SELECT orochi_auth._refresh_token(p_refresh_token);
$$ LANGUAGE sql SECURITY DEFINER;

-- Sign out (revoke session)
CREATE OR REPLACE FUNCTION orochi_auth.sign_out()
RETURNS void AS $$
    SELECT orochi_auth._sign_out(
        current_setting('orochi_auth.session_id', TRUE)::UUID
    );
$$ LANGUAGE sql SECURITY DEFINER;

-- Verify token (returns user info)
CREATE OR REPLACE FUNCTION orochi_auth.verify_token(
    p_token TEXT
) RETURNS orochi_auth.token_info AS $$
    SELECT orochi_auth._verify_token(p_token);
$$ LANGUAGE sql SECURITY DEFINER;

-- Get current user
CREATE OR REPLACE FUNCTION orochi_auth.uid()
RETURNS UUID AS $$
    SELECT NULLIF(current_setting('orochi_auth.user_id', TRUE), '')::UUID;
$$ LANGUAGE sql STABLE;

-- Get current role
CREATE OR REPLACE FUNCTION orochi_auth.role()
RETURNS TEXT AS $$
    SELECT COALESCE(NULLIF(current_setting('orochi_auth.role', TRUE), ''), 'anon');
$$ LANGUAGE sql STABLE;

-- Get JWT claims
CREATE OR REPLACE FUNCTION orochi_auth.jwt()
RETURNS JSONB AS $$
    SELECT orochi_auth._get_jwt_claims();
$$ LANGUAGE sql STABLE;
```

### 9.2 User Management Functions

```sql
-- ============================================================
-- User Management Functions
-- ============================================================

-- Get user by ID
CREATE OR REPLACE FUNCTION orochi_auth.get_user(
    p_user_id UUID
) RETURNS orochi_auth.user_record AS $$
    SELECT * FROM orochi_auth._get_user(p_user_id);
$$ LANGUAGE sql SECURITY DEFINER;

-- Update user metadata
CREATE OR REPLACE FUNCTION orochi_auth.update_user(
    p_user_id UUID DEFAULT orochi_auth.uid(),
    p_email TEXT DEFAULT NULL,
    p_password TEXT DEFAULT NULL,
    p_user_metadata JSONB DEFAULT NULL,
    p_app_metadata JSONB DEFAULT NULL
) RETURNS orochi_auth.user_record AS $$
    SELECT orochi_auth._update_user(p_user_id, p_email, p_password,
                                     p_user_metadata, p_app_metadata);
$$ LANGUAGE sql SECURITY DEFINER;

-- Delete user
CREATE OR REPLACE FUNCTION orochi_auth.delete_user(
    p_user_id UUID
) RETURNS void AS $$
    SELECT orochi_auth._delete_user(p_user_id);
$$ LANGUAGE sql SECURITY DEFINER;

-- List users (admin only)
CREATE OR REPLACE FUNCTION orochi_auth.list_users(
    p_page INTEGER DEFAULT 1,
    p_per_page INTEGER DEFAULT 50,
    p_search TEXT DEFAULT NULL
) RETURNS SETOF orochi_auth.user_record AS $$
    SELECT * FROM orochi_auth._list_users(p_page, p_per_page, p_search);
$$ LANGUAGE sql SECURITY DEFINER;
```

### 9.3 MFA Functions

```sql
-- ============================================================
-- MFA Functions
-- ============================================================

-- Enroll TOTP factor
CREATE OR REPLACE FUNCTION orochi_auth.mfa_enroll_totp(
    p_friendly_name TEXT DEFAULT 'Authenticator'
) RETURNS orochi_auth.mfa_enrollment AS $$
    SELECT orochi_auth._mfa_enroll_totp(orochi_auth.uid(), p_friendly_name);
$$ LANGUAGE sql SECURITY DEFINER;

-- Verify TOTP and complete enrollment
CREATE OR REPLACE FUNCTION orochi_auth.mfa_verify_totp(
    p_factor_id UUID,
    p_code TEXT
) RETURNS orochi_auth.mfa_verify_response AS $$
    SELECT orochi_auth._mfa_verify_totp(p_factor_id, p_code);
$$ LANGUAGE sql SECURITY DEFINER;

-- Challenge MFA (during login)
CREATE OR REPLACE FUNCTION orochi_auth.mfa_challenge(
    p_factor_id UUID
) RETURNS orochi_auth.mfa_challenge_response AS $$
    SELECT orochi_auth._mfa_challenge(p_factor_id);
$$ LANGUAGE sql SECURITY DEFINER;

-- Respond to MFA challenge
CREATE OR REPLACE FUNCTION orochi_auth.mfa_verify(
    p_challenge_id UUID,
    p_code TEXT
) RETURNS orochi_auth.auth_response AS $$
    SELECT orochi_auth._mfa_verify(p_challenge_id, p_code);
$$ LANGUAGE sql SECURITY DEFINER;

-- List user's MFA factors
CREATE OR REPLACE FUNCTION orochi_auth.mfa_list_factors()
RETURNS SETOF orochi_auth.mfa_factor_info AS $$
    SELECT * FROM orochi_auth._mfa_list_factors(orochi_auth.uid());
$$ LANGUAGE sql SECURITY DEFINER;

-- Unenroll MFA factor
CREATE OR REPLACE FUNCTION orochi_auth.mfa_unenroll(
    p_factor_id UUID
) RETURNS void AS $$
    SELECT orochi_auth._mfa_unenroll(orochi_auth.uid(), p_factor_id);
$$ LANGUAGE sql SECURITY DEFINER;
```

### 9.4 API Key Functions

```sql
-- ============================================================
-- API Key Functions
-- ============================================================

-- Create API key
CREATE OR REPLACE FUNCTION orochi_auth.create_api_key(
    p_name TEXT,
    p_scopes TEXT[] DEFAULT '{}',
    p_expires_at TIMESTAMPTZ DEFAULT NULL
) RETURNS orochi_auth.api_key_response AS $$
    SELECT orochi_auth._create_api_key(
        orochi_auth.uid(),
        current_setting('orochi_auth.tenant_id')::UUID,
        p_name, p_scopes, p_expires_at
    );
$$ LANGUAGE sql SECURITY DEFINER;

-- List API keys
CREATE OR REPLACE FUNCTION orochi_auth.list_api_keys()
RETURNS SETOF orochi_auth.api_key_info AS $$
    SELECT * FROM orochi_auth._list_api_keys(orochi_auth.uid());
$$ LANGUAGE sql SECURITY DEFINER;

-- Revoke API key
CREATE OR REPLACE FUNCTION orochi_auth.revoke_api_key(
    p_key_id UUID
) RETURNS void AS $$
    SELECT orochi_auth._revoke_api_key(orochi_auth.uid(), p_key_id);
$$ LANGUAGE sql SECURITY DEFINER;

-- Validate API key (internal use)
CREATE OR REPLACE FUNCTION orochi_auth.validate_api_key(
    p_api_key TEXT
) RETURNS orochi_auth.api_key_validation AS $$
    SELECT orochi_auth._validate_api_key(p_api_key);
$$ LANGUAGE sql SECURITY DEFINER;
```

### 9.5 Admin Functions

```sql
-- ============================================================
-- Admin Functions
-- ============================================================

-- Create user (admin)
CREATE OR REPLACE FUNCTION orochi_auth.admin_create_user(
    p_email TEXT,
    p_password TEXT DEFAULT NULL,
    p_user_metadata JSONB DEFAULT '{}',
    p_app_metadata JSONB DEFAULT '{}',
    p_email_confirmed BOOLEAN DEFAULT FALSE
) RETURNS orochi_auth.user_record AS $$
    SELECT orochi_auth._admin_create_user(
        current_setting('orochi_auth.tenant_id')::UUID,
        p_email, p_password, p_user_metadata, p_app_metadata, p_email_confirmed
    );
$$ LANGUAGE sql SECURITY DEFINER;

-- Update user (admin)
CREATE OR REPLACE FUNCTION orochi_auth.admin_update_user(
    p_user_id UUID,
    p_email TEXT DEFAULT NULL,
    p_password TEXT DEFAULT NULL,
    p_user_metadata JSONB DEFAULT NULL,
    p_app_metadata JSONB DEFAULT NULL,
    p_ban_duration INTERVAL DEFAULT NULL
) RETURNS orochi_auth.user_record AS $$
    SELECT orochi_auth._admin_update_user(
        p_user_id, p_email, p_password,
        p_user_metadata, p_app_metadata, p_ban_duration
    );
$$ LANGUAGE sql SECURITY DEFINER;

-- Delete user (admin)
CREATE OR REPLACE FUNCTION orochi_auth.admin_delete_user(
    p_user_id UUID
) RETURNS void AS $$
    SELECT orochi_auth._admin_delete_user(p_user_id);
$$ LANGUAGE sql SECURITY DEFINER;

-- Invite user by email
CREATE OR REPLACE FUNCTION orochi_auth.admin_invite_user(
    p_email TEXT,
    p_data JSONB DEFAULT '{}'
) RETURNS orochi_auth.invite_response AS $$
    SELECT orochi_auth._admin_invite_user(
        current_setting('orochi_auth.tenant_id')::UUID,
        p_email, p_data
    );
$$ LANGUAGE sql SECURITY DEFINER;

-- Generate link (magic link, password reset, etc.)
CREATE OR REPLACE FUNCTION orochi_auth.admin_generate_link(
    p_type TEXT,  -- 'signup', 'invite', 'recovery', 'magic_link'
    p_email TEXT,
    p_redirect_to TEXT DEFAULT NULL
) RETURNS orochi_auth.link_response AS $$
    SELECT orochi_auth._admin_generate_link(
        current_setting('orochi_auth.tenant_id')::UUID,
        p_type, p_email, p_redirect_to
    );
$$ LANGUAGE sql SECURITY DEFINER;
```

## 10. REST API Endpoint Structure

### 10.1 Authentication Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/auth/v1/signup` | Register new user |
| POST | `/auth/v1/token?grant_type=password` | Sign in with password |
| POST | `/auth/v1/token?grant_type=refresh_token` | Refresh token |
| POST | `/auth/v1/token?grant_type=id_token` | OAuth sign in |
| POST | `/auth/v1/logout` | Sign out |
| GET | `/auth/v1/user` | Get current user |
| PUT | `/auth/v1/user` | Update current user |
| POST | `/auth/v1/recover` | Password recovery |
| POST | `/auth/v1/verify` | Verify email/phone |
| POST | `/auth/v1/otp` | Send OTP |
| POST | `/auth/v1/magiclink` | Send magic link |

### 10.2 MFA Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/auth/v1/factors` | Enroll MFA factor |
| GET | `/auth/v1/factors` | List MFA factors |
| DELETE | `/auth/v1/factors/{factor_id}` | Unenroll factor |
| POST | `/auth/v1/factors/{factor_id}/challenge` | Create challenge |
| POST | `/auth/v1/factors/{factor_id}/verify` | Verify challenge |

### 10.3 Admin Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/auth/v1/admin/users` | List users |
| GET | `/auth/v1/admin/users/{user_id}` | Get user |
| POST | `/auth/v1/admin/users` | Create user |
| PUT | `/auth/v1/admin/users/{user_id}` | Update user |
| DELETE | `/auth/v1/admin/users/{user_id}` | Delete user |
| POST | `/auth/v1/admin/invite` | Invite user |
| POST | `/auth/v1/admin/generate_link` | Generate link |

### 10.4 API Key Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/auth/v1/api-keys` | List API keys |
| POST | `/auth/v1/api-keys` | Create API key |
| DELETE | `/auth/v1/api-keys/{key_id}` | Revoke API key |

## 11. Implementation Roadmap

### Phase 1: Core Infrastructure (Weeks 1-4)
- [ ] Schema creation and migrations
- [ ] Shared memory structures
- [ ] Background worker framework
- [ ] GUC variables
- [ ] Basic token validation

### Phase 2: Authentication Methods (Weeks 5-8)
- [ ] Password authentication with Argon2id
- [ ] JWT token generation and validation
- [ ] Refresh token rotation
- [ ] Session management
- [ ] API key authentication

### Phase 3: Advanced Features (Weeks 9-12)
- [ ] MFA (TOTP, SMS)
- [ ] OAuth provider integration
- [ ] Rate limiting
- [ ] Audit logging
- [ ] Key rotation

### Phase 4: Enterprise Features (Weeks 13-16)
- [ ] SAML integration
- [ ] Custom OAuth/OIDC
- [ ] Compliance reporting
- [ ] Connection pooler authentication
- [ ] Multi-tenant isolation

## 12. Testing Strategy

### Unit Tests
- Token validation (cache hit/miss)
- Password hashing and verification
- JWT encoding/decoding
- Rate limiting logic
- Session state transitions

### Integration Tests
- End-to-end authentication flows
- MFA enrollment and verification
- OAuth provider mocking
- Token refresh chains
- Concurrent session handling

### Performance Tests
- Token validation throughput (target: >100K/sec)
- Cache hit ratio (target: >95%)
- Authentication latency (target: <10ms p99)
- Shared memory pressure testing

### Security Tests
- JWT signature verification
- Token revocation propagation
- SQL injection prevention
- Rate limit bypass attempts
- Timing attack resistance

---

## Appendix A: Error Codes

| Code | Description |
|------|-------------|
| `invalid_credentials` | Email or password incorrect |
| `email_not_confirmed` | Email needs verification |
| `user_not_found` | User does not exist |
| `user_already_exists` | Email already registered |
| `invalid_token` | Token malformed or expired |
| `token_revoked` | Token has been revoked |
| `session_expired` | Session has expired |
| `mfa_required` | MFA verification needed |
| `mfa_invalid_code` | Incorrect MFA code |
| `rate_limit_exceeded` | Too many requests |
| `weak_password` | Password does not meet policy |
| `provider_error` | OAuth provider error |

## Appendix B: Metrics and Monitoring

### Prometheus Metrics

```
orochi_auth_requests_total{method,status}
orochi_auth_token_validations_total{cache_hit}
orochi_auth_login_attempts_total{method,result}
orochi_auth_mfa_challenges_total{type,result}
orochi_auth_session_count{state}
orochi_auth_token_cache_size
orochi_auth_rate_limit_hits_total
orochi_auth_key_rotation_age_seconds
```
