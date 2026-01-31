// Package api provides the HTTP client for the HowlerOps API (OrochiDB).
package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"net/http"
	"net/url"
	"time"

	"github.com/orochi-db/orochi-db/tools/cli/internal/config"
)

const (
	// maxResponseBodySize limits response body to 10MB to prevent memory exhaustion.
	maxResponseBodySize = 10 * 1024 * 1024

	// maxRetries is the maximum number of retry attempts for transient failures.
	maxRetries = 3

	// baseRetryDelay is the initial delay for exponential backoff.
	baseRetryDelay = 500 * time.Millisecond

	// maxRetryDelay caps the maximum delay between retries.
	maxRetryDelay = 10 * time.Second
)

// Client is the HowlerOps API client.
type Client struct {
	baseURL    string
	httpClient *http.Client
	token      string
}

// ClusterSize represents the size of a cluster.
type ClusterSize string

const (
	ClusterSizeSmall  ClusterSize = "small"
	ClusterSizeMedium ClusterSize = "medium"
	ClusterSizeLarge  ClusterSize = "large"
	ClusterSizeXLarge ClusterSize = "xlarge"
)

// ClusterStatus represents the status of a cluster.
type ClusterStatus string

const (
	ClusterStatusCreating    ClusterStatus = "creating"
	ClusterStatusRunning     ClusterStatus = "running"
	ClusterStatusStopped     ClusterStatus = "stopped"
	ClusterStatusDeleting    ClusterStatus = "deleting"
	ClusterStatusScaling     ClusterStatus = "scaling"
	ClusterStatusMaintenance ClusterStatus = "maintenance"
	ClusterStatusError       ClusterStatus = "error"
)

// Cluster represents an OrochiDB cluster.
type Cluster struct {
	ID             string        `json:"id"`
	Name           string        `json:"name"`
	Size           ClusterSize   `json:"size"`
	Region         string        `json:"region"`
	Replicas       int           `json:"replicas"`
	Status         ClusterStatus `json:"status"`
	Version        string        `json:"version"`
	CreatedAt      time.Time     `json:"created_at"`
	UpdatedAt      time.Time     `json:"updated_at"`
	Host           string        `json:"host,omitempty"`
	Port           int           `json:"port,omitempty"`
	ConnectionURI  string        `json:"connection_uri,omitempty"`
	StorageGB      int           `json:"storage_gb"`
	VCPU           int           `json:"vcpu"`
	MemoryGB       int           `json:"memory_gb"`
}

// User represents a HowlerOps user.
type User struct {
	ID        string    `json:"id"`
	Email     string    `json:"email"`
	Name      string    `json:"name"`
	CreatedAt time.Time `json:"created_at"`
}

// LoginRequest represents a login request.
type LoginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

// LoginResponse represents a login response.
type LoginResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	User         User   `json:"user"`
}

// CreateClusterRequest represents a cluster creation request.
type CreateClusterRequest struct {
	Name     string      `json:"name"`
	Size     ClusterSize `json:"size"`
	Region   string      `json:"region"`
	Replicas int         `json:"replicas"`
}

// ScaleClusterRequest represents a cluster scaling request.
type ScaleClusterRequest struct {
	Replicas int `json:"replicas"`
}

// APIError represents an error response from the API.
type APIError struct {
	StatusCode int    `json:"-"`
	Code       string `json:"code"`
	Message    string `json:"message"`
	Details    string `json:"details,omitempty"`
}

func (e *APIError) Error() string {
	if e.Details != "" {
		return fmt.Sprintf("%s: %s (%s)", e.Code, e.Message, e.Details)
	}
	return fmt.Sprintf("%s: %s", e.Code, e.Message)
}

// newHTTPTransport creates an HTTP transport with proper connection pooling settings.
func newHTTPTransport() *http.Transport {
	return &http.Transport{
		Proxy: http.ProxyFromEnvironment,
		DialContext: (&net.Dialer{
			Timeout:   30 * time.Second,
			KeepAlive: 30 * time.Second,
		}).DialContext,
		ForceAttemptHTTP2:     true,
		MaxIdleConns:          100,
		MaxIdleConnsPerHost:   10,
		MaxConnsPerHost:       100,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
		ResponseHeaderTimeout: 30 * time.Second,
	}
}

