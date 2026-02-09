package services

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/clients"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// PoolerService handles pooler-related business logic.
type PoolerService struct {
	db            *db.DB
	clientFactory *clients.PgDogAdminClientFactory
	logger        *slog.Logger
}

// NewPoolerService creates a new pooler service.
func NewPoolerService(db *db.DB, logger *slog.Logger) *PoolerService {
	return &PoolerService{
		db:            db,
		clientFactory: clients.NewPgDogAdminClientFactory(logger),
		logger:        logger.With("service", "pooler"),
	}
}

// GetPoolerStatus retrieves the complete pooler status for a cluster.
func (s *PoolerService) GetPoolerStatus(ctx context.Context, clusterID uuid.UUID) (*models.PoolerStatus, error) {
	s.logger.Debug("getting pooler status", "cluster_id", clusterID)

	// Get cluster to verify it exists and check pooler configuration
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	if !cluster.PoolerEnabled {
		return nil, models.ErrPoolerNotEnabled
	}

	// Get pooler configuration from database
	config, err := s.getPoolerConfig(ctx, clusterID)
	if err != nil {
		s.logger.Warn("failed to get pooler config from database, using defaults", "error", err)
		config = &models.PoolerConfig{}
		config.ApplyDefaults()
	}

	status := &models.PoolerStatus{
		ClusterID:   clusterID.String(),
		Healthy:     true,
		Config:      *config,
		LastUpdated: time.Now(),
	}

	// Get pooler endpoint from cluster
	if cluster.PoolerURL != nil {
		status.Endpoint = *cluster.PoolerURL
		// Admin endpoint is typically on the same host, different path/port
		status.AdminEndpoint = s.getAdminEndpoint(*cluster.PoolerURL)
	}

	// Try to get live stats from PgDog
	if cluster.PoolerURL != nil && *cluster.PoolerURL != "" {
		adminClient := s.clientFactory.CreateForCluster(clusterID.String(), s.extractHost(*cluster.PoolerURL))

		// Check if pooler is reachable
		if err := adminClient.Ping(ctx); err != nil {
			s.logger.Warn("pooler admin interface not reachable", "cluster_id", clusterID, "error", err)
			status.Healthy = false
		} else {
			// Get version
			if version, err := adminClient.GetVersion(ctx); err == nil {
				status.Version = version
			}

			// Get live stats
			if stats, err := adminClient.ShowStats(ctx); err == nil {
				status.Stats = *stats
			} else {
				s.logger.Warn("failed to get pooler stats", "cluster_id", clusterID, "error", err)
			}

			// Get live config (might differ from stored config if manually changed)
			if liveConfig, err := adminClient.ShowConfig(ctx); err == nil {
				// Merge live config with stored config (live takes precedence for runtime values)
				if liveConfig.Mode != "" {
					status.Config.Mode = liveConfig.Mode
				}
				if liveConfig.MaxPoolSize > 0 {
					status.Config.MaxPoolSize = liveConfig.MaxPoolSize
				}
			}
		}
	}

	// Derive replica counts from cluster configuration and health status.
	// The provisioner manages the actual K8s deployments; here we use
	// the cluster's configured node count as the desired replicas.
	status.Replicas = cluster.NodeCount
	if status.Replicas < 1 {
		status.Replicas = 1
	}
	if status.Healthy {
		status.ReadyReplicas = status.Replicas
	} else {
		status.ReadyReplicas = 0
	}

	return status, nil
}

// GetPoolerStats retrieves detailed pooler statistics.
func (s *PoolerService) GetPoolerStats(ctx context.Context, clusterID uuid.UUID) (*models.PoolerStats, error) {
	s.logger.Debug("getting pooler stats", "cluster_id", clusterID)

	// Get cluster to verify it exists and check pooler configuration
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	if !cluster.PoolerEnabled {
		return nil, models.ErrPoolerNotEnabled
	}

	if cluster.PoolerURL == nil || *cluster.PoolerURL == "" {
		return nil, models.ErrPoolerNotReady
	}

	// Create admin client and get stats
	adminClient := s.clientFactory.CreateForCluster(clusterID.String(), s.extractHost(*cluster.PoolerURL))

	stats, err := adminClient.ShowStats(ctx)
	if err != nil {
		s.logger.Error("failed to get pooler stats from PgDog", "cluster_id", clusterID, "error", err)
		return nil, fmt.Errorf("%w: %v", models.ErrPoolerConnectionFailed, err)
	}

	return stats, nil
}

