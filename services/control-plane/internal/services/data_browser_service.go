package services

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// DataBrowserService handles data browsing operations for cluster databases.
type DataBrowserService struct {
	db     *db.DB // Control plane database for storing query history
	logger *slog.Logger
}

// NewDataBrowserService creates a new data browser service.
func NewDataBrowserService(db *db.DB, logger *slog.Logger) *DataBrowserService {
	return &DataBrowserService{
		db:     db,
		logger: logger.With("service", "data_browser"),
	}
}

// internalTablePatterns defines patterns for tables that should not be shown to users.
var internalTablePatterns = []string{
	"_timescaledb_%",       // TimescaleDB internal tables
	"_hypertable_%",        // Hypertable internals
	"hypertable_data_%",    // Hypertable data chunks
	"orochi_%",             // Orochi internal tables
	"_orochi_%",            // Orochi internal prefixed
	"pg_%",                 // PostgreSQL system catalogs (except pg_catalog)
	"_pg_%",                // PostgreSQL internal
	"information_schema.%", // Information schema (handled separately)
	"chunk_%",              // TimescaleDB chunks
	"_materialized_hyper%", // Materialized views internal
	"_dist_hyper%",         // Distributed hypertable internal
	"_shard_%",             // Shard internal tables
}

// TableInfo represents information about a database table.
type TableInfo struct {
	Schema      string    `json:"schema"`
	Name        string    `json:"name"`
	Type        string    `json:"type"` // table, view, materialized_view
	RowEstimate int64     `json:"row_estimate"`
	SizeBytes   int64     `json:"size_bytes"`
	SizeHuman   string    `json:"size_human"`
	Columns     int       `json:"columns"`
	HasPrimary  bool      `json:"has_primary_key"`
	CreatedAt   time.Time `json:"created_at,omitempty"`
}

// ColumnInfo represents information about a table column.
type ColumnInfo struct {
	Name         string  `json:"name"`
	Type         string  `json:"type"`
	Nullable     bool    `json:"nullable"`
	DefaultValue *string `json:"default_value,omitempty"`
	IsPrimaryKey bool    `json:"is_primary_key"`
	IsForeignKey bool    `json:"is_foreign_key"`
	FKReference  *string `json:"fk_reference,omitempty"`
	Position     int     `json:"position"`
}

// TableSchema represents the full schema of a table.
type TableSchema struct {
	Schema      string       `json:"schema"`
	Name        string       `json:"name"`
	Columns     []ColumnInfo `json:"columns"`
	PrimaryKey  []string     `json:"primary_key"`
	Indexes     []IndexInfo  `json:"indexes"`
	ForeignKeys []FKInfo     `json:"foreign_keys"`
	RowEstimate int64        `json:"row_estimate"`
}

// IndexInfo represents information about an index.
type IndexInfo struct {
	Name       string   `json:"name"`
	Columns    []string `json:"columns"`
	IsUnique   bool     `json:"is_unique"`
	IsPrimary  bool     `json:"is_primary"`
	Definition string   `json:"definition"`
}

// FKInfo represents foreign key information.
type FKInfo struct {
	Name            string   `json:"name"`
	Columns         []string `json:"columns"`
	ReferencedTable string   `json:"referenced_table"`
	ReferencedCols  []string `json:"referenced_columns"`
}

// QueryResult represents the result of a SQL query.
type QueryResult struct {
	Columns       []string        `json:"columns"`
	ColumnTypes   []string        `json:"column_types"`
	Rows          [][]interface{} `json:"rows"`
	RowCount      int             `json:"row_count"`
	TotalCount    *int64          `json:"total_count,omitempty"` // For paginated queries
	ExecutionTime int64           `json:"execution_time_ms"`
	Truncated     bool            `json:"truncated"`
}

