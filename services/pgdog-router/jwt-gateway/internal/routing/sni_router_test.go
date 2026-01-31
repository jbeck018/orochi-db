package routing

import (
	"testing"
	"time"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/config"
)

// mockRegistry is a mock implementation of ClusterRegistry for testing.
type mockRegistry struct {
	clusters map[string]map[string]string // clusterID -> branch -> backend
}

func newMockRegistry() *mockRegistry {
	return &mockRegistry{
		clusters: make(map[string]map[string]string),
	}
}

func (r *mockRegistry) AddCluster(clusterID, branch, backend string) {
	if r.clusters[clusterID] == nil {
		r.clusters[clusterID] = make(map[string]string)
	}
	r.clusters[clusterID][branch] = backend
}

func (r *mockRegistry) GetClusterBackend(clusterID, branch string) (string, error) {
	branches, ok := r.clusters[clusterID]
	if !ok {
		return "", ErrClusterNotFound
	}

	backend, ok := branches[branch]
	if !ok {
		// Try main branch as default.
		if backend, ok = branches["main"]; ok && branch == "main" {
			return backend, nil
		}
		return "", ErrBranchNotFound
	}

	return backend, nil
}

func (r *mockRegistry) Close() error {
	return nil
}

func TestParseHostnameStatic(t *testing.T) {
	tests := []struct {
		name        string
		hostname    string
		baseDomain  string
		wantCluster string
		wantBranch  string
		wantErr     error
	}{
		{
			name:        "valid cluster and branch",
			hostname:    "my-cluster.dev.db.orochi.cloud",
			baseDomain:  "db.orochi.cloud",
			wantCluster: "my-cluster",
			wantBranch:  "dev",
			wantErr:     nil,
		},
		{
			name:        "valid cluster only (default to main)",
			hostname:    "my-cluster.db.orochi.cloud",
			baseDomain:  "db.orochi.cloud",
			wantCluster: "my-cluster",
			wantBranch:  "main",
			wantErr:     nil,
		},
		{
			name:        "cluster with numbers",
			hostname:    "cluster123.staging.db.orochi.cloud",
			baseDomain:  "db.orochi.cloud",
			wantCluster: "cluster123",
			wantBranch:  "staging",
			wantErr:     nil,
		},
		{
			name:        "branch with underscore",
			hostname:    "my-cluster.feature_branch.db.orochi.cloud",
			baseDomain:  "db.orochi.cloud",
			wantCluster: "my-cluster",
			wantBranch:  "feature_branch",
			wantErr:     nil,
		},
		{
			name:        "uppercase hostname normalized",
			hostname:    "MY-CLUSTER.DEV.db.orochi.cloud",
			baseDomain:  "db.orochi.cloud",
			wantCluster: "my-cluster",
			wantBranch:  "dev",
			wantErr:     nil,
		},
		{
			name:       "wrong base domain",
			hostname:   "my-cluster.dev.db.example.com",
			baseDomain: "db.orochi.cloud",
			wantErr:    ErrInvalidHostname,
		},
		{
			name:       "too many parts",
			hostname:   "a.b.c.db.orochi.cloud",
			baseDomain: "db.orochi.cloud",
			wantErr:    ErrInvalidHostname,
		},
		{
			name:       "empty cluster",
			hostname:   ".dev.db.orochi.cloud",
			baseDomain: "db.orochi.cloud",
			wantErr:    ErrInvalidHostname,
		},
		{
			name:       "cluster starting with hyphen",
			hostname:   "-cluster.dev.db.orochi.cloud",
			baseDomain: "db.orochi.cloud",
			wantErr:    ErrInvalidHostname,
		},
		{
			name:       "just base domain",
			hostname:   "db.orochi.cloud",
			baseDomain: "db.orochi.cloud",
			wantErr:    ErrInvalidHostname,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			cluster, branch, err := ParseHostnameStatic(tt.hostname, tt.baseDomain)

			if tt.wantErr != nil {
				if err == nil {
					t.Errorf("expected error %v, got nil", tt.wantErr)
				} else if err != tt.wantErr {
					t.Errorf("expected error %v, got %v", tt.wantErr, err)
				}
				return
			}

			if err != nil {
				t.Errorf("unexpected error: %v", err)
				return
			}

			if cluster != tt.wantCluster {
				t.Errorf("expected cluster %q, got %q", tt.wantCluster, cluster)
			}
			if branch != tt.wantBranch {
				t.Errorf("expected branch %q, got %q", tt.wantBranch, branch)
			}
		})
	}
}