// UpdatePoolerConfig updates the pooler configuration for a cluster.
func (s *PoolerService) UpdatePoolerConfig(ctx context.Context, clusterID uuid.UUID, req *models.PoolerUpdateRequest) (*models.PoolerConfig, error) {
	s.logger.Info("updating pooler config", "cluster_id", clusterID)

	// Validate request
	if err := req.Validate(); err != nil {
		return nil, err
	}

	// Get cluster to verify it exists
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	// Start transaction
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to update pooler config")
	}
	defer tx.Rollback(ctx)

	// Get current config or create default
	config, err := s.getPoolerConfigTx(ctx, tx, clusterID)
	if err != nil {
		config = &models.PoolerConfig{}
		config.ApplyDefaults()
	}

	// Apply updates
	if req.Enabled != nil {
		config.Enabled = *req.Enabled
	}
	if req.Mode != nil {
		config.Mode = *req.Mode
	}
	if req.MaxPoolSize != nil {
		config.MaxPoolSize = *req.MaxPoolSize
	}
	if req.MinPoolSize != nil {
		config.MinPoolSize = *req.MinPoolSize
	}
	if req.IdleTimeout != nil {
		config.IdleTimeout = *req.IdleTimeout
	}
	if req.ConnectTimeout != nil {
		config.ConnectTimeout = *req.ConnectTimeout
	}
	if req.QueryTimeout != nil {
		config.QueryTimeout = *req.QueryTimeout
	}
	if req.ReadWriteSplit != nil {
		config.ReadWriteSplit = *req.ReadWriteSplit
	}
	if req.ShardingEnabled != nil {
		config.ShardingEnabled = *req.ShardingEnabled
	}
	if req.ShardCount != nil {
		config.ShardCount = *req.ShardCount
	}
	if req.LoadBalancing != nil {
		config.LoadBalancing = *req.LoadBalancing
	}
	if req.HealthCheckSec != nil {
		config.HealthCheckSec = *req.HealthCheckSec
	}

	// Validate the complete config
	if err := config.Validate(); err != nil {
		return nil, err
	}

	// Update pooler_enabled on cluster if changing enabled state
	if req.Enabled != nil {
		updateClusterQuery := `
			UPDATE clusters SET pooler_enabled = $2, updated_at = NOW()
			WHERE id = $1
		`
		if _, err := tx.Exec(ctx, updateClusterQuery, clusterID, *req.Enabled); err != nil {
			s.logger.Error("failed to update cluster pooler_enabled", "error", err)
			return nil, errors.New("failed to update pooler config")
		}
	}

	// Store config as JSON in a settings table (upsert)
	configJSON, err := json.Marshal(config)
	if err != nil {
		s.logger.Error("failed to marshal pooler config", "error", err)
		return nil, errors.New("failed to update pooler config")
	}

	upsertQuery := `
		INSERT INTO cluster_pooler_config (cluster_id, config, updated_at)
		VALUES ($1, $2, NOW())
		ON CONFLICT (cluster_id) DO UPDATE
		SET config = $2, updated_at = NOW()
	`
	if _, err := tx.Exec(ctx, upsertQuery, clusterID, configJSON); err != nil {
		// Table might not exist, try to create it
		if strings.Contains(err.Error(), "does not exist") {
			if err := s.ensurePoolerConfigTable(ctx, tx); err != nil {
				s.logger.Error("failed to create pooler config table", "error", err)
				return nil, errors.New("failed to update pooler config")
			}
			// Retry upsert
			if _, err := tx.Exec(ctx, upsertQuery, clusterID, configJSON); err != nil {
				s.logger.Error("failed to upsert pooler config", "error", err)
				return nil, errors.New("failed to update pooler config")
			}
		} else {
			s.logger.Error("failed to upsert pooler config", "error", err)
			return nil, errors.New("failed to update pooler config")
		}
	}

	// Commit transaction
	if err := tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to update pooler config")
	}

	s.logger.Info("pooler config updated",
		"cluster_id", clusterID,
		"mode", config.Mode,
		"max_pool_size", config.MaxPoolSize,
	)

	// If pooler is running, trigger a config reload
	if cluster.PoolerURL != nil && *cluster.PoolerURL != "" && config.Enabled {
		go func() {
			reloadCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			if err := s.ReloadPoolerConfig(reloadCtx, clusterID); err != nil {
				s.logger.Warn("failed to reload pooler config after update", "cluster_id", clusterID, "error", err)
			}
		}()
	}

	return config, nil
}

