package models

import (
	"errors"
	"time"
)

// PoolerMode represents the connection pooling mode.
type PoolerMode string

const (
	// PoolerModeTransaction reuses connections after transaction completion.
	// Best for most applications with short-lived transactions.
	PoolerModeTransaction PoolerMode = "transaction"

	// PoolerModeSession assigns a connection for the entire session.
	// Required for applications using session-level features (prepared statements, temp tables).
	PoolerModeSession PoolerMode = "session"

	// PoolerModeStatement reuses connections after each statement.
	// Most aggressive pooling, but breaks multi-statement transactions.
	PoolerModeStatement PoolerMode = "statement"
)

// PoolerConfig represents the configuration for a PgDog connection pooler.
type PoolerConfig struct {
	Enabled         bool       `json:"enabled"`
	Mode            PoolerMode `json:"mode"`                      // transaction, session, statement
	MaxPoolSize     int        `json:"max_pool_size"`             // Maximum connections per pool
	MinPoolSize     int        `json:"min_pool_size"`             // Minimum idle connections per pool
	IdleTimeout     int        `json:"idle_timeout_seconds"`      // Seconds before idle connection is closed
	ConnectTimeout  int        `json:"connect_timeout_ms"`        // Connection timeout in milliseconds
	QueryTimeout    int        `json:"query_timeout_ms"`          // Query timeout in milliseconds (0 = unlimited)
	ReadWriteSplit  bool       `json:"read_write_split"`          // Enable read/write splitting
	ShardingEnabled bool       `json:"sharding_enabled"`          // Enable query-based sharding
	ShardCount      int        `json:"shard_count,omitempty"`     // Number of shards (if sharding enabled)
	LoadBalancing   string     `json:"load_balancing,omitempty"`  // Load balancing strategy: round_robin, random, least_conn
	HealthCheckSec  int        `json:"health_check_interval_sec"` // Health check interval in seconds
}

// PoolerStats represents real-time statistics from a PgDog pooler.
type PoolerStats struct {
	ActiveConnections  int     `json:"active_connections"`   // Currently active connections
	IdleConnections    int     `json:"idle_connections"`     // Idle connections in pool
	WaitingClients     int     `json:"waiting_clients"`      // Clients waiting for a connection
	TotalQueries       int64   `json:"total_queries"`        // Total queries executed
	QueriesPerSecond   float64 `json:"queries_per_second"`   // Current QPS
	AverageLatencyMs   float64 `json:"avg_latency_ms"`       // Average query latency
	P99LatencyMs       float64 `json:"p99_latency_ms"`       // 99th percentile latency
	PoolUtilization    float64 `json:"pool_utilization"`     // Pool utilization percentage
	BytesSent          int64   `json:"bytes_sent"`           // Total bytes sent to clients
	BytesReceived      int64   `json:"bytes_received"`       // Total bytes received from clients
	TotalTransactions  int64   `json:"total_transactions"`   // Total transactions executed
	TransactionsPerSec float64 `json:"transactions_per_sec"` // Current TPS
	ErrorCount         int64   `json:"error_count"`          // Total error count
	ConnectionsCreated int64   `json:"connections_created"`  // Total connections created
	ConnectionsClosed  int64   `json:"connections_closed"`   // Total connections closed
}

// PoolerStatus represents the complete status of a PgDog pooler for a cluster.
type PoolerStatus struct {
	ClusterID     string       `json:"cluster_id"`
	Healthy       bool         `json:"healthy"`
	Config        PoolerConfig `json:"config"`
	Stats         PoolerStats  `json:"stats"`
	Replicas      int          `json:"replicas"`       // Desired replicas
	ReadyReplicas int          `json:"ready_replicas"` // Ready replicas
	Version       string       `json:"version"`        // PgDog version
	Endpoint      string       `json:"endpoint"`       // Pooler endpoint URL
	AdminEndpoint string       `json:"admin_endpoint"` // Admin endpoint for stats/management
	LastUpdated   time.Time    `json:"last_updated"`
}

// PoolerUpdateRequest represents a request to update pooler configuration.
type PoolerUpdateRequest struct {
	Enabled         *bool       `json:"enabled,omitempty"`
	Mode            *PoolerMode `json:"mode,omitempty"`
	MaxPoolSize     *int        `json:"max_pool_size,omitempty"`
	MinPoolSize     *int        `json:"min_pool_size,omitempty"`
	IdleTimeout     *int        `json:"idle_timeout_seconds,omitempty"`
	ConnectTimeout  *int        `json:"connect_timeout_ms,omitempty"`
	QueryTimeout    *int        `json:"query_timeout_ms,omitempty"`
	ReadWriteSplit  *bool       `json:"read_write_split,omitempty"`
	ShardingEnabled *bool       `json:"sharding_enabled,omitempty"`
	ShardCount      *int        `json:"shard_count,omitempty"`
	LoadBalancing   *string     `json:"load_balancing,omitempty"`
	HealthCheckSec  *int        `json:"health_check_interval_sec,omitempty"`
}