// NewClient creates a new API client.
func NewClient() *Client {
	return &Client{
		baseURL: config.GetAPIEndpoint(),
		httpClient: &http.Client{
			Timeout:   30 * time.Second,
			Transport: newHTTPTransport(),
		},
		token: config.GetAccessToken(),
	}
}

// NewClientWithToken creates a new API client with a specific token.
func NewClientWithToken(token string) *Client {
	return &Client{
		baseURL: config.GetAPIEndpoint(),
		httpClient: &http.Client{
			Timeout:   30 * time.Second,
			Transport: newHTTPTransport(),
		},
		token: token,
	}
}

// isRetryableError determines if an error is transient and should be retried.
func isRetryableError(err error) bool {
	if err == nil {
		return false
	}

	// Check for network-related errors
	var netErr net.Error
	if errors.As(err, &netErr) {
		return netErr.Timeout() || netErr.Temporary()
	}

	// Check for connection refused, reset, etc.
	var opErr *net.OpError
	if errors.As(err, &opErr) {
		return true
	}

	return false
}

// isRetryableStatus determines if an HTTP status code is retryable.
func isRetryableStatus(statusCode int) bool {
	switch statusCode {
	case http.StatusTooManyRequests,
		http.StatusInternalServerError,
		http.StatusBadGateway,
		http.StatusServiceUnavailable,
		http.StatusGatewayTimeout:
		return true
	default:
		return false
	}
}

// calculateBackoff returns the delay for a retry attempt with jitter.
func calculateBackoff(attempt int) time.Duration {
	// Exponential backoff: baseDelay * 2^attempt
	delay := baseRetryDelay * time.Duration(1<<uint(attempt))
	if delay > maxRetryDelay {
		delay = maxRetryDelay
	}

	// Add jitter (0-25% of delay)
	jitter := time.Duration(rand.Int63n(int64(delay / 4)))
	return delay + jitter
}

// doRequest performs an HTTP request with retry logic.
func (c *Client) doRequest(method, path string, body interface{}) (*http.Response, error) {
	return c.doRequestWithContext(context.Background(), method, path, body)
}

// doRequestWithContext performs an HTTP request with context support and retry logic.
func (c *Client) doRequestWithContext(ctx context.Context, method, path string, body interface{}) (*http.Response, error) {
	var bodyBytes []byte
	if body != nil {
		var err error
		bodyBytes, err = json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("failed to marshal request body: %w", err)
		}
	}

	var lastErr error
	for attempt := 0; attempt <= maxRetries; attempt++ {
		// Check if context is already cancelled
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("request cancelled: %w", err)
		}

		// Create a new reader for each attempt (body may have been consumed)
		var bodyReader io.Reader
		if bodyBytes != nil {
			bodyReader = bytes.NewReader(bodyBytes)
		}

		req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, bodyReader)
		if err != nil {
			return nil, fmt.Errorf("failed to create request: %w", err)
		}

		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "application/json")
		req.Header.Set("User-Agent", "orochi-cli/1.0.0")

		if c.token != "" {
			req.Header.Set("Authorization", "Bearer "+c.token)
		}

		resp, err := c.httpClient.Do(req)
		if err != nil {
			lastErr = err
			if isRetryableError(err) && attempt < maxRetries {
				select {
				case <-ctx.Done():
					return nil, fmt.Errorf("request cancelled during retry: %w", ctx.Err())
				case <-time.After(calculateBackoff(attempt)):
					continue
				}
			}
			return nil, fmt.Errorf("request failed: %w", err)
		}

		// Check if we should retry based on status code
		if isRetryableStatus(resp.StatusCode) && attempt < maxRetries {
			resp.Body.Close()
			select {
			case <-ctx.Done():
				return nil, fmt.Errorf("request cancelled during retry: %w", ctx.Err())
			case <-time.After(calculateBackoff(attempt)):
				continue
			}
		}

		return resp, nil
	}

	return nil, fmt.Errorf("max retries exceeded: %w", lastErr)
}

