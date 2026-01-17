package models

import (
	"time"

	"github.com/google/uuid"
)

// QueryHistoryEntry represents a query executed by a user.
type QueryHistoryEntry struct {
	ID              uuid.UUID  `json:"id"`
	UserID          uuid.UUID  `json:"user_id"`
	ClusterID       uuid.UUID  `json:"cluster_id"`
	QueryText       string     `json:"query_text"`
	Description     *string    `json:"description,omitempty"`
	ExecutionTimeMs int64      `json:"execution_time_ms"`
	RowsAffected    int        `json:"rows_affected"`
	Status          string     `json:"status"` // success, error
	ErrorMessage    *string    `json:"error_message,omitempty"`
	CreatedAt       time.Time  `json:"created_at"`
}

// SavedQuery represents a user's saved query.
type SavedQuery struct {
	ID          uuid.UUID `json:"id"`
	UserID      uuid.UUID `json:"user_id"`
	ClusterID   uuid.UUID `json:"cluster_id"`
	Name        string    `json:"name"`
	Description *string   `json:"description,omitempty"`
	QueryText   string    `json:"query_text"`
	IsPublic    bool      `json:"is_public"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

// SaveQueryRequest represents a request to save a query.
type SaveQueryRequest struct {
	Name        string  `json:"name"`
	Description *string `json:"description,omitempty"`
	QueryText   string  `json:"query_text"`
	IsPublic    bool    `json:"is_public,omitempty"`
}

// ExecuteSQLRequest represents a request to execute SQL.
type ExecuteSQLRequest struct {
	SQL      string `json:"sql"`
	ReadOnly bool   `json:"read_only,omitempty"`
}

// TableDataRequest represents a request to fetch table data.
type TableDataRequest struct {
	Schema    string `json:"schema"`
	Table     string `json:"table"`
	Limit     int    `json:"limit,omitempty"`
	Offset    int    `json:"offset,omitempty"`
	OrderBy   string `json:"order_by,omitempty"`
	OrderDir  string `json:"order_dir,omitempty"` // ASC or DESC
}

// InternalTableStats represents statistics about internal/system tables.
type InternalTableStats struct {
	Hypertables      int    `json:"hypertables"`
	Chunks           int    `json:"chunks"`
	CompressedChunks int    `json:"compressed_chunks"`
	ShardCount       int    `json:"shard_count"`
	TotalSizeBytes   int64  `json:"total_size_bytes"`
	TotalSizeHuman   string `json:"total_size_human"`
}

// ClusterSettings represents configurable settings for a cluster.
type ClusterSettings struct {
	ID                    uuid.UUID  `json:"id"`
	ClusterID             uuid.UUID  `json:"cluster_id"`
	MaxConnections        int        `json:"max_connections"`
	SharedBuffers         string     `json:"shared_buffers"`
	EffectiveCacheSize    string     `json:"effective_cache_size"`
	MaintenanceWorkMem    string     `json:"maintenance_work_mem"`
	WorkMem               string     `json:"work_mem"`
	WALBuffers            string     `json:"wal_buffers"`
	RandomPageCost        float64    `json:"random_page_cost"`
	EffectiveIOConcurrency int       `json:"effective_io_concurrency"`
	LogMinDurationStmt    int        `json:"log_min_duration_statement"` // ms, -1 to disable
	AutoExplainEnabled    bool       `json:"auto_explain_enabled"`
	PGStatStatementsEnabled bool     `json:"pg_stat_statements_enabled"`
	CreatedAt             time.Time  `json:"created_at"`
	UpdatedAt             time.Time  `json:"updated_at"`
}

// ClusterSettingsUpdateRequest represents a request to update cluster settings.
type ClusterSettingsUpdateRequest struct {
	MaxConnections        *int     `json:"max_connections,omitempty"`
	SharedBuffers         *string  `json:"shared_buffers,omitempty"`
	EffectiveCacheSize    *string  `json:"effective_cache_size,omitempty"`
	MaintenanceWorkMem    *string  `json:"maintenance_work_mem,omitempty"`
	WorkMem               *string  `json:"work_mem,omitempty"`
	WALBuffers            *string  `json:"wal_buffers,omitempty"`
	RandomPageCost        *float64 `json:"random_page_cost,omitempty"`
	EffectiveIOConcurrency *int    `json:"effective_io_concurrency,omitempty"`
	LogMinDurationStmt    *int     `json:"log_min_duration_statement,omitempty"`
	AutoExplainEnabled    *bool    `json:"auto_explain_enabled,omitempty"`
	PGStatStatementsEnabled *bool  `json:"pg_stat_statements_enabled,omitempty"`
}

// PerformanceRecommendation represents a performance recommendation.
type PerformanceRecommendation struct {
	ID          uuid.UUID `json:"id"`
	ClusterID   uuid.UUID `json:"cluster_id"`
	Type        string    `json:"type"` // index, config, query
	Severity    string    `json:"severity"` // low, medium, high, critical
	Title       string    `json:"title"`
	Description string    `json:"description"`
	SQLFix      *string   `json:"sql_fix,omitempty"`
	Applied     bool      `json:"applied"`
	DismissedAt *time.Time `json:"dismissed_at,omitempty"`
	CreatedAt   time.Time `json:"created_at"`
}
