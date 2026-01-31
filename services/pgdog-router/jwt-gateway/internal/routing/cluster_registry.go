// Package routing provides cluster registry for looking up backend addresses.
package routing

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/config"
)

var (
	// ErrRegistryUnavailable is returned when the registry cannot be reached.
	ErrRegistryUnavailable = errors.New("registry unavailable")

	// ErrUnauthorized is returned when the API key is invalid.
	ErrUnauthorized = errors.New("unauthorized")
)

// ClusterRegistry is the interface for looking up cluster backend addresses.
type ClusterRegistry interface {
	// GetClusterBackend returns the backend address for a cluster and branch.
	GetClusterBackend(clusterID, branch string) (string, error)

	// Close closes the registry and releases resources.
	Close() error
}

// ClusterInfo contains information about a cluster.
type ClusterInfo struct {
	// ID is the unique cluster identifier.
	ID string `json:"id"`

	// Name is the human-readable cluster name.
	Name string `json:"name"`

	// Status is the current cluster status.
	Status string `json:"status"`

	// PgDogHost is the PgDog host for this cluster.
	PgDogHost string `json:"pgdog_host"`

	// PgDogPort is the PgDog port for this cluster.
	PgDogPort int `json:"pgdog_port"`

	// Branches contains branch-specific endpoints.
	Branches map[string]BranchInfo `json:"branches"`
}

// BranchInfo contains information about a database branch.
type BranchInfo struct {
	// Name is the branch name.
	Name string `json:"name"`

	// Status is the branch status.
	Status string `json:"status"`

	// Host overrides the cluster PgDog host for this branch.
	Host string `json:"host,omitempty"`

	// Port overrides the cluster PgDog port for this branch.
	Port int `json:"port,omitempty"`
}

// ControlPlaneRegistry implements ClusterRegistry using the control plane API.
type ControlPlaneRegistry struct {
	config config.ControlPlaneConfig
	client *http.Client
	logger *slog.Logger

	// Cache for cluster info.
	cache   map[string]*clusterCacheEntry
	cacheMu sync.RWMutex

	// Background refresh.
	stopChan chan struct{}
	wg       sync.WaitGroup
}

// clusterCacheEntry holds cached cluster information.
type clusterCacheEntry struct {
	info      *ClusterInfo
	expiresAt time.Time
}

// NewControlPlaneRegistry creates a new control plane registry.
func NewControlPlaneRegistry(cfg config.ControlPlaneConfig, cacheTTL time.Duration, logger *slog.Logger) *ControlPlaneRegistry {
	registry := &ControlPlaneRegistry{
		config: cfg,
		client: &http.Client{
			Timeout: cfg.Timeout,
		},
		logger:   logger,
		cache:    make(map[string]*clusterCacheEntry),
		stopChan: make(chan struct{}),
	}

	return registry
}

// GetClusterBackend returns the backend address for a cluster and branch.
func (r *ControlPlaneRegistry) GetClusterBackend(clusterID, branch string) (string, error) {
	// Check cache first.
	if info := r.getCachedCluster(clusterID); info != nil {
		return r.resolveBackendFromInfo(info, branch)
	}

	// Fetch from control plane.
	info, err := r.fetchClusterInfo(clusterID)
	if err != nil {
		return "", err
	}

	// Cache the result.
	r.cacheClusterInfo(clusterID, info)

	return r.resolveBackendFromInfo(info, branch)
}

// resolveBackendFromInfo resolves the backend address from cluster info.
func (r *ControlPlaneRegistry) resolveBackendFromInfo(info *ClusterInfo, branch string) (string, error) {
	// Check for branch-specific endpoint.
	if branchInfo, ok := info.Branches[branch]; ok {
		if branchInfo.Host != "" {
			host := branchInfo.Host
			port := branchInfo.Port
			if port == 0 {
				port = info.PgDogPort
			}
			return fmt.Sprintf("%s:%d", host, port), nil
		}
	} else if branch != "main" {
		// Branch not found.
		return "", ErrBranchNotFound
	}

	// Use cluster-level endpoint.
	if info.PgDogHost == "" {
		return "", ErrClusterNotFound
	}

	return fmt.Sprintf("%s:%d", info.PgDogHost, info.PgDogPort), nil
}

