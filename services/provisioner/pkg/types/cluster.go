// Package types provides shared type definitions for the provisioner service.
package types

import (
	"time"
)

// ClusterPhase represents the current phase of a cluster
type ClusterPhase string

const (
	ClusterPhaseUnknown     ClusterPhase = "Unknown"
	ClusterPhaseSettingUp   ClusterPhase = "Setting up"
	ClusterPhaseHealthy     ClusterPhase = "Cluster in healthy state"
	ClusterPhaseUnhealthy   ClusterPhase = "Cluster in unhealthy state"
	ClusterPhaseUpgrading   ClusterPhase = "Upgrading cluster"
	ClusterPhaseFailingOver ClusterPhase = "Failing over"
	ClusterPhaseDeleting    ClusterPhase = "Deleting"
)

// CompressionType represents the compression algorithm used for data
type CompressionType string

const (
	CompressionNone       CompressionType = "none"
	CompressionLZ4        CompressionType = "lz4"
	CompressionZSTD       CompressionType = "zstd"
	CompressionDelta      CompressionType = "delta"
	CompressionGorilla    CompressionType = "gorilla"
	CompressionDictionary CompressionType = "dictionary"
	CompressionRLE        CompressionType = "rle"
)

// BackupMethod represents the backup method used
type BackupMethod string

const (
	BackupMethodBarmanObjectStore BackupMethod = "barmanObjectStore"
	BackupMethodVolumeSnapshot    BackupMethod = "volumeSnapshot"
)

// BackupPhase represents the current phase of a backup
type BackupPhase string

const (
	BackupPhasePending   BackupPhase = "pending"
	BackupPhaseRunning   BackupPhase = "running"
	BackupPhaseCompleted BackupPhase = "completed"
	BackupPhaseFailed    BackupPhase = "failed"
)

// PoolMode represents the connection pooler mode
type PoolMode string

const (
	PoolModeSession     PoolMode = "session"
	PoolModeTransaction PoolMode = "transaction"
	PoolModeStatement   PoolMode = "statement"
)

// ClusterSpec defines the desired state of a PostgreSQL cluster
type ClusterSpec struct {
	// Name is the cluster name
	Name string `json:"name"`
	// Namespace is the Kubernetes namespace
	Namespace string `json:"namespace"`
	// Instances is the number of PostgreSQL instances
	Instances int32 `json:"instances"`
	// PostgresVersion is the PostgreSQL major version (e.g., "16", "17")
	PostgresVersion string `json:"postgresVersion"`
	// ImageName is the container image to use
	ImageName string `json:"imageName,omitempty"`
	// Resources defines CPU and memory requirements
	Resources ResourceRequirements `json:"resources"`
	// Storage defines the storage configuration
	Storage StorageSpec `json:"storage"`
	// OrochiConfig configures the Orochi DB extension
	OrochiConfig *OrochiConfig `json:"orochiConfig,omitempty"`
	// BackupConfig configures backups
	BackupConfig *BackupConfiguration `json:"backupConfig,omitempty"`
	// Monitoring configures monitoring
	Monitoring *MonitoringConfig `json:"monitoring,omitempty"`
	// Pooler configures the connection pooler
	Pooler *ConnectionPoolerSpec `json:"pooler,omitempty"`
	// Affinity configures pod affinity and anti-affinity
	Affinity *AffinityConfig `json:"affinity,omitempty"`
	// Labels are additional labels to apply
	Labels map[string]string `json:"labels,omitempty"`
	// Annotations are additional annotations to apply
	Annotations map[string]string `json:"annotations,omitempty"`
}

// ResourceRequirements defines compute resource requirements
type ResourceRequirements struct {
	CPURequest    string `json:"cpuRequest,omitempty"`
	CPULimit      string `json:"cpuLimit,omitempty"`
	MemoryRequest string `json:"memoryRequest,omitempty"`
	MemoryLimit   string `json:"memoryLimit,omitempty"`
	HugePages2Mi  string `json:"hugepages2Mi,omitempty"`
	HugePages1Gi  string `json:"hugepages1Gi,omitempty"`
}

