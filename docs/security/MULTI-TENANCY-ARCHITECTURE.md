# Multi-Tenancy and Access Control Architecture

## Document Information

| Field | Value |
|-------|-------|
| Version | 1.0.0 |
| Status | Design |
| Author | Security Architecture Team |
| Last Updated | 2026-01-16 |

---

## 1. Executive Summary

This document defines the multi-tenancy and access control architecture for Orochi DB Managed Service. The design provides secure tenant isolation, hierarchical account management, and fine-grained access control while maintaining operational efficiency and cost-effectiveness.

---

## 2. Account Hierarchy Data Model

### 2.1 Entity Relationship Diagram

```
                        +------------------+
                        |   Organization   |
                        |------------------|
                        | org_id (PK)      |
                        | name             |
                        | billing_email    |
                        | plan_tier        |
                        | status           |
                        | created_at       |
                        +--------+---------+
                                 |
                 +---------------+---------------+
                 |                               |
        +--------v---------+           +--------v---------+
        |       Team       |           |   Organization   |
        |------------------|           |     Settings     |
        | team_id (PK)     |           |------------------|
        | org_id (FK)      |           | org_id (FK)      |
        | name             |           | sso_provider     |
        | description      |           | mfa_required     |
        | created_at       |           | ip_allowlist     |
        +--------+---------+           | audit_retention  |
                 |                     +------------------+
        +--------+--------+
        |                 |
+-------v-------+ +-------v-------+
|  TeamMember   | |    Project    |
|---------------| |---------------|
| team_id (FK)  | | project_id(PK)|
| user_id (FK)  | | org_id (FK)   |
| role_id (FK)  | | team_id (FK)  |
| joined_at     | | name          |
+---------------+ | region        |
                  | status        |
+---------------+ +-------+-------+
|     User      |         |
|---------------|         |
| user_id (PK)  | +-------v-------+
| email         | |   Database    |
| auth_provider | |---------------|
| mfa_enabled   | | database_id   |
| status        | | project_id(FK)|
| created_at    | | name          |
+-------+-------+ | isolation_mode|
        |         | tier          |
        |         | status        |
+-------v-------+ +-------+-------+
|   APIKey      |         |
|---------------|         |
| key_id (PK)   | +-------v-------+
| user_id (FK)  | | DatabaseUser  |
| scope         | |---------------|
| permissions   | | db_user_id(PK)|
| expires_at    | | database_id   |
| last_used     | | username      |
+---------------+ | role_id       |
                  | password_hash |
                  +---------------+
```

### 2.2 Core Entity Definitions

#### 2.2.1 Organization (Billing Entity)

```sql
CREATE TABLE platform.organizations (
    org_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name                VARCHAR(255) NOT NULL,
    slug                VARCHAR(63) UNIQUE NOT NULL,
    billing_email       VARCHAR(255) NOT NULL,
    billing_address     JSONB,
    plan_tier           plan_tier NOT NULL DEFAULT 'free',
    plan_limits         JSONB NOT NULL DEFAULT '{}',
    status              org_status NOT NULL DEFAULT 'active',
    sso_config          JSONB,
    security_config     JSONB NOT NULL DEFAULT '{
        "mfa_required": false,
        "ip_allowlist_enabled": false,
        "session_timeout_minutes": 480,
        "password_policy": "standard"
    }',
    metadata            JSONB DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    suspended_at        TIMESTAMPTZ,
    deleted_at          TIMESTAMPTZ,

    CONSTRAINT valid_slug CHECK (slug ~ '^[a-z][a-z0-9-]*[a-z0-9]$')
);

CREATE TYPE plan_tier AS ENUM (
    'free',           -- Limited resources, shared infrastructure
    'starter',        -- Small teams, basic isolation
    'professional',   -- Advanced features, dedicated resources
    'enterprise',     -- Full isolation, SLA guarantees
    'custom'          -- Custom pricing and features
);

CREATE TYPE org_status AS ENUM (
    'active',
    'suspended',
    'pending_deletion',
    'deleted'
);

-- Plan limits structure
-- {
--   "max_teams": 5,
--   "max_users": 25,
--   "max_projects": 10,
--   "max_databases": 50,
--   "max_storage_gb": 100,
--   "max_compute_units": 16,
--   "max_connections": 500,
--   "features": ["columnar", "timeseries", "vector"],
--   "support_tier": "standard",
--   "sla_percentage": 99.9
-- }
```

#### 2.2.2 Team

```sql
CREATE TABLE platform.teams (
    team_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    name                VARCHAR(255) NOT NULL,
    slug                VARCHAR(63) NOT NULL,
    description         TEXT,
    default_role_id     UUID REFERENCES platform.roles(role_id),
    settings            JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_team_slug_per_org UNIQUE (org_id, slug),
    CONSTRAINT valid_team_slug CHECK (slug ~ '^[a-z][a-z0-9-]*[a-z0-9]$')
);

CREATE INDEX idx_teams_org ON platform.teams(org_id);
```

#### 2.2.3 User

```sql
CREATE TABLE platform.users (
    user_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email               VARCHAR(255) UNIQUE NOT NULL,
    email_verified      BOOLEAN NOT NULL DEFAULT FALSE,
    password_hash       VARCHAR(255),  -- NULL for SSO-only users
    auth_provider       auth_provider NOT NULL DEFAULT 'local',
    auth_provider_id    VARCHAR(255),  -- External provider user ID
    display_name        VARCHAR(255),
    avatar_url          TEXT,
    mfa_enabled         BOOLEAN NOT NULL DEFAULT FALSE,
    mfa_secret          VARCHAR(255),  -- Encrypted TOTP secret
    mfa_backup_codes    TEXT[],        -- Encrypted backup codes
    status              user_status NOT NULL DEFAULT 'active',
    last_login_at       TIMESTAMPTZ,
    last_login_ip       INET,
    failed_login_count  INTEGER NOT NULL DEFAULT 0,
    locked_until        TIMESTAMPTZ,
    password_changed_at TIMESTAMPTZ,
    preferences         JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT valid_email CHECK (email ~* '^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$')
);

CREATE TYPE auth_provider AS ENUM (
    'local',          -- Email/password
    'google',         -- Google OAuth
    'github',         -- GitHub OAuth
    'microsoft',      -- Microsoft Azure AD
    'okta',           -- Okta SAML/OIDC
    'saml',           -- Generic SAML
    'oidc'            -- Generic OIDC
);

CREATE TYPE user_status AS ENUM (
    'pending',        -- Email not verified
    'active',
    'suspended',
    'deleted'
);
```

