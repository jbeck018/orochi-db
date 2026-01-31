// Package clients provides external service clients for the control plane.
package clients

import (
	"bufio"
	"context"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// PgDogAdminClient provides access to PgDog's admin interface.
// PgDog exposes a PostgreSQL-compatible admin interface on port 6432
// that accepts commands like SHOW POOLS, SHOW STATS, SHOW CLIENTS, and RELOAD.
type PgDogAdminClient struct {
	host          string
	port          int
	user          string
	password      string
	database      string
	connectTimeout time.Duration
	readTimeout    time.Duration
	logger        *slog.Logger
	mu            sync.Mutex
}

// PgDogAdminConfig holds configuration for the PgDog admin client.
type PgDogAdminConfig struct {
	Host           string
	Port           int           // Default: 6432
	User           string        // Admin user
	Password       string        // Admin password
	Database       string        // Admin database (usually "pgdog")
	ConnectTimeout time.Duration // Connection timeout
	ReadTimeout    time.Duration // Read timeout for commands
}

// NewPgDogAdminClient creates a new PgDog admin client.
func NewPgDogAdminClient(cfg *PgDogAdminConfig, logger *slog.Logger) *PgDogAdminClient {
	if cfg.Port == 0 {
		cfg.Port = 6432
	}
	if cfg.Database == "" {
		cfg.Database = "pgdog"
	}
	if cfg.ConnectTimeout == 0 {
		cfg.ConnectTimeout = 5 * time.Second
	}
	if cfg.ReadTimeout == 0 {
		cfg.ReadTimeout = 10 * time.Second
	}

	return &PgDogAdminClient{
		host:           cfg.Host,
		port:           cfg.Port,
		user:           cfg.User,
		password:       cfg.Password,
		database:       cfg.Database,
		connectTimeout: cfg.ConnectTimeout,
		readTimeout:    cfg.ReadTimeout,
		logger:         logger.With("client", "pgdog_admin"),
	}
}

// connect establishes a connection to the PgDog admin interface.
func (c *PgDogAdminClient) connect(ctx context.Context) (net.Conn, error) {
	address := fmt.Sprintf("%s:%d", c.host, c.port)

	dialer := &net.Dialer{
		Timeout: c.connectTimeout,
	}

	conn, err := dialer.DialContext(ctx, "tcp", address)
	if err != nil {
		return nil, fmt.Errorf("failed to connect to PgDog admin at %s: %w", address, err)
	}

	return conn, nil
}

// executeCommand sends a command to PgDog and returns the response lines.
func (c *PgDogAdminClient) executeCommand(ctx context.Context, command string) ([][]string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := c.connect(ctx)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	// Set read deadline
	if err := conn.SetDeadline(time.Now().Add(c.readTimeout)); err != nil {
		return nil, fmt.Errorf("failed to set deadline: %w", err)
	}

	// Send command with newline
	if _, err := fmt.Fprintf(conn, "%s;\n", command); err != nil {
		return nil, fmt.Errorf("failed to send command: %w", err)
	}

	// Read response
	var rows [][]string
	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		line := scanner.Text()
		// Skip empty lines and separator lines
		if line == "" || strings.HasPrefix(line, "---") || strings.HasPrefix(line, "===") {
			continue
		}
		// End of response
		if strings.HasPrefix(line, "SHOW") || strings.HasPrefix(line, "RELOAD") {
			break
		}
		// Parse tab or pipe-separated values
		var fields []string
		if strings.Contains(line, "|") {
			fields = strings.Split(line, "|")
		} else {
			fields = strings.Split(line, "\t")
		}
		// Trim whitespace from each field
		for i := range fields {
			fields[i] = strings.TrimSpace(fields[i])
		}
		if len(fields) > 0 && fields[0] != "" {
			rows = append(rows, fields)
		}
	}

	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("failed to read response: %w", err)
	}

	return rows, nil
}

// ShowPools retrieves pool information from PgDog.
// Returns information about all connection pools.
func (c *PgDogAdminClient) ShowPools(ctx context.Context) ([]models.PoolInfo, error) {
	c.logger.Debug("executing SHOW POOLS")

	rows, err := c.executeCommand(ctx, "SHOW POOLS")
	if err != nil {
		return nil, fmt.Errorf("failed to execute SHOW POOLS: %w", err)
	}

	if len(rows) < 2 {
		// No data or only header
		return []models.PoolInfo{}, nil
	}

	// First row is header
	header := rows[0]
	headerMap := make(map[string]int)
	for i, col := range header {
		headerMap[strings.ToLower(col)] = i
	}

	var pools []models.PoolInfo
	for _, row := range rows[1:] {
		if len(row) < len(header) {
			continue
		}

		pool := models.PoolInfo{
			Database:          getField(row, headerMap, "database"),
			User:              getField(row, headerMap, "user"),
			ActiveConnections: getIntField(row, headerMap, "cl_active"),
			IdleConnections:   getIntField(row, headerMap, "sv_idle"),
			WaitingClients:    getIntField(row, headerMap, "cl_waiting"),
			MaxConnections:    getIntField(row, headerMap, "maxwait"),
		}
		pools = append(pools, pool)
	}

	return pools, nil
}

