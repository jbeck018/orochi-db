package services

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	pb "github.com/orochi-db/orochi-db/services/control-plane/api/proto"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/provisioner"
)

// ClusterService handles cluster-related business logic.
type ClusterService struct {
	db          *db.DB
	logger      *slog.Logger
	provisioner *provisioner.Client
}

// NewClusterService creates a new cluster service.
func NewClusterService(db *db.DB, logger *slog.Logger, provisionerClient *provisioner.Client) *ClusterService {
	return &ClusterService{
		db:          db,
		logger:      logger.With("service", "cluster"),
		provisioner: provisionerClient,
	}
}

// Create creates a new cluster.
// Uses INSERT ... ON CONFLICT for atomic check-and-insert to prevent TOCTOU race conditions.
func (s *ClusterService) Create(ctx context.Context, ownerID uuid.UUID, req *models.ClusterCreateRequest) (*models.Cluster, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	req.ApplyDefaults()

	cluster := &models.Cluster{
		ID:              uuid.New(),
		Name:            req.Name,
		OwnerID:         ownerID,
		OrganizationID:  req.OrganizationID, // Optional team/organization for isolation
		Status:          models.ClusterStatusPending,
		Tier:            req.Tier,
		Provider:        req.Provider,
		Region:          req.Region,
		Version:         req.Version,
		NodeCount:       req.NodeCount,
		NodeSize:        req.NodeSize,
		StorageGB:       req.StorageGB,
		MaintenanceDay:  req.MaintenanceDay,
		MaintenanceHour: req.MaintenanceHour,
		BackupEnabled:   req.BackupEnabled,
		BackupRetention: req.BackupRetention,
		PoolerEnabled:   req.PoolerEnabled, // PgBouncer connection pooling
		EnableColumnar:  req.EnableColumnar,
		DefaultShardCount: req.DefaultShardCount,
		CreatedAt:       time.Now(),
		UpdatedAt:       time.Now(),
	}

	// Apply tiering configuration if provided
	if req.TieringConfig != nil && req.TieringConfig.Enabled {
		cluster.TieringEnabled = true
		cluster.TieringHotDuration = &req.TieringConfig.HotDuration
		cluster.TieringWarmDuration = &req.TieringConfig.WarmDuration
		cluster.TieringColdDuration = &req.TieringConfig.ColdDuration
		cluster.TieringCompression = &req.TieringConfig.CompressionType

		// Apply S3 configuration for cold/frozen tiers
		if req.S3Config != nil {
			cluster.S3Endpoint = &req.S3Config.Endpoint
			cluster.S3Bucket = &req.S3Config.Bucket
			cluster.S3Region = &req.S3Config.Region
			// Note: S3 credentials are passed to provisioner via gRPC
			// and stored in Kubernetes secrets, not in database
		}
	}

	// Atomic insert with conflict detection on (owner_id, name) unique constraint.
	// This prevents TOCTOU race conditions where two concurrent requests could both
	// pass the existence check and then both try to insert.
	query := `
		INSERT INTO clusters (
			id, name, owner_id, organization_id, status, tier, provider, region, version,
			node_count, node_size, storage_gb, maintenance_day, maintenance_hour,
			backup_enabled, backup_retention_days, pooler_enabled,
			tiering_enabled, tiering_hot_duration, tiering_warm_duration, tiering_cold_duration,
			tiering_compression, s3_endpoint, s3_bucket, s3_region,
			enable_columnar, default_shard_count, created_at, updated_at
		)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26, $27, $28, $29)
		ON CONFLICT (owner_id, name) WHERE deleted_at IS NULL DO NOTHING
		RETURNING id
	`

	var insertedID uuid.UUID
	err := s.db.Pool.QueryRow(ctx, query,
		cluster.ID, cluster.Name, cluster.OwnerID, cluster.OrganizationID, cluster.Status,
		cluster.Tier, cluster.Provider, cluster.Region, cluster.Version,
		cluster.NodeCount, cluster.NodeSize, cluster.StorageGB,
		cluster.MaintenanceDay, cluster.MaintenanceHour,
		cluster.BackupEnabled, cluster.BackupRetention, cluster.PoolerEnabled,
		cluster.TieringEnabled, cluster.TieringHotDuration, cluster.TieringWarmDuration,
		cluster.TieringColdDuration, cluster.TieringCompression,
		cluster.S3Endpoint, cluster.S3Bucket, cluster.S3Region,
		cluster.EnableColumnar, cluster.DefaultShardCount,
		cluster.CreatedAt, cluster.UpdatedAt,
	).Scan(&insertedID)

	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			// ON CONFLICT DO NOTHING returned no rows - cluster already exists
			return nil, models.ErrClusterAlreadyExists
		}
		s.logger.Error("failed to create cluster", "error", err)
		return nil, errors.New("failed to create cluster")
	}

	s.logger.Info("cluster created",
		"cluster_id", cluster.ID,
		"name", cluster.Name,
		"owner_id", ownerID,
		"tiering_enabled", cluster.TieringEnabled,
		"columnar_enabled", cluster.EnableColumnar,
		"shard_count", cluster.DefaultShardCount,
	)

	// In production, this would trigger the provisioning workflow
	// Tiering configuration (hot/warm/cold durations, compression) is passed to provisioner
	// S3 credentials from req.S3Config are passed to provisioner via gRPC and stored in Kubernetes secrets
	// Use a derived context with timeout for the background operation (provisioning takes ~15s simulated)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()
		s.startProvisioning(ctx, cluster.ID)
	}()

	return cluster, nil
}

