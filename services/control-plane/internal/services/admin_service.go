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

// AdminService provides administrative operations across all users and clusters.
type AdminService struct {
	db     *db.DB
	logger *slog.Logger
}

// NewAdminService creates a new admin service.
func NewAdminService(db *db.DB, logger *slog.Logger) *AdminService {
	return &AdminService{
		db:     db,
		logger: logger.With("service", "admin"),
	}
}

// AdminStats represents platform-wide statistics.
type AdminStats struct {
	TotalUsers         int       `json:"total_users"`
	ActiveUsers        int       `json:"active_users"`
	TotalClusters      int       `json:"total_clusters"`
	RunningClusters    int       `json:"running_clusters"`
	TotalOrganizations int       `json:"total_organizations"`
	UpdatedAt          time.Time `json:"updated_at"`
}

// GetStats returns platform-wide statistics.
func (s *AdminService) GetStats(ctx context.Context) (*AdminStats, error) {
	stats := &AdminStats{UpdatedAt: time.Now()}

	// Count users
	userQuery := `SELECT COUNT(*), COUNT(*) FILTER (WHERE active = true) FROM users`
	if err := s.db.Pool.QueryRow(ctx, userQuery).Scan(&stats.TotalUsers, &stats.ActiveUsers); err != nil {
		s.logger.Error("failed to count users", "error", err)
		return nil, errors.New("failed to get stats")
	}

	// Count clusters
	clusterQuery := `
		SELECT COUNT(*), COUNT(*) FILTER (WHERE status = 'running')
		FROM clusters WHERE deleted_at IS NULL
	`
	if err := s.db.Pool.QueryRow(ctx, clusterQuery).Scan(&stats.TotalClusters, &stats.RunningClusters); err != nil {
		s.logger.Error("failed to count clusters", "error", err)
		return nil, errors.New("failed to get stats")
	}

	// Count organizations
	orgQuery := `SELECT COUNT(*) FROM organizations WHERE deleted_at IS NULL`
	if err := s.db.Pool.QueryRow(ctx, orgQuery).Scan(&stats.TotalOrganizations); err != nil {
		// Table might not exist yet, treat as 0
		stats.TotalOrganizations = 0
	}

	return stats, nil
}

// AdminUserResponse represents a user in admin context with additional info.
type AdminUserResponse struct {
	*models.User
	ClusterCount int `json:"cluster_count"`
}

// ListAllUsers returns all users with pagination (admin only).
func (s *AdminService) ListAllUsers(ctx context.Context, page, pageSize int, search string) ([]*AdminUserResponse, int, error) {
	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}
	offset := (page - 1) * pageSize

	// Build query with optional search
	countQuery := `SELECT COUNT(*) FROM users WHERE 1=1`
	listQuery := `
		SELECT u.id, u.email, u.name, u.role, u.active, u.created_at, u.updated_at, u.last_login_at,
			   COUNT(c.id) as cluster_count
		FROM users u
		LEFT JOIN clusters c ON c.owner_id = u.id AND c.deleted_at IS NULL
		WHERE 1=1
	`
	args := []any{}
	argIndex := 1

	if search != "" {
		searchFilter := fmt.Sprintf(" AND (u.email ILIKE $%d OR u.name ILIKE $%d)", argIndex, argIndex)
		countQuery += searchFilter
		listQuery += searchFilter
		args = append(args, "%"+search+"%")
		argIndex++
	}

	listQuery += fmt.Sprintf(`
		GROUP BY u.id
		ORDER BY u.created_at DESC
		LIMIT $%d OFFSET $%d
	`, argIndex, argIndex+1)
	args = append(args, pageSize, offset)

	// Get count
	var total int
	countArgs := args[:len(args)-2] // Exclude limit/offset
	if err := s.db.Pool.QueryRow(ctx, countQuery, countArgs...).Scan(&total); err != nil {
		s.logger.Error("failed to count users", "error", err)
		return nil, 0, errors.New("failed to list users")
	}

	// Get users
	rows, err := s.db.Pool.Query(ctx, listQuery, args...)
	if err != nil {
		s.logger.Error("failed to list users", "error", err)
		return nil, 0, errors.New("failed to list users")
	}
	defer rows.Close()

	users := make([]*AdminUserResponse, 0)
	for rows.Next() {
		user := &models.User{}
		var clusterCount int
		err := rows.Scan(
			&user.ID, &user.Email, &user.Name, &user.Role, &user.Active,
			&user.CreatedAt, &user.UpdatedAt, &user.LastLoginAt, &clusterCount,
		)
		if err != nil {
			s.logger.Error("failed to scan user", "error", err)
			continue
		}
		users = append(users, &AdminUserResponse{User: user, ClusterCount: clusterCount})
	}

	return users, total, nil
}

