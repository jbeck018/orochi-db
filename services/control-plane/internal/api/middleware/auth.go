// Package middleware provides HTTP middleware for the control plane API.
package middleware

import (
	"encoding/json"
	"net/http"
	"strings"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// AuthMiddleware provides JWT authentication middleware.
type AuthMiddleware struct {
	jwtManager  *auth.JWTManager
	userService *services.UserService
}

// NewAuthMiddleware creates a new auth middleware.
func NewAuthMiddleware(jwtManager *auth.JWTManager, userService *services.UserService) *AuthMiddleware {
	return &AuthMiddleware{
		jwtManager:  jwtManager,
		userService: userService,
	}
}

// Authenticate validates the JWT token and adds user info to the request context.
func (m *AuthMiddleware) Authenticate(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		tokenString := extractToken(r)
		if tokenString == "" {
			writeError(w, http.StatusUnauthorized, models.ErrCodeUnauthorized, "Authorization token is required")
			return
		}

		claims, err := m.jwtManager.ValidateAccessToken(tokenString)
		if err != nil {
			writeError(w, http.StatusUnauthorized, models.ErrCodeUnauthorized, "Invalid or expired token")
			return
		}

		// Optionally load full user object
		user, err := m.userService.GetByID(r.Context(), claims.UserID)
		if err != nil {
			writeError(w, http.StatusUnauthorized, models.ErrCodeUnauthorized, "User not found")
			return
		}

		if !user.Active {
			writeError(w, http.StatusForbidden, models.ErrCodeForbidden, "User account is inactive")
			return
		}

		// Add claims and user to context
		ctx := auth.ContextWithClaims(r.Context(), claims)
		ctx = auth.ContextWithUser(ctx, user)

		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// RequireRole checks if the user has one of the required roles.
func (m *AuthMiddleware) RequireRole(roles ...models.UserRole) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, ok := auth.ClaimsFromContext(r.Context())
			if !ok {
				writeError(w, http.StatusUnauthorized, models.ErrCodeUnauthorized, "Authentication required")
				return
			}

			hasRole := false
			for _, role := range roles {
				if claims.Role == role {
					hasRole = true
					break
				}
			}

			if !hasRole {
				writeError(w, http.StatusForbidden, models.ErrCodeForbidden, "Insufficient permissions")
				return
			}

			next.ServeHTTP(w, r)
		})
	}
}

// OptionalAuth attempts to authenticate but allows anonymous access.
func (m *AuthMiddleware) OptionalAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		tokenString := extractToken(r)
		if tokenString == "" {
			next.ServeHTTP(w, r)
			return
		}

		claims, err := m.jwtManager.ValidateAccessToken(tokenString)
		if err != nil {
			// Invalid token, but we allow the request to continue without auth
			next.ServeHTTP(w, r)
			return
		}

		user, err := m.userService.GetByID(r.Context(), claims.UserID)
		if err != nil || !user.Active {
			next.ServeHTTP(w, r)
			return
		}

		ctx := auth.ContextWithClaims(r.Context(), claims)
		ctx = auth.ContextWithUser(ctx, user)

		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// extractToken extracts the JWT token from the Authorization header.
func extractToken(r *http.Request) string {
	authHeader := r.Header.Get("Authorization")
	if authHeader == "" {
		return ""
	}

	parts := strings.SplitN(authHeader, " ", 2)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return ""
	}

	return parts[1]
}

// writeError writes a JSON error response.
func writeError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(models.APIError{
		Code:    code,
		Message: message,
	})
}
