// Package auth provides authentication utilities for the control plane.
package auth

import (
	"context"
	"errors"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
	"golang.org/x/crypto/bcrypt"

	"github.com/orochi-db/orochi-cloud/control-plane/internal/models"
	"github.com/orochi-db/orochi-cloud/control-plane/pkg/config"
)

// MinSecretLength is the minimum required length for JWT secrets (256 bits = 32 bytes).
const MinSecretLength = 32

// TokenType represents the type of JWT token.
type TokenType string

const (
	TokenTypeAccess  TokenType = "access"
	TokenTypeRefresh TokenType = "refresh"
)

// Claims represents the JWT claims for user tokens.
type Claims struct {
	jwt.RegisteredClaims
	UserID    uuid.UUID       `json:"user_id"`
	Email     string          `json:"email"`
	Role      models.UserRole `json:"role"`
	TokenType TokenType       `json:"token_type"`
}

// JWTManager handles JWT token operations.
type JWTManager struct {
	secret          []byte
	issuer          string
	accessTokenTTL  time.Duration
	refreshTokenTTL time.Duration
	clockSkewLeeway time.Duration
}

// NewJWTManager creates a new JWT manager with validated configuration.
// Returns an error if the secret is empty or too short.
func NewJWTManager(cfg *config.JWTConfig) (*JWTManager, error) {
	// Validate secret is not empty
	if cfg.Secret == "" {
		return nil, models.ErrJWTSecretEmpty
	}

	// Validate secret meets minimum length requirement for HS256
	if len(cfg.Secret) < MinSecretLength {
		return nil, models.ErrJWTSecretTooShort
	}

	// Default clock skew leeway to 30 seconds if not set
	leeway := cfg.ClockSkewLeeway
	if leeway == 0 {
		leeway = 30 * time.Second
	}

	return &JWTManager{
		secret:          []byte(cfg.Secret),
		issuer:          cfg.Issuer,
		accessTokenTTL:  cfg.AccessTokenTTL,
		refreshTokenTTL: cfg.RefreshTokenTTL,
		clockSkewLeeway: leeway,
	}, nil
}

// GenerateAccessToken generates a new access token for a user.
func (m *JWTManager) GenerateAccessToken(user *models.User) (string, error) {
	return m.generateToken(user, TokenTypeAccess, m.accessTokenTTL)
}

// GenerateRefreshToken generates a new refresh token for a user.
func (m *JWTManager) GenerateRefreshToken(user *models.User) (string, error) {
	return m.generateToken(user, TokenTypeRefresh, m.refreshTokenTTL)
}

// generateToken generates a JWT token with the specified parameters.
func (m *JWTManager) generateToken(user *models.User, tokenType TokenType, ttl time.Duration) (string, error) {
	now := time.Now()
	claims := &Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer:    m.issuer,
			Subject:   user.ID.String(),
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(ttl)),
			NotBefore: jwt.NewNumericDate(now),
			ID:        uuid.New().String(),
		},
		UserID:    user.ID,
		Email:     user.Email,
		Role:      user.Role,
		TokenType: tokenType,
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString(m.secret)
}

// ValidateToken validates a JWT token and returns the claims.
// It performs comprehensive validation including:
// - Signature verification
// - Expiry time check with clock skew tolerance
// - Not-before time check with clock skew tolerance
// - Issuer validation
func (m *JWTManager) ValidateToken(tokenString string) (*Claims, error) {
	// Check for empty token string
	if strings.TrimSpace(tokenString) == "" {
		return nil, models.ErrTokenMissing
	}

	// Parse and validate the token with options for clock skew tolerance
	token, err := jwt.ParseWithClaims(
		tokenString,
		&Claims{},
		func(token *jwt.Token) (interface{}, error) {
			// Validate signing method
			if _, ok := token.Method.(*jwt.SigningMethodHMAC); !ok {
				return nil, models.ErrTokenSignature
			}
			return m.secret, nil
		},
		jwt.WithLeeway(m.clockSkewLeeway),
		jwt.WithIssuer(m.issuer),
		jwt.WithIssuedAt(),
		jwt.WithExpirationRequired(),
	)

	if err != nil {
		return nil, m.mapJWTError(err)
	}

	// Extract and validate claims
	claims, ok := token.Claims.(*Claims)
	if !ok || !token.Valid {
		return nil, models.ErrTokenInvalid
	}

	// Additional explicit expiry validation (defense in depth)
	if claims.ExpiresAt == nil {
		return nil, models.ErrTokenInvalid
	}

	now := time.Now()
	expiryWithLeeway := claims.ExpiresAt.Time.Add(m.clockSkewLeeway)
	if now.After(expiryWithLeeway) {
		return nil, models.ErrTokenExpired
	}

	// Validate not-before with clock skew
	if claims.NotBefore != nil {
		nbfWithLeeway := claims.NotBefore.Time.Add(-m.clockSkewLeeway)
		if now.Before(nbfWithLeeway) {
			return nil, models.ErrTokenNotYetValid
		}
	}

	return claims, nil
}

