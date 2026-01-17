package services

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// InviteService handles organization invitation operations.
type InviteService struct {
	db     *db.DB
	logger *slog.Logger
}

// NewInviteService creates a new invite service.
func NewInviteService(db *db.DB, logger *slog.Logger) *InviteService {
	return &InviteService{
		db:     db,
		logger: logger.With("service", "invite"),
	}
}

// generateToken creates a secure random token for invitations.
func generateToken() (string, error) {
	bytes := make([]byte, 32)
	if _, err := rand.Read(bytes); err != nil {
		return "", err
	}
	return hex.EncodeToString(bytes), nil
}

// CreateInvite creates a new organization invitation.
func (s *InviteService) CreateInvite(ctx context.Context, orgID, inviterID uuid.UUID, email string, role models.OrganizationMemberRole) (*models.OrganizationInvite, error) {
	// Validate role
	if !role.IsValid() {
		return nil, models.ErrInvalidRole
	}

	// Check if user is already a member
	existingMember := `SELECT id FROM organization_members WHERE organization_id = $1 AND user_id = (SELECT id FROM users WHERE email = $2)`
	var memberID uuid.UUID
	err := s.db.Pool.QueryRow(ctx, existingMember, orgID, email).Scan(&memberID)
	if err == nil {
		return nil, models.ErrAlreadyMember
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		s.logger.Error("failed to check existing membership", "error", err)
		return nil, errors.New("failed to create invitation")
	}

	// Generate secure token
	token, err := generateToken()
	if err != nil {
		s.logger.Error("failed to generate token", "error", err)
		return nil, errors.New("failed to create invitation")
	}

	invite := &models.OrganizationInvite{
		ID:             uuid.New(),
		OrganizationID: orgID,
		Email:          email,
		Role:           role,
		Token:          token,
		InvitedBy:      inviterID,
		ExpiresAt:      time.Now().Add(24 * time.Hour),
		CreatedAt:      time.Now(),
	}

	query := `
		INSERT INTO organization_invites (id, organization_id, email, role, token, invited_by, expires_at, created_at)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
		ON CONFLICT (organization_id, email) DO UPDATE SET
			role = EXCLUDED.role,
			token = EXCLUDED.token,
			invited_by = EXCLUDED.invited_by,
			expires_at = EXCLUDED.expires_at,
			accepted_at = NULL
		RETURNING id
	`

	err = s.db.Pool.QueryRow(ctx, query,
		invite.ID, invite.OrganizationID, invite.Email, invite.Role,
		invite.Token, invite.InvitedBy, invite.ExpiresAt, invite.CreatedAt,
	).Scan(&invite.ID)

	if err != nil {
		s.logger.Error("failed to create invitation", "error", err)
		return nil, errors.New("failed to create invitation")
	}

	s.logger.Info("invitation created",
		"invite_id", invite.ID,
		"org_id", orgID,
		"email", email,
		"role", role,
	)

	return invite, nil
}

// GetInviteByToken retrieves an invitation by its token.
func (s *InviteService) GetInviteByToken(ctx context.Context, token string) (*models.OrganizationInvite, error) {
	query := `
		SELECT i.id, i.organization_id, i.email, i.role, i.token, i.invited_by, i.expires_at, i.accepted_at, i.created_at,
		       o.id, o.name, o.slug
		FROM organization_invites i
		JOIN organizations o ON i.organization_id = o.id
		WHERE i.token = $1
	`

	invite := &models.OrganizationInvite{}
	org := &models.Organization{}

	err := s.db.Pool.QueryRow(ctx, query, token).Scan(
		&invite.ID, &invite.OrganizationID, &invite.Email, &invite.Role,
		&invite.Token, &invite.InvitedBy, &invite.ExpiresAt, &invite.AcceptedAt, &invite.CreatedAt,
		&org.ID, &org.Name, &org.Slug,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrInviteNotFound
		}
		s.logger.Error("failed to get invitation", "error", err)
		return nil, errors.New("failed to retrieve invitation")
	}

	invite.Organization = org
	return invite, nil
}

// AcceptInvite accepts an invitation and adds the user to the organization.
func (s *InviteService) AcceptInvite(ctx context.Context, token string, userID uuid.UUID) (*models.OrganizationMember, error) {
	// Get the invitation
	invite, err := s.GetInviteByToken(ctx, token)
	if err != nil {
		return nil, err
	}

	// Check if already accepted
	if invite.AcceptedAt != nil {
		return nil, models.ErrInviteAlreadyUsed
	}

	// Check if expired
	if time.Now().After(invite.ExpiresAt) {
		return nil, models.ErrInviteExpired
	}

	// Start transaction
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to accept invitation")
	}
	defer tx.Rollback(ctx)

	// Add user to organization
	memberID := uuid.New()
	now := time.Now()
	memberQuery := `
		INSERT INTO organization_members (id, organization_id, user_id, role, joined_at)
		VALUES ($1, $2, $3, $4, $5)
		ON CONFLICT (organization_id, user_id) DO UPDATE SET role = EXCLUDED.role
		RETURNING id
	`
	err = tx.QueryRow(ctx, memberQuery, memberID, invite.OrganizationID, userID, invite.Role, now).Scan(&memberID)
	if err != nil {
		s.logger.Error("failed to add member", "error", err)
		return nil, errors.New("failed to accept invitation")
	}

	// Mark invitation as accepted
	acceptQuery := `UPDATE organization_invites SET accepted_at = $1 WHERE id = $2`
	_, err = tx.Exec(ctx, acceptQuery, now, invite.ID)
	if err != nil {
		s.logger.Error("failed to mark invitation accepted", "error", err)
		return nil, errors.New("failed to accept invitation")
	}

	// Update user's default organization if not set
	updateUserQuery := `UPDATE users SET default_organization_id = $1 WHERE id = $2 AND default_organization_id IS NULL`
	_, err = tx.Exec(ctx, updateUserQuery, invite.OrganizationID, userID)
	if err != nil {
		s.logger.Error("failed to update user default org", "error", err)
		// Non-fatal, continue
	}

	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to accept invitation")
	}

	member := &models.OrganizationMember{
		ID:             memberID,
		OrganizationID: invite.OrganizationID,
		UserID:         userID,
		Role:           invite.Role,
		JoinedAt:       now,
	}

	s.logger.Info("invitation accepted",
		"invite_id", invite.ID,
		"user_id", userID,
		"org_id", invite.OrganizationID,
	)

	return member, nil
}