// ShowStats retrieves statistics from PgDog.
// Returns aggregated statistics for all pools.
func (c *PgDogAdminClient) ShowStats(ctx context.Context) (*models.PoolerStats, error) {
	c.logger.Debug("executing SHOW STATS")

	rows, err := c.executeCommand(ctx, "SHOW STATS")
	if err != nil {
		return nil, fmt.Errorf("failed to execute SHOW STATS: %w", err)
	}

	stats := &models.PoolerStats{}

	if len(rows) < 2 {
		return stats, nil
	}

	// First row is header
	header := rows[0]
	headerMap := make(map[string]int)
	for i, col := range header {
		headerMap[strings.ToLower(col)] = i
	}

	// Aggregate stats from all rows
	for _, row := range rows[1:] {
		if len(row) < len(header) {
			continue
		}

		// Sum up statistics from all databases
		stats.TotalQueries += getInt64Field(row, headerMap, "total_query_count")
		stats.TotalTransactions += getInt64Field(row, headerMap, "total_xact_count")
		stats.BytesSent += getInt64Field(row, headerMap, "total_sent")
		stats.BytesReceived += getInt64Field(row, headerMap, "total_received")

		// Use latest per-second rates
		if qps := getFloat64Field(row, headerMap, "avg_query_count"); qps > 0 {
			stats.QueriesPerSecond = qps
		}
		if tps := getFloat64Field(row, headerMap, "avg_xact_count"); tps > 0 {
			stats.TransactionsPerSec = tps
		}

		// Latency (in microseconds, convert to milliseconds)
		avgTime := getFloat64Field(row, headerMap, "avg_query_time")
		if avgTime > 0 {
			stats.AverageLatencyMs = avgTime / 1000.0
		}
	}

	// Get pool counts from SHOW POOLS
	pools, err := c.ShowPools(ctx)
	if err == nil {
		for _, pool := range pools {
			stats.ActiveConnections += pool.ActiveConnections
			stats.IdleConnections += pool.IdleConnections
			stats.WaitingClients += pool.WaitingClients
		}
	}

	// Calculate pool utilization
	totalConns := stats.ActiveConnections + stats.IdleConnections
	if totalConns > 0 {
		stats.PoolUtilization = float64(stats.ActiveConnections) / float64(totalConns) * 100
	}

	return stats, nil
}

// ShowClients retrieves connected client information from PgDog.
func (c *PgDogAdminClient) ShowClients(ctx context.Context) ([]models.ClientInfo, error) {
	c.logger.Debug("executing SHOW CLIENTS")

	rows, err := c.executeCommand(ctx, "SHOW CLIENTS")
	if err != nil {
		return nil, fmt.Errorf("failed to execute SHOW CLIENTS: %w", err)
	}

	if len(rows) < 2 {
		return []models.ClientInfo{}, nil
	}

	header := rows[0]
	headerMap := make(map[string]int)
	for i, col := range header {
		headerMap[strings.ToLower(col)] = i
	}

	var clients []models.ClientInfo
	for _, row := range rows[1:] {
		if len(row) < len(header) {
			continue
		}

		client := models.ClientInfo{
			ID:         getField(row, headerMap, "ptr"),
			Database:   getField(row, headerMap, "database"),
			User:       getField(row, headerMap, "user"),
			Address:    getField(row, headerMap, "addr"),
			State:      getField(row, headerMap, "state"),
			QueryCount: getInt64Field(row, headerMap, "query_count"),
		}

		// Parse connect_time if available
		if connectTime := getField(row, headerMap, "connect_time"); connectTime != "" {
			if t, err := time.Parse("2006-01-02 15:04:05", connectTime); err == nil {
				client.ConnectedAt = t
			}
		}

		clients = append(clients, client)
	}

	return clients, nil
}

