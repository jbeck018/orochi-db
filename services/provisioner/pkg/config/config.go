// Package config provides configuration management for the provisioner service.
package config

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"gopkg.in/yaml.v3"
)

// Config holds the provisioner service configuration
type Config struct {
	// Server configuration
	Server ServerConfig `yaml:"server"`
	// Kubernetes configuration
	Kubernetes KubernetesConfig `yaml:"kubernetes"`
	// CloudNativePG configuration
	CloudNativePG CloudNativePGConfig `yaml:"cloudNativePG"`
	// OrochiDB default configuration
	OrochiDefaults OrochiDefaultsConfig `yaml:"orochiDefaults"`
	// Metrics configuration
	Metrics MetricsConfig `yaml:"metrics"`
	// Logging configuration
	Logging LoggingConfig `yaml:"logging"`
}

// ServerConfig configures the gRPC server
type ServerConfig struct {
	// GRPCPort is the port for the gRPC server
	GRPCPort int `yaml:"grpcPort"`
	// HealthPort is the port for the health check endpoint
	HealthPort int `yaml:"healthPort"`
	// TLSEnabled enables TLS for gRPC
	TLSEnabled bool `yaml:"tlsEnabled"`
	// TLSCertFile is the path to the TLS certificate
	TLSCertFile string `yaml:"tlsCertFile"`
	// TLSKeyFile is the path to the TLS private key
	TLSKeyFile string `yaml:"tlsKeyFile"`
	// MaxConcurrentStreams limits concurrent gRPC streams
	MaxConcurrentStreams uint32 `yaml:"maxConcurrentStreams"`
	// ShutdownTimeout is the graceful shutdown timeout
	ShutdownTimeout time.Duration `yaml:"shutdownTimeout"`
}

// KubernetesConfig configures Kubernetes client behavior
type KubernetesConfig struct {
	// InCluster indicates whether running inside a cluster
	InCluster bool `yaml:"inCluster"`
	// KubeconfigPath is the path to kubeconfig (if not in-cluster)
	KubeconfigPath string `yaml:"kubeconfigPath"`
	// DefaultNamespace is the default namespace for operations
	DefaultNamespace string `yaml:"defaultNamespace"`
	// QPS is the queries per second limit
	QPS float32 `yaml:"qps"`
	// Burst is the burst limit for throttling
	Burst int `yaml:"burst"`
	// Timeout is the default timeout for API calls
	Timeout time.Duration `yaml:"timeout"`
	// RetryConfig configures retry behavior
	RetryConfig RetryConfig `yaml:"retryConfig"`
}

// RetryConfig configures retry behavior
type RetryConfig struct {
	// MaxRetries is the maximum number of retries
	MaxRetries int `yaml:"maxRetries"`
	// InitialBackoff is the initial backoff duration
	InitialBackoff time.Duration `yaml:"initialBackoff"`
	// MaxBackoff is the maximum backoff duration
	MaxBackoff time.Duration `yaml:"maxBackoff"`
	// BackoffMultiplier is the backoff multiplier
	BackoffMultiplier float64 `yaml:"backoffMultiplier"`
}

// CloudNativePGConfig configures CloudNativePG operator settings
type CloudNativePGConfig struct {
	// OperatorNamespace is the namespace where CNPG operator runs
	OperatorNamespace string `yaml:"operatorNamespace"`
	// DefaultImageCatalog is the default image catalog
	DefaultImageCatalog string `yaml:"defaultImageCatalog"`
	// DefaultPostgresVersion is the default PostgreSQL version
	DefaultPostgresVersion string `yaml:"defaultPostgresVersion"`
	// WatchNamespaces lists namespaces to watch (empty = all)
	WatchNamespaces []string `yaml:"watchNamespaces"`
	// ClusterReadyTimeout is the timeout for cluster ready check
	ClusterReadyTimeout time.Duration `yaml:"clusterReadyTimeout"`
	// DefaultStorageClass is the default storage class
	DefaultStorageClass string `yaml:"defaultStorageClass"`
	// DefaultBackupRetention is the default backup retention
	DefaultBackupRetention string `yaml:"defaultBackupRetention"`
}

// OrochiDefaultsConfig provides default Orochi DB configuration
type OrochiDefaultsConfig struct {
	// DefaultShardCount is the default shard count
	DefaultShardCount int32 `yaml:"defaultShardCount"`
	// DefaultChunkInterval is the default chunk interval
	DefaultChunkInterval string `yaml:"defaultChunkInterval"`
	// DefaultCompression is the default compression type
	DefaultCompression string `yaml:"defaultCompression"`
	// EnableColumnarByDefault enables columnar storage by default
	EnableColumnarByDefault bool `yaml:"enableColumnarByDefault"`
	// ExtensionVersion is the Orochi extension version
	ExtensionVersion string `yaml:"extensionVersion"`
	// SharedPreloadLibraries lists libraries to preload
	SharedPreloadLibraries []string `yaml:"sharedPreloadLibraries"`
}

// MetricsConfig configures Prometheus metrics
type MetricsConfig struct {
	// Enabled enables metrics collection
	Enabled bool `yaml:"enabled"`
	// Port is the metrics server port
	Port int `yaml:"port"`
	// Path is the metrics endpoint path
	Path string `yaml:"path"`
}

// LoggingConfig configures logging
type LoggingConfig struct {
	// Level is the log level (debug, info, warn, error)
	Level string `yaml:"level"`
	// Format is the log format (json, console)
	Format string `yaml:"format"`
	// OutputPaths are the log output destinations
	OutputPaths []string `yaml:"outputPaths"`
}

