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

// ClusterSettingsHandler handles cluster settings requests.
type ClusterSettingsHandler struct {
	settingsService *services.ClusterSettingsService
	clusterService  *services.ClusterService
	logger          *slog.Logger
}

// NewClusterSettingsHandler creates a new cluster settings handler.
func NewClusterSettingsHandler(settingsService *services.ClusterSettingsService, clusterService *services.ClusterService, logger *slog.Logger) *ClusterSettingsHandler {
	return &ClusterSettingsHandler{
		settingsService: settingsService,
		clusterService:  clusterService,
		logger:          logger.With("handler", "cluster_settings"),
	}
}

// getClusterForUser retrieves a cluster and validates the user has access.
func (h *ClusterSettingsHandler) getClusterForUser(r *http.Request, user *models.User) (*models.Cluster, *models.APIError) {
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

// GetSettings retrieves settings for a cluster.
// GET /api/v1/clusters/{id}/settings
func (h *ClusterSettingsHandler) GetSettings(w http.ResponseWriter, r *http.Request) {
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
		status := http.StatusBadRequest
		if apiErr.Code == models.ErrCodeNotFound {
			status = http.StatusNotFound
		}
		writeJSON(w, status, apiErr)
		return
	}

	settings, err := h.settingsService.GetSettings(r.Context(), cluster.ID)
	if err != nil {
		h.logger.Error("failed to get cluster settings", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get cluster settings",
		})
		return
	}

	writeJSON(w, http.StatusOK, settings)
}

// UpdateSettings updates settings for a cluster.
// PATCH /api/v1/clusters/{id}/settings
func (h *ClusterSettingsHandler) UpdateSettings(w http.ResponseWriter, r *http.Request) {
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
		status := http.StatusBadRequest
		if apiErr.Code == models.ErrCodeNotFound {
			status = http.StatusNotFound
		}
		writeJSON(w, status, apiErr)
		return
	}

	var req models.ClusterSettingsUpdateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	settings, err := h.settingsService.CreateOrUpdateSettings(r.Context(), cluster.ID, &req)
	if err != nil {
		h.logger.Error("failed to update cluster settings", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to update cluster settings",
		})
		return
	}

	writeJSON(w, http.StatusOK, settings)
}

// GetRecommendations retrieves performance recommendations for a cluster.
// GET /api/v1/clusters/{id}/recommendations
func (h *ClusterSettingsHandler) GetRecommendations(w http.ResponseWriter, r *http.Request) {
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
		status := http.StatusBadRequest
		if apiErr.Code == models.ErrCodeNotFound {
			status = http.StatusNotFound
		}
		writeJSON(w, status, apiErr)
		return
	}

	recommendations, err := h.settingsService.GetRecommendations(r.Context(), cluster.ID)
	if err != nil {
		h.logger.Error("failed to get recommendations", "error", err, "cluster_id", cluster.ID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get recommendations",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"recommendations": recommendations,
		"count":           len(recommendations),
	})
}

// DismissRecommendation marks a recommendation as dismissed.
// POST /api/v1/clusters/{id}/recommendations/{recId}/dismiss
func (h *ClusterSettingsHandler) DismissRecommendation(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	_, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		status := http.StatusBadRequest
		if apiErr.Code == models.ErrCodeNotFound {
			status = http.StatusNotFound
		}
		writeJSON(w, status, apiErr)
		return
	}

	recIDStr := chi.URLParam(r, "recId")
	recID, err := uuid.Parse(recIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid recommendation ID",
		})
		return
	}

	if err := h.settingsService.DismissRecommendation(r.Context(), recID); err != nil {
		h.logger.Error("failed to dismiss recommendation", "error", err, "recommendation_id", recID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to dismiss recommendation",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{
		"message": "Recommendation dismissed",
	})
}

// ApplyRecommendation marks a recommendation as applied.
// POST /api/v1/clusters/{id}/recommendations/{recId}/apply
func (h *ClusterSettingsHandler) ApplyRecommendation(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	_, apiErr := h.getClusterForUser(r, user)
	if apiErr != nil {
		status := http.StatusBadRequest
		if apiErr.Code == models.ErrCodeNotFound {
			status = http.StatusNotFound
		}
		writeJSON(w, status, apiErr)
		return
	}

	recIDStr := chi.URLParam(r, "recId")
	recID, err := uuid.Parse(recIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid recommendation ID",
		})
		return
	}

	if err := h.settingsService.ApplyRecommendation(r.Context(), recID); err != nil {
		h.logger.Error("failed to apply recommendation", "error", err, "recommendation_id", recID)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to apply recommendation",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{
		"message": "Recommendation applied",
	})
}