// GetByID retrieves a cluster by ID.
func (s *ClusterService) GetByID(ctx context.Context, id uuid.UUID) (*models.Cluster, error) {
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
	err := s.db.Pool.QueryRow(ctx, query, id).Scan(
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
		s.logger.Error("failed to get cluster", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	return cluster, nil
}

// GetByName retrieves a cluster by owner and name.
func (s *ClusterService) GetByName(ctx context.Context, ownerID uuid.UUID, name string) (*models.Cluster, error) {
	query := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE owner_id = $1 AND name = $2 AND deleted_at IS NULL
	`

	cluster := &models.Cluster{}
	err := s.db.Pool.QueryRow(ctx, query, ownerID, name).Scan(
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
		s.logger.Error("failed to get cluster by name", "error", err)
		return nil, errors.New("failed to retrieve cluster")
	}

	return cluster, nil
}

// List retrieves all clusters for an owner with pagination.
func (s *ClusterService) List(ctx context.Context, ownerID uuid.UUID, page, pageSize int) (*models.ClusterListResponse, error) {
	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}

	offset := (page - 1) * pageSize

	// Get total count
	countQuery := `
		SELECT COUNT(*)
		FROM clusters
		WHERE owner_id = $1 AND deleted_at IS NULL
	`
	var totalCount int
	if err := s.db.Pool.QueryRow(ctx, countQuery, ownerID).Scan(&totalCount); err != nil {
		s.logger.Error("failed to count clusters", "error", err)
		return nil, errors.New("failed to list clusters")
	}

	// Get clusters
	query := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE owner_id = $1 AND deleted_at IS NULL
		ORDER BY created_at DESC
		LIMIT $2 OFFSET $3
	`

	rows, err := s.db.Pool.Query(ctx, query, ownerID, pageSize, offset)
	if err != nil {
		s.logger.Error("failed to list clusters", "error", err)
		return nil, errors.New("failed to list clusters")
	}
	defer rows.Close()

	clusters := make([]*models.Cluster, 0)
	for rows.Next() {
		cluster := &models.Cluster{}
		err := rows.Scan(
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
			s.logger.Error("failed to scan cluster", "error", err)
			continue
		}
		clusters = append(clusters, cluster)
	}

	return &models.ClusterListResponse{
		Clusters:   clusters,
		TotalCount: totalCount,
		Page:       page,
		PageSize:   pageSize,
	}, nil
}

// Update updates a cluster's configuration.
// Uses a transaction with SELECT ... FOR UPDATE to prevent TOCTOU race conditions.
func (s *ClusterService) Update(ctx context.Context, id uuid.UUID, req *models.ClusterUpdateRequest) (*models.Cluster, error) {
	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to update cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	cluster := &models.Cluster{}
	err = tx.QueryRow(ctx, selectQuery, id).Scan(
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
		s.logger.Error("failed to get cluster for update", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	// Check if cluster is in a state that allows updates
	if cluster.Status != models.ClusterStatusRunning && cluster.Status != models.ClusterStatusStopped {
		return nil, models.ErrClusterOperationPending
	}

	// Apply updates
	if req.Name != nil {
		cluster.Name = *req.Name
	}
	if req.NodeSize != nil {
		cluster.NodeSize = *req.NodeSize
	}
	if req.StorageGB != nil {
		if *req.StorageGB < cluster.StorageGB {
			return nil, errors.New("storage cannot be decreased")
		}
		cluster.StorageGB = *req.StorageGB
	}
	if req.MaintenanceDay != nil {
		cluster.MaintenanceDay = *req.MaintenanceDay
	}
	if req.MaintenanceHour != nil {
		cluster.MaintenanceHour = *req.MaintenanceHour
	}
	if req.BackupEnabled != nil {
		cluster.BackupEnabled = *req.BackupEnabled
	}
	if req.BackupRetention != nil {
		cluster.BackupRetention = *req.BackupRetention
	}

	cluster.Status = models.ClusterStatusUpdating
	cluster.UpdatedAt = time.Now()

	updateQuery := `
		UPDATE clusters
		SET name = $2, node_size = $3, storage_gb = $4, maintenance_day = $5,
			maintenance_hour = $6, backup_enabled = $7, backup_retention_days = $8,
			status = $9, updated_at = $10
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery,
		cluster.ID, cluster.Name, cluster.NodeSize, cluster.StorageGB,
		cluster.MaintenanceDay, cluster.MaintenanceHour,
		cluster.BackupEnabled, cluster.BackupRetention,
		cluster.Status, cluster.UpdatedAt,
	)
	if err != nil {
		s.logger.Error("failed to update cluster", "error", err)
		return nil, errors.New("failed to update cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to update cluster")
	}

	s.logger.Info("cluster updated", "cluster_id", id)

	// In production, this would trigger the update workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
		defer cancel()
		s.applyUpdate(ctx, cluster.ID)
	}()

	return cluster, nil
}

// Delete soft-deletes a cluster.
// Uses a transaction with SELECT ... FOR UPDATE to prevent TOCTOU race conditions.
func (s *ClusterService) Delete(ctx context.Context, id uuid.UUID) error {
	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return errors.New("failed to delete cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, status
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	var clusterID uuid.UUID
	var status models.ClusterStatus
	err = tx.QueryRow(ctx, selectQuery, id).Scan(&clusterID, &status)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster for deletion", "error", err, "cluster_id", id)
		return errors.New("failed to delete cluster")
	}

	// Check if cluster is already deleting
	if status == models.ClusterStatusDeleting {
		// Already deleting, no action needed - commit to release lock
		if err = tx.Commit(ctx); err != nil {
			s.logger.Error("failed to commit transaction", "error", err)
		}
		return nil
	}

	now := time.Now()
	updateQuery := `
		UPDATE clusters
		SET status = $2, deleted_at = $3, updated_at = $3
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery, id, models.ClusterStatusDeleting, now)
	if err != nil {
		s.logger.Error("failed to delete cluster", "error", err)
		return errors.New("failed to delete cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return errors.New("failed to delete cluster")
	}

	s.logger.Info("cluster deletion initiated", "cluster_id", id)

	// In production, this would trigger the deprovisioning workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()
		s.startDeprovisioning(ctx, id)
	}()

	return nil
}

// Scale scales a cluster's compute resources.
// Uses a transaction with SELECT ... FOR UPDATE to prevent TOCTOU race conditions.
func (s *ClusterService) Scale(ctx context.Context, id uuid.UUID, req *models.ClusterScaleRequest) (*models.Cluster, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to scale cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	cluster := &models.Cluster{}
	err = tx.QueryRow(ctx, selectQuery, id).Scan(
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
		s.logger.Error("failed to get cluster for scaling", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	if cluster.Status != models.ClusterStatusRunning {
		return nil, models.ErrClusterNotRunning
	}

	cluster.NodeCount = req.NodeCount
	if req.NodeSize != "" {
		cluster.NodeSize = req.NodeSize
	}
	cluster.Status = models.ClusterStatusScaling
	cluster.UpdatedAt = time.Now()

	updateQuery := `
		UPDATE clusters
		SET node_count = $2, node_size = $3, status = $4, updated_at = $5
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery,
		cluster.ID, cluster.NodeCount, cluster.NodeSize,
		cluster.Status, cluster.UpdatedAt,
	)
	if err != nil {
		s.logger.Error("failed to scale cluster", "error", err)
		return nil, errors.New("failed to scale cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to scale cluster")
	}

	s.logger.Info("cluster scaling initiated",
		"cluster_id", id,
		"node_count", req.NodeCount,
		"node_size", cluster.NodeSize,
	)

	// In production, this would trigger the scaling workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()
		s.performScaling(ctx, cluster.ID)
	}()

	return cluster, nil
}

// GetMetrics retrieves metrics for a cluster.
func (s *ClusterService) GetMetrics(ctx context.Context, clusterID uuid.UUID, from, to time.Time) ([]*models.ClusterMetrics, error) {
	// Verify cluster exists
	if _, err := s.GetByID(ctx, clusterID); err != nil {
		return nil, err
	}

	query := `
		SELECT cluster_id, cpu_usage, memory_usage, storage_usage,
			   connection_count, queries_per_sec, reads_per_sec, writes_per_sec,
			   replication_lag_ms, timestamp
		FROM cluster_metrics
		WHERE cluster_id = $1 AND timestamp >= $2 AND timestamp <= $3
		ORDER BY timestamp DESC
		LIMIT 1000
	`

	rows, err := s.db.Pool.Query(ctx, query, clusterID, from, to)
	if err != nil {
		s.logger.Error("failed to get metrics", "error", err)
		return nil, errors.New("failed to retrieve metrics")
	}
	defer rows.Close()

	metrics := make([]*models.ClusterMetrics, 0)
	for rows.Next() {
		m := &models.ClusterMetrics{}
		err := rows.Scan(
			&m.ClusterID, &m.CPUUsage, &m.MemoryUsage, &m.StorageUsage,
			&m.ConnectionCount, &m.QueriesPerSec, &m.ReadsPerSec, &m.WritesPerSec,
			&m.ReplicationLag, &m.Timestamp,
		)
		if err != nil {
			s.logger.Error("failed to scan metrics", "error", err)
			continue
		}
		metrics = append(metrics, m)
	}

	return metrics, nil
}

// GetLatestMetrics retrieves the most recent metrics for a cluster.
func (s *ClusterService) GetLatestMetrics(ctx context.Context, clusterID uuid.UUID) (*models.ClusterMetrics, error) {
	// Verify cluster exists
	if _, err := s.GetByID(ctx, clusterID); err != nil {
		return nil, err
	}

	query := `
		SELECT cluster_id, cpu_usage, memory_usage, storage_usage,
			   connection_count, queries_per_sec, reads_per_sec, writes_per_sec,
			   replication_lag_ms, timestamp
		FROM cluster_metrics
		WHERE cluster_id = $1
		ORDER BY timestamp DESC
		LIMIT 1
	`

	m := &models.ClusterMetrics{}
	err := s.db.Pool.QueryRow(ctx, query, clusterID).Scan(
		&m.ClusterID, &m.CPUUsage, &m.MemoryUsage, &m.StorageUsage,
		&m.ConnectionCount, &m.QueriesPerSec, &m.ReadsPerSec, &m.WritesPerSec,
		&m.ReplicationLag, &m.Timestamp,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			// Return empty metrics if none exist
			return &models.ClusterMetrics{
				ClusterID: clusterID,
				Timestamp: time.Now(),
			}, nil
		}
		s.logger.Error("failed to get latest metrics", "error", err)
		return nil, errors.New("failed to retrieve metrics")
	}

	return m, nil
}

// CheckOwnership verifies that a user owns a cluster.
func (s *ClusterService) CheckOwnership(ctx context.Context, clusterID, userID uuid.UUID) error {
	cluster, err := s.GetByID(ctx, clusterID)
	if err != nil {
		return err
	}

	if cluster.OwnerID != userID {
		return models.ErrForbidden
	}

	return nil
}

// GetByIDWithOwnerCheck retrieves a cluster by ID and verifies ownership in a single query.
// This avoids the N+1 query problem of calling CheckOwnership then GetByID separately.
func (s *ClusterService) GetByIDWithOwnerCheck(ctx context.Context, clusterID, userID uuid.UUID) (*models.Cluster, error) {
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

	if cluster.OwnerID != userID {
		return nil, models.ErrForbidden
	}

	return cluster, nil
}

// UpdateWithOwnerCheck updates a cluster's configuration with ownership verification.
// This avoids the N+1 query problem by checking ownership within the same transaction.
func (s *ClusterService) UpdateWithOwnerCheck(ctx context.Context, id, userID uuid.UUID, req *models.ClusterUpdateRequest) (*models.Cluster, error) {
	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to update cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	cluster := &models.Cluster{}
	err = tx.QueryRow(ctx, selectQuery, id).Scan(
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
		s.logger.Error("failed to get cluster for update", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	// Check ownership
	if cluster.OwnerID != userID {
		return nil, models.ErrForbidden
	}

	// Check if cluster is in a state that allows updates
	if cluster.Status != models.ClusterStatusRunning && cluster.Status != models.ClusterStatusStopped {
		return nil, models.ErrClusterOperationPending
	}

	// Apply updates
	if req.Name != nil {
		cluster.Name = *req.Name
	}
	if req.NodeSize != nil {
		cluster.NodeSize = *req.NodeSize
	}
	if req.StorageGB != nil {
		if *req.StorageGB < cluster.StorageGB {
			return nil, errors.New("storage cannot be decreased")
		}
		cluster.StorageGB = *req.StorageGB
	}
	if req.MaintenanceDay != nil {
		cluster.MaintenanceDay = *req.MaintenanceDay
	}
	if req.MaintenanceHour != nil {
		cluster.MaintenanceHour = *req.MaintenanceHour
	}
	if req.BackupEnabled != nil {
		cluster.BackupEnabled = *req.BackupEnabled
	}
	if req.BackupRetention != nil {
		cluster.BackupRetention = *req.BackupRetention
	}

	cluster.Status = models.ClusterStatusUpdating
	cluster.UpdatedAt = time.Now()

	updateQuery := `
		UPDATE clusters
		SET name = $2, node_size = $3, storage_gb = $4, maintenance_day = $5,
			maintenance_hour = $6, backup_enabled = $7, backup_retention_days = $8,
			status = $9, updated_at = $10
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery,
		cluster.ID, cluster.Name, cluster.NodeSize, cluster.StorageGB,
		cluster.MaintenanceDay, cluster.MaintenanceHour,
		cluster.BackupEnabled, cluster.BackupRetention,
		cluster.Status, cluster.UpdatedAt,
	)
	if err != nil {
		s.logger.Error("failed to update cluster", "error", err)
		return nil, errors.New("failed to update cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to update cluster")
	}

	s.logger.Info("cluster updated", "cluster_id", id)

	// In production, this would trigger the update workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
		defer cancel()
		s.applyUpdate(ctx, cluster.ID)
	}()

	return cluster, nil
}

// DeleteWithOwnerCheck soft-deletes a cluster with ownership verification.
// This avoids the N+1 query problem by checking ownership within the same transaction.
func (s *ClusterService) DeleteWithOwnerCheck(ctx context.Context, id, userID uuid.UUID) error {
	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return errors.New("failed to delete cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, owner_id, status
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	var clusterID uuid.UUID
	var ownerID uuid.UUID
	var status models.ClusterStatus
	err = tx.QueryRow(ctx, selectQuery, id).Scan(&clusterID, &ownerID, &status)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster for deletion", "error", err, "cluster_id", id)
		return errors.New("failed to delete cluster")
	}

	// Check ownership
	if ownerID != userID {
		return models.ErrForbidden
	}

	// Check if cluster is already deleting
	if status == models.ClusterStatusDeleting {
		// Already deleting, no action needed - commit to release lock
		if err = tx.Commit(ctx); err != nil {
			s.logger.Error("failed to commit transaction", "error", err)
		}
		return nil
	}

	now := time.Now()
	updateQuery := `
		UPDATE clusters
		SET status = $2, deleted_at = $3, updated_at = $3
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery, id, models.ClusterStatusDeleting, now)
	if err != nil {
		s.logger.Error("failed to delete cluster", "error", err)
		return errors.New("failed to delete cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return errors.New("failed to delete cluster")
	}

	s.logger.Info("cluster deletion initiated", "cluster_id", id)

	// In production, this would trigger the deprovisioning workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()
		s.startDeprovisioning(ctx, id)
	}()

	return nil
}

// ScaleWithOwnerCheck scales a cluster's compute resources with ownership verification.
// This avoids the N+1 query problem by checking ownership within the same transaction.
func (s *ClusterService) ScaleWithOwnerCheck(ctx context.Context, id, userID uuid.UUID, req *models.ClusterScaleRequest) (*models.Cluster, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to scale cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, name, owner_id, organization_id, status, tier, provider, region, version,
			   node_count, node_size, storage_gb, connection_url, pooler_url,
			   maintenance_day, maintenance_hour, backup_enabled, backup_retention_days,
			   pooler_enabled, tiering_enabled, tiering_hot_duration, tiering_warm_duration,
			   tiering_cold_duration, tiering_compression, s3_endpoint, s3_bucket, s3_region,
			   enable_columnar, default_shard_count, created_at, updated_at, deleted_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	cluster := &models.Cluster{}
	err = tx.QueryRow(ctx, selectQuery, id).Scan(
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
		s.logger.Error("failed to get cluster for scaling", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	// Check ownership
	if cluster.OwnerID != userID {
		return nil, models.ErrForbidden
	}

	if cluster.Status != models.ClusterStatusRunning {
		return nil, models.ErrClusterNotRunning
	}

	cluster.NodeCount = req.NodeCount
	if req.NodeSize != "" {
		cluster.NodeSize = req.NodeSize
	}
	cluster.Status = models.ClusterStatusScaling
	cluster.UpdatedAt = time.Now()

	updateQuery := `
		UPDATE clusters
		SET node_count = $2, node_size = $3, status = $4, updated_at = $5
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery,
		cluster.ID, cluster.NodeCount, cluster.NodeSize,
		cluster.Status, cluster.UpdatedAt,
	)
	if err != nil {
		s.logger.Error("failed to scale cluster", "error", err)
		return nil, errors.New("failed to scale cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to scale cluster")
	}

	s.logger.Info("cluster scaling initiated",
		"cluster_id", id,
		"node_count", req.NodeCount,
		"node_size", cluster.NodeSize,
	)

	// In production, this would trigger the scaling workflow
	// Use a derived context with timeout for the background operation
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()
		s.performScaling(ctx, cluster.ID)
	}()

	return cluster, nil
}

// startProvisioning initiates the provisioning process.
// Uses the real provisioner gRPC service when enabled, otherwise falls back to simulation.
func (s *ClusterService) startProvisioning(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster provisioning", "cluster_id", clusterID)

	// Get cluster details for provisioning
	cluster, err := s.GetByID(ctx, clusterID)
	if err != nil {
		s.logger.Error("failed to get cluster for provisioning", "cluster_id", clusterID, "error", err)
		if updateErr := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", updateErr)
		}
		return
	}

	// Update status to provisioning
	if err := s.updateStatus(ctx, clusterID, models.ClusterStatusProvisioning); err != nil {
		s.logger.Error("failed to update status to provisioning", "cluster_id", clusterID, "error", err)
		return
	}

	// Use real provisioner if enabled
	if s.provisioner != nil && s.provisioner.IsEnabled() {
		s.provisionWithRealProvisioner(ctx, cluster)
		return
	}

	// Fallback to simulated provisioning
	s.provisionSimulated(ctx, cluster)
}