#### 2.2.4 Organization Membership

```sql
CREATE TABLE platform.organization_members (
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    user_id             UUID NOT NULL REFERENCES platform.users(user_id),
    role_id             UUID NOT NULL REFERENCES platform.roles(role_id),
    invited_by          UUID REFERENCES platform.users(user_id),
    invited_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    accepted_at         TIMESTAMPTZ,
    status              membership_status NOT NULL DEFAULT 'pending',

    PRIMARY KEY (org_id, user_id)
);

CREATE TYPE membership_status AS ENUM (
    'pending',
    'active',
    'suspended',
    'removed'
);

CREATE INDEX idx_org_members_user ON platform.organization_members(user_id);
```

#### 2.2.5 Team Membership

```sql
CREATE TABLE platform.team_members (
    team_id             UUID NOT NULL REFERENCES platform.teams(team_id),
    user_id             UUID NOT NULL REFERENCES platform.users(user_id),
    role_id             UUID NOT NULL REFERENCES platform.roles(role_id),
    added_by            UUID REFERENCES platform.users(user_id),
    added_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    PRIMARY KEY (team_id, user_id)
);

CREATE INDEX idx_team_members_user ON platform.team_members(user_id);
```

#### 2.2.6 Project

```sql
CREATE TABLE platform.projects (
    project_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    team_id             UUID REFERENCES platform.teams(team_id),
    name                VARCHAR(255) NOT NULL,
    slug                VARCHAR(63) NOT NULL,
    description         TEXT,
    region              VARCHAR(63) NOT NULL,
    cloud_provider      cloud_provider NOT NULL,
    vpc_id              VARCHAR(255),  -- For VPC peering
    status              project_status NOT NULL DEFAULT 'active',
    settings            JSONB NOT NULL DEFAULT '{}',
    resource_limits     JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_project_slug_per_org UNIQUE (org_id, slug)
);

CREATE TYPE cloud_provider AS ENUM ('aws', 'gcp', 'azure');
CREATE TYPE project_status AS ENUM ('active', 'suspended', 'deleted');

CREATE INDEX idx_projects_org ON platform.projects(org_id);
CREATE INDEX idx_projects_team ON platform.projects(team_id);
```

#### 2.2.7 Database

```sql
CREATE TABLE platform.databases (
    database_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id          UUID NOT NULL REFERENCES platform.projects(project_id),
    name                VARCHAR(63) NOT NULL,
    isolation_mode      isolation_mode NOT NULL,
    instance_type       VARCHAR(63) NOT NULL,
    storage_tier        storage_tier NOT NULL DEFAULT 'ssd',
    storage_size_gb     INTEGER NOT NULL,
    postgres_version    VARCHAR(10) NOT NULL DEFAULT '17',
    connection_string   TEXT,  -- Encrypted
    status              database_status NOT NULL DEFAULT 'provisioning',
    high_availability   BOOLEAN NOT NULL DEFAULT FALSE,
    read_replicas       INTEGER NOT NULL DEFAULT 0,
    backup_retention    INTEGER NOT NULL DEFAULT 7,  -- Days
    maintenance_window  JSONB,
    extensions          TEXT[] NOT NULL DEFAULT '{}',
    features            JSONB NOT NULL DEFAULT '{}',
    metrics             JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_db_name_per_project UNIQUE (project_id, name),
    CONSTRAINT valid_db_name CHECK (name ~ '^[a-z][a-z0-9_]*$')
);

CREATE TYPE isolation_mode AS ENUM (
    'shared_cluster',    -- Shared cluster, separate database
    'dedicated_instance', -- Dedicated VM/instance
    'dedicated_cluster',  -- Dedicated Kubernetes namespace
    'isolated_vm'        -- VM-level isolation
);

CREATE TYPE storage_tier AS ENUM ('ssd', 'nvme', 's3_tiered');
CREATE TYPE database_status AS ENUM (
    'provisioning', 'running', 'stopped',
    'maintenance', 'failed', 'deleted'
);

CREATE INDEX idx_databases_project ON platform.databases(project_id);
```

#### 2.2.8 API Keys

```sql
CREATE TABLE platform.api_keys (
    key_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID REFERENCES platform.users(user_id),
    org_id              UUID REFERENCES platform.organizations(org_id),
    name                VARCHAR(255) NOT NULL,
    key_prefix          VARCHAR(12) NOT NULL,  -- First 12 chars for identification
    key_hash            VARCHAR(255) NOT NULL, -- bcrypt hash of full key
    scope               api_key_scope NOT NULL,
    permissions         JSONB NOT NULL DEFAULT '[]',
    ip_allowlist        INET[],
    rate_limit          INTEGER,  -- Requests per minute
    expires_at          TIMESTAMPTZ,
    last_used_at        TIMESTAMPTZ,
    last_used_ip        INET,
    use_count           BIGINT NOT NULL DEFAULT 0,
    status              api_key_status NOT NULL DEFAULT 'active',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at          TIMESTAMPTZ,
    revoked_by          UUID REFERENCES platform.users(user_id),

    CONSTRAINT api_key_owner CHECK (
        (user_id IS NOT NULL AND org_id IS NULL) OR
        (user_id IS NULL AND org_id IS NOT NULL)
    )
);

CREATE TYPE api_key_scope AS ENUM (
    'user',         -- User-level key (inherits user permissions)
    'project',      -- Project-scoped key
    'database',     -- Database-scoped key
    'service'       -- Service account key
);

CREATE TYPE api_key_status AS ENUM ('active', 'expired', 'revoked');

CREATE INDEX idx_api_keys_user ON platform.api_keys(user_id);
CREATE INDEX idx_api_keys_org ON platform.api_keys(org_id);
CREATE INDEX idx_api_keys_prefix ON platform.api_keys(key_prefix);
```