// fetchClusterInfo fetches cluster information from the control plane API.
func (r *ControlPlaneRegistry) fetchClusterInfo(clusterID string) (*ClusterInfo, error) {
	url := fmt.Sprintf("%s/api/v1/clusters/%s", r.config.URL, clusterID)

	var lastErr error
	for attempt := 0; attempt <= r.config.RetryCount; attempt++ {
		if attempt > 0 {
			time.Sleep(r.config.RetryDelay)
		}

		info, err := r.doFetch(url)
		if err == nil {
			return info, nil
		}

		lastErr = err

		// Don't retry on client errors.
		if errors.Is(err, ErrClusterNotFound) || errors.Is(err, ErrUnauthorized) {
			return nil, err
		}

		r.logger.Warn("Failed to fetch cluster info, retrying",
			"cluster_id", clusterID,
			"attempt", attempt+1,
			"error", err)
	}

	return nil, fmt.Errorf("%w: %v", ErrRegistryUnavailable, lastErr)
}

// doFetch performs a single fetch request.
func (r *ControlPlaneRegistry) doFetch(url string) (*ClusterInfo, error) {
	ctx, cancel := context.WithTimeout(context.Background(), r.config.Timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}

	// Add authorization header.
	if r.config.APIKey != "" {
		req.Header.Set("Authorization", "Bearer "+r.config.APIKey)
	}
	req.Header.Set("Accept", "application/json")

	resp, err := r.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	switch resp.StatusCode {
	case http.StatusOK:
		// Parse response.
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			return nil, err
		}

		var info ClusterInfo
		if err := json.Unmarshal(body, &info); err != nil {
			return nil, fmt.Errorf("failed to parse response: %w", err)
		}

		return &info, nil

	case http.StatusNotFound:
		return nil, ErrClusterNotFound

	case http.StatusUnauthorized, http.StatusForbidden:
		return nil, ErrUnauthorized

	default:
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("unexpected status %d: %s", resp.StatusCode, string(body))
	}
}

// getCachedCluster retrieves cached cluster info if available and not expired.
func (r *ControlPlaneRegistry) getCachedCluster(clusterID string) *ClusterInfo {
	r.cacheMu.RLock()
	entry, ok := r.cache[clusterID]
	r.cacheMu.RUnlock()

	if !ok || time.Now().After(entry.expiresAt) {
		return nil
	}

	return entry.info
}

// cacheClusterInfo caches cluster information.
func (r *ControlPlaneRegistry) cacheClusterInfo(clusterID string, info *ClusterInfo) {
	r.cacheMu.Lock()
	defer r.cacheMu.Unlock()

	// Use a default TTL of 5 minutes if not configured.
	ttl := 5 * time.Minute

	r.cache[clusterID] = &clusterCacheEntry{
		info:      info,
		expiresAt: time.Now().Add(ttl),
	}
}

// InvalidateCluster removes a cluster from the cache.
func (r *ControlPlaneRegistry) InvalidateCluster(clusterID string) {
	r.cacheMu.Lock()
	delete(r.cache, clusterID)
	r.cacheMu.Unlock()
}

// Close closes the registry.
func (r *ControlPlaneRegistry) Close() error {
	close(r.stopChan)
	r.wg.Wait()
	return nil
}

// StaticRegistry is a simple registry that uses a static mapping.
// Useful for testing and simple deployments.
type StaticRegistry struct {
	clusters map[string]*ClusterInfo
	mu       sync.RWMutex
}

// NewStaticRegistry creates a new static registry.
func NewStaticRegistry() *StaticRegistry {
	return &StaticRegistry{
		clusters: make(map[string]*ClusterInfo),
	}
}

// AddCluster adds a cluster to the static registry.
func (r *StaticRegistry) AddCluster(info *ClusterInfo) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.clusters[info.ID] = info
}

// RemoveCluster removes a cluster from the static registry.
func (r *StaticRegistry) RemoveCluster(clusterID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.clusters, clusterID)
}