// provisionWithRealProvisioner uses the gRPC provisioner service.
func (s *ClusterService) provisionWithRealProvisioner(ctx context.Context, cluster *models.Cluster) {
	s.logger.Info("using real provisioner for cluster", "cluster_id", cluster.ID, "name", cluster.Name)

	// Convert cluster model to proto spec
	spec := s.clusterToProtoSpec(cluster)

	req := &pb.CreateClusterRequest{
		Spec:         spec,
		WaitForReady: false, // We'll poll for status
		TimeoutSeconds: 600, // 10 minute timeout
	}

	resp, err := s.provisioner.CreateCluster(ctx, req)
	if err != nil {
		s.logger.Error("provisioner failed to create cluster", "cluster_id", cluster.ID, "error", err)
		if updateErr := s.updateStatus(context.Background(), cluster.ID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", cluster.ID, "error", updateErr)
		}
		return
	}

	s.logger.Info("provisioner accepted cluster creation",
		"cluster_id", cluster.ID,
		"provisioner_cluster_id", resp.ClusterId,
		"phase", resp.Phase.String(),
	)

	// Start background polling for cluster status
	go s.pollProvisionerStatus(context.Background(), cluster.ID, resp.ClusterId)
}

// pollProvisionerStatus polls the provisioner for cluster status updates.
func (s *ClusterService) pollProvisionerStatus(ctx context.Context, clusterID uuid.UUID, provisionerClusterID string) {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	timeout := time.After(15 * time.Minute)

	for {
		select {
		case <-ctx.Done():
			s.logger.Warn("status polling cancelled", "cluster_id", clusterID)
			return
		case <-timeout:
			s.logger.Error("provisioning timeout", "cluster_id", clusterID)
			if err := s.updateStatus(ctx, clusterID, models.ClusterStatusFailed); err != nil {
				s.logger.Error("failed to update status to failed after timeout", "cluster_id", clusterID, "error", err)
			}
			return
		case <-ticker.C:
			resp, err := s.provisioner.GetClusterStatus(ctx, &pb.GetClusterStatusRequest{
				ClusterId: provisionerClusterID,
			})
			if err != nil {
				s.logger.Warn("failed to get provisioner status", "cluster_id", clusterID, "error", err)
				continue
			}

			switch resp.Status.Phase {
			case pb.ClusterPhase_PHASE_HEALTHY:
				// Cluster is ready - update status and connection URL
				s.updateClusterFromProvisionerStatus(ctx, clusterID, provisionerClusterID)
				return
			case pb.ClusterPhase_PHASE_SETTING_UP, pb.ClusterPhase_PHASE_UPGRADING:
				// Still provisioning, continue polling
				s.logger.Debug("cluster still provisioning",
					"cluster_id", clusterID,
					"phase", resp.Status.Phase.String(),
					"ready", resp.Status.ReadyInstances,
					"total", resp.Status.TotalInstances,
				)
			default:
				// Check for failure
				s.logger.Error("cluster provisioning failed",
					"cluster_id", clusterID,
					"phase", resp.Status.Phase.String(),
				)
				if err := s.updateStatus(ctx, clusterID, models.ClusterStatusFailed); err != nil {
					s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", err)
				}
				return
			}
		}
	}
}

