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

// ClusterHandler handles cluster-related requests.
type ClusterHandler struct {
	clusterService *services.ClusterService
	logger         *slog.Logger
}

// NewClusterHandler creates a new cluster handler.
func NewClusterHandler(clusterService *services.ClusterService, logger *slog.Logger) *ClusterHandler {
	return &ClusterHandler{
		clusterService: clusterService,
		logger:         logger.With("handler", "cluster"),
	}
}

// List returns all clusters for the authenticated user.
// GET /api/v1/clusters
func (h *ClusterHandler) List(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	// Parse pagination parameters
	page, _ := strconv.Atoi(r.URL.Query().Get("page"))
	pageSize, _ := strconv.Atoi(r.URL.Query().Get("page_size"))

	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}

	response, err := h.clusterService.List(r.Context(), user.ID, page, pageSize)
	if err != nil {
		h.logger.Error("failed to list clusters", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to list clusters",
		})
		return
	}

	writeJSON(w, http.StatusOK, response)
}

// Create creates a new cluster.
// POST /api/v1/clusters
func (h *ClusterHandler) Create(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.ClusterCreateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	cluster, err := h.clusterService.Create(r.Context(), user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNameRequired),
			errors.Is(err, models.ErrClusterNameInvalid),
			errors.Is(err, models.ErrClusterTierRequired),
			errors.Is(err, models.ErrClusterProviderRequired),
			errors.Is(err, models.ErrClusterRegionRequired):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrClusterAlreadyExists):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: err.Error(),
			})
		default:
			h.logger.Error("failed to create cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to create cluster",
			})
		}
		return
	}

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"cluster": cluster,
	})
}

// Get returns a specific cluster.
// GET /api/v1/clusters/{id}
func (h *ClusterHandler) Get(w http.ResponseWriter, r *http.Request) {
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

	// Get cluster with ownership check in a single query (avoids N+1)
	cluster, err := h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterID, user.ID)
	if err != nil {
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
		h.logger.Error("failed to get cluster", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"cluster": cluster,
	})
}

// Update updates a cluster's configuration.
// PATCH /api/v1/clusters/{id}
func (h *ClusterHandler) Update(w http.ResponseWriter, r *http.Request) {
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

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.ClusterUpdateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	// Update with ownership check in a single transaction (avoids N+1)
	cluster, err := h.clusterService.UpdateWithOwnerCheck(r.Context(), clusterID, user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		case errors.Is(err, models.ErrClusterOperationPending):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Another operation is in progress",
			})
		default:
			h.logger.Error("failed to update cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to update cluster",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"cluster": cluster,
	})
}

// Delete deletes a cluster.
// DELETE /api/v1/clusters/{id}
func (h *ClusterHandler) Delete(w http.ResponseWriter, r *http.Request) {
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

	// Delete with ownership check in a single transaction (avoids N+1)
	if err := h.clusterService.DeleteWithOwnerCheck(r.Context(), clusterID, user.ID); err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to delete cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to delete cluster",
			})
		}
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

// Scale scales a cluster.
// POST /api/v1/clusters/{id}/scale
func (h *ClusterHandler) Scale(w http.ResponseWriter, r *http.Request) {
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

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.ClusterScaleRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	// Scale with ownership check in a single transaction (avoids N+1)
	cluster, err := h.clusterService.ScaleWithOwnerCheck(r.Context(), clusterID, user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		case errors.Is(err, models.ErrInvalidNodeCount),
			errors.Is(err, models.ErrNodeCountTooHigh):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrClusterNotRunning):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster must be running to scale",
			})
		default:
			h.logger.Error("failed to scale cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to scale cluster",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"cluster": cluster,
	})
}

// Suspend suspends a cluster (scale-to-zero).
// POST /api/v1/clusters/{id}/suspend
func (h *ClusterHandler) Suspend(w http.ResponseWriter, r *http.Request) {
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

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.SuspendClusterRequest
	// Allow empty body with defaults
	json.NewDecoder(r.Body).Decode(&req)

	err = h.clusterService.SuspendCluster(r.Context(), clusterID, user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		case errors.Is(err, models.ErrClusterAlreadySuspended):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is already suspended",
			})
		case errors.Is(err, models.ErrClusterSuspending):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is currently suspending",
			})
		case errors.Is(err, models.ErrClusterWaking):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is currently waking",
			})
		case errors.Is(err, models.ErrClusterOperationPending):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Another operation is in progress",
			})
		default:
			h.logger.Error("failed to suspend cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to suspend cluster",
			})
		}
		return
	}

	writeJSON(w, http.StatusAccepted, map[string]interface{}{
		"message": "Cluster suspend initiated",
		"status":  models.ClusterStatusSuspending,
	})
}

// Wake wakes a suspended cluster.
// POST /api/v1/clusters/{id}/wake
func (h *ClusterHandler) Wake(w http.ResponseWriter, r *http.Request) {
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

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req models.WakeClusterRequest
	// Allow empty body with defaults
	json.NewDecoder(r.Body).Decode(&req)

	state, err := h.clusterService.WakeCluster(r.Context(), clusterID, user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		case errors.Is(err, models.ErrClusterNotSuspended):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is not suspended",
			})
		case errors.Is(err, models.ErrClusterSuspending):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is currently suspending",
			})
		case errors.Is(err, models.ErrWakeTimeout):
			// Still return state but with error status
			writeJSON(w, http.StatusGatewayTimeout, map[string]interface{}{
				"state":   state,
				"message": "Wake timeout - cluster may still be starting",
			})
		default:
			h.logger.Error("failed to wake cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to wake cluster",
			})
		}
		return
	}

	statusCode := http.StatusAccepted
	if state.IsReady {
		statusCode = http.StatusOK
	}

	writeJSON(w, statusCode, map[string]interface{}{
		"state": state,
	})
}

// GetState returns the current state of a cluster.
// GET /api/v1/clusters/{id}/state
func (h *ClusterHandler) GetState(w http.ResponseWriter, r *http.Request) {
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

	// Verify ownership first
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
		h.logger.Error("failed to check ownership", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster state",
		})
		return
	}

	state, err := h.clusterService.GetClusterState(r.Context(), clusterID)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		h.logger.Error("failed to get cluster state", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster state",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"state": state,
	})
}

// GetStateInternal returns the cluster state for internal service-to-service calls.
// This endpoint is used by the JWT gateway for wake-on-connect.
// GET /api/v1/internal/clusters/{id}/state
func (h *ClusterHandler) GetStateInternal(w http.ResponseWriter, r *http.Request) {
	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	state, err := h.clusterService.GetClusterState(r.Context(), clusterID)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		h.logger.Error("failed to get cluster state (internal)", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster state",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"state": state,
	})
}

// WakeInternal triggers a wake for internal service-to-service calls.
// This endpoint is used by the JWT gateway for wake-on-connect.
// POST /api/v1/internal/clusters/{id}/wake
func (h *ClusterHandler) WakeInternal(w http.ResponseWriter, r *http.Request) {
	clusterID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID",
		})
		return
	}

	state, err := h.clusterService.TriggerWakeForConnection(r.Context(), clusterID)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		if errors.Is(err, models.ErrClusterSuspending) {
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Cluster is currently suspending",
			})
			return
		}
		h.logger.Error("failed to trigger wake (internal)", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to wake cluster",
		})
		return
	}

	statusCode := http.StatusAccepted
	if state.IsReady {
		statusCode = http.StatusOK
	}

	writeJSON(w, statusCode, map[string]interface{}{
		"state": state,
	})
}
