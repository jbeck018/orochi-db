// Package handlers provides HTTP handlers for the control plane API.
package handlers

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"

	"github.com/orochi-db/orochi-cloud/control-plane/internal/auth"
	"github.com/orochi-db/orochi-cloud/control-plane/internal/models"
	"github.com/orochi-db/orochi-cloud/control-plane/internal/services"
)

// AuthHandler handles authentication-related requests.
type AuthHandler struct {
	userService *services.UserService
	logger      *slog.Logger
}

// NewAuthHandler creates a new auth handler.
func NewAuthHandler(userService *services.UserService, logger *slog.Logger) *AuthHandler {
	return &AuthHandler{
		userService: userService,
		logger:      logger.With("handler", "auth"),
	}
}

// Register handles user registration.
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

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"user": user.ToResponse(),
	})
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