// updateClusterFromProvisionerStatus updates the cluster with connection info from provisioner.
func (s *ClusterService) updateClusterFromProvisionerStatus(ctx context.Context, clusterID uuid.UUID, provisionerClusterID string) {
	// Get detailed cluster info from provisioner
	resp, err := s.provisioner.GetCluster(ctx, &pb.GetClusterRequest{
		ClusterId: provisionerClusterID,
	})
	if err != nil {
		s.logger.Error("failed to get cluster details from provisioner", "cluster_id", clusterID, "error", err)
		return
	}

	// Extract connection info from provisioner response
	// The provisioner will have created secrets with connection credentials
	var connectionURL, poolerURL *string

	// Connection URL is typically in format: postgresql://user:pass@host:port/dbname
	// For CNPG clusters, this comes from the generated secrets
	if resp.Cluster != nil && resp.Cluster.Status != nil {
		// Build connection URL from cluster status
		if resp.Cluster.Status.CurrentPrimary != "" {
			connStr := fmt.Sprintf("postgresql://%s:5432/postgres", resp.Cluster.Status.CurrentPrimary)
			connectionURL = &connStr
		}
	}

	// Update database with connection info
	query := `
		UPDATE clusters
		SET status = $2, connection_url = $3, pooler_url = $4, updated_at = NOW()
		WHERE id = $1
	`
	if _, err := s.db.Pool.Exec(ctx, query, clusterID, models.ClusterStatusRunning, connectionURL, poolerURL); err != nil {
		s.logger.Error("failed to update cluster after provisioning", "error", err)
		return
	}

	s.logger.Info("cluster provisioning complete via real provisioner", "cluster_id", clusterID)
}

