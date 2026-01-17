-- ============================================================
-- Orochi DB Managed Service - RBAC Implementation
-- Multi-Tenancy Access Control Schema
-- Version: 1.0.0
-- ============================================================

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

-- Create platform schema for multi-tenancy management
CREATE SCHEMA IF NOT EXISTS platform;

-- ============================================================
-- ENUM TYPES
-- ============================================================

CREATE TYPE platform.plan_tier AS ENUM (
    'free',
    'starter',
    'professional',
    'enterprise',
    'custom'
);

CREATE TYPE platform.org_status AS ENUM (
    'active',
    'suspended',
    'pending_deletion',
    'deleted'
);

CREATE TYPE platform.user_status AS ENUM (
    'pending',
    'active',
    'suspended',
    'deleted'
);

CREATE TYPE platform.auth_provider AS ENUM (
    'local',
    'google',
    'github',
    'microsoft',
    'okta',
    'saml',
    'oidc'
);

CREATE TYPE platform.membership_status AS ENUM (
    'pending',
    'active',
    'suspended',
    'removed'
);

CREATE TYPE platform.role_type AS ENUM (
    'organization',
    'team',
    'project',
    'database'
);

CREATE TYPE platform.isolation_mode AS ENUM (
    'shared_cluster',
    'dedicated_instance',
    'dedicated_cluster',
    'isolated_vm'
);

CREATE TYPE platform.database_status AS ENUM (
    'provisioning',
    'running',
    'stopped',
    'maintenance',
    'failed',
    'deleted'
);

CREATE TYPE platform.api_key_scope AS ENUM (
    'user',
    'project',
    'database',
    'service'
);

CREATE TYPE platform.api_key_status AS ENUM (
    'active',
    'expired',
    'revoked'
);

CREATE TYPE platform.mfa_type AS ENUM (
    'totp',
    'webauthn',
    'sms',
    'email'
);

CREATE TYPE platform.audit_category AS ENUM (
    'authentication',
    'authorization',
    'data_access',
    'data_modification',
    'configuration',
    'security',
    'billing',
    'system'
);

CREATE TYPE platform.audit_outcome AS ENUM (
    'success',
    'failure',
    'error',
    'denied'
);

-- ============================================================
-- CORE TABLES
-- ============================================================