// PoolInfo represents information about a single connection pool.
type PoolInfo struct {
	Database          string `json:"database"`
	User              string `json:"user"`
	ActiveConnections int    `json:"active_connections"`
	IdleConnections   int    `json:"idle_connections"`
	WaitingClients    int    `json:"waiting_clients"`
	MaxConnections    int    `json:"max_connections"`
}

// ClientInfo represents information about a connected client.
type ClientInfo struct {
	ID           string    `json:"id"`
	Database     string    `json:"database"`
	User         string    `json:"user"`
	Address      string    `json:"address"`
	State        string    `json:"state"` // active, idle, waiting
	ConnectedAt  time.Time `json:"connected_at"`
	LastActivity time.Time `json:"last_activity"`
	QueryCount   int64     `json:"query_count"`
}

// Pooler-related errors.
var (
	ErrPoolerNotEnabled        = errors.New("connection pooler is not enabled for this cluster")
	ErrPoolerNotReady          = errors.New("connection pooler is not ready")
	ErrPoolerConnectionFailed  = errors.New("failed to connect to pooler admin interface")
	ErrPoolerReloadFailed      = errors.New("failed to reload pooler configuration")
	ErrInvalidPoolerMode       = errors.New("invalid pooler mode: must be transaction, session, or statement")
	ErrInvalidPoolSize         = errors.New("pool size must be greater than 0")
	ErrMinPoolGreaterThanMax   = errors.New("minimum pool size cannot be greater than maximum pool size")
	ErrInvalidTimeout          = errors.New("timeout value must be non-negative")
	ErrPoolerInvalidShardCount = errors.New("shard count must be greater than 0 when sharding is enabled")
	ErrInvalidLoadBalancing    = errors.New("invalid load balancing strategy: must be round_robin, random, or least_conn")
)

// Validate validates the PoolerConfig.
func (c *PoolerConfig) Validate() error {
	if c.Mode != "" && c.Mode != PoolerModeTransaction && c.Mode != PoolerModeSession && c.Mode != PoolerModeStatement {
		return ErrInvalidPoolerMode
	}
	if c.MaxPoolSize < 0 {
		return ErrInvalidPoolSize
	}
	if c.MinPoolSize < 0 {
		return ErrInvalidPoolSize
	}
	if c.MinPoolSize > c.MaxPoolSize && c.MaxPoolSize > 0 {
		return ErrMinPoolGreaterThanMax
	}
	if c.IdleTimeout < 0 || c.ConnectTimeout < 0 || c.QueryTimeout < 0 {
		return ErrInvalidTimeout
	}
	if c.ShardingEnabled && c.ShardCount <= 0 {
		return ErrPoolerInvalidShardCount
	}
	if c.LoadBalancing != "" {
		validLB := map[string]bool{"round_robin": true, "random": true, "least_conn": true}
		if !validLB[c.LoadBalancing] {
			return ErrInvalidLoadBalancing
		}
	}
	return nil
}

// Validate validates the PoolerUpdateRequest.
func (r *PoolerUpdateRequest) Validate() error {
	if r.Mode != nil {
		if *r.Mode != PoolerModeTransaction && *r.Mode != PoolerModeSession && *r.Mode != PoolerModeStatement {
			return ErrInvalidPoolerMode
		}
	}
	if r.MaxPoolSize != nil && *r.MaxPoolSize < 0 {
		return ErrInvalidPoolSize
	}
	if r.MinPoolSize != nil && *r.MinPoolSize < 0 {
		return ErrInvalidPoolSize
	}
	if r.MinPoolSize != nil && r.MaxPoolSize != nil && *r.MinPoolSize > *r.MaxPoolSize {
		return ErrMinPoolGreaterThanMax
	}
	if r.IdleTimeout != nil && *r.IdleTimeout < 0 {
		return ErrInvalidTimeout
	}
	if r.ConnectTimeout != nil && *r.ConnectTimeout < 0 {
		return ErrInvalidTimeout
	}
	if r.QueryTimeout != nil && *r.QueryTimeout < 0 {
		return ErrInvalidTimeout
	}
	if r.ShardingEnabled != nil && *r.ShardingEnabled {
		if r.ShardCount != nil && *r.ShardCount <= 0 {
			return ErrPoolerInvalidShardCount
		}
	}
	if r.LoadBalancing != nil {
		validLB := map[string]bool{"round_robin": true, "random": true, "least_conn": true}
		if !validLB[*r.LoadBalancing] {
			return ErrInvalidLoadBalancing
		}
	}
	return nil
}

// ApplyDefaults applies default values to the PoolerConfig.
func (c *PoolerConfig) ApplyDefaults() {
	if c.Mode == "" {
		c.Mode = PoolerModeTransaction
	}
	if c.MaxPoolSize == 0 {
		c.MaxPoolSize = 100
	}
	if c.MinPoolSize == 0 {
		c.MinPoolSize = 10
	}
	if c.IdleTimeout == 0 {
		c.IdleTimeout = 300 // 5 minutes
	}
	if c.ConnectTimeout == 0 {
		c.ConnectTimeout = 5000 // 5 seconds
	}
	if c.LoadBalancing == "" {
		c.LoadBalancing = "round_robin"
	}
	if c.HealthCheckSec == 0 {
		c.HealthCheckSec = 30
	}
}
