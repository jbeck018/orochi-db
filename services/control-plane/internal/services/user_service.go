// Package services provides business logic for the control plane.
package services

import (
	"context"
	"errors"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// UserService handles user-related business logic.
type UserService struct {
	db         *db.DB
	jwtManager *auth.JWTManager
	logger     *slog.Logger
}

// NewUserService creates a new user service.
func NewUserService(db *db.DB, jwtManager *auth.JWTManager, logger *slog.Logger) *UserService {
	return &UserService{
		db:         db,
		jwtManager: jwtManager,
		logger:     logger.With("service", "user"),
	}
}

// Register creates a new user account.
// Uses INSERT ... ON CONFLICT to atomically handle duplicate email registration.
func (s *UserService) Register(ctx context.Context, req *models.UserCreateRequest) (*models.User, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	// Hash password
	passwordHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.logger.Error("failed to hash password", "error", err)
		return nil, errors.New("failed to create user")
	}

	user := &models.User{
		ID:           uuid.New(),
		Email:        req.Email,
		PasswordHash: passwordHash,
		Name:         req.Name,
		Role:         models.UserRoleMember,
		Active:       true,
		CreatedAt:    time.Now(),
		UpdatedAt:    time.Now(),
	}

	// Use INSERT ... ON CONFLICT to atomically check for duplicate emails.
	// This prevents race conditions where two concurrent registrations with
	// the same email could both pass a separate existence check.
	query := `
		INSERT INTO users (id, email, password_hash, name, role, active, created_at, updated_at)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
		ON CONFLICT (email) DO NOTHING
		RETURNING id
	`

	var insertedID uuid.UUID
	err = s.db.Pool.QueryRow(ctx, query,
		user.ID, user.Email, user.PasswordHash, user.Name,
		user.Role, user.Active, user.CreatedAt, user.UpdatedAt,
	).Scan(&insertedID)

	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			// ON CONFLICT triggered - email already exists
			return nil, models.ErrUserExists
		}
		s.logger.Error("failed to create user", "error", err)
		return nil, errors.New("failed to create user")
	}

	s.logger.Info("user registered", "user_id", user.ID, "email", user.Email)
	return user, nil
}

// Login authenticates a user and returns tokens.
func (s *UserService) Login(ctx context.Context, req *models.UserLoginRequest) (*models.UserLoginResponse, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	user, err := s.GetByEmail(ctx, req.Email)
	if err != nil {
		if errors.Is(err, models.ErrUserNotFound) {
			return nil, models.ErrInvalidCredentials
		}
		return nil, err
	}

	if !user.Active {
		return nil, models.ErrUserInactive
	}

	if !auth.CheckPassword(req.Password, user.PasswordHash) {
		return nil, models.ErrInvalidCredentials
	}

	// Generate tokens
	accessToken, err := s.jwtManager.GenerateAccessToken(user)
	if err != nil {
		s.logger.Error("failed to generate access token", "error", err)
		return nil, errors.New("failed to generate token")
	}

	refreshToken, err := s.jwtManager.GenerateRefreshToken(user)
	if err != nil {
		s.logger.Error("failed to generate refresh token", "error", err)
		return nil, errors.New("failed to generate token")
	}

	// Update last login
	if err := s.updateLastLogin(ctx, user.ID); err != nil {
		s.logger.Warn("failed to update last login", "error", err)
	}

	s.logger.Info("user logged in", "user_id", user.ID, "email", user.Email)

	return &models.UserLoginResponse{
		AccessToken:  accessToken,
		RefreshToken: refreshToken,
		TokenType:    "Bearer",
		ExpiresIn:    s.jwtManager.GetAccessTokenTTL(),
		User:         user,
	}, nil
}

// GetByID retrieves a user by ID.
func (s *UserService) GetByID(ctx context.Context, id uuid.UUID) (*models.User, error) {
	query := `
		SELECT id, email, password_hash, name, role, active, created_at, updated_at, last_login_at
		FROM users
		WHERE id = $1
	`

	user := &models.User{}
	err := s.db.Pool.QueryRow(ctx, query, id).Scan(
		&user.ID, &user.Email, &user.PasswordHash, &user.Name,
		&user.Role, &user.Active, &user.CreatedAt, &user.UpdatedAt, &user.LastLoginAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrUserNotFound
		}
		s.logger.Error("failed to get user by id", "error", err, "user_id", id)
		return nil, errors.New("failed to retrieve user")
	}

	return user, nil
}

// GetByEmail retrieves a user by email.
func (s *UserService) GetByEmail(ctx context.Context, email string) (*models.User, error) {
	query := `
		SELECT id, email, password_hash, name, role, active, created_at, updated_at, last_login_at
		FROM users
		WHERE email = $1
	`

	user := &models.User{}
	err := s.db.Pool.QueryRow(ctx, query, email).Scan(
		&user.ID, &user.Email, &user.PasswordHash, &user.Name,
		&user.Role, &user.Active, &user.CreatedAt, &user.UpdatedAt, &user.LastLoginAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrUserNotFound
		}
		s.logger.Error("failed to get user by email", "error", err, "email", email)
		return nil, errors.New("failed to retrieve user")
	}

	return user, nil
}

// RefreshTokens generates new tokens from a valid refresh token.
func (s *UserService) RefreshTokens(ctx context.Context, refreshToken string) (*models.UserLoginResponse, error) {
	claims, err := s.jwtManager.ValidateRefreshToken(refreshToken)
	if err != nil {
		return nil, err
	}

	user, err := s.GetByID(ctx, claims.UserID)
	if err != nil {
		return nil, err
	}

	if !user.Active {
		return nil, models.ErrUserInactive
	}

	accessToken, err := s.jwtManager.GenerateAccessToken(user)
	if err != nil {
		s.logger.Error("failed to generate access token", "error", err)
		return nil, errors.New("failed to generate token")
	}

	newRefreshToken, err := s.jwtManager.GenerateRefreshToken(user)
	if err != nil {
		s.logger.Error("failed to generate refresh token", "error", err)
		return nil, errors.New("failed to generate token")
	}

	return &models.UserLoginResponse{
		AccessToken:  accessToken,
		RefreshToken: newRefreshToken,
		TokenType:    "Bearer",
		ExpiresIn:    s.jwtManager.GetAccessTokenTTL(),
		User:         user,
	}, nil
}

// updateLastLogin updates the user's last login timestamp.
func (s *UserService) updateLastLogin(ctx context.Context, userID uuid.UUID) error {
	query := `UPDATE users SET last_login_at = $1 WHERE id = $2`
	_, err := s.db.Pool.Exec(ctx, query, time.Now(), userID)
	return err
}
