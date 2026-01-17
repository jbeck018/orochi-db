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
		migrationCreateIndexes,
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