// parseResponse parses an HTTP response into the target type.
func parseResponse[T any](resp *http.Response) (*T, error) {
	defer resp.Body.Close()

	// Use LimitReader to prevent reading excessively large response bodies
	limitedReader := io.LimitReader(resp.Body, maxResponseBodySize)
	body, err := io.ReadAll(limitedReader)
	if err != nil {
		return nil, fmt.Errorf("failed to read response body: %w", err)
	}

	// Check if we hit the limit (body was truncated)
	if int64(len(body)) == maxResponseBodySize {
		return nil, fmt.Errorf("response body exceeds maximum size of %d bytes", maxResponseBodySize)
	}

	if resp.StatusCode >= 400 {
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return nil, &APIError{
				StatusCode: resp.StatusCode,
				Code:       "unknown_error",
				Message:    string(body),
			}
		}
		apiErr.StatusCode = resp.StatusCode
		return nil, &apiErr
	}

	var result T
	if err := json.Unmarshal(body, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// Login authenticates a user and returns tokens.
func (c *Client) Login(ctx context.Context, email, password string) (*LoginResponse, error) {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/auth/login", &LoginRequest{
		Email:    email,
		Password: password,
	})
	if err != nil {
		return nil, err
	}

	return parseResponse[LoginResponse](resp)
}

// Logout invalidates the current session.
func (c *Client) Logout(ctx context.Context) error {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/auth/logout", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	// Use LimitReader for body reading
	limitedReader := io.LimitReader(resp.Body, maxResponseBodySize)

	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(limitedReader)
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{
				StatusCode: resp.StatusCode,
				Code:       "unknown_error",
				Message:    string(body),
			}
		}
		return &apiErr
	}

	return nil
}

// GetCurrentUser returns the current authenticated user.
func (c *Client) GetCurrentUser(ctx context.Context) (*User, error) {
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/users/me", nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[User](resp)
}

// ListClusters returns all clusters for the authenticated user.
func (c *Client) ListClusters(ctx context.Context) ([]Cluster, error) {
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Clusters []Cluster `json:"clusters"`
	}

	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}

	return result.Clusters, nil
}

// GetCluster returns a specific cluster by name.
func (c *Client) GetCluster(ctx context.Context, name string) (*Cluster, error) {
	escapedName := url.PathEscape(name)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName, nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// CreateCluster creates a new cluster.
func (c *Client) CreateCluster(ctx context.Context, req *CreateClusterRequest) (*Cluster, error) {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters", req)
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// DeleteCluster deletes a cluster by name.
func (c *Client) DeleteCluster(ctx context.Context, name string) error {
	escapedName := url.PathEscape(name)
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/clusters/"+escapedName, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	// Use LimitReader for body reading
	limitedReader := io.LimitReader(resp.Body, maxResponseBodySize)

	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(limitedReader)
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{
				StatusCode: resp.StatusCode,
				Code:       "unknown_error",
				Message:    string(body),
			}
		}
		return &apiErr
	}

	return nil
}

