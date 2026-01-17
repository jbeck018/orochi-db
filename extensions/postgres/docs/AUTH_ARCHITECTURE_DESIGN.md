# Orochi DB Authentication Architecture Design

## Neon-Style Authentication Features for Distributed HTAP

**Version**: 1.0.0
**Status**: Architecture Design
**Author**: Architecture Team
**Last Updated**: 2026-01-16

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Passwordless Authentication (WebAuthn/FIDO2)](#2-passwordless-authentication-webauthfido2)
3. [Branch-Level Access Control](#3-branch-level-access-control)
4. [Compute Endpoint Authentication](#4-compute-endpoint-authentication)
5. [Database Roles Mapped to App Users](#5-database-roles-mapped-to-app-users)
6. [Distributed Architecture Integration](#6-distributed-architecture-integration)
7. [Implementation Plan](#7-implementation-plan)

---

## 1. Executive Summary

This document defines the authentication architecture for Orochi DB, implementing Neon-style authentication features optimized for distributed HTAP workloads. The design addresses:

- **Passwordless Authentication**: WebAuthn/FIDO2 passkey support with secure credential storage
- **Branch-Level Access Control**: Fine-grained permissions for database branches
- **Compute Endpoint Authentication**: Token-based connection authentication with PgCat integration
- **Dynamic Role Management**: Application user to database role mapping at scale

### Design Principles

1. **Zero Trust**: Every connection is authenticated and authorized
2. **Distributed First**: Authentication state replicated via Raft consensus
3. **Performance**: Sub-10ms authentication latency with local caching
4. **PostgreSQL Native**: Leverage native pg_hba.conf and GRANT mechanisms
5. **Scalability**: Support 100K+ roles without performance degradation

---

## 2. Passwordless Authentication (WebAuthn/FIDO2)

### 2.1 Architecture Overview

```
+------------------------------------------------------------------+
|                  PASSWORDLESS AUTHENTICATION FLOW                  |
+------------------------------------------------------------------+
|                                                                    |
|  +----------------+     +------------------+     +---------------+ |
|  |   User Device  |     |   Auth Service   |     | Orochi Cluster| |
|  | (Authenticator)|     |    (Control)     |     |   (Data)      | |
|  +----------------+     +------------------+     +---------------+ |
|         |                       |                       |         |
|         |  1. Registration      |                       |         |
|         |---------------------->|                       |         |
|         |  Challenge + Options  |                       |         |
|         |<----------------------|                       |         |
|         |                       |                       |         |
|         |  2. Create Credential |                       |         |
|         |  (Local Biometric)    |                       |         |
|         |                       |                       |         |
|         |  3. Send Attestation  |                       |         |
|         |---------------------->|                       |         |
|         |                       |  4. Store Credential  |         |
|         |                       |---------------------->|         |
|         |  Registration OK      |                       |         |
|         |<----------------------|                       |         |
|         |                       |                       |         |
|         |  5. Authentication    |                       |         |
|         |---------------------->|                       |         |
|         |  Challenge            |                       |         |
|         |<----------------------|                       |         |
|         |                       |                       |         |
|         |  6. Sign Challenge    |                       |         |
|         |  (Local Biometric)    |                       |         |
|         |                       |                       |         |
|         |  7. Send Assertion    |                       |         |
|         |---------------------->|                       |         |
|         |                       |  8. Verify Signature  |         |
|         |                       |---------------------->|         |
|         |  JWT Token            |                       |         |
|         |<----------------------|                       |         |
|                                                                    |
+------------------------------------------------------------------+
```

### 2.2 Credential Storage Schema

```sql
-- WebAuthn/FIDO2 credential storage in control plane
CREATE TABLE auth.webauthn_credentials (
    credential_id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id                 UUID NOT NULL REFERENCES auth.users(user_id),

    -- WebAuthn Credential Data
    webauthn_credential_id  BYTEA NOT NULL UNIQUE,  -- Authenticator's credential ID
    public_key_spki         BYTEA NOT NULL,          -- Public key in SPKI format
    public_key_algorithm    INTEGER NOT NULL,        -- COSE algorithm identifier

    -- Credential Metadata
    credential_type         credential_type NOT NULL DEFAULT 'passkey',
    device_name             VARCHAR(255),
    device_type             device_type,
    transports              TEXT[],                  -- usb, nfc, ble, internal, hybrid

    -- Security Counters
    sign_count              BIGINT NOT NULL DEFAULT 0,
    backup_eligible         BOOLEAN DEFAULT FALSE,
    backup_state            BOOLEAN DEFAULT FALSE,

    -- Attestation Data (optional, for enterprise)
    attestation_format      VARCHAR(50),
    attestation_statement   JSONB,
    aaguid                  BYTEA,                   -- Authenticator AAGUID

    -- Status and Timestamps
    status                  credential_status NOT NULL DEFAULT 'active',
    last_used_at            TIMESTAMPTZ,
    last_used_ip            INET,
    last_used_user_agent    TEXT,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at              TIMESTAMPTZ,
    revoked_by              UUID REFERENCES auth.users(user_id),
    revoked_reason          TEXT,

    CONSTRAINT valid_sign_count CHECK (sign_count >= 0)
);

CREATE TYPE credential_type AS ENUM (
    'passkey',              -- Platform/roaming passkey
    'security_key',         -- Hardware security key (YubiKey)
    'platform',             -- Platform authenticator (Touch ID, Windows Hello)
    'cross_platform'        -- Cross-platform (USB security key)
);

CREATE TYPE device_type AS ENUM (
    'windows_hello',
    'apple_touch_id',
    'apple_face_id',
    'android_biometric',
    'yubikey',
    'google_titan',
    'other'
);

CREATE TYPE credential_status AS ENUM (
    'pending_verification',
    'active',
    'suspended',
    'revoked'
);

-- Indexes for efficient lookup
CREATE INDEX idx_webauthn_user_id ON auth.webauthn_credentials(user_id);
CREATE INDEX idx_webauthn_credential_id ON auth.webauthn_credentials(webauthn_credential_id);
CREATE INDEX idx_webauthn_status ON auth.webauthn_credentials(status) WHERE status = 'active';

-- Device registration for passkey sync
CREATE TABLE auth.passkey_sync_devices (
    sync_device_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id                 UUID NOT NULL REFERENCES auth.users(user_id),
    credential_id           UUID NOT NULL REFERENCES auth.webauthn_credentials(credential_id),

    -- Device Identification
    device_fingerprint      VARCHAR(255) NOT NULL,
    platform                VARCHAR(50),             -- iOS, Android, macOS, Windows
    platform_version        VARCHAR(50),
    sync_provider           sync_provider,           -- iCloud, Google Password Manager

    -- Sync State
    last_sync_at            TIMESTAMPTZ,
    sync_status             VARCHAR(50) DEFAULT 'active',

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_device_credential UNIQUE (device_fingerprint, credential_id)
);

CREATE TYPE sync_provider AS ENUM (
    'icloud_keychain',
    'google_password_manager',
    'microsoft_authenticator',
    '1password',
    'dashlane',
    'none'
);
```

### 2.3 WebAuthn Registration Flow

```sql
-- Challenge generation and storage
CREATE TABLE auth.webauthn_challenges (
    challenge_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    challenge               BYTEA NOT NULL,          -- 32-byte random challenge
    challenge_type          challenge_type NOT NULL,
    user_id                 UUID REFERENCES auth.users(user_id),

    -- RP (Relying Party) Configuration
    rp_id                   VARCHAR(255) NOT NULL,   -- e.g., 'orochi.cloud'
    rp_name                 VARCHAR(255) NOT NULL,
    origin                  TEXT NOT NULL,

    -- Request Configuration
    user_verification       user_verification NOT NULL DEFAULT 'preferred',
    attestation             attestation_conveyance DEFAULT 'none',
    authenticator_selection JSONB,

    -- State
    ip_address              INET,
    user_agent              TEXT,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ NOT NULL,
    consumed_at             TIMESTAMPTZ,

    CONSTRAINT challenge_not_expired CHECK (expires_at > created_at)
);

CREATE TYPE challenge_type AS ENUM (
    'registration',
    'authentication'
);

CREATE TYPE user_verification AS ENUM (
    'required',
    'preferred',
    'discouraged'
);

CREATE TYPE attestation_conveyance AS ENUM (
    'none',
    'indirect',
    'direct',
    'enterprise'
);

-- Auto-cleanup of expired challenges
CREATE INDEX idx_challenges_expires ON auth.webauthn_challenges(expires_at);

-- Registration stored procedure
CREATE OR REPLACE FUNCTION auth.webauthn_begin_registration(
    p_user_id UUID,
    p_rp_id VARCHAR(255),
    p_rp_name VARCHAR(255),
    p_origin TEXT,
    p_user_verification user_verification DEFAULT 'preferred'
) RETURNS JSON AS $$
DECLARE
    v_challenge BYTEA;
    v_challenge_id UUID;
    v_user auth.users%ROWTYPE;
    v_options JSON;
BEGIN
    -- Get user
    SELECT * INTO v_user FROM auth.users WHERE user_id = p_user_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'User not found';
    END IF;

    -- Generate 32-byte random challenge
    v_challenge := gen_random_bytes(32);

    -- Store challenge
    INSERT INTO auth.webauthn_challenges (
        challenge, challenge_type, user_id, rp_id, rp_name, origin,
        user_verification, expires_at
    ) VALUES (
        v_challenge, 'registration', p_user_id, p_rp_id, p_rp_name, p_origin,
        p_user_verification, NOW() + INTERVAL '5 minutes'
    ) RETURNING challenge_id INTO v_challenge_id;

    -- Build PublicKeyCredentialCreationOptions
    v_options := json_build_object(
        'challenge', encode(v_challenge, 'base64'),
        'rp', json_build_object(
            'id', p_rp_id,
            'name', p_rp_name
        ),
        'user', json_build_object(
            'id', encode(p_user_id::text::bytea, 'base64'),
            'name', v_user.email,
            'displayName', COALESCE(v_user.display_name, v_user.email)
        ),
        'pubKeyCredParams', json_build_array(
            json_build_object('type', 'public-key', 'alg', -7),   -- ES256
            json_build_object('type', 'public-key', 'alg', -257)  -- RS256
        ),
        'timeout', 300000,
        'authenticatorSelection', json_build_object(
            'authenticatorAttachment', 'platform',
            'requireResidentKey', true,
            'residentKey', 'required',
            'userVerification', p_user_verification::text
        ),
        'attestation', 'none',
        'challengeId', v_challenge_id
    );

    RETURN v_options;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Complete registration
CREATE OR REPLACE FUNCTION auth.webauthn_complete_registration(
    p_challenge_id UUID,
    p_credential_id BYTEA,
    p_public_key BYTEA,
    p_algorithm INTEGER,
    p_sign_count BIGINT,
    p_transports TEXT[],
    p_device_name VARCHAR(255) DEFAULT NULL
) RETURNS UUID AS $$
DECLARE
    v_challenge auth.webauthn_challenges%ROWTYPE;
    v_credential_uuid UUID;
BEGIN
    -- Get and validate challenge
    SELECT * INTO v_challenge
    FROM auth.webauthn_challenges
    WHERE challenge_id = p_challenge_id
      AND challenge_type = 'registration'
      AND consumed_at IS NULL
      AND expires_at > NOW();

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Invalid or expired challenge';
    END IF;

    -- Check for duplicate credential
    IF EXISTS (SELECT 1 FROM auth.webauthn_credentials WHERE webauthn_credential_id = p_credential_id) THEN
        RAISE EXCEPTION 'Credential already registered';
    END IF;

    -- Store credential
    INSERT INTO auth.webauthn_credentials (
        user_id, webauthn_credential_id, public_key_spki, public_key_algorithm,
        sign_count, transports, device_name, status
    ) VALUES (
        v_challenge.user_id, p_credential_id, p_public_key, p_algorithm,
        p_sign_count, p_transports, p_device_name, 'active'
    ) RETURNING credential_id INTO v_credential_uuid;

    -- Mark challenge as consumed
    UPDATE auth.webauthn_challenges
    SET consumed_at = NOW()
    WHERE challenge_id = p_challenge_id;

    -- Audit log
    INSERT INTO auth.audit_logs (user_id, event_category, event_action, event_outcome, event_data)
    VALUES (v_challenge.user_id, 'authentication', 'webauthn.credential.registered', 'success',
            jsonb_build_object('credential_id', v_credential_uuid, 'device_name', p_device_name));

    RETURN v_credential_uuid;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

### 2.4 WebAuthn Authentication Flow

```sql
-- Begin authentication
CREATE OR REPLACE FUNCTION auth.webauthn_begin_authentication(
    p_email VARCHAR(255),
    p_rp_id VARCHAR(255),
    p_origin TEXT
) RETURNS JSON AS $$
DECLARE
    v_user auth.users%ROWTYPE;
    v_challenge BYTEA;
    v_challenge_id UUID;
    v_credentials JSON;
    v_options JSON;
BEGIN
    -- Get user by email
    SELECT * INTO v_user FROM auth.users WHERE email = p_email AND status = 'active';
    IF NOT FOUND THEN
        -- Return empty challenge to prevent user enumeration
        v_challenge := gen_random_bytes(32);
        RETURN json_build_object(
            'challenge', encode(v_challenge, 'base64'),
            'rpId', p_rp_id,
            'allowCredentials', '[]'::json,
            'timeout', 300000,
            'userVerification', 'preferred'
        );
    END IF;

    -- Generate challenge
    v_challenge := gen_random_bytes(32);

    -- Store challenge
    INSERT INTO auth.webauthn_challenges (
        challenge, challenge_type, user_id, rp_id, rp_name, origin, expires_at
    ) VALUES (
        v_challenge, 'authentication', v_user.user_id, p_rp_id, 'Orochi Cloud', p_origin,
        NOW() + INTERVAL '5 minutes'
    ) RETURNING challenge_id INTO v_challenge_id;

    -- Get user's active credentials
    SELECT json_agg(json_build_object(
        'type', 'public-key',
        'id', encode(webauthn_credential_id, 'base64'),
        'transports', transports
    ))
    INTO v_credentials
    FROM auth.webauthn_credentials
    WHERE user_id = v_user.user_id AND status = 'active';

    v_options := json_build_object(
        'challenge', encode(v_challenge, 'base64'),
        'rpId', p_rp_id,
        'allowCredentials', COALESCE(v_credentials, '[]'::json),
        'timeout', 300000,
        'userVerification', 'preferred',
        'challengeId', v_challenge_id
    );

    RETURN v_options;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Verify authentication assertion
CREATE OR REPLACE FUNCTION auth.webauthn_verify_authentication(
    p_challenge_id UUID,
    p_credential_id BYTEA,
    p_authenticator_data BYTEA,
    p_signature BYTEA,
    p_client_data_json TEXT,
    p_ip_address INET DEFAULT NULL,
    p_user_agent TEXT DEFAULT NULL
) RETURNS JSON AS $$
DECLARE
    v_challenge auth.webauthn_challenges%ROWTYPE;
    v_credential auth.webauthn_credentials%ROWTYPE;
    v_user auth.users%ROWTYPE;
    v_new_sign_count BIGINT;
    v_session_token TEXT;
    v_result JSON;
BEGIN
    -- Get challenge
    SELECT * INTO v_challenge
    FROM auth.webauthn_challenges
    WHERE challenge_id = p_challenge_id
      AND challenge_type = 'authentication'
      AND consumed_at IS NULL
      AND expires_at > NOW();

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Invalid or expired challenge';
    END IF;

    -- Get credential
    SELECT * INTO v_credential
    FROM auth.webauthn_credentials
    WHERE webauthn_credential_id = p_credential_id
      AND status = 'active';

    IF NOT FOUND THEN
        -- Log failed attempt
        INSERT INTO auth.audit_logs (user_id, event_category, event_action, event_outcome, event_data)
        VALUES (v_challenge.user_id, 'authentication', 'webauthn.authentication', 'failure',
                jsonb_build_object('reason', 'credential_not_found', 'ip', p_ip_address));
        RAISE EXCEPTION 'Credential not found or inactive';
    END IF;

    -- Verify user matches
    IF v_credential.user_id != v_challenge.user_id THEN
        RAISE EXCEPTION 'Credential does not belong to user';
    END IF;

    -- Signature verification would be done in application layer
    -- Here we trust the application has verified the signature

    -- Extract and verify sign count (replay attack protection)
    v_new_sign_count := get_byte(p_authenticator_data, 33)::bigint << 24 |
                        get_byte(p_authenticator_data, 34)::bigint << 16 |
                        get_byte(p_authenticator_data, 35)::bigint << 8 |
                        get_byte(p_authenticator_data, 36)::bigint;

    IF v_new_sign_count <= v_credential.sign_count AND v_new_sign_count != 0 THEN
        -- Possible cloned authenticator
        UPDATE auth.webauthn_credentials
        SET status = 'suspended'
        WHERE credential_id = v_credential.credential_id;

        INSERT INTO auth.audit_logs (user_id, event_category, event_action, event_outcome, event_data)
        VALUES (v_credential.user_id, 'security', 'webauthn.replay_detected', 'denied',
                jsonb_build_object('credential_id', v_credential.credential_id, 'ip', p_ip_address));

        RAISE EXCEPTION 'Potential credential clone detected';
    END IF;

    -- Update credential
    UPDATE auth.webauthn_credentials
    SET sign_count = v_new_sign_count,
        last_used_at = NOW(),
        last_used_ip = p_ip_address,
        last_used_user_agent = p_user_agent
    WHERE credential_id = v_credential.credential_id;

    -- Mark challenge consumed
    UPDATE auth.webauthn_challenges
    SET consumed_at = NOW()
    WHERE challenge_id = p_challenge_id;

    -- Get user
    SELECT * INTO v_user FROM auth.users WHERE user_id = v_credential.user_id;

    -- Create session
    v_session_token := encode(gen_random_bytes(32), 'hex');

    INSERT INTO auth.sessions (user_id, token_hash, ip_address, user_agent, expires_at)
    VALUES (v_user.user_id, crypt(v_session_token, gen_salt('bf')),
            p_ip_address, p_user_agent, NOW() + INTERVAL '24 hours');

    -- Audit log
    INSERT INTO auth.audit_logs (user_id, event_category, event_action, event_outcome, event_data)
    VALUES (v_user.user_id, 'authentication', 'webauthn.authentication', 'success',
            jsonb_build_object('credential_id', v_credential.credential_id, 'ip', p_ip_address));

    RETURN json_build_object(
        'success', true,
        'user_id', v_user.user_id,
        'email', v_user.email,
        'session_token', v_session_token
    );
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

### 2.5 Device Management API

```sql
-- List user's registered devices/credentials
CREATE OR REPLACE FUNCTION auth.list_user_credentials(p_user_id UUID)
RETURNS TABLE (
    credential_id UUID,
    device_name VARCHAR(255),
    device_type device_type,
    credential_type credential_type,
    transports TEXT[],
    status credential_status,
    last_used_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        wc.credential_id,
        wc.device_name,
        wc.device_type,
        wc.credential_type,
        wc.transports,
        wc.status,
        wc.last_used_at,
        wc.created_at
    FROM auth.webauthn_credentials wc
    WHERE wc.user_id = p_user_id
    ORDER BY wc.created_at DESC;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Revoke credential
CREATE OR REPLACE FUNCTION auth.revoke_credential(
    p_user_id UUID,
    p_credential_id UUID,
    p_reason TEXT DEFAULT NULL
) RETURNS BOOLEAN AS $$
DECLARE
    v_count INTEGER;
BEGIN
    -- Verify user owns credential and has at least one other active credential
    SELECT COUNT(*) INTO v_count
    FROM auth.webauthn_credentials
    WHERE user_id = p_user_id AND status = 'active';

    IF v_count <= 1 THEN
        RAISE EXCEPTION 'Cannot revoke last active credential';
    END IF;

    -- Revoke
    UPDATE auth.webauthn_credentials
    SET status = 'revoked',
        revoked_at = NOW(),
        revoked_by = p_user_id,
        revoked_reason = p_reason
    WHERE credential_id = p_credential_id
      AND user_id = p_user_id
      AND status = 'active';

    IF NOT FOUND THEN
        RETURN FALSE;
    END IF;

    -- Audit log
    INSERT INTO auth.audit_logs (user_id, event_category, event_action, event_outcome, event_data)
    VALUES (p_user_id, 'authentication', 'webauthn.credential.revoked', 'success',
            jsonb_build_object('credential_id', p_credential_id, 'reason', p_reason));

    RETURN TRUE;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

---

## 3. Branch-Level Access Control

### 3.1 Branch Permission Model

```
+------------------------------------------------------------------+
|                    BRANCH ACCESS HIERARCHY                        |
+------------------------------------------------------------------+
|                                                                    |
|  Project Level                                                     |
|  +---------------------------------------------------------+      |
|  |  project_admin: Full access to all branches             |      |
|  |  project_developer: Create branches, write to own       |      |
|  |  project_viewer: Read-only to all branches              |      |
|  +---------------------------------------------------------+      |
|            |                                                       |
|            v                                                       |
|  Main Branch (protected)                                           |
|  +---------------------------------------------------------+      |
|  |  Permissions inherited from project                      |      |
|  |  + Additional protection rules                           |      |
|  |  - Require approval for writes                           |      |
|  |  - Audit all DML operations                              |      |
|  +---------------------------------------------------------+      |
|            |                                                       |
|     +------+------+                                               |
|     |             |                                               |
|     v             v                                               |
|  Branch A      Branch B                                           |
|  (dev)         (staging)                                          |
|  +--------+    +--------+                                         |
|  | owner: |    | owner: |                                         |
|  | user_1 |    | user_2 |                                         |
|  +--------+    +--------+                                         |
|      |              |                                             |
|      v              v                                             |
|  Branch A1     Branch B1                                          |
|  (feature)     (hotfix)                                           |
|  +--------+    +--------+                                         |
|  |inherit |    |inherit |                                         |
|  |from A  |    |from B  |                                         |
|  +--------+    +--------+                                         |
|                                                                    |
+------------------------------------------------------------------+
```

### 3.2 Branch Schema

```sql
-- Branch metadata with access control
CREATE TABLE platform.branches (
    branch_id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id              UUID NOT NULL REFERENCES platform.clusters(cluster_id),
    parent_branch_id        UUID REFERENCES platform.branches(branch_id),

    -- Branch Identification
    name                    VARCHAR(63) NOT NULL,
    slug                    VARCHAR(63) NOT NULL,
    description             TEXT,

    -- Branch State
    status                  branch_status NOT NULL DEFAULT 'creating',
    protection_level        protection_level NOT NULL DEFAULT 'none',

    -- Copy-on-Write State
    parent_lsn              pg_lsn,                  -- LSN at branch creation
    parent_timestamp        TIMESTAMPTZ,             -- Timestamp at branch creation
    diverged_at             TIMESTAMPTZ,             -- When first write occurred

    -- Access Control
    owner_user_id           UUID NOT NULL REFERENCES platform.users(user_id),
    inherit_permissions     BOOLEAN NOT NULL DEFAULT TRUE,

    -- Compute Endpoint
    endpoint_id             UUID,
    connection_string       TEXT,                    -- Encrypted

    -- Lifecycle
    auto_delete_after       INTERVAL,               -- Auto-cleanup for ephemeral branches
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at              TIMESTAMPTZ,

    CONSTRAINT unique_branch_name_per_cluster UNIQUE (cluster_id, slug),
    CONSTRAINT valid_slug CHECK (slug ~ '^[a-z][a-z0-9-]*$')
);

CREATE TYPE branch_status AS ENUM (
    'creating',
    'ready',
    'suspended',
    'deleting',
    'deleted',
    'failed'
);

CREATE TYPE protection_level AS ENUM (
    'none',                  -- No protection
    'require_approval',      -- Require approval for writes
    'read_only',            -- No writes allowed
    'locked'                -- No access
);

-- Branch-specific permissions
CREATE TABLE platform.branch_permissions (
    permission_id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    branch_id               UUID NOT NULL REFERENCES platform.branches(branch_id),

    -- Grantee (one of these must be set)
    user_id                 UUID REFERENCES platform.users(user_id),
    team_id                 UUID REFERENCES platform.teams(team_id),
    role_id                 UUID REFERENCES platform.roles(role_id),

    -- Permission Level
    permission_level        branch_permission_level NOT NULL,

    -- Constraints
    valid_until             TIMESTAMPTZ,
    ip_restrictions         INET[],
    time_restrictions       JSONB,                   -- e.g., {"weekdays_only": true}

    -- Metadata
    granted_by              UUID NOT NULL REFERENCES platform.users(user_id),
    granted_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at              TIMESTAMPTZ,

    CONSTRAINT exactly_one_grantee CHECK (
        (user_id IS NOT NULL)::int +
        (team_id IS NOT NULL)::int +
        (role_id IS NOT NULL)::int = 1
    )
);

CREATE TYPE branch_permission_level AS ENUM (
    'admin',                -- Full control: DDL, DML, grant permissions
    'write',                -- Read + Write: SELECT, INSERT, UPDATE, DELETE
    'read',                 -- Read only: SELECT
    'connect'               -- Connect only: Can connect but no table access
);

-- Index for efficient permission lookup
CREATE INDEX idx_branch_perms_branch ON platform.branch_permissions(branch_id);
CREATE INDEX idx_branch_perms_user ON platform.branch_permissions(user_id) WHERE user_id IS NOT NULL;
CREATE INDEX idx_branch_perms_team ON platform.branch_permissions(team_id) WHERE team_id IS NOT NULL;

-- Branch protection rules
CREATE TABLE platform.branch_protection_rules (
    rule_id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    branch_id               UUID NOT NULL REFERENCES platform.branches(branch_id),

    -- Rule Configuration
    rule_type               protection_rule_type NOT NULL,
    rule_config             JSONB NOT NULL,

    -- Status
    enabled                 BOOLEAN NOT NULL DEFAULT TRUE,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_rule_per_branch UNIQUE (branch_id, rule_type)
);

CREATE TYPE protection_rule_type AS ENUM (
    'require_approval',          -- Require approval for changes
    'restrict_ddl',              -- Block DDL operations
    'restrict_delete',           -- Block DELETE operations
    'require_mfa',               -- Require MFA for access
    'audit_all_queries',         -- Log all queries
    'time_window',               -- Only allow access during time window
    'ip_allowlist'               -- Restrict to specific IPs
);

-- Example rule configs:
-- require_approval: {"approvers": ["user_id_1", "user_id_2"], "min_approvals": 1}
-- restrict_ddl: {"allowed_operations": ["CREATE INDEX"]}
-- time_window: {"allowed_hours": {"start": 9, "end": 17}, "timezone": "UTC", "weekdays_only": true}
```

### 3.3 Permission Resolution

```sql
-- Resolve effective permissions for user on branch
CREATE OR REPLACE FUNCTION platform.get_branch_permissions(
    p_user_id UUID,
    p_branch_id UUID
) RETURNS branch_permission_level AS $$
DECLARE
    v_branch platform.branches%ROWTYPE;
    v_permission branch_permission_level;
    v_project_permission branch_permission_level;
BEGIN
    -- Get branch
    SELECT * INTO v_branch FROM platform.branches WHERE branch_id = p_branch_id;
    IF NOT FOUND THEN
        RETURN NULL;
    END IF;

    -- Check if branch is locked
    IF v_branch.protection_level = 'locked' THEN
        RETURN NULL;
    END IF;

    -- Branch owner has admin
    IF v_branch.owner_user_id = p_user_id THEN
        RETURN 'admin';
    END IF;

    -- Check direct user permission
    SELECT permission_level INTO v_permission
    FROM platform.branch_permissions
    WHERE branch_id = p_branch_id
      AND user_id = p_user_id
      AND (valid_until IS NULL OR valid_until > NOW())
      AND revoked_at IS NULL
    ORDER BY
        CASE permission_level
            WHEN 'admin' THEN 1
            WHEN 'write' THEN 2
            WHEN 'read' THEN 3
            WHEN 'connect' THEN 4
        END
    LIMIT 1;

    IF v_permission IS NOT NULL THEN
        RETURN v_permission;
    END IF;

    -- Check team permissions
    SELECT bp.permission_level INTO v_permission
    FROM platform.branch_permissions bp
    JOIN platform.team_members tm ON bp.team_id = tm.team_id
    WHERE bp.branch_id = p_branch_id
      AND tm.user_id = p_user_id
      AND (bp.valid_until IS NULL OR bp.valid_until > NOW())
      AND bp.revoked_at IS NULL
    ORDER BY
        CASE bp.permission_level
            WHEN 'admin' THEN 1
            WHEN 'write' THEN 2
            WHEN 'read' THEN 3
            WHEN 'connect' THEN 4
        END
    LIMIT 1;

    IF v_permission IS NOT NULL THEN
        RETURN v_permission;
    END IF;

    -- Check inherited permissions from parent branch
    IF v_branch.inherit_permissions AND v_branch.parent_branch_id IS NOT NULL THEN
        v_permission := platform.get_branch_permissions(p_user_id, v_branch.parent_branch_id);
        IF v_permission IS NOT NULL THEN
            -- Downgrade write to read for inherited permissions on protected branches
            IF v_branch.protection_level = 'read_only' AND v_permission IN ('admin', 'write') THEN
                RETURN 'read';
            END IF;
            RETURN v_permission;
        END IF;
    END IF;

    -- Check project-level permissions
    v_project_permission := platform.get_project_branch_permission(
        p_user_id,
        (SELECT project_id FROM platform.clusters WHERE cluster_id = v_branch.cluster_id)
    );

    RETURN v_project_permission;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Cross-branch access validation (e.g., for COPY between branches)
CREATE OR REPLACE FUNCTION platform.validate_cross_branch_access(
    p_user_id UUID,
    p_source_branch_id UUID,
    p_target_branch_id UUID,
    p_operation VARCHAR(50)  -- 'copy', 'compare', 'merge'
) RETURNS BOOLEAN AS $$
DECLARE
    v_source_perm branch_permission_level;
    v_target_perm branch_permission_level;
BEGIN
    v_source_perm := platform.get_branch_permissions(p_user_id, p_source_branch_id);
    v_target_perm := platform.get_branch_permissions(p_user_id, p_target_branch_id);

    IF v_source_perm IS NULL OR v_target_perm IS NULL THEN
        RETURN FALSE;
    END IF;

    CASE p_operation
        WHEN 'copy' THEN
            -- Need read on source, write on target
            RETURN v_source_perm IN ('admin', 'write', 'read')
               AND v_target_perm IN ('admin', 'write');
        WHEN 'compare' THEN
            -- Need read on both
            RETURN v_source_perm IN ('admin', 'write', 'read')
               AND v_target_perm IN ('admin', 'write', 'read');
        WHEN 'merge' THEN
            -- Need write on both
            RETURN v_source_perm IN ('admin', 'write')
               AND v_target_perm IN ('admin', 'write');
        ELSE
            RETURN FALSE;
    END CASE;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

### 3.4 Integration with Orochi Sharding

```sql
-- Branch isolation at shard level
CREATE TABLE platform.branch_shard_mapping (
    mapping_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    branch_id               UUID NOT NULL REFERENCES platform.branches(branch_id),

    -- Shard Information (from orochi.orochi_shards)
    shard_id                BIGINT NOT NULL,
    table_oid               OID NOT NULL,

    -- Copy-on-Write State
    cow_state               cow_state NOT NULL DEFAULT 'shared',
    base_shard_id           BIGINT,                  -- Original shard for CoW

    -- Location
    node_id                 INTEGER NOT NULL,
    storage_path            TEXT,

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_branch_shard UNIQUE (branch_id, shard_id)
);

CREATE TYPE cow_state AS ENUM (
    'shared',               -- Sharing pages with parent
    'diverging',            -- Some pages copied
    'independent'           -- Fully independent
);

-- Function to get shard routing for branch
CREATE OR REPLACE FUNCTION platform.get_branch_shard_route(
    p_branch_id UUID,
    p_table_oid OID,
    p_hash_value INTEGER
) RETURNS TABLE (
    shard_id BIGINT,
    node_id INTEGER,
    node_host TEXT,
    node_port INTEGER,
    cow_state cow_state
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        bsm.shard_id,
        bsm.node_id,
        n.hostname::text,
        n.port,
        bsm.cow_state
    FROM platform.branch_shard_mapping bsm
    JOIN orochi.orochi_nodes n ON bsm.node_id = n.node_id
    JOIN orochi.orochi_shards s ON bsm.shard_id = s.shard_id
    WHERE bsm.branch_id = p_branch_id
      AND s.table_oid = p_table_oid
      AND p_hash_value >= s.hash_min
      AND p_hash_value <= s.hash_max;
END;
$$ LANGUAGE plpgsql;
```

---

## 4. Compute Endpoint Authentication

### 4.1 Connection Token Architecture

```
+------------------------------------------------------------------+
|              COMPUTE ENDPOINT AUTHENTICATION FLOW                  |
+------------------------------------------------------------------+
|                                                                    |
|  Application                 PgCat Pooler              Orochi DB   |
|      |                           |                         |       |
|      |  1. Connect with token    |                         |       |
|      |  postgresql://user:token@ |                         |       |
|      |  endpoint.orochi.cloud    |                         |       |
|      |-------------------------->|                         |       |
|      |                           |                         |       |
|      |                           |  2. Parse token         |       |
|      |                           |  (JWT or opaque)        |       |
|      |                           |                         |       |
|      |                           |  3. Validate locally    |       |
|      |                           |  (cached public key)    |       |
|      |                           |                         |       |
|      |                           |  4. Map to DB role      |       |
|      |                           |------------------------->       |
|      |                           |                         |       |
|      |                           |  5. SET ROLE            |       |
|      |                           |  SET orochi.branch_id   |       |
|      |                           |------------------------->       |
|      |                           |                         |       |
|      |  Connection Ready         |                         |       |
|      |<--------------------------|                         |       |
|      |                           |                         |       |
|      |  Query                    |                         |       |
|      |-------------------------->|                         |       |
|      |                           |  Forward with context   |       |
|      |                           |------------------------->       |
|      |                           |                         |       |
|      |  Results                  |<-------------------------|       |
|      |<--------------------------|                         |       |
|                                                                    |
+------------------------------------------------------------------+
```

### 4.2 Endpoint Credentials Schema

```sql
-- Compute endpoint configuration
CREATE TABLE platform.compute_endpoints (
    endpoint_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id              UUID NOT NULL REFERENCES platform.clusters(cluster_id),
    branch_id               UUID NOT NULL REFERENCES platform.branches(branch_id),

    -- Endpoint Configuration
    endpoint_name           VARCHAR(63) NOT NULL,
    endpoint_host           VARCHAR(255) NOT NULL,   -- e.g., ep-xxx.us-east-1.orochi.cloud
    endpoint_port           INTEGER NOT NULL DEFAULT 5432,

    -- Pooler Configuration
    pooler_mode             pooler_mode NOT NULL DEFAULT 'transaction',
    min_connections         INTEGER NOT NULL DEFAULT 0,
    max_connections         INTEGER NOT NULL DEFAULT 100,
    idle_timeout_seconds    INTEGER NOT NULL DEFAULT 300,

    -- Security
    ssl_mode                ssl_mode NOT NULL DEFAULT 'require',
    allowed_ips             INET[],

    -- State
    status                  endpoint_status NOT NULL DEFAULT 'creating',
    compute_units           DECIMAL(5,2),

    -- Autoscaling
    autoscale_enabled       BOOLEAN NOT NULL DEFAULT TRUE,
    autoscale_min_cu        DECIMAL(5,2) DEFAULT 0.25,
    autoscale_max_cu        DECIMAL(5,2) DEFAULT 4,
    scale_to_zero_seconds   INTEGER DEFAULT 300,

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TYPE pooler_mode AS ENUM ('session', 'transaction', 'statement');
CREATE TYPE ssl_mode AS ENUM ('disable', 'allow', 'prefer', 'require', 'verify-ca', 'verify-full');
CREATE TYPE endpoint_status AS ENUM ('creating', 'active', 'suspended', 'scaling', 'deleting');

-- Endpoint credentials (tokens)
CREATE TABLE platform.endpoint_credentials (
    credential_id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    endpoint_id             UUID NOT NULL REFERENCES platform.compute_endpoints(endpoint_id),

    -- Credential Type
    credential_type         endpoint_credential_type NOT NULL,

    -- For password-based
    username                VARCHAR(63),
    password_hash           TEXT,                    -- bcrypt hash

    -- For token-based (JWT)
    token_id                VARCHAR(64),             -- jti claim
    token_hash              TEXT,                    -- For opaque tokens

    -- Permissions
    db_role                 VARCHAR(63) NOT NULL,    -- PostgreSQL role to assume
    branch_permission       branch_permission_level NOT NULL,

    -- Constraints
    valid_from              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    valid_until             TIMESTAMPTZ,
    ip_allowlist            INET[],
    max_connections         INTEGER,

    -- Metadata
    description             TEXT,
    created_by              UUID REFERENCES platform.users(user_id),
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_used_at            TIMESTAMPTZ,
    use_count               BIGINT NOT NULL DEFAULT 0,

    -- Revocation
    revoked_at              TIMESTAMPTZ,
    revoked_by              UUID REFERENCES platform.users(user_id),

    CONSTRAINT valid_credential CHECK (
        (credential_type = 'password' AND username IS NOT NULL AND password_hash IS NOT NULL) OR
        (credential_type IN ('jwt', 'opaque_token') AND token_id IS NOT NULL)
    )
);

CREATE TYPE endpoint_credential_type AS ENUM (
    'password',             -- Traditional username/password
    'jwt',                  -- JWT token (embedded in connection string)
    'opaque_token'          -- Opaque token (lookup required)
);

-- Index for credential lookup
CREATE INDEX idx_endpoint_creds_endpoint ON platform.endpoint_credentials(endpoint_id);
CREATE INDEX idx_endpoint_creds_token ON platform.endpoint_credentials(token_id) WHERE token_id IS NOT NULL;
CREATE INDEX idx_endpoint_creds_username ON platform.endpoint_credentials(endpoint_id, username) WHERE username IS NOT NULL;

-- Connection string templates
CREATE TABLE platform.connection_string_templates (
    template_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    endpoint_id             UUID NOT NULL REFERENCES platform.compute_endpoints(endpoint_id),

    -- Template Configuration
    driver_type             VARCHAR(50) NOT NULL,    -- psql, jdbc, odbc, npgsql, etc.
    template                TEXT NOT NULL,

    -- Example templates:
    -- psql: postgresql://{user}:{password}@{host}:{port}/{database}?sslmode=require
    -- jdbc: jdbc:postgresql://{host}:{port}/{database}?user={user}&password={password}&sslmode=require

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

### 4.3 Token Generation and Validation

```sql
-- Generate JWT token for endpoint access
CREATE OR REPLACE FUNCTION platform.generate_endpoint_token(
    p_endpoint_id UUID,
    p_user_id UUID,
    p_db_role VARCHAR(63),
    p_branch_permission branch_permission_level,
    p_valid_duration INTERVAL DEFAULT INTERVAL '1 hour'
) RETURNS JSON AS $$
DECLARE
    v_endpoint platform.compute_endpoints%ROWTYPE;
    v_token_id VARCHAR(64);
    v_credential_id UUID;
    v_jwt_payload JSON;
    v_jwt_token TEXT;
BEGIN
    -- Get endpoint
    SELECT * INTO v_endpoint FROM platform.compute_endpoints WHERE endpoint_id = p_endpoint_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Endpoint not found';
    END IF;

    -- Verify user has access to branch
    IF platform.get_branch_permissions(p_user_id, v_endpoint.branch_id) IS NULL THEN
        RAISE EXCEPTION 'User does not have access to branch';
    END IF;

    -- Generate token ID
    v_token_id := encode(gen_random_bytes(32), 'hex');

    -- Store credential
    INSERT INTO platform.endpoint_credentials (
        endpoint_id, credential_type, token_id, db_role, branch_permission,
        valid_until, created_by
    ) VALUES (
        p_endpoint_id, 'jwt', v_token_id, p_db_role, p_branch_permission,
        NOW() + p_valid_duration, p_user_id
    ) RETURNING credential_id INTO v_credential_id;

    -- Build JWT payload (actual signing done in application layer)
    v_jwt_payload := json_build_object(
        'iss', 'orochi.cloud',
        'sub', p_user_id,
        'aud', v_endpoint.endpoint_host,
        'exp', EXTRACT(EPOCH FROM NOW() + p_valid_duration)::bigint,
        'iat', EXTRACT(EPOCH FROM NOW())::bigint,
        'jti', v_token_id,
        'orochi', json_build_object(
            'endpoint_id', p_endpoint_id,
            'branch_id', v_endpoint.branch_id,
            'cluster_id', v_endpoint.cluster_id,
            'db_role', p_db_role,
            'permission', p_branch_permission
        )
    );

    -- Return payload (application will sign)
    RETURN json_build_object(
        'credential_id', v_credential_id,
        'token_id', v_token_id,
        'payload', v_jwt_payload,
        'endpoint_host', v_endpoint.endpoint_host,
        'endpoint_port', v_endpoint.endpoint_port
    );
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Generate connection string with embedded token
CREATE OR REPLACE FUNCTION platform.generate_connection_string(
    p_credential_id UUID,
    p_signed_token TEXT,
    p_database VARCHAR(63) DEFAULT 'postgres',
    p_driver_type VARCHAR(50) DEFAULT 'psql'
) RETURNS TEXT AS $$
DECLARE
    v_credential platform.endpoint_credentials%ROWTYPE;
    v_endpoint platform.compute_endpoints%ROWTYPE;
    v_template TEXT;
    v_conn_string TEXT;
BEGIN
    -- Get credential and endpoint
    SELECT ec.*, ce.*
    INTO v_credential, v_endpoint
    FROM platform.endpoint_credentials ec
    JOIN platform.compute_endpoints ce ON ec.endpoint_id = ce.endpoint_id
    WHERE ec.credential_id = p_credential_id;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Credential not found';
    END IF;

    -- Get template
    SELECT template INTO v_template
    FROM platform.connection_string_templates
    WHERE endpoint_id = v_endpoint.endpoint_id AND driver_type = p_driver_type;

    IF v_template IS NULL THEN
        -- Default template
        v_template := 'postgresql://{user}:{password}@{host}:{port}/{database}?sslmode=require';
    END IF;

    -- Build connection string
    v_conn_string := v_template;
    v_conn_string := replace(v_conn_string, '{user}', 'orochi_token');
    v_conn_string := replace(v_conn_string, '{password}', p_signed_token);
    v_conn_string := replace(v_conn_string, '{host}', v_endpoint.endpoint_host);
    v_conn_string := replace(v_conn_string, '{port}', v_endpoint.endpoint_port::text);
    v_conn_string := replace(v_conn_string, '{database}', p_database);

    RETURN v_conn_string;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

### 4.4 PgCat Integration Configuration

```toml
# pgcat.toml - PgCat configuration for Orochi Cloud

[general]
host = "0.0.0.0"
port = 5432
connect_timeout = 5000
idle_timeout = 30000
shutdown_timeout = 5000

# TLS Configuration
[general.tls]
server_cert = "/etc/pgcat/server.crt"
server_key = "/etc/pgcat/server.key"

# Authentication Plugin for Orochi
[auth]
# Custom authentication module
type = "custom"
module = "orochi_auth"

[auth.orochi]
# JWT validation
jwt_public_key_path = "/etc/pgcat/jwt_public_key.pem"
jwt_issuer = "orochi.cloud"
jwt_audience_pattern = "*.orochi.cloud"

# Token validation endpoint (fallback for opaque tokens)
validation_endpoint = "http://auth-service:8080/validate"
validation_timeout_ms = 100
validation_cache_ttl_seconds = 60

# Connection context injection
[auth.orochi.context]
# These session variables are set after authentication
set_role = true                     # SET ROLE based on token
set_branch_id = true                # SET orochi.branch_id
set_user_id = true                  # SET orochi.user_id
set_permission_level = true         # SET orochi.permission_level

# Pool configuration per endpoint
[pools.default]
pool_mode = "transaction"
default_role = "orochi_default"
min_pool_size = 0
max_pool_size = 100

# Sharding configuration (integrates with Orochi distribution)
[pools.default.sharding]
enabled = true
sharding_function = "orochi_hash"   # Use Orochi's hash function
shard_count = 32

# Health check
[pools.default.health_check]
query = "SELECT 1"
interval = 30000
timeout = 5000

# Query routing rules
[query_routing]
# Route read-only queries to replicas when available
read_write_split = true
read_query_keywords = ["SELECT", "SHOW", "EXPLAIN"]

# Branch-specific routing
[query_routing.branch]
# Queries are routed based on orochi.branch_id session variable
route_by_session_var = "orochi.branch_id"
```

### 4.5 PgCat Authentication Module (Rust)

```rust
// src/auth/orochi_auth.rs - Custom authentication module for PgCat

use jsonwebtoken::{decode, DecodingKey, Validation, Algorithm};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;

#[derive(Debug, Serialize, Deserialize)]
pub struct OrochiTokenClaims {
    pub iss: String,
    pub sub: String,      // user_id
    pub aud: String,
    pub exp: i64,
    pub iat: i64,
    pub jti: String,      // token_id
    pub orochi: OrochiContext,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct OrochiContext {
    pub endpoint_id: String,
    pub branch_id: String,
    pub cluster_id: String,
    pub db_role: String,
    pub permission: String,
}

pub struct OrochiAuthenticator {
    jwt_public_key: DecodingKey,
    validation_endpoint: String,
    token_cache: Arc<RwLock<HashMap<String, CachedValidation>>>,
    cache_ttl: std::time::Duration,
}

#[derive(Clone)]
struct CachedValidation {
    claims: OrochiTokenClaims,
    cached_at: std::time::Instant,
}

impl OrochiAuthenticator {
    pub fn new(config: &AuthConfig) -> Result<Self, AuthError> {
        let public_key = std::fs::read_to_string(&config.jwt_public_key_path)?;
        let decoding_key = DecodingKey::from_rsa_pem(public_key.as_bytes())?;

        Ok(Self {
            jwt_public_key: decoding_key,
            validation_endpoint: config.validation_endpoint.clone(),
            token_cache: Arc::new(RwLock::new(HashMap::new())),
            cache_ttl: std::time::Duration::from_secs(config.validation_cache_ttl_seconds),
        })
    }

    /// Authenticate a connection using the provided token
    pub async fn authenticate(
        &self,
        username: &str,
        password: &str,  // Contains the JWT token
        database: &str,
        client_ip: &str,
    ) -> Result<AuthResult, AuthError> {
        // Check if this is a token-based authentication
        if username != "orochi_token" {
            return self.authenticate_password(username, password, database).await;
        }

        // Try JWT validation
        let claims = self.validate_jwt(password).await?;

        // Verify audience matches the endpoint
        // (In production, extract endpoint from connection)

        // Build session context
        let context = SessionContext {
            user_id: claims.sub.clone(),
            branch_id: claims.orochi.branch_id.clone(),
            db_role: claims.orochi.db_role.clone(),
            permission_level: claims.orochi.permission.clone(),
            endpoint_id: claims.orochi.endpoint_id.clone(),
        };

        // Build startup parameters to inject
        let mut startup_params = HashMap::new();
        startup_params.insert("orochi.user_id".to_string(), claims.sub);
        startup_params.insert("orochi.branch_id".to_string(), claims.orochi.branch_id);
        startup_params.insert("orochi.permission_level".to_string(), claims.orochi.permission);

        Ok(AuthResult {
            authenticated: true,
            db_role: claims.orochi.db_role,
            startup_params,
            context,
        })
    }

    async fn validate_jwt(&self, token: &str) -> Result<OrochiTokenClaims, AuthError> {
        // Check cache first
        let cache = self.token_cache.read().await;
        if let Some(cached) = cache.get(token) {
            if cached.cached_at.elapsed() < self.cache_ttl {
                return Ok(cached.claims.clone());
            }
        }
        drop(cache);

        // Validate JWT
        let mut validation = Validation::new(Algorithm::RS256);
        validation.set_issuer(&["orochi.cloud"]);
        validation.validate_exp = true;

        let token_data = decode::<OrochiTokenClaims>(
            token,
            &self.jwt_public_key,
            &validation,
        ).map_err(|e| AuthError::InvalidToken(e.to_string()))?;

        // Cache the validation
        let mut cache = self.token_cache.write().await;
        cache.insert(token.to_string(), CachedValidation {
            claims: token_data.claims.clone(),
            cached_at: std::time::Instant::now(),
        });

        Ok(token_data.claims)
    }

    /// Generate startup commands to run after connection
    pub fn get_startup_commands(&self, context: &SessionContext) -> Vec<String> {
        vec![
            format!("SET ROLE {}", quote_identifier(&context.db_role)),
            format!("SET orochi.branch_id = '{}'", context.branch_id),
            format!("SET orochi.user_id = '{}'", context.user_id),
            format!("SET orochi.permission_level = '{}'", context.permission_level),
        ]
    }
}

#[derive(Debug)]
pub struct AuthResult {
    pub authenticated: bool,
    pub db_role: String,
    pub startup_params: HashMap<String, String>,
    pub context: SessionContext,
}

#[derive(Debug, Clone)]
pub struct SessionContext {
    pub user_id: String,
    pub branch_id: String,
    pub db_role: String,
    pub permission_level: String,
    pub endpoint_id: String,
}
```

---

## 5. Database Roles Mapped to App Users

### 5.1 Dynamic Role Architecture

```
+------------------------------------------------------------------+
|                    DYNAMIC ROLE MANAGEMENT                         |
+------------------------------------------------------------------+
|                                                                    |
|  Application User                     PostgreSQL Cluster           |
|  (Platform Identity)                  (Database Identity)          |
|                                                                    |
|  +------------------+                 +------------------------+   |
|  |   app_user_123   |  ============>  |  orochi_u_abc123def   |   |
|  | (platform.users) |    Dynamic      |  (pg_catalog.pg_roles) |   |
|  +------------------+    Mapping      +------------------------+   |
|         |                                       |                  |
|         |                                       |                  |
|         v                                       v                  |
|  +------------------+                 +------------------------+   |
|  |   Role Template  |  ============>  |   GRANT permissions    |   |
|  | "developer"      |    Apply        |   ON branch schemas    |   |
|  +------------------+                 +------------------------+   |
|                                                                    |
|  Role Naming Convention:                                           |
|  - orochi_u_{hash8}    : User role                                |
|  - orochi_t_{hash8}    : Team role                                |
|  - orochi_s_{hash8}    : Service account role                     |
|  - orochi_b_{branch_id}: Branch-specific role                     |
|                                                                    |
+------------------------------------------------------------------+
```

### 5.2 Role Template System

```sql
-- Role templates define permission patterns
CREATE TABLE platform.role_templates (
    template_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id                  UUID REFERENCES platform.organizations(org_id),

    -- Template Identification
    template_name           VARCHAR(63) NOT NULL,
    display_name            VARCHAR(255) NOT NULL,
    description             TEXT,

    -- Base Permissions (applied to all schemas)
    base_privileges         JSONB NOT NULL,

    -- Schema-Specific Permissions
    schema_privileges       JSONB NOT NULL DEFAULT '{}',

    -- Table-Level Permissions (patterns)
    table_privileges        JSONB NOT NULL DEFAULT '{}',

    -- Function/Procedure Permissions
    function_privileges     JSONB NOT NULL DEFAULT '{}',

    -- Sequence Permissions
    sequence_privileges     JSONB NOT NULL DEFAULT '{}',

    -- Row-Level Security Policies
    rls_policies            JSONB NOT NULL DEFAULT '{}',

    -- PostgreSQL Role Options
    role_options            JSONB NOT NULL DEFAULT '{
        "login": true,
        "createdb": false,
        "createrole": false,
        "inherit": true,
        "replication": false,
        "bypassrls": false,
        "connection_limit": 10
    }',

    -- Template Status
    is_system               BOOLEAN NOT NULL DEFAULT FALSE,
    is_active               BOOLEAN NOT NULL DEFAULT TRUE,

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_template_name UNIQUE (org_id, template_name)
);

-- System default templates
INSERT INTO platform.role_templates (org_id, template_name, display_name, description, base_privileges, is_system) VALUES
(NULL, 'superuser', 'Superuser', 'Full database access', '{
    "all_schemas": ["ALL PRIVILEGES"]
}', TRUE),

(NULL, 'admin', 'Database Admin', 'DDL and DML on all objects', '{
    "all_schemas": ["USAGE", "CREATE"],
    "all_tables": ["SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER"],
    "all_sequences": ["USAGE", "SELECT", "UPDATE"],
    "all_functions": ["EXECUTE"]
}', TRUE),

(NULL, 'developer', 'Developer', 'DML access, limited DDL', '{
    "all_schemas": ["USAGE"],
    "all_tables": ["SELECT", "INSERT", "UPDATE", "DELETE"],
    "all_sequences": ["USAGE", "SELECT"],
    "all_functions": ["EXECUTE"]
}', TRUE),

(NULL, 'analyst', 'Analyst', 'Read-only access', '{
    "all_schemas": ["USAGE"],
    "all_tables": ["SELECT"],
    "all_sequences": ["SELECT"],
    "all_functions": ["EXECUTE"]
}', TRUE),

(NULL, 'readonly', 'Read Only', 'Read-only, no function execution', '{
    "all_schemas": ["USAGE"],
    "all_tables": ["SELECT"]
}', TRUE);

-- User to role mapping
CREATE TABLE platform.user_role_mappings (
    mapping_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),

    -- Source Identity
    user_id                 UUID REFERENCES platform.users(user_id),
    team_id                 UUID REFERENCES platform.teams(team_id),
    service_account_id      UUID,

    -- Target
    cluster_id              UUID NOT NULL REFERENCES platform.clusters(cluster_id),
    branch_id               UUID REFERENCES platform.branches(branch_id),

    -- Role Configuration
    template_id             UUID REFERENCES platform.role_templates(template_id),
    custom_privileges       JSONB,                   -- Override template

    -- Generated PostgreSQL Role
    pg_role_name            VARCHAR(63) NOT NULL,
    pg_role_created         BOOLEAN NOT NULL DEFAULT FALSE,

    -- Status
    status                  mapping_status NOT NULL DEFAULT 'pending',
    last_sync_at            TIMESTAMPTZ,
    sync_error              TEXT,

    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT exactly_one_identity CHECK (
        (user_id IS NOT NULL)::int +
        (team_id IS NOT NULL)::int +
        (service_account_id IS NOT NULL)::int = 1
    ),
    CONSTRAINT unique_user_cluster_branch UNIQUE (user_id, cluster_id, branch_id)
);

CREATE TYPE mapping_status AS ENUM (
    'pending',
    'active',
    'syncing',
    'error',
    'suspended',
    'deleted'
);
```

### 5.3 Role Creation and Management

```sql
-- Generate unique PostgreSQL role name
CREATE OR REPLACE FUNCTION platform.generate_pg_role_name(
    p_identity_type VARCHAR(10),  -- 'user', 'team', 'service'
    p_identity_id UUID
) RETURNS VARCHAR(63) AS $$
DECLARE
    v_hash VARCHAR(8);
    v_prefix VARCHAR(10);
BEGIN
    -- Generate 8-character hash from UUID
    v_hash := substring(md5(p_identity_id::text) from 1 for 8);

    -- Determine prefix
    v_prefix := CASE p_identity_type
        WHEN 'user' THEN 'orochi_u_'
        WHEN 'team' THEN 'orochi_t_'
        WHEN 'service' THEN 'orochi_s_'
        ELSE 'orochi_x_'
    END;

    RETURN v_prefix || v_hash;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- Create database role from mapping
CREATE OR REPLACE FUNCTION platform.create_db_role(p_mapping_id UUID)
RETURNS BOOLEAN AS $$
DECLARE
    v_mapping platform.user_role_mappings%ROWTYPE;
    v_template platform.role_templates%ROWTYPE;
    v_role_options JSONB;
    v_sql TEXT;
BEGIN
    -- Get mapping
    SELECT * INTO v_mapping FROM platform.user_role_mappings WHERE mapping_id = p_mapping_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Mapping not found';
    END IF;

    -- Get template
    SELECT * INTO v_template FROM platform.role_templates WHERE template_id = v_mapping.template_id;

    -- Merge role options
    v_role_options := COALESCE(v_template.role_options, '{}'::jsonb);

    -- Build CREATE ROLE statement
    v_sql := format('CREATE ROLE %I', v_mapping.pg_role_name);

    IF (v_role_options->>'login')::boolean THEN
        v_sql := v_sql || ' LOGIN';
    ELSE
        v_sql := v_sql || ' NOLOGIN';
    END IF;

    IF (v_role_options->>'createdb')::boolean THEN
        v_sql := v_sql || ' CREATEDB';
    END IF;

    IF (v_role_options->>'createrole')::boolean THEN
        v_sql := v_sql || ' CREATEROLE';
    END IF;

    IF v_role_options->>'connection_limit' IS NOT NULL THEN
        v_sql := v_sql || format(' CONNECTION LIMIT %s', v_role_options->>'connection_limit');
    END IF;

    -- Execute on target cluster (via dblink or FDW)
    -- In production, this would be executed on the actual cluster
    -- PERFORM dblink_exec(v_cluster_connection, v_sql);

    -- Update mapping
    UPDATE platform.user_role_mappings
    SET pg_role_created = TRUE,
        status = 'active',
        last_sync_at = NOW()
    WHERE mapping_id = p_mapping_id;

    RETURN TRUE;
END;
$$ LANGUAGE plpgsql;

-- Apply template privileges to role
CREATE OR REPLACE FUNCTION platform.apply_role_privileges(p_mapping_id UUID)
RETURNS BOOLEAN AS $$
DECLARE
    v_mapping platform.user_role_mappings%ROWTYPE;
    v_template platform.role_templates%ROWTYPE;
    v_privileges JSONB;
    v_schema_record RECORD;
    v_sql TEXT;
BEGIN
    -- Get mapping and template
    SELECT * INTO v_mapping FROM platform.user_role_mappings WHERE mapping_id = p_mapping_id;
    SELECT * INTO v_template FROM platform.role_templates WHERE template_id = v_mapping.template_id;

    -- Merge custom privileges
    v_privileges := v_template.base_privileges || COALESCE(v_mapping.custom_privileges, '{}'::jsonb);

    -- Apply schema-level privileges
    IF v_privileges->>'all_schemas' IS NOT NULL THEN
        FOR v_schema_record IN
            SELECT schema_name FROM information_schema.schemata
            WHERE schema_name NOT IN ('pg_catalog', 'information_schema', 'pg_toast')
        LOOP
            -- Grant schema usage
            FOR v_sql IN SELECT jsonb_array_elements_text(v_privileges->'all_schemas')
            LOOP
                EXECUTE format('GRANT %s ON SCHEMA %I TO %I',
                    v_sql, v_schema_record.schema_name, v_mapping.pg_role_name);
            END LOOP;
        END LOOP;
    END IF;

    -- Apply table-level privileges
    IF v_privileges->>'all_tables' IS NOT NULL THEN
        EXECUTE format('GRANT %s ON ALL TABLES IN SCHEMA public TO %I',
            array_to_string(ARRAY(SELECT jsonb_array_elements_text(v_privileges->'all_tables')), ', '),
            v_mapping.pg_role_name);

        -- Set default privileges for future tables
        EXECUTE format('ALTER DEFAULT PRIVILEGES GRANT %s ON TABLES TO %I',
            array_to_string(ARRAY(SELECT jsonb_array_elements_text(v_privileges->'all_tables')), ', '),
            v_mapping.pg_role_name);
    END IF;

    -- Apply sequence privileges
    IF v_privileges->>'all_sequences' IS NOT NULL THEN
        EXECUTE format('GRANT %s ON ALL SEQUENCES IN SCHEMA public TO %I',
            array_to_string(ARRAY(SELECT jsonb_array_elements_text(v_privileges->'all_sequences')), ', '),
            v_mapping.pg_role_name);
    END IF;

    -- Apply function privileges
    IF v_privileges->>'all_functions' IS NOT NULL THEN
        EXECUTE format('GRANT %s ON ALL FUNCTIONS IN SCHEMA public TO %I',
            array_to_string(ARRAY(SELECT jsonb_array_elements_text(v_privileges->'all_functions')), ', '),
            v_mapping.pg_role_name);
    END IF;

    RETURN TRUE;
END;
$$ LANGUAGE plpgsql;

-- Role cleanup when user is deleted
CREATE OR REPLACE FUNCTION platform.cleanup_user_roles(p_user_id UUID)
RETURNS INTEGER AS $$
DECLARE
    v_mapping RECORD;
    v_count INTEGER := 0;
BEGIN
    -- Mark mappings as deleted
    FOR v_mapping IN
        SELECT * FROM platform.user_role_mappings
        WHERE user_id = p_user_id AND status != 'deleted'
    LOOP
        -- Revoke all privileges
        -- PERFORM platform.revoke_role_privileges(v_mapping.mapping_id);

        -- Drop role from cluster
        -- PERFORM dblink_exec(v_cluster_connection,
        --     format('DROP ROLE IF EXISTS %I', v_mapping.pg_role_name));

        -- Update mapping
        UPDATE platform.user_role_mappings
        SET status = 'deleted',
            updated_at = NOW()
        WHERE mapping_id = v_mapping.mapping_id;

        v_count := v_count + 1;
    END LOOP;

    RETURN v_count;
END;
$$ LANGUAGE plpgsql;
```

### 5.4 Performance at Scale

```sql
-- Role caching layer for high-performance lookup
CREATE TABLE platform.role_cache (
    cache_key               VARCHAR(255) PRIMARY KEY,
    pg_role_name            VARCHAR(63) NOT NULL,
    permissions             JSONB NOT NULL,
    cached_at               TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ NOT NULL,

    CONSTRAINT cache_not_expired CHECK (expires_at > cached_at)
);

-- Index for cache cleanup
CREATE INDEX idx_role_cache_expires ON platform.role_cache(expires_at);

-- Batch role creation for performance
CREATE OR REPLACE FUNCTION platform.batch_create_roles(
    p_cluster_id UUID,
    p_user_ids UUID[]
) RETURNS INTEGER AS $$
DECLARE
    v_user_id UUID;
    v_count INTEGER := 0;
    v_batch_sql TEXT := '';
BEGIN
    -- Build batch SQL
    FOREACH v_user_id IN ARRAY p_user_ids
    LOOP
        v_batch_sql := v_batch_sql || format(
            'CREATE ROLE %I LOGIN CONNECTION LIMIT 10; ',
            platform.generate_pg_role_name('user', v_user_id)
        );
        v_count := v_count + 1;
    END LOOP;

    -- Execute batch (would use dblink in production)
    -- PERFORM dblink_exec(v_cluster_connection, v_batch_sql);

    RETURN v_count;
END;
$$ LANGUAGE plpgsql;

-- Statistics on role management
CREATE TABLE platform.role_stats (
    stat_id                 BIGSERIAL PRIMARY KEY,
    cluster_id              UUID NOT NULL,
    stat_time               TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    -- Counts
    total_roles             INTEGER NOT NULL,
    active_roles            INTEGER NOT NULL,
    pending_roles           INTEGER NOT NULL,
    error_roles             INTEGER NOT NULL,

    -- Performance
    avg_create_time_ms      DECIMAL(10,2),
    avg_sync_time_ms        DECIMAL(10,2),

    -- Storage
    pg_roles_table_size     BIGINT
);

-- Index for stats queries
CREATE INDEX idx_role_stats_cluster_time ON platform.role_stats(cluster_id, stat_time DESC);
```

---

## 6. Distributed Architecture Integration

### 6.1 Authentication State Replication via Raft

```
+------------------------------------------------------------------+
|           AUTHENTICATION STATE REPLICATION                         |
+------------------------------------------------------------------+
|                                                                    |
|  Control Plane                                                     |
|  +----------------------------------------------------------+     |
|  |  Auth Service                                             |     |
|  |  +------------------+                                     |     |
|  |  | Auth State       |                                     |     |
|  |  | - Sessions       |                                     |     |
|  |  | - Tokens         |                                     |     |
|  |  | - Credentials    |                                     |     |
|  |  +------------------+                                     |     |
|  |           |                                               |     |
|  |           v                                               |     |
|  |  +------------------+     +------------------+            |     |
|  |  | Raft Leader      |<--->| Raft Followers   |            |     |
|  |  | (Coordinator)    |     | (Workers)        |            |     |
|  |  +------------------+     +------------------+            |     |
|  +----------------------------------------------------------+     |
|                     |                                              |
|                     v                                              |
|  Data Plane                                                        |
|  +----------------------------------------------------------+     |
|  |                                                           |     |
|  |  +-------------+  +-------------+  +-------------+        |     |
|  |  | Node 1      |  | Node 2      |  | Node 3      |        |     |
|  |  | Auth Cache  |  | Auth Cache  |  | Auth Cache  |        |     |
|  |  +-------------+  +-------------+  +-------------+        |     |
|  |        ^                ^                ^                |     |
|  |        |                |                |                |     |
|  |        +----------------+----------------+                |     |
|  |                         |                                 |     |
|  |                  Cache Invalidation                       |     |
|  |                  via Raft Log                             |     |
|  +----------------------------------------------------------+     |
|                                                                    |
+------------------------------------------------------------------+
```

### 6.2 Raft Log Entries for Auth State

```c
// src/auth/auth_raft.h - Authentication state in Raft log

#ifndef OROCHI_AUTH_RAFT_H
#define OROCHI_AUTH_RAFT_H

#include "postgres.h"
#include "../consensus/raft.h"

/* Auth-specific Raft log entry types */
typedef enum AuthRaftEntryType
{
    AUTH_RAFT_SESSION_CREATE = 100,
    AUTH_RAFT_SESSION_INVALIDATE,
    AUTH_RAFT_SESSION_REFRESH,
    AUTH_RAFT_TOKEN_ISSUE,
    AUTH_RAFT_TOKEN_REVOKE,
    AUTH_RAFT_CREDENTIAL_REGISTER,
    AUTH_RAFT_CREDENTIAL_REVOKE,
    AUTH_RAFT_ROLE_CREATE,
    AUTH_RAFT_ROLE_DROP,
    AUTH_RAFT_ROLE_GRANT,
    AUTH_RAFT_ROLE_REVOKE,
    AUTH_RAFT_BRANCH_PERMISSION_GRANT,
    AUTH_RAFT_BRANCH_PERMISSION_REVOKE,
    AUTH_RAFT_CACHE_INVALIDATE
} AuthRaftEntryType;

/* Session create entry */
typedef struct AuthSessionCreateEntry
{
    char            session_id[64];
    char            user_id[64];
    char            credential_id[64];
    TimestampTz     created_at;
    TimestampTz     expires_at;
    char            ip_address[64];
} AuthSessionCreateEntry;

/* Token issue entry */
typedef struct AuthTokenIssueEntry
{
    char            token_id[64];
    char            user_id[64];
    char            endpoint_id[64];
    char            branch_id[64];
    char            db_role[64];
    char            permission_level[20];
    TimestampTz     issued_at;
    TimestampTz     expires_at;
} AuthTokenIssueEntry;

/* Role sync entry */
typedef struct AuthRoleSyncEntry
{
    char            cluster_id[64];
    char            pg_role_name[64];
    char            user_id[64];
    char            template_name[64];
    char            privileges[4096];    /* JSON */
} AuthRoleSyncEntry;

/* Cache invalidation entry */
typedef struct AuthCacheInvalidateEntry
{
    char            cache_type[32];      /* session, token, role, permission */
    char            cache_key[256];
    TimestampTz     invalidated_at;
} AuthCacheInvalidateEntry;

/* Submit auth state change to Raft */
extern uint64 auth_raft_submit(AuthRaftEntryType type, void *data, int32 size);

/* Apply auth Raft entry to local state */
extern void auth_raft_apply(RaftLogEntry *entry, void *context);

/* Register auth state machine with Raft */
extern void auth_raft_init(RaftNode *node);

#endif /* OROCHI_AUTH_RAFT_H */
```

### 6.3 Cross-Node Authentication Cache

```c
// src/auth/auth_cache.h - Distributed authentication cache

#ifndef OROCHI_AUTH_CACHE_H
#define OROCHI_AUTH_CACHE_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/hsearch.h"

/* Cache configuration */
#define AUTH_CACHE_MAX_SESSIONS     100000
#define AUTH_CACHE_MAX_TOKENS       500000
#define AUTH_CACHE_MAX_ROLES        50000
#define AUTH_CACHE_TTL_SECONDS      300

/* Session cache entry */
typedef struct SessionCacheEntry
{
    char            session_id[64];
    char            user_id[64];
    char            branch_id[64];
    char            permission_level[20];
    TimestampTz     expires_at;
    TimestampTz     cached_at;
    bool            is_valid;
} SessionCacheEntry;

/* Token cache entry */
typedef struct TokenCacheEntry
{
    char            token_id[64];
    char            user_id[64];
    char            endpoint_id[64];
    char            branch_id[64];
    char            db_role[64];
    char            permission_level[20];
    TimestampTz     expires_at;
    TimestampTz     cached_at;
    bool            is_valid;
} TokenCacheEntry;

/* Role cache entry */
typedef struct RoleCacheEntry
{
    char            cache_key[256];      /* user_id:cluster_id:branch_id */
    char            pg_role_name[64];
    char            permissions[4096];   /* JSON */
    TimestampTz     cached_at;
    bool            is_valid;
} RoleCacheEntry;

/* Shared memory state */
typedef struct AuthCacheSharedState
{
    LWLock         *sessions_lock;
    LWLock         *tokens_lock;
    LWLock         *roles_lock;
    int             session_count;
    int             token_count;
    int             role_count;
    uint64          cache_hits;
    uint64          cache_misses;
    uint64          invalidations;
} AuthCacheSharedState;

/* Initialize auth cache in shared memory */
extern void auth_cache_init(void);
extern Size auth_cache_shmem_size(void);

/* Session cache operations */
extern bool auth_cache_session_get(const char *session_id, SessionCacheEntry *entry);
extern void auth_cache_session_put(SessionCacheEntry *entry);
extern void auth_cache_session_invalidate(const char *session_id);
extern void auth_cache_session_invalidate_user(const char *user_id);

/* Token cache operations */
extern bool auth_cache_token_get(const char *token_id, TokenCacheEntry *entry);
extern void auth_cache_token_put(TokenCacheEntry *entry);
extern void auth_cache_token_invalidate(const char *token_id);

/* Role cache operations */
extern bool auth_cache_role_get(const char *user_id, const char *cluster_id,
                                 const char *branch_id, RoleCacheEntry *entry);
extern void auth_cache_role_put(RoleCacheEntry *entry);
extern void auth_cache_role_invalidate(const char *cache_key);
extern void auth_cache_role_invalidate_cluster(const char *cluster_id);

/* Cache maintenance */
extern void auth_cache_cleanup_expired(void);
extern void auth_cache_get_stats(uint64 *hits, uint64 *misses, uint64 *invalidations);

#endif /* OROCHI_AUTH_CACHE_H */
```

### 6.4 C Implementation for Cache

```c
// src/auth/auth_cache.c - Authentication cache implementation

#include "postgres.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "auth_cache.h"

/* Static shared state */
static AuthCacheSharedState *AuthCacheState = NULL;
static HTAB *SessionCache = NULL;
static HTAB *TokenCache = NULL;
static HTAB *RoleCache = NULL;

Size
auth_cache_shmem_size(void)
{
    Size size = 0;

    /* Shared state */
    size = add_size(size, sizeof(AuthCacheSharedState));

    /* Session cache hash table */
    size = add_size(size, hash_estimate_size(AUTH_CACHE_MAX_SESSIONS,
                                              sizeof(SessionCacheEntry)));

    /* Token cache hash table */
    size = add_size(size, hash_estimate_size(AUTH_CACHE_MAX_TOKENS,
                                              sizeof(TokenCacheEntry)));

    /* Role cache hash table */
    size = add_size(size, hash_estimate_size(AUTH_CACHE_MAX_ROLES,
                                              sizeof(RoleCacheEntry)));

    return size;
}

void
auth_cache_init(void)
{
    bool found;
    HASHCTL hash_info;

    /* Initialize shared state */
    AuthCacheState = ShmemInitStruct("AuthCacheState",
                                     sizeof(AuthCacheSharedState),
                                     &found);

    if (!found)
    {
        LWLockPadded *locks = GetNamedLWLockTranche("orochi_auth");
        AuthCacheState->sessions_lock = &locks[0].lock;
        AuthCacheState->tokens_lock = &locks[1].lock;
        AuthCacheState->roles_lock = &locks[2].lock;
        AuthCacheState->session_count = 0;
        AuthCacheState->token_count = 0;
        AuthCacheState->role_count = 0;
        AuthCacheState->cache_hits = 0;
        AuthCacheState->cache_misses = 0;
        AuthCacheState->invalidations = 0;
    }

    /* Initialize session cache */
    memset(&hash_info, 0, sizeof(hash_info));
    hash_info.keysize = 64;  /* session_id */
    hash_info.entrysize = sizeof(SessionCacheEntry);

    SessionCache = ShmemInitHash("SessionCache",
                                  AUTH_CACHE_MAX_SESSIONS,
                                  AUTH_CACHE_MAX_SESSIONS,
                                  &hash_info,
                                  HASH_ELEM | HASH_STRINGS);

    /* Initialize token cache */
    hash_info.keysize = 64;  /* token_id */
    hash_info.entrysize = sizeof(TokenCacheEntry);

    TokenCache = ShmemInitHash("TokenCache",
                                AUTH_CACHE_MAX_TOKENS,
                                AUTH_CACHE_MAX_TOKENS,
                                &hash_info,
                                HASH_ELEM | HASH_STRINGS);

    /* Initialize role cache */
    hash_info.keysize = 256;  /* cache_key */
    hash_info.entrysize = sizeof(RoleCacheEntry);

    RoleCache = ShmemInitHash("RoleCache",
                               AUTH_CACHE_MAX_ROLES,
                               AUTH_CACHE_MAX_ROLES,
                               &hash_info,
                               HASH_ELEM | HASH_STRINGS);

    elog(LOG, "Orochi auth cache initialized: sessions=%d, tokens=%d, roles=%d",
         AUTH_CACHE_MAX_SESSIONS, AUTH_CACHE_MAX_TOKENS, AUTH_CACHE_MAX_ROLES);
}

bool
auth_cache_session_get(const char *session_id, SessionCacheEntry *entry)
{
    SessionCacheEntry *cached;
    bool found;
    TimestampTz now = GetCurrentTimestamp();

    LWLockAcquire(AuthCacheState->sessions_lock, LW_SHARED);

    cached = hash_search(SessionCache, session_id, HASH_FIND, &found);

    if (found && cached->is_valid && cached->expires_at > now)
    {
        memcpy(entry, cached, sizeof(SessionCacheEntry));
        AuthCacheState->cache_hits++;
        LWLockRelease(AuthCacheState->sessions_lock);
        return true;
    }

    AuthCacheState->cache_misses++;
    LWLockRelease(AuthCacheState->sessions_lock);
    return false;
}

void
auth_cache_session_put(SessionCacheEntry *entry)
{
    SessionCacheEntry *cached;
    bool found;

    entry->cached_at = GetCurrentTimestamp();

    LWLockAcquire(AuthCacheState->sessions_lock, LW_EXCLUSIVE);

    cached = hash_search(SessionCache, entry->session_id, HASH_ENTER, &found);

    if (!found)
        AuthCacheState->session_count++;

    memcpy(cached, entry, sizeof(SessionCacheEntry));

    LWLockRelease(AuthCacheState->sessions_lock);
}

void
auth_cache_session_invalidate(const char *session_id)
{
    SessionCacheEntry *cached;
    bool found;

    LWLockAcquire(AuthCacheState->sessions_lock, LW_EXCLUSIVE);

    cached = hash_search(SessionCache, session_id, HASH_FIND, &found);

    if (found)
    {
        cached->is_valid = false;
        AuthCacheState->invalidations++;
    }

    LWLockRelease(AuthCacheState->sessions_lock);
}

void
auth_cache_session_invalidate_user(const char *user_id)
{
    HASH_SEQ_STATUS status;
    SessionCacheEntry *entry;
    int invalidated = 0;

    LWLockAcquire(AuthCacheState->sessions_lock, LW_EXCLUSIVE);

    hash_seq_init(&status, SessionCache);

    while ((entry = hash_seq_search(&status)) != NULL)
    {
        if (strcmp(entry->user_id, user_id) == 0)
        {
            entry->is_valid = false;
            invalidated++;
        }
    }

    AuthCacheState->invalidations += invalidated;

    LWLockRelease(AuthCacheState->sessions_lock);

    elog(DEBUG1, "Invalidated %d sessions for user %s", invalidated, user_id);
}

/* Token and role cache operations follow same pattern */

void
auth_cache_cleanup_expired(void)
{
    HASH_SEQ_STATUS status;
    SessionCacheEntry *session;
    TokenCacheEntry *token;
    TimestampTz now = GetCurrentTimestamp();
    int removed_sessions = 0;
    int removed_tokens = 0;

    /* Clean up expired sessions */
    LWLockAcquire(AuthCacheState->sessions_lock, LW_EXCLUSIVE);
    hash_seq_init(&status, SessionCache);

    while ((session = hash_seq_search(&status)) != NULL)
    {
        if (!session->is_valid || session->expires_at < now)
        {
            hash_search(SessionCache, session->session_id, HASH_REMOVE, NULL);
            removed_sessions++;
        }
    }
    AuthCacheState->session_count -= removed_sessions;
    LWLockRelease(AuthCacheState->sessions_lock);

    /* Clean up expired tokens */
    LWLockAcquire(AuthCacheState->tokens_lock, LW_EXCLUSIVE);
    hash_seq_init(&status, TokenCache);

    while ((token = hash_seq_search(&status)) != NULL)
    {
        if (!token->is_valid || token->expires_at < now)
        {
            hash_search(TokenCache, token->token_id, HASH_REMOVE, NULL);
            removed_tokens++;
        }
    }
    AuthCacheState->token_count -= removed_tokens;
    LWLockRelease(AuthCacheState->tokens_lock);

    if (removed_sessions > 0 || removed_tokens > 0)
    {
        elog(LOG, "Auth cache cleanup: removed %d sessions, %d tokens",
             removed_sessions, removed_tokens);
    }
}

void
auth_cache_get_stats(uint64 *hits, uint64 *misses, uint64 *invalidations)
{
    *hits = AuthCacheState->cache_hits;
    *misses = AuthCacheState->cache_misses;
    *invalidations = AuthCacheState->invalidations;
}
```

### 6.5 GUC Configuration

```c
// Add to src/core/init.c

/* Authentication GUC variables */
bool    orochi_auth_cache_enabled = true;
int     orochi_auth_session_timeout_hours = 24;
int     orochi_auth_token_timeout_minutes = 60;
int     orochi_auth_cache_cleanup_interval_seconds = 60;
int     orochi_auth_max_sessions_per_user = 10;
int     orochi_auth_max_credentials_per_user = 5;
char   *orochi_auth_jwt_public_key_path = NULL;
char   *orochi_auth_jwt_issuer = "orochi.cloud";

static void
orochi_define_auth_gucs(void)
{
    DefineCustomBoolVariable("orochi.auth_cache_enabled",
                            "Enable authentication caching",
                            NULL,
                            &orochi_auth_cache_enabled,
                            true,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_session_timeout_hours",
                           "Session timeout in hours",
                           NULL,
                           &orochi_auth_session_timeout_hours,
                           24,
                           1,
                           720,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_token_timeout_minutes",
                           "Endpoint token timeout in minutes",
                           NULL,
                           &orochi_auth_token_timeout_minutes,
                           60,
                           5,
                           1440,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_cache_cleanup_interval_seconds",
                           "Auth cache cleanup interval",
                           NULL,
                           &orochi_auth_cache_cleanup_interval_seconds,
                           60,
                           10,
                           3600,
                           PGC_SIGHUP,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_max_sessions_per_user",
                           "Maximum concurrent sessions per user",
                           NULL,
                           &orochi_auth_max_sessions_per_user,
                           10,
                           1,
                           100,
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_max_credentials_per_user",
                           "Maximum WebAuthn credentials per user",
                           NULL,
                           &orochi_auth_max_credentials_per_user,
                           5,
                           1,
                           20,
                           PGC_USERSET,
                           0,
                           NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_jwt_public_key_path",
                              "Path to JWT public key file",
                              NULL,
                              &orochi_auth_jwt_public_key_path,
                              NULL,
                              PGC_POSTMASTER,
                              GUC_SUPERUSER_ONLY,
                              NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_jwt_issuer",
                              "Expected JWT issuer",
                              NULL,
                              &orochi_auth_jwt_issuer,
                              "orochi.cloud",
                              PGC_SIGHUP,
                              0,
                              NULL, NULL, NULL);
}
```

---

## 7. Implementation Plan

### 7.1 Phase 1: Core Authentication (Weeks 1-4)

| Task | Description | Priority |
|------|-------------|----------|
| WebAuthn Schema | Create credential storage tables | P0 |
| Registration Flow | Implement challenge/response | P0 |
| Authentication Flow | JWT generation and validation | P0 |
| Session Management | Session cache in shared memory | P0 |
| Basic Raft Integration | Session replication | P1 |

### 7.2 Phase 2: Branch Access Control (Weeks 5-8)

| Task | Description | Priority |
|------|-------------|----------|
| Branch Permissions Schema | Create permission tables | P0 |
| Permission Resolution | Inheritance and evaluation | P0 |
| Protection Rules | Implement branch protection | P1 |
| Cross-Branch Validation | Access validation functions | P1 |
| Shard Integration | Branch-aware shard routing | P1 |

### 7.3 Phase 3: Endpoint Authentication (Weeks 9-12)

| Task | Description | Priority |
|------|-------------|----------|
| Endpoint Credentials | Token generation and storage | P0 |
| PgCat Integration | Custom auth module | P0 |
| Connection String Generation | Template-based generation | P1 |
| Token Validation Cache | High-performance token lookup | P1 |

### 7.4 Phase 4: Dynamic Roles (Weeks 13-16)

| Task | Description | Priority |
|------|-------------|----------|
| Role Templates | Template system implementation | P0 |
| Dynamic Role Creation | Batch role provisioning | P0 |
| Privilege Application | Apply template to roles | P0 |
| Role Cleanup | Cascading deletion | P1 |
| Performance Optimization | Scale to 100K+ roles | P1 |

### 7.5 Success Metrics

| Metric | Target |
|--------|--------|
| WebAuthn registration latency | < 500ms |
| WebAuthn authentication latency | < 200ms |
| Token validation latency (cached) | < 5ms |
| Token validation latency (uncached) | < 50ms |
| Permission resolution latency | < 10ms |
| Role creation latency (batch 100) | < 2s |
| Auth cache hit ratio | > 95% |
| Cross-node cache consistency | < 100ms |

---

## 8. Appendix

### A. Security Considerations

1. **Credential Storage**: All sensitive data (private keys, tokens) encrypted with AES-256-GCM
2. **Token Security**: Short-lived JWTs (1 hour), refresh tokens (7 days) with rotation
3. **Replay Protection**: WebAuthn sign count verification, token nonces
4. **Rate Limiting**: Authentication attempts rate-limited per IP and user
5. **Audit Logging**: All authentication events logged with timestamps and context

### B. Compliance Mapping

| Feature | SOC 2 | GDPR | HIPAA |
|---------|-------|------|-------|
| MFA/WebAuthn | CC6.1 | Art. 32 | 164.312(d) |
| Session Management | CC6.6 | Art. 32 | 164.312(d) |
| Access Control | CC6.3 | Art. 25 | 164.312(a) |
| Audit Logging | CC7.2 | Art. 30 | 164.312(b) |
| Encryption | CC6.7 | Art. 32 | 164.312(e) |

### C. API Reference

See `sql/auth.sql` for complete SQL function signatures and `src/auth/*.h` for C API.

---

*Document History*

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0.0 | 2026-01-16 | Architecture Team | Initial design |

