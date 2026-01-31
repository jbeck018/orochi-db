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
	ClusterStatusSuspended    ClusterStatus = "suspended"   // Scale-to-zero: cluster pods are scaled down
	ClusterStatusSuspending   ClusterStatus = "suspending"  // Scale-to-zero: transitioning to suspended
	ClusterStatusWaking       ClusterStatus = "waking"      // Scale-to-zero: transitioning from suspended to running
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
	CloudProviderAWS          CloudProvider = "aws"
	CloudProviderGCP          CloudProvider = "gcp"
	CloudProviderAzure        CloudProvider = "azure"
	CloudProviderDigitalOcean CloudProvider = "digitalocean"
)

// Cluster represents an Orochi DB cluster.
type Cluster struct {
	ID                  uuid.UUID      `json:"id"`
	Name                string         `json:"name"`
	OwnerID             uuid.UUID      `json:"owner_id"`
	OrganizationID      *uuid.UUID     `json:"organization_id,omitempty"`    // Optional team/organization for multi-tenant isolation
	Status              ClusterStatus  `json:"status"`
	Tier                ClusterTier    `json:"tier"`
	Provider            CloudProvider  `json:"provider"`
	Region              string         `json:"region"`
	Version             string         `json:"version"`
	NodeCount           int            `json:"node_count"`
	NodeSize            string         `json:"node_size"`
	StorageGB           int            `json:"storage_gb"`
	ConnectionURL       *string        `json:"connection_url,omitempty"`     // Direct PostgreSQL connection
	PoolerURL           *string        `json:"pooler_url,omitempty"`         // PgBouncer pooler connection
	MaintenanceDay      string         `json:"maintenance_day"`
	MaintenanceHour     int            `json:"maintenance_hour"`
	BackupEnabled       bool           `json:"backup_enabled"`
	BackupRetention     int            `json:"backup_retention_days"`
	PoolerEnabled       bool           `json:"pooler_enabled"`               // Whether connection pooling is enabled
	TieringEnabled      bool           `json:"tiering_enabled"`              // Whether tiered storage is enabled
	TieringHotDuration  *string        `json:"tiering_hot_duration,omitempty"`   // Hot tier duration
	TieringWarmDuration *string        `json:"tiering_warm_duration,omitempty"`  // Warm tier duration
	TieringColdDuration *string        `json:"tiering_cold_duration,omitempty"`  // Cold tier duration
	TieringCompression  *string        `json:"tiering_compression,omitempty"`    // Compression type (lz4, zstd)
	S3Endpoint          *string        `json:"s3_endpoint,omitempty"`        // S3 endpoint for cold/frozen tiers
	S3Bucket            *string        `json:"s3_bucket,omitempty"`          // S3 bucket name
	S3Region            *string        `json:"s3_region,omitempty"`          // S3 region
	EnableColumnar      bool           `json:"enable_columnar"`              // Enable columnar storage
	DefaultShardCount   int            `json:"default_shard_count"`          // Default shard count for distributed tables
	// Scale-to-zero configuration
	ScaleToZeroEnabled  bool           `json:"scale_to_zero_enabled"`        // Enable automatic scale-to-zero
	IdleTimeoutSeconds  int            `json:"idle_timeout_seconds"`         // Seconds of inactivity before suspending (default: 300)
	WakeTimeoutSeconds  int            `json:"wake_timeout_seconds"`         // Max seconds to wait for cluster to wake (default: 60)
	LastActivityAt      *time.Time     `json:"last_activity_at,omitempty"`   // Last connection/query activity timestamp
	SuspendedAt         *time.Time     `json:"suspended_at,omitempty"`       // When cluster was suspended
	CreatedAt           time.Time      `json:"created_at"`
	UpdatedAt           time.Time      `json:"updated_at"`
	DeletedAt           *time.Time     `json:"deleted_at,omitempty"`
}

// TieringConfig represents the tiered storage configuration for a cluster.
// Defines data lifecycle policies for hot/warm/cold/frozen tiers.
type TieringConfig struct {
	Enabled         bool   `json:"enabled"`                   // Enable tiered storage
	HotDuration     string `json:"hotDuration"`               // e.g., "7d" - time before moving to warm tier
	WarmDuration    string `json:"warmDuration"`              // e.g., "30d" - time before moving to cold tier
	ColdDuration    string `json:"coldDuration"`              // e.g., "90d" - time before moving to frozen tier
	CompressionType string `json:"compressionType,omitempty"` // lz4, zstd (defaults to zstd)
}

// S3Config represents S3 storage configuration for tiered storage.
// Used for cold and frozen tiers with object storage backends.
type S3Config struct {
	Endpoint        string `json:"endpoint"`                  // S3 endpoint URL
	Bucket          string `json:"bucket"`                    // S3 bucket name
	Region          string `json:"region"`                    // S3 region
	AccessKeyID     string `json:"accessKeyId"`               // S3 access key
	SecretAccessKey string `json:"secretAccessKey,omitempty"` // S3 secret key (sensitive - omitted in responses)
}