// connectToCluster creates a connection pool to a cluster's database.
func (s *DataBrowserService) connectToCluster(ctx context.Context, cluster *models.Cluster) (*pgxpool.Pool, error) {
	if cluster.ConnectionURL == nil || *cluster.ConnectionURL == "" {
		return nil, errors.New("cluster has no connection URL")
	}

	// Use pooler URL if available and enabled
	connURL := *cluster.ConnectionURL
	if cluster.PoolerEnabled && cluster.PoolerURL != nil && *cluster.PoolerURL != "" {
		connURL = *cluster.PoolerURL
	}

	config, err := pgxpool.ParseConfig(connURL)
	if err != nil {
		return nil, fmt.Errorf("failed to parse connection URL: %w", err)
	}

	// Configure pool for data browsing (short-lived connections)
	config.MaxConns = 5
	config.MinConns = 1
	config.MaxConnLifetime = 5 * time.Minute
	config.MaxConnIdleTime = 1 * time.Minute

	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		return nil, fmt.Errorf("failed to connect to cluster: %w", err)
	}

	// Test connection
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("failed to ping cluster: %w", err)
	}

	return pool, nil
}

// ListTables lists all user-visible tables in the cluster database.
func (s *DataBrowserService) ListTables(ctx context.Context, cluster *models.Cluster) ([]TableInfo, error) {
	pool, err := s.connectToCluster(ctx, cluster)
	if err != nil {
		return nil, err
	}
	defer pool.Close()

	// Query to get tables with metadata, excluding internal tables
	query := `
		WITH table_stats AS (
			SELECT
				schemaname,
				tablename,
				n_live_tup as row_estimate,
				pg_total_relation_size(schemaname || '.' || tablename) as size_bytes
			FROM pg_stat_user_tables
		)
		SELECT
			t.table_schema,
			t.table_name,
			t.table_type,
			COALESCE(ts.row_estimate, 0),
			COALESCE(ts.size_bytes, 0),
			pg_size_pretty(COALESCE(ts.size_bytes, 0)),
			(SELECT COUNT(*) FROM information_schema.columns c WHERE c.table_schema = t.table_schema AND c.table_name = t.table_name),
			EXISTS(
				SELECT 1 FROM information_schema.table_constraints tc
				WHERE tc.table_schema = t.table_schema
				AND tc.table_name = t.table_name
				AND tc.constraint_type = 'PRIMARY KEY'
			)
		FROM information_schema.tables t
		LEFT JOIN table_stats ts ON ts.schemaname = t.table_schema AND ts.tablename = t.table_name
		WHERE t.table_schema NOT IN ('pg_catalog', 'information_schema', '_timescaledb_internal', '_timescaledb_catalog', '_timescaledb_config', '_timescaledb_cache')
		AND t.table_type IN ('BASE TABLE', 'VIEW')
		AND t.table_name NOT LIKE '_hyper_%'
		AND t.table_name NOT LIKE 'chunk_%'
		AND t.table_name NOT LIKE '_dist_hyper_%'
		AND t.table_name NOT LIKE '_materialized_hyper_%'
		AND t.table_name NOT LIKE 'orochi_%'
		AND t.table_name NOT LIKE '_orochi_%'
		AND t.table_name NOT LIKE '_shard_%'
		ORDER BY t.table_schema, t.table_name
	`

	rows, err := pool.Query(ctx, query)
	if err != nil {
		s.logger.Error("failed to list tables", "error", err, "cluster_id", cluster.ID)
		return nil, fmt.Errorf("failed to list tables: %w", err)
	}
	defer rows.Close()

	tables := make([]TableInfo, 0)
	for rows.Next() {
		var t TableInfo
		err := rows.Scan(
			&t.Schema, &t.Name, &t.Type, &t.RowEstimate,
			&t.SizeBytes, &t.SizeHuman, &t.Columns, &t.HasPrimary,
		)
		if err != nil {
			s.logger.Error("failed to scan table info", "error", err)
			continue
		}

		// Normalize table type
		switch t.Type {
		case "BASE TABLE":
			t.Type = "table"
		case "VIEW":
			t.Type = "view"
		}

		tables = append(tables, t)
	}

	return tables, nil
}

