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

// InviteHandler handles invitation-related requests.
type InviteHandler struct {
	inviteService *services.InviteService
	orgService    *services.OrganizationService
	logger        *slog.Logger
}

// NewInviteHandler creates a new invite handler.
func NewInviteHandler(inviteService *services.InviteService, orgService *services.OrganizationService, logger *slog.Logger) *InviteHandler {
	return &InviteHandler{
		inviteService: inviteService,
		orgService:    orgService,
		logger:        logger.With("handler", "invite"),
	}
}

// CreateInvite creates a new invitation for an organization.
// POST /api/v1/organizations/{id}/invites
func (h *InviteHandler) CreateInvite(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgIDStr := chi.URLParam(r, "id")
	orgID, err := uuid.Parse(orgIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Check if user is admin or owner of the organization
	member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
	if err != nil {
		if errors.Is(err, models.ErrNotOrgMember) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Not a member of this organization",
			})
			return
		}
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to check membership",
		})
		return
	}

	if !member.Role.CanInviteMembers() {
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Insufficient permissions to invite members",
		})
		return
	}

	var req models.OrganizationInviteRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	if req.Email == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeValidation,
			Message: "Email is required",
		})
		return
	}

	// Default to member role if not specified
	if req.Role == "" {
		req.Role = models.OrgRoleMember
	}

	// Only owners can invite admins or owners
	if (req.Role == models.OrgRoleOwner || req.Role == models.OrgRoleAdmin) && member.Role != models.OrgRoleOwner {
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Only owners can invite admins or owners",
		})
		return
	}

	invite, err := h.inviteService.CreateInvite(r.Context(), orgID, user.ID, req.Email, req.Role)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrAlreadyMember):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "User is already a member of this organization",
			})
		case errors.Is(err, models.ErrInvalidRole):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: "Invalid role specified",
			})
		default:
			h.logger.Error("failed to create invitation", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to create invitation",
			})
		}
		return
	}

	// Include the token in response so it can be sent via email
	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"invite":     invite,
		"invite_url": "/invite/" + invite.Token,
	})
}

// ListInvites lists all pending invitations for an organization.
// GET /api/v1/organizations/{id}/invites
func (h *InviteHandler) ListInvites(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgIDStr := chi.URLParam(r, "id")
	orgID, err := uuid.Parse(orgIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	// Check membership
	member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
	if err != nil {
		if errors.Is(err, models.ErrNotOrgMember) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Not a member of this organization",
			})
			return
		}
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to check membership",
		})
		return
	}

	// Only admins and owners can view invites
	if !member.Role.CanInviteMembers() {
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Insufficient permissions to view invitations",
		})
		return
	}

	invites, err := h.inviteService.ListPendingInvites(r.Context(), orgID)
	if err != nil {
		h.logger.Error("failed to list invitations", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to list invitations",
		})
		return
	}

	// Convert to responses (hiding tokens for non-owners)
	responses := make([]*models.OrganizationInviteResponse, len(invites))
	for i, inv := range invites {
		responses[i] = inv.ToResponse()
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"invites": responses,
	})
}

// RevokeInvite revokes an invitation.
// DELETE /api/v1/organizations/{id}/invites/{inviteId}
func (h *InviteHandler) RevokeInvite(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgIDStr := chi.URLParam(r, "id")
	orgID, err := uuid.Parse(orgIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	inviteIDStr := chi.URLParam(r, "inviteId")
	inviteID, err := uuid.Parse(inviteIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid invite ID",
		})
		return
	}

	// Check membership and permissions
	member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
	if err != nil {
		if errors.Is(err, models.ErrNotOrgMember) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Not a member of this organization",
			})
			return
		}
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to check membership",
		})
		return
	}

	if !member.Role.CanInviteMembers() {
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Insufficient permissions to revoke invitations",
		})
		return
	}

	if err := h.inviteService.RevokeInvite(r.Context(), inviteID, orgID); err != nil {
		if errors.Is(err, models.ErrInviteNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Invitation not found",
			})
			return
		}
		h.logger.Error("failed to revoke invitation", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to revoke invitation",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"message": "Invitation revoked",
	})
}

