// Package db provides database access layer for the control plane.
package db

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/orochi-db/orochi-db/services/control-plane/pkg/config"
)

// DB wraps the PostgreSQL connection pool.
type DB struct {
	Pool *pgxpool.Pool
}

// New creates a new database connection pool.
func New(ctx context.Context, cfg *config.DatabaseConfig) (*DB, error) {
	poolConfig, err := pgxpool.ParseConfig(cfg.DSN())
	if err != nil {
		return nil, fmt.Errorf("failed to parse database config: %w", err)
	}

	poolConfig.MaxConns = cfg.MaxConns
	poolConfig.MinConns = cfg.MinConns
	poolConfig.MaxConnLifetime = cfg.MaxConnLifetime
	poolConfig.MaxConnIdleTime = cfg.MaxConnIdleTime

	// Configure connection settings
	poolConfig.ConnConfig.ConnectTimeout = 5 * time.Second

	pool, err := pgxpool.NewWithConfig(ctx, poolConfig)
	if err != nil {
		return nil, fmt.Errorf("failed to create connection pool: %w", err)
	}

	// Verify connection
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("failed to ping database: %w", err)
	}

	return &DB{Pool: pool}, nil
}

// Close closes the database connection pool.
func (db *DB) Close() {
	if db.Pool != nil {
		db.Pool.Close()
	}
}

// Health checks the database health.
func (db *DB) Health(ctx context.Context) error {
	return db.Pool.Ping(ctx)
}

// BeginTx begins a new transaction.
func (db *DB) BeginTx(ctx context.Context) (pgx.Tx, error) {
	return db.Pool.Begin(ctx)
}

// RunMigrations runs all database migrations.
func (db *DB) RunMigrations(ctx context.Context) error {
	migrations := []string{
		migrationCreateUsersTable,
		migrationCreateClustersTable,
		migrationCreateClusterMetricsTable,
		migrationCreateOrganizationsTable,
		migrationCreateOrgMembersTable,
		migrationCreateIndexes,
		migrationCreateOrgIndexes,
		migrationAddPoolerAndOrgToClusters,
		migrationCreateUpdateTriggerFunction,
		migrationCreateInvitesAndSettings,
		migrationAddTieringConfig,
		migrationAddAvatar,
	}

	for i, migration := range migrations {
		if _, err := db.Pool.Exec(ctx, migration); err != nil {
			return fmt.Errorf("failed to run migration %d: %w", i+1, err)
		}
	}

	return nil
}

const migrationCreateUsersTable = `
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
`

const migrationCreateClustersTable = `
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
`

const migrationCreateClusterMetricsTable = `
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
`

const migrationCreateIndexes = `
CREATE INDEX IF NOT EXISTS idx_clusters_owner_id ON clusters(owner_id);
CREATE INDEX IF NOT EXISTS idx_clusters_status ON clusters(status);
CREATE INDEX IF NOT EXISTS idx_clusters_deleted_at ON clusters(deleted_at);
CREATE INDEX IF NOT EXISTS idx_cluster_metrics_cluster_id ON cluster_metrics(cluster_id);
CREATE INDEX IF NOT EXISTS idx_cluster_metrics_timestamp ON cluster_metrics(timestamp);
CREATE INDEX IF NOT EXISTS idx_cluster_metrics_cluster_timestamp ON cluster_metrics(cluster_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
`

const migrationCreateOrganizationsTable = `
CREATE TABLE IF NOT EXISTS organizations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(64) NOT NULL,
    slug VARCHAR(64) NOT NULL UNIQUE,
    owner_id UUID NOT NULL REFERENCES users(id),
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    deleted_at TIMESTAMP WITH TIME ZONE
);
`

const migrationCreateOrgMembersTable = `
CREATE TABLE IF NOT EXISTS organization_members (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    organization_id UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role VARCHAR(20) NOT NULL DEFAULT 'member',
    invited_by UUID REFERENCES users(id),
    joined_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    UNIQUE(organization_id, user_id)
);
`