// StorageSpec defines storage configuration
type StorageSpec struct {
	Size                 string `json:"size"`
	StorageClass         string `json:"storageClass,omitempty"`
	ResizeInUseVolumes   bool   `json:"resizeInUseVolumes,omitempty"`
	WALSize              string `json:"walSize,omitempty"`
	WALStorageClass      string `json:"walStorageClass,omitempty"`
	TablespaceSize       string `json:"tablespaceSize,omitempty"`
}

// OrochiConfig configures the Orochi DB extension
type OrochiConfig struct {
	Enabled             bool            `json:"enabled"`
	DefaultShardCount   int32           `json:"defaultShardCount,omitempty"`
	ChunkInterval       string          `json:"chunkInterval,omitempty"`
	EnableColumnar      bool            `json:"enableColumnar,omitempty"`
	DefaultCompression  CompressionType `json:"defaultCompression,omitempty"`
	Tiering             *TieringConfig  `json:"tiering,omitempty"`
	CustomParameters    map[string]string `json:"customParameters,omitempty"`
}

// TieringConfig configures tiered storage
type TieringConfig struct {
	Enabled      bool      `json:"enabled"`
	HotDuration  string    `json:"hotDuration,omitempty"`
	WarmDuration string    `json:"warmDuration,omitempty"`
	ColdDuration string    `json:"coldDuration,omitempty"`
	S3Config     *S3Config `json:"s3Config,omitempty"`
}

// S3Config configures S3-compatible storage
type S3Config struct {
	Endpoint        string `json:"endpoint"`
	Bucket          string `json:"bucket"`
	Region          string `json:"region,omitempty"`
	AccessKeySecret string `json:"accessKeySecret,omitempty"`
}

// BackupConfiguration configures backup settings
type BackupConfiguration struct {
	RetentionPolicy    string              `json:"retentionPolicy,omitempty"`
	BarmanObjectStore  *BarmanObjectStore  `json:"barmanObjectStore,omitempty"`
	ScheduledBackup    *ScheduledBackup    `json:"scheduledBackup,omitempty"`
	VolumeSnapshot     *VolumeSnapshotSpec `json:"volumeSnapshot,omitempty"`
}

// BarmanObjectStore configures backup to object storage
type BarmanObjectStore struct {
	DestinationPath string         `json:"destinationPath"`
	Endpoint        string         `json:"endpoint,omitempty"`
	S3Credentials   *S3Credentials `json:"s3Credentials,omitempty"`
	WAL             *WALConfig     `json:"wal,omitempty"`
	Data            *DataConfig    `json:"data,omitempty"`
}

// S3Credentials configures S3 authentication
type S3Credentials struct {
	AccessKeyIDSecret     string `json:"accessKeyIdSecret,omitempty"`
	SecretAccessKeySecret string `json:"secretAccessKeySecret,omitempty"`
	RegionSecret          string `json:"regionSecret,omitempty"`
	SessionTokenSecret    string `json:"sessionTokenSecret,omitempty"`
	InheritFromIAMRole    bool   `json:"inheritFromIamRole,omitempty"`
}

// WALConfig configures WAL archiving
type WALConfig struct {
	Compression CompressionType `json:"compression,omitempty"`
	MaxParallel string          `json:"maxParallel,omitempty"`
}

// DataConfig configures data backup
type DataConfig struct {
	Compression         CompressionType `json:"compression,omitempty"`
	Jobs                int32           `json:"jobs,omitempty"`
	ImmediateCheckpoint bool            `json:"immediateCheckpoint,omitempty"`
}

// ScheduledBackup configures automatic backups
type ScheduledBackup struct {
	Enabled              bool   `json:"enabled"`
	Schedule             string `json:"schedule,omitempty"`
	Immediate            bool   `json:"immediate,omitempty"`
	BackupOwnerReference string `json:"backupOwnerReference,omitempty"`
}

// VolumeSnapshotSpec configures volume snapshots
type VolumeSnapshotSpec struct {
	Enabled       bool   `json:"enabled"`
	SnapshotClass string `json:"snapshotClass,omitempty"`
}

