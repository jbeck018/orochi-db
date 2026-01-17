# Orochi DB Authentication System Design

## Technical Architecture for Supabase-Style Auth Features

**Version**: 1.0.0
**Status**: Design Document
**Author**: Architecture Team
**Date**: 2026-01-16

---

## Table of Contents

1. [Overview](#overview)
2. [GoTrue-based Auth Service Architecture](#1-gotrue-based-auth-service-architecture)
3. [JWT Integration with PostgreSQL](#2-jwt-integration-with-postgresql)
4. [Anonymous Auth with Upgradable Accounts](#3-anonymous-auth-with-upgradable-accounts)
5. [Magic Links and Phone OTP](#4-magic-links-and-phone-otp)
6. [Auth Hooks (Webhooks)](#5-auth-hooks-webhooks)
7. [Custom JWT Claims](#6-custom-jwt-claims)
8. [Direct RLS Integration](#7-direct-rls-integration)
9. [Implementation Notes](#8-implementation-notes)

---

## Overview

This document specifies the authentication system for Orochi DB, providing Supabase-compatible authentication features that integrate deeply with PostgreSQL. The design prioritizes:

- **Database-native integration**: Auth functions accessible directly in SQL
- **Zero-copy JWT validation**: Validate tokens without external service calls
- **RLS-first security**: Row Level Security as the primary access control mechanism
- **Horizontal scalability**: Stateless JWT validation across distributed nodes

### Architecture Diagram

```
+------------------+     +-------------------+     +------------------+
|   Client Apps    |     |   Auth Service    |     |   PostgreSQL     |
|  (Web/Mobile)    |     |   (GoTrue API)    |     |   (Orochi DB)    |
+--------+---------+     +--------+----------+     +--------+---------+
         |                        |                         |
         | 1. Auth Request        |                         |
         +----------------------->|                         |
         |                        | 2. Validate/Create      |
         |                        +------------------------>|
         |                        |                         |
         |                        | 3. Return JWT           |
         |<-----------------------+                         |
         |                        |                         |
         | 4. API Request + JWT   |                         |
         +------------------------------------------------->|
         |                        |                         |
         |                        |    5. Validate JWT      |
         |                        |    6. Set auth context  |
         |                        |    7. Execute with RLS  |
         |<-------------------------------------------------+
         |                        |                         |
```

---

## 1. GoTrue-based Auth Service Architecture

### 1.1 Database Schema

The `auth` schema contains all authentication-related tables with appropriate isolation from user data.

```sql
-- ============================================================
-- Auth Schema Setup
-- ============================================================

-- Create auth schema (separate from orochi schema)
CREATE SCHEMA IF NOT EXISTS auth;

-- Grant usage to authenticated role
GRANT USAGE ON SCHEMA auth TO authenticated;
GRANT USAGE ON SCHEMA auth TO anon;
GRANT USAGE ON SCHEMA auth TO service_role;

-- ============================================================
-- Core Authentication Tables
-- ============================================================

-- Users table - Core user identity
CREATE TABLE auth.users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Instance identification (for multi-tenant)
    instance_id UUID,

    -- Authentication identifiers
    email TEXT UNIQUE,
    phone TEXT UNIQUE,

    -- Encrypted password (argon2id)
    encrypted_password TEXT,

    -- Email verification
    email_confirmed_at TIMESTAMPTZ,
    email_confirm_token TEXT,
    email_confirm_sent_at TIMESTAMPTZ,

    -- Phone verification
    phone_confirmed_at TIMESTAMPTZ,
    phone_confirm_token TEXT,
    phone_confirm_sent_at TIMESTAMPTZ,

    -- Password recovery
    recovery_token TEXT,
    recovery_sent_at TIMESTAMPTZ,

    -- Email change workflow
    email_change TEXT,
    email_change_token_new TEXT,
    email_change_token_current TEXT,
    email_change_confirm_status SMALLINT DEFAULT 0,
    email_change_sent_at TIMESTAMPTZ,

    -- Phone change workflow
    phone_change TEXT,
    phone_change_token TEXT,
    phone_change_sent_at TIMESTAMPTZ,

    -- Reauthentication
    reauthentication_token TEXT,
    reauthentication_sent_at TIMESTAMPTZ,

    -- User metadata (application-defined)
    raw_app_meta_data JSONB DEFAULT '{}'::jsonb,
    raw_user_meta_data JSONB DEFAULT '{}'::jsonb,

    -- Account status
    is_super_admin BOOLEAN DEFAULT FALSE,
    is_sso_user BOOLEAN DEFAULT FALSE,
    is_anonymous BOOLEAN DEFAULT FALSE,

    -- Role for RBAC
    role TEXT DEFAULT 'authenticated',

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    last_sign_in_at TIMESTAMPTZ,
    banned_until TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,

    -- Constraints
    CONSTRAINT users_email_or_phone_check CHECK (
        email IS NOT NULL OR phone IS NOT NULL OR is_anonymous = TRUE
    )
);

-- Indexes for users table
CREATE INDEX idx_users_instance_id ON auth.users(instance_id);
CREATE INDEX idx_users_email ON auth.users(email) WHERE email IS NOT NULL;
CREATE INDEX idx_users_phone ON auth.users(phone) WHERE phone IS NOT NULL;
CREATE INDEX idx_users_created_at ON auth.users(created_at);
CREATE INDEX idx_users_is_anonymous ON auth.users(is_anonymous) WHERE is_anonymous = TRUE;
CREATE INDEX idx_users_recovery_token ON auth.users(recovery_token)
    WHERE recovery_token IS NOT NULL;
CREATE INDEX idx_users_email_confirm_token ON auth.users(email_confirm_token)
    WHERE email_confirm_token IS NOT NULL;

-- Sessions table - Active user sessions
CREATE TABLE auth.sessions (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Session tokens
    refresh_token TEXT UNIQUE,
    access_token_hash TEXT,  -- SHA256 hash of current access token

    -- Session metadata
    user_agent TEXT,
    ip_address INET,

    -- AAL (Authenticator Assurance Level) for MFA
    aal auth.aal_level DEFAULT 'aal1',

    -- Factor used for authentication
    factor_id UUID REFERENCES auth.mfa_factors(id) ON DELETE SET NULL,

    -- AMR (Authentication Methods References)
    amr JSONB DEFAULT '[]'::jsonb,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    refreshed_at TIMESTAMPTZ DEFAULT NOW(),
    not_after TIMESTAMPTZ,  -- Session expiration (absolute)

    -- Tracking
    tag TEXT  -- Optional session tag for organization
);

-- Indexes for sessions
CREATE INDEX idx_sessions_user_id ON auth.sessions(user_id);
CREATE INDEX idx_sessions_refresh_token ON auth.sessions(refresh_token)
    WHERE refresh_token IS NOT NULL;
CREATE INDEX idx_sessions_created_at ON auth.sessions(created_at);
CREATE INDEX idx_sessions_not_after ON auth.sessions(not_after)
    WHERE not_after IS NOT NULL;

-- Refresh tokens table - Token rotation history
CREATE TABLE auth.refresh_tokens (
    id BIGSERIAL PRIMARY KEY,
    session_id UUID REFERENCES auth.sessions(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Token value (encrypted)
    token TEXT NOT NULL UNIQUE,

    -- Token family for rotation detection
    parent TEXT,  -- Previous token in rotation chain

    -- Status
    revoked BOOLEAN DEFAULT FALSE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Indexes for refresh tokens
CREATE INDEX idx_refresh_tokens_session_id ON auth.refresh_tokens(session_id);
CREATE INDEX idx_refresh_tokens_user_id ON auth.refresh_tokens(user_id);
CREATE INDEX idx_refresh_tokens_parent ON auth.refresh_tokens(parent)
    WHERE parent IS NOT NULL;
CREATE INDEX idx_refresh_tokens_token ON auth.refresh_tokens(token);

-- Identities table - External OAuth providers
CREATE TABLE auth.identities (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Provider information
    provider TEXT NOT NULL,
    provider_id TEXT NOT NULL,

    -- Identity data from provider
    identity_data JSONB NOT NULL DEFAULT '{}'::jsonb,

    -- Email from provider (may differ from auth.users.email)
    email TEXT GENERATED ALWAYS AS (identity_data->>'email') STORED,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    last_sign_in_at TIMESTAMPTZ,

    -- Unique constraint per provider
    CONSTRAINT identities_provider_provider_id_key UNIQUE (provider, provider_id)
);

-- Indexes for identities
CREATE INDEX idx_identities_user_id ON auth.identities(user_id);
CREATE INDEX idx_identities_email ON auth.identities(email) WHERE email IS NOT NULL;

-- MFA Factors table
CREATE TABLE auth.mfa_factors (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Factor type and status
    factor_type auth.factor_type NOT NULL,
    status auth.factor_status NOT NULL DEFAULT 'unverified',
    friendly_name TEXT,

    -- TOTP-specific
    secret TEXT,  -- Encrypted TOTP secret

    -- Phone-specific
    phone TEXT,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Indexes for MFA factors
CREATE INDEX idx_mfa_factors_user_id ON auth.mfa_factors(user_id);
CREATE INDEX idx_mfa_factors_user_id_status ON auth.mfa_factors(user_id, status);

-- MFA Challenges table
CREATE TABLE auth.mfa_challenges (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    factor_id UUID NOT NULL REFERENCES auth.mfa_factors(id) ON DELETE CASCADE,

    -- Challenge details
    ip_address INET,
    otp_code TEXT,  -- For phone/email OTP (encrypted)

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    verified_at TIMESTAMPTZ,
    expires_at TIMESTAMPTZ NOT NULL DEFAULT (NOW() + INTERVAL '5 minutes')
);

-- Index for MFA challenges
CREATE INDEX idx_mfa_challenges_factor_id ON auth.mfa_challenges(factor_id);
CREATE INDEX idx_mfa_challenges_expires_at ON auth.mfa_challenges(expires_at);

-- Audit log table
CREATE TABLE auth.audit_log_entries (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    instance_id UUID,

    -- Audit information
    payload JSONB NOT NULL,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    ip_address INET
);

-- Index for audit logs
CREATE INDEX idx_audit_log_instance ON auth.audit_log_entries(instance_id);
CREATE INDEX idx_audit_log_created ON auth.audit_log_entries(created_at);

-- Partitioning for audit logs (by month)
-- Note: In production, implement time-based partitioning

-- ============================================================
-- Custom Types for Auth
-- ============================================================

-- AAL (Authenticator Assurance Level) per NIST SP 800-63B
CREATE TYPE auth.aal_level AS ENUM ('aal1', 'aal2', 'aal3');

-- MFA Factor types
CREATE TYPE auth.factor_type AS ENUM ('totp', 'phone', 'webauthn');

-- MFA Factor status
CREATE TYPE auth.factor_status AS ENUM ('unverified', 'verified');

-- Code challenge methods for PKCE
CREATE TYPE auth.code_challenge_method AS ENUM ('plain', 's256');

-- ============================================================
-- Flow State table for OAuth/PKCE flows
-- ============================================================

CREATE TABLE auth.flow_state (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID REFERENCES auth.users(id) ON DELETE CASCADE,

    -- OAuth flow
    auth_code TEXT UNIQUE,
    provider_type TEXT,
    provider_access_token TEXT,
    provider_refresh_token TEXT,

    -- PKCE
    code_challenge TEXT,
    code_challenge_method auth.code_challenge_method,

    -- State
    authentication_method TEXT,
    auth_code_issued_at TIMESTAMPTZ,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Index for flow state
CREATE INDEX idx_flow_state_auth_code ON auth.flow_state(auth_code)
    WHERE auth_code IS NOT NULL;
CREATE INDEX idx_flow_state_created_at ON auth.flow_state(created_at);

-- ============================================================
-- SAML Configuration (for Enterprise SSO)
-- ============================================================

CREATE TABLE auth.saml_providers (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- SSO domain
    sso_provider_id UUID NOT NULL,

    -- SAML configuration
    entity_id TEXT UNIQUE NOT NULL,
    metadata_xml TEXT NOT NULL,
    metadata_url TEXT,

    -- Attribute mapping
    attribute_mapping JSONB DEFAULT '{}'::jsonb,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE auth.saml_relay_states (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    sso_provider_id UUID NOT NULL,

    -- State management
    request_id TEXT NOT NULL,
    for_email TEXT,
    redirect_to TEXT,

    -- Flow state reference
    flow_state_id UUID REFERENCES auth.flow_state(id) ON DELETE CASCADE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Index for SAML relay states
CREATE INDEX idx_saml_relay_request_id ON auth.saml_relay_states(request_id);

-- ============================================================
-- SSO Providers (Organizational SSO)
-- ============================================================

CREATE TABLE auth.sso_providers (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Provider configuration
    resource_id TEXT UNIQUE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE auth.sso_domains (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    sso_provider_id UUID NOT NULL REFERENCES auth.sso_providers(id) ON DELETE CASCADE,

    -- Domain configuration
    domain TEXT NOT NULL,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),

    CONSTRAINT sso_domains_domain_unique UNIQUE (domain)
);

-- Index for SSO domains
CREATE INDEX idx_sso_domains_provider ON auth.sso_domains(sso_provider_id);
```

### 1.2 Auth Service API Endpoints

The auth service (GoTrue-compatible) exposes these endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/signup` | POST | Create new user account |
| `/token?grant_type=password` | POST | Login with email/password |
| `/token?grant_type=refresh_token` | POST | Refresh access token |
| `/token?grant_type=id_token` | POST | OAuth token exchange |
| `/logout` | POST | Sign out (revoke session) |
| `/user` | GET | Get current user |
| `/user` | PUT | Update user |
| `/recover` | POST | Send password recovery email |
| `/verify` | POST | Verify email/phone token |
| `/otp` | POST | Request OTP (magic link/phone) |
| `/magiclink` | POST | Send magic link email |
| `/factors` | GET/POST | List/create MFA factors |
| `/factors/{id}/verify` | POST | Verify MFA factor |
| `/factors/{id}/challenge` | POST | Create MFA challenge |
| `/sso/saml/acs` | POST | SAML Assertion Consumer Service |
| `/sso/saml/metadata` | GET | SAML metadata |
| `/authorize` | GET | OAuth authorization endpoint |
| `/callback` | GET | OAuth callback endpoint |

### 1.3 Service Configuration Table

```sql
-- Instance configuration
CREATE TABLE auth.config (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    instance_id UUID UNIQUE,

    -- JWT Configuration
    jwt_secret TEXT NOT NULL,
    jwt_exp INTEGER DEFAULT 3600,  -- 1 hour
    jwt_aud TEXT DEFAULT 'authenticated',

    -- Site configuration
    site_url TEXT NOT NULL,
    mailer_url_paths JSONB DEFAULT '{
        "confirmation": "/auth/v1/verify",
        "recovery": "/auth/v1/verify",
        "email_change": "/auth/v1/verify",
        "invite": "/auth/v1/verify"
    }'::jsonb,

    -- Email configuration
    smtp_host TEXT,
    smtp_port INTEGER DEFAULT 587,
    smtp_user TEXT,
    smtp_pass TEXT,  -- Encrypted
    smtp_sender TEXT,
    smtp_sender_name TEXT DEFAULT 'Orochi Auth',

    -- SMS configuration (Twilio/MessageBird)
    sms_provider TEXT,  -- 'twilio', 'messagebird', etc.
    sms_config JSONB DEFAULT '{}'::jsonb,  -- Provider-specific config (encrypted)

    -- Security settings
    password_min_length INTEGER DEFAULT 6,
    password_require_uppercase BOOLEAN DEFAULT FALSE,
    password_require_lowercase BOOLEAN DEFAULT FALSE,
    password_require_number BOOLEAN DEFAULT FALSE,
    password_require_special BOOLEAN DEFAULT FALSE,

    -- Feature flags
    disable_signup BOOLEAN DEFAULT FALSE,
    enable_signup_email_confirm BOOLEAN DEFAULT TRUE,
    enable_signup_phone_confirm BOOLEAN DEFAULT FALSE,
    enable_anonymous_sign_in BOOLEAN DEFAULT TRUE,
    enable_manual_linking BOOLEAN DEFAULT FALSE,
    enable_mfa BOOLEAN DEFAULT TRUE,

    -- Session configuration
    session_timebox INTEGER,  -- Max session lifetime in seconds
    session_inactivity_timeout INTEGER DEFAULT 86400,  -- 24 hours

    -- Rate limiting
    rate_limit_email_sent INTEGER DEFAULT 30,  -- Per hour
    rate_limit_sms_sent INTEGER DEFAULT 30,  -- Per hour
    rate_limit_token_refresh INTEGER DEFAULT 150,  -- Per hour

    -- OAuth providers configuration
    external_providers JSONB DEFAULT '[]'::jsonb,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);
```

---

## 2. JWT Integration with PostgreSQL

### 2.1 JWT Validation in PostgreSQL

The auth system validates JWTs directly in PostgreSQL without external calls.

```sql
-- ============================================================
-- JWT Types and Constants
-- ============================================================

-- Store JWT signing keys
CREATE TABLE auth.jwt_secrets (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    instance_id UUID,

    -- Key material
    secret TEXT NOT NULL,  -- HMAC secret or private key
    algorithm TEXT NOT NULL DEFAULT 'HS256',  -- HS256, RS256, ES256

    -- Key metadata
    key_id TEXT,  -- For key rotation (kid claim)
    is_current BOOLEAN DEFAULT TRUE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    expires_at TIMESTAMPTZ
);

-- ============================================================
-- auth.uid() - Get current authenticated user ID
-- ============================================================

CREATE OR REPLACE FUNCTION auth.uid()
RETURNS UUID
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT NULLIF(
        current_setting('request.jwt.claim.sub', TRUE),
        ''
    )::UUID;
$$;

COMMENT ON FUNCTION auth.uid() IS
'Returns the user ID from the current JWT. Returns NULL if not authenticated.';

-- ============================================================
-- auth.role() - Get current user role
-- ============================================================

CREATE OR REPLACE FUNCTION auth.role()
RETURNS TEXT
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT COALESCE(
        NULLIF(current_setting('request.jwt.claim.role', TRUE), ''),
        'anon'
    );
$$;

COMMENT ON FUNCTION auth.role() IS
'Returns the role from the current JWT. Returns ''anon'' if not authenticated.';

-- ============================================================
-- auth.jwt() - Get full JWT claims
-- ============================================================

CREATE OR REPLACE FUNCTION auth.jwt()
RETURNS JSONB
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT COALESCE(
        current_setting('request.jwt.claims', TRUE)::JSONB,
        '{}'::JSONB
    );
$$;

COMMENT ON FUNCTION auth.jwt() IS
'Returns all claims from the current JWT as JSONB.';

-- ============================================================
-- auth.email() - Get current user email
-- ============================================================

CREATE OR REPLACE FUNCTION auth.email()
RETURNS TEXT
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT NULLIF(
        current_setting('request.jwt.claim.email', TRUE),
        ''
    );
$$;

-- ============================================================
-- auth.aal() - Get Authenticator Assurance Level
-- ============================================================

CREATE OR REPLACE FUNCTION auth.aal()
RETURNS auth.aal_level
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT COALESCE(
        NULLIF(current_setting('request.jwt.claim.aal', TRUE), '')::auth.aal_level,
        'aal1'::auth.aal_level
    );
$$;

-- ============================================================
-- auth.session_id() - Get current session ID
-- ============================================================

CREATE OR REPLACE FUNCTION auth.session_id()
RETURNS UUID
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT NULLIF(
        current_setting('request.jwt.claim.session_id', TRUE),
        ''
    )::UUID;
$$;
```

### 2.2 C Implementation for JWT Validation

The following C code validates JWTs in PostgreSQL for optimal performance:

```c
/*-------------------------------------------------------------------------
 * src/auth/jwt.h
 * JWT validation header for Orochi DB
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JWT_H
#define OROCHI_JWT_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/jsonb.h"

/* JWT Algorithm types */
typedef enum OrochiJwtAlgorithm
{
    OROCHI_JWT_HS256 = 0,    /* HMAC-SHA256 */
    OROCHI_JWT_HS384,        /* HMAC-SHA384 */
    OROCHI_JWT_HS512,        /* HMAC-SHA512 */
    OROCHI_JWT_RS256,        /* RSA-SHA256 */
    OROCHI_JWT_RS384,        /* RSA-SHA384 */
    OROCHI_JWT_RS512,        /* RSA-SHA512 */
    OROCHI_JWT_ES256,        /* ECDSA-SHA256 */
    OROCHI_JWT_ES384,        /* ECDSA-SHA384 */
    OROCHI_JWT_ES512         /* ECDSA-SHA512 */
} OrochiJwtAlgorithm;

/* JWT validation result */
typedef struct OrochiJwtResult
{
    bool        is_valid;       /* Token passed validation */
    Jsonb      *header;         /* Decoded header */
    Jsonb      *payload;        /* Decoded payload */
    char       *error;          /* Error message if invalid */
    TimestampTz exp;            /* Expiration timestamp */
    TimestampTz iat;            /* Issued at timestamp */
    TimestampTz nbf;            /* Not before timestamp */
} OrochiJwtResult;

/* Function declarations */
extern OrochiJwtResult *orochi_jwt_decode(const char *token, const char *secret,
                                          OrochiJwtAlgorithm algorithm);
extern bool orochi_jwt_verify_signature(const char *header_payload,
                                        const char *signature,
                                        const char *secret,
                                        OrochiJwtAlgorithm algorithm);
extern char *orochi_base64url_decode(const char *input, int *output_len);
extern char *orochi_base64url_encode(const unsigned char *input, int input_len);

/* SQL function declarations */
extern Datum orochi_jwt_validate(PG_FUNCTION_ARGS);
extern Datum orochi_jwt_decode_claims(PG_FUNCTION_ARGS);
extern Datum orochi_jwt_get_claim(PG_FUNCTION_ARGS);

#endif /* OROCHI_JWT_H */
```

```c
/*-------------------------------------------------------------------------
 * src/auth/jwt.c
 * JWT validation implementation for Orochi DB
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"
#include "mb/pg_wchar.h"

#include "jwt.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <string.h>

PG_FUNCTION_INFO_V1(orochi_jwt_validate);
PG_FUNCTION_INFO_V1(orochi_jwt_decode_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_get_claim);
PG_FUNCTION_INFO_V1(orochi_jwt_set_auth_context);

/*
 * Base64URL decode (JWT uses URL-safe base64)
 */
char *
orochi_base64url_decode(const char *input, int *output_len)
{
    int input_len = strlen(input);
    int padding = (4 - (input_len % 4)) % 4;
    int padded_len = input_len + padding;
    char *padded = palloc(padded_len + 1);
    char *output;
    int i;

    /* Copy and convert URL-safe chars */
    for (i = 0; i < input_len; i++)
    {
        if (input[i] == '-')
            padded[i] = '+';
        else if (input[i] == '_')
            padded[i] = '/';
        else
            padded[i] = input[i];
    }

    /* Add padding */
    for (; i < padded_len; i++)
        padded[i] = '=';
    padded[padded_len] = '\0';

    /* Decode */
    *output_len = pg_b64_dec_len(padded_len);
    output = palloc(*output_len + 1);
    *output_len = pg_b64_decode(padded, padded_len, output, *output_len);
    output[*output_len] = '\0';

    pfree(padded);
    return output;
}

/*
 * Verify HMAC signature
 */
static bool
verify_hmac_signature(const char *header_payload, const char *signature,
                      const char *secret, OrochiJwtAlgorithm alg)
{
    const EVP_MD *md;
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;
    char *expected_sig;
    int sig_len;
    bool result;

    /* Select hash algorithm */
    switch (alg)
    {
        case OROCHI_JWT_HS256:
            md = EVP_sha256();
            break;
        case OROCHI_JWT_HS384:
            md = EVP_sha384();
            break;
        case OROCHI_JWT_HS512:
            md = EVP_sha512();
            break;
        default:
            return false;
    }

    /* Compute HMAC */
    if (!HMAC(md, secret, strlen(secret),
              (unsigned char *)header_payload, strlen(header_payload),
              hmac_result, &hmac_len))
        return false;

    /* Decode provided signature */
    expected_sig = orochi_base64url_decode(signature, &sig_len);

    /* Constant-time comparison */
    result = (sig_len == (int)hmac_len) &&
             (CRYPTO_memcmp(hmac_result, expected_sig, hmac_len) == 0);

    pfree(expected_sig);
    return result;
}

/*
 * orochi_jwt_decode
 * Decode and validate a JWT token
 */
OrochiJwtResult *
orochi_jwt_decode(const char *token, const char *secret,
                  OrochiJwtAlgorithm algorithm)
{
    OrochiJwtResult *result;
    char *token_copy;
    char *header_b64, *payload_b64, *signature;
    char *header_json, *payload_json;
    char *header_payload;
    int header_len, payload_len;
    Jsonb *header_jsonb, *payload_jsonb;
    JsonbValue *exp_value, *iat_value, *nbf_value;

    result = palloc0(sizeof(OrochiJwtResult));
    result->is_valid = false;

    /* Make a copy we can modify */
    token_copy = pstrdup(token);

    /* Split into three parts */
    header_b64 = token_copy;
    payload_b64 = strchr(header_b64, '.');
    if (!payload_b64)
    {
        result->error = "Invalid token format: missing payload separator";
        pfree(token_copy);
        return result;
    }
    *payload_b64++ = '\0';

    signature = strchr(payload_b64, '.');
    if (!signature)
    {
        result->error = "Invalid token format: missing signature separator";
        pfree(token_copy);
        return result;
    }
    *signature++ = '\0';

    /* Decode header and payload */
    header_json = orochi_base64url_decode(header_b64, &header_len);
    payload_json = orochi_base64url_decode(payload_b64, &payload_len);

    /* Parse JSON */
    header_jsonb = DatumGetJsonbP(
        DirectFunctionCall1(jsonb_in, CStringGetDatum(header_json)));
    payload_jsonb = DatumGetJsonbP(
        DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json)));

    result->header = header_jsonb;
    result->payload = payload_jsonb;

    /* Reconstruct header.payload for signature verification */
    header_payload = psprintf("%s.%s", header_b64, payload_b64);

    /* Verify signature */
    if (!orochi_jwt_verify_signature(header_payload, signature, secret, algorithm))
    {
        result->error = "Invalid signature";
        pfree(header_payload);
        pfree(token_copy);
        return result;
    }

    /* Check expiration */
    exp_value = findJsonbValueFromContainer(&payload_jsonb->root,
                                            JB_FOBJECT, "exp");
    if (exp_value && exp_value->type == jbvNumeric)
    {
        int64 exp_unix = DatumGetInt64(
            DirectFunctionCall1(numeric_int8,
                NumericGetDatum(exp_value->val.numeric)));
        result->exp = (TimestampTz)((exp_unix -
            ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) *
            USECS_PER_SEC);

        if (GetCurrentTimestamp() > result->exp)
        {
            result->error = "Token has expired";
            pfree(header_payload);
            pfree(token_copy);
            return result;
        }
    }

    /* Check not-before */
    nbf_value = findJsonbValueFromContainer(&payload_jsonb->root,
                                            JB_FOBJECT, "nbf");
    if (nbf_value && nbf_value->type == jbvNumeric)
    {
        int64 nbf_unix = DatumGetInt64(
            DirectFunctionCall1(numeric_int8,
                NumericGetDatum(nbf_value->val.numeric)));
        result->nbf = (TimestampTz)((nbf_unix -
            ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) *
            USECS_PER_SEC);

        if (GetCurrentTimestamp() < result->nbf)
        {
            result->error = "Token not yet valid";
            pfree(header_payload);
            pfree(token_copy);
            return result;
        }
    }

    result->is_valid = true;

    pfree(header_payload);
    pfree(token_copy);
    return result;
}

/*
 * orochi_jwt_validate SQL function
 * Validates a JWT and returns true/false
 */
Datum
orochi_jwt_validate(PG_FUNCTION_ARGS)
{
    text *token_text = PG_GETARG_TEXT_PP(0);
    text *secret_text = PG_GETARG_TEXT_PP(1);
    char *token = text_to_cstring(token_text);
    char *secret = text_to_cstring(secret_text);
    OrochiJwtResult *result;

    result = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);

    PG_RETURN_BOOL(result->is_valid);
}

/*
 * orochi_jwt_decode_claims SQL function
 * Decodes a JWT and returns the claims as JSONB
 */
Datum
orochi_jwt_decode_claims(PG_FUNCTION_ARGS)
{
    text *token_text = PG_GETARG_TEXT_PP(0);
    text *secret_text = PG_GETARG_TEXT_PP(1);
    char *token = text_to_cstring(token_text);
    char *secret = text_to_cstring(secret_text);
    OrochiJwtResult *result;

    result = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);

    if (!result->is_valid)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                 errmsg("JWT validation failed: %s", result->error)));

    PG_RETURN_JSONB_P(result->payload);
}

/*
 * orochi_jwt_set_auth_context SQL function
 * Sets session variables from JWT claims for use by auth.uid(), auth.jwt(), etc.
 */
Datum
orochi_jwt_set_auth_context(PG_FUNCTION_ARGS)
{
    text *token_text = PG_GETARG_TEXT_PP(0);
    text *secret_text = PG_GETARG_TEXT_PP(1);
    char *token = text_to_cstring(token_text);
    char *secret = text_to_cstring(secret_text);
    OrochiJwtResult *result;
    StringInfoData claims_str;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;

    result = orochi_jwt_decode(token, secret, OROCHI_JWT_HS256);

    if (!result->is_valid)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                 errmsg("JWT validation failed: %s", result->error)));

    /* Set the full claims object */
    initStringInfo(&claims_str);
    (void) JsonbToCString(&claims_str, &result->payload->root, VARSIZE(result->payload));

    SetConfigOption("request.jwt.claims", claims_str.data,
                    PGC_USERSET, PGC_S_SESSION);

    /* Extract and set individual claims for convenience */
    it = JsonbIteratorInit(&result->payload->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
    {
        if (r == WJB_KEY)
        {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            char *setting_name = psprintf("request.jwt.claim.%s", key);

            r = JsonbIteratorNext(&it, &v, false);
            if (r == WJB_VALUE)
            {
                char *value = NULL;

                switch (v.type)
                {
                    case jbvString:
                        value = pnstrdup(v.val.string.val, v.val.string.len);
                        break;
                    case jbvNumeric:
                        value = DatumGetCString(
                            DirectFunctionCall1(numeric_out,
                                NumericGetDatum(v.val.numeric)));
                        break;
                    case jbvBool:
                        value = v.val.boolean ? "true" : "false";
                        break;
                    default:
                        /* For complex types, stringify */
                        break;
                }

                if (value)
                    SetConfigOption(setting_name, value,
                                    PGC_USERSET, PGC_S_SESSION);
            }
        }
    }

    PG_RETURN_BOOL(true);
}
```

### 2.3 SQL Functions for JWT Context Setting

```sql
-- ============================================================
-- Set auth context from JWT (called by connection pooler/proxy)
-- ============================================================

CREATE OR REPLACE FUNCTION auth.set_request_jwt(token TEXT)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_secret TEXT;
    v_claims JSONB;
BEGIN
    -- Get JWT secret from config
    SELECT jwt_secret INTO v_secret
    FROM auth.config
    LIMIT 1;

    IF v_secret IS NULL THEN
        RAISE EXCEPTION 'JWT secret not configured';
    END IF;

    -- Validate and decode JWT
    SELECT orochi._jwt_decode_claims(token, v_secret) INTO v_claims;

    -- Set session variables
    PERFORM set_config('request.jwt.claims', v_claims::TEXT, TRUE);
    PERFORM set_config('request.jwt.claim.sub', v_claims->>'sub', TRUE);
    PERFORM set_config('request.jwt.claim.role', COALESCE(v_claims->>'role', 'authenticated'), TRUE);
    PERFORM set_config('request.jwt.claim.email', v_claims->>'email', TRUE);
    PERFORM set_config('request.jwt.claim.aal', COALESCE(v_claims->>'aal', 'aal1'), TRUE);
    PERFORM set_config('request.jwt.claim.session_id', v_claims->>'session_id', TRUE);

    -- Set app_metadata claims if present
    IF v_claims ? 'app_metadata' THEN
        PERFORM set_config('request.jwt.claim.app_metadata',
                          (v_claims->'app_metadata')::TEXT, TRUE);
    END IF;

    -- Set user_metadata claims if present
    IF v_claims ? 'user_metadata' THEN
        PERFORM set_config('request.jwt.claim.user_metadata',
                          (v_claims->'user_metadata')::TEXT, TRUE);
    END IF;
END;
$$;

-- ============================================================
-- Clear auth context (for connection pooling)
-- ============================================================

CREATE OR REPLACE FUNCTION auth.clear_request_jwt()
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
BEGIN
    PERFORM set_config('request.jwt.claims', '', TRUE);
    PERFORM set_config('request.jwt.claim.sub', '', TRUE);
    PERFORM set_config('request.jwt.claim.role', '', TRUE);
    PERFORM set_config('request.jwt.claim.email', '', TRUE);
    PERFORM set_config('request.jwt.claim.aal', '', TRUE);
    PERFORM set_config('request.jwt.claim.session_id', '', TRUE);
    PERFORM set_config('request.jwt.claim.app_metadata', '', TRUE);
    PERFORM set_config('request.jwt.claim.user_metadata', '', TRUE);
END;
$$;

-- ============================================================
-- JWT validation SQL wrapper
-- ============================================================

CREATE OR REPLACE FUNCTION auth.validate_jwt(token TEXT)
RETURNS TABLE(
    is_valid BOOLEAN,
    user_id UUID,
    role TEXT,
    email TEXT,
    expires_at TIMESTAMPTZ,
    claims JSONB
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_secret TEXT;
    v_claims JSONB;
    v_exp BIGINT;
BEGIN
    SELECT jwt_secret INTO v_secret FROM auth.config LIMIT 1;

    BEGIN
        SELECT orochi._jwt_decode_claims(token, v_secret) INTO v_claims;

        v_exp := (v_claims->>'exp')::BIGINT;

        RETURN QUERY SELECT
            TRUE,
            (v_claims->>'sub')::UUID,
            v_claims->>'role',
            v_claims->>'email',
            to_timestamp(v_exp),
            v_claims;
    EXCEPTION WHEN OTHERS THEN
        RETURN QUERY SELECT
            FALSE,
            NULL::UUID,
            NULL::TEXT,
            NULL::TEXT,
            NULL::TIMESTAMPTZ,
            NULL::JSONB;
    END;
END;
$$;
```

---

## 3. Anonymous Auth with Upgradable Accounts

### 3.1 Anonymous User Creation

```sql
-- ============================================================
-- Create anonymous user
-- ============================================================

CREATE OR REPLACE FUNCTION auth.create_anonymous_user(
    p_instance_id UUID DEFAULT NULL
)
RETURNS TABLE(
    user_id UUID,
    access_token TEXT,
    refresh_token TEXT,
    expires_at TIMESTAMPTZ
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_user_id UUID;
    v_session_id UUID;
    v_refresh_token TEXT;
    v_access_token TEXT;
    v_expires_at TIMESTAMPTZ;
    v_jwt_secret TEXT;
    v_jwt_exp INTEGER;
BEGIN
    -- Check if anonymous auth is enabled
    IF NOT EXISTS (
        SELECT 1 FROM auth.config
        WHERE enable_anonymous_sign_in = TRUE
        LIMIT 1
    ) THEN
        RAISE EXCEPTION 'Anonymous sign-in is not enabled';
    END IF;

    -- Get JWT config
    SELECT jwt_secret, jwt_exp
    INTO v_jwt_secret, v_jwt_exp
    FROM auth.config
    LIMIT 1;

    -- Create anonymous user
    INSERT INTO auth.users (
        instance_id,
        is_anonymous,
        role,
        raw_app_meta_data,
        raw_user_meta_data
    )
    VALUES (
        p_instance_id,
        TRUE,
        'authenticated',
        '{"provider": "anonymous", "providers": ["anonymous"]}'::jsonb,
        '{}'::jsonb
    )
    RETURNING id INTO v_user_id;

    -- Create session
    v_refresh_token := encode(gen_random_bytes(32), 'base64');
    v_expires_at := NOW() + (v_jwt_exp || ' seconds')::INTERVAL;

    INSERT INTO auth.sessions (
        user_id,
        refresh_token,
        aal,
        amr
    )
    VALUES (
        v_user_id,
        v_refresh_token,
        'aal1',
        '[{"method": "anonymous", "timestamp": ' ||
        extract(epoch from NOW())::BIGINT || '}]'
    )
    RETURNING id INTO v_session_id;

    -- Store refresh token
    INSERT INTO auth.refresh_tokens (session_id, user_id, token)
    VALUES (v_session_id, v_user_id, v_refresh_token);

    -- Generate access token (JWT)
    v_access_token := auth._generate_jwt(
        v_user_id,
        NULL,  -- No email for anonymous
        'authenticated',
        v_session_id,
        'aal1',
        v_jwt_exp,
        v_jwt_secret
    );

    RETURN QUERY SELECT v_user_id, v_access_token, v_refresh_token, v_expires_at;
END;
$$;
```

### 3.2 Account Upgrade/Linking

```sql
-- ============================================================
-- Upgrade anonymous user to permanent account
-- ============================================================

CREATE OR REPLACE FUNCTION auth.upgrade_anonymous_user(
    p_email TEXT DEFAULT NULL,
    p_phone TEXT DEFAULT NULL,
    p_password TEXT DEFAULT NULL,
    p_user_metadata JSONB DEFAULT '{}'::jsonb
)
RETURNS TABLE(
    user_id UUID,
    access_token TEXT,
    refresh_token TEXT,
    email TEXT,
    phone TEXT
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_user_id UUID;
    v_session_id UUID;
    v_old_refresh_token TEXT;
    v_new_refresh_token TEXT;
    v_access_token TEXT;
    v_jwt_secret TEXT;
    v_jwt_exp INTEGER;
    v_encrypted_password TEXT;
BEGIN
    -- Get current user from JWT context
    v_user_id := auth.uid();

    IF v_user_id IS NULL THEN
        RAISE EXCEPTION 'No authenticated user found';
    END IF;

    -- Verify user is anonymous
    IF NOT EXISTS (
        SELECT 1 FROM auth.users
        WHERE id = v_user_id AND is_anonymous = TRUE
    ) THEN
        RAISE EXCEPTION 'User is not an anonymous account';
    END IF;

    -- Validate that email or phone is provided
    IF p_email IS NULL AND p_phone IS NULL THEN
        RAISE EXCEPTION 'Email or phone is required for account upgrade';
    END IF;

    -- Check for email uniqueness
    IF p_email IS NOT NULL AND EXISTS (
        SELECT 1 FROM auth.users WHERE email = p_email AND id != v_user_id
    ) THEN
        RAISE EXCEPTION 'Email already in use';
    END IF;

    -- Check for phone uniqueness
    IF p_phone IS NOT NULL AND EXISTS (
        SELECT 1 FROM auth.users WHERE phone = p_phone AND id != v_user_id
    ) THEN
        RAISE EXCEPTION 'Phone number already in use';
    END IF;

    -- Hash password if provided
    IF p_password IS NOT NULL THEN
        v_encrypted_password := crypt(p_password, gen_salt('bf', 10));
    END IF;

    -- Get JWT config
    SELECT jwt_secret, jwt_exp INTO v_jwt_secret, v_jwt_exp
    FROM auth.config LIMIT 1;

    -- Update user record
    UPDATE auth.users
    SET
        email = COALESCE(p_email, email),
        phone = COALESCE(p_phone, phone),
        encrypted_password = COALESCE(v_encrypted_password, encrypted_password),
        is_anonymous = FALSE,
        raw_user_meta_data = raw_user_meta_data || p_user_metadata,
        raw_app_meta_data = raw_app_meta_data || jsonb_build_object(
            'provider', CASE
                WHEN p_email IS NOT NULL THEN 'email'
                ELSE 'phone'
            END,
            'providers', ARRAY[
                CASE WHEN p_email IS NOT NULL THEN 'email' END,
                CASE WHEN p_phone IS NOT NULL THEN 'phone' END,
                'anonymous'
            ]
        ),
        updated_at = NOW()
    WHERE id = v_user_id;

    -- Rotate refresh token
    SELECT s.id, s.refresh_token
    INTO v_session_id, v_old_refresh_token
    FROM auth.sessions s
    WHERE s.user_id = v_user_id
    ORDER BY s.created_at DESC
    LIMIT 1;

    v_new_refresh_token := encode(gen_random_bytes(32), 'base64');

    -- Update session
    UPDATE auth.sessions
    SET
        refresh_token = v_new_refresh_token,
        updated_at = NOW(),
        amr = amr || jsonb_build_array(jsonb_build_object(
            'method', CASE WHEN p_email IS NOT NULL THEN 'email' ELSE 'phone' END,
            'timestamp', extract(epoch from NOW())::BIGINT
        ))
    WHERE id = v_session_id;

    -- Mark old refresh token as revoked
    UPDATE auth.refresh_tokens
    SET revoked = TRUE, updated_at = NOW()
    WHERE token = v_old_refresh_token;

    -- Create new refresh token
    INSERT INTO auth.refresh_tokens (session_id, user_id, token, parent)
    VALUES (v_session_id, v_user_id, v_new_refresh_token, v_old_refresh_token);

    -- Generate new access token
    v_access_token := auth._generate_jwt(
        v_user_id,
        p_email,
        'authenticated',
        v_session_id,
        'aal1',
        v_jwt_exp,
        v_jwt_secret
    );

    -- Log audit event
    INSERT INTO auth.audit_log_entries (payload)
    VALUES (jsonb_build_object(
        'action', 'anonymous_upgrade',
        'actor_id', v_user_id,
        'actor_via_sso', FALSE,
        'log_type', 'user'
    ));

    RETURN QUERY SELECT
        v_user_id,
        v_access_token,
        v_new_refresh_token,
        p_email,
        p_phone;
END;
$$;

-- ============================================================
-- Link additional identity to existing account
-- ============================================================

CREATE OR REPLACE FUNCTION auth.link_identity(
    p_provider TEXT,
    p_provider_id TEXT,
    p_identity_data JSONB
)
RETURNS auth.identities
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_user_id UUID;
    v_identity auth.identities;
BEGIN
    -- Check if manual linking is enabled
    IF NOT EXISTS (
        SELECT 1 FROM auth.config WHERE enable_manual_linking = TRUE
    ) THEN
        RAISE EXCEPTION 'Manual identity linking is not enabled';
    END IF;

    v_user_id := auth.uid();

    IF v_user_id IS NULL THEN
        RAISE EXCEPTION 'No authenticated user found';
    END IF;

    -- Check if identity already exists
    IF EXISTS (
        SELECT 1 FROM auth.identities
        WHERE provider = p_provider AND provider_id = p_provider_id
    ) THEN
        RAISE EXCEPTION 'This identity is already linked to an account';
    END IF;

    -- Create identity link
    INSERT INTO auth.identities (
        user_id,
        provider,
        provider_id,
        identity_data,
        last_sign_in_at
    )
    VALUES (
        v_user_id,
        p_provider,
        p_provider_id,
        p_identity_data,
        NOW()
    )
    RETURNING * INTO v_identity;

    -- Update user's app metadata
    UPDATE auth.users
    SET raw_app_meta_data = raw_app_meta_data || jsonb_build_object(
        'providers', (
            SELECT array_agg(DISTINCT provider)
            FROM auth.identities
            WHERE user_id = v_user_id
        )
    ),
    updated_at = NOW()
    WHERE id = v_user_id;

    RETURN v_identity;
END;
$$;
```

### 3.3 Data Preservation During Upgrade

The anonymous upgrade mechanism preserves all user data because:

1. The same `user_id` (UUID) is maintained
2. All foreign key relationships remain intact
3. RLS policies using `auth.uid()` continue to work seamlessly

```sql
-- Example: User data table with proper FK
CREATE TABLE public.user_profiles (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
    display_name TEXT,
    avatar_url TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- RLS policy that works for both anonymous and upgraded users
ALTER TABLE public.user_profiles ENABLE ROW LEVEL SECURITY;

CREATE POLICY user_profiles_policy ON public.user_profiles
    FOR ALL
    USING (user_id = auth.uid())
    WITH CHECK (user_id = auth.uid());
```

---

## 4. Magic Links and Phone OTP

### 4.1 Token Generation and Storage

```sql
-- ============================================================
-- OTP/Token Types
-- ============================================================

CREATE TYPE auth.one_time_token_type AS ENUM (
    'confirmation_token',
    'recovery_token',
    'email_change_token_new',
    'email_change_token_current',
    'phone_change_token',
    'reauthentication_token'
);

-- ============================================================
-- One-time tokens table (for magic links, OTP)
-- ============================================================

CREATE TABLE auth.one_time_tokens (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Token details
    token_type auth.one_time_token_type NOT NULL,
    token_hash TEXT NOT NULL,  -- SHA256 hash of token

    -- For email tokens
    relates_to TEXT,  -- Email address or phone number

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    expires_at TIMESTAMPTZ NOT NULL,
    used_at TIMESTAMPTZ,

    -- Uniqueness per user per token type
    CONSTRAINT one_time_tokens_unique_user_type
        UNIQUE (user_id, token_type)
);

-- Index for token lookup
CREATE INDEX idx_one_time_tokens_hash ON auth.one_time_tokens(token_hash);
CREATE INDEX idx_one_time_tokens_expires ON auth.one_time_tokens(expires_at);

-- ============================================================
-- Magic link generation
-- ============================================================

CREATE OR REPLACE FUNCTION auth.send_magic_link(
    p_email TEXT,
    p_redirect_to TEXT DEFAULT NULL
)
RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_user auth.users;
    v_token TEXT;
    v_token_hash TEXT;
    v_site_url TEXT;
    v_mailer_url_paths JSONB;
    v_rate_limit INTEGER;
    v_recent_count INTEGER;
BEGIN
    -- Get config
    SELECT site_url, mailer_url_paths, rate_limit_email_sent
    INTO v_site_url, v_mailer_url_paths, v_rate_limit
    FROM auth.config LIMIT 1;

    -- Find or create user
    SELECT * INTO v_user FROM auth.users WHERE email = p_email;

    IF v_user IS NULL THEN
        -- Check if signup is disabled
        IF EXISTS (SELECT 1 FROM auth.config WHERE disable_signup = TRUE) THEN
            -- Return success to prevent email enumeration
            RETURN jsonb_build_object('message', 'If the email exists, a magic link has been sent');
        END IF;

        -- Create new user
        INSERT INTO auth.users (email, role, raw_app_meta_data)
        VALUES (
            p_email,
            'authenticated',
            '{"provider": "email", "providers": ["email"]}'::jsonb
        )
        RETURNING * INTO v_user;
    END IF;

    -- Rate limiting check
    SELECT COUNT(*) INTO v_recent_count
    FROM auth.one_time_tokens
    WHERE user_id = v_user.id
    AND token_type = 'confirmation_token'
    AND created_at > NOW() - INTERVAL '1 hour';

    IF v_recent_count >= v_rate_limit THEN
        RAISE EXCEPTION 'Too many requests. Please try again later.';
    END IF;

    -- Generate secure token
    v_token := encode(gen_random_bytes(32), 'hex');
    v_token_hash := encode(sha256(v_token::bytea), 'hex');

    -- Delete existing token for this user/type
    DELETE FROM auth.one_time_tokens
    WHERE user_id = v_user.id
    AND token_type = 'confirmation_token';

    -- Store token
    INSERT INTO auth.one_time_tokens (
        user_id,
        token_type,
        token_hash,
        relates_to,
        expires_at
    )
    VALUES (
        v_user.id,
        'confirmation_token',
        v_token_hash,
        p_email,
        NOW() + INTERVAL '1 hour'
    );

    -- Queue email notification (via pg_notify or webhook)
    PERFORM pg_notify('auth_email', jsonb_build_object(
        'type', 'magic_link',
        'email', p_email,
        'token', v_token,
        'redirect_to', COALESCE(p_redirect_to, v_site_url),
        'link', v_site_url || (v_mailer_url_paths->>'confirmation') ||
               '?token=' || v_token ||
               '&type=magiclink' ||
               COALESCE('&redirect_to=' || p_redirect_to, '')
    )::TEXT);

    -- Log audit
    INSERT INTO auth.audit_log_entries (payload)
    VALUES (jsonb_build_object(
        'action', 'magic_link_sent',
        'actor_id', v_user.id,
        'log_type', 'user'
    ));

    RETURN jsonb_build_object(
        'message', 'Magic link sent',
        'user_id', v_user.id
    );
END;
$$;

-- ============================================================
-- Phone OTP generation
-- ============================================================

CREATE OR REPLACE FUNCTION auth.send_phone_otp(
    p_phone TEXT,
    p_channel TEXT DEFAULT 'sms'  -- 'sms' or 'whatsapp'
)
RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_user auth.users;
    v_otp TEXT;
    v_otp_hash TEXT;
    v_rate_limit INTEGER;
    v_recent_count INTEGER;
BEGIN
    -- Get rate limit
    SELECT rate_limit_sms_sent INTO v_rate_limit
    FROM auth.config LIMIT 1;

    -- Find or create user
    SELECT * INTO v_user FROM auth.users WHERE phone = p_phone;

    IF v_user IS NULL THEN
        IF EXISTS (SELECT 1 FROM auth.config WHERE disable_signup = TRUE) THEN
            RETURN jsonb_build_object('message', 'If the phone exists, an OTP has been sent');
        END IF;

        INSERT INTO auth.users (phone, role, raw_app_meta_data)
        VALUES (
            p_phone,
            'authenticated',
            '{"provider": "phone", "providers": ["phone"]}'::jsonb
        )
        RETURNING * INTO v_user;
    END IF;

    -- Rate limiting
    SELECT COUNT(*) INTO v_recent_count
    FROM auth.one_time_tokens
    WHERE user_id = v_user.id
    AND token_type = 'confirmation_token'
    AND created_at > NOW() - INTERVAL '1 hour';

    IF v_recent_count >= v_rate_limit THEN
        RAISE EXCEPTION 'Too many requests. Please try again later.';
    END IF;

    -- Generate 6-digit OTP
    v_otp := lpad((floor(random() * 1000000)::int)::text, 6, '0');
    v_otp_hash := encode(sha256(v_otp::bytea), 'hex');

    -- Delete existing token
    DELETE FROM auth.one_time_tokens
    WHERE user_id = v_user.id
    AND token_type = 'confirmation_token';

    -- Store OTP
    INSERT INTO auth.one_time_tokens (
        user_id,
        token_type,
        token_hash,
        relates_to,
        expires_at
    )
    VALUES (
        v_user.id,
        'confirmation_token',
        v_otp_hash,
        p_phone,
        NOW() + INTERVAL '10 minutes'
    );

    -- Queue SMS notification
    PERFORM pg_notify('auth_sms', jsonb_build_object(
        'type', 'otp',
        'phone', p_phone,
        'otp', v_otp,
        'channel', p_channel
    )::TEXT);

    -- Log audit
    INSERT INTO auth.audit_log_entries (payload)
    VALUES (jsonb_build_object(
        'action', 'phone_otp_sent',
        'actor_id', v_user.id,
        'log_type', 'user'
    ));

    RETURN jsonb_build_object(
        'message', 'OTP sent',
        'message_id', gen_random_uuid()
    );
END;
$$;

-- ============================================================
-- Verify OTP/Magic Link token
-- ============================================================

CREATE OR REPLACE FUNCTION auth.verify_otp(
    p_token TEXT,
    p_type TEXT,  -- 'magiclink', 'signup', 'sms', 'phone_change', 'email_change'
    p_email TEXT DEFAULT NULL,
    p_phone TEXT DEFAULT NULL
)
RETURNS TABLE(
    user_id UUID,
    access_token TEXT,
    refresh_token TEXT,
    expires_at TIMESTAMPTZ
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_token_hash TEXT;
    v_token_record auth.one_time_tokens;
    v_user auth.users;
    v_session_id UUID;
    v_refresh_token TEXT;
    v_access_token TEXT;
    v_expires_at TIMESTAMPTZ;
    v_jwt_secret TEXT;
    v_jwt_exp INTEGER;
    v_token_type auth.one_time_token_type;
BEGIN
    -- Get JWT config
    SELECT jwt_secret, jwt_exp INTO v_jwt_secret, v_jwt_exp
    FROM auth.config LIMIT 1;

    -- Determine token type
    v_token_type := CASE p_type
        WHEN 'magiclink' THEN 'confirmation_token'
        WHEN 'signup' THEN 'confirmation_token'
        WHEN 'sms' THEN 'confirmation_token'
        WHEN 'phone_change' THEN 'phone_change_token'
        WHEN 'email_change' THEN 'email_change_token_new'
        ELSE 'confirmation_token'
    END;

    -- Hash the provided token
    v_token_hash := encode(sha256(p_token::bytea), 'hex');

    -- Find token
    SELECT * INTO v_token_record
    FROM auth.one_time_tokens
    WHERE token_hash = v_token_hash
    AND token_type = v_token_type
    AND expires_at > NOW()
    AND used_at IS NULL;

    IF v_token_record IS NULL THEN
        RAISE EXCEPTION 'Invalid or expired token';
    END IF;

    -- Get user
    SELECT * INTO v_user FROM auth.users WHERE id = v_token_record.user_id;

    -- Verify email/phone matches if provided
    IF p_email IS NOT NULL AND v_token_record.relates_to != p_email THEN
        RAISE EXCEPTION 'Token does not match email';
    END IF;

    IF p_phone IS NOT NULL AND v_token_record.relates_to != p_phone THEN
        RAISE EXCEPTION 'Token does not match phone';
    END IF;

    -- Mark token as used
    UPDATE auth.one_time_tokens
    SET used_at = NOW()
    WHERE id = v_token_record.id;

    -- Update user confirmation status
    IF p_type IN ('magiclink', 'signup') THEN
        UPDATE auth.users
        SET email_confirmed_at = COALESCE(email_confirmed_at, NOW()),
            last_sign_in_at = NOW(),
            updated_at = NOW()
        WHERE id = v_user.id;
    ELSIF p_type = 'sms' THEN
        UPDATE auth.users
        SET phone_confirmed_at = COALESCE(phone_confirmed_at, NOW()),
            last_sign_in_at = NOW(),
            updated_at = NOW()
        WHERE id = v_user.id;
    END IF;

    -- Create session
    v_refresh_token := encode(gen_random_bytes(32), 'base64');
    v_expires_at := NOW() + (v_jwt_exp || ' seconds')::INTERVAL;

    INSERT INTO auth.sessions (
        user_id,
        refresh_token,
        aal,
        amr
    )
    VALUES (
        v_user.id,
        v_refresh_token,
        'aal1',
        jsonb_build_array(jsonb_build_object(
            'method', CASE WHEN p_email IS NOT NULL THEN 'otp' ELSE 'sms_otp' END,
            'timestamp', extract(epoch from NOW())::BIGINT
        ))
    )
    RETURNING id INTO v_session_id;

    -- Store refresh token
    INSERT INTO auth.refresh_tokens (session_id, user_id, token)
    VALUES (v_session_id, v_user.id, v_refresh_token);

    -- Generate access token
    v_access_token := auth._generate_jwt(
        v_user.id,
        v_user.email,
        'authenticated',
        v_session_id,
        'aal1',
        v_jwt_exp,
        v_jwt_secret
    );

    RETURN QUERY SELECT v_user.id, v_access_token, v_refresh_token, v_expires_at;
END;
$$;
```

### 4.2 Email/SMS Delivery Integration

The auth system uses PostgreSQL's LISTEN/NOTIFY for delivery integration:

```sql
-- ============================================================
-- Email queue table (for reliable delivery)
-- ============================================================

CREATE TABLE auth.email_queue (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Recipient
    to_email TEXT NOT NULL,

    -- Email content
    template TEXT NOT NULL,  -- 'magic_link', 'confirm_signup', 'recovery', etc.
    template_data JSONB NOT NULL,

    -- Status tracking
    status TEXT DEFAULT 'pending',  -- pending, sent, failed
    attempts INTEGER DEFAULT 0,
    last_attempt_at TIMESTAMPTZ,
    error_message TEXT,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    sent_at TIMESTAMPTZ
);

-- Index for queue processing
CREATE INDEX idx_email_queue_status ON auth.email_queue(status, created_at)
    WHERE status = 'pending';

-- ============================================================
-- SMS queue table
-- ============================================================

CREATE TABLE auth.sms_queue (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Recipient
    to_phone TEXT NOT NULL,

    -- Message
    message TEXT NOT NULL,
    channel TEXT DEFAULT 'sms',  -- sms, whatsapp

    -- Status tracking
    status TEXT DEFAULT 'pending',
    message_sid TEXT,  -- Provider message ID
    attempts INTEGER DEFAULT 0,
    last_attempt_at TIMESTAMPTZ,
    error_message TEXT,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    sent_at TIMESTAMPTZ
);

-- Index for queue processing
CREATE INDEX idx_sms_queue_status ON auth.sms_queue(status, created_at)
    WHERE status = 'pending';

-- ============================================================
-- Delivery worker functions (called by external service)
-- ============================================================

CREATE OR REPLACE FUNCTION auth.process_email_queue(
    p_limit INTEGER DEFAULT 10
)
RETURNS SETOF auth.email_queue
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
BEGIN
    RETURN QUERY
    UPDATE auth.email_queue
    SET
        status = 'processing',
        attempts = attempts + 1,
        last_attempt_at = NOW()
    WHERE id IN (
        SELECT id FROM auth.email_queue
        WHERE status = 'pending'
        AND (attempts < 3 OR last_attempt_at < NOW() - INTERVAL '1 hour')
        ORDER BY created_at
        LIMIT p_limit
        FOR UPDATE SKIP LOCKED
    )
    RETURNING *;
END;
$$;

CREATE OR REPLACE FUNCTION auth.mark_email_sent(
    p_id UUID,
    p_success BOOLEAN,
    p_error TEXT DEFAULT NULL
)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
BEGIN
    UPDATE auth.email_queue
    SET
        status = CASE WHEN p_success THEN 'sent' ELSE 'failed' END,
        sent_at = CASE WHEN p_success THEN NOW() ELSE NULL END,
        error_message = p_error
    WHERE id = p_id;
END;
$$;
```

---

## 5. Auth Hooks (Webhooks)

### 5.1 Webhook Configuration

```sql
-- ============================================================
-- Webhook configuration table
-- ============================================================

CREATE TABLE auth.hooks (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Hook identification
    hook_name TEXT NOT NULL UNIQUE,

    -- Hook configuration
    hook_type TEXT NOT NULL,  -- 'pre_token_generation', 'send_sms', 'send_email', etc.

    -- Execution method
    execution_mode TEXT NOT NULL DEFAULT 'sync',  -- 'sync' or 'async'

    -- Target configuration
    target_type TEXT NOT NULL,  -- 'pg_function', 'http'

    -- For pg_function
    pg_schema TEXT,
    pg_function TEXT,

    -- For HTTP webhooks
    http_url TEXT,
    http_method TEXT DEFAULT 'POST',
    http_timeout_ms INTEGER DEFAULT 5000,
    http_headers JSONB DEFAULT '{}'::jsonb,

    -- Retry configuration (for async)
    max_retries INTEGER DEFAULT 3,
    retry_interval_seconds INTEGER DEFAULT 60,

    -- Secret for webhook signature (HMAC-SHA256)
    signing_secret TEXT,

    -- Status
    enabled BOOLEAN DEFAULT TRUE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- ============================================================
-- Supported hook types and events
-- ============================================================

CREATE TYPE auth.hook_event AS ENUM (
    -- User lifecycle
    'user.created',
    'user.updated',
    'user.deleted',
    'user.signed_in',
    'user.signed_out',

    -- Session events
    'session.created',
    'session.revoked',
    'session.refreshed',

    -- Authentication events
    'auth.token_refreshed',
    'auth.password_changed',
    'auth.email_changed',
    'auth.phone_changed',

    -- MFA events
    'mfa.factor_enrolled',
    'mfa.factor_verified',
    'mfa.factor_unenrolled',
    'mfa.challenge_created',
    'mfa.challenge_verified',

    -- Identity events
    'identity.linked',
    'identity.unlinked',

    -- Customization hooks (synchronous)
    'pre_token_generation',
    'custom_access_token',
    'send_sms',
    'send_email'
);

-- ============================================================
-- Webhook event subscriptions
-- ============================================================

CREATE TABLE auth.hook_subscriptions (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hook_id UUID NOT NULL REFERENCES auth.hooks(id) ON DELETE CASCADE,
    event_type auth.hook_event NOT NULL,

    -- Filters (optional)
    filter_expression JSONB,  -- JSON filter for event payload

    -- Status
    enabled BOOLEAN DEFAULT TRUE,

    UNIQUE (hook_id, event_type)
);

-- ============================================================
-- Webhook delivery log
-- ============================================================

CREATE TABLE auth.hook_logs (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hook_id UUID NOT NULL REFERENCES auth.hooks(id) ON DELETE CASCADE,

    -- Event information
    event_type auth.hook_event NOT NULL,
    event_payload JSONB NOT NULL,

    -- Delivery status
    status TEXT NOT NULL,  -- 'pending', 'delivered', 'failed'
    attempts INTEGER DEFAULT 0,

    -- Response information
    response_status INTEGER,
    response_body TEXT,
    response_time_ms INTEGER,

    -- Error information
    error_message TEXT,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    delivered_at TIMESTAMPTZ,
    next_retry_at TIMESTAMPTZ
);

-- Indexes for log management
CREATE INDEX idx_hook_logs_status ON auth.hook_logs(status, next_retry_at)
    WHERE status = 'pending';
CREATE INDEX idx_hook_logs_created ON auth.hook_logs(created_at);
CREATE INDEX idx_hook_logs_hook ON auth.hook_logs(hook_id, created_at DESC);
```

### 5.2 Webhook Payload Structures

```sql
-- ============================================================
-- Webhook payload generation function
-- ============================================================

CREATE OR REPLACE FUNCTION auth._build_webhook_payload(
    p_event_type auth.hook_event,
    p_user_id UUID DEFAULT NULL,
    p_session_id UUID DEFAULT NULL,
    p_additional_data JSONB DEFAULT '{}'::jsonb
)
RETURNS JSONB
LANGUAGE plpgsql
AS $$
DECLARE
    v_payload JSONB;
    v_user_data JSONB;
    v_session_data JSONB;
BEGIN
    -- Build base payload
    v_payload := jsonb_build_object(
        'type', 'webhook',
        'event', p_event_type,
        'timestamp', extract(epoch from NOW())::BIGINT,
        'webhook_id', gen_random_uuid()
    );

    -- Add user data if user_id provided
    IF p_user_id IS NOT NULL THEN
        SELECT jsonb_build_object(
            'id', u.id,
            'email', u.email,
            'phone', u.phone,
            'role', u.role,
            'is_anonymous', u.is_anonymous,
            'email_confirmed_at', u.email_confirmed_at,
            'phone_confirmed_at', u.phone_confirmed_at,
            'created_at', u.created_at,
            'updated_at', u.updated_at,
            'last_sign_in_at', u.last_sign_in_at,
            'app_metadata', u.raw_app_meta_data,
            'user_metadata', u.raw_user_meta_data
        )
        INTO v_user_data
        FROM auth.users u
        WHERE u.id = p_user_id;

        v_payload := v_payload || jsonb_build_object('user', v_user_data);
    END IF;

    -- Add session data if session_id provided
    IF p_session_id IS NOT NULL THEN
        SELECT jsonb_build_object(
            'id', s.id,
            'user_id', s.user_id,
            'aal', s.aal,
            'amr', s.amr,
            'created_at', s.created_at,
            'updated_at', s.updated_at
        )
        INTO v_session_data
        FROM auth.sessions s
        WHERE s.id = p_session_id;

        v_payload := v_payload || jsonb_build_object('session', v_session_data);
    END IF;

    -- Merge additional data
    v_payload := v_payload || p_additional_data;

    RETURN v_payload;
END;
$$;

-- ============================================================
-- Trigger webhook execution
-- ============================================================

CREATE OR REPLACE FUNCTION auth.trigger_webhook(
    p_event_type auth.hook_event,
    p_user_id UUID DEFAULT NULL,
    p_session_id UUID DEFAULT NULL,
    p_additional_data JSONB DEFAULT '{}'::jsonb
)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
DECLARE
    v_hook RECORD;
    v_payload JSONB;
    v_signing_payload TEXT;
    v_signature TEXT;
BEGIN
    -- Build payload
    v_payload := auth._build_webhook_payload(
        p_event_type, p_user_id, p_session_id, p_additional_data
    );

    -- Find subscribed hooks
    FOR v_hook IN
        SELECT h.*, hs.filter_expression
        FROM auth.hooks h
        JOIN auth.hook_subscriptions hs ON h.id = hs.hook_id
        WHERE hs.event_type = p_event_type
        AND h.enabled = TRUE
        AND hs.enabled = TRUE
    LOOP
        -- Apply filter if present
        IF v_hook.filter_expression IS NOT NULL THEN
            IF NOT v_payload @> v_hook.filter_expression THEN
                CONTINUE;
            END IF;
        END IF;

        IF v_hook.execution_mode = 'sync' AND v_hook.target_type = 'pg_function' THEN
            -- Execute PostgreSQL function synchronously
            EXECUTE format(
                'SELECT %I.%I($1)',
                v_hook.pg_schema,
                v_hook.pg_function
            ) USING v_payload;
        ELSE
            -- Queue for async execution
            INSERT INTO auth.hook_logs (
                hook_id,
                event_type,
                event_payload,
                status,
                next_retry_at
            )
            VALUES (
                v_hook.id,
                p_event_type,
                v_payload,
                'pending',
                NOW()
            );

            -- Notify webhook worker
            PERFORM pg_notify('auth_webhook', jsonb_build_object(
                'hook_id', v_hook.id,
                'event_type', p_event_type
            )::TEXT);
        END IF;
    END LOOP;
END;
$$;
```

### 5.3 Webhook Signature Verification

Webhooks include HMAC-SHA256 signatures for verification:

```
X-Orochi-Signature: sha256=<signature>
X-Orochi-Timestamp: <unix_timestamp>
X-Orochi-Webhook-Id: <webhook_uuid>
```

The signature is computed as:
```
signature = HMAC-SHA256(signing_secret, timestamp + "." + payload_json)
```

---

## 6. Custom JWT Claims

### 6.1 Custom Claims Storage

```sql
-- ============================================================
-- Custom claims configuration
-- ============================================================

CREATE TABLE auth.custom_claims_config (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    instance_id UUID,

    -- Claim definition
    claim_name TEXT NOT NULL,

    -- Source configuration
    source_type TEXT NOT NULL,  -- 'user_metadata', 'app_metadata', 'sql_function', 'static'
    source_path TEXT,  -- JSON path for metadata sources
    source_function TEXT,  -- For sql_function source
    static_value JSONB,  -- For static source

    -- Claim options
    include_in_access_token BOOLEAN DEFAULT TRUE,
    include_in_id_token BOOLEAN DEFAULT TRUE,

    -- Validation
    required BOOLEAN DEFAULT FALSE,

    -- Status
    enabled BOOLEAN DEFAULT TRUE,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),

    UNIQUE (instance_id, claim_name)
);

-- ============================================================
-- User-specific custom claims (overrides)
-- ============================================================

CREATE TABLE auth.user_claims (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,

    -- Claim data
    claim_name TEXT NOT NULL,
    claim_value JSONB NOT NULL,

    -- Timestamps
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),

    UNIQUE (user_id, claim_name)
);

-- Index for efficient lookup
CREATE INDEX idx_user_claims_user ON auth.user_claims(user_id);
```

### 6.2 JWT Generation with Custom Claims

```sql
-- ============================================================
-- Generate JWT with custom claims
-- ============================================================

CREATE OR REPLACE FUNCTION auth._generate_jwt(
    p_user_id UUID,
    p_email TEXT,
    p_role TEXT,
    p_session_id UUID,
    p_aal auth.aal_level,
    p_exp_seconds INTEGER,
    p_secret TEXT
)
RETURNS TEXT
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = auth, pg_temp
AS $$
DECLARE
    v_claims JSONB;
    v_user auth.users;
    v_custom_claims JSONB := '{}'::jsonb;
    v_config_claim RECORD;
    v_user_claim RECORD;
    v_claim_value JSONB;
    v_now BIGINT;
BEGIN
    v_now := extract(epoch from NOW())::BIGINT;

    -- Get user
    SELECT * INTO v_user FROM auth.users WHERE id = p_user_id;

    -- Build standard claims
    v_claims := jsonb_build_object(
        'aud', 'authenticated',
        'exp', v_now + p_exp_seconds,
        'iat', v_now,
        'iss', (SELECT site_url FROM auth.config LIMIT 1),
        'sub', p_user_id,
        'email', p_email,
        'phone', v_user.phone,
        'role', p_role,
        'aal', p_aal,
        'session_id', p_session_id,
        'amr', (
            SELECT amr FROM auth.sessions WHERE id = p_session_id
        ),
        'app_metadata', v_user.raw_app_meta_data,
        'user_metadata', v_user.raw_user_meta_data
    );

    -- Process configured custom claims
    FOR v_config_claim IN
        SELECT * FROM auth.custom_claims_config
        WHERE enabled = TRUE
        AND include_in_access_token = TRUE
    LOOP
        v_claim_value := NULL;

        CASE v_config_claim.source_type
            WHEN 'user_metadata' THEN
                v_claim_value := v_user.raw_user_meta_data #>
                    string_to_array(v_config_claim.source_path, '.');
            WHEN 'app_metadata' THEN
                v_claim_value := v_user.raw_app_meta_data #>
                    string_to_array(v_config_claim.source_path, '.');
            WHEN 'sql_function' THEN
                EXECUTE format('SELECT %s($1)', v_config_claim.source_function)
                    INTO v_claim_value
                    USING p_user_id;
            WHEN 'static' THEN
                v_claim_value := v_config_claim.static_value;
        END CASE;

        IF v_claim_value IS NOT NULL OR NOT v_config_claim.required THEN
            v_custom_claims := v_custom_claims ||
                jsonb_build_object(v_config_claim.claim_name, v_claim_value);
        END IF;
    END LOOP;

    -- Process user-specific claim overrides
    FOR v_user_claim IN
        SELECT * FROM auth.user_claims WHERE user_id = p_user_id
    LOOP
        v_custom_claims := v_custom_claims ||
            jsonb_build_object(v_user_claim.claim_name, v_user_claim.claim_value);
    END LOOP;

    -- Merge custom claims
    v_claims := v_claims || v_custom_claims;

    -- Fire pre_token_generation hook (allows claim modification)
    SELECT auth._execute_token_hook(v_claims, p_user_id, p_session_id)
    INTO v_claims;

    -- Generate and sign JWT
    RETURN orochi._jwt_encode(v_claims, p_secret, 'HS256');
END;
$$;

-- ============================================================
-- Pre-token generation hook execution
-- ============================================================

CREATE OR REPLACE FUNCTION auth._execute_token_hook(
    p_claims JSONB,
    p_user_id UUID,
    p_session_id UUID
)
RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
DECLARE
    v_hook auth.hooks;
    v_result JSONB;
BEGIN
    -- Find token generation hook
    SELECT h.* INTO v_hook
    FROM auth.hooks h
    JOIN auth.hook_subscriptions hs ON h.id = hs.hook_id
    WHERE hs.event_type = 'pre_token_generation'
    AND h.enabled = TRUE
    AND h.target_type = 'pg_function'
    LIMIT 1;

    IF v_hook IS NULL THEN
        RETURN p_claims;
    END IF;

    -- Execute hook function
    EXECUTE format(
        'SELECT %I.%I($1, $2, $3)',
        v_hook.pg_schema,
        v_hook.pg_function
    )
    INTO v_result
    USING p_claims, p_user_id, p_session_id;

    RETURN COALESCE(v_result, p_claims);
END;
$$;
```

### 6.3 Accessing Custom Claims in PostgreSQL

```sql
-- ============================================================
-- Get specific custom claim
-- ============================================================

CREATE OR REPLACE FUNCTION auth.get_claim(claim_name TEXT)
RETURNS JSONB
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT auth.jwt()->claim_name;
$$;

-- ============================================================
-- Check if user has specific claim value
-- ============================================================

CREATE OR REPLACE FUNCTION auth.has_claim(claim_name TEXT, claim_value JSONB)
RETURNS BOOLEAN
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT auth.jwt()->claim_name = claim_value;
$$;

-- ============================================================
-- Get array claim as text array
-- ============================================================

CREATE OR REPLACE FUNCTION auth.get_claim_array(claim_name TEXT)
RETURNS TEXT[]
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT ARRAY(
        SELECT jsonb_array_elements_text(auth.jwt()->claim_name)
    );
$$;

-- ============================================================
-- Check if array claim contains value
-- ============================================================

CREATE OR REPLACE FUNCTION auth.claim_contains(claim_name TEXT, value TEXT)
RETURNS BOOLEAN
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT auth.jwt()->claim_name ? value;
$$;
```

---

## 7. Direct RLS Integration

### 7.1 RLS Policy Patterns

```sql
-- ============================================================
-- Standard RLS Patterns for auth.uid()
-- ============================================================

-- Pattern 1: Simple owner-based access
-- User can only access their own rows

CREATE TABLE public.user_data (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id),
    content TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

ALTER TABLE public.user_data ENABLE ROW LEVEL SECURITY;

-- Single policy for all operations
CREATE POLICY user_data_owner_policy ON public.user_data
    FOR ALL
    USING (user_id = auth.uid())
    WITH CHECK (user_id = auth.uid());


-- Pattern 2: Role-based access
-- Different access levels based on JWT role claim

CREATE TABLE public.articles (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    author_id UUID NOT NULL REFERENCES auth.users(id),
    title TEXT NOT NULL,
    content TEXT,
    status TEXT DEFAULT 'draft',  -- draft, published, archived
    created_at TIMESTAMPTZ DEFAULT NOW()
);

ALTER TABLE public.articles ENABLE ROW LEVEL SECURITY;

-- Anyone can read published articles
CREATE POLICY articles_read_published ON public.articles
    FOR SELECT
    USING (status = 'published');

-- Authors can read their own articles (any status)
CREATE POLICY articles_read_own ON public.articles
    FOR SELECT
    USING (author_id = auth.uid());

-- Authors can insert/update their own articles
CREATE POLICY articles_write_own ON public.articles
    FOR INSERT
    WITH CHECK (author_id = auth.uid());

CREATE POLICY articles_update_own ON public.articles
    FOR UPDATE
    USING (author_id = auth.uid())
    WITH CHECK (author_id = auth.uid());

-- Only admins can delete
CREATE POLICY articles_delete_admin ON public.articles
    FOR DELETE
    USING (auth.jwt()->>'role' = 'admin');


-- Pattern 3: Multi-tenant isolation
-- Tenant ID from custom claim

CREATE TABLE public.tenant_data (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    tenant_id UUID NOT NULL,
    user_id UUID REFERENCES auth.users(id),
    data JSONB,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

ALTER TABLE public.tenant_data ENABLE ROW LEVEL SECURITY;

-- Tenant isolation policy
CREATE POLICY tenant_data_isolation ON public.tenant_data
    FOR ALL
    USING (tenant_id = (auth.jwt()->>'tenant_id')::UUID)
    WITH CHECK (tenant_id = (auth.jwt()->>'tenant_id')::UUID);


-- Pattern 4: Hierarchical access (org -> team -> user)

CREATE TABLE public.team_documents (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id UUID NOT NULL,
    team_id UUID NOT NULL,
    owner_id UUID NOT NULL REFERENCES auth.users(id),
    title TEXT NOT NULL,
    content TEXT,
    visibility TEXT DEFAULT 'team',  -- private, team, org, public
    created_at TIMESTAMPTZ DEFAULT NOW()
);

ALTER TABLE public.team_documents ENABLE ROW LEVEL SECURITY;

-- Helper function for organization membership
CREATE OR REPLACE FUNCTION auth.user_org_ids()
RETURNS UUID[]
LANGUAGE sql
STABLE
AS $$
    SELECT COALESCE(
        (SELECT ARRAY(
            SELECT jsonb_array_elements_text(auth.jwt()->'app_metadata'->'org_ids')
        )::UUID[]),
        ARRAY[]::UUID[]
    );
$$;

-- Helper function for team membership
CREATE OR REPLACE FUNCTION auth.user_team_ids()
RETURNS UUID[]
LANGUAGE sql
STABLE
AS $$
    SELECT COALESCE(
        (SELECT ARRAY(
            SELECT jsonb_array_elements_text(auth.jwt()->'app_metadata'->'team_ids')
        )::UUID[]),
        ARRAY[]::UUID[]
    );
$$;

-- Hierarchical access policy
CREATE POLICY team_docs_read ON public.team_documents
    FOR SELECT
    USING (
        visibility = 'public'
        OR (visibility = 'org' AND org_id = ANY(auth.user_org_ids()))
        OR (visibility = 'team' AND team_id = ANY(auth.user_team_ids()))
        OR (visibility = 'private' AND owner_id = auth.uid())
        OR owner_id = auth.uid()
    );

CREATE POLICY team_docs_write ON public.team_documents
    FOR INSERT
    WITH CHECK (
        team_id = ANY(auth.user_team_ids())
        AND owner_id = auth.uid()
    );

CREATE POLICY team_docs_update ON public.team_documents
    FOR UPDATE
    USING (owner_id = auth.uid())
    WITH CHECK (owner_id = auth.uid());

CREATE POLICY team_docs_delete ON public.team_documents
    FOR DELETE
    USING (
        owner_id = auth.uid()
        OR auth.jwt()->>'role' = 'org_admin'
    );
```

### 7.2 Performance Optimizations

```sql
-- ============================================================
-- Performance-optimized RLS patterns
-- ============================================================

-- Create materialized auth context for complex policies
-- This reduces repeated JWT parsing in hot paths

CREATE OR REPLACE FUNCTION auth._get_auth_context()
RETURNS TABLE(
    user_id UUID,
    role TEXT,
    tenant_id UUID,
    org_ids UUID[],
    team_ids UUID[],
    is_admin BOOLEAN,
    aal auth.aal_level
)
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$
    SELECT
        auth.uid(),
        auth.role(),
        (auth.jwt()->>'tenant_id')::UUID,
        auth.user_org_ids(),
        auth.user_team_ids(),
        auth.jwt()->>'role' = 'admin',
        auth.aal();
$$;

-- Use in RLS policy (caches result per statement)
CREATE POLICY optimized_policy ON public.some_table
    FOR SELECT
    USING (
        EXISTS (
            SELECT 1 FROM auth._get_auth_context() ctx
            WHERE some_table.tenant_id = ctx.tenant_id
            AND some_table.owner_id = ctx.user_id
        )
    );


-- ============================================================
-- Index strategy for RLS-heavy tables
-- ============================================================

-- Always index columns used in RLS policies
CREATE INDEX idx_user_data_user_id ON public.user_data(user_id);
CREATE INDEX idx_tenant_data_tenant_user ON public.tenant_data(tenant_id, user_id);
CREATE INDEX idx_team_docs_visibility ON public.team_documents(visibility, org_id, team_id);

-- For time-series data with RLS
CREATE INDEX idx_events_user_time ON public.events(user_id, created_at DESC);

-- Partial indexes for common access patterns
CREATE INDEX idx_articles_published ON public.articles(status) WHERE status = 'published';


-- ============================================================
-- RLS bypass for service role
-- ============================================================

-- Service role bypasses RLS by default
-- Use this pattern for admin operations:

-- Option 1: Use service_role with BYPASSRLS
GRANT BYPASSRLS TO service_role;

-- Option 2: Explicit bypass in policy
CREATE POLICY admin_bypass ON public.some_table
    FOR ALL
    USING (
        auth.role() = 'service_role'
        OR user_id = auth.uid()
    );
```

### 7.3 RLS with Realtime Subscriptions

```sql
-- ============================================================
-- RLS-aware realtime configuration
-- ============================================================

-- Enable realtime for a table with RLS
-- The realtime system respects RLS policies

CREATE PUBLICATION orochi_realtime FOR TABLE
    public.user_data,
    public.articles,
    public.team_documents;

-- Realtime authorization function
-- Called for each subscription to validate access

CREATE OR REPLACE FUNCTION auth.authorize_realtime(
    p_table_name TEXT,
    p_filter JSONB
)
RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
DECLARE
    v_user_id UUID;
BEGIN
    v_user_id := auth.uid();

    IF v_user_id IS NULL THEN
        RETURN FALSE;
    END IF;

    -- Validate filter matches user's access
    -- This prevents subscribing to other users' data

    IF p_table_name = 'public.user_data' THEN
        RETURN (p_filter->>'user_id')::UUID = v_user_id;
    END IF;

    IF p_table_name = 'public.team_documents' THEN
        RETURN (p_filter->>'team_id')::UUID = ANY(auth.user_team_ids());
    END IF;

    RETURN FALSE;
END;
$$;
```

---

## 8. Implementation Notes

### 8.1 C Module Registration

Add to `src/auth/auth.c`:

```c
/*-------------------------------------------------------------------------
 * src/auth/auth.c
 * Authentication module registration for Orochi DB
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"

#include "../orochi.h"
#include "jwt.h"
#include "auth.h"

/* GUC variables for auth */
char   *orochi_auth_jwt_secret = NULL;
int     orochi_auth_jwt_exp = 3600;
bool    orochi_auth_enable_anonymous = true;
int     orochi_auth_rate_limit_email = 30;
int     orochi_auth_rate_limit_sms = 30;

/*
 * orochi_auth_define_gucs
 *    Define authentication GUC parameters
 */
void
orochi_auth_define_gucs(void)
{
    DefineCustomStringVariable("orochi.auth_jwt_secret",
                               "JWT signing secret",
                               NULL,
                               &orochi_auth_jwt_secret,
                               NULL,
                               PGC_SIGHUP,
                               GUC_SUPERUSER_ONLY,
                               NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_jwt_exp",
                            "JWT expiration time in seconds",
                            NULL,
                            &orochi_auth_jwt_exp,
                            3600,
                            60,
                            86400,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("orochi.auth_enable_anonymous",
                             "Enable anonymous authentication",
                             NULL,
                             &orochi_auth_enable_anonymous,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);
}

/*
 * orochi_auth_init
 *    Initialize auth module
 */
void
orochi_auth_init(void)
{
    orochi_auth_define_gucs();

    elog(LOG, "Orochi Auth module initialized");
}
```

### 8.2 SQL Extension File Addition

Add to `sql/auth.sql`:

```sql
-- ============================================================
-- Auth Extension SQL Registration
-- ============================================================

-- C Functions
CREATE FUNCTION orochi._jwt_validate(token TEXT, secret TEXT)
RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'orochi_jwt_validate'
    LANGUAGE C STRICT;

CREATE FUNCTION orochi._jwt_decode_claims(token TEXT, secret TEXT)
RETURNS JSONB
    AS 'MODULE_PATHNAME', 'orochi_jwt_decode_claims'
    LANGUAGE C STRICT;

CREATE FUNCTION orochi._jwt_encode(claims JSONB, secret TEXT, algorithm TEXT)
RETURNS TEXT
    AS 'MODULE_PATHNAME', 'orochi_jwt_encode'
    LANGUAGE C STRICT;

CREATE FUNCTION orochi._jwt_set_auth_context(token TEXT, secret TEXT)
RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'orochi_jwt_set_auth_context'
    LANGUAGE C STRICT;

-- Grant execute to appropriate roles
GRANT EXECUTE ON FUNCTION auth.uid() TO authenticated, anon, service_role;
GRANT EXECUTE ON FUNCTION auth.role() TO authenticated, anon, service_role;
GRANT EXECUTE ON FUNCTION auth.jwt() TO authenticated, anon, service_role;
GRANT EXECUTE ON FUNCTION auth.email() TO authenticated, anon, service_role;
GRANT EXECUTE ON FUNCTION auth.aal() TO authenticated, anon, service_role;
GRANT EXECUTE ON FUNCTION auth.session_id() TO authenticated, anon, service_role;
```

### 8.3 Connection Pooler Integration

For PgCat/PgBouncer integration, the pooler should:

1. Extract JWT from connection parameters or first query
2. Call `SELECT auth.set_request_jwt($token)` at session start
3. Call `SELECT auth.clear_request_jwt()` at session end (for transaction pooling)

Example PgCat configuration:

```toml
[pools.orochi]
pool_mode = "transaction"

# Auth hook (called on each transaction)
query_parser_enabled = true

[pools.orochi.sharding]
sharding_function = "pg_bigint_hash"

[pools.orochi.users.authenticated]
pool_size = 50
statement_timeout = 30000

# Custom transaction start hook
[[pools.orochi.hooks]]
type = "transaction_start"
query = "SELECT auth.set_request_jwt($1)"
extract_parameter = "jwt"
```

### 8.4 Security Considerations

1. **JWT Secret Management**: Store secrets encrypted, rotate periodically
2. **Token Expiration**: Short-lived access tokens (15 min - 1 hour)
3. **Refresh Token Rotation**: Rotate on each use, detect token reuse
4. **Rate Limiting**: Protect against brute force attacks
5. **Audit Logging**: Log all auth events for compliance
6. **MFA Enforcement**: Require AAL2 for sensitive operations

### 8.5 Testing Strategy

```sql
-- Test auth.uid() returns NULL for unauthenticated
DO $$
BEGIN
    PERFORM set_config('request.jwt.claim.sub', '', TRUE);
    ASSERT auth.uid() IS NULL, 'auth.uid() should be NULL when not authenticated';
END;
$$;

-- Test auth.uid() returns correct UUID when authenticated
DO $$
DECLARE
    v_test_uuid UUID := gen_random_uuid();
BEGIN
    PERFORM set_config('request.jwt.claim.sub', v_test_uuid::TEXT, TRUE);
    ASSERT auth.uid() = v_test_uuid, 'auth.uid() should return the set UUID';
END;
$$;

-- Test RLS policy enforcement
CREATE TABLE test_rls (id UUID, user_id UUID, data TEXT);
ALTER TABLE test_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_policy ON test_rls USING (user_id = auth.uid());

DO $$
DECLARE
    v_user1 UUID := gen_random_uuid();
    v_user2 UUID := gen_random_uuid();
    v_count INTEGER;
BEGIN
    -- Insert test data
    INSERT INTO test_rls VALUES (gen_random_uuid(), v_user1, 'user1 data');
    INSERT INTO test_rls VALUES (gen_random_uuid(), v_user2, 'user2 data');

    -- Set context to user1
    PERFORM set_config('request.jwt.claim.sub', v_user1::TEXT, TRUE);

    -- User1 should only see their data
    SELECT COUNT(*) INTO v_count FROM test_rls;
    ASSERT v_count = 1, 'User should only see their own rows';
END;
$$;

DROP TABLE test_rls;
```

---

## Appendix A: JWT Token Structure

### Access Token Claims

```json
{
  "aud": "authenticated",
  "exp": 1705420800,
  "iat": 1705417200,
  "iss": "https://your-project.orochi.io",
  "sub": "user-uuid",
  "email": "user@example.com",
  "phone": "+1234567890",
  "role": "authenticated",
  "aal": "aal1",
  "session_id": "session-uuid",
  "amr": [
    {"method": "password", "timestamp": 1705417200}
  ],
  "app_metadata": {
    "provider": "email",
    "providers": ["email"]
  },
  "user_metadata": {
    "name": "John Doe"
  }
}
```

### Refresh Token

Stored encrypted in `auth.refresh_tokens`, not a JWT.

---

## Appendix B: Webhook Event Payloads

### user.created

```json
{
  "type": "webhook",
  "event": "user.created",
  "timestamp": 1705417200,
  "webhook_id": "uuid",
  "user": {
    "id": "user-uuid",
    "email": "user@example.com",
    "role": "authenticated",
    "is_anonymous": false,
    "created_at": "2024-01-16T12:00:00Z",
    "app_metadata": {},
    "user_metadata": {}
  }
}
```

### session.created

```json
{
  "type": "webhook",
  "event": "session.created",
  "timestamp": 1705417200,
  "webhook_id": "uuid",
  "user": { ... },
  "session": {
    "id": "session-uuid",
    "user_id": "user-uuid",
    "aal": "aal1",
    "amr": [{"method": "password", "timestamp": 1705417200}],
    "created_at": "2024-01-16T12:00:00Z"
  }
}
```

---

## Appendix C: Migration from Supabase Auth

For users migrating from Supabase:

1. Schema is compatible - direct table migration possible
2. JWT format identical - existing tokens work
3. API endpoints match GoTrue specification
4. RLS policies using `auth.uid()` work unchanged

Migration script:

```sql
-- Migrate users
INSERT INTO auth.users
SELECT * FROM supabase_auth.users;

-- Migrate sessions
INSERT INTO auth.sessions
SELECT * FROM supabase_auth.sessions;

-- Migrate identities
INSERT INTO auth.identities
SELECT * FROM supabase_auth.identities;

-- Migrate refresh tokens
INSERT INTO auth.refresh_tokens
SELECT * FROM supabase_auth.refresh_tokens;
```

---

**Document Version**: 1.0.0
**Last Updated**: 2026-01-16
**Compatibility**: Orochi DB 1.0+, PostgreSQL 16+
