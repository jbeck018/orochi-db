// Package types provides shared types for Orochi Cloud services
package types

import "time"

// ClusterSize represents the size tier of a cluster
type ClusterSize string

const (
	ClusterSizeSmall  ClusterSize = "small"
	ClusterSizeMedium ClusterSize = "medium"
	ClusterSizeLarge  ClusterSize = "large"
	ClusterSizeXLarge ClusterSize = "xlarge"
)

// ClusterStatus represents the current state of a cluster
type ClusterStatus string

const (
	ClusterStatusPending      ClusterStatus = "pending"
	ClusterStatusProvisioning ClusterStatus = "provisioning"
	ClusterStatusRunning      ClusterStatus = "running"
	ClusterStatusScaling      ClusterStatus = "scaling"
	ClusterStatusUpdating     ClusterStatus = "updating"
	ClusterStatusDeleting     ClusterStatus = "deleting"
	ClusterStatusFailed       ClusterStatus = "failed"
	ClusterStatusStopped      ClusterStatus = "stopped"
)

// Cluster represents an Orochi DB cluster
type Cluster struct {
	ID          string        `json:"id"`
	Name        string        `json:"name"`
	OwnerID     string        `json:"owner_id"`
	Size        ClusterSize   `json:"size"`
	Region      string        `json:"region"`
	Status      ClusterStatus `json:"status"`
	Replicas    int           `json:"replicas"`
	Version     string        `json:"version"`
	Endpoint    string        `json:"endpoint,omitempty"`
	Port        int           `json:"port,omitempty"`
	StorageGB   int           `json:"storage_gb"`
	CreatedAt   time.Time     `json:"created_at"`
	UpdatedAt   time.Time     `json:"updated_at"`
	DeletedAt   *time.Time    `json:"deleted_at,omitempty"`
	Config      ClusterConfig `json:"config"`
	Metrics     *ClusterMetrics `json:"metrics,omitempty"`
}

// ClusterConfig holds cluster configuration
type ClusterConfig struct {
	// PostgreSQL configuration
	MaxConnections       int    `json:"max_connections"`
	SharedBuffers        string `json:"shared_buffers"`
	WorkMem              string `json:"work_mem"`
	MaintenanceWorkMem   string `json:"maintenance_work_mem"`
	EffectiveCacheSize   string `json:"effective_cache_size"`

	// Orochi-specific configuration
	OrochiShardCount     int    `json:"orochi_shard_count"`
	OrochiReplicationFactor int  `json:"orochi_replication_factor"`
	OrochiEnableColumnar bool   `json:"orochi_enable_columnar"`
	OrochiEnableTimeSeries bool `json:"orochi_enable_time_series"`

	// Backup configuration
	BackupEnabled        bool   `json:"backup_enabled"`
	BackupSchedule       string `json:"backup_schedule"` // Cron expression
	BackupRetentionDays  int    `json:"backup_retention_days"`

	// High availability
	HAEnabled            bool   `json:"ha_enabled"`
	SyncReplicas         int    `json:"sync_replicas"`
}

// ClusterMetrics holds cluster metrics
type ClusterMetrics struct {
	CPUUsage           float64 `json:"cpu_usage"`
	MemoryUsage        float64 `json:"memory_usage"`
	StorageUsage       float64 `json:"storage_usage"`
	ConnectionCount    int     `json:"connection_count"`
	QueriesPerSecond   float64 `json:"queries_per_second"`
	TransactionsPerSec float64 `json:"transactions_per_sec"`
	ReplicationLag     float64 `json:"replication_lag_ms"`
	Timestamp          time.Time `json:"timestamp"`
}

// CreateClusterRequest represents a request to create a cluster
type CreateClusterRequest struct {
	Name      string      `json:"name" validate:"required,min=3,max=63"`
	Size      ClusterSize `json:"size" validate:"required,oneof=small medium large xlarge"`
	Region    string      `json:"region" validate:"required"`
	Replicas  int         `json:"replicas" validate:"min=1,max=10"`
	StorageGB int         `json:"storage_gb" validate:"min=10,max=10000"`
	Config    *ClusterConfig `json:"config,omitempty"`
}

// UpdateClusterRequest represents a request to update a cluster
type UpdateClusterRequest struct {
	Size      *ClusterSize    `json:"size,omitempty"`
	Replicas  *int            `json:"replicas,omitempty"`
	StorageGB *int            `json:"storage_gb,omitempty"`
	Config    *ClusterConfig  `json:"config,omitempty"`
}

// ScaleClusterRequest represents a request to scale a cluster
type ScaleClusterRequest struct {
	Replicas  *int         `json:"replicas,omitempty"`
	Size      *ClusterSize `json:"size,omitempty"`
}

// ClusterSizeSpec defines resources for each cluster size
var ClusterSizeSpecs = map[ClusterSize]struct {
	CPUCores    float64
	MemoryGB    int
	StorageGB   int
	MaxReplicas int
}{
	ClusterSizeSmall:  {CPUCores: 1, MemoryGB: 2, StorageGB: 20, MaxReplicas: 2},
	ClusterSizeMedium: {CPUCores: 2, MemoryGB: 4, StorageGB: 50, MaxReplicas: 4},
	ClusterSizeLarge:  {CPUCores: 4, MemoryGB: 8, StorageGB: 100, MaxReplicas: 6},
	ClusterSizeXLarge: {CPUCores: 8, MemoryGB: 16, StorageGB: 200, MaxReplicas: 10},
}

// SupportedRegions lists all supported deployment regions
var SupportedRegions = []string{
	"us-east-1",
	"us-west-2",
	"eu-west-1",
	"eu-central-1",
	"ap-southeast-1",
	"ap-northeast-1",
}