// ScaleCluster scales a cluster to a new replica count.
func (c *Client) ScaleCluster(ctx context.Context, name string, replicas int) (*Cluster, error) {
	escapedName := url.PathEscape(name)
	resp, err := c.doRequestWithContext(ctx, "PATCH", "/v1/clusters/"+escapedName+"/scale", &ScaleClusterRequest{
		Replicas: replicas,
	})
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// GetConnectionString returns the connection string for a cluster.
func (c *Client) GetConnectionString(ctx context.Context, name string) (string, error) {
	cluster, err := c.GetCluster(ctx, name)
	if err != nil {
		return "", err
	}

	if cluster.ConnectionURI != "" {
		return cluster.ConnectionURI, nil
	}

	// Construct connection string if not provided
	return fmt.Sprintf("postgresql://user@%s:%d/orochi?sslmode=require", cluster.Host, cluster.Port), nil
}

// ValidateClusterSize validates a cluster size string.
func ValidateClusterSize(size string) (ClusterSize, error) {
	switch ClusterSize(size) {
	case ClusterSizeSmall, ClusterSizeMedium, ClusterSizeLarge, ClusterSizeXLarge:
		return ClusterSize(size), nil
	default:
		return "", fmt.Errorf("invalid cluster size: %s (must be small, medium, large, or xlarge)", size)
	}
}

// GetClusterSpecs returns the specs for a cluster size.
func GetClusterSpecs(size ClusterSize) (vcpu int, memoryGB int, storageGB int) {
	switch size {
	case ClusterSizeSmall:
		return 2, 4, 50
	case ClusterSizeMedium:
		return 4, 8, 100
	case ClusterSizeLarge:
		return 8, 16, 250
	case ClusterSizeXLarge:
		return 16, 32, 500
	default:
		return 2, 4, 50
	}
}

// Backup represents a cluster backup.
type Backup struct {
	ID          string    `json:"id"`
	ClusterID   string    `json:"cluster_id"`
	ClusterName string    `json:"cluster_name"`
	Status      string    `json:"status"`
	Size        string    `json:"size"`
	CreatedAt   time.Time `json:"created_at"`
	ExpiresAt   time.Time `json:"expires_at,omitempty"`
}

// Branch represents a database branch.
type Branch struct {
	ID           string    `json:"id"`
	Name         string    `json:"name"`
	ClusterID    string    `json:"cluster_id"`
	ParentBranch string    `json:"parent_branch"`
	Status       string    `json:"status"`
	CreatedAt    time.Time `json:"created_at"`
	ConnectionURI string   `json:"connection_uri,omitempty"`
}

// Organization represents an organization.
type Organization struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Slug        string    `json:"slug"`
	Role        string    `json:"role,omitempty"`
	MemberCount int       `json:"member_count"`
	CreatedAt   time.Time `json:"created_at"`
}

// OrgMember represents an organization member.
type OrgMember struct {
	ID       string    `json:"id"`
	Email    string    `json:"email"`
	Name     string    `json:"name"`
	Role     string    `json:"role"`
	JoinedAt time.Time `json:"joined_at"`
}

// Setting represents a cluster setting.
type Setting struct {
	Name            string `json:"name"`
	Value           string `json:"value"`
	DefaultValue    string `json:"default_value"`
	Type            string `json:"type"`
	Description     string `json:"description,omitempty"`
	RequiresRestart bool   `json:"requires_restart"`
}

// LogEntry represents a log entry.
type LogEntry struct {
	Timestamp time.Time `json:"timestamp"`
	Level     string    `json:"level"`
	Message   string    `json:"message"`
}

// Event represents a cluster event.
type Event struct {
	ID        string    `json:"id"`
	Timestamp time.Time `json:"timestamp"`
	Type      string    `json:"type"`
	Message   string    `json:"message"`
}

// AuditEntry represents an audit log entry.
type AuditEntry struct {
	ID        string    `json:"id"`
	Timestamp time.Time `json:"timestamp"`
	Type      string    `json:"type"`
	User      string    `json:"user"`
	Action    string    `json:"action"`
}

// Version represents a PostgreSQL version.
type Version struct {
	Version string `json:"version"`
	Status  string `json:"status"`
	EOL     string `json:"eol,omitempty"`
}

// MetricsSummary represents a metrics summary.
type MetricsSummary struct {
	CPUPercent        float64 `json:"cpu_percent"`
	MemoryPercent     float64 `json:"memory_percent"`
	StorageUsedGB     float64 `json:"storage_used_gb"`
	StorageTotalGB    float64 `json:"storage_total_gb"`
	ActiveConnections int     `json:"active_connections"`
	MaxConnections    int     `json:"max_connections"`
	QueriesPerSecond  float64 `json:"queries_per_second"`
}

// CPUMetrics represents CPU metrics.
type CPUMetrics struct {
	Current float64 `json:"current"`
	Average float64 `json:"average"`
	Peak    float64 `json:"peak"`
}