func TestSNIRouter_Route(t *testing.T) {
	// Create a mock registry with some clusters.
	registry := newMockRegistry()
	registry.AddCluster("cluster1", "main", "pgdog-cluster1.internal:5432")
	registry.AddCluster("cluster1", "dev", "pgdog-cluster1-dev.internal:5432")
	registry.AddCluster("cluster2", "main", "pgdog-cluster2.internal:5432")

	// Create SNI router.
	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         5 * time.Minute,
			NegativeTTL: 30 * time.Second,
			MaxSize:     1000,
		},
	}

	router, err := NewSNIRouter(cfg, registry)
	if err != nil {
		t.Fatalf("failed to create SNI router: %v", err)
	}
	defer router.Close()

	tests := []struct {
		name           string
		hostname       string
		wantCluster    string
		wantBranch     string
		wantBackend    string
		wantErr        error
	}{
		{
			name:        "route to cluster1 main",
			hostname:    "cluster1.main.db.orochi.cloud",
			wantCluster: "cluster1",
			wantBranch:  "main",
			wantBackend: "pgdog-cluster1.internal:5432",
		},
		{
			name:        "route to cluster1 dev",
			hostname:    "cluster1.dev.db.orochi.cloud",
			wantCluster: "cluster1",
			wantBranch:  "dev",
			wantBackend: "pgdog-cluster1-dev.internal:5432",
		},
		{
			name:        "route to cluster2 main",
			hostname:    "cluster2.main.db.orochi.cloud",
			wantCluster: "cluster2",
			wantBranch:  "main",
			wantBackend: "pgdog-cluster2.internal:5432",
		},
		{
			name:     "unknown cluster",
			hostname: "unknown.main.db.orochi.cloud",
			wantErr:  ErrClusterNotFound,
		},
		{
			name:     "unknown branch",
			hostname: "cluster1.unknown.db.orochi.cloud",
			wantErr:  ErrBranchNotFound,
		},
		{
			name:     "invalid hostname format",
			hostname: "invalid-hostname.example.com",
			wantErr:  ErrInvalidHostname,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			route, err := router.Route(tt.hostname)

			if tt.wantErr != nil {
				if err == nil {
					t.Errorf("expected error %v, got nil", tt.wantErr)
				}
				return
			}

			if err != nil {
				t.Errorf("unexpected error: %v", err)
				return
			}

			if route.ClusterID != tt.wantCluster {
				t.Errorf("expected cluster %q, got %q", tt.wantCluster, route.ClusterID)
			}
			if route.Branch != tt.wantBranch {
				t.Errorf("expected branch %q, got %q", tt.wantBranch, route.Branch)
			}
			if route.BackendAddress != tt.wantBackend {
				t.Errorf("expected backend %q, got %q", tt.wantBackend, route.BackendAddress)
			}
		})
	}
}

