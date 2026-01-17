package handlers

import (
	"errors"
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// MetricsHandler handles metrics-related requests.
type MetricsHandler struct {
	clusterService *services.ClusterService
	logger         *slog.Logger
}

// NewMetricsHandler creates a new metrics handler.
func NewMetricsHandler(clusterService *services.ClusterService, logger *slog.Logger) *MetricsHandler {
	return &MetricsHandler{
		clusterService: clusterService,
		logger:         logger.With("handler", "metrics"),
	}
}

// GetClusterMetrics returns metrics for a cluster.
// GET /api/v1/clusters/{id}/metrics
func (h *MetricsHandler) GetClusterMetrics(w http.ResponseWriter, r *http.Request) {
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

	// Check ownership
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
			Message: "Failed to get metrics",
		})
		return
	}

	// Parse time range from query parameters
	fromStr := r.URL.Query().Get("from")
	toStr := r.URL.Query().Get("to")

	var from, to time.Time
	var parseErr error

	if fromStr != "" {
		from, parseErr = time.Parse(time.RFC3339, fromStr)
		if parseErr != nil {
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Invalid 'from' time format. Use RFC3339 format.",
			})
			return
		}
	} else {
		// Default to last hour
		from = time.Now().Add(-1 * time.Hour)
	}

	if toStr != "" {
		to, parseErr = time.Parse(time.RFC3339, toStr)
		if parseErr != nil {
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeBadRequest,
				Message: "Invalid 'to' time format. Use RFC3339 format.",
			})
			return
		}
	} else {
		to = time.Now()
	}

	// Validate time range
	if from.After(to) {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "'from' must be before 'to'",
		})
		return
	}

	// Limit time range to 7 days
	if to.Sub(from) > 7*24*time.Hour {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Time range cannot exceed 7 days",
		})
		return
	}

	metrics, err := h.clusterService.GetMetrics(r.Context(), clusterID, from, to)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		h.logger.Error("failed to get metrics", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get metrics",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"metrics": metrics,
		"from":    from,
		"to":      to,
	})
}

// GetLatestMetrics returns the most recent metrics for a cluster.
// GET /api/v1/clusters/{id}/metrics/latest
func (h *MetricsHandler) GetLatestMetrics(w http.ResponseWriter, r *http.Request) {
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

	// Check ownership
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
			Message: "Failed to get metrics",
		})
		return
	}

	metrics, err := h.clusterService.GetLatestMetrics(r.Context(), clusterID)
	if err != nil {
		if errors.Is(err, models.ErrClusterNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
			return
		}
		h.logger.Error("failed to get latest metrics", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get metrics",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"metrics": metrics,
	})
}

// HealthHandler handles health check requests.
type HealthHandler struct {
	logger *slog.Logger
}

// NewHealthHandler creates a new health handler.
func NewHealthHandler(logger *slog.Logger) *HealthHandler {
	return &HealthHandler{
		logger: logger.With("handler", "health"),
	}
}

// Health returns the health status of the API.
// GET /health
func (h *HealthHandler) Health(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"status":    "healthy",
		"timestamp": time.Now().UTC(),
	})
}

// Ready returns the readiness status of the API.
// GET /ready
func (h *HealthHandler) Ready(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"status":    "ready",
		"timestamp": time.Now().UTC(),
	})
}
