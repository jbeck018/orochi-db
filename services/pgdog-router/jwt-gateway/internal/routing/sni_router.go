// Package routing provides SNI-based routing for multi-tenant PostgreSQL connections.
package routing

import (
	"errors"
	"fmt"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/config"
)

var (
	// ErrInvalidHostname is returned when the SNI hostname format is invalid.
	ErrInvalidHostname = errors.New("invalid hostname format")

	// ErrClusterNotFound is returned when a cluster cannot be found in the registry.
	ErrClusterNotFound = errors.New("cluster not found")

	// ErrBranchNotFound is returned when a branch cannot be found for a cluster.
	ErrBranchNotFound = errors.New("branch not found")

	// ErrRoutingDisabled is returned when SNI routing is not enabled.
	ErrRoutingDisabled = errors.New("SNI routing is disabled")
)

// RouteInfo contains parsed routing information from an SNI hostname.
type RouteInfo struct {
	// ClusterID is the unique identifier of the target cluster.
	ClusterID string

	// Branch is the database branch name.
	Branch string

	// FullHostname is the original SNI hostname.
	FullHostname string

	// BackendAddress is the resolved backend address (host:port).
	BackendAddress string
}

// SNIRouter handles parsing SNI hostnames and routing to backend clusters.
type SNIRouter struct {
	config   config.SNIConfig
	registry ClusterRegistry

	// Pattern for matching and extracting cluster/branch from hostname.
	pattern *regexp.Regexp

	// Cache for resolved routes.
	cache     map[string]*cacheEntry
	cacheMu   sync.RWMutex
	cacheSize int

	// Background refresh channel.
	refreshChan chan string
	stopChan    chan struct{}
	wg          sync.WaitGroup
}

// cacheEntry holds a cached route resolution.
type cacheEntry struct {
	route      *RouteInfo
	expiresAt  time.Time
	isNegative bool // true if this is a "not found" cache entry
}

// NewSNIRouter creates a new SNI router with the given configuration and registry.
func NewSNIRouter(cfg config.SNIConfig, registry ClusterRegistry) (*SNIRouter, error) {
	if !cfg.Enabled {
		return nil, ErrRoutingDisabled
	}

	// Build regex pattern from domain pattern.
	// Convert "{cluster}.{branch}.db.orochi.cloud" to a regex.
	pattern, err := buildPattern(cfg.DomainPattern, cfg.BaseDomain)
	if err != nil {
		return nil, fmt.Errorf("failed to build pattern: %w", err)
	}

	router := &SNIRouter{
		config:      cfg,
		registry:    registry,
		pattern:     pattern,
		cache:       make(map[string]*cacheEntry),
		refreshChan: make(chan string, 100),
		stopChan:    make(chan struct{}),
	}

	// Start background refresh goroutine if enabled.
	if cfg.Cache.RefreshAhead {
		router.wg.Add(1)
		go router.backgroundRefresh()
	}

	return router, nil
}

// buildPattern creates a regex pattern for parsing hostnames.
func buildPattern(domainPattern, baseDomain string) (*regexp.Regexp, error) {
	// Escape special regex characters in base domain.
	escapedBase := regexp.QuoteMeta(baseDomain)

	// Replace placeholders with named capture groups.
	pattern := domainPattern

	// Handle {cluster} placeholder.
	pattern = strings.Replace(pattern, "{cluster}", `(?P<cluster>[a-zA-Z0-9][-a-zA-Z0-9]*)`, 1)

	// Handle {branch} placeholder.
	pattern = strings.Replace(pattern, "{branch}", `(?P<branch>[a-zA-Z0-9][-a-zA-Z0-9_]*)`, 1)

	// Escape the rest and anchor.
	// Replace the base domain in the pattern.
	pattern = strings.Replace(pattern, baseDomain, escapedBase, 1)

	// Add anchors.
	pattern = "^" + pattern + "$"

	return regexp.Compile(pattern)
}