// ListPendingInvites lists all pending invitations for an organization.
func (s *InviteService) ListPendingInvites(ctx context.Context, orgID uuid.UUID) ([]*models.OrganizationInvite, error) {
	query := `
		SELECT i.id, i.organization_id, i.email, i.role, i.token, i.invited_by, i.expires_at, i.created_at,
		       u.id, u.email, u.name
		FROM organization_invites i
		JOIN users u ON i.invited_by = u.id
		WHERE i.organization_id = $1 AND i.accepted_at IS NULL AND i.expires_at > NOW()
		ORDER BY i.created_at DESC
	`

	rows, err := s.db.Pool.Query(ctx, query, orgID)
	if err != nil {
		s.logger.Error("failed to list invitations", "error", err)
		return nil, errors.New("failed to list invitations")
	}
	defer rows.Close()

	invites := make([]*models.OrganizationInvite, 0)
	for rows.Next() {
		invite := &models.OrganizationInvite{}
		inviter := &models.User{}
		err := rows.Scan(
			&invite.ID, &invite.OrganizationID, &invite.Email, &invite.Role,
			&invite.Token, &invite.InvitedBy, &invite.ExpiresAt, &invite.CreatedAt,
			&inviter.ID, &inviter.Email, &inviter.Name,
		)
		if err != nil {
			s.logger.Error("failed to scan invitation", "error", err)
			continue
		}
		invite.InvitedByUser = inviter
		invites = append(invites, invite)
	}

	return invites, nil
}

// ListInvitesForEmail lists all pending invitations for an email address.
func (s *InviteService) ListInvitesForEmail(ctx context.Context, email string) ([]*models.OrganizationInvite, error) {
	query := `
		SELECT i.id, i.organization_id, i.email, i.role, i.token, i.invited_by, i.expires_at, i.created_at,
		       o.id, o.name, o.slug
		FROM organization_invites i
		JOIN organizations o ON i.organization_id = o.id
		WHERE i.email = $1 AND i.accepted_at IS NULL AND i.expires_at > NOW()
		ORDER BY i.created_at DESC
	`

	rows, err := s.db.Pool.Query(ctx, query, email)
	if err != nil {
		s.logger.Error("failed to list invitations for email", "error", err)
		return nil, errors.New("failed to list invitations")
	}
	defer rows.Close()

	invites := make([]*models.OrganizationInvite, 0)
	for rows.Next() {
		invite := &models.OrganizationInvite{}
		org := &models.Organization{}
		err := rows.Scan(
			&invite.ID, &invite.OrganizationID, &invite.Email, &invite.Role,
			&invite.Token, &invite.InvitedBy, &invite.ExpiresAt, &invite.CreatedAt,
			&org.ID, &org.Name, &org.Slug,
		)
		if err != nil {
			s.logger.Error("failed to scan invitation", "error", err)
			continue
		}
		invite.Organization = org
		invites = append(invites, invite)
	}

	return invites, nil
}

// RevokeInvite revokes an invitation.
func (s *InviteService) RevokeInvite(ctx context.Context, inviteID, orgID uuid.UUID) error {
	query := `DELETE FROM organization_invites WHERE id = $1 AND organization_id = $2 AND accepted_at IS NULL`

	result, err := s.db.Pool.Exec(ctx, query, inviteID, orgID)
	if err != nil {
		s.logger.Error("failed to revoke invitation", "error", err)
		return errors.New("failed to revoke invitation")
	}

	if result.RowsAffected() == 0 {
		return models.ErrInviteNotFound
	}

	s.logger.Info("invitation revoked", "invite_id", inviteID, "org_id", orgID)
	return nil
}

// ResendInvite regenerates the token and extends the expiration.
func (s *InviteService) ResendInvite(ctx context.Context, inviteID, orgID uuid.UUID) (*models.OrganizationInvite, error) {
	token, err := generateToken()
	if err != nil {
		s.logger.Error("failed to generate token", "error", err)
		return nil, errors.New("failed to resend invitation")
	}

	newExpiry := time.Now().Add(24 * time.Hour)

	query := `
		UPDATE organization_invites
		SET token = $1, expires_at = $2
		WHERE id = $3 AND organization_id = $4 AND accepted_at IS NULL
		RETURNING id, organization_id, email, role, token, invited_by, expires_at, created_at
	`

	invite := &models.OrganizationInvite{}
	err = s.db.Pool.QueryRow(ctx, query, token, newExpiry, inviteID, orgID).Scan(
		&invite.ID, &invite.OrganizationID, &invite.Email, &invite.Role,
		&invite.Token, &invite.InvitedBy, &invite.ExpiresAt, &invite.CreatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrInviteNotFound
		}
		s.logger.Error("failed to resend invitation", "error", err)
		return nil, errors.New("failed to resend invitation")
	}

	s.logger.Info("invitation resent", "invite_id", inviteID, "org_id", orgID)
	return invite, nil
}