// ResendInvite resends an invitation with a new token.
// POST /api/v1/organizations/{id}/invites/{inviteId}/resend
func (h *InviteHandler) ResendInvite(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	orgIDStr := chi.URLParam(r, "id")
	orgID, err := uuid.Parse(orgIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid organization ID",
		})
		return
	}

	inviteIDStr := chi.URLParam(r, "inviteId")
	inviteID, err := uuid.Parse(inviteIDStr)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid invite ID",
		})
		return
	}

	// Check membership and permissions
	member, err := h.orgService.CheckMembership(r.Context(), orgID, user.ID)
	if err != nil {
		if errors.Is(err, models.ErrNotOrgMember) {
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Not a member of this organization",
			})
			return
		}
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to check membership",
		})
		return
	}

	if !member.Role.CanInviteMembers() {
		writeJSON(w, http.StatusForbidden, models.APIError{
			Code:    models.ErrCodeForbidden,
			Message: "Insufficient permissions to resend invitations",
		})
		return
	}

	invite, err := h.inviteService.ResendInvite(r.Context(), inviteID, orgID)
	if err != nil {
		if errors.Is(err, models.ErrInviteNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Invitation not found",
			})
			return
		}
		h.logger.Error("failed to resend invitation", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to resend invitation",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"invite":     invite,
		"invite_url": "/invite/" + invite.Token,
	})
}

// GetInviteByToken retrieves an invitation by its token (public endpoint).
// GET /api/v1/invites/{token}
func (h *InviteHandler) GetInviteByToken(w http.ResponseWriter, r *http.Request) {
	token := chi.URLParam(r, "token")
	if token == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Token is required",
		})
		return
	}

	invite, err := h.inviteService.GetInviteByToken(r.Context(), token)
	if err != nil {
		if errors.Is(err, models.ErrInviteNotFound) {
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Invitation not found or expired",
			})
			return
		}
		h.logger.Error("failed to get invitation", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to get invitation",
		})
		return
	}

	// Check if expired
	if invite.AcceptedAt != nil {
		writeJSON(w, http.StatusGone, models.APIError{
			Code:    models.ErrCodeConflict,
			Message: "Invitation has already been used",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"invite": invite.ToResponse(),
	})
}

// AcceptInvite accepts an invitation and joins the organization.
// POST /api/v1/invites/{token}/accept
func (h *InviteHandler) AcceptInvite(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	token := chi.URLParam(r, "token")
	if token == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Token is required",
		})
		return
	}

	member, err := h.inviteService.AcceptInvite(r.Context(), token, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrInviteNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Invitation not found",
			})
		case errors.Is(err, models.ErrInviteExpired):
			writeJSON(w, http.StatusGone, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Invitation has expired",
			})
		case errors.Is(err, models.ErrInviteAlreadyUsed):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Invitation has already been used",
			})
		default:
			h.logger.Error("failed to accept invitation", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to accept invitation",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"member":  member,
		"message": "Successfully joined organization",
	})
}

// ListMyInvites lists all pending invitations for the current user's email.
// GET /api/v1/invites/me
func (h *InviteHandler) ListMyInvites(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	invites, err := h.inviteService.ListInvitesForEmail(r.Context(), user.Email)
	if err != nil {
		h.logger.Error("failed to list invitations", "error", err)
		writeJSON(w, http.StatusInternalServerError, models.APIError{
			Code:    models.ErrCodeInternal,
			Message: "Failed to list invitations",
		})
		return
	}

	// Convert to responses
	responses := make([]*models.OrganizationInviteResponse, len(invites))
	for i, inv := range invites {
		responses[i] = inv.ToResponse()
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"invites": responses,
	})
}