---

## 3. Role-Based Access Control (RBAC)

### 3.1 Role Definitions

```sql
CREATE TABLE platform.roles (
    role_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID REFERENCES platform.organizations(org_id),  -- NULL for system roles
    name                VARCHAR(63) NOT NULL,
    display_name        VARCHAR(255) NOT NULL,
    description         TEXT,
    role_type           role_type NOT NULL,
    permissions         JSONB NOT NULL,
    is_system_role      BOOLEAN NOT NULL DEFAULT FALSE,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_role_name_per_org UNIQUE (org_id, name)
);

CREATE TYPE role_type AS ENUM (
    'organization',   -- Organization-level role
    'team',          -- Team-level role
    'project',       -- Project-level role
    'database'       -- Database-level role
);
```

### 3.2 Predefined System Roles

#### 3.2.1 Organization Roles

| Role | Description | Key Permissions |
|------|-------------|-----------------|
| **Owner** | Full organization control | All permissions, cannot be removed |
| **Admin** | Administrative access | Manage members, billing, settings |
| **Member** | Standard access | View org, access assigned resources |
| **Billing** | Billing only | View/manage billing, invoices |
| **Auditor** | Audit/compliance | Read-only access to audit logs |

#### 3.2.2 Team Roles

| Role | Description | Key Permissions |
|------|-------------|-----------------|
| **Team Lead** | Full team control | Manage team members, all projects |
| **Team Admin** | Team administration | Manage team settings, members |
| **Team Member** | Standard team access | Access team projects/databases |

#### 3.2.3 Project Roles

| Role | Description | Key Permissions |
|------|-------------|-----------------|
| **Project Owner** | Full project control | All project operations |
| **Project Admin** | Project administration | Manage databases, settings |
| **Developer** | Development access | Create/modify databases, deploy |
| **Viewer** | Read-only access | View resources, metrics |

#### 3.2.4 Database Roles

| Role | Description | Key Permissions |
|------|-------------|-----------------|
| **Database Admin** | Full database control | DDL, DML, user management |
| **Read/Write** | Data manipulation | SELECT, INSERT, UPDATE, DELETE |
| **Read Only** | Query access | SELECT only |
| **Analyst** | Analytics access | SELECT on analytics schemas |

### 3.3 Permission Matrix

#### 3.3.1 Organization Permissions

| Permission | Owner | Admin | Member | Billing | Auditor |
|------------|:-----:|:-----:|:------:|:-------:|:-------:|
| org.view | X | X | X | X | X |
| org.update | X | X | | | |
| org.delete | X | | | | |
| org.members.view | X | X | X | | X |
| org.members.invite | X | X | | | |
| org.members.remove | X | X | | | |
| org.members.update_role | X | X | | | |
| org.billing.view | X | X | | X | |
| org.billing.update | X | X | | X | |
| org.settings.view | X | X | | | X |
| org.settings.update | X | X | | | |
| org.sso.configure | X | | | | |
| org.audit_log.view | X | X | | | X |
| org.teams.create | X | X | | | |
| org.teams.delete | X | X | | | |
| org.projects.create | X | X | X | | |
| org.api_keys.manage | X | X | | | |

#### 3.3.2 Team Permissions

| Permission | Team Lead | Team Admin | Team Member |
|------------|:---------:|:----------:|:-----------:|
| team.view | X | X | X |
| team.update | X | X | |
| team.delete | X | | |
| team.members.view | X | X | X |
| team.members.add | X | X | |
| team.members.remove | X | X | |
| team.members.update_role | X | X | |
| team.projects.view | X | X | X |
| team.projects.create | X | X | |
| team.projects.assign | X | X | |

#### 3.3.3 Project Permissions

| Permission | Project Owner | Project Admin | Developer | Viewer |
|------------|:-------------:|:-------------:|:---------:|:------:|
| project.view | X | X | X | X |
| project.update | X | X | | |
| project.delete | X | | | |
| project.settings.view | X | X | X | X |
| project.settings.update | X | X | | |
| project.databases.view | X | X | X | X |
| project.databases.create | X | X | X | |
| project.databases.delete | X | X | | |
| project.databases.modify | X | X | X | |
| project.metrics.view | X | X | X | X |
| project.logs.view | X | X | X | |
| project.api_keys.manage | X | X | | |

#### 3.3.4 Database Permissions

| Permission | DB Admin | Read/Write | Read Only | Analyst |
|------------|:--------:|:----------:|:---------:|:-------:|
| database.connect | X | X | X | X |
| database.view_metadata | X | X | X | X |
| database.ddl.create | X | | | |
| database.ddl.alter | X | | | |
| database.ddl.drop | X | | | |
| database.dml.select | X | X | X | X |
| database.dml.insert | X | X | | |
| database.dml.update | X | X | | |
| database.dml.delete | X | X | | |
| database.users.manage | X | | | |
| database.backup.view | X | | | |
| database.backup.restore | X | | | |
| database.extensions.manage | X | | | |
| database.replication.manage | X | | | |

### 3.4 Custom Roles

