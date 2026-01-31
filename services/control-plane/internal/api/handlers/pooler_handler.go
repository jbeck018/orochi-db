package handlers

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// PoolerHandler handles pooler-related requests.
type PoolerHandler struct {
	poolerService  *services.PoolerService
	clusterService *services.ClusterService
	logger         *slog.Logger
}

// NewPoolerHandler creates a new pooler handler.
func NewPoolerHandler(poolerService *services.PoolerService, clusterService *services.ClusterService, logger *slog.Logger) *PoolerHandler {
	return &PoolerHandler{
		poolerService:  poolerService,
		clusterService: clusterService,
		logger:         logger.With("handler", "pooler"),
	}
}

// GetPoolerStatus returns the pooler status for a cluster.
// GET /api/v1/clusters/{id}/pooler
func (h *PoolerHandler) GetPoolerStatus(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	status, err := h.poolerService.GetPoolerStatus(r.Context(), clusterID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		default:
			h.logger.Error("failed to get pooler status", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to get pooler status",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"pooler": status,
	})
}

// UpdatePoolerConfig updates the pooler configuration for a cluster.
// PATCH /api/v1/clusters/{id}/pooler
func (h *PoolerHandler) UpdatePoolerConfig(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.PoolerUpdateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	config, err := h.poolerService.UpdatePoolerConfig(r.Context(), clusterID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrInvalidPoolerMode),
			errors.Is(err, models.ErrInvalidPoolSize),
			errors.Is(err, models.ErrMinPoolGreaterThanMax),
			errors.Is(err, models.ErrInvalidTimeout),
			errors.Is(err, models.ErrPoolerInvalidShardCount),
			errors.Is(err, models.ErrInvalidLoadBalancing):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		default:
			h.logger.Error("failed to update pooler config", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to update pooler configuration",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"config":  config,
		"message": "Pooler configuration updated successfully",
	})
}

// ReloadPoolerConfig triggers a configuration reload on the pooler.
// POST /api/v1/clusters/{id}/pooler/reload
func (h *PoolerHandler) ReloadPoolerConfig(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	if err := h.poolerService.ReloadPoolerConfig(r.Context(), clusterID); err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrPoolerNotReady):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Connection pooler is not ready",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrPoolerReloadFailed):
			writeJSON(w, http.StatusServiceUnavailable, models.APIError{
				Code:    models.ErrCodeServiceUnavailable,
				Message: "Failed to reload pooler configuration",
			})
		default:
			h.logger.Error("failed to reload pooler config", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to reload pooler configuration",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"message": "Pooler configuration reloaded successfully",
	})
}

// GetPoolerStats returns detailed pooler statistics for a cluster.
// GET /api/v1/clusters/{id}/pooler/stats
func (h *PoolerHandler) GetPoolerStats(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	stats, err := h.poolerService.GetPoolerStats(r.Context(), clusterID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrPoolerNotReady):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Connection pooler is not ready",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrPoolerConnectionFailed):
			writeJSON(w, http.StatusServiceUnavailable, models.APIError{
				Code:    models.ErrCodeServiceUnavailable,
				Message: "Failed to connect to pooler",
			})
		default:
			h.logger.Error("failed to get pooler stats", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to get pooler statistics",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"stats": stats,
	})
}

// GetPoolerClients returns connected clients for a cluster's pooler.
// GET /api/v1/clusters/{id}/pooler/clients
func (h *PoolerHandler) GetPoolerClients(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	clients, err := h.poolerService.GetPoolerClients(r.Context(), clusterID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrPoolerNotReady):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Connection pooler is not ready",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrPoolerConnectionFailed):
			writeJSON(w, http.StatusServiceUnavailable, models.APIError{
				Code:    models.ErrCodeServiceUnavailable,
				Message: "Failed to connect to pooler",
			})
		default:
			h.logger.Error("failed to get pooler clients", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to get pooler clients",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"clients": clients,
		"count":   len(clients),
	})
}

// GetPoolerPools returns pool information for a cluster's pooler.
// GET /api/v1/clusters/{id}/pooler/pools
func (h *PoolerHandler) GetPoolerPools(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	// Verify user has access to this cluster
	if err := h.clusterService.CheckOwnership(r.Context(), clusterID, user.ID); err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrForbidden) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
		h.logger.Error("failed to check cluster ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to verify access",
		})
		return
	}

	pools, err := h.poolerService.GetPoolerPools(r.Context(), clusterID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrPoolerNotEnabled):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Connection pooler is not enabled for this cluster",
			})
		case errors.Is(err, models.ErrPoolerNotReady):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Connection pooler is not ready",
			})
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrPoolerConnectionFailed):
			writeJSON(w, http.StatusServiceUnavailable, models.APIError{
				Code:    models.ErrCodeServiceUnavailable,
				Message: "Failed to connect to pooler",
			})
		default:
			h.logger.Error("failed to get pooler pools", "error", err, "cluster_id", clusterID)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to get pooler pools",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"pools": pools,
		"count": len(pools),
	})
}