// MonitoringConfig configures monitoring
type MonitoringConfig struct {
	EnablePodMonitor     bool              `json:"enablePodMonitor,omitempty"`
	EnablePrometheusRule bool              `json:"enablePrometheusRule,omitempty"`
	CustomQueries        map[string]string `json:"customQueries,omitempty"`
	ScrapeInterval       string            `json:"scrapeInterval,omitempty"`
}

// ConnectionPoolerSpec configures the connection pooler
type ConnectionPoolerSpec struct {
	Enabled   bool            `json:"enabled"`
	Instances int32           `json:"instances,omitempty"`
	Type      string          `json:"type,omitempty"`
	PgBouncer *PgBouncerConfig `json:"pgbouncer,omitempty"`
}

// PgBouncerConfig configures PgBouncer
type PgBouncerConfig struct {
	PoolMode        PoolMode          `json:"poolMode,omitempty"`
	DefaultPoolSize int32             `json:"defaultPoolSize,omitempty"`
	MaxClientConn   int32             `json:"maxClientConn,omitempty"`
	Parameters      map[string]string `json:"parameters,omitempty"`
}

// AffinityConfig configures pod affinity
type AffinityConfig struct {
	EnablePodAntiAffinity bool              `json:"enablePodAntiAffinity,omitempty"`
	TopologyKey           string            `json:"topologyKey,omitempty"`
	NodeSelector          map[string]string `json:"nodeSelector,omitempty"`
	Tolerations           []Toleration      `json:"tolerations,omitempty"`
}

// Toleration configures pod tolerations
type Toleration struct {
	Key               string `json:"key,omitempty"`
	Operator          string `json:"operator,omitempty"`
	Value             string `json:"value,omitempty"`
	Effect            string `json:"effect,omitempty"`
	TolerationSeconds *int64 `json:"tolerationSeconds,omitempty"`
}

// ClusterStatus represents the current status of a cluster
type ClusterStatus struct {
	Phase                     ClusterPhase     `json:"phase"`
	ReadyInstances            int32            `json:"readyInstances"`
	TotalInstances            int32            `json:"totalInstances"`
	CurrentPrimary            string           `json:"currentPrimary,omitempty"`
	CurrentPrimaryTimestamp   string           `json:"currentPrimaryTimestamp,omitempty"`
	FirstRecoverabilityPoint  string           `json:"firstRecoverabilityPoint,omitempty"`
	LastSuccessfulBackup      string           `json:"lastSuccessfulBackup,omitempty"`
	Instances                 []InstanceStatus `json:"instances,omitempty"`
	Conditions                []string         `json:"conditions,omitempty"`
}

// InstanceStatus represents the status of a single instance
type InstanceStatus struct {
	Name      string    `json:"name"`
	Role      string    `json:"role"`
	Ready     bool      `json:"ready"`
	PodIP     string    `json:"podIP,omitempty"`
	StartTime time.Time `json:"startTime,omitempty"`
}

// ClusterInfo contains full cluster information
type ClusterInfo struct {
	ClusterID string        `json:"clusterId"`
	Name      string        `json:"name"`
	Namespace string        `json:"namespace"`
	Spec      ClusterSpec   `json:"spec"`
	Status    ClusterStatus `json:"status"`
	CreatedAt time.Time     `json:"createdAt"`
	UpdatedAt time.Time     `json:"updatedAt"`
}

// BackupInfo contains backup information
type BackupInfo struct {
	BackupID    string      `json:"backupId"`
	Name        string      `json:"name"`
	ClusterName string      `json:"clusterName"`
	Method      BackupMethod `json:"method"`
	Phase       BackupPhase  `json:"phase"`
	StartedAt   time.Time   `json:"startedAt,omitempty"`
	CompletedAt time.Time   `json:"completedAt,omitempty"`
	BeginWAL    string      `json:"beginWal,omitempty"`
	EndWAL      string      `json:"endWal,omitempty"`
	BeginLSN    string      `json:"beginLsn,omitempty"`
	EndLSN      string      `json:"endLsn,omitempty"`
	BackupSize  int64       `json:"backupSize,omitempty"`
}