// DefaultConfig returns a config with sensible defaults
func DefaultConfig() *Config {
	return &Config{
		Server: ServerConfig{
			GRPCPort:             8080,
			HealthPort:           8081,
			TLSEnabled:           false,
			MaxConcurrentStreams: 100,
			ShutdownTimeout:      30 * time.Second,
		},
		Kubernetes: KubernetesConfig{
			InCluster:        true,
			DefaultNamespace: "orochi-cloud",
			QPS:              50,
			Burst:            100,
			Timeout:          30 * time.Second,
			RetryConfig: RetryConfig{
				MaxRetries:        5,
				InitialBackoff:    100 * time.Millisecond,
				MaxBackoff:        30 * time.Second,
				BackoffMultiplier: 2.0,
			},
		},
		CloudNativePG: CloudNativePGConfig{
			OperatorNamespace:      "cnpg-system",
			DefaultPostgresVersion: "16",
			ClusterReadyTimeout:    10 * time.Minute,
			DefaultStorageClass:    "standard",
			DefaultBackupRetention: "30d",
		},
		OrochiDefaults: OrochiDefaultsConfig{
			DefaultShardCount:       32,
			DefaultChunkInterval:    "1 day",
			DefaultCompression:      "lz4",
			EnableColumnarByDefault: true,
			ExtensionVersion:        "1.0.0",
			SharedPreloadLibraries:  []string{"orochi"},
		},
		Metrics: MetricsConfig{
			Enabled: true,
			Port:    9090,
			Path:    "/metrics",
		},
		Logging: LoggingConfig{
			Level:       "info",
			Format:      "json",
			OutputPaths: []string{"stdout"},
		},
	}
}

// LoadConfig loads configuration from a file
func LoadConfig(path string) (*Config, error) {
	cfg := DefaultConfig()

	if path != "" {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("failed to read config file: %w", err)
		}

		if err := yaml.Unmarshal(data, cfg); err != nil {
			return nil, fmt.Errorf("failed to parse config file: %w", err)
		}
	}

	// Override with environment variables
	cfg.applyEnvOverrides()

	if err := cfg.Validate(); err != nil {
		return nil, fmt.Errorf("invalid configuration: %w", err)
	}

	return cfg, nil
}

// applyEnvOverrides applies environment variable overrides
func (c *Config) applyEnvOverrides() {
	if port := os.Getenv("PROVISIONER_GRPC_PORT"); port != "" {
		if p, err := strconv.Atoi(port); err == nil {
			c.Server.GRPCPort = p
		}
	}

	if port := os.Getenv("PROVISIONER_HEALTH_PORT"); port != "" {
		if p, err := strconv.Atoi(port); err == nil {
			c.Server.HealthPort = p
		}
	}

	if ns := os.Getenv("PROVISIONER_DEFAULT_NAMESPACE"); ns != "" {
		c.Kubernetes.DefaultNamespace = ns
	}

	if kubeconfig := os.Getenv("KUBECONFIG"); kubeconfig != "" {
		c.Kubernetes.KubeconfigPath = kubeconfig
		c.Kubernetes.InCluster = false
	}

	if level := os.Getenv("PROVISIONER_LOG_LEVEL"); level != "" {
		c.Logging.Level = level
	}

	if format := os.Getenv("PROVISIONER_LOG_FORMAT"); format != "" {
		c.Logging.Format = format
	}

	if os.Getenv("PROVISIONER_TLS_ENABLED") == "true" {
		c.Server.TLSEnabled = true
	}

	if cert := os.Getenv("PROVISIONER_TLS_CERT"); cert != "" {
		c.Server.TLSCertFile = cert
	}

	if key := os.Getenv("PROVISIONER_TLS_KEY"); key != "" {
		c.Server.TLSKeyFile = key
	}

	if version := os.Getenv("OROCHI_DEFAULT_PG_VERSION"); version != "" {
		c.CloudNativePG.DefaultPostgresVersion = version
	}

	if sc := os.Getenv("OROCHI_DEFAULT_STORAGE_CLASS"); sc != "" {
		c.CloudNativePG.DefaultStorageClass = sc
	}
}

// Validate validates the configuration
func (c *Config) Validate() error {
	if c.Server.GRPCPort < 1 || c.Server.GRPCPort > 65535 {
		return fmt.Errorf("invalid gRPC port: %d", c.Server.GRPCPort)
	}

	if c.Server.HealthPort < 1 || c.Server.HealthPort > 65535 {
		return fmt.Errorf("invalid health port: %d", c.Server.HealthPort)
	}

	if c.Server.TLSEnabled {
		if c.Server.TLSCertFile == "" {
			return fmt.Errorf("TLS enabled but no certificate file specified")
		}
		if c.Server.TLSKeyFile == "" {
			return fmt.Errorf("TLS enabled but no key file specified")
		}
	}

	if !c.Kubernetes.InCluster && c.Kubernetes.KubeconfigPath == "" {
		return fmt.Errorf("not running in-cluster but no kubeconfig path specified")
	}

	if c.Kubernetes.QPS <= 0 {
		return fmt.Errorf("invalid QPS: %f", c.Kubernetes.QPS)
	}

	if c.Kubernetes.Burst <= 0 {
		return fmt.Errorf("invalid burst: %d", c.Kubernetes.Burst)
	}

	return nil
}

// String returns a string representation of the config (for logging)
func (c *Config) String() string {
	data, _ := yaml.Marshal(c)
	return string(data)
}
