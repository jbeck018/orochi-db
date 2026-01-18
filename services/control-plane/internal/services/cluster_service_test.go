package services_test

import (
	"context"
	"log/slog"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
	"github.com/orochi-db/orochi-db/services/control-plane/pkg/config"
)

// testDB is the shared test database connection
var testDB *db.DB

// TestMain sets up the test database
func TestMain(m *testing.M) {
	// Use environment variables for test database configuration
	cfg := &config.DatabaseConfig{
		Host:            getEnvOrDefault("TEST_DB_HOST", "localhost"),
		Port:            getEnvOrDefaultInt("TEST_DB_PORT", 5432),
		User:            getEnvOrDefault("TEST_DB_USER", "orochi"),
		Password:        getEnvOrDefault("TEST_DB_PASSWORD", "orochi"),
		Database:        getEnvOrDefault("TEST_DB_NAME", "orochi_cloud_test"),
		SSLMode:         "disable",
		MaxConns:        5,
		MinConns:        1,
		MaxConnLifetime: time.Hour,
		MaxConnIdleTime: 30 * time.Minute,
	}

	// Connect to database
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	var err error
	testDB, err = db.New(ctx, cfg)
	if err != nil {
		// Skip tests if database is not available
		os.Stderr.WriteString("Warning: Test database not available, skipping integration tests\n")
		os.Exit(0)
	}
	defer testDB.Close()

	// Run migrations
	if err := testDB.RunMigrations(ctx); err != nil {
		os.Stderr.WriteString("Failed to run migrations: " + err.Error() + "\n")
		os.Exit(1)
	}

	// Run tests
	code := m.Run()
	os.Exit(code)
}

func getEnvOrDefault(key, defaultValue string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return defaultValue
}

func getEnvOrDefaultInt(key string, defaultValue int) int {
	if value := os.Getenv(key); value != "" {
		if intVal, err := strconv.Atoi(value); err == nil {
			return intVal
		}
	}
	return defaultValue
}

// createTestUser creates a test user for cluster ownership
func createTestUser(t *testing.T, ctx context.Context, email string) uuid.UUID {
	userID := uuid.New()
	_, err := testDB.Pool.Exec(ctx, `
		INSERT INTO users (id, email, password_hash, name, role, active)
		VALUES ($1, $2, 'test-hash', 'Test User', 'member', true)
		ON CONFLICT (email) DO UPDATE SET id = EXCLUDED.id
		RETURNING id
	`, userID, email)
	if err != nil {
		t.Fatalf("failed to create test user: %v", err)
	}
	return userID
}

// cleanupTestCluster removes a test cluster
func cleanupTestCluster(ctx context.Context, clusterID uuid.UUID) {
	testDB.Pool.Exec(ctx, "DELETE FROM clusters WHERE id = $1", clusterID)
}

// cleanupTestUser removes a test user
func cleanupTestUser(ctx context.Context, userID uuid.UUID) {
	testDB.Pool.Exec(ctx, "DELETE FROM users WHERE id = $1", userID)
}

