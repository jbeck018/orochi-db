// Package auth provides JWT authentication for the PgDog gateway.
package auth

import (
	"github.com/golang-jwt/jwt/v5"
)

// OrochiClaims represents the custom claims structure for Orochi DB.
// These claims are extracted from the JWT and injected as session variables.
type OrochiClaims struct {
	jwt.RegisteredClaims

	// UserID is the unique identifier for the authenticated user.
	UserID string `json:"user_id,omitempty"`

	// TenantID is the tenant/organization identifier for multi-tenant isolation.
	TenantID string `json:"tenant_id,omitempty"`

	// BranchID is the database branch identifier for branch-based access control.
	BranchID string `json:"branch_id,omitempty"`

	// Permissions is a list of permission strings granted to this token.
	Permissions []string `json:"permissions,omitempty"`

	// PermissionLevel is the numeric permission level (0=none, 1=read, 2=write, 3=admin).
	PermissionLevel int `json:"permission_level,omitempty"`

	// DatabaseName is the target database for this connection.
	DatabaseName string `json:"database,omitempty"`

	// PoolMode overrides the default pool mode (transaction, session, statement).
	PoolMode string `json:"pool_mode,omitempty"`

	// MaxConnections limits the number of connections for this user/tenant.
	MaxConnections int `json:"max_connections,omitempty"`

	// ReadOnly forces read-only mode when true.
	ReadOnly bool `json:"read_only,omitempty"`

	// Metadata contains additional key-value pairs for custom session variables.
	Metadata map[string]string `json:"metadata,omitempty"`
}

// HasPermission checks if the claims include a specific permission.
func (c *OrochiClaims) HasPermission(permission string) bool {
	for _, p := range c.Permissions {
		if p == permission || p == "*" {
			return true
		}
	}
	return false
}

// CanWrite returns true if the permission level allows write operations.
func (c *OrochiClaims) CanWrite() bool {
	return c.PermissionLevel >= 2 && !c.ReadOnly
}

// CanAdmin returns true if the permission level allows admin operations.
func (c *OrochiClaims) CanAdmin() bool {
	return c.PermissionLevel >= 3
}

// GetSessionVariables returns a map of session variable names to values.
// These will be set on the PostgreSQL connection after authentication.
func (c *OrochiClaims) GetSessionVariables() map[string]string {
	vars := make(map[string]string)

	if c.UserID != "" {
		vars["orochi.user_id"] = c.UserID
	}
	if c.TenantID != "" {
		vars["orochi.tenant_id"] = c.TenantID
	}
	if c.BranchID != "" {
		vars["orochi.branch_id"] = c.BranchID
	}
	if c.PermissionLevel > 0 {
		vars["orochi.permission_level"] = formatInt(c.PermissionLevel)
	}
	if c.ReadOnly {
		vars["orochi.read_only"] = "true"
	}

	// Add custom metadata as session variables
	for k, v := range c.Metadata {
		vars["orochi."+k] = v
	}

	return vars
}

// formatInt converts an integer to a string without importing strconv.
func formatInt(n int) string {
	if n == 0 {
		return "0"
	}
	negative := n < 0
	if negative {
		n = -n
	}
	var digits []byte
	for n > 0 {
		digits = append([]byte{byte('0' + n%10)}, digits...)
		n /= 10
	}
	if negative {
		digits = append([]byte{'-'}, digits...)
	}
	return string(digits)
}
