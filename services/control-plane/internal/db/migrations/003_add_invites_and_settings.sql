-- Migration: 003_add_invites_and_settings
-- Description: Add organization invites, query history, and cluster settings support

-- Organization invites table for team onboarding
CREATE TABLE IF NOT EXISTS organization_invites (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    organization_id UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    email VARCHAR(255) NOT NULL,
    role VARCHAR(50) NOT NULL DEFAULT 'member', -- owner, admin, member, viewer
    token VARCHAR(255) NOT NULL UNIQUE,
    invited_by UUID NOT NULL REFERENCES users(id),
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    accepted_at TIMESTAMP WITH TIME ZONE,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    UNIQUE(organization_id, email)
);

-- Indexes for invites
CREATE INDEX IF NOT EXISTS idx_organization_invites_org_id ON organization_invites(organization_id);
CREATE INDEX IF NOT EXISTS idx_organization_invites_email ON organization_invites(email);
CREATE INDEX IF NOT EXISTS idx_organization_invites_token ON organization_invites(token);
CREATE INDEX IF NOT EXISTS idx_organization_invites_expires_at ON organization_invites(expires_at);

-- Query history for SQL editor
CREATE TABLE IF NOT EXISTS query_history (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE,
    query_text TEXT NOT NULL,
    description TEXT, -- AI-generated or user-provided description
    execution_time_ms INTEGER,
    rows_affected INTEGER,
    status VARCHAR(20) NOT NULL DEFAULT 'success', -- success, error
    error_message TEXT,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Indexes for query history
CREATE INDEX IF NOT EXISTS idx_query_history_user_id ON query_history(user_id);
CREATE INDEX IF NOT EXISTS idx_query_history_cluster_id ON query_history(cluster_id);
CREATE INDEX IF NOT EXISTS idx_query_history_created_at ON query_history(created_at DESC);

-- Saved queries for SQL editor bookmarks
CREATE TABLE IF NOT EXISTS saved_queries (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    cluster_id UUID REFERENCES clusters(id) ON DELETE CASCADE, -- NULL = global
    name VARCHAR(255) NOT NULL,
    description TEXT,
    query_text TEXT NOT NULL,
    is_favorite BOOLEAN NOT NULL DEFAULT false,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Indexes for saved queries
CREATE INDEX IF NOT EXISTS idx_saved_queries_user_id ON saved_queries(user_id);
CREATE INDEX IF NOT EXISTS idx_saved_queries_cluster_id ON saved_queries(cluster_id);

-- Trigger for saved_queries updated_at
DROP TRIGGER IF EXISTS update_saved_queries_updated_at ON saved_queries;
CREATE TRIGGER update_saved_queries_updated_at
    BEFORE UPDATE ON saved_queries
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- Cluster settings for advanced configuration
CREATE TABLE IF NOT EXISTS cluster_settings (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE UNIQUE,
    -- Connection pooling settings
    pooler_mode VARCHAR(20) NOT NULL DEFAULT 'transaction', -- transaction, session, statement
    pooler_default_pool_size INTEGER NOT NULL DEFAULT 25,
    pooler_max_client_conn INTEGER NOT NULL DEFAULT 100,
    -- Performance settings
    statement_timeout_ms INTEGER NOT NULL DEFAULT 30000,
    idle_in_transaction_timeout_ms INTEGER NOT NULL DEFAULT 60000,
    -- Maintenance settings
    auto_vacuum_enabled BOOLEAN NOT NULL DEFAULT true,
    -- Monitoring settings
    pg_stat_statements_enabled BOOLEAN NOT NULL DEFAULT true,
    slow_query_threshold_ms INTEGER NOT NULL DEFAULT 1000,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Trigger for cluster_settings updated_at
DROP TRIGGER IF EXISTS update_cluster_settings_updated_at ON cluster_settings;
CREATE TRIGGER update_cluster_settings_updated_at
    BEFORE UPDATE ON cluster_settings
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- Add default organization for users during registration (optional)
ALTER TABLE users ADD COLUMN IF NOT EXISTS default_organization_id UUID REFERENCES organizations(id) ON DELETE SET NULL;
CREATE INDEX IF NOT EXISTS idx_users_default_org ON users(default_organization_id);

-- Performance advisor recommendations table
CREATE TABLE IF NOT EXISTS performance_recommendations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE,
    type VARCHAR(50) NOT NULL, -- missing_index, unused_index, slow_query, connection_issue
    severity VARCHAR(20) NOT NULL DEFAULT 'info', -- info, warning, critical
    title VARCHAR(255) NOT NULL,
    description TEXT NOT NULL,
    recommendation TEXT NOT NULL,
    metadata JSONB, -- Additional context (table name, index suggestion, etc.)
    is_dismissed BOOLEAN NOT NULL DEFAULT false,
    dismissed_at TIMESTAMP WITH TIME ZONE,
    dismissed_by UUID REFERENCES users(id),
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Indexes for recommendations
CREATE INDEX IF NOT EXISTS idx_performance_recommendations_cluster_id ON performance_recommendations(cluster_id);
CREATE INDEX IF NOT EXISTS idx_performance_recommendations_type ON performance_recommendations(type);
CREATE INDEX IF NOT EXISTS idx_performance_recommendations_severity ON performance_recommendations(severity);
CREATE INDEX IF NOT EXISTS idx_performance_recommendations_is_dismissed ON performance_recommendations(is_dismissed);
