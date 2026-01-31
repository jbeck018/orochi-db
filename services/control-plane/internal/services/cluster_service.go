package services

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// ClusterService handles cluster-related business logic.
type ClusterService struct {
	db     *db.DB
	logger *slog.Logger
}

// NewClusterService creates a new cluster service.
func NewClusterService(db *db.DB, logger *slog.Logger) *ClusterService {
	return &ClusterService{
		db:     db,
		logger: logger.With("service", "cluster"),
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

// startProvisioning simulates the provisioning process.
// In production, this would interact with cloud providers.
func (s *ClusterService) startProvisioning(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster provisioning", "cluster_id", clusterID)

	// Simulate provisioning time with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("provisioning cancelled during initial phase", "cluster_id", clusterID, "error", ctx.Err())
		return
	case <-time.After(5 * time.Second):
	}

	// Update status to provisioning
	if err := s.updateStatus(ctx, clusterID, models.ClusterStatusProvisioning); err != nil {
		s.logger.Error("failed to update status to provisioning", "cluster_id", clusterID, "error", err)
		return
	}

	// Simulate more provisioning with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("provisioning cancelled during main phase", "cluster_id", clusterID, "error", ctx.Err())
		if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); err != nil {
			s.logger.Error("failed to update status to failed after cancellation", "cluster_id", clusterID, "error", err)
		}
		return
	case <-time.After(10 * time.Second):
	}

	// Get cluster to check if pooler is enabled
	cluster, err := s.GetByID(ctx, clusterID)
	if err != nil {
		s.logger.Error("failed to get cluster for provisioning", "cluster_id", clusterID, "error", err)
		if updateErr := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", updateErr)
		}
		return
	}

	// Generate connection URL and pooler URL (if pooler is enabled)
	clusterHost := fmt.Sprintf("cluster-%s.orochi.cloud", clusterID.String()[:8])
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
	if _, err := s.db.Pool.Exec(ctx, query, clusterID, models.ClusterStatusRunning, connectionURL, poolerURL); err != nil {
		s.logger.Error("failed to update cluster after provisioning", "error", err)
		if updateErr := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); updateErr != nil {
			s.logger.Error("failed to update status to failed after provisioning error", "cluster_id", clusterID, "error", updateErr)
		}
		return
	}

	s.logger.Info("cluster provisioning complete",
		"cluster_id", clusterID,
		"pooler_enabled", cluster.PoolerEnabled,
	)
}

// startDeprovisioning simulates the deprovisioning process.
func (s *ClusterService) startDeprovisioning(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster deprovisioning", "cluster_id", clusterID)

	// Simulate deprovisioning with cancellation support
	select {
	case <-ctx.Done():
		s.logger.Warn("deprovisioning cancelled", "cluster_id", clusterID, "error", ctx.Err())
		return
	case <-time.After(5 * time.Second):
	}

	s.logger.Info("cluster deprovisioning complete", "cluster_id", clusterID)
}

