-- PgDog Integration Test Database Setup
-- Creates test tables and data for integration testing

-- Enable Orochi extension
CREATE EXTENSION IF NOT EXISTS orochi;

-- Create test schema
CREATE SCHEMA IF NOT EXISTS test_data;

-- Multi-tenant users table (for sharding tests)
CREATE TABLE test_data.users (
    id BIGSERIAL PRIMARY KEY,
    tenant_id INTEGER NOT NULL,
    email VARCHAR(255) NOT NULL,
    name VARCHAR(255),
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Distributed orders table (for sharding tests)
CREATE TABLE test_data.orders (
    id BIGSERIAL PRIMARY KEY,
    tenant_id INTEGER NOT NULL,
    user_id BIGINT NOT NULL,
    total_amount DECIMAL(10,2),
    status VARCHAR(50) DEFAULT 'pending',
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Create indexes
CREATE INDEX idx_users_tenant ON test_data.users(tenant_id);
CREATE INDEX idx_orders_tenant ON test_data.orders(tenant_id);

-- Insert test data across multiple tenants
INSERT INTO test_data.users (tenant_id, email, name) 
SELECT 
    (i % 10) + 1,  -- tenant_id 1-10
    'user' || i || '@tenant' || ((i % 10) + 1) || '.com',
    'User ' || i
FROM generate_series(1, 1000) AS i;

INSERT INTO test_data.orders (tenant_id, user_id, total_amount, status)
SELECT 
    (i % 10) + 1,
    (i % 1000) + 1,
    (random() * 1000)::DECIMAL(10,2),
    CASE (i % 4) 
        WHEN 0 THEN 'pending'
        WHEN 1 THEN 'completed'
        WHEN 2 THEN 'shipped'
        ELSE 'cancelled'
    END
FROM generate_series(1, 5000) AS i;

-- Create hypertable for time-series tests (if orochi supports it)
CREATE TABLE test_data.metrics (
    time TIMESTAMPTZ NOT NULL,
    device_id INTEGER NOT NULL,
    metric_name VARCHAR(100),
    value DOUBLE PRECISION
);

-- Create reference table (replicated across all shards)
CREATE TABLE test_data.tenants (
    id SERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    plan VARCHAR(50) DEFAULT 'free',
    created_at TIMESTAMPTZ DEFAULT NOW()
);

INSERT INTO test_data.tenants (id, name, plan)
SELECT i, 'Tenant ' || i, 
    CASE (i % 3)
        WHEN 0 THEN 'free'
        WHEN 1 THEN 'pro'
        ELSE 'enterprise'
    END
FROM generate_series(1, 10) AS i;

-- Grant permissions
GRANT ALL ON SCHEMA test_data TO postgres;
GRANT ALL ON ALL TABLES IN SCHEMA test_data TO postgres;
GRANT ALL ON ALL SEQUENCES IN SCHEMA test_data TO postgres;

-- Create test roles for RLS testing
CREATE ROLE test_user LOGIN PASSWORD 'test_user_pass';
GRANT CONNECT ON DATABASE orochi_test TO test_user;
GRANT USAGE ON SCHEMA test_data TO test_user;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA test_data TO test_user;

-- Row Level Security for tenant isolation
ALTER TABLE test_data.users ENABLE ROW LEVEL SECURITY;
ALTER TABLE test_data.orders ENABLE ROW LEVEL SECURITY;

-- RLS policies based on session variable
CREATE POLICY tenant_isolation_users ON test_data.users
    USING (tenant_id = current_setting('orochi.tenant_id', true)::INTEGER);

CREATE POLICY tenant_isolation_orders ON test_data.orders
    USING (tenant_id = current_setting('orochi.tenant_id', true)::INTEGER);

-- Function to set tenant context (used after JWT validation)
CREATE OR REPLACE FUNCTION test_data.set_tenant_context(p_tenant_id INTEGER)
RETURNS VOID AS $$
BEGIN
    PERFORM set_config('orochi.tenant_id', p_tenant_id::TEXT, false);
END;
$$ LANGUAGE plpgsql;

GRANT EXECUTE ON FUNCTION test_data.set_tenant_context TO test_user;

-- Verify setup
SELECT 'Database setup complete' AS status;
SELECT COUNT(*) AS user_count FROM test_data.users;
SELECT COUNT(*) AS order_count FROM test_data.orders;
SELECT COUNT(*) AS tenant_count FROM test_data.tenants;
