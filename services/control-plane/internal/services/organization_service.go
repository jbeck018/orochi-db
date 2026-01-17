package services

import (
	"context"
	"errors"
	"log/slog"
	"regexp"
	"strings"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
)

// OrganizationService handles organization-related business logic.
type OrganizationService struct {
	db     *db.DB
	logger *slog.Logger
}

// NewOrganizationService creates a new organization service.
func NewOrganizationService(db *db.DB, logger *slog.Logger) *OrganizationService {
	return &OrganizationService{
		db:     db,
		logger: logger.With("service", "organization"),
	}
}

// slugify converts a name to a URL-friendly slug.
func slugify(name string) string {
	// Convert to lowercase
	slug := strings.ToLower(name)
	// Replace spaces and special chars with hyphens
	reg := regexp.MustCompile(`[^a-z0-9]+`)
	slug = reg.ReplaceAllString(slug, "-")
	// Remove leading/trailing hyphens
	slug = strings.Trim(slug, "-")
	// Limit length to 63 chars
	if len(slug) > 63 {
		slug = slug[:63]
	}
	return slug
}

// Create creates a new organization.
func (s *OrganizationService) Create(ctx context.Context, ownerID uuid.UUID, req *models.OrganizationCreateRequest) (*models.Organization, error) {
	if err := req.Validate(); err != nil {
		return nil, err
	}

	// Generate slug if not provided
	slug := req.Slug
	if slug == "" {
		slug = slugify(req.Name)
	}

	org := &models.Organization{
		ID:        uuid.New(),
		Name:      req.Name,
		Slug:      slug,
		OwnerID:   ownerID,
		CreatedAt: time.Now(),
		UpdatedAt: time.Now(),
	}

	// Start a transaction to create org and add owner as member
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return nil, errors.New("failed to create organization")
	}
	defer tx.Rollback(ctx)

	// Insert organization
	query := `
		INSERT INTO organizations (id, name, slug, owner_id, created_at, updated_at)
		VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (slug) DO NOTHING
		RETURNING id
	`

	var insertedID uuid.UUID
	err = tx.QueryRow(ctx, query, org.ID, org.Name, org.Slug, org.OwnerID, org.CreatedAt, org.UpdatedAt).Scan(&insertedID)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrOrgAlreadyExists
		}
		s.logger.Error("failed to create organization", "error", err)
		return nil, errors.New("failed to create organization")
	}

	// Add owner as member with owner role
	memberQuery := `
		INSERT INTO organization_members (id, organization_id, user_id, role, joined_at)
		VALUES ($1, $2, $3, $4, $5)
	`
	_, err = tx.Exec(ctx, memberQuery, uuid.New(), org.ID, ownerID, models.OrgRoleOwner, org.CreatedAt)
	if err != nil {
		s.logger.Error("failed to add owner as member", "error", err)
		return nil, errors.New("failed to create organization")
	}

	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return nil, errors.New("failed to create organization")
	}

	s.logger.Info("organization created",
		"org_id", org.ID,
		"name", org.Name,
		"owner_id", ownerID,
	)

	return org, nil
}

// GetByID retrieves an organization by ID.
func (s *OrganizationService) GetByID(ctx context.Context, id uuid.UUID) (*models.Organization, error) {
	query := `
		SELECT id, name, slug, owner_id, created_at, updated_at
		FROM organizations
		WHERE id = $1
	`

	org := &models.Organization{}
	err := s.db.Pool.QueryRow(ctx, query, id).Scan(
		&org.ID, &org.Name, &org.Slug, &org.OwnerID, &org.CreatedAt, &org.UpdatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrOrgNotFound
		}
		s.logger.Error("failed to get organization", "error", err, "org_id", id)
		return nil, errors.New("failed to retrieve organization")
	}

	return org, nil
}

// GetBySlug retrieves an organization by slug.
func (s *OrganizationService) GetBySlug(ctx context.Context, slug string) (*models.Organization, error) {
	query := `
		SELECT id, name, slug, owner_id, created_at, updated_at
		FROM organizations
		WHERE slug = $1
	`

	org := &models.Organization{}
	err := s.db.Pool.QueryRow(ctx, query, slug).Scan(
		&org.ID, &org.Name, &org.Slug, &org.OwnerID, &org.CreatedAt, &org.UpdatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrOrgNotFound
		}
		s.logger.Error("failed to get organization by slug", "error", err, "slug", slug)
		return nil, errors.New("failed to retrieve organization")
	}

	return org, nil
}