// MemoryMetrics represents memory metrics.
type MemoryMetrics struct {
	UsedGB  float64 `json:"used_gb"`
	TotalGB float64 `json:"total_gb"`
	Percent float64 `json:"percent"`
}

// StorageMetrics represents storage metrics.
type StorageMetrics struct {
	UsedGB  float64 `json:"used_gb"`
	TotalGB float64 `json:"total_gb"`
	Percent float64 `json:"percent"`
}

// QueryMetrics represents query performance metrics.
type QueryMetrics struct {
	QueriesPerSecond float64 `json:"queries_per_second"`
	AvgLatencyMs     float64 `json:"avg_latency_ms"`
	SlowQueries      int     `json:"slow_queries"`
}

// ConnectionMetrics represents connection metrics.
type ConnectionMetrics struct {
	Active  int `json:"active"`
	Idle    int `json:"idle"`
	Max     int `json:"max"`
	Waiting int `json:"waiting"`
}

// DataJob represents a data import/export job.
type DataJob struct {
	ID        string    `json:"id"`
	Type      string    `json:"type"`
	Status    string    `json:"status"`
	Progress  int       `json:"progress"`
	CreatedAt time.Time `json:"created_at"`
}

// LogsRequest represents parameters for fetching logs.
type LogsRequest struct {
	Tail  int    `json:"tail,omitempty"`
	Since string `json:"since,omitempty"`
	Level string `json:"level,omitempty"`
}

// AuditRequest represents parameters for fetching audit logs.
type AuditRequest struct {
	Since string `json:"since,omitempty"`
	Type  string `json:"type,omitempty"`
}

// ImportRequest represents a data import request.
type ImportRequest struct {
	Cluster    string `json:"cluster"`
	Database   string `json:"database"`
	Table      string `json:"table"`
	FilePath   string `json:"file_path,omitempty"`
	URL        string `json:"url,omitempty"`
	Format     string `json:"format"`
	Compressed bool   `json:"compressed"`
	Delimiter  string `json:"delimiter,omitempty"`
	HasHeader  bool   `json:"has_header"`
}

// ExportRequest represents a data export request.
type ExportRequest struct {
	Cluster    string `json:"cluster"`
	Database   string `json:"database"`
	Table      string `json:"table"`
	FilePath   string `json:"file_path"`
	Format     string `json:"format"`
	Compressed bool   `json:"compressed"`
	Delimiter  string `json:"delimiter,omitempty"`
	HasHeader  bool   `json:"has_header"`
}

// CopyRequest represents a data copy request.
type CopyRequest struct {
	FromCluster string `json:"from_cluster"`
	ToCluster   string `json:"to_cluster"`
	Database    string `json:"database"`
	Table       string `json:"table"`
}

// UpgradeJob represents an upgrade job.
type UpgradeJob struct {
	ID          string    `json:"id"`
	ClusterName string    `json:"cluster_name"`
	FromVersion string    `json:"from_version"`
	ToVersion   string    `json:"to_version"`
	Status      string    `json:"status"`
	ScheduledAt time.Time `json:"scheduled_at,omitempty"`
}

// Backup operations

// ListBackups lists all backups for a cluster.
func (c *Client) ListBackups(ctx context.Context, clusterName string) ([]Backup, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/backups", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Backups []Backup `json:"backups"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Backups, nil
}

// CreateBackup creates a new backup.
func (c *Client) CreateBackup(ctx context.Context, clusterName string, retainDays int) (*Backup, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedName+"/backups", map[string]int{"retain_days": retainDays})
	if err != nil {
		return nil, err
	}
	return parseResponse[Backup](resp)
}

// RestoreBackup restores a backup to a cluster.
func (c *Client) RestoreBackup(ctx context.Context, backupID, targetCluster string) (*Cluster, error) {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/backups/"+url.PathEscape(backupID)+"/restore", map[string]string{"target_cluster": targetCluster})
	if err != nil {
		return nil, err
	}
	return parseResponse[Cluster](resp)
}

