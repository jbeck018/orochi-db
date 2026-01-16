// Package auth provides authentication utilities for the control plane.
package auth

import (
	"context"
	"errors"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
	"golang.org/x/crypto/bcrypt"

	"github.com/orochi-db/orochi-cloud/control-plane/internal/models"
	"github.com/orochi-db/orochi-cloud/control-plane/pkg/config"
)

// TokenType represents the type of JWT token.
type TokenType string

const (
	TokenTypeAccess  TokenType = "access"
	TokenTypeRefresh TokenType = "refresh"
)

// Claims represents the JWT claims for user tokens.
type Claims struct {
	jwt.RegisteredClaims
	UserID    uuid.UUID        `json:"user_id"`
	Email     string           `json:"email"`
	Role      models.UserRole  `json:"role"`
	TokenType TokenType        `json:"token_type"`
}

// JWTManager handles JWT token operations.
type JWTManager struct {
	secret          []byte
	issuer          string
	accessTokenTTL  time.Duration
	refreshTokenTTL time.Duration
}

// NewJWTManager creates a new JWT manager.
func NewJWTManager(cfg *config.JWTConfig) *JWTManager {
	return &JWTManager{
		secret:          []byte(cfg.Secret),
		issuer:          cfg.Issuer,
		accessTokenTTL:  cfg.AccessTokenTTL,
		refreshTokenTTL: cfg.RefreshTokenTTL,
	}
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
func (m *JWTManager) ValidateToken(tokenString string) (*Claims, error) {
	token, err := jwt.ParseWithClaims(tokenString, &Claims{}, func(token *jwt.Token) (interface{}, error) {
		if _, ok := token.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, errors.New("unexpected signing method")
		}
		return m.secret, nil
	})

	if err != nil {
		return nil, models.ErrTokenInvalid
	}

	claims, ok := token.Claims.(*Claims)
	if !ok || !token.Valid {
		return nil, models.ErrTokenInvalid
	}

	return claims, nil
}

// ValidateAccessToken validates an access token.
func (m *JWTManager) ValidateAccessToken(tokenString string) (*Claims, error) {
	claims, err := m.ValidateToken(tokenString)
	if err != nil {
		return nil, err
	}

	if claims.TokenType != TokenTypeAccess {
		return nil, models.ErrTokenInvalid
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
		return nil, models.ErrTokenInvalid
	}

	return claims, nil
}

// GetAccessTokenTTL returns the access token TTL in seconds.
func (m *JWTManager) GetAccessTokenTTL() int64 {
	return int64(m.accessTokenTTL.Seconds())
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