// GetClusterBackend returns the backend address for a cluster and branch.
func (r *StaticRegistry) GetClusterBackend(clusterID, branch string) (string, error) {
	r.mu.RLock()
	info, ok := r.clusters[clusterID]
	r.mu.RUnlock()

	if !ok {
		return "", ErrClusterNotFound
	}

	// Check for branch-specific endpoint.
	if branchInfo, ok := info.Branches[branch]; ok {
		if branchInfo.Host != "" {
			host := branchInfo.Host
			port := branchInfo.Port
			if port == 0 {
				port = info.PgDogPort
			}
			return fmt.Sprintf("%s:%d", host, port), nil
		}
	} else if branch != "main" {
		return "", ErrBranchNotFound
	}

	if info.PgDogHost == "" {
		return "", ErrClusterNotFound
	}

	return fmt.Sprintf("%s:%d", info.PgDogHost, info.PgDogPort), nil
}

// Close closes the static registry.
func (r *StaticRegistry) Close() error {
	return nil
}

// WildcardRegistry wraps another registry and adds support for wildcard patterns.
type WildcardRegistry struct {
	base     ClusterRegistry
	patterns map[string]string // pattern -> backend mapping
	mu       sync.RWMutex
}

// NewWildcardRegistry creates a new wildcard registry.
func NewWildcardRegistry(base ClusterRegistry) *WildcardRegistry {
	return &WildcardRegistry{
		base:     base,
		patterns: make(map[string]string),
	}
}

// AddPattern adds a wildcard pattern.
// Pattern format: "*" matches any cluster, "dev-*" matches clusters starting with "dev-".
func (r *WildcardRegistry) AddPattern(pattern, backend string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.patterns[pattern] = backend
}

// GetClusterBackend returns the backend address for a cluster and branch.
func (r *WildcardRegistry) GetClusterBackend(clusterID, branch string) (string, error) {
	// Try base registry first.
	backend, err := r.base.GetClusterBackend(clusterID, branch)
	if err == nil {
		return backend, nil
	}

	if !errors.Is(err, ErrClusterNotFound) {
		return "", err
	}

	// Try wildcard patterns.
	r.mu.RLock()
	defer r.mu.RUnlock()

	// Check prefix patterns first (more specific).
	for pattern, backend := range r.patterns {
		if pattern != "*" && matchWildcard(pattern, clusterID) {
			return backend, nil
		}
	}

	// Fall back to catch-all wildcard.
	if backend, ok := r.patterns["*"]; ok {
		return backend, nil
	}

	return "", ErrClusterNotFound
}

// matchWildcard checks if a cluster ID matches a wildcard pattern.
func matchWildcard(pattern, clusterID string) bool {
	if pattern == "*" {
		return true
	}

	// Prefix match: "dev-*" matches "dev-cluster1".
	if len(pattern) > 0 && pattern[len(pattern)-1] == '*' {
		prefix := pattern[:len(pattern)-1]
		return len(clusterID) >= len(prefix) && clusterID[:len(prefix)] == prefix
	}

	return pattern == clusterID
}

// Close closes the wildcard registry.
func (r *WildcardRegistry) Close() error {
	return r.base.Close()
}

// ChainedRegistry chains multiple registries together.
// It tries each registry in order until one succeeds.
type ChainedRegistry struct {
	registries []ClusterRegistry
}

// NewChainedRegistry creates a new chained registry.
func NewChainedRegistry(registries ...ClusterRegistry) *ChainedRegistry {
	return &ChainedRegistry{
		registries: registries,
	}
}

// GetClusterBackend returns the backend address for a cluster and branch.
func (r *ChainedRegistry) GetClusterBackend(clusterID, branch string) (string, error) {
	var lastErr error

	for _, registry := range r.registries {
		backend, err := registry.GetClusterBackend(clusterID, branch)
		if err == nil {
			return backend, nil
		}
		lastErr = err

		// Only continue on "not found" errors.
		if !errors.Is(err, ErrClusterNotFound) && !errors.Is(err, ErrBranchNotFound) {
			return "", err
		}
	}

	if lastErr != nil {
		return "", lastErr
	}

	return "", ErrClusterNotFound
}

// Close closes all registries in the chain.
func (r *ChainedRegistry) Close() error {
	var errs []error
	for _, registry := range r.registries {
		if err := registry.Close(); err != nil {
			errs = append(errs, err)
		}
	}

	if len(errs) > 0 {
		return fmt.Errorf("failed to close registries: %v", errs)
	}

	return nil
}
