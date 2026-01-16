package models

import (
	"time"

	"github.com/google/uuid"
)

// ClusterStatus represents the current status of a cluster.
type ClusterStatus string

const (
	ClusterStatusPending      ClusterStatus = "pending"
	ClusterStatusProvisioning ClusterStatus = "provisioning"
	ClusterStatusRunning      ClusterStatus = "running"
	ClusterStatusScaling      ClusterStatus = "scaling"
	ClusterStatusUpdating     ClusterStatus = "updating"
	ClusterStatusStopped      ClusterStatus = "stopped"
	ClusterStatusDeleting     ClusterStatus = "deleting"
	ClusterStatusFailed       ClusterStatus = "failed"
)

// ClusterTier represents the pricing tier of a cluster.
type ClusterTier string

const (
	ClusterTierDev        ClusterTier = "dev"
	ClusterTierStarter    ClusterTier = "starter"
	ClusterTierProduction ClusterTier = "production"
	ClusterTierEnterprise ClusterTier = "enterprise"
)

// CloudProvider represents the cloud provider where the cluster runs.
type CloudProvider string

const (
	CloudProviderAWS   CloudProvider = "aws"
	CloudProviderGCP   CloudProvider = "gcp"
	CloudProviderAzure CloudProvider = "azure"
)

// Cluster represents an Orochi DB cluster.
type Cluster struct {
	ID             uuid.UUID     `json:"id"`
	Name           string        `json:"name"`
	OwnerID        uuid.UUID     `json:"owner_id"`
	Status         ClusterStatus `json:"status"`
	Tier           ClusterTier   `json:"tier"`
	Provider       CloudProvider `json:"provider"`
	Region         string        `json:"region"`
	Version        string        `json:"version"`
	NodeCount      int           `json:"node_count"`
	NodeSize       string        `json:"node_size"`
	StorageGB      int           `json:"storage_gb"`
	ConnectionURL  string        `json:"connection_url,omitempty"`
	MaintenanceDay string        `json:"maintenance_day"`
	MaintenanceHour int          `json:"maintenance_hour"`
	BackupEnabled  bool          `json:"backup_enabled"`
	BackupRetention int          `json:"backup_retention_days"`
	CreatedAt      time.Time     `json:"created_at"`
	UpdatedAt      time.Time     `json:"updated_at"`
	DeletedAt      *time.Time    `json:"deleted_at,omitempty"`
}

// ClusterCreateRequest represents a request to create a new cluster.
type ClusterCreateRequest struct {
	Name            string        `json:"name"`
	Tier            ClusterTier   `json:"tier"`
	Provider        CloudProvider `json:"provider"`
	Region          string        `json:"region"`
	Version         string        `json:"version,omitempty"`
	NodeCount       int           `json:"node_count,omitempty"`
	NodeSize        string        `json:"node_size,omitempty"`
	StorageGB       int           `json:"storage_gb,omitempty"`
	MaintenanceDay  string        `json:"maintenance_day,omitempty"`
	MaintenanceHour int           `json:"maintenance_hour,omitempty"`
	BackupEnabled   bool          `json:"backup_enabled,omitempty"`
	BackupRetention int           `json:"backup_retention_days,omitempty"`
}

// ClusterUpdateRequest represents a request to update a cluster.
type ClusterUpdateRequest struct {
	Name            *string `json:"name,omitempty"`
	NodeSize        *string `json:"node_size,omitempty"`
	StorageGB       *int    `json:"storage_gb,omitempty"`
	MaintenanceDay  *string `json:"maintenance_day,omitempty"`
	MaintenanceHour *int    `json:"maintenance_hour,omitempty"`
	BackupEnabled   *bool   `json:"backup_enabled,omitempty"`
	BackupRetention *int    `json:"backup_retention_days,omitempty"`
}

// ClusterScaleRequest represents a request to scale a cluster.
type ClusterScaleRequest struct {
	NodeCount int    `json:"node_count"`
	NodeSize  string `json:"node_size,omitempty"`
}

// ClusterMetrics represents metrics for a cluster.
type ClusterMetrics struct {
	ClusterID       uuid.UUID `json:"cluster_id"`
	CPUUsage        float64   `json:"cpu_usage_percent"`
	MemoryUsage     float64   `json:"memory_usage_percent"`
	StorageUsage    float64   `json:"storage_usage_percent"`
	ConnectionCount int       `json:"connection_count"`
	QueriesPerSec   float64   `json:"queries_per_second"`
	ReadsPerSec     float64   `json:"reads_per_second"`
	WritesPerSec    float64   `json:"writes_per_second"`
	ReplicationLag  int64     `json:"replication_lag_ms"`
	Timestamp       time.Time `json:"timestamp"`
}

// ClusterListResponse represents a paginated list of clusters.
type ClusterListResponse struct {
	Clusters   []*Cluster `json:"clusters"`
	TotalCount int        `json:"total_count"`
	Page       int        `json:"page"`
	PageSize   int        `json:"page_size"`
}

// Validate validates the ClusterCreateRequest.
func (r *ClusterCreateRequest) Validate() error {
	if r.Name == "" {
		return ErrClusterNameRequired
	}
	if len(r.Name) < 3 || len(r.Name) > 63 {
		return ErrClusterNameInvalid
	}
	if r.Tier == "" {
		return ErrClusterTierRequired
	}
	if r.Provider == "" {
		return ErrClusterProviderRequired
	}
	if r.Region == "" {
		return ErrClusterRegionRequired
	}
	return nil
}

// Validate validates the ClusterScaleRequest.
func (r *ClusterScaleRequest) Validate() error {
	if r.NodeCount < 1 {
		return ErrInvalidNodeCount
	}
	if r.NodeCount > 100 {
		return ErrNodeCountTooHigh
	}
	return nil
}

// ApplyDefaults applies default values to the create request.
func (r *ClusterCreateRequest) ApplyDefaults() {
	if r.Version == "" {
		r.Version = "1.0.0"
	}
	if r.NodeCount == 0 {
		switch r.Tier {
		case ClusterTierDev:
			r.NodeCount = 1
		case ClusterTierStarter:
			r.NodeCount = 1
		case ClusterTierProduction:
			r.NodeCount = 3
		case ClusterTierEnterprise:
			r.NodeCount = 5
		}
	}
	if r.NodeSize == "" {
		switch r.Tier {
		case ClusterTierDev:
			r.NodeSize = "small"
		case ClusterTierStarter:
			r.NodeSize = "medium"
		case ClusterTierProduction:
			r.NodeSize = "large"
		case ClusterTierEnterprise:
			r.NodeSize = "xlarge"
		}
	}
	if r.StorageGB == 0 {
		switch r.Tier {
		case ClusterTierDev:
			r.StorageGB = 10
		case ClusterTierStarter:
			r.StorageGB = 50
		case ClusterTierProduction:
			r.StorageGB = 200
		case ClusterTierEnterprise:
			r.StorageGB = 500
		}
	}
	if r.MaintenanceDay == "" {
		r.MaintenanceDay = "sunday"
	}
	if r.BackupRetention == 0 {
		r.BackupRetention = 7
	}
}