// Route resolves an SNI hostname to a backend address.
func (r *SNIRouter) Route(hostname string) (*RouteInfo, error) {
	// Normalize hostname to lowercase.
	hostname = strings.ToLower(hostname)

	// Check cache first.
	if route := r.getCached(hostname); route != nil {
		return route, nil
	}

	// Check negative cache.
	if r.isNegativelyCached(hostname) {
		return nil, ErrClusterNotFound
	}

	// Parse the hostname.
	clusterID, branch, err := r.parseHostname(hostname)
	if err != nil {
		return nil, err
	}

	// Look up the backend address from registry.
	backend, err := r.registry.GetClusterBackend(clusterID, branch)
	if err != nil {
		// Cache negative result.
		r.cacheNegative(hostname)
		return nil, err
	}

	route := &RouteInfo{
		ClusterID:      clusterID,
		Branch:         branch,
		FullHostname:   hostname,
		BackendAddress: backend,
	}

	// Cache the result.
	r.cacheRoute(hostname, route)

	return route, nil
}

// parseHostname extracts cluster ID and branch from an SNI hostname.
func (r *SNIRouter) parseHostname(hostname string) (clusterID, branch string, err error) {
	matches := r.pattern.FindStringSubmatch(hostname)
	if matches == nil {
		return "", "", ErrInvalidHostname
	}

	// Extract named groups.
	for i, name := range r.pattern.SubexpNames() {
		if i == 0 {
			continue
		}
		switch name {
		case "cluster":
			clusterID = matches[i]
		case "branch":
			branch = matches[i]
		}
	}

	if clusterID == "" {
		return "", "", ErrInvalidHostname
	}

	// Default branch to "main" if not specified.
	if branch == "" {
		branch = "main"
	}

	return clusterID, branch, nil
}

// ParseHostname extracts cluster ID and branch from an SNI hostname.
// This is a public version for testing and external use.
func (r *SNIRouter) ParseHostname(hostname string) (clusterID, branch string, err error) {
	return r.parseHostname(strings.ToLower(hostname))
}

// getCached retrieves a cached route if it exists and is not expired.
func (r *SNIRouter) getCached(hostname string) *RouteInfo {
	r.cacheMu.RLock()
	entry, ok := r.cache[hostname]
	r.cacheMu.RUnlock()

	if !ok || entry.isNegative {
		return nil
	}

	if time.Now().After(entry.expiresAt) {
		// Entry expired, trigger background refresh if enabled.
		if r.config.Cache.RefreshAhead {
			select {
			case r.refreshChan <- hostname:
			default:
				// Channel full, skip refresh.
			}
		}
		return nil
	}

	// Check if we should trigger background refresh.
	if r.config.Cache.RefreshAhead {
		refreshAt := entry.expiresAt.Add(-time.Duration(float64(r.config.Cache.TTL) * (1 - r.config.Cache.RefreshAheadRatio)))
		if time.Now().After(refreshAt) {
			select {
			case r.refreshChan <- hostname:
			default:
				// Channel full, skip refresh.
			}
		}
	}

	return entry.route
}

// isNegativelyCached checks if a hostname is in the negative cache.
func (r *SNIRouter) isNegativelyCached(hostname string) bool {
	r.cacheMu.RLock()
	entry, ok := r.cache[hostname]
	r.cacheMu.RUnlock()

	if !ok {
		return false
	}

	if !entry.isNegative {
		return false
	}

	if time.Now().After(entry.expiresAt) {
		return false
	}

	return true
}

// cacheRoute adds a route to the cache.
func (r *SNIRouter) cacheRoute(hostname string, route *RouteInfo) {
	r.cacheMu.Lock()
	defer r.cacheMu.Unlock()

	// Evict if cache is full.
	if len(r.cache) >= r.config.Cache.MaxSize {
		r.evictOldest()
	}

	r.cache[hostname] = &cacheEntry{
		route:     route,
		expiresAt: time.Now().Add(r.config.Cache.TTL),
	}
}

// cacheNegative adds a negative cache entry.
func (r *SNIRouter) cacheNegative(hostname string) {
	r.cacheMu.Lock()
	defer r.cacheMu.Unlock()

	r.cache[hostname] = &cacheEntry{
		isNegative: true,
		expiresAt:  time.Now().Add(r.config.Cache.NegativeTTL),
	}
}

// evictOldest removes the oldest cache entry.
func (r *SNIRouter) evictOldest() {
	var oldestKey string
	var oldestTime time.Time

	for key, entry := range r.cache {
		if oldestKey == "" || entry.expiresAt.Before(oldestTime) {
			oldestKey = key
			oldestTime = entry.expiresAt
		}
	}

	if oldestKey != "" {
		delete(r.cache, oldestKey)
	}
}