// ReloadPoolerConfig triggers a configuration reload on the PgDog pooler.
func (s *PoolerService) ReloadPoolerConfig(ctx context.Context, clusterID uuid.UUID) error {
	s.logger.Info("reloading pooler config", "cluster_id", clusterID)

	// Get cluster to verify it exists and get pooler URL
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return err
	}

	if !cluster.PoolerEnabled {
		return models.ErrPoolerNotEnabled
	}

	if cluster.PoolerURL == nil || *cluster.PoolerURL == "" {
		return models.ErrPoolerNotReady
	}

	// Create admin client and send reload command
	adminClient := s.clientFactory.CreateForCluster(clusterID.String(), s.extractHost(*cluster.PoolerURL))

	if err := adminClient.Reload(ctx); err != nil {
		s.logger.Error("failed to reload pooler config", "cluster_id", clusterID, "error", err)
		return fmt.Errorf("%w: %v", models.ErrPoolerReloadFailed, err)
	}

	s.logger.Info("pooler config reloaded successfully", "cluster_id", clusterID)
	return nil
}

// GetPoolerClients retrieves connected clients from the pooler.
func (s *PoolerService) GetPoolerClients(ctx context.Context, clusterID uuid.UUID) ([]models.ClientInfo, error) {
	s.logger.Debug("getting pooler clients", "cluster_id", clusterID)

	// Get cluster to verify it exists and check pooler configuration
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	if !cluster.PoolerEnabled {
		return nil, models.ErrPoolerNotEnabled
	}

	if cluster.PoolerURL == nil || *cluster.PoolerURL == "" {
		return nil, models.ErrPoolerNotReady
	}

	// Create admin client and get clients
	adminClient := s.clientFactory.CreateForCluster(clusterID.String(), s.extractHost(*cluster.PoolerURL))

	clients, err := adminClient.ShowClients(ctx)
	if err != nil {
		s.logger.Error("failed to get pooler clients from PgDog", "cluster_id", clusterID, "error", err)
		return nil, fmt.Errorf("%w: %v", models.ErrPoolerConnectionFailed, err)
	}

	return clients, nil
}

// GetPoolerPools retrieves pool information from the pooler.
func (s *PoolerService) GetPoolerPools(ctx context.Context, clusterID uuid.UUID) ([]models.PoolInfo, error) {
	s.logger.Debug("getting pooler pools", "cluster_id", clusterID)

	// Get cluster to verify it exists and check pooler configuration
	cluster, err := s.getCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	if !cluster.PoolerEnabled {
		return nil, models.ErrPoolerNotEnabled
	}

	if cluster.PoolerURL == nil || *cluster.PoolerURL == "" {
		return nil, models.ErrPoolerNotReady
	}

	// Create admin client and get pools
	adminClient := s.clientFactory.CreateForCluster(clusterID.String(), s.extractHost(*cluster.PoolerURL))

	pools, err := adminClient.ShowPools(ctx)
	if err != nil {
		s.logger.Error("failed to get pooler pools from PgDog", "cluster_id", clusterID, "error", err)
		return nil, fmt.Errorf("%w: %v", models.ErrPoolerConnectionFailed, err)
	}

	return pools, nil
}

