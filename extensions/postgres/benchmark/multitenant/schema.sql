--
-- Multi-Tenant Schema Definitions for Orochi DB
--
-- This file contains schema definitions for testing different
-- multi-tenant isolation models:
--
-- 1. Shared Table with tenant_id (Row-Level Security)
-- 2. Schema-per-Tenant (Schema-based isolation)
-- 3. Hybrid approach (Sharded by tenant with RLS)
--
-- Usage:
--   psql -f schema.sql -d orochi_bench
--

-- =============================================================================
-- Cleanup existing objects
-- =============================================================================

DROP TABLE IF EXISTS tenant_data CASCADE;
DROP TABLE IF EXISTS tenant_metrics CASCADE;
DROP TABLE IF EXISTS tenant_audit_log CASCADE;
DROP TABLE IF EXISTS tenants CASCADE;

DROP FUNCTION IF EXISTS set_tenant_context(INTEGER);
DROP FUNCTION IF EXISTS get_current_tenant();
DROP FUNCTION IF EXISTS audit_tenant_access();

-- Drop tenant-specific schemas (for schema isolation model)
DO $$
DECLARE
    schema_name TEXT;
BEGIN
    FOR schema_name IN (
        SELECT nspname FROM pg_namespace
        WHERE nspname LIKE 'tenant_%'
    ) LOOP
        EXECUTE format('DROP SCHEMA IF EXISTS %I CASCADE', schema_name);
    END LOOP;
END $$;

-- =============================================================================
-- MODEL 1: Shared Table with Row-Level Security
-- =============================================================================

-- Tenant registry table
CREATE TABLE tenants (
    tenant_id       SERIAL PRIMARY KEY,
    tenant_name     VARCHAR(100) NOT NULL UNIQUE,
    tenant_tier     VARCHAR(20) DEFAULT 'standard',  -- standard, premium, enterprise
    created_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    quota_rows      BIGINT DEFAULT 1000000,          -- Row quota per tenant
    quota_storage_mb INTEGER DEFAULT 1000,            -- Storage quota in MB
    is_active       BOOLEAN DEFAULT TRUE,
    metadata        JSONB DEFAULT '{}'::JSONB
);

-- Create indexes for tenant table
CREATE INDEX idx_tenants_name ON tenants(tenant_name);
CREATE INDEX idx_tenants_tier ON tenants(tenant_tier);
CREATE INDEX idx_tenants_active ON tenants(is_active) WHERE is_active = TRUE;

-- Main tenant data table (shared table model)
CREATE TABLE tenant_data (
    id              BIGSERIAL,
    tenant_id       INTEGER NOT NULL REFERENCES tenants(tenant_id) ON DELETE CASCADE,
    key             VARCHAR(255) NOT NULL,
    value           TEXT,
    numeric_value   NUMERIC(15, 4),
    json_data       JSONB,
    vector_data     FLOAT8[],               -- For vector similarity workloads
    created_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    updated_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW(),

    -- Composite primary key ensures tenant data locality
    PRIMARY KEY (tenant_id, id)
) PARTITION BY LIST (tenant_id);

-- Create default partition for new tenants
CREATE TABLE tenant_data_default PARTITION OF tenant_data DEFAULT;

-- Indexes for tenant data (will be inherited by partitions)
CREATE INDEX idx_tenant_data_key ON tenant_data(tenant_id, key);
CREATE INDEX idx_tenant_data_created ON tenant_data(tenant_id, created_at DESC);
CREATE INDEX idx_tenant_data_numeric ON tenant_data(tenant_id, numeric_value)
    WHERE numeric_value IS NOT NULL;
CREATE INDEX idx_tenant_data_json ON tenant_data USING GIN (json_data jsonb_path_ops)
    WHERE json_data IS NOT NULL;

-- Tenant metrics table (for monitoring per-tenant usage)
CREATE TABLE tenant_metrics (
    id              BIGSERIAL,
    tenant_id       INTEGER NOT NULL REFERENCES tenants(tenant_id) ON DELETE CASCADE,
    metric_name     VARCHAR(100) NOT NULL,
    metric_value    NUMERIC(20, 4) NOT NULL,
    recorded_at     TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    PRIMARY KEY (tenant_id, id)
);

CREATE INDEX idx_tenant_metrics_name ON tenant_metrics(tenant_id, metric_name, recorded_at DESC);

