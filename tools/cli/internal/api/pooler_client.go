// Package api provides the HTTP client for the Orochi Cloud API.
package api

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/url"
	"time"
)

// PoolerMode represents the pooler connection mode.
type PoolerMode string

const (
	PoolerModeSession     PoolerMode = "session"
	PoolerModeTransaction PoolerMode = "transaction"
	PoolerModeStatement   PoolerMode = "statement"
)

// PoolerStatus represents the overall status of a pooler.
type PoolerStatus struct {
	ClusterID       string          `json:"cluster_id"`
	ClusterName     string          `json:"cluster_name"`
	Healthy         bool            `json:"healthy"`
	Status          string          `json:"status"`
	Version         string          `json:"version"`
	Uptime          time.Duration   `json:"uptime"`
	UptimeSeconds   int64           `json:"uptime_seconds"`
	Config          *PoolerConfig   `json:"config"`
	Stats           *PoolerStats    `json:"stats"`
	Endpoints       *PoolerEndpoint `json:"endpoints,omitempty"`
	LastHealthCheck time.Time       `json:"last_health_check"`
}

// PoolerEndpoint contains pooler connection endpoints.
type PoolerEndpoint struct {
	ReadWrite string `json:"read_write"`
	ReadOnly  string `json:"read_only,omitempty"`
	Admin     string `json:"admin,omitempty"`
}

// PoolerConfig represents the pooler configuration.
type PoolerConfig struct {
	Mode            PoolerMode `json:"mode"`
	MaxPoolSize     int        `json:"max_pool_size"`
	MinPoolSize     int        `json:"min_pool_size"`
	IdleTimeout     int        `json:"idle_timeout"`
	ConnectionLimit int        `json:"connection_limit"`
	ReadWriteSplit  bool       `json:"read_write_split"`
	ShardingEnabled bool       `json:"sharding_enabled"`
	ShardCount      int        `json:"shard_count,omitempty"`
	LoadBalancing   string     `json:"load_balancing"`
	QueryTimeout    int        `json:"query_timeout"`
	BanTime         int        `json:"ban_time"`
}

// PoolerConfigUpdate represents pooler configuration updates.
type PoolerConfigUpdate struct {
	Mode            *PoolerMode `json:"mode,omitempty"`
	MaxPoolSize     *int        `json:"max_pool_size,omitempty"`
	MinPoolSize     *int        `json:"min_pool_size,omitempty"`
	IdleTimeout     *int        `json:"idle_timeout,omitempty"`
	ConnectionLimit *int        `json:"connection_limit,omitempty"`
	ReadWriteSplit  *bool       `json:"read_write_split,omitempty"`
	ShardingEnabled *bool       `json:"sharding_enabled,omitempty"`
	ShardCount      *int        `json:"shard_count,omitempty"`
	LoadBalancing   *string     `json:"load_balancing,omitempty"`
	QueryTimeout    *int        `json:"query_timeout,omitempty"`
	BanTime         *int        `json:"ban_time,omitempty"`
}

// PoolerStats represents pooler statistics.
type PoolerStats struct {
	// Connection stats
	ActiveConnections   int `json:"active_connections"`
	IdleConnections     int `json:"idle_connections"`
	TotalConnections    int `json:"total_connections"`
	WaitingClients      int `json:"waiting_clients"`
	MaxConnectionsUsed  int `json:"max_connections_used"`
	ConnectionsCreated  int `json:"connections_created"`
	ConnectionsClosed   int `json:"connections_closed"`
	ConnectionsRecycled int `json:"connections_recycled"`

	// Query stats
	TotalQueries     int64   `json:"total_queries"`
	QueriesPerSecond float64 `json:"queries_per_second"`
	ReadQueries      int64   `json:"read_queries"`
	WriteQueries     int64   `json:"write_queries"`

	// Latency stats (in milliseconds)
	AvgLatencyMs   float64 `json:"avg_latency_ms"`
	P50LatencyMs   float64 `json:"p50_latency_ms"`
	P95LatencyMs   float64 `json:"p95_latency_ms"`
	P99LatencyMs   float64 `json:"p99_latency_ms"`
	MaxLatencyMs   float64 `json:"max_latency_ms"`
	TotalWaitMs    float64 `json:"total_wait_ms"`
	AvgWaitMs      float64 `json:"avg_wait_ms"`

	// Traffic stats (in bytes)
	BytesSent     int64 `json:"bytes_sent"`
	BytesReceived int64 `json:"bytes_received"`

	// Error stats
	TotalErrors       int64   `json:"total_errors"`
	ConnectionErrors  int64   `json:"connection_errors"`
	QueryErrors       int64   `json:"query_errors"`
	TimeoutErrors     int64   `json:"timeout_errors"`
	ErrorRate         float64 `json:"error_rate"`
	BannedConnections int     `json:"banned_connections"`

	// Transaction stats
	TransactionsTotal    int64 `json:"transactions_total"`
	TransactionsCommit   int64 `json:"transactions_commit"`
	TransactionsRollback int64 `json:"transactions_rollback"`

	// Pool utilization
	PoolUtilization float64 `json:"pool_utilization"`

	// Timestamp
	Timestamp time.Time `json:"timestamp"`
}