// clusterToProtoSpec converts a cluster model to a provisioner proto spec.
func (s *ClusterService) clusterToProtoSpec(cluster *models.Cluster) *pb.ClusterSpec {
	spec := &pb.ClusterSpec{
		Name:            cluster.Name,
		Namespace:       "customer-" + cluster.ID.String()[:8], // Use unique namespace per cluster
		Instances:       int32(cluster.NodeCount),
		PostgresVersion: "16", // Default to PG 16
		Resources: &pb.ResourceRequirements{
			CpuRequest:    s.nodeSizeToCPU(cluster.NodeSize),
			CpuLimit:      s.nodeSizeToCPU(cluster.NodeSize),
			MemoryRequest: s.nodeSizeToMemory(cluster.NodeSize),
			MemoryLimit:   s.nodeSizeToMemory(cluster.NodeSize),
		},
		Storage: &pb.StorageSpec{
			Size:                fmt.Sprintf("%dGi", cluster.StorageGB),
			StorageClass:        "do-block-storage", // DigitalOcean block storage
			ResizeInUseVolumes:  true,
		},
	}

	// Add Orochi configuration
	spec.OrochiConfig = &pb.OrochiConfig{
		Enabled:           true,
		DefaultShardCount: int32(cluster.DefaultShardCount),
		ChunkInterval:     "7d", // Default chunk interval
		EnableColumnar:    cluster.EnableColumnar,
		DefaultCompression: pb.CompressionType_COMPRESSION_ZSTD,
	}

	// Add tiering configuration if enabled
	if cluster.TieringEnabled {
		spec.OrochiConfig.Tiering = &pb.TieringConfig{
			Enabled: true,
		}
		if cluster.TieringHotDuration != nil {
			spec.OrochiConfig.Tiering.HotDuration = *cluster.TieringHotDuration
		}
		if cluster.TieringWarmDuration != nil {
			spec.OrochiConfig.Tiering.WarmDuration = *cluster.TieringWarmDuration
		}
		if cluster.TieringColdDuration != nil {
			spec.OrochiConfig.Tiering.ColdDuration = *cluster.TieringColdDuration
		}
		// Add S3 config if provided
		if cluster.S3Endpoint != nil && cluster.S3Bucket != nil {
			spec.OrochiConfig.Tiering.S3Config = &pb.S3Config{
				Endpoint: *cluster.S3Endpoint,
				Bucket:   *cluster.S3Bucket,
			}
			if cluster.S3Region != nil {
				spec.OrochiConfig.Tiering.S3Config.Region = *cluster.S3Region
			}
		}
	}

	// Add pooler configuration if enabled
	if cluster.PoolerEnabled {
		spec.Pooler = &pb.ConnectionPoolerSpec{
			Enabled:   true,
			Instances: 2,
			Type:      pb.PoolerType_POOLER_PGBOUNCER,
			Pgbouncer: &pb.PgBouncerConfig{
				PoolMode:        "transaction",
				DefaultPoolSize: 25,
				MaxClientConn:   200,
			},
		}
	}

	// Add backup configuration if enabled
	if cluster.BackupEnabled {
		spec.BackupConfig = &pb.BackupConfiguration{
			RetentionPolicy: fmt.Sprintf("%dd", cluster.BackupRetention),
			ScheduledBackup: &pb.ScheduledBackup{
				Enabled:  true,
				Schedule: "0 2 * * *", // Daily at 2 AM
			},
		}
	}

	return spec
}