const migrationCreateOrgIndexes = `
CREATE INDEX IF NOT EXISTS idx_organizations_owner_id ON organizations(owner_id);
CREATE INDEX IF NOT EXISTS idx_organizations_slug ON organizations(slug);
CREATE INDEX IF NOT EXISTS idx_organizations_deleted_at ON organizations(deleted_at);
CREATE INDEX IF NOT EXISTS idx_org_members_org_id ON organization_members(organization_id);
CREATE INDEX IF NOT EXISTS idx_org_members_user_id ON organization_members(user_id);
`

const migrationAddPoolerAndOrgToClusters = `
-- Add organization_id column for team-based cluster isolation
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS organization_id UUID REFERENCES organizations(id) ON DELETE SET NULL;

-- Add pooler_url column for PgBouncer connection pooling
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS pooler_url TEXT;

-- Add pooler_enabled column to track if connection pooling is enabled
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS pooler_enabled BOOLEAN NOT NULL DEFAULT false;

-- Create index for organization_id lookups
CREATE INDEX IF NOT EXISTS idx_clusters_organization_id ON clusters(organization_id);
`

const migrationCreateUpdateTriggerFunction = `
-- Create the update_updated_at_column function for triggers
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';
`

const migrationCreateInvitesAndSettings = `
-- Migration: 003_add_invites_and_settings
-- Organization invites table for team onboarding
CREATE TABLE IF NOT EXISTS organization_invites (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    organization_id UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    email VARCHAR(255) NOT NULL,
    role VARCHAR(50) NOT NULL DEFAULT 'member',
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
    description TEXT,
    execution_time_ms INTEGER,
    rows_affected INTEGER,
    status VARCHAR(20) NOT NULL DEFAULT 'success',
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
    cluster_id UUID REFERENCES clusters(id) ON DELETE CASCADE,
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

-- Cluster settings for advanced configuration
CREATE TABLE IF NOT EXISTS cluster_settings (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE UNIQUE,
    pooler_mode VARCHAR(20) NOT NULL DEFAULT 'transaction',
    pooler_default_pool_size INTEGER NOT NULL DEFAULT 25,
    pooler_max_client_conn INTEGER NOT NULL DEFAULT 100,
    statement_timeout_ms INTEGER NOT NULL DEFAULT 30000,
    idle_in_transaction_timeout_ms INTEGER NOT NULL DEFAULT 60000,
    auto_vacuum_enabled BOOLEAN NOT NULL DEFAULT true,
    pg_stat_statements_enabled BOOLEAN NOT NULL DEFAULT true,
    slow_query_threshold_ms INTEGER NOT NULL DEFAULT 1000,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Add default organization for users during registration
ALTER TABLE users ADD COLUMN IF NOT EXISTS default_organization_id UUID REFERENCES organizations(id) ON DELETE SET NULL;
CREATE INDEX IF NOT EXISTS idx_users_default_org ON users(default_organization_id);

-- Performance advisor recommendations table
CREATE TABLE IF NOT EXISTS performance_recommendations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    cluster_id UUID NOT NULL REFERENCES clusters(id) ON DELETE CASCADE,
    type VARCHAR(50) NOT NULL,
    severity VARCHAR(20) NOT NULL DEFAULT 'info',
    title VARCHAR(255) NOT NULL,
    description TEXT NOT NULL,
    recommendation TEXT NOT NULL,
    metadata JSONB,
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
`

const migrationAddTieringConfig = `
-- Migration: 004_tiering - Add tiered storage support
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_enabled BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_hot_duration VARCHAR(20);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_warm_duration VARCHAR(20);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_cold_duration VARCHAR(20);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_compression VARCHAR(10);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_endpoint VARCHAR(255);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_bucket VARCHAR(255);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_region VARCHAR(100);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS enable_columnar BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS default_shard_count INTEGER NOT NULL DEFAULT 4;
CREATE INDEX IF NOT EXISTS idx_clusters_tiering_enabled ON clusters(tiering_enabled);
`

const migrationAddAvatar = `
-- Migration: 005_add_avatar - Add avatar column to users table
ALTER TABLE users ADD COLUMN IF NOT EXISTS avatar TEXT;
`