// backgroundRefresh handles background cache refresh.
func (r *SNIRouter) backgroundRefresh() {
	defer r.wg.Done()

	for {
		select {
		case hostname := <-r.refreshChan:
			r.refreshHostname(hostname)
		case <-r.stopChan:
			return
		}
	}
}

// refreshHostname refreshes the cache entry for a hostname.
func (r *SNIRouter) refreshHostname(hostname string) {
	clusterID, branch, err := r.parseHostname(hostname)
	if err != nil {
		return
	}

	backend, err := r.registry.GetClusterBackend(clusterID, branch)
	if err != nil {
		// Update to negative cache.
		r.cacheNegative(hostname)
		return
	}

	route := &RouteInfo{
		ClusterID:      clusterID,
		Branch:         branch,
		FullHostname:   hostname,
		BackendAddress: backend,
	}

	r.cacheRoute(hostname, route)
}

// InvalidateCache removes a hostname from the cache.
func (r *SNIRouter) InvalidateCache(hostname string) {
	r.cacheMu.Lock()
	delete(r.cache, strings.ToLower(hostname))
	r.cacheMu.Unlock()
}

// InvalidateCacheByCluster removes all cache entries for a cluster.
func (r *SNIRouter) InvalidateCacheByCluster(clusterID string) {
	r.cacheMu.Lock()
	defer r.cacheMu.Unlock()

	for key, entry := range r.cache {
		if entry.route != nil && entry.route.ClusterID == clusterID {
			delete(r.cache, key)
		}
	}
}

// CacheStats returns cache statistics.
func (r *SNIRouter) CacheStats() CacheStats {
	r.cacheMu.RLock()
	defer r.cacheMu.RUnlock()

	stats := CacheStats{
		Size: len(r.cache),
	}

	now := time.Now()
	for _, entry := range r.cache {
		if entry.isNegative {
			stats.NegativeEntries++
		}
		if now.After(entry.expiresAt) {
			stats.ExpiredEntries++
		}
	}

	return stats
}

// CacheStats holds cache statistics.
type CacheStats struct {
	Size            int
	NegativeEntries int
	ExpiredEntries  int
}

// Close stops the router and cleans up resources.
func (r *SNIRouter) Close() {
	close(r.stopChan)
	r.wg.Wait()
}

// GetDefaultBackend returns the default backend address for fallback routing.
func (r *SNIRouter) GetDefaultBackend() string {
	return r.config.DefaultBackend
}

// AllowsUnknownClusters returns whether unknown clusters should use default backend.
func (r *SNIRouter) AllowsUnknownClusters() bool {
	return r.config.AllowUnknownClusters
}

// ParseHostnameStatic parses a hostname without needing a router instance.
// This is useful for testing and validation.
func ParseHostnameStatic(hostname, baseDomain string) (clusterID, branch string, err error) {
	// Expected format: {cluster}.{branch}.{baseDomain}
	hostname = strings.ToLower(hostname)

	if !strings.HasSuffix(hostname, "."+baseDomain) {
		return "", "", ErrInvalidHostname
	}

	// Remove base domain suffix.
	prefix := strings.TrimSuffix(hostname, "."+baseDomain)

	// Split into parts.
	parts := strings.Split(prefix, ".")

	switch len(parts) {
	case 1:
		// Just cluster, default to main branch.
		clusterID = parts[0]
		branch = "main"
	case 2:
		// cluster.branch format.
		clusterID = parts[0]
		branch = parts[1]
	default:
		return "", "", ErrInvalidHostname
	}

	// Validate cluster ID format.
	if !isValidIdentifier(clusterID) {
		return "", "", ErrInvalidHostname
	}

	// Validate branch format.
	if !isValidIdentifier(branch) {
		return "", "", ErrInvalidHostname
	}

	return clusterID, branch, nil
}

// isValidIdentifier checks if a string is a valid cluster/branch identifier.
func isValidIdentifier(s string) bool {
	if s == "" || len(s) > 63 {
		return false
	}

	// Must start with alphanumeric.
	if !isAlphanumeric(s[0]) {
		return false
	}

	// Can contain alphanumeric, hyphens, underscores.
	for i := 1; i < len(s); i++ {
		c := s[i]
		if !isAlphanumeric(c) && c != '-' && c != '_' {
			return false
		}
	}

	return true
}

// isAlphanumeric checks if a byte is alphanumeric.
func isAlphanumeric(c byte) bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
}
