-- Migration: 001_initial
-- Description: Initial schema for Orochi Cloud control plane

-- Users table
CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email VARCHAR(255) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    name VARCHAR(255) NOT NULL,
    role VARCHAR(50) NOT NULL DEFAULT 'member',
    active BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    last_login_at TIMESTAMP WITH TIME ZONE
);

-- Clusters table
CREATE TABLE IF NOT EXISTS clusters (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(63) NOT NULL,
    owner_id UUID NOT NULL REFERENCES users(id),
    status VARCHAR(50) NOT NULL DEFAULT 'pending',
    tier VARCHAR(50) NOT NULL,
    provider VARCHAR(50) NOT NULL,
    region VARCHAR(100) NOT NULL,
    version VARCHAR(50) NOT NULL,
    node_count INTEGER NOT NULL DEFAULT 1,
    node_size VARCHAR(50) NOT NULL,
    storage_gb INTEGER NOT NULL,
    connection_url TEXT,
    maintenance_day VARCHAR(20) NOT NULL DEFAULT 'sunday',
    maintenance_hour INTEGER NOT NULL DEFAULT 3,
    backup_enabled BOOLEAN NOT NULL DEFAULT true,
    backup_retention_days INTEGER NOT NULL DEFAULT 7,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    deleted_at TIMESTAMP WITH TIME ZONE,
    UNIQUE(owner_id, name)
);

-- Cluster metrics table (for time-series metrics data)
CREATE TABLE IF NOT EXISTS cluster_metrics (
    id BIGSERIAL PRIMARY KEY,
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE,
    cpu_usage DOUBLE PRECISION NOT NULL DEFAULT 0,
    memory_usage DOUBLE PRECISION NOT NULL DEFAULT 0,
    storage_usage DOUBLE PRECISION NOT NULL DEFAULT 0,
    connection_count INTEGER NOT NULL DEFAULT 0,
    queries_per_sec DOUBLE PRECISION NOT NULL DEFAULT 0,
    reads_per_sec DOUBLE PRECISION NOT NULL DEFAULT 0,
    writes_per_sec DOUBLE PRECISION NOT NULL DEFAULT 0,
    replication_lag_ms BIGINT NOT NULL DEFAULT 0,
    timestamp TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Indexes
CREATE INDEX IF NOT EXISTS idx_clusters_owner_id ON clusters(owner_id);
CREATE INDEX IF NOT EXISTS idx_clusters_status ON clusters(status);
CREATE INDEX IF NOT EXISTS idx_clusters_deleted_at ON clusters(deleted_at);
CREATE INDEX IF NOT EXISTS idx_cluster_metrics_cluster_id ON cluster_metrics(cluster_id);
CREATE INDEX IF NOT EXISTS idx_cluster_metrics_timestamp ON cluster_metrics(timestamp);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);

-- Function to update updated_at timestamp
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

-- Triggers for updated_at
DROP TRIGGER IF EXISTS update_users_updated_at ON users;
CREATE TRIGGER update_users_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS update_clusters_updated_at ON clusters;
CREATE TRIGGER update_clusters_updated_at
    BEFORE UPDATE ON clusters
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();