// GetTableSchema retrieves the full schema information for a table.
func (s *DataBrowserService) GetTableSchema(ctx context.Context, cluster *models.Cluster, schema, tableName string) (*TableSchema, error) {
	pool, err := s.connectToCluster(ctx, cluster)
	if err != nil {
		return nil, err
	}
	defer pool.Close()

	// Validate table exists and is visible
	var exists bool
	err = pool.QueryRow(ctx, `
		SELECT EXISTS(
			SELECT 1 FROM information_schema.tables
			WHERE table_schema = $1 AND table_name = $2
			AND table_schema NOT IN ('pg_catalog', 'information_schema', '_timescaledb_internal')
		)
	`, schema, tableName).Scan(&exists)
	if err != nil {
		return nil, fmt.Errorf("failed to check table existence: %w", err)
	}
	if !exists {
		return nil, models.ErrTableNotFound
	}

	tableSchema := &TableSchema{
		Schema: schema,
		Name:   tableName,
	}

	// Get columns
	columnQuery := `
		SELECT
			c.column_name,
			c.data_type,
			c.is_nullable = 'YES',
			c.column_default,
			c.ordinal_position,
			COALESCE(
				(SELECT true FROM information_schema.key_column_usage kcu
				 JOIN information_schema.table_constraints tc ON kcu.constraint_name = tc.constraint_name
				 WHERE tc.constraint_type = 'PRIMARY KEY'
				 AND kcu.table_schema = c.table_schema
				 AND kcu.table_name = c.table_name
				 AND kcu.column_name = c.column_name),
				false
			) as is_pk,
			COALESCE(
				(SELECT true FROM information_schema.key_column_usage kcu
				 JOIN information_schema.table_constraints tc ON kcu.constraint_name = tc.constraint_name
				 WHERE tc.constraint_type = 'FOREIGN KEY'
				 AND kcu.table_schema = c.table_schema
				 AND kcu.table_name = c.table_name
				 AND kcu.column_name = c.column_name),
				false
			) as is_fk
		FROM information_schema.columns c
		WHERE c.table_schema = $1 AND c.table_name = $2
		ORDER BY c.ordinal_position
	`

	rows, err := pool.Query(ctx, columnQuery, schema, tableName)
	if err != nil {
		return nil, fmt.Errorf("failed to get columns: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var col ColumnInfo
		err := rows.Scan(
			&col.Name, &col.Type, &col.Nullable, &col.DefaultValue,
			&col.Position, &col.IsPrimaryKey, &col.IsForeignKey,
		)
		if err != nil {
			s.logger.Error("failed to scan column", "error", err)
			continue
		}
		if col.IsPrimaryKey {
			tableSchema.PrimaryKey = append(tableSchema.PrimaryKey, col.Name)
		}
		tableSchema.Columns = append(tableSchema.Columns, col)
	}

	// Get indexes
	indexQuery := `
		SELECT
			i.relname as index_name,
			array_agg(a.attname ORDER BY array_position(ix.indkey, a.attnum)) as columns,
			ix.indisunique,
			ix.indisprimary,
			pg_get_indexdef(ix.indexrelid) as definition
		FROM pg_class t
		JOIN pg_namespace n ON n.oid = t.relnamespace
		JOIN pg_index ix ON t.oid = ix.indrelid
		JOIN pg_class i ON i.oid = ix.indexrelid
		JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY(ix.indkey)
		WHERE n.nspname = $1 AND t.relname = $2
		GROUP BY i.relname, ix.indisunique, ix.indisprimary, ix.indexrelid
	`

	indexRows, err := pool.Query(ctx, indexQuery, schema, tableName)
	if err != nil {
		s.logger.Warn("failed to get indexes", "error", err)
	} else {
		defer indexRows.Close()
		for indexRows.Next() {
			var idx IndexInfo
			err := indexRows.Scan(&idx.Name, &idx.Columns, &idx.IsUnique, &idx.IsPrimary, &idx.Definition)
			if err != nil {
				s.logger.Error("failed to scan index", "error", err)
				continue
			}
			tableSchema.Indexes = append(tableSchema.Indexes, idx)
		}
	}

	// Get row estimate
	err = pool.QueryRow(ctx, `
		SELECT COALESCE(n_live_tup, 0)
		FROM pg_stat_user_tables
		WHERE schemaname = $1 AND relname = $2
	`, schema, tableName).Scan(&tableSchema.RowEstimate)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		s.logger.Warn("failed to get row estimate", "error", err)
	}

	return tableSchema, nil
}