// ClusterCreateRequest represents a request to create a new cluster.
type ClusterCreateRequest struct {
	Name              string         `json:"name"`
	Tier              ClusterTier    `json:"tier"`
	Provider          CloudProvider  `json:"provider"`
	Region            string         `json:"region"`
	Version           string         `json:"version,omitempty"`
	NodeCount         int            `json:"node_count,omitempty"`
	NodeSize          string         `json:"node_size,omitempty"`
	StorageGB         int            `json:"storage_gb,omitempty"`
	MaintenanceDay    string         `json:"maintenance_day,omitempty"`
	MaintenanceHour   int            `json:"maintenance_hour,omitempty"`
	BackupEnabled     bool           `json:"backup_enabled,omitempty"`
	BackupRetention   int            `json:"backup_retention_days,omitempty"`
	PoolerEnabled     bool           `json:"pooler_enabled,omitempty"`     // Enable PgBouncer connection pooling
	OrganizationID    *uuid.UUID     `json:"organization_id,omitempty"`    // Optional team/organization
	TieringConfig     *TieringConfig `json:"tieringConfig,omitempty"`      // Tiered storage configuration
	S3Config          *S3Config      `json:"s3Config,omitempty"`           // S3 configuration for cold/frozen tiers
	EnableColumnar    bool           `json:"enableColumnar"`               // Enable columnar storage for analytics
	DefaultShardCount int            `json:"defaultShardCount,omitempty"`  // Default number of shards for distributed tables
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

// ClusterStateResponse represents the response for cluster state queries.
type ClusterStateResponse struct {
	ClusterID     uuid.UUID     `json:"cluster_id"`
	Status        ClusterStatus `json:"status"`
	IsReady       bool          `json:"is_ready"`        // True if cluster can accept connections
	LastActivity  *time.Time    `json:"last_activity,omitempty"`
	SuspendedAt   *time.Time    `json:"suspended_at,omitempty"`
	WakeStartedAt *time.Time    `json:"wake_started_at,omitempty"`
	Message       string        `json:"message,omitempty"`
}

// SuspendClusterRequest represents a request to suspend a cluster.
type SuspendClusterRequest struct {
	Force          bool `json:"force"`           // Force suspend even with active connections
	DrainTimeout   int  `json:"drain_timeout"`   // Seconds to wait for connections to drain (default: 30)
}

// WakeClusterRequest represents a request to wake a suspended cluster.
type WakeClusterRequest struct {
	WaitForReady bool `json:"wait_for_ready"` // Block until cluster is ready (default: false)
	Timeout      int  `json:"timeout"`        // Timeout in seconds (default: 60)
}

// ScaleToZeroConfig represents scale-to-zero configuration for a cluster.
type ScaleToZeroConfig struct {
	Enabled            bool `json:"enabled"`
	IdleTimeoutSeconds int  `json:"idle_timeout_seconds"` // Default: 300 (5 minutes)
	WakeTimeoutSeconds int  `json:"wake_timeout_seconds"` // Default: 60
	MaxQueuedConns     int  `json:"max_queued_connections"` // Default: 100
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

	// Validate tiering configuration if provided
	if r.TieringConfig != nil && r.TieringConfig.Enabled {
		if r.TieringConfig.HotDuration == "" {
			return ErrTieringHotDurationRequired
		}
		if r.TieringConfig.WarmDuration == "" {
			return ErrTieringWarmDurationRequired
		}
		if r.TieringConfig.ColdDuration == "" {
			return ErrTieringColdDurationRequired
		}
		// Validate compression type if provided
		if r.TieringConfig.CompressionType != "" {
			if r.TieringConfig.CompressionType != "lz4" && r.TieringConfig.CompressionType != "zstd" {
				return ErrTieringInvalidCompression
			}
		}
	}

	// Validate S3 configuration if tiering is enabled
	if r.TieringConfig != nil && r.TieringConfig.Enabled {
		if r.S3Config == nil {
			return ErrS3ConfigRequired
		}
		if r.S3Config.Endpoint == "" {
			return ErrS3EndpointRequired
		}
		if r.S3Config.Bucket == "" {
			return ErrS3BucketRequired
		}
		if r.S3Config.Region == "" {
			return ErrS3RegionRequired
		}
		if r.S3Config.AccessKeyID == "" {
			return ErrS3AccessKeyRequired
		}
		if r.S3Config.SecretAccessKey == "" {
			return ErrS3SecretKeyRequired
		}
	}

	// Validate shard count if provided
	if r.DefaultShardCount < 0 {
		return ErrInvalidShardCount
	}
	if r.DefaultShardCount > 1024 {
		return ErrShardCountTooHigh
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
	// Enable pooler by default for production and enterprise tiers
	// Connection pooling is recommended for all production workloads
	if r.Tier == ClusterTierProduction || r.Tier == ClusterTierEnterprise {
		r.PoolerEnabled = true
	}

	// Apply tiering defaults if enabled
	if r.TieringConfig != nil && r.TieringConfig.Enabled {
		if r.TieringConfig.CompressionType == "" {
			r.TieringConfig.CompressionType = "zstd" // Default to zstd compression
		}
	}

	// Apply default shard count based on tier if not specified
	if r.DefaultShardCount == 0 {
		switch r.Tier {
		case ClusterTierDev:
			r.DefaultShardCount = 4
		case ClusterTierStarter:
			r.DefaultShardCount = 8
		case ClusterTierProduction:
			r.DefaultShardCount = 16
		case ClusterTierEnterprise:
			r.DefaultShardCount = 32
		}
	}

	// Enable columnar storage by default for production and enterprise tiers
	// Columnar storage provides better analytics performance
	if r.Tier == ClusterTierProduction || r.Tier == ClusterTierEnterprise {
		r.EnableColumnar = true
	}
}