// RestoreOptions configures point-in-time recovery
type RestoreOptions struct {
	BackupID               string `json:"backupId,omitempty"`
	RecoveryTargetTime     string `json:"recoveryTargetTime,omitempty"`
	RecoveryTargetLSN      string `json:"recoveryTargetLsn,omitempty"`
	RecoveryTargetName     string `json:"recoveryTargetName,omitempty"`
	RecoveryTargetInclusive bool   `json:"recoveryTargetInclusive,omitempty"`
}

// ClusterCondition represents a condition on a cluster
type ClusterCondition struct {
	Type               string    `json:"type"`
	Status             string    `json:"status"`
	LastTransitionTime time.Time `json:"lastTransitionTime"`
	Reason             string    `json:"reason,omitempty"`
	Message            string    `json:"message,omitempty"`
}

// DefaultClusterSpec returns a ClusterSpec with sensible defaults
func DefaultClusterSpec(name, namespace string) ClusterSpec {
	return ClusterSpec{
		Name:            name,
		Namespace:       namespace,
		Instances:       3,
		PostgresVersion: "16",
		Resources: ResourceRequirements{
			CPURequest:    "500m",
			CPULimit:      "2",
			MemoryRequest: "1Gi",
			MemoryLimit:   "4Gi",
		},
		Storage: StorageSpec{
			Size:               "10Gi",
			ResizeInUseVolumes: true,
		},
		OrochiConfig: &OrochiConfig{
			Enabled:            true,
			DefaultShardCount:  32,
			ChunkInterval:      "1 day",
			EnableColumnar:     true,
			DefaultCompression: CompressionLZ4,
		},
		Monitoring: &MonitoringConfig{
			EnablePodMonitor: true,
			ScrapeInterval:   "30s",
		},
		Affinity: &AffinityConfig{
			EnablePodAntiAffinity: true,
			TopologyKey:           "kubernetes.io/hostname",
		},
	}
}

// Validate validates the cluster spec
func (s *ClusterSpec) Validate() error {
	if s.Name == "" {
		return ErrClusterNameRequired
	}
	if s.Namespace == "" {
		return ErrNamespaceRequired
	}
	if s.Instances < 1 || s.Instances > 100 {
		return ErrInvalidInstanceCount
	}
	if s.Storage.Size == "" {
		return ErrStorageSizeRequired
	}
	return nil
}

// Error types for validation
var (
	ErrClusterNameRequired  = &ValidationError{Field: "name", Message: "cluster name is required"}
	ErrNamespaceRequired    = &ValidationError{Field: "namespace", Message: "namespace is required"}
	ErrInvalidInstanceCount = &ValidationError{Field: "instances", Message: "instance count must be between 1 and 100"}
	ErrStorageSizeRequired  = &ValidationError{Field: "storage.size", Message: "storage size is required"}
)

// ValidationError represents a validation error
type ValidationError struct {
	Field   string
	Message string
}

func (e *ValidationError) Error() string {
	return e.Field + ": " + e.Message
}

// BranchMethod represents the method used to create a branch
type BranchMethod string

const (
	// BranchMethodVolumeSnapshot creates a branch via volume snapshot (instant, requires CSI)
	BranchMethodVolumeSnapshot BranchMethod = "volumeSnapshot"
	// BranchMethodPgBasebackup creates a branch via pg_basebackup (slower, always available)
	BranchMethodPgBasebackup BranchMethod = "pg_basebackup"
	// BranchMethodClone creates a branch using PG18+ file_copy_method=clone with XFS reflinks
	BranchMethodClone BranchMethod = "clone"
	// BranchMethodPITR creates a branch from a point-in-time recovery
	BranchMethodPITR BranchMethod = "pitr"
)

// BranchPhase represents the current phase of a branch
type BranchPhase string

const (
	BranchPhasePending   BranchPhase = "pending"
	BranchPhaseCreating  BranchPhase = "creating"
	BranchPhaseReady     BranchPhase = "ready"
	BranchPhaseFailed    BranchPhase = "failed"
	BranchPhaseDeleting  BranchPhase = "deleting"
	BranchPhasePromoting BranchPhase = "promoting"
)