func TestSNIRouter_Caching(t *testing.T) {
	// Create a mock registry.
	registry := newMockRegistry()
	registry.AddCluster("cached-cluster", "main", "pgdog-cached.internal:5432")

	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         100 * time.Millisecond,
			NegativeTTL: 50 * time.Millisecond,
			MaxSize:     1000,
		},
	}

	router, err := NewSNIRouter(cfg, registry)
	if err != nil {
		t.Fatalf("failed to create SNI router: %v", err)
	}
	defer router.Close()

	hostname := "cached-cluster.main.db.orochi.cloud"

	// First request should populate cache.
	route1, err := router.Route(hostname)
	if err != nil {
		t.Fatalf("first route failed: %v", err)
	}

	// Second request should hit cache.
	route2, err := router.Route(hostname)
	if err != nil {
		t.Fatalf("second route failed: %v", err)
	}

	if route1.BackendAddress != route2.BackendAddress {
		t.Errorf("cached result differs: %q vs %q", route1.BackendAddress, route2.BackendAddress)
	}

	// Check cache stats.
	stats := router.CacheStats()
	if stats.Size != 1 {
		t.Errorf("expected cache size 1, got %d", stats.Size)
	}

	// Wait for cache to expire.
	time.Sleep(150 * time.Millisecond)

	// Should still work but refetch from registry.
	route3, err := router.Route(hostname)
	if err != nil {
		t.Fatalf("third route failed: %v", err)
	}

	if route3.BackendAddress != route1.BackendAddress {
		t.Errorf("result after expiry differs: %q vs %q", route3.BackendAddress, route1.BackendAddress)
	}
}

func TestSNIRouter_NegativeCaching(t *testing.T) {
	registry := newMockRegistry()

	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         5 * time.Minute,
			NegativeTTL: 100 * time.Millisecond,
			MaxSize:     1000,
		},
	}

	router, err := NewSNIRouter(cfg, registry)
	if err != nil {
		t.Fatalf("failed to create SNI router: %v", err)
	}
	defer router.Close()

	hostname := "nonexistent.main.db.orochi.cloud"

	// First request should fail and cache negative result.
	_, err = router.Route(hostname)
	if err == nil {
		t.Fatal("expected error for nonexistent cluster")
	}

	// Second request should hit negative cache immediately.
	_, err = router.Route(hostname)
	if err == nil {
		t.Fatal("expected error from negative cache")
	}

	// Add the cluster to registry.
	registry.AddCluster("nonexistent", "main", "pgdog-new.internal:5432")

	// Should still fail due to negative cache.
	_, err = router.Route(hostname)
	if err == nil {
		t.Fatal("expected error while in negative cache")
	}

	// Wait for negative cache to expire.
	time.Sleep(150 * time.Millisecond)

	// Now should succeed.
	route, err := router.Route(hostname)
	if err != nil {
		t.Fatalf("expected success after negative cache expiry: %v", err)
	}

	if route.BackendAddress != "pgdog-new.internal:5432" {
		t.Errorf("expected backend 'pgdog-new.internal:5432', got %q", route.BackendAddress)
	}
}

func TestSNIRouter_InvalidateCache(t *testing.T) {
	registry := newMockRegistry()
	registry.AddCluster("test-cluster", "main", "pgdog-old.internal:5432")

	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         5 * time.Minute,
			NegativeTTL: 30 * time.Second,
			MaxSize:     1000,
		},
	}

	router, err := NewSNIRouter(cfg, registry)
	if err != nil {
		t.Fatalf("failed to create SNI router: %v", err)
	}
	defer router.Close()

	hostname := "test-cluster.main.db.orochi.cloud"

	// Populate cache.
	route1, _ := router.Route(hostname)
	if route1.BackendAddress != "pgdog-old.internal:5432" {
		t.Fatalf("unexpected initial backend: %s", route1.BackendAddress)
	}

	// Update registry.
	registry.AddCluster("test-cluster", "main", "pgdog-new.internal:5432")

	// Should still return old value from cache.
	route2, _ := router.Route(hostname)
	if route2.BackendAddress != "pgdog-old.internal:5432" {
		t.Error("cache should return old value")
	}

	// Invalidate cache.
	router.InvalidateCache(hostname)

	// Should now return new value.
	route3, _ := router.Route(hostname)
	if route3.BackendAddress != "pgdog-new.internal:5432" {
		t.Errorf("expected new backend after invalidation, got: %s", route3.BackendAddress)
	}
}

