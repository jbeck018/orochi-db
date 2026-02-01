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

// OrganizationHandler handles organization-related requests.
type OrganizationHandler struct {
	orgService *services.OrganizationService
	logger     *slog.Logger
}

// NewOrganizationHandler creates a new organization handler.
func NewOrganizationHandler(orgService *services.OrganizationService, logger *slog.Logger) *OrganizationHandler {
	return &OrganizationHandler{
		orgService: orgService,
		logger:     logger.With("handler", "organization"),
	}
}

// List returns all organizations for the authenticated user.
// GET /api/v1/organizations
func (h *OrganizationHandler) List(w http.ResponseWriter, r *http.Request) {
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

	response, err := h.orgService.ListByUser(r.Context(), user.ID, page, pageSize)
	if err != nil {
		h.logger.Error("failed to list organizations", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to list organizations",
		})
		return
	}

	writeJSON(w, http.StatusOK, response)
}

// Create creates a new organization.
// POST /api/v1/organizations
func (h *OrganizationHandler) Create(w http.ResponseWriter, r *http.Request) {
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

	var req models.OrganizationCreateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	org, err := h.orgService.Create(r.Context(), user.ID, &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrOrgNameRequired),
			errors.Is(err, models.ErrOrgNameInvalid):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrOrgAlreadyExists):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: err.Error(),
			})
		default:
			h.logger.Error("failed to create organization", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to create organization",
			})
		}
		return
	}

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"organization": org,
	})
}

// Get returns a specific organization.
// GET /api/v1/organizations/{id}
func (h *OrganizationHandler) Get(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Get org and verify user is a member
	org, err := h.orgService.GetByID(r.Context(), orgID)
	if err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		h.logger.Error("failed to get organization", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get organization",
		})
		return
	}

	// Check if user is owner or member
	if org.OwnerID != user.ID {
		_, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
		if err != nil {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"organization": org,
	})
}

// Update updates an organization.
// PATCH /api/v1/organizations/{id}
func (h *OrganizationHandler) Update(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Verify ownership
	if err := h.orgService.CheckOwnership(r.Context(), orgID, user.ID); err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Only the owner can update the organization",
		})
		return
	}

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req struct {
		Name string `json:"name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	org, err := h.orgService.Update(r.Context(), orgID, req.Name)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrOrgNameRequired):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrOrgNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
		default:
			h.logger.Error("failed to update organization", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to update organization",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"organization": org,
	})
}

// Delete deletes an organization.
// DELETE /api/v1/organizations/{id}
func (h *OrganizationHandler) Delete(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Verify ownership
	if err := h.orgService.CheckOwnership(r.Context(), orgID, user.ID); err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Only the owner can delete the organization",
		})
		return
	}

	if err := h.orgService.Delete(r.Context(), orgID); err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		h.logger.Error("failed to delete organization", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to delete organization",
		})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

// GetMembers returns all members of an organization.
// GET /api/v1/organizations/{id}/members
func (h *OrganizationHandler) GetMembers(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Verify user is a member
	org, err := h.orgService.GetByID(r.Context(), orgID)
	if err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		h.logger.Error("failed to get organization", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get organization",
		})
		return
	}

	if org.OwnerID != user.ID {
		_, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
		if err != nil {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
			return
		}
	}

	members, err := h.orgService.GetMembers(r.Context(), orgID)
	if err != nil {
		h.logger.Error("failed to get members", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get members",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"members": members,
	})
}

// AddMember adds a member to an organization.
// POST /api/v1/organizations/{id}/members
func (h *OrganizationHandler) AddMember(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Verify user is owner or admin
	org, err := h.orgService.GetByID(r.Context(), orgID)
	if err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		h.logger.Error("failed to get organization", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get organization",
		})
		return
	}

	if org.OwnerID != user.ID {
		member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
		if err != nil || !member.Role.CanInviteMembers() {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Insufficient permissions to add members",
			})
			return
		}
	}

	// Limit request body size to 1MB
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	var req struct {
		UserID uuid.UUID                     `json:"user_id"`
		Role   models.OrganizationMemberRole `json:"role"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	// Default to member role if not specified
	if req.Role == "" {
		req.Role = models.OrgRoleMember
	}

	if err := h.orgService.AddMember(r.Context(), orgID, req.UserID, req.Role); err != nil {
		h.logger.Error("failed to add member", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to add member",
		})
		return
	}

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"message": "Member added successfully",
	})
}

// RemoveMember removes a member from an organization.
// DELETE /api/v1/organizations/{id}/members/{memberId}
func (h *OrganizationHandler) RemoveMember(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgID, err := uuid.Parse(chi.URLParam(r, "id"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	memberID, err := uuid.Parse(chi.URLParam(r, "memberId"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid member ID",
		})
		return
	}

	// Verify user is owner or admin
	org, err := h.orgService.GetByID(r.Context(), orgID)
	if err != nil {
		if errors.Is(err, models.ErrOrgNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Organization not found",
			})
			return
		}
		h.logger.Error("failed to get organization", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get organization",
		})
		return
	}

	// Can't remove the owner
	if memberID == org.OwnerID {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Cannot remove the organization owner",
		})
		return
	}

	// Check permissions - owner can remove anyone, admins can remove members/viewers
	if org.OwnerID != user.ID {
		member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
		if err != nil || !member.Role.CanInviteMembers() {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Insufficient permissions to remove members",
			})
			return
		}
	}

	if err := h.orgService.RemoveMember(r.Context(), orgID, memberID); err != nil {
		if errors.Is(err, models.ErrNotOrgMember) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Member not found",
			})
			return
		}
		h.logger.Error("failed to remove member", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to remove member",
		})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}