// AdminClusterResponse represents a cluster in admin context with owner info.
type AdminClusterResponse struct {
	*models.Cluster
	OwnerEmail string `json:"owner_email"`
	OwnerName  string `json:"owner_name"`
}

// ListAllClusters returns all clusters with pagination (admin only).
func (s *AdminService) ListAllClusters(ctx context.Context, page, pageSize int, status string) ([]*AdminClusterResponse, int, error) {
	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}
	offset := (page - 1) * pageSize

	countQuery := `SELECT COUNT(*) FROM clusters WHERE deleted_at IS NULL`
	listQuery := `
		SELECT c.id, c.name, c.owner_id, c.status, c.tier, c.provider, c.region, c.version,
			   c.node_count, c.node_size, c.storage_gb, c.connection_url,
			   c.maintenance_day, c.maintenance_hour, c.backup_enabled, c.backup_retention_days,
			   c.created_at, c.updated_at, c.deleted_at,
			   u.email as owner_email, u.name as owner_name
		FROM clusters c
		JOIN users u ON u.id = c.owner_id
		WHERE c.deleted_at IS NULL
	`
	args := []any{}
	argIndex := 1

	if status != "" {
		statusFilter := fmt.Sprintf(" AND c.status = $%d", argIndex)
		countQuery += statusFilter
		listQuery += statusFilter
		args = append(args, status)
		argIndex++
	}

	listQuery += fmt.Sprintf(`
		ORDER BY c.created_at DESC
		LIMIT $%d OFFSET $%d
	`, argIndex, argIndex+1)
	args = append(args, pageSize, offset)

	// Get count
	var total int
	countArgs := args[:len(args)-2]
	if err := s.db.Pool.QueryRow(ctx, countQuery, countArgs...).Scan(&total); err != nil {
		s.logger.Error("failed to count clusters", "error", err)
		return nil, 0, errors.New("failed to list clusters")
	}

	// Get clusters
	rows, err := s.db.Pool.Query(ctx, listQuery, args...)
	if err != nil {
		s.logger.Error("failed to list clusters", "error", err)
		return nil, 0, errors.New("failed to list clusters")
	}
	defer rows.Close()

	clusters := make([]*AdminClusterResponse, 0)
	for rows.Next() {
		cluster := &models.Cluster{}
		var ownerEmail, ownerName string
		err := rows.Scan(
			&cluster.ID, &cluster.Name, &cluster.OwnerID, &cluster.Status,
			&cluster.Tier, &cluster.Provider, &cluster.Region, &cluster.Version,
			&cluster.NodeCount, &cluster.NodeSize, &cluster.StorageGB, &cluster.ConnectionURL,
			&cluster.MaintenanceDay, &cluster.MaintenanceHour,
			&cluster.BackupEnabled, &cluster.BackupRetention,
			&cluster.CreatedAt, &cluster.UpdatedAt, &cluster.DeletedAt,
			&ownerEmail, &ownerName,
		)
		if err != nil {
			s.logger.Error("failed to scan cluster", "error", err)
			continue
		}
		clusters = append(clusters, &AdminClusterResponse{
			Cluster:    cluster,
			OwnerEmail: ownerEmail,
			OwnerName:  ownerName,
		})
	}

	return clusters, total, nil
}