// TestClusterLifecycle_Create_Get_Delete tests the full lifecycle of a cluster
func TestClusterLifecycle_Create_Get_Delete(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelDebug}))

	// Create test user
	userEmail := "test-lifecycle-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	// Create cluster service
	svc := services.NewClusterService(testDB, logger)

	// Test: Create cluster
	t.Run("CreateCluster", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "test-cluster-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierDev,
			Provider: models.CloudProviderAWS,
			Region:   "us-east-1",
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		if cluster.ID == uuid.Nil {
			t.Error("cluster ID should not be nil")
		}
		if cluster.OwnerID != userID {
			t.Errorf("cluster owner ID mismatch: got %v, want %v", cluster.OwnerID, userID)
		}
		if cluster.Status != models.ClusterStatusPending {
			t.Errorf("cluster status should be pending, got %v", cluster.Status)
		}

		// Test: Get cluster by ID
		t.Run("GetByID", func(t *testing.T) {
			retrieved, err := svc.GetByID(ctx, cluster.ID)
			if err != nil {
				t.Fatalf("failed to get cluster: %v", err)
			}
			if retrieved.Name != cluster.Name {
				t.Errorf("cluster name mismatch: got %v, want %v", retrieved.Name, cluster.Name)
			}
		})

		// Test: Get cluster by name
		t.Run("GetByName", func(t *testing.T) {
			retrieved, err := svc.GetByName(ctx, userID, cluster.Name)
			if err != nil {
				t.Fatalf("failed to get cluster by name: %v", err)
			}
			if retrieved.ID != cluster.ID {
				t.Errorf("cluster ID mismatch: got %v, want %v", retrieved.ID, cluster.ID)
			}
		})

		// Test: List clusters
		t.Run("List", func(t *testing.T) {
			resp, err := svc.List(ctx, userID, 1, 10)
			if err != nil {
				t.Fatalf("failed to list clusters: %v", err)
			}
			if resp.TotalCount < 1 {
				t.Error("expected at least one cluster in list")
			}
			found := false
			for _, c := range resp.Clusters {
				if c.ID == cluster.ID {
					found = true
					break
				}
			}
			if !found {
				t.Error("created cluster not found in list")
			}
		})

		// Test: Delete cluster
		t.Run("Delete", func(t *testing.T) {
			err := svc.Delete(ctx, cluster.ID)
			if err != nil {
				t.Fatalf("failed to delete cluster: %v", err)
			}

			// Verify cluster is marked as deleting/deleted
			retrieved, err := svc.GetByID(ctx, cluster.ID)
			if err == nil && retrieved.Status != models.ClusterStatusDeleting {
				t.Error("cluster should be in deleting status or not found")
			}
		})
	})
}

// TestClusterIsolation tests that clusters are isolated between users
func TestClusterIsolation(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	// Create two test users
	user1Email := "test-isolation-1-" + uuid.New().String()[:8] + "@example.com"
	user2Email := "test-isolation-2-" + uuid.New().String()[:8] + "@example.com"
	user1ID := createTestUser(t, ctx, user1Email)
	user2ID := createTestUser(t, ctx, user2Email)
	defer cleanupTestUser(ctx, user1ID)
	defer cleanupTestUser(ctx, user2ID)

	svc := services.NewClusterService(testDB, logger)

	// User 1 creates a cluster
	req := &models.ClusterCreateRequest{
		Name:     "isolated-cluster-" + uuid.New().String()[:8],
		Tier:     models.ClusterTierDev,
		Provider: models.CloudProviderGCP,
		Region:   "us-central1",
	}

	cluster, err := svc.Create(ctx, user1ID, req)
	if err != nil {
		t.Fatalf("failed to create cluster for user1: %v", err)
	}
	defer cleanupTestCluster(ctx, cluster.ID)

	// Test: User 2 should not see User 1's cluster in their list
	t.Run("IsolatedList", func(t *testing.T) {
		resp, err := svc.List(ctx, user2ID, 1, 100)
		if err != nil {
			t.Fatalf("failed to list clusters for user2: %v", err)
		}
		for _, c := range resp.Clusters {
			if c.ID == cluster.ID {
				t.Error("user2 should not see user1's cluster in list")
			}
		}
	})

	// Test: User 2 can get the cluster by ID (public read) but ownership check fails
	t.Run("OwnershipCheck", func(t *testing.T) {
		_, err := svc.GetByIDWithOwnerCheck(ctx, cluster.ID, user2ID)
		if err != models.ErrForbidden {
			t.Errorf("expected ErrForbidden, got %v", err)
		}
	})

	// Test: User 2 cannot update User 1's cluster
	t.Run("IsolatedUpdate", func(t *testing.T) {
		newName := "hacked-cluster"
		_, err := svc.UpdateWithOwnerCheck(ctx, cluster.ID, user2ID, &models.ClusterUpdateRequest{
			Name: &newName,
		})
		if err != models.ErrForbidden {
			t.Errorf("expected ErrForbidden for update, got %v", err)
		}
	})

	// Test: User 2 cannot delete User 1's cluster
	t.Run("IsolatedDelete", func(t *testing.T) {
		err := svc.DeleteWithOwnerCheck(ctx, cluster.ID, user2ID)
		if err != models.ErrForbidden {
			t.Errorf("expected ErrForbidden for delete, got %v", err)
		}
	})
}