// ListByUser retrieves all organizations for a user (as owner or member).
func (s *OrganizationService) ListByUser(ctx context.Context, userID uuid.UUID, page, pageSize int) (*models.OrganizationListResponse, error) {
	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}

	offset := (page - 1) * pageSize

	// Get total count
	countQuery := `
		SELECT COUNT(DISTINCT o.id)
		FROM organizations o
		LEFT JOIN organization_members om ON o.id = om.organization_id
		WHERE o.owner_id = $1 OR om.user_id = $1
	`
	var totalCount int
	if err := s.db.Pool.QueryRow(ctx, countQuery, userID).Scan(&totalCount); err != nil {
		s.logger.Error("failed to count organizations", "error", err)
		return nil, errors.New("failed to list organizations")
	}

	// Get organizations
	query := `
		SELECT DISTINCT o.id, o.name, o.slug, o.owner_id, o.created_at, o.updated_at
		FROM organizations o
		LEFT JOIN organization_members om ON o.id = om.organization_id
		WHERE o.owner_id = $1 OR om.user_id = $1
		ORDER BY o.created_at DESC
		LIMIT $2 OFFSET $3
	`

	rows, err := s.db.Pool.Query(ctx, query, userID, pageSize, offset)
	if err != nil {
		s.logger.Error("failed to list organizations", "error", err)
		return nil, errors.New("failed to list organizations")
	}
	defer rows.Close()

	orgs := make([]*models.Organization, 0)
	for rows.Next() {
		org := &models.Organization{}
		err := rows.Scan(
			&org.ID, &org.Name, &org.Slug, &org.OwnerID, &org.CreatedAt, &org.UpdatedAt,
		)
		if err != nil {
			s.logger.Error("failed to scan organization", "error", err)
			continue
		}
		orgs = append(orgs, org)
	}

	return &models.OrganizationListResponse{
		Organizations: orgs,
		TotalCount:    totalCount,
		Page:          page,
		PageSize:      pageSize,
	}, nil
}

// Update updates an organization.
func (s *OrganizationService) Update(ctx context.Context, id uuid.UUID, name string) (*models.Organization, error) {
	if name == "" {
		return nil, models.ErrOrgNameRequired
	}

	query := `
		UPDATE organizations
		SET name = $2, updated_at = NOW()
		WHERE id = $1
		RETURNING id, name, slug, owner_id, created_at, updated_at
	`

	org := &models.Organization{}
	err := s.db.Pool.QueryRow(ctx, query, id, name).Scan(
		&org.ID, &org.Name, &org.Slug, &org.OwnerID, &org.CreatedAt, &org.UpdatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrOrgNotFound
		}
		s.logger.Error("failed to update organization", "error", err)
		return nil, errors.New("failed to update organization")
	}

	s.logger.Info("organization updated", "org_id", id, "name", name)
	return org, nil
}

// Delete deletes an organization and all its memberships.
func (s *OrganizationService) Delete(ctx context.Context, id uuid.UUID) error {
	// Start a transaction
	tx, err := s.db.Pool.Begin(ctx)
	if err != nil {
		s.logger.Error("failed to begin transaction", "error", err)
		return errors.New("failed to delete organization")
	}
	defer tx.Rollback(ctx)

	// Check if org exists
	var orgID uuid.UUID
	err = tx.QueryRow(ctx, "SELECT id FROM organizations WHERE id = $1", id).Scan(&orgID)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return models.ErrOrgNotFound
		}
		s.logger.Error("failed to check organization", "error", err)
		return errors.New("failed to delete organization")
	}

	// Delete memberships (should cascade but being explicit)
	_, err = tx.Exec(ctx, "DELETE FROM organization_members WHERE organization_id = $1", id)
	if err != nil {
		s.logger.Error("failed to delete organization members", "error", err)
		return errors.New("failed to delete organization")
	}

	// Unassign clusters from this organization
	_, err = tx.Exec(ctx, "UPDATE clusters SET organization_id = NULL WHERE organization_id = $1", id)
	if err != nil {
		s.logger.Error("failed to unassign clusters", "error", err)
		return errors.New("failed to delete organization")
	}

	// Delete organization
	_, err = tx.Exec(ctx, "DELETE FROM organizations WHERE id = $1", id)
	if err != nil {
		s.logger.Error("failed to delete organization", "error", err)
		return errors.New("failed to delete organization")
	}

	if err = tx.Commit(ctx); err != nil {
		s.logger.Error("failed to commit transaction", "error", err)
		return errors.New("failed to delete organization")
	}

	s.logger.Info("organization deleted", "org_id", id)
	return nil
}