// PoolerClient represents a connected client to the pooler.
type PoolerClient struct {
	ID              string    `json:"id"`
	Address         string    `json:"address"`
	Port            int       `json:"port"`
	Database        string    `json:"database"`
	User            string    `json:"user"`
	State           string    `json:"state"`
	ApplicationName string    `json:"application_name,omitempty"`
	ConnectedAt     time.Time `json:"connected_at"`
	LastActivity    time.Time `json:"last_activity"`
	TotalQueries    int64     `json:"total_queries"`
	QueryDuration   float64   `json:"query_duration_ms,omitempty"`
	CurrentQuery    string    `json:"current_query,omitempty"`
	WaitTime        float64   `json:"wait_time_ms,omitempty"`
}

// PoolerClientFilter filters for listing pooler clients.
type PoolerClientFilter struct {
	Database string `json:"database,omitempty"`
	User     string `json:"user,omitempty"`
	State    string `json:"state,omitempty"`
}

// GetPoolerStatus returns the pooler status for a cluster.
func (c *Client) GetPoolerStatus(ctx context.Context, clusterID string) (*PoolerStatus, error) {
	escapedID := url.PathEscape(clusterID)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedID+"/pooler/status", nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[PoolerStatus](resp)
}

// GetPoolerConfig returns the pooler configuration for a cluster.
func (c *Client) GetPoolerConfig(ctx context.Context, clusterID string) (*PoolerConfig, error) {
	escapedID := url.PathEscape(clusterID)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedID+"/pooler/config", nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[PoolerConfig](resp)
}

// UpdatePoolerConfig updates the pooler configuration for a cluster.
func (c *Client) UpdatePoolerConfig(ctx context.Context, clusterID string, config *PoolerConfigUpdate) (*PoolerConfig, error) {
	escapedID := url.PathEscape(clusterID)
	resp, err := c.doRequestWithContext(ctx, "PATCH", "/v1/clusters/"+escapedID+"/pooler/config", config)
	if err != nil {
		return nil, err
	}

	return parseResponse[PoolerConfig](resp)
}

// ReloadPoolerConfig reloads the pooler configuration without restart.
func (c *Client) ReloadPoolerConfig(ctx context.Context, clusterID string) error {
	escapedID := url.PathEscape(clusterID)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedID+"/pooler/reload", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		// Parse error response
		var apiErr APIError
		body, readErr := io.ReadAll(io.LimitReader(resp.Body, 10*1024*1024))
		if readErr != nil {
			return fmt.Errorf("failed to read error response: %w", readErr)
		}
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{
				StatusCode: resp.StatusCode,
				Code:       "unknown_error",
				Message:    string(body),
			}
		}
		apiErr.StatusCode = resp.StatusCode
		return &apiErr
	}

	return nil
}

// GetPoolerStats returns detailed pooler statistics for a cluster.
func (c *Client) GetPoolerStats(ctx context.Context, clusterID string) (*PoolerStats, error) {
	escapedID := url.PathEscape(clusterID)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedID+"/pooler/stats", nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[PoolerStats](resp)
}

// GetPoolerClients returns the list of connected clients to the pooler.
func (c *Client) GetPoolerClients(ctx context.Context, clusterID string, filter *PoolerClientFilter) ([]PoolerClient, error) {
	escapedID := url.PathEscape(clusterID)
	path := "/v1/clusters/" + escapedID + "/pooler/clients"

	// Add query parameters if filter is provided
	if filter != nil {
		params := url.Values{}
		if filter.Database != "" {
			params.Set("database", filter.Database)
		}
		if filter.User != "" {
			params.Set("user", filter.User)
		}
		if filter.State != "" {
			params.Set("state", filter.State)
		}
		if len(params) > 0 {
			path += "?" + params.Encode()
		}
	}

	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Clients []PoolerClient `json:"clients"`
	}

	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}

	return result.Clients, nil
}


// ValidatePoolerMode validates a pooler mode string.
func ValidatePoolerMode(mode string) (PoolerMode, error) {
	switch PoolerMode(mode) {
	case PoolerModeSession, PoolerModeTransaction, PoolerModeStatement:
		return PoolerMode(mode), nil
	default:
		return "", fmt.Errorf("invalid pooler mode: %s (must be session, transaction, or statement)", mode)
	}
}

// ValidateLoadBalancing validates a load balancing strategy.
func ValidateLoadBalancing(strategy string) error {
	validStrategies := map[string]bool{
		"round_robin":           true,
		"random":                true,
		"least_connections":     true,
		"least_outstanding":     true,
		"query_parser_weighted": true,
	}
	if !validStrategies[strategy] {
		return fmt.Errorf("invalid load balancing strategy: %s (must be round_robin, random, least_connections, least_outstanding, or query_parser_weighted)", strategy)
	}
	return nil
}