// TestClusterPoolerConfiguration tests that pooler configuration is saved correctly
func TestClusterPoolerConfiguration(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	// Create test user
	userEmail := "test-pooler-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	svc := services.NewClusterService(testDB, logger)

	// Test: Production tier should have pooler enabled by default
	t.Run("ProductionTierPoolerEnabled", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "prod-cluster-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "us-west-2",
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create production cluster: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		if !cluster.PoolerEnabled {
			t.Error("production tier cluster should have pooler enabled by default")
		}
	})

	// Test: Dev tier should not have pooler enabled by default
	t.Run("DevTierPoolerDisabled", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:          "dev-cluster-" + uuid.New().String()[:8],
			Tier:          models.ClusterTierDev,
			Provider:      models.CloudProviderAWS,
			Region:        "us-east-1",
			PoolerEnabled: false, // Explicitly disabled
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create dev cluster: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		if cluster.PoolerEnabled {
			t.Error("dev tier cluster should not have pooler enabled by default")
		}
	})

	// Test: Explicit pooler enable for dev tier
	t.Run("ExplicitPoolerEnable", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:          "dev-pooler-cluster-" + uuid.New().String()[:8],
			Tier:          models.ClusterTierDev,
			Provider:      models.CloudProviderAWS,
			Region:        "eu-west-1",
			PoolerEnabled: true, // Explicitly enabled
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster with pooler: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		if !cluster.PoolerEnabled {
			t.Error("cluster with explicit pooler enable should have pooler enabled")
		}
	})
}

// TestClusterOrganizationIsolation tests that clusters can be isolated by organization
func TestClusterOrganizationIsolation(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	// Create test user
	userEmail := "test-org-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	// Create test organization
	orgID := uuid.New()
	_, err := testDB.Pool.Exec(ctx, `
		INSERT INTO organizations (id, name, slug, owner_id)
		VALUES ($1, 'Test Org', $2, $3)
	`, orgID, "test-org-"+uuid.New().String()[:8], userID)
	if err != nil {
		t.Fatalf("failed to create test organization: %v", err)
	}
	defer func() {
		testDB.Pool.Exec(ctx, "DELETE FROM organizations WHERE id = $1", orgID)
	}()

	svc := services.NewClusterService(testDB, logger)

	// Test: Create cluster with organization
	t.Run("CreateWithOrganization", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:           "org-cluster-" + uuid.New().String()[:8],
			Tier:           models.ClusterTierStarter,
			Provider:       models.CloudProviderAzure,
			Region:         "eastus",
			OrganizationID: &orgID,
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster with organization: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		if cluster.OrganizationID == nil {
			t.Error("cluster should have organization ID set")
		} else if *cluster.OrganizationID != orgID {
			t.Errorf("organization ID mismatch: got %v, want %v", *cluster.OrganizationID, orgID)
		}

		// Verify the cluster can be retrieved
		retrieved, err := svc.GetByID(ctx, cluster.ID)
		if err != nil {
			t.Fatalf("failed to get cluster: %v", err)
		}
		if retrieved.OrganizationID == nil || *retrieved.OrganizationID != orgID {
			t.Error("retrieved cluster should have correct organization ID")
		}
	})
}