```sql
-- Example: Create a custom "Data Engineer" role
INSERT INTO platform.roles (org_id, name, display_name, description, role_type, permissions)
VALUES (
    'org-uuid-here',
    'data_engineer',
    'Data Engineer',
    'Access to create and manage data pipelines and schemas',
    'project',
    '{
        "project.view": true,
        "project.databases.view": true,
        "project.databases.modify": true,
        "project.metrics.view": true,
        "database.connect": true,
        "database.ddl.create": true,
        "database.ddl.alter": true,
        "database.dml.select": true,
        "database.dml.insert": true,
        "orochi.pipelines.create": true,
        "orochi.pipelines.manage": true,
        "orochi.hypertables.create": true
    }'
);
```

### 3.5 Permission Evaluation Function

```sql
CREATE OR REPLACE FUNCTION platform.check_permission(
    p_user_id UUID,
    p_resource_type VARCHAR(63),
    p_resource_id UUID,
    p_permission VARCHAR(127)
) RETURNS BOOLEAN AS $$
DECLARE
    v_has_permission BOOLEAN := FALSE;
    v_org_id UUID;
    v_team_id UUID;
    v_project_id UUID;
BEGIN
    -- Get resource hierarchy
    CASE p_resource_type
        WHEN 'organization' THEN
            v_org_id := p_resource_id;
        WHEN 'team' THEN
            SELECT org_id INTO v_org_id FROM platform.teams WHERE team_id = p_resource_id;
            v_team_id := p_resource_id;
        WHEN 'project' THEN
            SELECT org_id, team_id INTO v_org_id, v_team_id
            FROM platform.projects WHERE project_id = p_resource_id;
            v_project_id := p_resource_id;
        WHEN 'database' THEN
            SELECT p.org_id, p.team_id, d.project_id
            INTO v_org_id, v_team_id, v_project_id
            FROM platform.databases d
            JOIN platform.projects p ON d.project_id = p.project_id
            WHERE d.database_id = p_resource_id;
    END CASE;

    -- Check organization-level permissions
    SELECT EXISTS (
        SELECT 1 FROM platform.organization_members om
        JOIN platform.roles r ON om.role_id = r.role_id
        WHERE om.user_id = p_user_id
          AND om.org_id = v_org_id
          AND om.status = 'active'
          AND (r.permissions->>p_permission)::boolean = true
    ) INTO v_has_permission;

    IF v_has_permission THEN RETURN TRUE; END IF;

    -- Check team-level permissions (if applicable)
    IF v_team_id IS NOT NULL THEN
        SELECT EXISTS (
            SELECT 1 FROM platform.team_members tm
            JOIN platform.roles r ON tm.role_id = r.role_id
            WHERE tm.user_id = p_user_id
              AND tm.team_id = v_team_id
              AND (r.permissions->>p_permission)::boolean = true
        ) INTO v_has_permission;

        IF v_has_permission THEN RETURN TRUE; END IF;
    END IF;

    -- Check project-level permissions (if applicable)
    IF v_project_id IS NOT NULL THEN
        SELECT EXISTS (
            SELECT 1 FROM platform.project_members pm
            JOIN platform.roles r ON pm.role_id = r.role_id
            WHERE pm.user_id = p_user_id
              AND pm.project_id = v_project_id
              AND (r.permissions->>p_permission)::boolean = true
        ) INTO v_has_permission;
    END IF;

    RETURN v_has_permission;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
```

---

## 4. SSO/SAML Integration

### 4.1 SSO Configuration Schema

```sql
CREATE TABLE platform.sso_configurations (
    sso_config_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    provider_type       sso_provider_type NOT NULL,
    provider_name       VARCHAR(255) NOT NULL,
    is_enabled          BOOLEAN NOT NULL DEFAULT FALSE,
    is_primary          BOOLEAN NOT NULL DEFAULT FALSE,

    -- SAML Configuration
    saml_entity_id      VARCHAR(512),
    saml_sso_url        TEXT,
    saml_slo_url        TEXT,
    saml_certificate    TEXT,  -- Encrypted X.509 certificate
    saml_signing_cert   TEXT,
    saml_attribute_mapping JSONB DEFAULT '{
        "email": "email",
        "firstName": "firstName",
        "lastName": "lastName",
        "groups": "groups"
    }',

    -- OIDC Configuration
    oidc_client_id      VARCHAR(255),
    oidc_client_secret  TEXT,  -- Encrypted
    oidc_issuer_url     TEXT,
    oidc_authorization_url TEXT,
    oidc_token_url      TEXT,
    oidc_userinfo_url   TEXT,
    oidc_scopes         TEXT[] DEFAULT ARRAY['openid', 'profile', 'email'],

    -- Common Settings
    auto_provision_users BOOLEAN NOT NULL DEFAULT TRUE,
    default_role_id     UUID REFERENCES platform.roles(role_id),
    group_mapping       JSONB DEFAULT '{}',
    domain_restrictions TEXT[],  -- Allowed email domains

    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT one_primary_per_org UNIQUE (org_id, is_primary)
        WHERE is_primary = TRUE
);

CREATE TYPE sso_provider_type AS ENUM (
    'saml',
    'oidc',
    'google',
    'github',
    'microsoft',
    'okta'
);

-- Group to role mapping example:
-- {
--   "Engineering": "role-uuid-developer",
--   "Platform": "role-uuid-admin",
--   "Analytics": "role-uuid-analyst"
-- }
```

### 4.2 SSO Session Management

```sql
CREATE TABLE platform.sso_sessions (
    session_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID NOT NULL REFERENCES platform.users(user_id),
    sso_config_id       UUID NOT NULL REFERENCES platform.sso_configurations(sso_config_id),
    session_index       VARCHAR(255),  -- SAML SessionIndex
    name_id             VARCHAR(255),  -- SAML NameID
    provider_session_id VARCHAR(255),  -- Provider's session ID
    attributes          JSONB,         -- Raw SAML/OIDC attributes
    ip_address          INET,
    user_agent          TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ NOT NULL,
    revoked_at          TIMESTAMPTZ,

    CONSTRAINT session_not_expired CHECK (expires_at > created_at)
);

CREATE INDEX idx_sso_sessions_user ON platform.sso_sessions(user_id);
CREATE INDEX idx_sso_sessions_expires ON platform.sso_sessions(expires_at);
```