// DeleteBackup deletes a backup.
func (c *Client) DeleteBackup(ctx context.Context, backupID string) error {
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/backups/"+url.PathEscape(backupID), nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// Branch operations

// ListBranches lists all branches for a cluster.
func (c *Client) ListBranches(ctx context.Context, clusterName string) ([]Branch, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/branches", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Branches []Branch `json:"branches"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Branches, nil
}

// CreateBranch creates a new branch.
func (c *Client) CreateBranch(ctx context.Context, clusterName, branchName, parentBranch string) (*Branch, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedName+"/branches", map[string]string{
		"name":          branchName,
		"parent_branch": parentBranch,
	})
	if err != nil {
		return nil, err
	}
	return parseResponse[Branch](resp)
}

// GetBranch gets a branch by name.
func (c *Client) GetBranch(ctx context.Context, clusterName, branchName string) (*Branch, error) {
	escapedCluster := url.PathEscape(clusterName)
	escapedBranch := url.PathEscape(branchName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedCluster+"/branches/"+escapedBranch, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[Branch](resp)
}

// DeleteBranch deletes a branch.
func (c *Client) DeleteBranch(ctx context.Context, clusterName, branchName string) error {
	escapedCluster := url.PathEscape(clusterName)
	escapedBranch := url.PathEscape(branchName)
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/clusters/"+escapedCluster+"/branches/"+escapedBranch, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// ResetBranch resets a branch to a parent or point in time.
func (c *Client) ResetBranch(ctx context.Context, clusterName, branchName, resetTo string) (*Branch, error) {
	escapedCluster := url.PathEscape(clusterName)
	escapedBranch := url.PathEscape(branchName)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedCluster+"/branches/"+escapedBranch+"/reset", map[string]string{
		"reset_to": resetTo,
	})
	if err != nil {
		return nil, err
	}
	return parseResponse[Branch](resp)
}

// Organization operations

// ListOrganizations lists all organizations.
func (c *Client) ListOrganizations(ctx context.Context) ([]Organization, error) {
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/organizations", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Organizations []Organization `json:"organizations"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Organizations, nil
}

// CreateOrganization creates a new organization.
func (c *Client) CreateOrganization(ctx context.Context, name, slug string) (*Organization, error) {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/organizations", map[string]string{
		"name": name,
		"slug": slug,
	})
	if err != nil {
		return nil, err
	}
	return parseResponse[Organization](resp)
}

// GetOrganization gets an organization by slug.
func (c *Client) GetOrganization(ctx context.Context, slug string) (*Organization, error) {
	escapedSlug := url.PathEscape(slug)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/organizations/"+escapedSlug, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[Organization](resp)
}

// DeleteOrganization deletes an organization.
func (c *Client) DeleteOrganization(ctx context.Context, slug string) error {
	escapedSlug := url.PathEscape(slug)
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/organizations/"+escapedSlug, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// ListOrgMembers lists organization members.
func (c *Client) ListOrgMembers(ctx context.Context, slug string) ([]OrgMember, error) {
	escapedSlug := url.PathEscape(slug)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/organizations/"+escapedSlug+"/members", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Members []OrgMember `json:"members"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Members, nil
}

// InviteOrgMember invites a member to an organization.
func (c *Client) InviteOrgMember(ctx context.Context, slug, email, role string) error {
	escapedSlug := url.PathEscape(slug)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/organizations/"+escapedSlug+"/members", map[string]string{
		"email": email,
		"role":  role,
	})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// RemoveOrgMember removes a member from an organization.