// TestClusterTieringConfiguration tests tiering configuration persistence and retrieval
func TestClusterTieringConfiguration(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	// Create test user
	userEmail := "test-tiering-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	svc := services.NewClusterService(testDB, logger)

	t.Run("CreateWithTiering", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "tiered-cluster-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "us-west-2",
			TieringConfig: &models.TieringConfig{
				Enabled:         true,
				HotDuration:     "7d",
				WarmDuration:    "30d",
				ColdDuration:    "90d",
				CompressionType: "zstd",
			},
			S3Config: &models.S3Config{
				Endpoint:        "s3.amazonaws.com",
				Bucket:          "orochi-cold-tier",
				Region:          "us-west-2",
				AccessKeyID:     "AKIAIOSFODNN7EXAMPLE",
				SecretAccessKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
			},
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster with tiering: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		// Verify tiering is enabled
		if !cluster.TieringEnabled {
			t.Error("cluster should have tiering enabled")
		}

		// Verify tiering durations
		if cluster.TieringHotDuration == nil || *cluster.TieringHotDuration != "7d" {
			t.Errorf("hot duration mismatch: got %v, want 7d", cluster.TieringHotDuration)
		}
		if cluster.TieringWarmDuration == nil || *cluster.TieringWarmDuration != "30d" {
			t.Errorf("warm duration mismatch: got %v, want 30d", cluster.TieringWarmDuration)
		}
		if cluster.TieringColdDuration == nil || *cluster.TieringColdDuration != "90d" {
			t.Errorf("cold duration mismatch: got %v, want 90d", cluster.TieringColdDuration)
		}

		// Verify compression type
		if cluster.TieringCompression == nil || *cluster.TieringCompression != "zstd" {
			t.Errorf("compression type mismatch: got %v, want zstd", cluster.TieringCompression)
		}

		// Verify S3 configuration
		if cluster.S3Endpoint == nil || *cluster.S3Endpoint != "s3.amazonaws.com" {
			t.Errorf("S3 endpoint mismatch: got %v, want s3.amazonaws.com", cluster.S3Endpoint)
		}
		if cluster.S3Bucket == nil || *cluster.S3Bucket != "orochi-cold-tier" {
			t.Errorf("S3 bucket mismatch: got %v, want orochi-cold-tier", cluster.S3Bucket)
		}
		if cluster.S3Region == nil || *cluster.S3Region != "us-west-2" {
			t.Errorf("S3 region mismatch: got %v, want us-west-2", cluster.S3Region)
		}

		// Retrieve and verify persistence
		retrieved, err := svc.GetByID(ctx, cluster.ID)
		if err != nil {
			t.Fatalf("failed to retrieve cluster: %v", err)
		}

		if !retrieved.TieringEnabled {
			t.Error("retrieved cluster should have tiering enabled")
		}
		if retrieved.TieringHotDuration == nil || *retrieved.TieringHotDuration != "7d" {
			t.Error("retrieved cluster hot duration mismatch")
		}
		if retrieved.S3Bucket == nil || *retrieved.S3Bucket != "orochi-cold-tier" {
			t.Error("retrieved cluster S3 bucket mismatch")
		}
	})

	t.Run("CreateWithoutTiering", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "no-tiering-cluster-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierDev,
			Provider: models.CloudProviderGCP,
			Region:   "us-central1",
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster without tiering: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		// Verify tiering is disabled
		if cluster.TieringEnabled {
			t.Error("cluster should not have tiering enabled")
		}

		// Verify tiering fields are nil
		if cluster.TieringHotDuration != nil {
			t.Error("hot duration should be nil when tiering is disabled")
		}
		if cluster.TieringWarmDuration != nil {
			t.Error("warm duration should be nil when tiering is disabled")
		}
		if cluster.TieringColdDuration != nil {
			t.Error("cold duration should be nil when tiering is disabled")
		}
		if cluster.S3Endpoint != nil {
			t.Error("S3 endpoint should be nil when tiering is disabled")
		}
	})

	t.Run("CreateWithDefaultCompression", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "default-compression-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "eu-west-1",
			TieringConfig: &models.TieringConfig{
				Enabled:      true,
				HotDuration:  "7d",
				WarmDuration: "30d",
				ColdDuration: "90d",
				// CompressionType not set - should default to zstd
			},
			S3Config: &models.S3Config{
				Endpoint:        "s3.eu-west-1.amazonaws.com",
				Bucket:          "eu-bucket",
				Region:          "eu-west-1",
				AccessKeyID:     "key",
				SecretAccessKey: "secret",
			},
		}

		// Apply defaults
		req.ApplyDefaults()

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster with default compression: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		// Verify compression defaults to zstd
		if cluster.TieringCompression == nil || *cluster.TieringCompression != "zstd" {
			t.Errorf("compression should default to zstd, got %v", cluster.TieringCompression)
		}
	})

	t.Run("CreateWithLZ4Compression", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "lz4-compression-" + uuid.New().String()[:8],
			Tier:     models.ClusterTierEnterprise,
			Provider: models.CloudProviderAzure,
			Region:   "eastus",
			TieringConfig: &models.TieringConfig{
				Enabled:         true,
				HotDuration:     "14d",
				WarmDuration:    "60d",
				ColdDuration:    "180d",
				CompressionType: "lz4",
			},
			S3Config: &models.S3Config{
				Endpoint:        "https://storage.azure.com",
				Bucket:          "azure-cold",
				Region:          "eastus",
				AccessKeyID:     "azure-key",
				SecretAccessKey: "azure-secret",
			},
		}

		cluster, err := svc.Create(ctx, userID, req)
		if err != nil {
			t.Fatalf("failed to create cluster with lz4 compression: %v", err)
		}
		defer cleanupTestCluster(ctx, cluster.ID)

		// Verify lz4 compression
		if cluster.TieringCompression == nil || *cluster.TieringCompression != "lz4" {
			t.Errorf("compression should be lz4, got %v", cluster.TieringCompression)
		}
	})
}