// performScaling simulates the scaling process.
func (s *ClusterService) performScaling(ctx context.Context, clusterID uuid.UUID) {
	s.logger.Info("starting cluster scaling", "cluster_id", clusterID)

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
	s.logger.Info("cluster scaling complete", "cluster_id", clusterID)
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

// SuspendCluster suspends a cluster (scale-to-zero).
// Uses a transaction with SELECT ... FOR UPDATE to prevent TOCTOU race conditions.
func (s *ClusterService) SuspendCluster(ctx context.Context, clusterID, userID uuid.UUID, req *models.SuspendClusterRequest) error {
	// Set defaults
	drainTimeout := req.DrainTimeout
	if drainTimeout <= 0 {
		drainTimeout = 30
	}

	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return errors.New("failed to suspend cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, owner_id, status, scale_to_zero_enabled
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	var id uuid.UUID
	var ownerID uuid.UUID
	var status models.ClusterStatus
	var scaleToZeroEnabled bool
	err = tx.QueryRow(ctx, selectQuery, clusterID).Scan(&id, &ownerID, &status, &scaleToZeroEnabled)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster for suspend", "error", err, "cluster_id", clusterID)
		return errors.New("failed to suspend cluster")
	}

	// Check ownership
	if ownerID != userID {
		return models.ErrForbidden
	}

	// Check if scale-to-zero is enabled (or allow manual suspend regardless)
	// For now, we allow manual suspend even if auto scale-to-zero is disabled

	// Check current status
	switch status {
	case models.ClusterStatusSuspended:
		return models.ErrClusterAlreadySuspended
	case models.ClusterStatusSuspending:
		return models.ErrClusterSuspending
	case models.ClusterStatusWaking:
		return models.ErrClusterWaking
	case models.ClusterStatusRunning:
		// OK to suspend
	default:
		return models.ErrClusterOperationPending
	}

	now := time.Now()
	updateQuery := `
		UPDATE clusters
		SET status = $2, suspended_at = $3, updated_at = $3
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery, clusterID, models.ClusterStatusSuspending, now)
	if err != nil {
		s.logger.Error("failed to update cluster to suspending", "error", err)
		return errors.New("failed to suspend cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return errors.New("failed to suspend cluster")
	}

	s.logger.Info("cluster suspend initiated",
		"cluster_id", clusterID,
		"drain_timeout", drainTimeout,
		"force", req.Force,
	)

	// Trigger the suspend workflow in background
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), time.Duration(drainTimeout+60)*time.Second)
		defer cancel()
		s.performSuspend(ctx, clusterID, drainTimeout, req.Force)
	}()

	return nil
}

// WakeCluster wakes a suspended cluster.
// Uses a transaction with SELECT ... FOR UPDATE to prevent TOCTOU race conditions.
func (s *ClusterService) WakeCluster(ctx context.Context, clusterID, userID uuid.UUID, req *models.WakeClusterRequest) (*models.ClusterStateResponse, error) {
	// Set defaults
	timeout := req.Timeout
	if timeout <= 0 {
		timeout = 60
	}

	// Start a transaction to ensure atomicity
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to wake cluster")
	}
	defer tx.Rollback(ctx)

	// Lock the row for update to prevent concurrent modifications
	selectQuery := `
		SELECT id, owner_id, status, suspended_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
		FOR UPDATE
	`

	var id uuid.UUID
	var ownerID uuid.UUID
	var status models.ClusterStatus
	var suspendedAt *time.Time
	err = tx.QueryRow(ctx, selectQuery, clusterID).Scan(&id, &ownerID, &status, &suspendedAt)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster for wake", "error", err, "cluster_id", clusterID)
		return nil, errors.New("failed to wake cluster")
	}

	// Check ownership
	if ownerID != userID {
		return nil, models.ErrForbidden
	}

	// Check current status
	switch status {
	case models.ClusterStatusSuspended:
		// OK to wake
	case models.ClusterStatusWaking:
		// Already waking - return current state
		return &models.ClusterStateResponse{
			ClusterID:   clusterID,
			Status:      status,
			IsReady:     false,
			SuspendedAt: suspendedAt,
			Message:     "Cluster is already waking",
		}, nil
	case models.ClusterStatusRunning:
		return &models.ClusterStateResponse{
			ClusterID: clusterID,
			Status:    status,
			IsReady:   true,
			Message:   "Cluster is already running",
		}, nil
	case models.ClusterStatusSuspending:
		return nil, models.ErrClusterSuspending
	default:
		return nil, models.ErrClusterNotSuspended
	}

	now := time.Now()
	updateQuery := `
		UPDATE clusters
		SET status = $2, updated_at = $3
		WHERE id = $1
	`

	_, err = tx.Exec(ctx, updateQuery, clusterID, models.ClusterStatusWaking, now)
	if err != nil {
		s.logger.Error("failed to update cluster to waking", "error", err)
		return nil, errors.New("failed to wake cluster")
	}

	// Commit the transaction
	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to wake cluster")
	}

	s.logger.Info("cluster wake initiated",
		"cluster_id", clusterID,
		"timeout", timeout,
		"wait_for_ready", req.WaitForReady,
	)

	// Trigger the wake workflow in background
	wakeDone := make(chan error, 1)
	go func() {
		wakeCtx, cancel := context.WithTimeout(context.Background(), time.Duration(timeout)*time.Second)
		defer cancel()
		wakeDone <- s.performWake(wakeCtx, clusterID)
	}()

	response := &models.ClusterStateResponse{
		ClusterID:     clusterID,
		Status:        models.ClusterStatusWaking,
		IsReady:       false,
		SuspendedAt:   suspendedAt,
		WakeStartedAt: &now,
		Message:       "Cluster wake initiated",
	}

	// If requested, wait for the cluster to be ready
	if req.WaitForReady {
		wakeCtx, cancel := context.WithTimeout(ctx, time.Duration(timeout)*time.Second)
		defer cancel()

		select {
		case err := <-wakeDone:
			if err != nil {
				response.Message = fmt.Sprintf("Wake failed: %v", err)
				return response, err
			}
			response.Status = models.ClusterStatusRunning
			response.IsReady = true
			response.Message = "Cluster is ready"
		case <-wakeCtx.Done():
			response.Message = "Wake timeout - cluster may still be starting"
			return response, models.ErrWakeTimeout
		}
	}

	return response, nil
}

// GetClusterState returns the current state of a cluster for wake-on-connect.
func (s *ClusterService) GetClusterState(ctx context.Context, clusterID uuid.UUID) (*models.ClusterStateResponse, error) {
	query := `
		SELECT id, status, last_activity_at, suspended_at
		FROM clusters
		WHERE id = $1 AND deleted_at IS NULL
	`

	var id uuid.UUID
	var status models.ClusterStatus
	var lastActivity *time.Time
	var suspendedAt *time.Time

	err := s.db.Pool.QueryRow(ctx, query, clusterID).Scan(&id, &status, &lastActivity, &suspendedAt)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster state", "error", err, "cluster_id", clusterID)
		return nil, errors.New("failed to get cluster state")
	}

	isReady := status == models.ClusterStatusRunning
	var message string
	switch status {
	case models.ClusterStatusRunning:
		message = "Cluster is ready"
	case models.ClusterStatusSuspended:
		message = "Cluster is suspended"
	case models.ClusterStatusWaking:
		message = "Cluster is waking up"
	case models.ClusterStatusSuspending:
		message = "Cluster is suspending"
	default:
		message = fmt.Sprintf("Cluster status: %s", status)
	}

	return &models.ClusterStateResponse{
		ClusterID:    clusterID,
		Status:       status,
		IsReady:      isReady,
		LastActivity: lastActivity,
		SuspendedAt:  suspendedAt,
		Message:      message,
	}, nil
}

// UpdateLastActivity updates the last activity timestamp for a cluster.
func (s *ClusterService) UpdateLastActivity(ctx context.Context, clusterID uuid.UUID) error {
	query := `UPDATE clusters SET last_activity_at = NOW(), updated_at = NOW() WHERE id = $1 AND deleted_at IS NULL`
	_, err := s.db.Pool.Exec(ctx, query, clusterID)
	if err != nil {
		s.logger.Error("failed to update last activity", "error", err, "cluster_id", clusterID)
		return fmt.Errorf("failed to update last activity: %w", err)
	}
	return nil
}

// performSuspend handles the actual suspend workflow.
func (s *ClusterService) performSuspend(ctx context.Context, clusterID uuid.UUID, drainTimeout int, force bool) {
	s.logger.Info("performing cluster suspend",
		"cluster_id", clusterID,
		"drain_timeout", drainTimeout,
		"force", force,
	)

	// In production, this would:
	// 1. Notify the provisioner to drain connections
	// 2. Wait for connections to drain (or timeout)
	// 3. Scale PostgreSQL pods to 0
	// 4. Update cluster status to suspended

	// Simulate drain time
	if !force {
		select {
		case <-ctx.Done():
			s.logger.Warn("suspend cancelled during drain", "cluster_id", clusterID, "error", ctx.Err())
			if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusRunning); err != nil {
				s.logger.Error("failed to restore running status after cancelled suspend", "cluster_id", clusterID, "error", err)
			}
			return
		case <-time.After(time.Duration(drainTimeout) * time.Second / 2): // Simulated drain
		}
	}

	// Simulate pod scale down
	select {
	case <-ctx.Done():
		s.logger.Warn("suspend cancelled during scale down", "cluster_id", clusterID, "error", ctx.Err())
		if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusRunning); err != nil {
			s.logger.Error("failed to restore running status after cancelled suspend", "cluster_id", clusterID, "error", err)
		}
		return
	case <-time.After(2 * time.Second):
	}

	// Update to suspended
	now := time.Now()
	query := `UPDATE clusters SET status = $2, suspended_at = $3, updated_at = $3 WHERE id = $1`
	if _, err := s.db.Pool.Exec(ctx, query, clusterID, models.ClusterStatusSuspended, now); err != nil {
		s.logger.Error("failed to update cluster to suspended", "error", err, "cluster_id", clusterID)
		if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); err != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", err)
		}
		return
	}

	s.logger.Info("cluster suspended successfully", "cluster_id", clusterID)
}

// performWake handles the actual wake workflow.
func (s *ClusterService) performWake(ctx context.Context, clusterID uuid.UUID) error {
	s.logger.Info("performing cluster wake", "cluster_id", clusterID)

	// In production, this would:
	// 1. Notify the provisioner to scale PostgreSQL pods back up
	// 2. Wait for pods to be ready
	// 3. Update cluster status to running

	// Simulate pod startup time (target < 5 seconds cold start)
	select {
	case <-ctx.Done():
		s.logger.Warn("wake cancelled during startup", "cluster_id", clusterID, "error", ctx.Err())
		if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusSuspended); err != nil {
			s.logger.Error("failed to restore suspended status after cancelled wake", "cluster_id", clusterID, "error", err)
		}
		return ctx.Err()
	case <-time.After(3 * time.Second): // Simulated quick cold start
	}

	// Update to running
	now := time.Now()
	query := `UPDATE clusters SET status = $2, suspended_at = NULL, last_activity_at = $3, updated_at = $3 WHERE id = $1`
	if _, err := s.db.Pool.Exec(ctx, query, clusterID, models.ClusterStatusRunning, now); err != nil {
		s.logger.Error("failed to update cluster to running", "error", err, "cluster_id", clusterID)
		if err := s.updateStatus(context.Background(), clusterID, models.ClusterStatusFailed); err != nil {
			s.logger.Error("failed to update status to failed", "cluster_id", clusterID, "error", err)
		}
		return fmt.Errorf("failed to update cluster status: %w", err)
	}

	s.logger.Info("cluster woke successfully", "cluster_id", clusterID)
	return nil
}

// TriggerWakeForConnection is called by the JWT gateway to wake a suspended cluster.
// This is an internal API that does not require user authentication.
func (s *ClusterService) TriggerWakeForConnection(ctx context.Context, clusterID uuid.UUID) (*models.ClusterStateResponse, error) {
	// Get current state
	state, err := s.GetClusterState(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	// If already running, just return
	if state.Status == models.ClusterStatusRunning {
		return state, nil
	}

	// If already waking, return current state
	if state.Status == models.ClusterStatusWaking {
		return state, nil
	}

	// If not suspended, can't wake
	if state.Status != models.ClusterStatusSuspended {
		return nil, fmt.Errorf("cluster cannot be woken from state: %s", state.Status)
	}

	// Start wake without ownership check (internal API)
	now := time.Now()
	query := `
		UPDATE clusters
		SET status = $2, updated_at = $3
		WHERE id = $1 AND status = $4 AND deleted_at IS NULL
	`

	result, err := s.db.Pool.Exec(ctx, query, clusterID, models.ClusterStatusWaking, now, models.ClusterStatusSuspended)
	if err != nil {
		s.logger.Error("failed to update cluster to waking", "error", err, "cluster_id", clusterID)
		return nil, errors.New("failed to wake cluster")
	}

	if result.RowsAffected() == 0 {
		// Another request already started the wake
		return s.GetClusterState(ctx, clusterID)
	}

	s.logger.Info("cluster wake triggered by connection", "cluster_id", clusterID)

	// Trigger wake in background
	go func() {
		wakeCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if err := s.performWake(wakeCtx, clusterID); err != nil {
			s.logger.Error("wake failed", "cluster_id", clusterID, "error", err)
		}
	}()

	return &models.ClusterStateResponse{
		ClusterID:     clusterID,
		Status:        models.ClusterStatusWaking,
		IsReady:       false,
		SuspendedAt:   state.SuspendedAt,
		WakeStartedAt: &now,
		Message:       "Cluster wake initiated by connection",
	}, nil
}

// WaitForClusterReady waits for a cluster to become ready after wake.
func (s *ClusterService) WaitForClusterReady(ctx context.Context, clusterID uuid.UUID, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			if time.Now().After(deadline) {
				return models.ErrWakeTimeout
			}

			state, err := s.GetClusterState(ctx, clusterID)
			if err != nil {
				return err
			}

			if state.IsReady {
				return nil
			}

			if state.Status == models.ClusterStatusFailed {
				return errors.New("cluster failed to wake")
			}

			if state.Status == models.ClusterStatusSuspended {
				return errors.New("cluster was not woken")
			}
		}
	}
}
