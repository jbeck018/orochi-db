// Package models defines the data models for the control plane.
package models

import (
	"time"

	"github.com/google/uuid"
)

// Organization represents a team or organization that owns clusters.
type Organization struct {
	ID        uuid.UUID  `json:"id"`
	Name      string     `json:"name"`
	Slug      string     `json:"slug"`
	OwnerID   uuid.UUID  `json:"owner_id"`
	CreatedAt time.Time  `json:"created_at"`
	UpdatedAt time.Time  `json:"updated_at"`
	DeletedAt *time.Time `json:"deleted_at,omitempty"`
}

// OrganizationMemberRole represents a member's role in an organization.
type OrganizationMemberRole string

const (
	OrgRoleOwner  OrganizationMemberRole = "owner"
	OrgRoleAdmin  OrganizationMemberRole = "admin"
	OrgRoleMember OrganizationMemberRole = "member"
	OrgRoleViewer OrganizationMemberRole = "viewer"
)

// OrganizationMember represents a user's membership in an organization.
type OrganizationMember struct {
	ID             uuid.UUID              `json:"id"`
	OrganizationID uuid.UUID              `json:"organization_id"`
	UserID         uuid.UUID              `json:"user_id"`
	Role           OrganizationMemberRole `json:"role"`
	InvitedBy      *uuid.UUID             `json:"invited_by,omitempty"`
	JoinedAt       time.Time              `json:"joined_at"`
	User           *User                  `json:"user,omitempty"`
}

// OrganizationCreateRequest represents a request to create an organization.
type OrganizationCreateRequest struct {
	Name string `json:"name"`
	Slug string `json:"slug,omitempty"`
}

// OrganizationInviteRequest represents a request to invite a user to an organization.
type OrganizationInviteRequest struct {
	Email string                 `json:"email"`
	Role  OrganizationMemberRole `json:"role"`
}

// OrganizationListResponse represents a paginated list of organizations.
type OrganizationListResponse struct {
	Organizations []*Organization `json:"organizations"`
	TotalCount    int             `json:"total_count"`
	Page          int             `json:"page"`
	PageSize      int             `json:"page_size"`
}

// Validate validates the OrganizationCreateRequest.
func (r *OrganizationCreateRequest) Validate() error {
	if r.Name == "" {
		return ErrOrgNameRequired
	}
	if len(r.Name) < 2 || len(r.Name) > 64 {
		return ErrOrgNameInvalid
	}
	return nil
}

// CanManageCluster checks if a member role can manage clusters.
func (role OrganizationMemberRole) CanManageCluster() bool {
	return role == OrgRoleOwner || role == OrgRoleAdmin || role == OrgRoleMember
}

// CanInviteMembers checks if a member role can invite new members.
func (role OrganizationMemberRole) CanInviteMembers() bool {
	return role == OrgRoleOwner || role == OrgRoleAdmin
}

// CanDeleteOrg checks if a member role can delete the organization.
func (role OrganizationMemberRole) CanDeleteOrg() bool {
	return role == OrgRoleOwner
}

// IsValid checks if the role is a valid organization member role.
func (role OrganizationMemberRole) IsValid() bool {
	switch role {
	case OrgRoleOwner, OrgRoleAdmin, OrgRoleMember, OrgRoleViewer:
		return true
	default:
		return false
	}
}

// OrganizationInvite represents an invitation to join an organization.
type OrganizationInvite struct {
	ID             uuid.UUID              `json:"id"`
	OrganizationID uuid.UUID              `json:"organization_id"`
	Email          string                 `json:"email"`
	Role           OrganizationMemberRole `json:"role"`
	Token          string                 `json:"token,omitempty"` // Only shown to inviter
	InvitedBy      uuid.UUID              `json:"invited_by"`
	ExpiresAt      time.Time              `json:"expires_at"`
	AcceptedAt     *time.Time             `json:"accepted_at,omitempty"`
	CreatedAt      time.Time              `json:"created_at"`
	Organization   *Organization          `json:"organization,omitempty"`
	InvitedByUser  *User                  `json:"invited_by_user,omitempty"`
}

// OrganizationInviteResponse represents an invite response without sensitive data.
type OrganizationInviteResponse struct {
	ID             uuid.UUID              `json:"id"`
	OrganizationID uuid.UUID              `json:"organization_id"`
	Email          string                 `json:"email"`
	Role           OrganizationMemberRole `json:"role"`
	ExpiresAt      time.Time              `json:"expires_at"`
	CreatedAt      time.Time              `json:"created_at"`
	Organization   *Organization          `json:"organization,omitempty"`
}

// ToResponse converts an invite to a response (hiding token).
func (i *OrganizationInvite) ToResponse() *OrganizationInviteResponse {
	return &OrganizationInviteResponse{
		ID:             i.ID,
		OrganizationID: i.OrganizationID,
		Email:          i.Email,
		Role:           i.Role,
		ExpiresAt:      i.ExpiresAt,
		CreatedAt:      i.CreatedAt,
		Organization:   i.Organization,
	}
}