-- Audit log for tenant access (security compliance)
CREATE TABLE tenant_audit_log (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       INTEGER REFERENCES tenants(tenant_id),
    action          VARCHAR(50) NOT NULL,   -- SELECT, INSERT, UPDATE, DELETE
    table_name      VARCHAR(100) NOT NULL,
    row_count       INTEGER,
    user_name       VARCHAR(100),
    session_id      TEXT,
    ip_address      INET,
    query_text      TEXT,
    executed_at     TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE INDEX idx_audit_log_tenant ON tenant_audit_log(tenant_id, executed_at DESC);
CREATE INDEX idx_audit_log_action ON tenant_audit_log(action, executed_at DESC);

-- =============================================================================
-- Row-Level Security Policies
-- =============================================================================

-- Enable RLS on tenant data tables
ALTER TABLE tenant_data ENABLE ROW LEVEL SECURITY;
ALTER TABLE tenant_metrics ENABLE ROW LEVEL SECURITY;
ALTER TABLE tenant_audit_log ENABLE ROW LEVEL SECURITY;

-- Create policies for tenant_data
-- Note: These policies use session variables set by the application

-- Policy for SELECT operations
CREATE POLICY tenant_data_select_policy ON tenant_data
    FOR SELECT
    USING (
        tenant_id = COALESCE(
            NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER,
            0
        )
    );

-- Policy for INSERT operations
CREATE POLICY tenant_data_insert_policy ON tenant_data
    FOR INSERT
    WITH CHECK (
        tenant_id = COALESCE(
            NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER,
            0
        )
    );

-- Policy for UPDATE operations
CREATE POLICY tenant_data_update_policy ON tenant_data
    FOR UPDATE
    USING (
        tenant_id = COALESCE(
            NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER,
            0
        )
    )
    WITH CHECK (
        tenant_id = COALESCE(
            NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER,
            0
        )
    );

-- Policy for DELETE operations
CREATE POLICY tenant_data_delete_policy ON tenant_data
    FOR DELETE
    USING (
        tenant_id = COALESCE(
            NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER,
            0
        )
    );

-- Similar policies for tenant_metrics
CREATE POLICY tenant_metrics_select_policy ON tenant_metrics
    FOR SELECT
    USING (tenant_id = NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER);

CREATE POLICY tenant_metrics_insert_policy ON tenant_metrics
    FOR INSERT
    WITH CHECK (tenant_id = NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER);

-- Audit log: tenants can only see their own audit entries
CREATE POLICY tenant_audit_select_policy ON tenant_audit_log
    FOR SELECT
    USING (
        tenant_id = NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER
        OR current_setting('app.is_admin', true)::BOOLEAN = TRUE
    );

-- Only system can insert audit logs
CREATE POLICY tenant_audit_insert_policy ON tenant_audit_log
    FOR INSERT
    WITH CHECK (
        current_setting('app.is_system', true)::BOOLEAN = TRUE
    );

-- =============================================================================
-- Helper Functions
-- =============================================================================

-- Function to set current tenant context
CREATE OR REPLACE FUNCTION set_tenant_context(p_tenant_id INTEGER)
RETURNS VOID AS $$
BEGIN
    -- Validate tenant exists and is active
    IF NOT EXISTS (SELECT 1 FROM tenants WHERE tenant_id = p_tenant_id AND is_active = TRUE) THEN
        RAISE EXCEPTION 'Invalid or inactive tenant: %', p_tenant_id;
    END IF;

    -- Set session variables
    PERFORM set_config('app.current_tenant_id', p_tenant_id::TEXT, false);
    PERFORM set_config('app.is_admin', 'false', false);
    PERFORM set_config('app.is_system', 'false', false);
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Function to get current tenant ID
CREATE OR REPLACE FUNCTION get_current_tenant()
RETURNS INTEGER AS $$
BEGIN
    RETURN NULLIF(current_setting('app.current_tenant_id', true), '')::INTEGER;
END;
$$ LANGUAGE plpgsql STABLE;

-- Audit logging trigger function
CREATE OR REPLACE FUNCTION audit_tenant_access()
RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO tenant_audit_log (
        tenant_id,
        action,
        table_name,
        row_count,
        user_name,
        session_id,
        executed_at
    ) VALUES (
        get_current_tenant(),
        TG_OP,
        TG_TABLE_NAME,
        CASE TG_OP
            WHEN 'INSERT' THEN 1
            WHEN 'UPDATE' THEN 1
            WHEN 'DELETE' THEN 1
            ELSE 0
        END,
        current_user,
        pg_backend_pid()::TEXT,
        NOW()
    );

    RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- =============================================================================
-- MODEL 2: Schema-per-Tenant (Generated dynamically)
-- =============================================================================

-- Function to create a new tenant schema
CREATE OR REPLACE FUNCTION create_tenant_schema(p_tenant_id INTEGER)
RETURNS VOID AS $$
DECLARE
    schema_name TEXT := 'tenant_' || p_tenant_id;
BEGIN
    -- Create tenant-specific schema
    EXECUTE format('CREATE SCHEMA IF NOT EXISTS %I', schema_name);

    -- Create data table in tenant schema
    EXECUTE format('
        CREATE TABLE IF NOT EXISTS %I.data (
            id              BIGSERIAL PRIMARY KEY,
            key             VARCHAR(255) NOT NULL,
            value           TEXT,
            numeric_value   NUMERIC(15, 4),
            json_data       JSONB,
            vector_data     FLOAT8[],
            created_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
            updated_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW()
        )', schema_name);

    -- Create indexes
    EXECUTE format('CREATE INDEX IF NOT EXISTS idx_%s_data_key ON %I.data(key)', schema_name, schema_name);
    EXECUTE format('CREATE INDEX IF NOT EXISTS idx_%s_data_created ON %I.data(created_at DESC)', schema_name, schema_name);

    -- Create metrics table
    EXECUTE format('
        CREATE TABLE IF NOT EXISTS %I.metrics (
            id              BIGSERIAL PRIMARY KEY,
            metric_name     VARCHAR(100) NOT NULL,
            metric_value    NUMERIC(20, 4) NOT NULL,
            recorded_at     TIMESTAMP WITH TIME ZONE DEFAULT NOW()
        )', schema_name);

    RAISE NOTICE 'Created schema for tenant %', p_tenant_id;
END;
$$ LANGUAGE plpgsql;

-- Function to drop a tenant schema
CREATE OR REPLACE FUNCTION drop_tenant_schema(p_tenant_id INTEGER)
RETURNS VOID AS $$
DECLARE
    schema_name TEXT := 'tenant_' || p_tenant_id;
BEGIN
    EXECUTE format('DROP SCHEMA IF EXISTS %I CASCADE', schema_name);
    RAISE NOTICE 'Dropped schema for tenant %', p_tenant_id;
END;
$$ LANGUAGE plpgsql;

-- =============================================================================
-- MODEL 3: Partition-per-Tenant (For high isolation + performance)
-- =============================================================================

-- Function to create partition for a specific tenant
CREATE OR REPLACE FUNCTION create_tenant_partition(p_tenant_id INTEGER)
RETURNS VOID AS $$
DECLARE
    partition_name TEXT := 'tenant_data_' || p_tenant_id;
BEGIN
    -- Check if partition already exists
    IF NOT EXISTS (
        SELECT 1 FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relname = partition_name
        AND n.nspname = 'public'
    ) THEN
        EXECUTE format('
            CREATE TABLE %I PARTITION OF tenant_data
            FOR VALUES IN (%L)',
            partition_name, p_tenant_id
        );

        RAISE NOTICE 'Created partition for tenant %', p_tenant_id;
    ELSE
        RAISE NOTICE 'Partition already exists for tenant %', p_tenant_id;
    END IF;
END;
$$ LANGUAGE plpgsql;

-- =============================================================================
-- Initial Data Setup
-- =============================================================================

-- Create default tenants for benchmarking
INSERT INTO tenants (tenant_name, tenant_tier, quota_rows, quota_storage_mb) VALUES
    ('tenant_1', 'standard', 100000, 500),
    ('tenant_2', 'standard', 100000, 500),
    ('tenant_3', 'premium', 500000, 2000),
    ('tenant_4', 'premium', 500000, 2000),
    ('tenant_5', 'enterprise', 1000000, 5000),
    ('tenant_6', 'enterprise', 1000000, 5000),
    ('tenant_7', 'standard', 100000, 500),
    ('tenant_8', 'standard', 100000, 500),
    ('tenant_9', 'premium', 500000, 2000),
    ('tenant_10', 'enterprise', 1000000, 5000)
ON CONFLICT (tenant_name) DO NOTHING;

-- Create partitions for each tenant
DO $$
DECLARE
    t RECORD;
BEGIN
    FOR t IN SELECT tenant_id FROM tenants LOOP
        PERFORM create_tenant_partition(t.tenant_id);
    END LOOP;
END $$;

-- Create schemas for schema-based isolation model
DO $$
DECLARE
    t RECORD;
BEGIN
    FOR t IN SELECT tenant_id FROM tenants LOOP
        PERFORM create_tenant_schema(t.tenant_id);
    END LOOP;
END $$;

-- =============================================================================
-- Views for Monitoring
-- =============================================================================

-- View: Tenant usage statistics
CREATE OR REPLACE VIEW tenant_usage_stats AS
SELECT
    t.tenant_id,
    t.tenant_name,
    t.tenant_tier,
    t.quota_rows,
    COALESCE(d.row_count, 0) AS actual_rows,
    ROUND(COALESCE(d.row_count, 0)::NUMERIC / t.quota_rows * 100, 2) AS usage_percent,
    t.is_active
FROM tenants t
LEFT JOIN (
    SELECT tenant_id, COUNT(*) as row_count
    FROM tenant_data
    GROUP BY tenant_id
) d ON t.tenant_id = d.tenant_id;

-- View: Cross-tenant performance comparison
CREATE OR REPLACE VIEW tenant_performance_comparison AS
SELECT
    t.tenant_id,
    t.tenant_name,
    t.tenant_tier,
    COALESCE(m.avg_latency, 0) AS avg_query_latency_ms,
    COALESCE(m.total_queries, 0) AS total_queries,
    COALESCE(m.error_count, 0) AS error_count
FROM tenants t
LEFT JOIN (
    SELECT
        tenant_id,
        AVG(CASE WHEN metric_name = 'query_latency_ms' THEN metric_value END) AS avg_latency,
        SUM(CASE WHEN metric_name = 'query_count' THEN metric_value END) AS total_queries,
        SUM(CASE WHEN metric_name = 'error_count' THEN metric_value END) AS error_count
    FROM tenant_metrics
    WHERE recorded_at > NOW() - INTERVAL '1 hour'
    GROUP BY tenant_id
) m ON t.tenant_id = m.tenant_id
WHERE t.is_active = TRUE;

-- =============================================================================
-- Comments and Documentation
-- =============================================================================

COMMENT ON TABLE tenants IS 'Registry of all tenants in the multi-tenant system';
COMMENT ON TABLE tenant_data IS 'Main data table using shared-table multi-tenancy with RLS';
COMMENT ON TABLE tenant_metrics IS 'Per-tenant performance and usage metrics';
COMMENT ON TABLE tenant_audit_log IS 'Security audit log of all tenant data access';

COMMENT ON FUNCTION set_tenant_context(INTEGER) IS 'Sets the current tenant context for RLS policies';
COMMENT ON FUNCTION get_current_tenant() IS 'Returns the current tenant ID from session context';
COMMENT ON FUNCTION create_tenant_schema(INTEGER) IS 'Creates a dedicated schema for a tenant (schema isolation model)';
COMMENT ON FUNCTION create_tenant_partition(INTEGER) IS 'Creates a dedicated partition for a tenant (partition isolation model)';

-- =============================================================================
-- Grant permissions
-- =============================================================================

-- Grant basic permissions to public role for benchmarking
GRANT SELECT, INSERT, UPDATE, DELETE ON tenant_data TO PUBLIC;
GRANT SELECT, INSERT ON tenant_metrics TO PUBLIC;
GRANT SELECT ON tenants TO PUBLIC;
GRANT SELECT ON tenant_usage_stats TO PUBLIC;
GRANT SELECT ON tenant_performance_comparison TO PUBLIC;
GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO PUBLIC;

-- Grant execute permissions on helper functions
GRANT EXECUTE ON FUNCTION set_tenant_context(INTEGER) TO PUBLIC;
GRANT EXECUTE ON FUNCTION get_current_tenant() TO PUBLIC;

\echo ''
\echo 'Multi-tenant schema created successfully!'
\echo ''
\echo 'Available isolation models:'
\echo '  1. Shared Table with RLS: Use tenant_data table with set_tenant_context()'
\echo '  2. Schema per Tenant: Use tenant_N.data tables'
\echo '  3. Partition per Tenant: Uses tenant_data with automatic partitioning'
\echo ''
\echo 'Example usage:'
\echo '  SELECT set_tenant_context(1);'
\echo '  SELECT * FROM tenant_data;  -- Only shows tenant 1 data'
\echo ''