// BranchSpec defines the specification for creating a database branch
type BranchSpec struct {
	// Name is the branch name
	Name string `json:"name"`
	// SourceCluster is the name of the parent cluster
	SourceCluster string `json:"sourceCluster"`
	// SourceNamespace is the namespace of the parent cluster
	SourceNamespace string `json:"sourceNamespace"`
	// TargetNamespace is the namespace for the branch (optional, defaults to source namespace)
	TargetNamespace string `json:"targetNamespace,omitempty"`
	// Method is the branching method to use
	Method BranchMethod `json:"method,omitempty"`
	// PointInTime specifies a point-in-time for PITR branches (RFC3339 format)
	PointInTime string `json:"pointInTime,omitempty"`
	// LSN specifies a specific LSN for PITR branches
	LSN string `json:"lsn,omitempty"`
	// Instances overrides the instance count (defaults to 1 for branches)
	Instances int32 `json:"instances,omitempty"`
	// Inherit specifies whether to inherit parent cluster configuration
	Inherit bool `json:"inherit,omitempty"`
	// Labels are additional labels to apply
	Labels map[string]string `json:"labels,omitempty"`
	// Annotations are additional annotations to apply
	Annotations map[string]string `json:"annotations,omitempty"`
}

// BranchStatus represents the current status of a branch
type BranchStatus struct {
	Phase              BranchPhase `json:"phase"`
	Message            string      `json:"message,omitempty"`
	ClusterName        string      `json:"clusterName,omitempty"`
	ConnectionString   string      `json:"connectionString,omitempty"`
	PoolerConnection   string      `json:"poolerConnection,omitempty"`
	SourceLSN          string      `json:"sourceLsn,omitempty"`
	SourceTimestamp    string      `json:"sourceTimestamp,omitempty"`
	CreationMethod     BranchMethod `json:"creationMethod"`
	CreationDuration   string      `json:"creationDuration,omitempty"`
}

// BranchInfo contains full branch information
type BranchInfo struct {
	BranchID        string            `json:"branchId"`
	Name            string            `json:"name"`
	ParentCluster   string            `json:"parentCluster"`
	ParentNamespace string            `json:"parentNamespace"`
	Namespace       string            `json:"namespace"`
	Spec            BranchSpec        `json:"spec"`
	Status          BranchStatus      `json:"status"`
	CreatedAt       time.Time         `json:"createdAt"`
	UpdatedAt       time.Time         `json:"updatedAt"`
	Labels          map[string]string `json:"labels,omitempty"`
}

// VolumeSnapshotConfig configures volume snapshot-based branching
type VolumeSnapshotConfig struct {
	// Enabled enables volume snapshot branching
	Enabled bool `json:"enabled"`
	// SnapshotClass is the VolumeSnapshotClass to use
	SnapshotClass string `json:"snapshotClass,omitempty"`
	// RetentionPolicy specifies how long to retain snapshots
	RetentionPolicy string `json:"retentionPolicy,omitempty"`
}

// ValidateBranchSpec validates a branch specification
func (s *BranchSpec) Validate() error {
	if s.Name == "" {
		return &ValidationError{Field: "name", Message: "branch name is required"}
	}
	if s.SourceCluster == "" {
		return &ValidationError{Field: "sourceCluster", Message: "source cluster is required"}
	}
	if s.SourceNamespace == "" {
		return &ValidationError{Field: "sourceNamespace", Message: "source namespace is required"}
	}
	// Validate PITR options
	if s.PointInTime != "" && s.LSN != "" {
		return &ValidationError{Field: "pointInTime", Message: "cannot specify both pointInTime and LSN"}
	}
	if (s.PointInTime != "" || s.LSN != "") && s.Method != "" && s.Method != BranchMethodPITR {
		return &ValidationError{Field: "method", Message: "pointInTime or LSN requires PITR method"}
	}
	return nil
}

// DefaultBranchMethod returns the recommended branch method based on environment
func DefaultBranchMethod(hasVolumeSnapshots, hasXFSReflinks bool) BranchMethod {
	// Prefer volume snapshots for instant branching
	if hasVolumeSnapshots {
		return BranchMethodVolumeSnapshot
	}
	// Fall back to XFS clone method if available (PG18+)
	if hasXFSReflinks {
		return BranchMethodClone
	}
	// Default to pg_basebackup (always available)
	return BranchMethodPgBasebackup
}