---

## 5. Multi-Factor Authentication (MFA)

### 5.1 MFA Configuration

```sql
CREATE TABLE platform.mfa_configurations (
    mfa_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID NOT NULL REFERENCES platform.users(user_id),
    mfa_type            mfa_type NOT NULL,
    is_primary          BOOLEAN NOT NULL DEFAULT FALSE,
    is_verified         BOOLEAN NOT NULL DEFAULT FALSE,

    -- TOTP Configuration
    totp_secret         TEXT,  -- Encrypted base32 secret
    totp_algorithm      VARCHAR(10) DEFAULT 'SHA1',
    totp_digits         INTEGER DEFAULT 6,
    totp_period         INTEGER DEFAULT 30,

    -- WebAuthn/FIDO2 Configuration
    webauthn_credential_id TEXT,
    webauthn_public_key TEXT,
    webauthn_sign_count BIGINT DEFAULT 0,
    webauthn_transports TEXT[],
    webauthn_device_name VARCHAR(255),

    -- SMS/Phone Configuration
    phone_number        VARCHAR(20),
    phone_verified      BOOLEAN DEFAULT FALSE,

    -- Backup Codes
    backup_codes        TEXT[],  -- Encrypted, hashed codes
    backup_codes_used   INTEGER DEFAULT 0,

    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    verified_at         TIMESTAMPTZ,
    last_used_at        TIMESTAMPTZ,

    CONSTRAINT one_primary_mfa_per_user UNIQUE (user_id, is_primary)
        WHERE is_primary = TRUE
);

CREATE TYPE mfa_type AS ENUM (
    'totp',      -- Time-based OTP (Google Authenticator, Authy)
    'webauthn',  -- WebAuthn/FIDO2 (YubiKey, Touch ID)
    'sms',       -- SMS codes (not recommended for high security)
    'email'      -- Email codes
);

CREATE INDEX idx_mfa_user ON platform.mfa_configurations(user_id);
```

### 5.2 MFA Challenge/Response

```sql
CREATE TABLE platform.mfa_challenges (
    challenge_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID NOT NULL REFERENCES platform.users(user_id),
    mfa_id              UUID NOT NULL REFERENCES platform.mfa_configurations(mfa_id),
    challenge_type      mfa_type NOT NULL,
    challenge_data      TEXT,  -- Encrypted challenge (e.g., sent SMS code)
    webauthn_challenge  TEXT,  -- WebAuthn challenge for FIDO2
    attempts            INTEGER NOT NULL DEFAULT 0,
    max_attempts        INTEGER NOT NULL DEFAULT 5,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ NOT NULL,
    verified_at         TIMESTAMPTZ,
    ip_address          INET,

    CONSTRAINT challenge_not_expired CHECK (expires_at > created_at),
    CONSTRAINT max_attempts_not_exceeded CHECK (attempts <= max_attempts)
);

CREATE INDEX idx_mfa_challenges_user ON platform.mfa_challenges(user_id);
CREATE INDEX idx_mfa_challenges_expires ON platform.mfa_challenges(expires_at);
```

---

## 6. Tenant Isolation Architecture

### 6.1 Isolation Models Comparison

| Aspect | Shared Cluster | Dedicated Instance | K8s Namespace | VM Isolation |
|--------|----------------|-------------------|---------------|--------------|
| **Cost** | Lowest | Medium | Medium-High | Highest |
| **Isolation Level** | Database | Instance | Pod/Network | Full |
| **Performance Isolation** | Low | High | Medium | Highest |
| **Noisy Neighbor Risk** | High | Low | Medium | None |
| **Provisioning Time** | Seconds | Minutes | Minutes | Minutes |
| **Compliance** | Basic | SOC2 | SOC2/HIPAA | HIPAA/PCI |
| **Recommended Tier** | Free/Starter | Professional | Professional | Enterprise |

### 6.2 Recommended Architecture by Plan Tier

```
+------------------+     +----------------------+     +------------------+
|   Free/Starter   |     |    Professional      |     |    Enterprise    |
+------------------+     +----------------------+     +------------------+
         |                         |                          |
         v                         v                          v
+------------------+     +----------------------+     +------------------+
| Shared Cluster   |     | Dedicated Instance   |     | Isolated VM      |
| - Separate DBs   |     | - Own PG instance    |     | - Own VM/Node    |
| - Schema prefix  |     | - Resource limits    |     | - VPC isolation  |
| - Row-level sec  |     | - Network policies   |     | - Dedicated CPU  |
| - Connection pool|     | - Namespace isolation|     | - Encryption keys|
+------------------+     +----------------------+     +------------------+
```

### 6.3 Shared Cluster Isolation (Free/Starter)

```
+----------------------------------------------------------+
|                    PostgreSQL Cluster                      |
|  +------------------+  +------------------+                |
|  | Database: tenant_a | | Database: tenant_b |              |
|  |  +-------------+   | |  +-------------+   |              |
|  |  | public      |   | |  | public      |   |              |
|  |  | orochi      |   | |  | orochi      |   |              |
|  |  | analytics   |   | |  | analytics   |   |              |
|  |  +-------------+   | |  +-------------+   |              |
|  +------------------+ | +------------------+ |              |
|                       |                       |              |
|  Connection Pool (PgBouncer)                               |
|  +-----------------------------------------------------+   |
|  | tenant_a_pool (max 100) | tenant_b_pool (max 100)   |   |
|  +-----------------------------------------------------+   |
+----------------------------------------------------------+
```

**Security Controls:**

