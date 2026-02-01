package models

import "errors"

// User-related errors.
var (
	ErrEmailRequired        = errors.New("email is required")
	ErrPasswordRequired     = errors.New("password is required")
	ErrPasswordTooShort     = errors.New("password must be at least 8 characters")
	ErrNameRequired         = errors.New("name is required")
	ErrUserNotFound         = errors.New("user not found")
	ErrUserExists           = errors.New("user with this email already exists")
	ErrInvalidCredentials   = errors.New("invalid email or password")
	ErrUserInactive         = errors.New("user account is inactive")
	ErrOrganizationRequired = errors.New("organization name or invite token is required")
)

// Cluster-related errors.
var (
	ErrClusterNameRequired     = errors.New("cluster name is required")
	ErrClusterNameInvalid      = errors.New("cluster name must be between 3 and 63 characters")
	ErrClusterTierRequired     = errors.New("cluster tier is required")
	ErrClusterProviderRequired = errors.New("cloud provider is required")
	ErrClusterRegionRequired   = errors.New("region is required")
	ErrClusterNotFound         = errors.New("cluster not found")
	ErrClusterAlreadyExists    = errors.New("cluster with this name already exists")
	ErrInvalidNodeCount        = errors.New("node count must be at least 1")
	ErrNodeCountTooHigh        = errors.New("node count cannot exceed 100")
	ErrClusterNotRunning       = errors.New("cluster is not in running state")
	ErrClusterOperationPending = errors.New("another operation is pending on this cluster")
)

// Scale-to-zero errors.
var (
	ErrClusterAlreadySuspended = errors.New("cluster is already suspended")
	ErrClusterNotSuspended     = errors.New("cluster is not suspended")
	ErrClusterSuspending       = errors.New("cluster is currently suspending")
	ErrClusterWaking           = errors.New("cluster is currently waking")
	ErrWakeTimeout             = errors.New("cluster wake timeout exceeded")
	ErrDrainTimeout            = errors.New("connection drain timeout exceeded")
	ErrMaxQueuedConnections    = errors.New("maximum queued connections reached during wake")
	ErrScaleToZeroDisabled     = errors.New("scale-to-zero is not enabled for this cluster")
)

// Tiering configuration errors.
var (
	ErrTieringHotDurationRequired  = errors.New("hot tier duration is required when tiering is enabled")
	ErrTieringWarmDurationRequired = errors.New("warm tier duration is required when tiering is enabled")
	ErrTieringColdDurationRequired = errors.New("cold tier duration is required when tiering is enabled")
	ErrTieringInvalidCompression   = errors.New("compression type must be 'lz4' or 'zstd'")
	ErrS3ConfigRequired            = errors.New("S3 configuration is required when tiering is enabled")
	ErrS3EndpointRequired          = errors.New("S3 endpoint is required")
	ErrS3BucketRequired            = errors.New("S3 bucket is required")
	ErrS3RegionRequired            = errors.New("S3 region is required")
	ErrS3AccessKeyRequired         = errors.New("S3 access key is required")
	ErrS3SecretKeyRequired         = errors.New("S3 secret key is required")
	ErrInvalidShardCount           = errors.New("shard count must be non-negative")
	ErrShardCountTooHigh           = errors.New("shard count cannot exceed 1024")
)

// Authentication errors.
var (
	ErrTokenInvalid      = errors.New("invalid or expired token")
	ErrTokenMissing      = errors.New("authorization token is required")
	ErrTokenExpired      = errors.New("token has expired")
	ErrTokenNotYetValid  = errors.New("token is not yet valid")
	ErrTokenMalformed    = errors.New("token is malformed")
	ErrTokenSignature    = errors.New("token signature is invalid")
	ErrTokenIssuer       = errors.New("token issuer is invalid")
	ErrTokenTypeMismatch = errors.New("token type does not match expected type")
	ErrJWTSecretTooShort = errors.New("JWT secret must be at least 32 bytes")
	ErrJWTSecretEmpty    = errors.New("JWT secret cannot be empty")
	ErrUnauthorized      = errors.New("unauthorized access")
	ErrForbidden         = errors.New("access forbidden")
)

// Organization-related errors.
var (
	ErrOrgNameRequired            = errors.New("organization name is required")
	ErrOrgNameInvalid             = errors.New("organization name must be between 2 and 64 characters")
	ErrOrgNotFound                = errors.New("organization not found")
	ErrOrgAlreadyExists           = errors.New("organization with this slug already exists")
	ErrNotOrgMember               = errors.New("user is not a member of this organization")
	ErrInsufficientOrgPermissions = errors.New("insufficient organization permissions")
)

// Invite-related errors.
var (
	ErrInviteNotFound    = errors.New("invitation not found")
	ErrInviteExpired     = errors.New("invitation has expired")
	ErrInviteAlreadyUsed = errors.New("invitation has already been used")
	ErrAlreadyMember     = errors.New("user is already a member of this organization")
	ErrInvalidRole       = errors.New("invalid organization role")
)

// Data browser errors.
var (
	ErrTableNotFound    = errors.New("table not found")
	ErrQueryNotAllowed  = errors.New("query not allowed")
	ErrConnectionFailed = errors.New("failed to connect to cluster database")
)

// APIError represents a structured API error response.
type APIError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Details any    `json:"details,omitempty"`
}

// Error implements the error interface.
func (e *APIError) Error() string {
	return e.Message
}

// NewAPIError creates a new API error.
func NewAPIError(code, message string) *APIError {
	return &APIError{
		Code:    code,
		Message: message,
	}
}

// WithDetails adds details to the API error.
func (e *APIError) WithDetails(details any) *APIError {
	e.Details = details
	return e
}

// Common API error codes.
const (
	ErrCodeBadRequest         = "BAD_REQUEST"
	ErrCodeUnauthorized       = "UNAUTHORIZED"
	ErrCodeForbidden          = "FORBIDDEN"
	ErrCodeNotFound           = "NOT_FOUND"
	ErrCodeConflict           = "CONFLICT"
	ErrCodeValidation         = "VALIDATION_ERROR"
	ErrCodeInternal           = "INTERNAL_ERROR"
	ErrCodeServiceUnavailable = "SERVICE_UNAVAILABLE"
)
