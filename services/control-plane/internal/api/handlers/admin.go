// Package handlers provides HTTP request handlers for the control plane API.
package handlers

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// AdminHandler handles admin-related HTTP requests.
type AdminHandler struct {
	adminService *services.AdminService
	logger       *slog.Logger
}

// NewAdminHandler creates a new admin handler.
func NewAdminHandler(adminService *services.AdminService, logger *slog.Logger) *AdminHandler {
	return &AdminHandler{
		adminService: adminService,
		logger:       logger.With("handler", "admin"),
	}
}

// GetStats returns platform-wide statistics.
func (h *AdminHandler) GetStats(w http.ResponseWriter, r *http.Request) {
	stats, err := h.adminService.GetStats(r.Context())
	if err != nil {
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data":    stats,
	})
}

// ListUsers returns all users with pagination.
func (h *AdminHandler) ListUsers(w http.ResponseWriter, r *http.Request) {
	page, _ := strconv.Atoi(r.URL.Query().Get("page"))
	pageSize, _ := strconv.Atoi(r.URL.Query().Get("page_size"))
	search := r.URL.Query().Get("search")

	if page < 1 {
		page = 1
	}
	if pageSize < 1 {
		pageSize = 20
	}

	users, total, err := h.adminService.ListAllUsers(r.Context(), page, pageSize, search)
	if err != nil {
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data": map[string]any{
			"users":       users,
			"total_count": total,
			"page":        page,
			"page_size":   pageSize,
		},
	})
}

// GetUser returns a specific user by ID.
func (h *AdminHandler) GetUser(w http.ResponseWriter, r *http.Request) {
	idParam := chi.URLParam(r, "id")
	id, err := uuid.Parse(idParam)
	if err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid user ID")
		return
	}

	user, err := h.adminService.GetUserByIDAdmin(r.Context(), id)
	if err != nil {
		if err == models.ErrUserNotFound {
			h.writeError(w, http.StatusNotFound, models.ErrCodeNotFound, "User not found")
			return
		}
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data":    user,
	})
}

// UpdateUserRoleRequest represents a request to update a user's role.
type UpdateUserRoleRequest struct {
	Role models.UserRole `json:"role"`
}

// UpdateUserRole updates a user's role.
func (h *AdminHandler) UpdateUserRole(w http.ResponseWriter, r *http.Request) {
	idParam := chi.URLParam(r, "id")
	id, err := uuid.Parse(idParam)
	if err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid user ID")
		return
	}

	var req UpdateUserRoleRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid request body")
		return
	}

	// Validate role
	if req.Role != models.UserRoleAdmin && req.Role != models.UserRoleMember && req.Role != models.UserRoleViewer {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeValidation, "Invalid role")
		return
	}

	if err := h.adminService.UpdateUserRole(r.Context(), id, req.Role); err != nil {
		if err == models.ErrUserNotFound {
			h.writeError(w, http.StatusNotFound, models.ErrCodeNotFound, "User not found")
			return
		}
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"message": "User role updated",
	})
}

// SetUserActiveRequest represents a request to set user active status.
type SetUserActiveRequest struct {
	Active bool `json:"active"`
}

// SetUserActive activates or deactivates a user.
func (h *AdminHandler) SetUserActive(w http.ResponseWriter, r *http.Request) {
	idParam := chi.URLParam(r, "id")
	id, err := uuid.Parse(idParam)
	if err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid user ID")
		return
	}

	var req SetUserActiveRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid request body")
		return
	}

	if err := h.adminService.SetUserActive(r.Context(), id, req.Active); err != nil {
		if err == models.ErrUserNotFound {
			h.writeError(w, http.StatusNotFound, models.ErrCodeNotFound, "User not found")
			return
		}
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	status := "activated"
	if !req.Active {
		status = "deactivated"
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"message": "User " + status,
	})
}

// ListClusters returns all clusters with pagination.
func (h *AdminHandler) ListClusters(w http.ResponseWriter, r *http.Request) {
	page, _ := strconv.Atoi(r.URL.Query().Get("page"))
	pageSize, _ := strconv.Atoi(r.URL.Query().Get("page_size"))
	status := r.URL.Query().Get("status")

	if page < 1 {
		page = 1
	}
	if pageSize < 1 {
		pageSize = 20
	}

	clusters, total, err := h.adminService.ListAllClusters(r.Context(), page, pageSize, status)
	if err != nil {
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data": map[string]any{
			"clusters":    clusters,
			"total_count": total,
			"page":        page,
			"page_size":   pageSize,
		},
	})
}

// GetCluster returns a specific cluster by ID.
func (h *AdminHandler) GetCluster(w http.ResponseWriter, r *http.Request) {
	idParam := chi.URLParam(r, "id")
	id, err := uuid.Parse(idParam)
	if err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid cluster ID")
		return
	}

	cluster, err := h.adminService.GetClusterByIDAdmin(r.Context(), id)
	if err != nil {
		if err == models.ErrClusterNotFound {
			h.writeError(w, http.StatusNotFound, models.ErrCodeNotFound, "Cluster not found")
			return
		}
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data":    cluster,
	})
}

// ForceDeleteCluster hard deletes a cluster.
func (h *AdminHandler) ForceDeleteCluster(w http.ResponseWriter, r *http.Request) {
	idParam := chi.URLParam(r, "id")
	id, err := uuid.Parse(idParam)
	if err != nil {
		h.writeError(w, http.StatusBadRequest, models.ErrCodeBadRequest, "Invalid cluster ID")
		return
	}

	if err := h.adminService.ForceDeleteCluster(r.Context(), id); err != nil {
		if err == models.ErrClusterNotFound {
			h.writeError(w, http.StatusNotFound, models.ErrCodeNotFound, "Cluster not found")
			return
		}
		h.writeError(w, http.StatusInternalServerError, models.ErrCodeInternal, err.Error())
		return
	}

	h.writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"message": "Cluster permanently deleted",
	})
}

// writeJSON writes a JSON response.
func (h *AdminHandler) writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}

// writeError writes a JSON error response.
func (h *AdminHandler) writeError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(models.APIError{
		Code:    code,
		Message: message,
	})
}
