// Package types provides shared types for Orochi Cloud services
package types

import "time"

// APIResponse is the standard response wrapper
type APIResponse[T any] struct {
	Success   bool      `json:"success"`
	Data      T         `json:"data,omitempty"`
	Error     *APIError `json:"error,omitempty"`
	Timestamp time.Time `json:"timestamp"`
}

// APIError represents an API error
type APIError struct {
	Code    string            `json:"code"`
	Message string            `json:"message"`
	Details map[string]string `json:"details,omitempty"`
}

// Common error codes
const (
	ErrCodeBadRequest          = "BAD_REQUEST"
	ErrCodeUnauthorized        = "UNAUTHORIZED"
	ErrCodeForbidden           = "FORBIDDEN"
	ErrCodeNotFound            = "NOT_FOUND"
	ErrCodeConflict            = "CONFLICT"
	ErrCodeValidation          = "VALIDATION_ERROR"
	ErrCodeInternal            = "INTERNAL_ERROR"
	ErrCodeServiceUnavailable  = "SERVICE_UNAVAILABLE"
	ErrCodeRateLimited         = "RATE_LIMITED"
	ErrCodeQuotaExceeded       = "QUOTA_EXCEEDED"
)

// PaginatedResponse wraps paginated results
type PaginatedResponse[T any] struct {
	Items      []T    `json:"items"`
	Total      int64  `json:"total"`
	Page       int    `json:"page"`
	PageSize   int    `json:"page_size"`
	TotalPages int    `json:"total_pages"`
	HasMore    bool   `json:"has_more"`
}

// PaginationParams holds pagination parameters
type PaginationParams struct {
	Page     int    `json:"page" form:"page"`
	PageSize int    `json:"page_size" form:"page_size"`
	SortBy   string `json:"sort_by" form:"sort_by"`
	SortDir  string `json:"sort_dir" form:"sort_dir"` // "asc" or "desc"
}

// DefaultPagination returns default pagination params
func DefaultPagination() PaginationParams {
	return PaginationParams{
		Page:     1,
		PageSize: 20,
		SortBy:   "created_at",
		SortDir:  "desc",
	}
}

// FilterParams holds common filter parameters
type FilterParams struct {
	Search    string   `json:"search" form:"search"`
	Status    []string `json:"status" form:"status"`
	Region    []string `json:"region" form:"region"`
	StartDate *time.Time `json:"start_date" form:"start_date"`
	EndDate   *time.Time `json:"end_date" form:"end_date"`
}

// HealthResponse represents a health check response
type HealthResponse struct {
	Status    string            `json:"status"` // "healthy", "degraded", "unhealthy"
	Version   string            `json:"version"`
	Uptime    string            `json:"uptime"`
	Checks    map[string]bool   `json:"checks"`
	Timestamp time.Time         `json:"timestamp"`
}

// MetricsQuery represents a metrics query request
type MetricsQuery struct {
	ClusterID  string    `json:"cluster_id"`
	MetricName string    `json:"metric_name"`
	StartTime  time.Time `json:"start_time"`
	EndTime    time.Time `json:"end_time"`
	Step       string    `json:"step"` // e.g., "1m", "5m", "1h"
}

// MetricDataPoint represents a single metric data point
type MetricDataPoint struct {
	Timestamp time.Time `json:"timestamp"`
	Value     float64   `json:"value"`
}

// MetricsResponse contains metric query results
type MetricsResponse struct {
	MetricName string            `json:"metric_name"`
	ClusterID  string            `json:"cluster_id"`
	DataPoints []MetricDataPoint `json:"data_points"`
}

// Event represents a system event
type Event struct {
	ID          string                 `json:"id"`
	Type        string                 `json:"type"`
	Source      string                 `json:"source"`
	ClusterID   string                 `json:"cluster_id,omitempty"`
	UserID      string                 `json:"user_id,omitempty"`
	Severity    string                 `json:"severity"` // "info", "warning", "error", "critical"
	Message     string                 `json:"message"`
	Details     map[string]interface{} `json:"details,omitempty"`
	Timestamp   time.Time              `json:"timestamp"`
}