// nodeSizeToCPU converts node size to CPU request/limit.
func (s *ClusterService) nodeSizeToCPU(nodeSize string) string {
	switch nodeSize {
	case "small":
		return "250m"
	case "medium":
		return "500m"
	case "large":
		return "1"
	case "xlarge":
		return "2"
	case "2xlarge":
		return "4"
	default:
		return "500m"
	}
}

// nodeSizeToMemory converts node size to memory request/limit.
func (s *ClusterService) nodeSizeToMemory(nodeSize string) string {
	switch nodeSize {
	case "small":
		return "512Mi"
	case "medium":
		return "1Gi"
	case "large":
		return "2Gi"
	case "xlarge":
		return "4Gi"
	case "2xlarge":
		return "8Gi"
	default:
		return "1Gi"
	}
}

// provisionSimulated runs simulated provisioning (for development/testing).
func (s *ClusterService) provisionSimulated(ctx context.Context, cluster *models.Cluster) {
	s.logger.Info("using simulated provisioning for cluster", "cluster_id", cluster.ID)

	// Simulate provisioning time with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("provisioning cancelled during main phase", "cluster_id", cluster.ID, "error", ctx.Err())
		if err := s.updateStatus(context.Background(), cluster.ID, models.ClusterStatusFailed); err != nil {
			s.logger.Error("failed to update status to failed after cancellation", "cluster_id", cluster.ID, "error", err)
		}
		return
	case <-time.After(10 * time.Second):
	}

	// Generate connection URL and pooler URL (if pooler is enabled)
	clusterHost := fmt.Sprintf("cluster-%s.orochi.cloud", cluster.ID.String()[:8])
	password := uuid.New().String()[:8]
	connectionURL := fmt.Sprintf("postgresql://orochi:%s@%s:5432/orochi", password, clusterHost)

	var poolerURL *string
	if cluster.PoolerEnabled {
		// PgBouncer pooler typically runs on port 6432
		poolerStr := fmt.Sprintf("postgresql://orochi:%s@%s:6432/orochi?pgbouncer=true", password, clusterHost)
		poolerURL = &poolerStr
	}

	query := `
		UPDATE clusters
		SET status = $2, connection_url = $3, pooler_url = $4, updated_at = NOW()
		WHERE id = $1
	`
	if _, err := s.db.Pool.Exec(ctx, query, cluster.ID, models.ClusterStatusRunning, connectionURL, poolerURL); err != nil {
		s.logger.Error("failed to update cluster after provisioning", "error", err)
		if updateErr := s.updateStatus(context.Background(), cluster.ID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed after provisioning error", "cluster_id", cluster.ID, "error", updateErr)
		}
		return
	}

	s.logger.Info("simulated cluster provisioning complete",
		"cluster_id", cluster.ID,
		"pooler_enabled", cluster.PoolerEnabled,
	)
}