```sql
-- Each tenant gets a dedicated database
CREATE DATABASE tenant_<tenant_id>
    OWNER tenant_<tenant_id>_owner
    CONNECTION LIMIT 100;

-- Database-level isolation
REVOKE ALL ON DATABASE tenant_<tenant_id> FROM PUBLIC;
GRANT CONNECT ON DATABASE tenant_<tenant_id> TO tenant_<tenant_id>_role;

-- Schema isolation
CREATE SCHEMA IF NOT EXISTS tenant_data;
GRANT USAGE ON SCHEMA tenant_data TO tenant_<tenant_id>_role;

-- Row-level security for shared metadata tables
ALTER TABLE platform.audit_logs ENABLE ROW LEVEL SECURITY;

CREATE POLICY tenant_isolation ON platform.audit_logs
    USING (org_id = current_setting('app.current_org_id')::uuid);
```

### 6.4 Dedicated Instance Isolation (Professional)

```
+----------------------------------------------------------+
|                   Kubernetes Namespace                     |
|                   ns: tenant-<tenant_id>                   |
|                                                            |
|  +------------------+     +------------------+              |
|  | Pod: postgres    |     | Pod: pgbouncer   |              |
|  | CPU: 2 cores     |     | Connections: 500 |              |
|  | Memory: 8GB      |     +------------------+              |
|  | Storage: 100GB   |                                      |
|  +------------------+     +------------------+              |
|                           | NetworkPolicy    |              |
|  +------------------+     | - Allow: tenant  |              |
|  | Pod: orochi-     |     | - Deny: others   |              |
|  | workers          |     +------------------+              |
|  +------------------+                                      |
+----------------------------------------------------------+
```

**Kubernetes Resource Quotas:**

```yaml
apiVersion: v1
kind: ResourceQuota
metadata:
  name: tenant-quota
  namespace: tenant-<tenant_id>
spec:
  hard:
    requests.cpu: "4"
    requests.memory: 16Gi
    limits.cpu: "8"
    limits.memory: 32Gi
    persistentvolumeclaims: "5"
    requests.storage: 500Gi
    pods: "10"
```

**Network Policy:**

```yaml
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: tenant-isolation
  namespace: tenant-<tenant_id>
spec:
  podSelector: {}
  policyTypes:
    - Ingress
    - Egress
  ingress:
    - from:
        - namespaceSelector:
            matchLabels:
              name: platform-ingress
        - namespaceSelector:
            matchLabels:
              tenant-id: <tenant_id>
  egress:
    - to:
        - namespaceSelector:
            matchLabels:
              name: platform-services
    - to:
        - ipBlock:
            cidr: 0.0.0.0/0
      ports:
        - protocol: TCP
          port: 443
```

### 6.5 VM-Level Isolation (Enterprise)

```
+----------------------------------------------------------+
|                  Customer VPC (peered)                     |
|                                                            |
|  +-------------------------+  +-------------------------+  |
|  | VM: pg-primary          |  | VM: pg-replica          |  |
|  | - Dedicated CPU         |  | - Dedicated CPU         |  |
|  | - Dedicated memory      |  | - Dedicated memory      |  |
|  | - Encrypted storage     |  | - Encrypted storage     |  |
|  | - Customer-managed keys |  | - Read replica          |  |
|  +-------------------------+  +-------------------------+  |
|                                                            |
|  +-------------------------+  +-------------------------+  |
|  | VM: orochi-coordinator  |  | VM: orochi-workers      |  |
|  | - Query routing         |  | - Distributed execution |  |
|  | - Connection pooling    |  | - Background jobs       |  |
|  +-------------------------+  +-------------------------+  |
|                                                            |
|  Private Link / VPC Peering to Customer Network            |
+----------------------------------------------------------+
```

**Customer-Managed Encryption:**

```sql
-- Enterprise customers can provide their own encryption keys
CREATE TABLE platform.customer_encryption_keys (
    key_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    key_type            encryption_key_type NOT NULL,
    kms_provider        kms_provider NOT NULL,
    kms_key_id          VARCHAR(512) NOT NULL,  -- ARN, resource ID, etc.
    key_version         INTEGER NOT NULL DEFAULT 1,
    is_active           BOOLEAN NOT NULL DEFAULT TRUE,
    rotation_schedule   INTERVAL,
    last_rotated_at     TIMESTAMPTZ,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT one_active_key_per_type UNIQUE (org_id, key_type)
        WHERE is_active = TRUE
);

CREATE TYPE encryption_key_type AS ENUM (
    'data_at_rest',      -- Database encryption
    'backup',            -- Backup encryption
    'connection',        -- TLS certificates
    'secrets'            -- API keys, passwords
);

CREATE TYPE kms_provider AS ENUM (
    'aws_kms',
    'gcp_kms',
    'azure_keyvault',
    'hashicorp_vault'
);
```

---

## 7. Resource Quotas and Limits

### 7.1 Quota Configuration

```sql
CREATE TABLE platform.resource_quotas (
    quota_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id),
    project_id          UUID REFERENCES platform.projects(project_id),
    database_id         UUID REFERENCES platform.databases(database_id),

    -- Compute Limits
    max_cpu_cores       DECIMAL(5,2),
    max_memory_gb       INTEGER,
    max_temp_storage_gb INTEGER,

    -- Storage Limits
    max_storage_gb      INTEGER,
    max_backup_storage_gb INTEGER,

    -- Connection Limits
    max_connections     INTEGER,
    max_connection_rate INTEGER,  -- per minute

    -- Query Limits
    max_query_time_seconds INTEGER,
    max_rows_returned   BIGINT,
    max_result_size_mb  INTEGER,

    -- Rate Limits
    max_queries_per_minute INTEGER,
    max_writes_per_minute INTEGER,

    -- Orochi-Specific Limits
    max_shards          INTEGER,
    max_chunks          INTEGER,
    max_pipelines       INTEGER,
    max_vector_dimensions INTEGER,

    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    -- Hierarchy: database > project > org
    CONSTRAINT quota_hierarchy CHECK (
        (database_id IS NOT NULL) OR
        (database_id IS NULL AND project_id IS NOT NULL) OR
        (database_id IS NULL AND project_id IS NULL)
    )
);

CREATE INDEX idx_quotas_org ON platform.resource_quotas(org_id);
CREATE INDEX idx_quotas_project ON platform.resource_quotas(project_id);
CREATE INDEX idx_quotas_database ON platform.resource_quotas(database_id);
```

