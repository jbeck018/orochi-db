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

// ClusterSettingsService handles cluster settings operations.
type ClusterSettingsService struct {
	db     *db.DB
	logger *slog.Logger
}

// NewClusterSettingsService creates a new cluster settings service.
func NewClusterSettingsService(db *db.DB, logger *slog.Logger) *ClusterSettingsService {
	return &ClusterSettingsService{
		db:     db,
		logger: logger.With("service", "cluster_settings"),
	}
}

// GetSettings retrieves settings for a cluster.
func (s *ClusterSettingsService) GetSettings(ctx context.Context, clusterID uuid.UUID) (*models.ClusterSettings, error) {
	query := `
		SELECT id, cluster_id, max_connections, shared_buffers, effective_cache_size,
		       maintenance_work_mem, work_mem, wal_buffers, random_page_cost,
		       effective_io_concurrency, log_min_duration_statement, auto_explain_enabled,
		       pg_stat_statements_enabled, created_at, updated_at
		FROM cluster_settings
		WHERE cluster_id = $1
	`

	settings := &models.ClusterSettings{}
	err := s.db.Pool.QueryRow(ctx, query, clusterID).Scan(
		&settings.ID, &settings.ClusterID, &settings.MaxConnections, &settings.SharedBuffers,
		&settings.EffectiveCacheSize, &settings.MaintenanceWorkMem, &settings.WorkMem,
		&settings.WALBuffers, &settings.RandomPageCost, &settings.EffectiveIOConcurrency,
		&settings.LogMinDurationStmt, &settings.AutoExplainEnabled, &settings.PGStatStatementsEnabled,
		&settings.CreatedAt, &settings.UpdatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			// Return default settings if none exist
			return s.getDefaultSettings(clusterID), nil
		}
		s.logger.Error("failed to get cluster settings", "error", err, "cluster_id", clusterID)
		return nil, fmt.Errorf("failed to get cluster settings: %w", err)
	}

	return settings, nil
}

// getDefaultSettings returns default settings for a new cluster.
func (s *ClusterSettingsService) getDefaultSettings(clusterID uuid.UUID) *models.ClusterSettings {
	now := time.Now()
	return &models.ClusterSettings{
		ID:                      uuid.New(),
		ClusterID:               clusterID,
		MaxConnections:          100,
		SharedBuffers:           "256MB",
		EffectiveCacheSize:      "768MB",
		MaintenanceWorkMem:      "64MB",
		WorkMem:                 "4MB",
		WALBuffers:              "16MB",
		RandomPageCost:          1.1,
		EffectiveIOConcurrency:  200,
		LogMinDurationStmt:      1000, // 1 second
		AutoExplainEnabled:      false,
		PGStatStatementsEnabled: true,
		CreatedAt:               now,
		UpdatedAt:               now,
	}
}

