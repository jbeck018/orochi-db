package handlers

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// DataBrowserHandler handles data browsing requests.
type DataBrowserHandler struct {
	dataBrowserService *services.DataBrowserService
	clusterService     *services.ClusterService
	logger             *slog.Logger
}

// NewDataBrowserHandler creates a new data browser handler.
func NewDataBrowserHandler(dataBrowserService *services.DataBrowserService, clusterService *services.ClusterService, logger *slog.Logger) *DataBrowserHandler {
	return &DataBrowserHandler{
		dataBrowserService: dataBrowserService,
		clusterService:     clusterService,
		logger:             logger.With("handler", "data_browser"),
	}
}

// getClusterForUser retrieves a cluster and validates the user has access.
func (h *DataBrowserHandler) getClusterForUser(r *http.Request, user *models.User) (*models.Cluster, *models.APIError) {
	clusterIDStr := chi.URLParam(r, "id")
	clusterID, err := uuid.Parse(clusterIDStr)
	if err != nil {
		return nil, &models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		}
	}

	cluster, err := h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterID, user.ID)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			return nil, &models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			}
		}
		h.logger.Error("failed to get cluster", "error", err)
		return nil, &models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster",
		}
	}

	return cluster, nil
}

// ListTables lists all user-visible tables in a cluster database.
// GET /api/v1/clusters/{id}/data/tables
func (h *DataBrowserHandler) ListTables(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cluster is not running",
		})
		return
	}

	tables, err := h.dataBrowserService.ListTables(r.Context(), cluster)
	if err != nil {
		if errors.Is(err, models.ErrConnectionFailed) {
			writeJSON(w, http.StatusServiceUnavailable, models.APIError{
				Code:    models.ErrCodeServiceUnavailable,
				Message: "Failed to connect to cluster database",
			})
			return
		}
		h.logger.Error("failed to list tables", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to list tables",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"tables": tables,
		"count":  len(tables),
	})
}

// GetTableSchema retrieves the schema for a specific table.
// GET /api/v1/clusters/{id}/data/tables/{schema}/{table}
func (h *DataBrowserHandler) GetTableSchema(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cluster is not running",
		})
		return
	}

	schema := chi.URLParam(r, "schema")
	table := chi.URLParam(r, "table")

	if schema == "" || table == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Schema and table name are required",
		})
		return
	}

	tableSchema, err := h.dataBrowserService.GetTableSchema(r.Context(), cluster, schema, table)
	if err != nil {
		if errors.Is(err, models.ErrTableNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Table not found",
			})
			return
		}
		h.logger.Error("failed to get table schema", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get table schema",
		})
		return
	}

	writeJSON(w, http.StatusOK, tableSchema)
}

// GetTableData retrieves paginated data from a table.
// GET /api/v1/clusters/{id}/data/tables/{schema}/{table}/data
func (h *DataBrowserHandler) GetTableData(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cluster is not running",
		})
		return
	}

	schema := chi.URLParam(r, "schema")
	table := chi.URLParam(r, "table")

	if schema == "" || table == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Schema and table name are required",
		})
		return
	}

	// Parse query parameters
	limit := 100
	offset := 0
	orderBy := ""
	orderDir := "ASC"

	if l := r.URL.Query().Get("limit"); l != "" {
		if parsed, err := strconv.Atoi(l); err == nil && parsed > 0 {
			limit = parsed
		}
	}
	if o := r.URL.Query().Get("offset"); o != "" {
		if parsed, err := strconv.Atoi(o); err == nil && parsed >= 0 {
			offset = parsed
		}
	}
	if ob := r.URL.Query().Get("order_by"); ob != "" {
		orderBy = ob
	}
	if od := r.URL.Query().Get("order_dir"); od == "DESC" || od == "desc" {
		orderDir = "DESC"
	}

	result, err := h.dataBrowserService.QueryTableData(r.Context(), cluster, user.ID, schema, table, limit, offset, orderBy, orderDir)
	if err != nil {
		if errors.Is(err, models.ErrTableNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Table not found",
			})
			return
		}
		h.logger.Error("failed to get table data", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get table data",
		})
		return
	}

	writeJSON(w, http.StatusOK, result)
}

// ExecuteSQL executes arbitrary SQL on the cluster database.
// POST /api/v1/clusters/{id}/data/query
func (h *DataBrowserHandler) ExecuteSQL(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cluster is not running",
		})
		return
	}

	var req models.ExecuteSQLRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	if req.SQL == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeValidation,
			Message: "SQL query is required",
		})
		return
	}

	result, err := h.dataBrowserService.ExecuteSQL(r.Context(), cluster, user.ID, req.SQL, req.ReadOnly)
	if err != nil {
		if errors.Is(err, models.ErrQueryNotAllowed) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: err.Error(),
			})
			return
		}
		h.logger.Error("failed to execute SQL", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: err.Error(),
		})
		return
	}

	writeJSON(w, http.StatusOK, result)
}

// GetQueryHistory retrieves the query history for a cluster.
// GET /api/v1/clusters/{id}/data/history
func (h *DataBrowserHandler) GetQueryHistory(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	limit := 50
	if l := r.URL.Query().Get("limit"); l != "" {
		if parsed, err := strconv.Atoi(l); err == nil && parsed > 0 {
			limit = parsed
		}
	}

	history, err := h.dataBrowserService.GetQueryHistory(r.Context(), user.ID, cluster.ID, limit)
	if err != nil {
		h.logger.Error("failed to get query history", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get query history",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"history": history,
		"count":   len(history),
	})
}

// GetInternalStats retrieves statistics about internal/system tables.
// GET /api/v1/clusters/{id}/data/stats
func (h *DataBrowserHandler) GetInternalStats(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	cluster, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		writeJSON(w, http.StatusBadRequest, apiErr)
		return
	}

	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cluster is not running",
		})
		return
	}

	stats, err := h.dataBrowserService.GetInternalStats(r.Context(), cluster)
	if err != nil {
		h.logger.Error("failed to get internal stats", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get internal statistics",
		})
		return
	}

	writeJSON(w, http.StatusOK, stats)
}