### 7.2 Default Quotas by Plan

```sql
-- Plan-based default quotas
INSERT INTO platform.plan_quotas (plan_tier, quotas) VALUES
('free', '{
    "max_databases": 1,
    "max_storage_gb": 1,
    "max_connections": 10,
    "max_cpu_cores": 0.25,
    "max_memory_gb": 0.5,
    "max_query_time_seconds": 30,
    "max_rows_returned": 10000,
    "max_queries_per_minute": 60,
    "features": ["basic"]
}'),
('starter', '{
    "max_databases": 5,
    "max_storage_gb": 20,
    "max_connections": 50,
    "max_cpu_cores": 2,
    "max_memory_gb": 4,
    "max_query_time_seconds": 60,
    "max_rows_returned": 100000,
    "max_queries_per_minute": 300,
    "features": ["basic", "columnar"]
}'),
('professional', '{
    "max_databases": 50,
    "max_storage_gb": 500,
    "max_connections": 500,
    "max_cpu_cores": 16,
    "max_memory_gb": 64,
    "max_query_time_seconds": 300,
    "max_rows_returned": 1000000,
    "max_queries_per_minute": 1000,
    "features": ["basic", "columnar", "timeseries", "vector", "pipelines"]
}'),
('enterprise', '{
    "max_databases": -1,
    "max_storage_gb": -1,
    "max_connections": -1,
    "max_cpu_cores": -1,
    "max_memory_gb": -1,
    "max_query_time_seconds": -1,
    "max_rows_returned": -1,
    "max_queries_per_minute": -1,
    "features": ["all"]
}');
```

### 7.3 Noisy Neighbor Prevention

```sql
-- Resource usage tracking for rate limiting
CREATE TABLE platform.resource_usage (
    usage_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL,
    database_id         UUID NOT NULL,
    window_start        TIMESTAMPTZ NOT NULL,
    window_duration     INTERVAL NOT NULL DEFAULT '1 minute',

    -- Metrics
    query_count         INTEGER NOT NULL DEFAULT 0,
    write_count         INTEGER NOT NULL DEFAULT 0,
    cpu_time_ms         BIGINT NOT NULL DEFAULT 0,
    memory_peak_mb      INTEGER NOT NULL DEFAULT 0,
    io_read_mb          BIGINT NOT NULL DEFAULT 0,
    io_write_mb         BIGINT NOT NULL DEFAULT 0,
    connections_used    INTEGER NOT NULL DEFAULT 0,

    PRIMARY KEY (org_id, database_id, window_start)
);

-- Partition by time for efficient cleanup
CREATE INDEX idx_usage_window ON platform.resource_usage(window_start);

-- Automatic cleanup of old metrics
SELECT add_retention_policy('platform.resource_usage', INTERVAL '7 days');
```

**Connection Pooling with Tenant Isolation (PgBouncer):**

```ini
; pgbouncer.ini
[databases]
; Each tenant gets their own pool
tenant_abc = host=pg-shared port=5432 dbname=tenant_abc pool_size=100 reserve_pool=10
tenant_xyz = host=pg-shared port=5432 dbname=tenant_xyz pool_size=100 reserve_pool=10

[pgbouncer]
pool_mode = transaction
max_client_conn = 10000
default_pool_size = 50
reserve_pool_size = 10
reserve_pool_timeout = 3
max_db_connections = 100    ; Per-database limit
max_user_connections = 50   ; Per-user limit
stats_period = 60

; Authentication
auth_type = hba
auth_hba_file = /etc/pgbouncer/pg_hba.conf
auth_query = SELECT usename, passwd FROM platform.pgbouncer_users WHERE usename=$1

; Rate limiting
client_idle_timeout = 300
query_timeout = 300
query_wait_timeout = 60
```

---

## 8. Audit Logging

### 8.1 Audit Log Schema

```sql
CREATE TABLE platform.audit_logs (
    log_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    event_time          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    org_id              UUID NOT NULL,
    user_id             UUID,
    api_key_id          UUID,

    -- Event Classification
    event_category      audit_category NOT NULL,
    event_action        VARCHAR(127) NOT NULL,
    event_outcome       audit_outcome NOT NULL,

    -- Resource Information
    resource_type       VARCHAR(63),
    resource_id         UUID,
    resource_name       VARCHAR(255),

    -- Request Context
    ip_address          INET,
    user_agent          TEXT,
    request_id          UUID,
    session_id          UUID,

    -- Event Details
    event_data          JSONB NOT NULL DEFAULT '{}',
    previous_state      JSONB,
    new_state           JSONB,

    -- Security Context
    authentication_method VARCHAR(63),
    mfa_used            BOOLEAN DEFAULT FALSE,
    risk_score          DECIMAL(3,2)
);

CREATE TYPE audit_category AS ENUM (
    'authentication',
    'authorization',
    'data_access',
    'data_modification',
    'configuration',
    'security',
    'billing',
    'system'
);

CREATE TYPE audit_outcome AS ENUM (
    'success',
    'failure',
    'error',
    'denied'
);

-- Hypertable for efficient time-series queries
SELECT create_hypertable('platform.audit_logs', 'event_time',
    chunk_time_interval => INTERVAL '1 day');

-- Indexes for common query patterns
CREATE INDEX idx_audit_org_time ON platform.audit_logs(org_id, event_time DESC);
CREATE INDEX idx_audit_user ON platform.audit_logs(user_id, event_time DESC);
CREATE INDEX idx_audit_resource ON platform.audit_logs(resource_type, resource_id, event_time DESC);
CREATE INDEX idx_audit_action ON platform.audit_logs(event_action, event_time DESC);

-- Retention policy based on plan
-- Free: 7 days, Starter: 30 days, Professional: 90 days, Enterprise: configurable
```