func TestSNIRouter_InvalidateCacheByCluster(t *testing.T) {
	registry := newMockRegistry()
	registry.AddCluster("cluster-a", "main", "pgdog-a-main.internal:5432")
	registry.AddCluster("cluster-a", "dev", "pgdog-a-dev.internal:5432")
	registry.AddCluster("cluster-b", "main", "pgdog-b-main.internal:5432")

	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         5 * time.Minute,
			NegativeTTL: 30 * time.Second,
			MaxSize:     1000,
		},
	}

	router, err := NewSNIRouter(cfg, registry)
	if err != nil {
		t.Fatalf("failed to create SNI router: %v", err)
	}
	defer router.Close()

	// Populate cache for all endpoints.
	router.Route("cluster-a.main.db.orochi.cloud")
	router.Route("cluster-a.dev.db.orochi.cloud")
	router.Route("cluster-b.main.db.orochi.cloud")

	stats := router.CacheStats()
	if stats.Size != 3 {
		t.Errorf("expected cache size 3, got %d", stats.Size)
	}

	// Invalidate cluster-a.
	router.InvalidateCacheByCluster("cluster-a")

	stats = router.CacheStats()
	if stats.Size != 1 {
		t.Errorf("expected cache size 1 after invalidation, got %d", stats.Size)
	}
}

func TestStaticRegistry(t *testing.T) {
	registry := NewStaticRegistry()

	// Add a cluster.
	registry.AddCluster(&ClusterInfo{
		ID:        "static-cluster",
		Name:      "Static Cluster",
		PgDogHost: "pgdog-static.internal",
		PgDogPort: 5432,
		Branches: map[string]BranchInfo{
			"main": {Name: "main"},
			"dev":  {Name: "dev", Host: "pgdog-static-dev.internal", Port: 5433},
		},
	})

	// Test main branch.
	backend, err := registry.GetClusterBackend("static-cluster", "main")
	if err != nil {
		t.Fatalf("failed to get main branch: %v", err)
	}
	if backend != "pgdog-static.internal:5432" {
		t.Errorf("expected 'pgdog-static.internal:5432', got %q", backend)
	}

	// Test dev branch with override.
	backend, err = registry.GetClusterBackend("static-cluster", "dev")
	if err != nil {
		t.Fatalf("failed to get dev branch: %v", err)
	}
	if backend != "pgdog-static-dev.internal:5433" {
		t.Errorf("expected 'pgdog-static-dev.internal:5433', got %q", backend)
	}

	// Test unknown cluster.
	_, err = registry.GetClusterBackend("unknown", "main")
	if err != ErrClusterNotFound {
		t.Errorf("expected ErrClusterNotFound, got %v", err)
	}

	// Test unknown branch.
	_, err = registry.GetClusterBackend("static-cluster", "unknown")
	if err != ErrBranchNotFound {
		t.Errorf("expected ErrBranchNotFound, got %v", err)
	}

	// Test remove cluster.
	registry.RemoveCluster("static-cluster")
	_, err = registry.GetClusterBackend("static-cluster", "main")
	if err != ErrClusterNotFound {
		t.Errorf("expected ErrClusterNotFound after removal, got %v", err)
	}
}

func TestWildcardRegistry(t *testing.T) {
	base := NewStaticRegistry()
	base.AddCluster(&ClusterInfo{
		ID:        "known-cluster",
		PgDogHost: "pgdog-known.internal",
		PgDogPort: 5432,
		Branches:  map[string]BranchInfo{"main": {Name: "main"}},
	})

	wildcard := NewWildcardRegistry(base)
	wildcard.AddPattern("dev-*", "pgdog-dev-pool.internal:5432")
	wildcard.AddPattern("*", "pgdog-default.internal:5432")

	// Test known cluster from base.
	backend, err := wildcard.GetClusterBackend("known-cluster", "main")
	if err != nil {
		t.Fatalf("failed to get known cluster: %v", err)
	}
	if backend != "pgdog-known.internal:5432" {
		t.Errorf("expected 'pgdog-known.internal:5432', got %q", backend)
	}

	// Test prefix match.
	backend, err = wildcard.GetClusterBackend("dev-cluster1", "main")
	if err != nil {
		t.Fatalf("failed to get dev prefix cluster: %v", err)
	}
	if backend != "pgdog-dev-pool.internal:5432" {
		t.Errorf("expected 'pgdog-dev-pool.internal:5432', got %q", backend)
	}

	// Test wildcard match.
	backend, err = wildcard.GetClusterBackend("some-random-cluster", "main")
	if err != nil {
		t.Fatalf("failed to get wildcard cluster: %v", err)
	}
	if backend != "pgdog-default.internal:5432" {
		t.Errorf("expected 'pgdog-default.internal:5432', got %q", backend)
	}
}

