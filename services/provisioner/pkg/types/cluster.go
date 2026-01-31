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
	Enabled           bool              `json:"enabled"`
	// PgDog-specific configuration
	Image             string            `json:"image,omitempty"`
	ImageTag          string            `json:"image_tag,omitempty"`
	Replicas          int32             `json:"replicas"`
	Resources         ResourceSpec      `json:"resources,omitempty"`
	Mode              string            `json:"mode"` // transaction, session
	MaxPoolSize       int32             `json:"max_pool_size"`
	MinPoolSize       int32             `json:"min_pool_size"`
	IdleTimeout       int32             `json:"idle_timeout_seconds"`
	ReadWriteSplit    bool              `json:"read_write_split"`
	ShardingEnabled   bool              `json:"sharding_enabled"`
	ShardCount        int32             `json:"shard_count,omitempty"`
	TLSEnabled        bool              `json:"tls_enabled"`
	JWTAuth           *JWTAuthConfig    `json:"jwt_auth,omitempty"`
	// Legacy CloudNativePG PgBouncer configuration (for backward compatibility)
	Instances         int32             `json:"instances,omitempty"` // Legacy: same as Replicas
	Type              string            `json:"type,omitempty"`      // Legacy: rw/ro for CloudNativePG pooler
	PgBouncer         *PgBouncerConfig  `json:"pgbouncer,omitempty"` // Legacy: CloudNativePG pooler
}

// ResourceSpec defines resource requests and limits for pooler pods
type ResourceSpec struct {
	CPURequest    string `json:"cpu_request,omitempty"`
	CPULimit      string `json:"cpu_limit,omitempty"`
	MemoryRequest string `json:"memory_request,omitempty"`
	MemoryLimit   string `json:"memory_limit,omitempty"`
}

// JWTAuthConfig configures JWT authentication for PgDog
type JWTAuthConfig struct {
	Enabled       bool   `json:"enabled"`
	Issuer        string `json:"issuer,omitempty"`
	Audience      string `json:"audience,omitempty"`
	PublicKeyPath string `json:"public_key_path,omitempty"`
	SecretName    string `json:"secret_name,omitempty"` // K8s secret containing JWT keys
}

// PgBouncerConfig configures PgBouncer (legacy CloudNativePG pooler)
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
	Pooler                    *PoolerStatus    `json:"pooler,omitempty"`
}

// PoolerStatus represents the status of the connection pooler
type PoolerStatus struct {
	Enabled         bool   `json:"enabled"`
	Ready           bool   `json:"ready"`
	Replicas        int32  `json:"replicas"`
	ReadyReplicas   int32  `json:"readyReplicas"`
	Endpoint        string `json:"endpoint,omitempty"`
	JWTEndpoint     string `json:"jwtEndpoint,omitempty"`
	InternalEndpoint string `json:"internalEndpoint,omitempty"`
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

// SuspendInfo contains information about a suspended cluster.
type SuspendInfo struct {
	Suspended        bool      `json:"suspended"`
	SuspendedAt      time.Time `json:"suspendedAt,omitempty"`
	PreviousReplicas int32     `json:"previousReplicas,omitempty"`
}