// mapJWTError maps JWT library errors to domain-specific errors.
func (m *JWTManager) mapJWTError(err error) error {
	if err == nil {
		return nil
	}

	// Check for specific JWT errors using errors.Is
	switch {
	case errors.Is(err, jwt.ErrTokenExpired):
		return models.ErrTokenExpired
	case errors.Is(err, jwt.ErrTokenNotValidYet):
		return models.ErrTokenNotYetValid
	case errors.Is(err, jwt.ErrTokenMalformed):
		return models.ErrTokenMalformed
	case errors.Is(err, jwt.ErrTokenSignatureInvalid):
		return models.ErrTokenSignature
	case errors.Is(err, jwt.ErrTokenInvalidIssuer):
		return models.ErrTokenIssuer
	case errors.Is(err, jwt.ErrTokenInvalidClaims):
		return models.ErrTokenInvalid
	default:
		// Check if error message contains specific indicators
		errMsg := err.Error()
		if strings.Contains(errMsg, "expired") {
			return models.ErrTokenExpired
		}
		if strings.Contains(errMsg, "issuer") {
			return models.ErrTokenIssuer
		}
		if strings.Contains(errMsg, "signature") {
			return models.ErrTokenSignature
		}
		return models.ErrTokenInvalid
	}
}

// ValidateAccessToken validates an access token.
func (m *JWTManager) ValidateAccessToken(tokenString string) (*Claims, error) {
	claims, err := m.ValidateToken(tokenString)
	if err != nil {
		return nil, err
	}

	if claims.TokenType != TokenTypeAccess {
		return nil, models.ErrTokenTypeMismatch
	}

	return claims, nil
}

// ValidateRefreshToken validates a refresh token.
func (m *JWTManager) ValidateRefreshToken(tokenString string) (*Claims, error) {
	claims, err := m.ValidateToken(tokenString)
	if err != nil {
		return nil, err
	}

	if claims.TokenType != TokenTypeRefresh {
		return nil, models.ErrTokenTypeMismatch
	}

	return claims, nil
}

// GetAccessTokenTTL returns the access token TTL in seconds.
func (m *JWTManager) GetAccessTokenTTL() int64 {
	return int64(m.accessTokenTTL.Seconds())
}

// GetClockSkewLeeway returns the configured clock skew leeway.
func (m *JWTManager) GetClockSkewLeeway() time.Duration {
	return m.clockSkewLeeway
}

// HashPassword hashes a password using bcrypt.
func HashPassword(password string) (string, error) {
	bytes, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return "", err
	}
	return string(bytes), nil
}

// CheckPassword compares a password with a hash.
func CheckPassword(password, hash string) bool {
	err := bcrypt.CompareHashAndPassword([]byte(hash), []byte(password))
	return err == nil
}

// Context key types for storing auth info in context.
type contextKey string

const (
	userContextKey   contextKey = "user"
	claimsContextKey contextKey = "claims"
)

// ContextWithUser stores user in context.
func ContextWithUser(ctx context.Context, user *models.User) context.Context {
	return context.WithValue(ctx, userContextKey, user)
}

// UserFromContext retrieves user from context.
func UserFromContext(ctx context.Context) (*models.User, bool) {
	user, ok := ctx.Value(userContextKey).(*models.User)
	return user, ok
}

// ContextWithClaims stores claims in context.
func ContextWithClaims(ctx context.Context, claims *Claims) context.Context {
	return context.WithValue(ctx, claimsContextKey, claims)
}

// ClaimsFromContext retrieves claims from context.
func ClaimsFromContext(ctx context.Context) (*Claims, bool) {
	claims, ok := ctx.Value(claimsContextKey).(*Claims)
	return claims, ok
}