// ShowConfig retrieves current configuration from PgDog.
func (c *PgDogAdminClient) ShowConfig(ctx context.Context) (*models.PoolerConfig, error) {
	c.logger.Debug("executing SHOW CONFIG")

	rows, err := c.executeCommand(ctx, "SHOW CONFIG")
	if err != nil {
		return nil, fmt.Errorf("failed to execute SHOW CONFIG: %w", err)
	}

	config := &models.PoolerConfig{
		Enabled: true,
	}

	// Parse config key-value pairs
	for _, row := range rows {
		if len(row) < 2 {
			continue
		}

		key := strings.ToLower(row[0])
		value := row[1]

		switch key {
		case "pool_mode":
			switch value {
			case "transaction":
				config.Mode = models.PoolerModeTransaction
			case "session":
				config.Mode = models.PoolerModeSession
			case "statement":
				config.Mode = models.PoolerModeStatement
			}
		case "default_pool_size", "max_client_conn":
			if v, err := strconv.Atoi(value); err == nil {
				config.MaxPoolSize = v
			}
		case "min_pool_size":
			if v, err := strconv.Atoi(value); err == nil {
				config.MinPoolSize = v
			}
		case "server_idle_timeout", "idle_timeout":
			if v, err := strconv.Atoi(value); err == nil {
				config.IdleTimeout = v
			}
		case "connect_timeout", "server_connect_timeout":
			if v, err := strconv.Atoi(value); err == nil {
				config.ConnectTimeout = v * 1000 // Convert to ms
			}
		case "query_timeout":
			if v, err := strconv.Atoi(value); err == nil {
				config.QueryTimeout = v * 1000 // Convert to ms
			}
		case "load_balance_hosts", "load_balancing_mode":
			config.LoadBalancing = value
		}
	}

	return config, nil
}

// Reload sends a RELOAD command to PgDog to reload its configuration.
func (c *PgDogAdminClient) Reload(ctx context.Context) error {
	c.logger.Info("executing RELOAD")

	_, err := c.executeCommand(ctx, "RELOAD")
	if err != nil {
		return fmt.Errorf("failed to execute RELOAD: %w", err)
	}

	c.logger.Info("PgDog configuration reloaded successfully")
	return nil
}

// Ping checks if the PgDog admin interface is accessible.
func (c *PgDogAdminClient) Ping(ctx context.Context) error {
	conn, err := c.connect(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	return nil
}

// GetVersion retrieves the PgDog version.
func (c *PgDogAdminClient) GetVersion(ctx context.Context) (string, error) {
	c.logger.Debug("executing SHOW VERSION")

	rows, err := c.executeCommand(ctx, "SHOW VERSION")
	if err != nil {
		return "", fmt.Errorf("failed to execute SHOW VERSION: %w", err)
	}

	if len(rows) > 0 && len(rows[0]) > 0 {
		return rows[0][0], nil
	}

	return "unknown", nil
}

// Helper functions for parsing response fields

func getField(row []string, headerMap map[string]int, key string) string {
	if idx, ok := headerMap[key]; ok && idx < len(row) {
		return row[idx]
	}
	return ""
}

func getIntField(row []string, headerMap map[string]int, key string) int {
	if s := getField(row, headerMap, key); s != "" {
		if v, err := strconv.Atoi(s); err == nil {
			return v
		}
	}
	return 0
}

func getInt64Field(row []string, headerMap map[string]int, key string) int64 {
	if s := getField(row, headerMap, key); s != "" {
		if v, err := strconv.ParseInt(s, 10, 64); err == nil {
			return v
		}
	}
	return 0
}

func getFloat64Field(row []string, headerMap map[string]int, key string) float64 {
	if s := getField(row, headerMap, key); s != "" {
		if v, err := strconv.ParseFloat(s, 64); err == nil {
			return v
		}
	}
	return 0
}

// PgDogAdminClientFactory creates PgDog admin clients for clusters.
type PgDogAdminClientFactory struct {
	logger *slog.Logger
}

// NewPgDogAdminClientFactory creates a new factory for PgDog admin clients.
func NewPgDogAdminClientFactory(logger *slog.Logger) *PgDogAdminClientFactory {
	return &PgDogAdminClientFactory{
		logger: logger.With("factory", "pgdog_admin"),
	}
}

// CreateForCluster creates a PgDog admin client for a specific cluster.
func (f *PgDogAdminClientFactory) CreateForCluster(clusterID string, poolerEndpoint string) *PgDogAdminClient {
	// Parse endpoint to extract host
	// Expected format: host:port or just host (default port 6432)
	host := poolerEndpoint
	port := 6432

	if colonIdx := strings.LastIndex(poolerEndpoint, ":"); colonIdx > 0 {
		host = poolerEndpoint[:colonIdx]
		if p, err := strconv.Atoi(poolerEndpoint[colonIdx+1:]); err == nil {
			port = p
		}
	}

	cfg := &PgDogAdminConfig{
		Host:     host,
		Port:     port,
		User:     "pgdog_admin",
		Password: "", // Retrieved from secrets in production
		Database: "pgdog",
	}

	return NewPgDogAdminClient(cfg, f.logger.With("cluster_id", clusterID))
}