// QueryTableData retrieves paginated data from a table.
func (s *DataBrowserService) QueryTableData(ctx context.Context, cluster *models.Cluster, userID uuid.UUID, schema, tableName string, limit, offset int, orderBy string, orderDir string) (*QueryResult, error) {
	pool, err := s.connectToCluster(ctx, cluster)
	if err != nil {
		return nil, err
	}
	defer pool.Close()

	// Validate table exists
	var exists bool
	err = pool.QueryRow(ctx, `
		SELECT EXISTS(
			SELECT 1 FROM information_schema.tables
			WHERE table_schema = $1 AND table_name = $2
			AND table_schema NOT IN ('pg_catalog', 'information_schema', '_timescaledb_internal')
		)
	`, schema, tableName).Scan(&exists)
	if err != nil {
		return nil, fmt.Errorf("failed to check table existence: %w", err)
	}
	if !exists {
		return nil, models.ErrTableNotFound
	}

	// Sanitize inputs
	if limit <= 0 || limit > 1000 {
		limit = 100
	}
	if offset < 0 {
		offset = 0
	}
	if orderDir != "ASC" && orderDir != "DESC" {
		orderDir = "ASC"
	}

	// Build query
	fullTableName := fmt.Sprintf("%s.%s", pgx.Identifier{schema}.Sanitize(), pgx.Identifier{tableName}.Sanitize())

	// Get total count
	var totalCount int64
	countQuery := fmt.Sprintf("SELECT COUNT(*) FROM %s", fullTableName)
	err = pool.QueryRow(ctx, countQuery).Scan(&totalCount)
	if err != nil {
		s.logger.Warn("failed to get total count", "error", err)
	}

	// Build data query with ordering
	dataQuery := fmt.Sprintf("SELECT * FROM %s", fullTableName)
	if orderBy != "" {
		// Validate orderBy column exists
		var columnExists bool
		err = pool.QueryRow(ctx, `
			SELECT EXISTS(
				SELECT 1 FROM information_schema.columns
				WHERE table_schema = $1 AND table_name = $2 AND column_name = $3
			)
		`, schema, tableName, orderBy).Scan(&columnExists)
		if err == nil && columnExists {
			dataQuery += fmt.Sprintf(" ORDER BY %s %s", pgx.Identifier{orderBy}.Sanitize(), orderDir)
		}
	}
	dataQuery += fmt.Sprintf(" LIMIT %d OFFSET %d", limit, offset)

	// Execute query and time it
	start := time.Now()
	rows, err := pool.Query(ctx, dataQuery)
	if err != nil {
		return nil, fmt.Errorf("failed to query table: %w", err)
	}
	defer rows.Close()

	// Get column info
	fieldDescs := rows.FieldDescriptions()
	columns := make([]string, len(fieldDescs))
	columnTypes := make([]string, len(fieldDescs))
	for i, fd := range fieldDescs {
		columns[i] = string(fd.Name)
		columnTypes[i] = fmt.Sprintf("%d", fd.DataTypeOID) // OID as string, frontend can map to type names
	}

	// Collect rows
	resultRows := make([][]interface{}, 0)
	for rows.Next() {
		values, err := rows.Values()
		if err != nil {
			s.logger.Error("failed to scan row", "error", err)
			continue
		}
		resultRows = append(resultRows, values)
	}

	executionTime := time.Since(start).Milliseconds()

	result := &QueryResult{
		Columns:       columns,
		ColumnTypes:   columnTypes,
		Rows:          resultRows,
		RowCount:      len(resultRows),
		TotalCount:    &totalCount,
		ExecutionTime: executionTime,
		Truncated:     len(resultRows) == limit && totalCount > int64(offset+limit),
	}

	// Store in query history
	go s.recordQueryHistory(context.Background(), userID, cluster.ID, dataQuery, executionTime, len(resultRows), "success", "")

	return result, nil
}