// getCluster retrieves a cluster by ID.
func (s *PoolerService) getCluster(ctx context.Context, clusterID uuid.UUID) (*models.Cluster, error) {
	query := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
	`

	cluster := &models.Cluster{}
	err := s.db.Pool.QueryRow(ctx, query, clusterID).Scan(
		&cluster.ID, &cluster.Name, &cluster.OwnerID, &cluster.OrganizationID, &cluster.Status,
		&cluster.Tier, &cluster.Provider, &cluster.Region, &cluster.Version,
		&cluster.NodeCount, &cluster.NodeSize, &cluster.StorageGB, &cluster.ConnectionURL, &cluster.PoolerURL,
		&cluster.MaintenanceDay, &cluster.MaintenanceHour,
		&cluster.BackupEnabled, &cluster.BackupRetention, &cluster.PoolerEnabled,
		&cluster.TieringEnabled, &cluster.TieringHotDuration, &cluster.TieringWarmDuration,
		&cluster.TieringColdDuration, &cluster.TieringCompression,
		&cluster.S3Endpoint, &cluster.S3Bucket, &cluster.S3Region,
		&cluster.EnableColumnar, &cluster.DefaultShardCount,
		&cluster.CreatedAt, &cluster.UpdatedAt, &cluster.DeletedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster", "error", err, "cluster_id", clusterID)
		return nil, errors.New("failed to retrieve cluster")
	}

	return cluster, nil
}

// getPoolerConfig retrieves pooler config from the database.
func (s *PoolerService) getPoolerConfig(ctx context.Context, clusterID uuid.UUID) (*models.PoolerConfig, error) {
	query := `
		SELECT config FROM cluster_pooler_config WHERE cluster_id = $1
	`

	var configJSON []byte
	err := s.db.Pool.QueryRow(ctx, query, clusterID).Scan(&configJSON)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			// Return default config if none exists
			config := &models.PoolerConfig{Enabled: true}
			config.ApplyDefaults()
			return config, nil
		}
		return nil, err
	}

	var config models.PoolerConfig
	if err := json.Unmarshal(configJSON, &config); err != nil {
		return nil, err
	}

	return &config, nil
}

// getPoolerConfigTx retrieves pooler config within a transaction.
func (s *PoolerService) getPoolerConfigTx(ctx context.Context, tx pgx.Tx, clusterID uuid.UUID) (*models.PoolerConfig, error) {
	query := `
		SELECT config FROM cluster_pooler_config WHERE cluster_id = $1
	`

	var configJSON []byte
	err := tx.QueryRow(ctx, query, clusterID).Scan(&configJSON)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, err
		}
		return nil, err
	}

	var config models.PoolerConfig
	if err := json.Unmarshal(configJSON, &config); err != nil {
		return nil, err
	}

	return &config, nil
}

// ensurePoolerConfigTable creates the pooler config table if it doesn't exist.
func (s *PoolerService) ensurePoolerConfigTable(ctx context.Context, tx pgx.Tx) error {
	createTableQuery := `
		CREATE TABLE IF NOT EXISTS cluster_pooler_config (
			cluster_id UUID PRIMARY KEY REFERENCES clusters(id) ON DELETE CASCADE,
			config JSONB NOT NULL,
			created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
			updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
		)
	`
	_, err := tx.Exec(ctx, createTableQuery)
	return err
}

// extractHost extracts the host from a PostgreSQL connection URL.
func (s *PoolerService) extractHost(poolerURL string) string {
	// Expected format: postgresql://user:pass@host:port/db
	// We need to extract host:port for the admin connection

	// Remove protocol prefix
	url := poolerURL
	if idx := strings.Index(url, "://"); idx >= 0 {
		url = url[idx+3:]
	}

	// Remove credentials
	if idx := strings.Index(url, "@"); idx >= 0 {
		url = url[idx+1:]
	}

	// Remove database and query params
	if idx := strings.Index(url, "/"); idx >= 0 {
		url = url[:idx]
	}

	return url
}

// getAdminEndpoint derives the admin endpoint from the pooler URL.
func (s *PoolerService) getAdminEndpoint(poolerURL string) string {
	host := s.extractHost(poolerURL)
	// Admin typically runs on the same host
	return fmt.Sprintf("pgdog://%s/admin", host)
}