// CheckMembership checks if a user is a member of an organization.
func (s *OrganizationService) CheckMembership(ctx context.Context, orgID, userID uuid.UUID) (*models.OrganizationMember, error) {
	query := `
		SELECT id, organization_id, user_id, role, joined_at
		FROM organization_members
		WHERE organization_id = $1 AND user_id = $2
	`

	member := &models.OrganizationMember{}
	err := s.db.Pool.QueryRow(ctx, query, orgID, userID).Scan(
		&member.ID, &member.OrganizationID, &member.UserID, &member.Role,
		&member.JoinedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, models.ErrNotOrgMember
		}
		s.logger.Error("failed to check membership", "error", err)
		return nil, errors.New("failed to check membership")
	}

	return member, nil
}

// CheckOwnership verifies that a user owns an organization.
func (s *OrganizationService) CheckOwnership(ctx context.Context, orgID, userID uuid.UUID) error {
	org, err := s.GetByID(ctx, orgID)
	if err != nil {
		return err
	}

	if org.OwnerID != userID {
		return models.ErrForbidden
	}

	return nil
}

// GetMembers retrieves all members of an organization.
func (s *OrganizationService) GetMembers(ctx context.Context, orgID uuid.UUID) ([]*models.OrganizationMember, error) {
	query := `
		SELECT om.id, om.organization_id, om.user_id, om.role, om.joined_at,
			   u.id, u.email, u.name, u.role, u.created_at
		FROM organization_members om
		JOIN users u ON om.user_id = u.id
		WHERE om.organization_id = $1
		ORDER BY om.joined_at ASC
	`

	rows, err := s.db.Pool.Query(ctx, query, orgID)
	if err != nil {
		s.logger.Error("failed to get members", "error", err)
		return nil, errors.New("failed to retrieve members")
	}
	defer rows.Close()

	members := make([]*models.OrganizationMember, 0)
	for rows.Next() {
		member := &models.OrganizationMember{}
		user := &models.User{}
		err := rows.Scan(
			&member.ID, &member.OrganizationID, &member.UserID, &member.Role, &member.JoinedAt,
			&user.ID, &user.Email, &user.Name, &user.Role, &user.CreatedAt,
		)
		if err != nil {
			s.logger.Error("failed to scan member", "error", err)
			continue
		}
		member.User = user
		members = append(members, member)
	}

	return members, nil
}

// AddMember adds a user to an organization.
func (s *OrganizationService) AddMember(ctx context.Context, orgID, userID uuid.UUID, role models.OrganizationMemberRole) error {
	query := `
		INSERT INTO organization_members (id, organization_id, user_id, role, joined_at)
		VALUES ($1, $2, $3, $4, NOW())
		ON CONFLICT (organization_id, user_id) DO UPDATE SET role = $4
	`

	_, err := s.db.Pool.Exec(ctx, query, uuid.New(), orgID, userID, role)
	if err != nil {
		s.logger.Error("failed to add member", "error", err)
		return errors.New("failed to add member")
	}

	s.logger.Info("member added to organization", "org_id", orgID, "user_id", userID, "role", role)
	return nil
}

// RemoveMember removes a user from an organization.
func (s *OrganizationService) RemoveMember(ctx context.Context, orgID, userID uuid.UUID) error {
	result, err := s.db.Pool.Exec(ctx,
		"DELETE FROM organization_members WHERE organization_id = $1 AND user_id = $2",
		orgID, userID,
	)
	if err != nil {
		s.logger.Error("failed to remove member", "error", err)
		return errors.New("failed to remove member")
	}

	if result.RowsAffected() == 0 {
		return models.ErrNotOrgMember
	}

	s.logger.Info("member removed from organization", "org_id", orgID, "user_id", userID)
	return nil
}