// ExecuteSQL executes arbitrary SQL on the cluster database.
func (s *DataBrowserService) ExecuteSQL(ctx context.Context, cluster *models.Cluster, userID uuid.UUID, sql string, readOnly bool) (*QueryResult, error) {
	pool, err := s.connectToCluster(ctx, cluster)
	if err != nil {
		return nil, err
	}
	defer pool.Close()

	// Trim and validate SQL
	sql = strings.TrimSpace(sql)
	if sql == "" {
		return nil, errors.New("SQL query is required")
	}

	// Check for dangerous operations if read-only mode
	if readOnly {
		upperSQL := strings.ToUpper(sql)
		dangerousOps := []string{"INSERT", "UPDATE", "DELETE", "DROP", "CREATE", "ALTER", "TRUNCATE", "GRANT", "REVOKE"}
		for _, op := range dangerousOps {
			if strings.Contains(upperSQL, op) {
				return nil, fmt.Errorf("operation '%s' not allowed in read-only mode", op)
			}
		}
	}

	start := time.Now()

	// Execute query
	rows, err := pool.Query(ctx, sql)
	if err != nil {
		executionTime := time.Since(start).Milliseconds()
		go s.recordQueryHistory(context.Background(), userID, cluster.ID, sql, executionTime, 0, "error", err.Error())
		return nil, fmt.Errorf("query execution failed: %w", err)
	}
	defer rows.Close()

	// Get column info
	fieldDescs := rows.FieldDescriptions()
	columns := make([]string, len(fieldDescs))
	columnTypes := make([]string, len(fieldDescs))
	for i, fd := range fieldDescs {
		columns[i] = string(fd.Name)
		columnTypes[i] = fmt.Sprintf("%d", fd.DataTypeOID)
	}

	// Collect rows (with limit to prevent memory issues)
	maxRows := 10000
	resultRows := make([][]interface{}, 0)
	truncated := false
	for rows.Next() {
		if len(resultRows) >= maxRows {
			truncated = true
			break
		}
		values, err := rows.Values()
		if err != nil {
			s.logger.Error("failed to scan row", "error", err)
			continue
		}
		resultRows = append(resultRows, values)
	}

	executionTime := time.Since(start).Milliseconds()

	result := &QueryResult{
		Columns:       columns,
		ColumnTypes:   columnTypes,
		Rows:          resultRows,
		RowCount:      len(resultRows),
		ExecutionTime: executionTime,
		Truncated:     truncated,
	}

	// Record in history
	go s.recordQueryHistory(context.Background(), userID, cluster.ID, sql, executionTime, len(resultRows), "success", "")

	return result, nil
}

// recordQueryHistory records a query in the history table.
func (s *DataBrowserService) recordQueryHistory(ctx context.Context, userID, clusterID uuid.UUID, query string, executionTimeMs int64, rowsAffected int, status, errorMsg string) {
	insertQuery := `
		INSERT INTO query_history (id, user_id, cluster_id, query_text, execution_time_ms, rows_affected, status, error_message, created_at)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, NOW())
	`

	var errMsgPtr *string
	if errorMsg != "" {
		errMsgPtr = &errorMsg
	}

	_, err := s.db.Pool.Exec(ctx, insertQuery,
		uuid.New(), userID, clusterID, query, executionTimeMs, rowsAffected, status, errMsgPtr,
	)
	if err != nil {
		s.logger.Error("failed to record query history", "error", err)
	}
}