// startDeprovisioning initiates the deprovisioning process.
// Uses the real provisioner gRPC service when enabled, otherwise falls back to simulation.
func (s *ClusterService) startDeprovisioning(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster deprovisioning", "cluster_id", clusterID)

	// Use real provisioner if enabled
	if s.provisioner != nil && s.provisioner.IsEnabled() {
		s.deprovisionWithRealProvisioner(ctx, clusterID)
		return
	}

	// Simulate deprovisioning with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("deprovisioning cancelled", "cluster_id", clusterID, "error", ctx.Err())
		return
	case <-time.After(5 * time.Second):
	}

	s.logger.Info("simulated cluster deprovisioning complete", "cluster_id", clusterID)
}

// deprovisionWithRealProvisioner uses the gRPC provisioner service.
func (s *ClusterService) deprovisionWithRealProvisioner(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("using real provisioner for cluster deletion", "cluster_id", clusterID)

	// Get cluster to determine namespace
	cluster, err := s.GetByID(ctx, clusterID)
	if err != nil {
		s.logger.Error("failed to get cluster for deprovisioning", "cluster_id", clusterID, "error", err)
		return
	}

	req := &pb.DeleteClusterRequest{
		ClusterId:     cluster.Name, // Use name as cluster ID in K8s
		Namespace:     "customer-" + clusterID.String()[:8],
		DeleteBackups: false, // Preserve backups by default
		DeletePvcs:    true,  // Clean up PVCs
	}

	resp, err := s.provisioner.DeleteCluster(ctx, req)
	if err != nil {
		s.logger.Error("provisioner failed to delete cluster", "cluster_id", clusterID, "error", err)
		return
	}

	if resp.Success {
		s.logger.Info("cluster deletion initiated via real provisioner", "cluster_id", clusterID)
	} else {
		s.logger.Warn("cluster deletion may have issues", "cluster_id", clusterID, "message", resp.Message)
	}
}