### 8.2 Audit Event Examples

```json
// Authentication Event
{
  "event_category": "authentication",
  "event_action": "user.login",
  "event_outcome": "success",
  "event_data": {
    "auth_method": "password",
    "mfa_method": "totp",
    "session_duration": 28800,
    "client_type": "web"
  }
}

// Data Access Event
{
  "event_category": "data_access",
  "event_action": "database.query",
  "event_outcome": "success",
  "resource_type": "database",
  "resource_id": "db-uuid",
  "event_data": {
    "query_type": "SELECT",
    "tables_accessed": ["users", "orders"],
    "rows_returned": 1523,
    "execution_time_ms": 45,
    "query_hash": "abc123..."
  }
}

// Configuration Change
{
  "event_category": "configuration",
  "event_action": "database.settings.update",
  "event_outcome": "success",
  "resource_type": "database",
  "previous_state": {
    "max_connections": 100,
    "statement_timeout": "30s"
  },
  "new_state": {
    "max_connections": 200,
    "statement_timeout": "60s"
  }
}
```

---

## 9. Security Considerations

### 9.1 Authentication Security

| Control | Implementation |
|---------|----------------|
| Password Hashing | bcrypt with cost factor 12 |
| Password Policy | Minimum 12 chars, complexity requirements |
| Credential Storage | Secrets encrypted with AES-256-GCM |
| Session Tokens | JWT with RS256, 1-hour expiry |
| Refresh Tokens | Opaque tokens, 30-day expiry, rotation |
| Rate Limiting | 5 failed attempts, 15-minute lockout |

### 9.2 Network Security

| Control | Implementation |
|---------|----------------|
| TLS | TLS 1.3 required, TLS 1.2 minimum |
| Certificate Validation | Strict hostname verification |
| IP Allowlisting | Optional per-org, per-API key |
| VPC Peering | Available for Professional+ |
| Private Link | AWS PrivateLink, GCP Private Service Connect |
| WAF | CloudFlare/AWS WAF for API endpoints |

### 9.3 Data Protection

| Control | Implementation |
|---------|----------------|
| Encryption at Rest | AES-256, customer-managed keys for Enterprise |
| Encryption in Transit | TLS 1.3 for all connections |
| Backup Encryption | Separate encryption keys per tenant |
| Key Rotation | Automatic 90-day rotation |
| Data Masking | PII masking in logs and errors |
| Secure Deletion | Cryptographic erasure on delete |

### 9.4 Compliance Controls

| Standard | Controls Implemented |
|----------|---------------------|
| SOC 2 Type II | Audit logging, access controls, encryption |
| HIPAA | BAA available, encryption, audit trails |
| GDPR | Data export, deletion, consent management |
| PCI DSS | Network isolation, encryption, logging |
| ISO 27001 | Information security management |

### 9.5 Threat Model Summary

```
+------------------------------------------------------------------+
|                        THREAT MODEL                               |
+------------------------------------------------------------------+
| Threat                  | Mitigation                              |
+-------------------------+-----------------------------------------+
| Credential stuffing     | Rate limiting, MFA, breach detection    |
| SQL injection           | Parameterized queries, input validation |
| Privilege escalation    | RBAC, least privilege, audit logging    |
| Tenant data leakage     | Database isolation, RLS, encryption     |
| Insider threat          | Audit logging, access reviews, MFA      |
| DDoS                    | Rate limiting, WAF, auto-scaling        |
| Man-in-the-middle       | TLS 1.3, certificate pinning            |
| Data breach             | Encryption, key management, monitoring  |
| Supply chain attack     | Dependency scanning, SBOM               |
| Noisy neighbor          | Resource quotas, fair scheduling        |
+------------------------------------------------------------------+
```

---

## 10. Implementation Recommendations

### 10.1 Phase 1: Core Foundation (Weeks 1-4)

1. Implement account hierarchy (organizations, teams, users)
2. Deploy authentication system with MFA support
3. Create RBAC framework with predefined roles
4. Set up shared cluster isolation for Free/Starter

### 10.2 Phase 2: Advanced Features (Weeks 5-8)

1. Implement SSO/SAML integration
2. Deploy API key management system
3. Create resource quota enforcement
4. Set up comprehensive audit logging

### 10.3 Phase 3: Enterprise Features (Weeks 9-12)

1. Implement dedicated instance isolation
2. Deploy VM-level isolation for Enterprise
3. Add customer-managed encryption keys
4. Complete compliance documentation

### 10.4 Success Metrics

| Metric | Target |
|--------|--------|
| Authentication latency | < 200ms p99 |
| Authorization check | < 10ms p99 |
| Tenant isolation violations | 0 |
| Audit log completeness | 100% |
| MFA adoption | > 80% for Enterprise |
| SSO adoption | > 90% for Enterprise |

---

## 11. References

- [PostgreSQL Row Level Security](https://www.postgresql.org/docs/current/ddl-rowsecurity.html)
- [NIST SP 800-63B Authentication Guidelines](https://pages.nist.gov/800-63-3/sp800-63b.html)
- [OWASP Authentication Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html)
- [AWS Multi-Tenancy Whitepaper](https://aws.amazon.com/whitepapers/saas-tenant-isolation-strategies/)
- [Kubernetes Network Policies](https://kubernetes.io/docs/concepts/services-networking/network-policies/)