// TestClusterValidationErrors tests that validation errors are returned correctly
func TestClusterValidationErrors(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	userEmail := "test-validation-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	svc := services.NewClusterService(testDB, logger)

	t.Run("TieringEnabledWithoutHotDuration", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "invalid-cluster",
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "us-west-2",
			TieringConfig: &models.TieringConfig{
				Enabled:      true,
				WarmDuration: "30d",
				ColdDuration: "90d",
			},
			S3Config: &models.S3Config{
				Endpoint:        "s3.amazonaws.com",
				Bucket:          "bucket",
				Region:          "us-west-2",
				AccessKeyID:     "key",
				SecretAccessKey: "secret",
			},
		}

		_, err := svc.Create(ctx, userID, req)
		if err != models.ErrTieringHotDurationRequired {
			t.Errorf("expected ErrTieringHotDurationRequired, got %v", err)
		}
	})

	t.Run("TieringEnabledWithoutS3Config", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "invalid-cluster",
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "us-west-2",
			TieringConfig: &models.TieringConfig{
				Enabled:      true,
				HotDuration:  "7d",
				WarmDuration: "30d",
				ColdDuration: "90d",
			},
			S3Config: nil, // Missing S3 config
		}

		_, err := svc.Create(ctx, userID, req)
		if err != models.ErrS3ConfigRequired {
			t.Errorf("expected ErrS3ConfigRequired, got %v", err)
		}
	})

	t.Run("InvalidCompressionType", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:     "invalid-cluster",
			Tier:     models.ClusterTierProduction,
			Provider: models.CloudProviderAWS,
			Region:   "us-west-2",
			TieringConfig: &models.TieringConfig{
				Enabled:         true,
				HotDuration:     "7d",
				WarmDuration:    "30d",
				ColdDuration:    "90d",
				CompressionType: "gzip", // Invalid
			},
			S3Config: &models.S3Config{
				Endpoint:        "s3.amazonaws.com",
				Bucket:          "bucket",
				Region:          "us-west-2",
				AccessKeyID:     "key",
				SecretAccessKey: "secret",
			},
		}

		_, err := svc.Create(ctx, userID, req)
		if err != models.ErrTieringInvalidCompression {
			t.Errorf("expected ErrTieringInvalidCompression, got %v", err)
		}
	})

	t.Run("InvalidShardCount", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:              "invalid-cluster",
			Tier:              models.ClusterTierDev,
			Provider:          models.CloudProviderAWS,
			Region:            "us-east-1",
			DefaultShardCount: 2000, // Too high
		}

		_, err := svc.Create(ctx, userID, req)
		if err != models.ErrShardCountTooHigh {
			t.Errorf("expected ErrShardCountTooHigh, got %v", err)
		}
	})

	t.Run("NegativeShardCount", func(t *testing.T) {
		req := &models.ClusterCreateRequest{
			Name:              "invalid-cluster",
			Tier:              models.ClusterTierDev,
			Provider:          models.CloudProviderAWS,
			Region:            "us-east-1",
			DefaultShardCount: -1,
		}

		_, err := svc.Create(ctx, userID, req)
		if err != models.ErrInvalidShardCount {
			t.Errorf("expected ErrInvalidShardCount, got %v", err)
		}
	})
}