// performScaling initiates the scaling process.
// Uses the real provisioner gRPC service when enabled, otherwise falls back to simulation.
func (s *ClusterService) performScaling(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster scaling", "cluster_id", clusterID)

	// Use real provisioner if enabled
	if s.provisioner != nil && s.provisioner.IsEnabled() {
		s.scaleWithRealProvisioner(ctx, clusterID)
		return
	}

	// Simulate scaling with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("scaling cancelled", "cluster_id", clusterID, "error", ctx.Err())
		return
	case <-time.After(5 * time.Second):
	}

	if err := s.updateStatus(ctx, clusterID, models.ClusterStatusRunning); err != nil {
		s.logger.Error("failed to update status after scaling", "cluster_id", clusterID, "error", err)
		return
	}
	s.logger.Info("simulated cluster scaling complete", "cluster_id", clusterID)
}

// scaleWithRealProvisioner uses the gRPC provisioner service.
func (s *ClusterService) scaleWithRealProvisioner(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("using real provisioner for cluster scaling", "cluster_id", clusterID)

	// Get current cluster configuration
	cluster, err := s.GetByID(ctx, clusterID)
	if err != nil {
		s.logger.Error("failed to get cluster for scaling", "cluster_id", clusterID, "error", err)
		if updateErr := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", updateErr)
		}
		return
	}

	// Build update request with new configuration
	spec := s.clusterToProtoSpec(cluster)

	req := &pb.UpdateClusterRequest{
		ClusterId:     cluster.Name,
		Spec:          spec,
		RollingUpdate: true,
	}

	resp, err := s.provisioner.UpdateCluster(ctx, req)
	if err != nil {
		s.logger.Error("provisioner failed to scale cluster", "cluster_id", clusterID, "error", err)
		if updateErr := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", updateErr)
		}
		return
	}

	s.logger.Info("cluster scaling initiated via real provisioner",
		"cluster_id", clusterID,
		"phase", resp.Phase.String(),
	)

	// Start background polling for completion
	go s.pollScalingStatus(context.Background(), clusterID, cluster.Name)
}

// pollScalingStatus polls for scaling completion.
func (s *ClusterService) pollScalingStatus(ctx context.Context, clusterID uuid.UUID, provisionerClusterID string) {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	timeout := time.After(10 * time.Minute)

	for {
		select {
		case <-ctx.Done():
			return
		case <-timeout:
			s.logger.Error("scaling timeout", "cluster_id", clusterID)
			if err := s.updateStatus(ctx, clusterID, models.ClusterStatusFailed); err != nil {
				s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", err)
			}
			return
		case <-ticker.C:
			resp, err := s.provisioner.GetClusterStatus(ctx, &pb.GetClusterStatusRequest{
				ClusterId: provisionerClusterID,
			})
			if err != nil {
				s.logger.Warn("failed to get scaling status", "cluster_id", clusterID, "error", err)
				continue
			}

			if resp.Status.Phase == pb.ClusterPhase_PHASE_HEALTHY {
				if err := s.updateStatus(ctx, clusterID, models.ClusterStatusRunning); err != nil {
					s.logger.Error("failed to update status after scaling", "cluster_id", clusterID, "error", err)
				}
				s.logger.Info("cluster scaling complete via real provisioner", "cluster_id", clusterID)
				return
			}
		}
	}
}

// applyUpdate simulates applying configuration updates.
func (s *ClusterService) applyUpdate(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("applying cluster update", "cluster_id", clusterID)

	// Simulate update with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("update cancelled", "cluster_id", clusterID, "error", ctx.Err())
		return
	case <-time.After(3 * time.Second):
	}

	if err := s.updateStatus(ctx, clusterID, models.ClusterStatusRunning); err != nil {
		s.logger.Error("failed to update status after update", "cluster_id", clusterID, "error", err)
		return
	}
	s.logger.Info("cluster update complete", "cluster_id", clusterID)
}

// updateStatus updates the cluster status.
func (s *ClusterService) updateStatus(ctx context.Context, clusterID uuid.UUID, status models.ClusterStatus) error {
	query := `UPDATE clusters SET status = $2, updated_at = NOW() WHERE id = $1`
	if _, err := s.db.Pool.Exec(ctx, query, clusterID, status); err != nil {
		s.logger.Error("failed to update cluster status", "error", err, "cluster_id", clusterID)
		return fmt.Errorf("failed to update cluster status: %w", err)
	}
	return nil
}
