// Package handlers provides HTTP handlers for the control plane API.
package handlers

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// AuthHandler handles authentication-related requests.
type AuthHandler struct {
	userService         *services.UserService
	organizationService *services.OrganizationService
	inviteService       *services.InviteService
	logger              *slog.Logger
}

// NewAuthHandler creates a new auth handler.
func NewAuthHandler(userService *services.UserService, organizationService *services.OrganizationService, inviteService *services.InviteService, logger *slog.Logger) *AuthHandler {
	return &AuthHandler{
		userService:         userService,
		organizationService: organizationService,
		inviteService:       inviteService,
		logger:              logger.With("handler", "auth"),
	}
}

// Register handles user registration with organization-required flow.
// Users must either create a new organization or provide an invite token.
// POST /api/v1/auth/register
func (h *AuthHandler) Register(w http.ResponseWriter, r *http.Request) {
	var req models.UserCreateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	// Validate organization requirement: must have either org name or invite token
	hasOrgName := req.OrganizationName != ""
	hasInviteToken := req.InviteToken != nil && *req.InviteToken != ""

	if !hasOrgName && !hasInviteToken {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeValidation,
			Message: "Organization name or invite token is required",
		})
		return
	}

	// If using invite token, validate it first before creating user
	var invite *models.OrganizationInvite
	if hasInviteToken {
		var err error
		invite, err = h.inviteService.GetInviteByToken(r.Context(), *req.InviteToken)
		if err != nil {
			if errors.Is(err, models.ErrInviteNotFound) {
				writeJSON(w, http.StatusBadRequest, models.APIError{
					Code:    models.ErrCodeValidation,
					Message: "Invalid or expired invitation token",
				})
				return
			}
			h.logger.Error("failed to validate invite token", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to validate invitation",
			})
			return
		}

		// Check if invite is already used
		if invite.AcceptedAt != nil {
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: "Invitation has already been used",
			})
			return
		}

		// Check if invite email matches registration email
		if invite.Email != req.Email {
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: "Email does not match the invitation",
			})
			return
		}
	}

	// Register the user
	user, err := h.userService.Register(r.Context(), &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrEmailRequired),
			errors.Is(err, models.ErrPasswordRequired),
			errors.Is(err, models.ErrPasswordTooShort),
			errors.Is(err, models.ErrNameRequired):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrUserExists):
			writeJSON(w, http.StatusConflict, models.APIError{
				Code:    models.ErrCodeConflict,
				Message: err.Error(),
			})
		default:
			h.logger.Error("registration failed", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Registration failed",
			})
		}
		return
	}

	var organization *models.Organization
	var member *models.OrganizationMember

	// Handle organization: either create new or join via invite
	if hasOrgName {
		// Create new organization with user as owner
		org, err := h.organizationService.Create(r.Context(), user.ID, &models.OrganizationCreateRequest{
			Name: req.OrganizationName,
		})
		if err != nil {
			h.logger.Error("failed to create organization during registration", "error", err, "user_id", user.ID)
			// User is created but org failed - log but don't fail the registration
			// The user can create an org later
			writeJSON(w, http.StatusCreated, map[string]interface{}{
				"user":    user.ToResponse(),
				"warning": "User created but organization creation failed. Please create an organization from the dashboard.",
			})
			return
		}
		organization = org

		// Get member info (user is automatically added as owner)
		member, _ = h.organizationService.CheckMembership(r.Context(), org.ID, user.ID)
	} else if hasInviteToken && invite != nil {
		// Accept the invitation
		m, err := h.inviteService.AcceptInvite(r.Context(), *req.InviteToken, user.ID)
		if err != nil {
			h.logger.Error("failed to accept invitation during registration", "error", err, "user_id", user.ID)
			writeJSON(w, http.StatusCreated, map[string]interface{}{
				"user":    user.ToResponse(),
				"warning": "User created but failed to join organization. Please try accepting the invitation again.",
			})
			return
		}
		member = m

		// Get the organization
		organization, _ = h.organizationService.GetByID(r.Context(), invite.OrganizationID)
	}

	response := map[string]interface{}{
		"user": user.ToResponse(),
	}
	if organization != nil {
		response["organization"] = organization
	}
	if member != nil {
		response["membership"] = member
	}

	writeJSON(w, http.StatusCreated, response)
}

// Login handles user login.
// POST /api/v1/auth/login
func (h *AuthHandler) Login(w http.ResponseWriter, r *http.Request) {
	var req models.UserLoginRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	response, err := h.userService.Login(r.Context(), &req)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrEmailRequired),
			errors.Is(err, models.ErrPasswordRequired):
			writeJSON(w, http.StatusBadRequest, models.APIError{
				Code:    models.ErrCodeValidation,
				Message: err.Error(),
			})
		case errors.Is(err, models.ErrInvalidCredentials):
			writeJSON(w, http.StatusUnauthorized, models.APIError{
				Code:    models.ErrCodeUnauthorized,
				Message: "Invalid email or password",
			})
		case errors.Is(err, models.ErrUserInactive):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Account is inactive",
			})
		default:
			h.logger.Error("login failed", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Login failed",
			})
		}
		return
	}

	// Convert user to response format
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"access_token":  response.AccessToken,
		"refresh_token": response.RefreshToken,
		"token_type":    response.TokenType,
		"expires_in":    response.ExpiresIn,
		"user":          response.User.ToResponse(),
	})
}

// RefreshToken handles token refresh.
// POST /api/v1/auth/refresh
func (h *AuthHandler) RefreshToken(w http.ResponseWriter, r *http.Request) {
	var req struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	if req.RefreshToken == "" {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeValidation,
			Message: "Refresh token is required",
		})
		return
	}

	response, err := h.userService.RefreshTokens(r.Context(), req.RefreshToken)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrTokenInvalid):
			writeJSON(w, http.StatusUnauthorized, models.APIError{
				Code:    models.ErrCodeUnauthorized,
				Message: "Invalid or expired refresh token",
			})
		case errors.Is(err, models.ErrUserInactive):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Account is inactive",
			})
		default:
			h.logger.Error("token refresh failed", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Token refresh failed",
			})
		}
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"access_token":  response.AccessToken,
		"refresh_token": response.RefreshToken,
		"token_type":    response.TokenType,
		"expires_in":    response.ExpiresIn,
	})
}

// GetCurrentUser returns the currently authenticated user.
// GET /api/v1/auth/me
func (h *AuthHandler) GetCurrentUser(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"user": user.ToResponse(),
	})
}

// writeJSON writes a JSON response.
func writeJSON(w http.ResponseWriter, status int, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(data); err != nil {
		// Log error but can't do much at this point
		slog.Error("failed to encode JSON response", "error", err)
	}
}