// GetClusterByIDAdmin retrieves any cluster by ID (admin only, no ownership check).
func (s *AdminService) GetClusterByIDAdmin(ctx context.Context, id uuid.UUID) (*AdminClusterResponse, error) {
	query := `
		SELECT c.id, c.name, c.owner_id, c.status, c.tier, c.provider, c.region, c.version,
			   c.node_count, c.node_size, c.storage_gb, c.connection_url,
			   c.maintenance_day, c.maintenance_hour, c.backup_enabled, c.backup_retention_days,
			   c.created_at, c.updated_at, c.deleted_at,
			   u.email as owner_email, u.name as owner_name
		FROM clusters c
		JOIN users u ON u.id = c.owner_id
		WHERE c.id = $1
	`

	cluster := &models.Cluster{}
	var ownerEmail, ownerName string
	err := s.db.Pool.QueryRow(ctx, query, id).Scan(
		&cluster.ID, &cluster.Name, &cluster.OwnerID, &cluster.Status,
		&cluster.Tier, &cluster.Provider, &cluster.Region, &cluster.Version,
		&cluster.NodeCount, &cluster.NodeSize, &cluster.StorageGB, &cluster.ConnectionURL,
		&cluster.MaintenanceDay, &cluster.MaintenanceHour,
		&cluster.BackupEnabled, &cluster.BackupRetention,
		&cluster.CreatedAt, &cluster.UpdatedAt, &cluster.DeletedAt,
		&ownerEmail, &ownerName,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrClusterNotFound
		}
		s.logger.Error("failed to get cluster", "error", err, "cluster_id", id)
		return nil, errors.New("failed to retrieve cluster")
	}

	return &AdminClusterResponse{
		Cluster:    cluster,
		OwnerEmail: ownerEmail,
		OwnerName:  ownerName,
	}, nil
}

// UpdateUserRole updates a user's role (admin only).
func (s *AdminService) UpdateUserRole(ctx context.Context, userID uuid.UUID, role models.UserRole) error {
	query := `UPDATE users SET role = $2, updated_at = NOW() WHERE id = $1`
	result, err := s.db.Pool.Exec(ctx, query, userID, role)
	if err != nil {
		s.logger.Error("failed to update user role", "error", err, "user_id", userID)
		return errors.New("failed to update user role")
	}
	if result.RowsAffected() == 0 {
		return models.ErrUserNotFound
	}
	s.logger.Info("user role updated", "user_id", userID, "role", role)
	return nil
}

// SetUserActive activates or deactivates a user (admin only).
func (s *AdminService) SetUserActive(ctx context.Context, userID uuid.UUID, active bool) error {
	query := `UPDATE users SET active = $2, updated_at = NOW() WHERE id = $1`
	result, err := s.db.Pool.Exec(ctx, query, userID, active)
	if err != nil {
		s.logger.Error("failed to update user active status", "error", err, "user_id", userID)
		return errors.New("failed to update user status")
	}
	if result.RowsAffected() == 0 {
		return models.ErrUserNotFound
	}
	s.logger.Info("user active status updated", "user_id", userID, "active", active)
	return nil
}

// ForceDeleteCluster performs a hard delete on a cluster (admin only, for debugging).
func (s *AdminService) ForceDeleteCluster(ctx context.Context, clusterID uuid.UUID) error {
	// First delete metrics
	metricsQuery := `DELETE FROM cluster_metrics WHERE cluster_id = $1`
	if _, err := s.db.Pool.Exec(ctx, metricsQuery, clusterID); err != nil {
		s.logger.Error("failed to delete cluster metrics", "error", err, "cluster_id", clusterID)
		return errors.New("failed to delete cluster metrics")
	}

	// Then delete cluster
	clusterQuery := `DELETE FROM clusters WHERE id = $1`
	result, err := s.db.Pool.Exec(ctx, clusterQuery, clusterID)
	if err != nil {
		s.logger.Error("failed to force delete cluster", "error", err, "cluster_id", clusterID)
		return errors.New("failed to delete cluster")
	}
	if result.RowsAffected() == 0 {
		return models.ErrClusterNotFound
	}

	s.logger.Warn("cluster force deleted", "cluster_id", clusterID)
	return nil
}

// GetUserByIDAdmin retrieves any user by ID (admin only).
func (s *AdminService) GetUserByIDAdmin(ctx context.Context, userID uuid.UUID) (*AdminUserResponse, error) {
	query := `
		SELECT u.id, u.email, u.name, u.role, u.active, u.created_at, u.updated_at, u.last_login_at,
			   COUNT(c.id) as cluster_count
		FROM users u
		LEFT JOIN clusters c ON c.owner_id = u.id AND c.deleted_at IS NULL
		WHERE u.id = $1
		GROUP BY u.id
	`

	user := &models.User{}
	var clusterCount int
	err := s.db.Pool.QueryRow(ctx, query, userID).Scan(
		&user.ID, &user.Email, &user.Name, &user.Role, &user.Active,
		&user.CreatedAt, &user.UpdatedAt, &user.LastLoginAt, &clusterCount,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrUserNotFound
		}
		s.logger.Error("failed to get user", "error", err, "user_id", userID)
		return nil, errors.New("failed to retrieve user")
	}

	return &AdminUserResponse{User: user, ClusterCount: clusterCount}, nil
}
