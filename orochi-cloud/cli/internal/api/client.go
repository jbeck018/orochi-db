// Package api provides the HTTP client for the Orochi Cloud API.
package api

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"

	"github.com/orochi-db/orochi-cloud/cli/internal/config"
)

// Client is the Orochi Cloud API client.
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

// Cluster represents an Orochi Cloud cluster.
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

// User represents an Orochi Cloud user.
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

// NewClient creates a new API client.
func NewClient() *Client {
	return &Client{
		baseURL: config.GetAPIEndpoint(),
		httpClient: &http.Client{
			Timeout: 30 * time.Second,
		},
		token: config.GetAccessToken(),
	}
}

// NewClientWithToken creates a new API client with a specific token.
func NewClientWithToken(token string) *Client {
	return &Client{
		baseURL: config.GetAPIEndpoint(),
		httpClient: &http.Client{
			Timeout: 30 * time.Second,
		},
		token: token,
	}
}

// doRequest performs an HTTP request.
func (c *Client) doRequest(method, path string, body interface{}) (*http.Response, error) {
	var bodyReader io.Reader
	if body != nil {
		jsonBody, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("failed to marshal request body: %w", err)
		}
		bodyReader = bytes.NewReader(jsonBody)
	}

	req, err := http.NewRequest(method, c.baseURL+path, bodyReader)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orochi-cli/1.0.0")

	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}

	return c.httpClient.Do(req)
}

// parseResponse parses an HTTP response into the target type.
func parseResponse[T any](resp *http.Response) (*T, error) {
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("failed to read response body: %w", err)
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
func (c *Client) Login(email, password string) (*LoginResponse, error) {
	resp, err := c.doRequest("POST", "/v1/auth/login", &LoginRequest{
		Email:    email,
		Password: password,
	})
	if err != nil {
		return nil, err
	}

	return parseResponse[LoginResponse](resp)
}

// Logout invalidates the current session.
func (c *Client) Logout() error {
	resp, err := c.doRequest("POST", "/v1/auth/logout", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(resp.Body)
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
func (c *Client) GetCurrentUser() (*User, error) {
	resp, err := c.doRequest("GET", "/v1/users/me", nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[User](resp)
}

// ListClusters returns all clusters for the authenticated user.
func (c *Client) ListClusters() ([]Cluster, error) {
	resp, err := c.doRequest("GET", "/v1/clusters", nil)
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
func (c *Client) GetCluster(name string) (*Cluster, error) {
	resp, err := c.doRequest("GET", "/v1/clusters/"+name, nil)
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// CreateCluster creates a new cluster.
func (c *Client) CreateCluster(req *CreateClusterRequest) (*Cluster, error) {
	resp, err := c.doRequest("POST", "/v1/clusters", req)
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// DeleteCluster deletes a cluster by name.
func (c *Client) DeleteCluster(name string) error {
	resp, err := c.doRequest("DELETE", "/v1/clusters/"+name, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(resp.Body)
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
func (c *Client) ScaleCluster(name string, replicas int) (*Cluster, error) {
	resp, err := c.doRequest("PATCH", "/v1/clusters/"+name+"/scale", &ScaleClusterRequest{
		Replicas: replicas,
	})
	if err != nil {
		return nil, err
	}

	return parseResponse[Cluster](resp)
}

// GetConnectionString returns the connection string for a cluster.
func (c *Client) GetConnectionString(name string) (string, error) {
	cluster, err := c.GetCluster(name)
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
