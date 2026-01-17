package models

import "errors"

// User-related errors.
var (
	ErrEmailRequired    = errors.New("email is required")
	ErrPasswordRequired = errors.New("password is required")
	ErrPasswordTooShort = errors.New("password must be at least 8 characters")
	ErrNameRequired     = errors.New("name is required")
	ErrUserNotFound     = errors.New("user not found")
	ErrUserExists       = errors.New("user with this email already exists")
	ErrInvalidCredentials = errors.New("invalid email or password")
	ErrUserInactive     = errors.New("user account is inactive")
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

// Authentication errors.
var (
	ErrTokenInvalid   = errors.New("invalid or expired token")
	ErrTokenMissing   = errors.New("authorization token is required")
	ErrUnauthorized   = errors.New("unauthorized access")
	ErrForbidden      = errors.New("access forbidden")
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
	ErrCodeBadRequest          = "BAD_REQUEST"
	ErrCodeUnauthorized        = "UNAUTHORIZED"
	ErrCodeForbidden           = "FORBIDDEN"
	ErrCodeNotFound            = "NOT_FOUND"
	ErrCodeConflict            = "CONFLICT"
	ErrCodeValidation          = "VALIDATION_ERROR"
	ErrCodeInternal            = "INTERNAL_ERROR"
	ErrCodeServiceUnavailable  = "SERVICE_UNAVAILABLE"
)