// CreateOrUpdateSettings creates or updates settings for a cluster.
func (s *ClusterSettingsService) CreateOrUpdateSettings(ctx context.Context, clusterID uuid.UUID, req *models.ClusterSettingsUpdateRequest) (*models.ClusterSettings, error) {
	// Get existing settings or defaults
	settings, err := s.GetSettings(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	// Apply updates
	if req.MaxConnections != nil {
		settings.MaxConnections = *req.MaxConnections
	}
	if req.SharedBuffers != nil {
		settings.SharedBuffers = *req.SharedBuffers
	}
	if req.EffectiveCacheSize != nil {
		settings.EffectiveCacheSize = *req.EffectiveCacheSize
	}
	if req.MaintenanceWorkMem != nil {
		settings.MaintenanceWorkMem = *req.MaintenanceWorkMem
	}
	if req.WorkMem != nil {
		settings.WorkMem = *req.WorkMem
	}
	if req.WALBuffers != nil {
		settings.WALBuffers = *req.WALBuffers
	}
	if req.RandomPageCost != nil {
		settings.RandomPageCost = *req.RandomPageCost
	}
	if req.EffectiveIOConcurrency != nil {
		settings.EffectiveIOConcurrency = *req.EffectiveIOConcurrency
	}
	if req.LogMinDurationStmt != nil {
		settings.LogMinDurationStmt = *req.LogMinDurationStmt
	}
	if req.AutoExplainEnabled != nil {
		settings.AutoExplainEnabled = *req.AutoExplainEnabled
	}
	if req.PGStatStatementsEnabled != nil {
		settings.PGStatStatementsEnabled = *req.PGStatStatementsEnabled
	}

	settings.UpdatedAt = time.Now()

	// Upsert settings
	query := `
		INSERT INTO cluster_settings (
			id, cluster_id, max_connections, shared_buffers, effective_cache_size,
			maintenance_work_mem, work_mem, wal_buffers, random_page_cost,
			effective_io_concurrency, log_min_duration_statement, auto_explain_enabled,
			pg_stat_statements_enabled, created_at, updated_at
		) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)
		ON CONFLICT (cluster_id) DO UPDATE SET
			max_connections = EXCLUDED.max_connections,
			shared_buffers = EXCLUDED.shared_buffers,
			effective_cache_size = EXCLUDED.effective_cache_size,
			maintenance_work_mem = EXCLUDED.maintenance_work_mem,
			work_mem = EXCLUDED.work_mem,
			wal_buffers = EXCLUDED.wal_buffers,
			random_page_cost = EXCLUDED.random_page_cost,
			effective_io_concurrency = EXCLUDED.effective_io_concurrency,
			log_min_duration_statement = EXCLUDED.log_min_duration_statement,
			auto_explain_enabled = EXCLUDED.auto_explain_enabled,
			pg_stat_statements_enabled = EXCLUDED.pg_stat_statements_enabled,
			updated_at = EXCLUDED.updated_at
		RETURNING id, created_at
	`

	err = s.db.Pool.QueryRow(ctx, query,
		settings.ID, settings.ClusterID, settings.MaxConnections, settings.SharedBuffers,
		settings.EffectiveCacheSize, settings.MaintenanceWorkMem, settings.WorkMem,
		settings.WALBuffers, settings.RandomPageCost, settings.EffectiveIOConcurrency,
		settings.LogMinDurationStmt, settings.AutoExplainEnabled, settings.PGStatStatementsEnabled,
		settings.CreatedAt, settings.UpdatedAt,
	).Scan(&settings.ID, &settings.CreatedAt)
	if err != nil {
		s.logger.Error("failed to upsert cluster settings", "error", err, "cluster_id", clusterID)
		return nil, fmt.Errorf("failed to save cluster settings: %w", err)
	}

	s.logger.Info("cluster settings updated", "cluster_id", clusterID)
	return settings, nil
}

// GetRecommendations retrieves performance recommendations for a cluster.
func (s *ClusterSettingsService) GetRecommendations(ctx context.Context, clusterID uuid.UUID) ([]*models.PerformanceRecommendation, error) {
	query := `
		SELECT id, cluster_id, type, severity, title, description, sql_fix, applied, dismissed_at, created_at
		FROM performance_recommendations
		WHERE cluster_id = $1 AND dismissed_at IS NULL
		ORDER BY
			CASE severity
				WHEN 'critical' THEN 1
				WHEN 'high' THEN 2
				WHEN 'medium' THEN 3
				WHEN 'low' THEN 4
			END,
			created_at DESC
	`

	rows, err := s.db.Pool.Query(ctx, query, clusterID)
	if err != nil {
		s.logger.Error("failed to get performance recommendations", "error", err, "cluster_id", clusterID)
		return nil, fmt.Errorf("failed to get recommendations: %w", err)
	}
	defer rows.Close()

	recommendations := make([]*models.PerformanceRecommendation, 0)
	for rows.Next() {
		rec := &models.PerformanceRecommendation{}
		err := rows.Scan(
			&rec.ID, &rec.ClusterID, &rec.Type, &rec.Severity, &rec.Title,
			&rec.Description, &rec.SQLFix, &rec.Applied, &rec.DismissedAt, &rec.CreatedAt,
		)
		if err != nil {
			s.logger.Error("failed to scan recommendation", "error", err)
			continue
		}
		recommendations = append(recommendations, rec)
	}

	return recommendations, nil
}

// CreateRecommendation creates a new performance recommendation.
func (s *ClusterSettingsService) CreateRecommendation(ctx context.Context, clusterID uuid.UUID, recType, severity, title, description string, sqlFix *string) (*models.PerformanceRecommendation, error) {
	rec := &models.PerformanceRecommendation{
		ID:          uuid.New(),
		ClusterID:   clusterID,
		Type:        recType,
		Severity:    severity,
		Title:       title,
		Description: description,
		SQLFix:      sqlFix,
		Applied:     false,
		CreatedAt:   time.Now(),
	}

	query := `
		INSERT INTO performance_recommendations (id, cluster_id, type, severity, title, description, sql_fix, applied, created_at)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
	`

	_, err := s.db.Pool.Exec(ctx, query,
		rec.ID, rec.ClusterID, rec.Type, rec.Severity, rec.Title,
		rec.Description, rec.SQLFix, rec.Applied, rec.CreatedAt,
	)
	if err != nil {
		s.logger.Error("failed to create recommendation", "error", err, "cluster_id", clusterID)
		return nil, fmt.Errorf("failed to create recommendation: %w", err)
	}

	return rec, nil
}

// DismissRecommendation marks a recommendation as dismissed.
func (s *ClusterSettingsService) DismissRecommendation(ctx context.Context, recommendationID uuid.UUID) error {
	query := `
		UPDATE performance_recommendations
		SET dismissed_at = NOW()
		WHERE id = $1
	`

	result, err := s.db.Pool.Exec(ctx, query, recommendationID)
	if err != nil {
		s.logger.Error("failed to dismiss recommendation", "error", err, "recommendation_id", recommendationID)
		return fmt.Errorf("failed to dismiss recommendation: %w", err)
	}

	if result.RowsAffected() == 0 {
		return errors.New("recommendation not found")
	}

	s.logger.Info("recommendation dismissed", "recommendation_id", recommendationID)
	return nil
}

// ApplyRecommendation marks a recommendation as applied.
func (s *ClusterSettingsService) ApplyRecommendation(ctx context.Context, recommendationID uuid.UUID) error {
	query := `
		UPDATE performance_recommendations
		SET applied = true
		WHERE id = $1
	`

	result, err := s.db.Pool.Exec(ctx, query, recommendationID)
	if err != nil {
		s.logger.Error("failed to apply recommendation", "error", err, "recommendation_id", recommendationID)
		return fmt.Errorf("failed to apply recommendation: %w", err)
	}

	if result.RowsAffected() == 0 {
		return errors.New("recommendation not found")
	}

	s.logger.Info("recommendation applied", "recommendation_id", recommendationID)
	return nil
}