// GetQueryHistory retrieves query history for a user/cluster.
func (s *DataBrowserService) GetQueryHistory(ctx context.Context, userID, clusterID uuid.UUID, limit int) ([]models.QueryHistoryEntry, error) {
	if limit <= 0 || limit > 100 {
		limit = 50
	}

	query := `
		SELECT id, user_id, cluster_id, query_text, description, execution_time_ms, rows_affected, status, error_message, created_at
		FROM query_history
		WHERE user_id = $1 AND cluster_id = $2
		ORDER BY created_at DESC
		LIMIT $3
	`

	rows, err := s.db.Pool.Query(ctx, query, userID, clusterID, limit)
	if err != nil {
		return nil, fmt.Errorf("failed to get query history: %w", err)
	}
	defer rows.Close()

	entries := make([]models.QueryHistoryEntry, 0)
	for rows.Next() {
		var entry models.QueryHistoryEntry
		err := rows.Scan(
			&entry.ID, &entry.UserID, &entry.ClusterID, &entry.QueryText, &entry.Description,
			&entry.ExecutionTimeMs, &entry.RowsAffected, &entry.Status, &entry.ErrorMessage, &entry.CreatedAt,
		)
		if err != nil {
			s.logger.Error("failed to scan query history entry", "error", err)
			continue
		}
		entries = append(entries, entry)
	}

	return entries, nil
}

// GetInternalStats retrieves statistics about internal/system tables (hypertables, chunks, shards).
func (s *DataBrowserService) GetInternalStats(ctx context.Context, cluster *models.Cluster) (*models.InternalTableStats, error) {
	pool, err := s.connectToCluster(ctx, cluster)
	if err != nil {
		return nil, err
	}
	defer pool.Close()

	stats := &models.InternalTableStats{}

	// Try to get TimescaleDB hypertable info if extension is available
	var tsExists bool
	err = pool.QueryRow(ctx, `
		SELECT EXISTS(SELECT 1 FROM pg_extension WHERE extname = 'timescaledb')
	`).Scan(&tsExists)
	if err != nil {
		s.logger.Warn("failed to check for timescaledb", "error", err)
	}

	if tsExists {
		// Count hypertables
		err = pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM _timescaledb_catalog.hypertable
		`).Scan(&stats.Hypertables)
		if err != nil {
			s.logger.Warn("failed to count hypertables", "error", err)
		}

		// Count chunks
		err = pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM _timescaledb_catalog.chunk
		`).Scan(&stats.Chunks)
		if err != nil {
			s.logger.Warn("failed to count chunks", "error", err)
		}

		// Count compressed chunks
		err = pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM _timescaledb_catalog.chunk WHERE compressed_chunk_id IS NOT NULL
		`).Scan(&stats.CompressedChunks)
		if err != nil {
			s.logger.Warn("failed to count compressed chunks", "error", err)
		}
	}

	// Try to get Orochi shard info if available
	var orochiExists bool
	err = pool.QueryRow(ctx, `
		SELECT EXISTS(SELECT 1 FROM pg_extension WHERE extname = 'orochi')
	`).Scan(&orochiExists)
	if err != nil {
		s.logger.Warn("failed to check for orochi", "error", err)
	}

	if orochiExists {
		// Count shards from orochi catalog
		err = pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM orochi_catalog.shards
		`).Scan(&stats.ShardCount)
		if err != nil {
			// Table might not exist
			s.logger.Debug("failed to count shards", "error", err)
		}
	}

	// Get total size of internal tables
	sizeQuery := `
		SELECT COALESCE(SUM(pg_total_relation_size(schemaname || '.' || tablename)), 0)
		FROM pg_tables
		WHERE schemaname LIKE '_timescaledb%'
		   OR schemaname LIKE 'orochi%'
		   OR tablename LIKE '_hyper_%'
		   OR tablename LIKE 'chunk_%'
	`
	err = pool.QueryRow(ctx, sizeQuery).Scan(&stats.TotalSizeBytes)
	if err != nil {
		s.logger.Warn("failed to get internal table size", "error", err)
	}

	// Get human-readable size
	if stats.TotalSizeBytes > 0 {
		err = pool.QueryRow(ctx, `SELECT pg_size_pretty($1::bigint)`, stats.TotalSizeBytes).Scan(&stats.TotalSizeHuman)
		if err != nil {
			stats.TotalSizeHuman = fmt.Sprintf("%d bytes", stats.TotalSizeBytes)
		}
	} else {
		stats.TotalSizeHuman = "0 bytes"
	}

	return stats, nil
}