func TestChainedRegistry(t *testing.T) {
	primary := NewStaticRegistry()
	primary.AddCluster(&ClusterInfo{
		ID:        "primary-cluster",
		PgDogHost: "pgdog-primary.internal",
		PgDogPort: 5432,
		Branches:  map[string]BranchInfo{"main": {Name: "main"}},
	})

	fallback := NewStaticRegistry()
	fallback.AddCluster(&ClusterInfo{
		ID:        "fallback-cluster",
		PgDogHost: "pgdog-fallback.internal",
		PgDogPort: 5432,
		Branches:  map[string]BranchInfo{"main": {Name: "main"}},
	})

	chained := NewChainedRegistry(primary, fallback)

	// Test primary registry.
	backend, err := chained.GetClusterBackend("primary-cluster", "main")
	if err != nil {
		t.Fatalf("failed to get primary cluster: %v", err)
	}
	if backend != "pgdog-primary.internal:5432" {
		t.Errorf("expected 'pgdog-primary.internal:5432', got %q", backend)
	}

	// Test fallback to secondary.
	backend, err = chained.GetClusterBackend("fallback-cluster", "main")
	if err != nil {
		t.Fatalf("failed to get fallback cluster: %v", err)
	}
	if backend != "pgdog-fallback.internal:5432" {
		t.Errorf("expected 'pgdog-fallback.internal:5432', got %q", backend)
	}

	// Test not found in any registry.
	_, err = chained.GetClusterBackend("unknown", "main")
	if err != ErrClusterNotFound {
		t.Errorf("expected ErrClusterNotFound, got %v", err)
	}
}

func TestIsValidIdentifier(t *testing.T) {
	tests := []struct {
		input string
		valid bool
	}{
		{"cluster", true},
		{"cluster1", true},
		{"my-cluster", true},
		{"my_cluster", true},
		{"CLUSTER", true},
		{"c", true},
		{"", false},
		{"-cluster", false},
		{"_cluster", false},
		{"cluster!", false},
		{"cluster.name", false},
		{"cluster/name", false},
		{string(make([]byte, 64)), false}, // Too long
	}

	for _, tt := range tests {
		result := isValidIdentifier(tt.input)
		if result != tt.valid {
			t.Errorf("isValidIdentifier(%q) = %v, want %v", tt.input, result, tt.valid)
		}
	}
}

func BenchmarkSNIRouter_Route(b *testing.B) {
	registry := newMockRegistry()
	for i := 0; i < 100; i++ {
		registry.AddCluster(
			"cluster"+string(rune('0'+i%10)),
			"main",
			"pgdog.internal:5432",
		)
	}

	cfg := config.SNIConfig{
		Enabled:       true,
		DomainPattern: "{cluster}.{branch}.db.orochi.cloud",
		BaseDomain:    "db.orochi.cloud",
		Cache: config.CacheConfig{
			TTL:         5 * time.Minute,
			NegativeTTL: 30 * time.Second,
			MaxSize:     1000,
		},
	}

	router, _ := NewSNIRouter(cfg, registry)
	defer router.Close()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		router.Route("cluster1.main.db.orochi.cloud")
	}
}

func BenchmarkParseHostnameStatic(b *testing.B) {
	hostname := "my-cluster.dev.db.orochi.cloud"
	baseDomain := "db.orochi.cloud"

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ParseHostnameStatic(hostname, baseDomain)
	}
}