func (c *Client) RemoveOrgMember(ctx context.Context, slug, email string) error {
	escapedSlug := url.PathEscape(slug)
	escapedEmail := url.PathEscape(email)
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/organizations/"+escapedSlug+"/members/"+escapedEmail, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// Settings operations

// ListSettings lists cluster settings.
func (c *Client) ListSettings(ctx context.Context, clusterName string) ([]Setting, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/settings", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Settings []Setting `json:"settings"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Settings, nil
}

// GetSetting gets a specific setting.
func (c *Client) GetSetting(ctx context.Context, clusterName, settingName string) (*Setting, error) {
	escapedCluster := url.PathEscape(clusterName)
	escapedSetting := url.PathEscape(settingName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedCluster+"/settings/"+escapedSetting, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[Setting](resp)
}

// SetSetting sets a setting value.
func (c *Client) SetSetting(ctx context.Context, clusterName, settingName, value string) (*Setting, error) {
	escapedCluster := url.PathEscape(clusterName)
	escapedSetting := url.PathEscape(settingName)
	resp, err := c.doRequestWithContext(ctx, "PUT", "/v1/clusters/"+escapedCluster+"/settings/"+escapedSetting, map[string]string{
		"value": value,
	})
	if err != nil {
		return nil, err
	}
	return parseResponse[Setting](resp)
}

// ResetSetting resets a setting to default.
func (c *Client) ResetSetting(ctx context.Context, clusterName, settingName string) (*Setting, error) {
	escapedCluster := url.PathEscape(clusterName)
	escapedSetting := url.PathEscape(settingName)
	resp, err := c.doRequestWithContext(ctx, "DELETE", "/v1/clusters/"+escapedCluster+"/settings/"+escapedSetting, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[Setting](resp)
}

// ApplySettings applies pending settings (triggers restart).
func (c *Client) ApplySettings(ctx context.Context, clusterName string) error {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedName+"/settings/apply", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// Metrics operations

// GetMetricsSummary gets metrics summary for a cluster.
func (c *Client) GetMetricsSummary(ctx context.Context, clusterName, period string) (*MetricsSummary, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/metrics/summary?period="+url.QueryEscape(period), nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[MetricsSummary](resp)
}

// GetCPUMetrics gets CPU metrics for a cluster.
func (c *Client) GetCPUMetrics(ctx context.Context, clusterName, period, interval string) (*CPUMetrics, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/metrics/cpu?period=" + url.QueryEscape(period)
	if interval != "" {
		path += "&interval=" + url.QueryEscape(interval)
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[CPUMetrics](resp)
}

// GetMemoryMetrics gets memory metrics for a cluster.
func (c *Client) GetMemoryMetrics(ctx context.Context, clusterName, period, interval string) (*MemoryMetrics, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/metrics/memory?period=" + url.QueryEscape(period)
	if interval != "" {
		path += "&interval=" + url.QueryEscape(interval)
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[MemoryMetrics](resp)
}

// GetStorageMetrics gets storage metrics for a cluster.
func (c *Client) GetStorageMetrics(ctx context.Context, clusterName, period, interval string) (*StorageMetrics, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/metrics/storage?period=" + url.QueryEscape(period)
	if interval != "" {
		path += "&interval=" + url.QueryEscape(interval)
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[StorageMetrics](resp)
}

// GetQueryMetrics gets query metrics for a cluster.
func (c *Client) GetQueryMetrics(ctx context.Context, clusterName, period string) (*QueryMetrics, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/metrics/queries?period="+url.QueryEscape(period), nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[QueryMetrics](resp)
}

// GetConnectionMetrics gets connection metrics for a cluster.
func (c *Client) GetConnectionMetrics(ctx context.Context, clusterName string) (*ConnectionMetrics, error) {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/clusters/"+escapedName+"/metrics/connections", nil)
	if err != nil {
		return nil, err
	}
	return parseResponse[ConnectionMetrics](resp)
}

// Admin operations

// RestartCluster restarts a cluster.
func (c *Client) RestartCluster(ctx context.Context, clusterName string) error {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedName+"/restart", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// SetMaintenanceMode enables or disables maintenance mode.
func (c *Client) SetMaintenanceMode(ctx context.Context, clusterName string, enable bool) error {
	escapedName := url.PathEscape(clusterName)
	resp, err := c.doRequestWithContext(ctx, "PUT", "/v1/clusters/"+escapedName+"/maintenance", map[string]bool{
		"enabled": enable,
	})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, maxResponseBodySize))
		var apiErr APIError
		if err := json.Unmarshal(body, &apiErr); err != nil {
			return &APIError{StatusCode: resp.StatusCode, Code: "unknown_error", Message: string(body)}
		}
		return &apiErr
	}
	return nil
}

// UpgradeCluster schedules a cluster upgrade.
func (c *Client) UpgradeCluster(ctx context.Context, clusterName, version, schedule string) (*UpgradeJob, error) {
	escapedName := url.PathEscape(clusterName)
	body := map[string]string{"version": version}
	if schedule != "" {
		body["schedule"] = schedule
	}
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedName+"/upgrade", body)
	if err != nil {
		return nil, err
	}
	return parseResponse[UpgradeJob](resp)
}

// GetClusterLogs gets cluster logs.
func (c *Client) GetClusterLogs(ctx context.Context, clusterName string, req *LogsRequest) ([]LogEntry, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/logs?tail=" + fmt.Sprintf("%d", req.Tail)
	if req.Since != "" {
		path += "&since=" + url.QueryEscape(req.Since)
	}
	if req.Level != "" {
		path += "&level=" + url.QueryEscape(req.Level)
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Logs []LogEntry `json:"logs"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Logs, nil
}

// GetClusterEvents gets cluster events.
func (c *Client) GetClusterEvents(ctx context.Context, clusterName, since string) ([]Event, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/events"
	if since != "" {
		path += "?since=" + url.QueryEscape(since)
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Events []Event `json:"events"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Events, nil
}

// GetAuditLogs gets audit logs.
func (c *Client) GetAuditLogs(ctx context.Context, clusterName string, req *AuditRequest) ([]AuditEntry, error) {
	escapedName := url.PathEscape(clusterName)
	path := "/v1/clusters/" + escapedName + "/audit"
	if req.Since != "" || req.Type != "" {
		path += "?"
		if req.Since != "" {
			path += "since=" + url.QueryEscape(req.Since)
		}
		if req.Type != "" {
			if req.Since != "" {
				path += "&"
			}
			path += "type=" + url.QueryEscape(req.Type)
		}
	}
	resp, err := c.doRequestWithContext(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		AuditLogs []AuditEntry `json:"audit_logs"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.AuditLogs, nil
}

// GetAvailableVersions gets available PostgreSQL versions.
func (c *Client) GetAvailableVersions(ctx context.Context) ([]Version, error) {
	resp, err := c.doRequestWithContext(ctx, "GET", "/v1/postgres/versions", nil)
	if err != nil {
		return nil, err
	}

	type listResponse struct {
		Versions []Version `json:"versions"`
	}
	result, err := parseResponse[listResponse](resp)
	if err != nil {
		return nil, err
	}
	return result.Versions, nil
}

// Data operations

// ImportFromFile imports data from a local file.
func (c *Client) ImportFromFile(ctx context.Context, req *ImportRequest) (*DataJob, error) {
	escapedCluster := url.PathEscape(req.Cluster)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedCluster+"/data/import", req)
	if err != nil {
		return nil, err
	}
	return parseResponse[DataJob](resp)
}

// ImportFromURL imports data from a URL.
func (c *Client) ImportFromURL(ctx context.Context, req *ImportRequest) (*DataJob, error) {
	escapedCluster := url.PathEscape(req.Cluster)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedCluster+"/data/import", req)
	if err != nil {
		return nil, err
	}
	return parseResponse[DataJob](resp)
}

// ExportToFile exports data to a file.
func (c *Client) ExportToFile(ctx context.Context, req *ExportRequest) (*DataJob, error) {
	escapedCluster := url.PathEscape(req.Cluster)
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/clusters/"+escapedCluster+"/data/export", req)
	if err != nil {
		return nil, err
	}
	return parseResponse[DataJob](resp)
}

// CopyData copies data between clusters.
func (c *Client) CopyData(ctx context.Context, req *CopyRequest) (*DataJob, error) {
	resp, err := c.doRequestWithContext(ctx, "POST", "/v1/data/copy", req)
	if err != nil {
		return nil, err
	}
	return parseResponse[DataJob](resp)
}