-- Organizations (Billing Entity)
CREATE TABLE platform.organizations (
    org_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name                VARCHAR(255) NOT NULL,
    slug                VARCHAR(63) UNIQUE NOT NULL,
    billing_email       VARCHAR(255) NOT NULL,
    billing_address     JSONB,
    plan_tier           platform.plan_tier NOT NULL DEFAULT 'free',
    plan_limits         JSONB NOT NULL DEFAULT '{}',
    status              platform.org_status NOT NULL DEFAULT 'active',
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

CREATE INDEX idx_organizations_slug ON platform.organizations(slug);
CREATE INDEX idx_organizations_status ON platform.organizations(status) WHERE status != 'deleted';

-- Users
CREATE TABLE platform.users (
    user_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email               VARCHAR(255) UNIQUE NOT NULL,
    email_verified      BOOLEAN NOT NULL DEFAULT FALSE,
    password_hash       VARCHAR(255),
    auth_provider       platform.auth_provider NOT NULL DEFAULT 'local',
    auth_provider_id    VARCHAR(255),
    display_name        VARCHAR(255),
    avatar_url          TEXT,
    mfa_enabled         BOOLEAN NOT NULL DEFAULT FALSE,
    mfa_secret          TEXT,
    mfa_backup_codes    TEXT[],
    status              platform.user_status NOT NULL DEFAULT 'active',
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

CREATE INDEX idx_users_email ON platform.users(email);
CREATE INDEX idx_users_auth_provider ON platform.users(auth_provider, auth_provider_id)
    WHERE auth_provider != 'local';

-- Roles
CREATE TABLE platform.roles (
    role_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
    name                VARCHAR(63) NOT NULL,
    display_name        VARCHAR(255) NOT NULL,
    description         TEXT,
    role_type           platform.role_type NOT NULL,
    permissions         JSONB NOT NULL,
    is_system_role      BOOLEAN NOT NULL DEFAULT FALSE,
    parent_role_id      UUID REFERENCES platform.roles(role_id),
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_role_name_per_org UNIQUE (org_id, name)
);

CREATE INDEX idx_roles_org ON platform.roles(org_id);
CREATE INDEX idx_roles_type ON platform.roles(role_type);

-- Teams
CREATE TABLE platform.teams (
    team_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
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

-- Organization Membership
CREATE TABLE platform.organization_members (
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
    user_id             UUID NOT NULL REFERENCES platform.users(user_id) ON DELETE CASCADE,
    role_id             UUID NOT NULL REFERENCES platform.roles(role_id),
    invited_by          UUID REFERENCES platform.users(user_id),
    invited_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    accepted_at         TIMESTAMPTZ,
    status              platform.membership_status NOT NULL DEFAULT 'pending',

    PRIMARY KEY (org_id, user_id)
);

CREATE INDEX idx_org_members_user ON platform.organization_members(user_id);
CREATE INDEX idx_org_members_status ON platform.organization_members(status);

-- Team Membership
CREATE TABLE platform.team_members (
    team_id             UUID NOT NULL REFERENCES platform.teams(team_id) ON DELETE CASCADE,
    user_id             UUID NOT NULL REFERENCES platform.users(user_id) ON DELETE CASCADE,
    role_id             UUID NOT NULL REFERENCES platform.roles(role_id),
    added_by            UUID REFERENCES platform.users(user_id),
    added_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    PRIMARY KEY (team_id, user_id)
);

CREATE INDEX idx_team_members_user ON platform.team_members(user_id);

-- Projects
CREATE TABLE platform.projects (
    project_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
    team_id             UUID REFERENCES platform.teams(team_id) ON DELETE SET NULL,
    name                VARCHAR(255) NOT NULL,
    slug                VARCHAR(63) NOT NULL,
    description         TEXT,
    region              VARCHAR(63) NOT NULL,
    status              VARCHAR(20) NOT NULL DEFAULT 'active',
    settings            JSONB NOT NULL DEFAULT '{}',
    resource_limits     JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_project_slug_per_org UNIQUE (org_id, slug)
);

CREATE INDEX idx_projects_org ON platform.projects(org_id);
CREATE INDEX idx_projects_team ON platform.projects(team_id);

-- Project Membership
CREATE TABLE platform.project_members (
    project_id          UUID NOT NULL REFERENCES platform.projects(project_id) ON DELETE CASCADE,
    user_id             UUID NOT NULL REFERENCES platform.users(user_id) ON DELETE CASCADE,
    role_id             UUID NOT NULL REFERENCES platform.roles(role_id),
    added_by            UUID REFERENCES platform.users(user_id),
    added_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    PRIMARY KEY (project_id, user_id)
);

CREATE INDEX idx_project_members_user ON platform.project_members(user_id);

-- Databases
CREATE TABLE platform.databases (
    database_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id          UUID NOT NULL REFERENCES platform.projects(project_id) ON DELETE CASCADE,
    name                VARCHAR(63) NOT NULL,
    isolation_mode      platform.isolation_mode NOT NULL,
    instance_type       VARCHAR(63) NOT NULL,
    storage_size_gb     INTEGER NOT NULL,
    postgres_version    VARCHAR(10) NOT NULL DEFAULT '17',
    connection_string   TEXT,
    status              platform.database_status NOT NULL DEFAULT 'provisioning',
    high_availability   BOOLEAN NOT NULL DEFAULT FALSE,
    read_replicas       INTEGER NOT NULL DEFAULT 0,
    backup_retention    INTEGER NOT NULL DEFAULT 7,
    maintenance_window  JSONB,
    extensions          TEXT[] NOT NULL DEFAULT '{}',
    features            JSONB NOT NULL DEFAULT '{}',
    metrics             JSONB NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT unique_db_name_per_project UNIQUE (project_id, name),
    CONSTRAINT valid_db_name CHECK (name ~ '^[a-z][a-z0-9_]*$')
);

CREATE INDEX idx_databases_project ON platform.databases(project_id);
CREATE INDEX idx_databases_status ON platform.databases(status);

-- API Keys
CREATE TABLE platform.api_keys (
    key_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID REFERENCES platform.users(user_id) ON DELETE CASCADE,
    org_id              UUID REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
    name                VARCHAR(255) NOT NULL,
    key_prefix          VARCHAR(12) NOT NULL,
    key_hash            VARCHAR(255) NOT NULL,
    scope               platform.api_key_scope NOT NULL,
    permissions         JSONB NOT NULL DEFAULT '[]',
    resource_ids        UUID[],
    ip_allowlist        INET[],
    rate_limit          INTEGER,
    expires_at          TIMESTAMPTZ,
    last_used_at        TIMESTAMPTZ,
    last_used_ip        INET,
    use_count           BIGINT NOT NULL DEFAULT 0,
    status              platform.api_key_status NOT NULL DEFAULT 'active',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at          TIMESTAMPTZ,
    revoked_by          UUID REFERENCES platform.users(user_id),

    CONSTRAINT api_key_owner CHECK (
        (user_id IS NOT NULL AND org_id IS NULL) OR
        (user_id IS NULL AND org_id IS NOT NULL)
    )
);

CREATE INDEX idx_api_keys_user ON platform.api_keys(user_id);
CREATE INDEX idx_api_keys_org ON platform.api_keys(org_id);
CREATE INDEX idx_api_keys_prefix ON platform.api_keys(key_prefix);
CREATE INDEX idx_api_keys_status ON platform.api_keys(status) WHERE status = 'active';

-- ============================================================
-- PREDEFINED SYSTEM ROLES
-- ============================================================

-- Organization Roles
INSERT INTO platform.roles (role_id, org_id, name, display_name, description, role_type, permissions, is_system_role)
VALUES
-- Owner: Full control
(gen_random_uuid(), NULL, 'org_owner', 'Organization Owner', 'Full organization control, cannot be removed', 'organization', '{
    "org.view": true,
    "org.update": true,
    "org.delete": true,
    "org.members.view": true,
    "org.members.invite": true,
    "org.members.remove": true,
    "org.members.update_role": true,
    "org.billing.view": true,
    "org.billing.update": true,
    "org.settings.view": true,
    "org.settings.update": true,
    "org.sso.configure": true,
    "org.audit_log.view": true,
    "org.teams.create": true,
    "org.teams.delete": true,
    "org.projects.create": true,
    "org.api_keys.manage": true,
    "team.*": true,
    "project.*": true,
    "database.*": true
}', true),

-- Admin: Administrative access
(gen_random_uuid(), NULL, 'org_admin', 'Organization Admin', 'Administrative access to organization', 'organization', '{
    "org.view": true,
    "org.update": true,
    "org.members.view": true,
    "org.members.invite": true,
    "org.members.remove": true,
    "org.members.update_role": true,
    "org.billing.view": true,
    "org.billing.update": true,
    "org.settings.view": true,
    "org.settings.update": true,
    "org.audit_log.view": true,
    "org.teams.create": true,
    "org.teams.delete": true,
    "org.projects.create": true,
    "org.api_keys.manage": true,
    "team.*": true,
    "project.*": true,
    "database.*": true
}', true),

-- Member: Standard access
(gen_random_uuid(), NULL, 'org_member', 'Organization Member', 'Standard organization member', 'organization', '{
    "org.view": true,
    "org.members.view": true,
    "org.projects.create": true
}', true),

-- Billing: Billing only
(gen_random_uuid(), NULL, 'org_billing', 'Billing Manager', 'Billing and invoice management', 'organization', '{
    "org.view": true,
    "org.billing.view": true,
    "org.billing.update": true
}', true),

-- Auditor: Read-only audit access
(gen_random_uuid(), NULL, 'org_auditor', 'Auditor', 'Read-only access to audit logs and settings', 'organization', '{
    "org.view": true,
    "org.members.view": true,
    "org.settings.view": true,
    "org.audit_log.view": true
}', true);

-- Team Roles
INSERT INTO platform.roles (role_id, org_id, name, display_name, description, role_type, permissions, is_system_role)
VALUES
(gen_random_uuid(), NULL, 'team_lead', 'Team Lead', 'Full team control', 'team', '{
    "team.view": true,
    "team.update": true,
    "team.delete": true,
    "team.members.view": true,
    "team.members.add": true,
    "team.members.remove": true,
    "team.members.update_role": true,
    "team.projects.view": true,
    "team.projects.create": true,
    "team.projects.assign": true,
    "project.*": true,
    "database.*": true
}', true),

(gen_random_uuid(), NULL, 'team_admin', 'Team Admin', 'Team administration', 'team', '{
    "team.view": true,
    "team.update": true,
    "team.members.view": true,
    "team.members.add": true,
    "team.members.remove": true,
    "team.members.update_role": true,
    "team.projects.view": true,
    "team.projects.create": true,
    "team.projects.assign": true,
    "project.*": true,
    "database.*": true
}', true),

(gen_random_uuid(), NULL, 'team_member', 'Team Member', 'Standard team access', 'team', '{
    "team.view": true,
    "team.members.view": true,
    "team.projects.view": true
}', true);

-- Project Roles
INSERT INTO platform.roles (role_id, org_id, name, display_name, description, role_type, permissions, is_system_role)
VALUES
(gen_random_uuid(), NULL, 'project_owner', 'Project Owner', 'Full project control', 'project', '{
    "project.view": true,
    "project.update": true,
    "project.delete": true,
    "project.settings.view": true,
    "project.settings.update": true,
    "project.databases.view": true,
    "project.databases.create": true,
    "project.databases.delete": true,
    "project.databases.modify": true,
    "project.metrics.view": true,
    "project.logs.view": true,
    "project.api_keys.manage": true,
    "database.*": true
}', true),

(gen_random_uuid(), NULL, 'project_admin', 'Project Admin', 'Project administration', 'project', '{
    "project.view": true,
    "project.update": true,
    "project.settings.view": true,
    "project.settings.update": true,
    "project.databases.view": true,
    "project.databases.create": true,
    "project.databases.delete": true,
    "project.databases.modify": true,
    "project.metrics.view": true,
    "project.logs.view": true,
    "project.api_keys.manage": true,
    "database.*": true
}', true),

(gen_random_uuid(), NULL, 'project_developer', 'Developer', 'Development access', 'project', '{
    "project.view": true,
    "project.settings.view": true,
    "project.databases.view": true,
    "project.databases.create": true,
    "project.databases.modify": true,
    "project.metrics.view": true,
    "project.logs.view": true,
    "database.connect": true,
    "database.view_metadata": true,
    "database.ddl.create": true,
    "database.ddl.alter": true,
    "database.dml.select": true,
    "database.dml.insert": true,
    "database.dml.update": true,
    "database.dml.delete": true
}', true),

(gen_random_uuid(), NULL, 'project_viewer', 'Viewer', 'Read-only access', 'project', '{
    "project.view": true,
    "project.settings.view": true,
    "project.databases.view": true,
    "project.metrics.view": true,
    "database.connect": true,
    "database.view_metadata": true,
    "database.dml.select": true
}', true);

-- Database Roles
INSERT INTO platform.roles (role_id, org_id, name, display_name, description, role_type, permissions, is_system_role)
VALUES
(gen_random_uuid(), NULL, 'db_admin', 'Database Admin', 'Full database control', 'database', '{
    "database.connect": true,
    "database.view_metadata": true,
    "database.ddl.create": true,
    "database.ddl.alter": true,
    "database.ddl.drop": true,
    "database.dml.select": true,
    "database.dml.insert": true,
    "database.dml.update": true,
    "database.dml.delete": true,
    "database.users.manage": true,
    "database.backup.view": true,
    "database.backup.restore": true,
    "database.extensions.manage": true,
    "database.replication.manage": true
}', true),

(gen_random_uuid(), NULL, 'db_readwrite', 'Read/Write', 'Data manipulation access', 'database', '{
    "database.connect": true,
    "database.view_metadata": true,
    "database.dml.select": true,
    "database.dml.insert": true,
    "database.dml.update": true,
    "database.dml.delete": true
}', true),

(gen_random_uuid(), NULL, 'db_readonly', 'Read Only', 'Query access only', 'database', '{
    "database.connect": true,
    "database.view_metadata": true,
    "database.dml.select": true
}', true),

(gen_random_uuid(), NULL, 'db_analyst', 'Analyst', 'Analytics schema access', 'database', '{
    "database.connect": true,
    "database.view_metadata": true,
    "database.dml.select": true,
    "orochi.analytics.query": true
}', true);

-- ============================================================
-- PERMISSION CHECK FUNCTIONS
-- ============================================================

-- Check if user has specific permission on a resource
CREATE OR REPLACE FUNCTION platform.check_permission(
    p_user_id UUID,
    p_resource_type VARCHAR(63),
    p_resource_id UUID,
    p_permission VARCHAR(127)
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
AS $$
DECLARE
    v_has_permission BOOLEAN := FALSE;
    v_org_id UUID;
    v_team_id UUID;
    v_project_id UUID;
    v_permissions JSONB;
BEGIN
    -- Get resource hierarchy
    CASE p_resource_type
        WHEN 'organization' THEN
            v_org_id := p_resource_id;
        WHEN 'team' THEN
            SELECT org_id INTO v_org_id
            FROM platform.teams WHERE team_id = p_resource_id;
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
        ELSE
            RETURN FALSE;
    END CASE;

    -- Check organization-level permissions
    SELECT r.permissions INTO v_permissions
    FROM platform.organization_members om
    JOIN platform.roles r ON om.role_id = r.role_id
    WHERE om.user_id = p_user_id
      AND om.org_id = v_org_id
      AND om.status = 'active';

    IF v_permissions IS NOT NULL THEN
        -- Check exact permission
        IF (v_permissions->>p_permission)::boolean = true THEN
            RETURN TRUE;
        END IF;
        -- Check wildcard permissions (e.g., "team.*" covers "team.view")
        IF (v_permissions->>(split_part(p_permission, '.', 1) || '.*'))::boolean = true THEN
            RETURN TRUE;
        END IF;
    END IF;

    -- Check team-level permissions (if applicable)
    IF v_team_id IS NOT NULL THEN
        SELECT r.permissions INTO v_permissions
        FROM platform.team_members tm
        JOIN platform.roles r ON tm.role_id = r.role_id
        WHERE tm.user_id = p_user_id
          AND tm.team_id = v_team_id;

        IF v_permissions IS NOT NULL THEN
            IF (v_permissions->>p_permission)::boolean = true THEN
                RETURN TRUE;
            END IF;
            IF (v_permissions->>(split_part(p_permission, '.', 1) || '.*'))::boolean = true THEN
                RETURN TRUE;
            END IF;
        END IF;
    END IF;

    -- Check project-level permissions (if applicable)
    IF v_project_id IS NOT NULL THEN
        SELECT r.permissions INTO v_permissions
        FROM platform.project_members pm
        JOIN platform.roles r ON pm.role_id = r.role_id
        WHERE pm.user_id = p_user_id
          AND pm.project_id = v_project_id;

        IF v_permissions IS NOT NULL THEN
            IF (v_permissions->>p_permission)::boolean = true THEN
                RETURN TRUE;
            END IF;
            IF (v_permissions->>(split_part(p_permission, '.', 1) || '.*'))::boolean = true THEN
                RETURN TRUE;
            END IF;
        END IF;
    END IF;

    RETURN FALSE;
END;
$$;

-- Get all permissions for a user on a resource
CREATE OR REPLACE FUNCTION platform.get_user_permissions(
    p_user_id UUID,
    p_resource_type VARCHAR(63),
    p_resource_id UUID
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
AS $$
DECLARE
    v_org_id UUID;
    v_team_id UUID;
    v_project_id UUID;
    v_permissions JSONB := '{}';
    v_role_permissions JSONB;
BEGIN
    -- Get resource hierarchy (same as check_permission)
    CASE p_resource_type
        WHEN 'organization' THEN
            v_org_id := p_resource_id;
        WHEN 'team' THEN
            SELECT org_id INTO v_org_id
            FROM platform.teams WHERE team_id = p_resource_id;
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

    -- Aggregate all applicable permissions
    FOR v_role_permissions IN
        -- Organization permissions
        SELECT r.permissions
        FROM platform.organization_members om
        JOIN platform.roles r ON om.role_id = r.role_id
        WHERE om.user_id = p_user_id
          AND om.org_id = v_org_id
          AND om.status = 'active'
        UNION ALL
        -- Team permissions
        SELECT r.permissions
        FROM platform.team_members tm
        JOIN platform.roles r ON tm.role_id = r.role_id
        WHERE tm.user_id = p_user_id
          AND tm.team_id = v_team_id
          AND v_team_id IS NOT NULL
        UNION ALL
        -- Project permissions
        SELECT r.permissions
        FROM platform.project_members pm
        JOIN platform.roles r ON pm.role_id = r.role_id
        WHERE pm.user_id = p_user_id
          AND pm.project_id = v_project_id
          AND v_project_id IS NOT NULL
    LOOP
        v_permissions := v_permissions || v_role_permissions;
    END LOOP;

    RETURN v_permissions;
END;
$$;

-- ============================================================
-- AUDIT LOGGING
-- ============================================================

CREATE TABLE platform.audit_logs (
    log_id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    event_time          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    org_id              UUID NOT NULL,
    user_id             UUID,
    api_key_id          UUID,
    event_category      platform.audit_category NOT NULL,
    event_action        VARCHAR(127) NOT NULL,
    event_outcome       platform.audit_outcome NOT NULL,
    resource_type       VARCHAR(63),
    resource_id         UUID,
    resource_name       VARCHAR(255),
    ip_address          INET,
    user_agent          TEXT,
    request_id          UUID,
    session_id          UUID,
    event_data          JSONB NOT NULL DEFAULT '{}',
    previous_state      JSONB,
    new_state           JSONB,
    authentication_method VARCHAR(63),
    mfa_used            BOOLEAN DEFAULT FALSE,
    risk_score          DECIMAL(3,2)
);

-- Partition audit logs by time (requires TimescaleDB or manual partitioning)
CREATE INDEX idx_audit_org_time ON platform.audit_logs(org_id, event_time DESC);
CREATE INDEX idx_audit_user ON platform.audit_logs(user_id, event_time DESC);
CREATE INDEX idx_audit_resource ON platform.audit_logs(resource_type, resource_id, event_time DESC);
CREATE INDEX idx_audit_action ON platform.audit_logs(event_action, event_time DESC);
CREATE INDEX idx_audit_category ON platform.audit_logs(event_category, event_time DESC);

-- Function to log audit events
CREATE OR REPLACE FUNCTION platform.log_audit_event(
    p_org_id UUID,
    p_user_id UUID,
    p_category platform.audit_category,
    p_action VARCHAR(127),
    p_outcome platform.audit_outcome,
    p_resource_type VARCHAR(63) DEFAULT NULL,
    p_resource_id UUID DEFAULT NULL,
    p_event_data JSONB DEFAULT '{}',
    p_previous_state JSONB DEFAULT NULL,
    p_new_state JSONB DEFAULT NULL
) RETURNS UUID
LANGUAGE plpgsql
AS $$
DECLARE
    v_log_id UUID;
BEGIN
    INSERT INTO platform.audit_logs (
        org_id, user_id, event_category, event_action, event_outcome,
        resource_type, resource_id, event_data, previous_state, new_state,
        ip_address, request_id
    ) VALUES (
        p_org_id, p_user_id, p_category, p_action, p_outcome,
        p_resource_type, p_resource_id, p_event_data, p_previous_state, p_new_state,
        inet_client_addr(), gen_random_uuid()
    )
    RETURNING log_id INTO v_log_id;

    RETURN v_log_id;
END;
$$;

-- ============================================================
-- RESOURCE QUOTAS
-- ============================================================

CREATE TABLE platform.resource_quotas (
    quota_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id              UUID NOT NULL REFERENCES platform.organizations(org_id) ON DELETE CASCADE,
    project_id          UUID REFERENCES platform.projects(project_id) ON DELETE CASCADE,
    database_id         UUID REFERENCES platform.databases(database_id) ON DELETE CASCADE,
    max_cpu_cores       DECIMAL(5,2),
    max_memory_gb       INTEGER,
    max_storage_gb      INTEGER,
    max_backup_storage_gb INTEGER,
    max_connections     INTEGER,
    max_connection_rate INTEGER,
    max_query_time_seconds INTEGER,
    max_rows_returned   BIGINT,
    max_result_size_mb  INTEGER,
    max_queries_per_minute INTEGER,
    max_writes_per_minute INTEGER,
    max_shards          INTEGER,
    max_chunks          INTEGER,
    max_pipelines       INTEGER,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT quota_hierarchy CHECK (
        (database_id IS NOT NULL) OR
        (database_id IS NULL AND project_id IS NOT NULL) OR
        (database_id IS NULL AND project_id IS NULL)
    )
);

CREATE INDEX idx_quotas_org ON platform.resource_quotas(org_id);
CREATE INDEX idx_quotas_project ON platform.resource_quotas(project_id);
CREATE INDEX idx_quotas_database ON platform.resource_quotas(database_id);

-- Default plan quotas
CREATE TABLE platform.plan_quotas (
    plan_tier           platform.plan_tier PRIMARY KEY,
    quotas              JSONB NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

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

-- ============================================================
-- ROW LEVEL SECURITY POLICIES
-- ============================================================

-- Enable RLS on key tables
ALTER TABLE platform.organizations ENABLE ROW LEVEL SECURITY;
ALTER TABLE platform.teams ENABLE ROW LEVEL SECURITY;
ALTER TABLE platform.projects ENABLE ROW LEVEL SECURITY;
ALTER TABLE platform.databases ENABLE ROW LEVEL SECURITY;
ALTER TABLE platform.audit_logs ENABLE ROW LEVEL SECURITY;

-- Organization access policy
CREATE POLICY org_access ON platform.organizations
    USING (
        org_id IN (
            SELECT om.org_id
            FROM platform.organization_members om
            WHERE om.user_id = current_setting('app.current_user_id', true)::uuid
              AND om.status = 'active'
        )
    );

-- Team access policy
CREATE POLICY team_access ON platform.teams
    USING (
        org_id IN (
            SELECT om.org_id
            FROM platform.organization_members om
            WHERE om.user_id = current_setting('app.current_user_id', true)::uuid
              AND om.status = 'active'
        )
    );

-- Project access policy
CREATE POLICY project_access ON platform.projects
    USING (
        org_id IN (
            SELECT om.org_id
            FROM platform.organization_members om
            WHERE om.user_id = current_setting('app.current_user_id', true)::uuid
              AND om.status = 'active'
        )
    );

-- Database access policy
CREATE POLICY database_access ON platform.databases
    USING (
        project_id IN (
            SELECT p.project_id
            FROM platform.projects p
            JOIN platform.organization_members om ON p.org_id = om.org_id
            WHERE om.user_id = current_setting('app.current_user_id', true)::uuid
              AND om.status = 'active'
        )
    );

-- Audit log access policy (org-scoped)
CREATE POLICY audit_org_access ON platform.audit_logs
    USING (
        org_id IN (
            SELECT om.org_id
            FROM platform.organization_members om
            JOIN platform.roles r ON om.role_id = r.role_id
            WHERE om.user_id = current_setting('app.current_user_id', true)::uuid
              AND om.status = 'active'
              AND (r.permissions->>'org.audit_log.view')::boolean = true
        )
    );

-- ============================================================
-- HELPER VIEWS
-- ============================================================

-- View: User's accessible resources
CREATE OR REPLACE VIEW platform.user_resources AS
SELECT
    u.user_id,
    u.email,
    o.org_id,
    o.name as org_name,
    o.plan_tier,
    om.status as org_membership_status,
    r.name as org_role,
    r.permissions as org_permissions
FROM platform.users u
JOIN platform.organization_members om ON u.user_id = om.user_id
JOIN platform.organizations o ON om.org_id = o.org_id
JOIN platform.roles r ON om.role_id = r.role_id
WHERE om.status = 'active'
  AND o.status = 'active';

-- View: Database access summary
CREATE OR REPLACE VIEW platform.database_access_summary AS
SELECT
    d.database_id,
    d.name as database_name,
    p.project_id,
    p.name as project_name,
    o.org_id,
    o.name as org_name,
    d.isolation_mode,
    d.status,
    d.instance_type,
    d.storage_size_gb
FROM platform.databases d
JOIN platform.projects p ON d.project_id = p.project_id
JOIN platform.organizations o ON p.org_id = o.org_id
WHERE d.status != 'deleted';

-- ============================================================
-- TRIGGERS FOR AUDIT LOGGING
-- ============================================================

-- Trigger function for automatic audit logging
CREATE OR REPLACE FUNCTION platform.audit_trigger_func()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
    v_action VARCHAR(127);
    v_org_id UUID;
BEGIN
    -- Determine action
    IF TG_OP = 'INSERT' THEN
        v_action := TG_TABLE_NAME || '.create';
    ELSIF TG_OP = 'UPDATE' THEN
        v_action := TG_TABLE_NAME || '.update';
    ELSIF TG_OP = 'DELETE' THEN
        v_action := TG_TABLE_NAME || '.delete';
    END IF;

    -- Get org_id from the record
    IF TG_TABLE_NAME = 'organizations' THEN
        v_org_id := COALESCE(NEW.org_id, OLD.org_id);
    ELSIF TG_TABLE_NAME IN ('teams', 'projects') THEN
        v_org_id := COALESCE(NEW.org_id, OLD.org_id);
    ELSIF TG_TABLE_NAME = 'databases' THEN
        SELECT org_id INTO v_org_id
        FROM platform.projects
        WHERE project_id = COALESCE(NEW.project_id, OLD.project_id);
    END IF;

    -- Log the event
    PERFORM platform.log_audit_event(
        v_org_id,
        current_setting('app.current_user_id', true)::uuid,
        'configuration',
        v_action,
        'success',
        TG_TABLE_NAME,
        COALESCE(NEW.org_id, NEW.team_id, NEW.project_id, NEW.database_id,
                 OLD.org_id, OLD.team_id, OLD.project_id, OLD.database_id),
        CASE WHEN TG_OP != 'DELETE' THEN to_jsonb(NEW) ELSE '{}' END,
        CASE WHEN TG_OP != 'INSERT' THEN to_jsonb(OLD) ELSE NULL END,
        CASE WHEN TG_OP != 'DELETE' THEN to_jsonb(NEW) ELSE NULL END
    );

    RETURN COALESCE(NEW, OLD);
END;
$$;

-- Apply audit triggers
CREATE TRIGGER audit_organizations
    AFTER INSERT OR UPDATE OR DELETE ON platform.organizations
    FOR EACH ROW EXECUTE FUNCTION platform.audit_trigger_func();

CREATE TRIGGER audit_teams
    AFTER INSERT OR UPDATE OR DELETE ON platform.teams
    FOR EACH ROW EXECUTE FUNCTION platform.audit_trigger_func();

CREATE TRIGGER audit_projects
    AFTER INSERT OR UPDATE OR DELETE ON platform.projects
    FOR EACH ROW EXECUTE FUNCTION platform.audit_trigger_func();

CREATE TRIGGER audit_databases
    AFTER INSERT OR UPDATE OR DELETE ON platform.databases
    FOR EACH ROW EXECUTE FUNCTION platform.audit_trigger_func();

-- ============================================================
-- UTILITY FUNCTIONS
-- ============================================================

-- Generate API key
CREATE OR REPLACE FUNCTION platform.generate_api_key(
    p_user_id UUID,
    p_name VARCHAR(255),
    p_scope platform.api_key_scope,
    p_permissions JSONB DEFAULT '[]',
    p_expires_in INTERVAL DEFAULT NULL
) RETURNS TABLE(key_id UUID, api_key TEXT)
LANGUAGE plpgsql
AS $$
DECLARE
    v_key_id UUID := gen_random_uuid();
    v_raw_key TEXT;
    v_key_prefix VARCHAR(12);
    v_key_hash VARCHAR(255);
BEGIN
    -- Generate random key: prefix_random
    v_raw_key := 'orochi_' || encode(gen_random_bytes(32), 'base64');
    v_raw_key := replace(replace(v_raw_key, '+', 'x'), '/', 'y');
    v_key_prefix := substring(v_raw_key from 1 for 12);
    v_key_hash := crypt(v_raw_key, gen_salt('bf', 12));

    INSERT INTO platform.api_keys (
        key_id, user_id, name, key_prefix, key_hash, scope,
        permissions, expires_at
    ) VALUES (
        v_key_id, p_user_id, p_name, v_key_prefix, v_key_hash, p_scope,
        p_permissions,
        CASE WHEN p_expires_in IS NOT NULL THEN NOW() + p_expires_in ELSE NULL END
    );

    RETURN QUERY SELECT v_key_id, v_raw_key;
END;
$$;

-- Validate API key
CREATE OR REPLACE FUNCTION platform.validate_api_key(
    p_api_key TEXT
) RETURNS TABLE(
    key_id UUID,
    user_id UUID,
    org_id UUID,
    scope platform.api_key_scope,
    permissions JSONB
)
LANGUAGE plpgsql
AS $$
DECLARE
    v_key_prefix VARCHAR(12);
    v_key_record RECORD;
BEGIN
    v_key_prefix := substring(p_api_key from 1 for 12);

    SELECT ak.* INTO v_key_record
    FROM platform.api_keys ak
    WHERE ak.key_prefix = v_key_prefix
      AND ak.status = 'active'
      AND (ak.expires_at IS NULL OR ak.expires_at > NOW());

    IF v_key_record IS NULL THEN
        RETURN;
    END IF;

    -- Verify key hash
    IF v_key_record.key_hash = crypt(p_api_key, v_key_record.key_hash) THEN
        -- Update last used
        UPDATE platform.api_keys
        SET last_used_at = NOW(),
            use_count = use_count + 1,
            last_used_ip = inet_client_addr()
        WHERE platform.api_keys.key_id = v_key_record.key_id;

        RETURN QUERY SELECT
            v_key_record.key_id,
            v_key_record.user_id,
            v_key_record.org_id,
            v_key_record.scope,
            v_key_record.permissions;
    END IF;

    RETURN;
END;
$$;

-- ============================================================
-- COMMENTS
-- ============================================================

COMMENT ON SCHEMA platform IS 'Multi-tenancy management schema for Orochi DB Managed Service';
COMMENT ON TABLE platform.organizations IS 'Top-level billing entities (tenants)';
COMMENT ON TABLE platform.users IS 'User accounts with authentication details';
COMMENT ON TABLE platform.roles IS 'Role definitions with permission sets';
COMMENT ON TABLE platform.teams IS 'Team groupings within organizations';
COMMENT ON TABLE platform.projects IS 'Projects containing databases';
COMMENT ON TABLE platform.databases IS 'Managed PostgreSQL database instances';
COMMENT ON TABLE platform.api_keys IS 'API keys for programmatic access';
COMMENT ON TABLE platform.audit_logs IS 'Comprehensive audit trail of all actions';

COMMENT ON FUNCTION platform.check_permission IS 'Check if user has specific permission on resource';
COMMENT ON FUNCTION platform.get_user_permissions IS 'Get all permissions for user on resource';
COMMENT ON FUNCTION platform.log_audit_event IS 'Log an audit event';
COMMENT ON FUNCTION platform.generate_api_key IS 'Generate new API key for user';
COMMENT ON FUNCTION platform.validate_api_key IS 'Validate API key and return associated permissions';