// TestClusterUpdatePreservesTiering tests that update operations preserve tiering config
func TestClusterUpdatePreservesTiering(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	userEmail := "test-update-tiering-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	svc := services.NewClusterService(testDB, logger)

	// Create cluster with tiering
	req := &models.ClusterCreateRequest{
		Name:     "update-test-" + uuid.New().String()[:8],
		Tier:     models.ClusterTierProduction,
		Provider: models.CloudProviderAWS,
		Region:   "us-west-2",
		TieringConfig: &models.TieringConfig{
			Enabled:         true,
			HotDuration:     "7d",
			WarmDuration:    "30d",
			ColdDuration:    "90d",
			CompressionType: "zstd",
		},
		S3Config: &models.S3Config{
			Endpoint:        "s3.amazonaws.com",
			Bucket:          "test-bucket",
			Region:          "us-west-2",
			AccessKeyID:     "key",
			SecretAccessKey: "secret",
		},
	}

	cluster, err := svc.Create(ctx, userID, req)
	if err != nil {
		t.Fatalf("failed to create cluster: %v", err)
	}
	defer cleanupTestCluster(ctx, cluster.ID)

	// Wait for cluster to be in running state
	time.Sleep(100 * time.Millisecond)
	_, err = testDB.Pool.Exec(ctx, "UPDATE clusters SET status = $1 WHERE id = $2",
		models.ClusterStatusRunning, cluster.ID)
	if err != nil {
		t.Fatalf("failed to set cluster to running: %v", err)
	}

	// Update cluster (non-tiering field)
	newName := "updated-name"
	updateReq := &models.ClusterUpdateRequest{
		Name: &newName,
	}

	updated, err := svc.Update(ctx, cluster.ID, updateReq)
	if err != nil {
		t.Fatalf("failed to update cluster: %v", err)
	}

	// Wait for update to complete
	time.Sleep(100 * time.Millisecond)
	_, err = testDB.Pool.Exec(ctx, "UPDATE clusters SET status = $1 WHERE id = $2",
		models.ClusterStatusRunning, cluster.ID)
	if err != nil {
		t.Fatalf("failed to set cluster to running after update: %v", err)
	}

	// Retrieve and verify tiering config is preserved
	retrieved, err := svc.GetByID(ctx, cluster.ID)
	if err != nil {
		t.Fatalf("failed to retrieve cluster after update: %v", err)
	}

	if !retrieved.TieringEnabled {
		t.Error("tiering should still be enabled after update")
	}
	if retrieved.TieringHotDuration == nil || *retrieved.TieringHotDuration != "7d" {
		t.Error("hot duration should be preserved after update")
	}
	if retrieved.S3Bucket == nil || *retrieved.S3Bucket != "test-bucket" {
		t.Error("S3 bucket should be preserved after update")
	}
	if updated.Name != newName {
		t.Errorf("name should be updated, got %v want %v", updated.Name, newName)
	}
}

// TestClusterShardCountConfiguration tests shard count defaults and persistence
func TestClusterShardCountConfiguration(t *testing.T) {
	if testDB == nil {
		t.Skip("Test database not available")
	}

	ctx := context.Background()
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelWarn}))

	userEmail := "test-shards-" + uuid.New().String()[:8] + "@example.com"
	userID := createTestUser(t, ctx, userEmail)
	defer cleanupTestUser(ctx, userID)

	svc := services.NewClusterService(testDB, logger)

	tests := []struct {
		name               string
		tier               models.ClusterTier
		explicitShardCount int
		expectedShardCount int
	}{
		{
			name:               "dev tier default",
			tier:               models.ClusterTierDev,
			explicitShardCount: 0,
			expectedShardCount: 4,
		},
		{
			name:               "production tier default",
			tier:               models.ClusterTierProduction,
			explicitShardCount: 0,
			expectedShardCount: 16,
		},
		{
			name:               "explicit override",
			tier:               models.ClusterTierDev,
			explicitShardCount: 64,
			expectedShardCount: 64,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := &models.ClusterCreateRequest{
				Name:              "shard-test-" + uuid.New().String()[:8],
				Tier:              tt.tier,
				Provider:          models.CloudProviderAWS,
				Region:            "us-east-1",
				DefaultShardCount: tt.explicitShardCount,
			}

			cluster, err := svc.Create(ctx, userID, req)
			if err != nil {
				t.Fatalf("failed to create cluster: %v", err)
			}
			defer cleanupTestCluster(ctx, cluster.ID)

			if cluster.DefaultShardCount != tt.expectedShardCount {
				t.Errorf("shard count mismatch: got %d, want %d",
					cluster.DefaultShardCount, tt.expectedShardCount)
			}

			// Verify persistence
			retrieved, err := svc.GetByID(ctx, cluster.ID)
			if err != nil {
				t.Fatalf("failed to retrieve cluster: %v", err)
			}

			if retrieved.DefaultShardCount != tt.expectedShardCount {
				t.Errorf("retrieved shard count mismatch: got %d, want %d",
					retrieved.DefaultShardCount, tt.expectedShardCount)
			}
		})
	}
}
