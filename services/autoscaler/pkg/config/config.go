// Package config provides configuration management for the Orochi autoscaler.
package config

import (
	"fmt"
	"os"
	"time"

	"gopkg.in/yaml.v3"
)

// Config holds the complete autoscaler configuration.
type Config struct {
	// Server configuration
	Server ServerConfig `yaml:"server"`

	// Kubernetes configuration
	Kubernetes KubernetesConfig `yaml:"kubernetes"`

	// Prometheus configuration
	Prometheus PrometheusConfig `yaml:"prometheus"`

	// Scaling defaults
	Scaling ScalingDefaults `yaml:"scaling"`

	// Logging configuration
	Logging LoggingConfig `yaml:"logging"`
}

// ServerConfig holds gRPC and HTTP server configuration.
type ServerConfig struct {
	// GRPCPort is the port for gRPC server
	GRPCPort int `yaml:"grpcPort"`

	// MetricsPort is the port for Prometheus metrics endpoint
	MetricsPort int `yaml:"metricsPort"`

	// HealthPort is the port for health checks
	HealthPort int `yaml:"healthPort"`

	// ShutdownTimeout is the graceful shutdown timeout
	ShutdownTimeout time.Duration `yaml:"shutdownTimeout"`
}

// KubernetesConfig holds Kubernetes client configuration.
type KubernetesConfig struct {
	// InCluster indicates whether running inside Kubernetes
	InCluster bool `yaml:"inCluster"`

	// Kubeconfig path (used when InCluster is false)
	Kubeconfig string `yaml:"kubeconfig"`

	// Namespace to watch (empty means all namespaces)
	Namespace string `yaml:"namespace"`

	// ResyncPeriod for informers
	ResyncPeriod time.Duration `yaml:"resyncPeriod"`

	// QPS for Kubernetes API client
	QPS float32 `yaml:"qps"`

	// Burst for Kubernetes API client
	Burst int `yaml:"burst"`

	// LabelSelector for filtering resources
	LabelSelector string `yaml:"labelSelector"`
}

// PrometheusConfig holds Prometheus client configuration.
type PrometheusConfig struct {
	// Address of Prometheus server
	Address string `yaml:"address"`

	// Timeout for queries
	Timeout time.Duration `yaml:"timeout"`

	// MetricPrefix for custom metrics
	MetricPrefix string `yaml:"metricPrefix"`

	// ScrapeInterval for metrics collection
	ScrapeInterval time.Duration `yaml:"scrapeInterval"`
}

// ScalingDefaults holds default scaling parameters.
type ScalingDefaults struct {
	// Horizontal scaling defaults
	Horizontal HorizontalDefaults `yaml:"horizontal"`

	// Vertical scaling defaults
	Vertical VerticalDefaults `yaml:"vertical"`

	// General defaults
	EvaluationInterval   time.Duration `yaml:"evaluationInterval"`
	MetricHistoryWindow  time.Duration `yaml:"metricHistoryWindow"`
	StabilizationWindow  time.Duration `yaml:"stabilizationWindow"`
	MaxConcurrentScaling int           `yaml:"maxConcurrentScaling"`
}

// HorizontalDefaults holds default horizontal scaling parameters.
type HorizontalDefaults struct {
	MinReplicas             int32         `yaml:"minReplicas"`
	MaxReplicas             int32         `yaml:"maxReplicas"`
	TargetCPUUtilization    float64       `yaml:"targetCPUUtilization"`
	TargetMemoryUtilization float64       `yaml:"targetMemoryUtilization"`
	TargetConnectionsPerPod int64         `yaml:"targetConnectionsPerPod"`
	TargetQueryLatencyMs    float64       `yaml:"targetQueryLatencyMs"`
	ScaleUpCooldown         time.Duration `yaml:"scaleUpCooldown"`
	ScaleDownCooldown       time.Duration `yaml:"scaleDownCooldown"`
	ScaleUpStep             int32         `yaml:"scaleUpStep"`
	ScaleDownStep           int32         `yaml:"scaleDownStep"`
	ScaleUpThreshold        float64       `yaml:"scaleUpThreshold"`
	ScaleDownThreshold      float64       `yaml:"scaleDownThreshold"`
}

// VerticalDefaults holds default vertical scaling parameters.
type VerticalDefaults struct {
	MinCPU                  string        `yaml:"minCPU"`
	MaxCPU                  string        `yaml:"maxCPU"`
	MinMemory               string        `yaml:"minMemory"`
	MaxMemory               string        `yaml:"maxMemory"`
	TargetCPUUtilization    float64       `yaml:"targetCPUUtilization"`
	TargetMemoryUtilization float64       `yaml:"targetMemoryUtilization"`
	Cooldown                time.Duration `yaml:"cooldown"`
	CPUIncrement            string        `yaml:"cpuIncrement"`
	MemoryIncrement         string        `yaml:"memoryIncrement"`
}

// LoggingConfig holds logging configuration.
type LoggingConfig struct {
	// Level is the logging level (debug, info, warn, error)
	Level string `yaml:"level"`

	// Format is the logging format (json, text)
	Format string `yaml:"format"`

	// Output is where to write logs (stdout, stderr, file path)
	Output string `yaml:"output"`
}

// DefaultConfig returns a configuration with sensible defaults.
func DefaultConfig() *Config {
	return &Config{
		Server: ServerConfig{
			GRPCPort:        50051,
			MetricsPort:     9090,
			HealthPort:      8080,
			ShutdownTimeout: 30 * time.Second,
		},
		Kubernetes: KubernetesConfig{
			InCluster:    true,
			ResyncPeriod: 30 * time.Second,
			QPS:          50,
			Burst:        100,
		},
		Prometheus: PrometheusConfig{
			Address:        "http://prometheus:9090",
			Timeout:        30 * time.Second,
			MetricPrefix:   "orochi_",
			ScrapeInterval: 15 * time.Second,
		},
		Scaling: ScalingDefaults{
			EvaluationInterval:   15 * time.Second,
			MetricHistoryWindow:  5 * time.Minute,
			StabilizationWindow:  3 * time.Minute,
			MaxConcurrentScaling: 5,
			Horizontal: HorizontalDefaults{
				MinReplicas:             1,
				MaxReplicas:             10,
				TargetCPUUtilization:    70.0,
				TargetMemoryUtilization: 80.0,
				TargetConnectionsPerPod: 100,
				TargetQueryLatencyMs:    100.0,
				ScaleUpCooldown:         60 * time.Second,
				ScaleDownCooldown:       300 * time.Second,
				ScaleUpStep:             2,
				ScaleDownStep:           1,
				ScaleUpThreshold:        1.1,
				ScaleDownThreshold:      0.9,
			},
			Vertical: VerticalDefaults{
				MinCPU:                  "250m",
				MaxCPU:                  "8",
				MinMemory:               "512Mi",
				MaxMemory:               "32Gi",
				TargetCPUUtilization:    70.0,
				TargetMemoryUtilization: 80.0,
				Cooldown:                600 * time.Second,
				CPUIncrement:            "250m",
				MemoryIncrement:         "256Mi",
			},
		},
		Logging: LoggingConfig{
			Level:  "info",
			Format: "json",
			Output: "stdout",
		},
	}
}

// LoadConfig loads configuration from a YAML file.
func LoadConfig(path string) (*Config, error) {
	config := DefaultConfig()

	if path == "" {
		return config, nil
	}

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return config, nil
		}
		return nil, fmt.Errorf("failed to read config file: %w", err)
	}

	if err := yaml.Unmarshal(data, config); err != nil {
		return nil, fmt.Errorf("failed to parse config file: %w", err)
	}

	return config, nil
}

// LoadConfigFromEnv loads configuration from environment variables.
func LoadConfigFromEnv(config *Config) {
	if v := os.Getenv("AUTOSCALER_GRPC_PORT"); v != "" {
		fmt.Sscanf(v, "%d", &config.Server.GRPCPort)
	}
	if v := os.Getenv("AUTOSCALER_METRICS_PORT"); v != "" {
		fmt.Sscanf(v, "%d", &config.Server.MetricsPort)
	}
	if v := os.Getenv("AUTOSCALER_HEALTH_PORT"); v != "" {
		fmt.Sscanf(v, "%d", &config.Server.HealthPort)
	}

	if v := os.Getenv("KUBERNETES_IN_CLUSTER"); v == "false" {
		config.Kubernetes.InCluster = false
	}
	if v := os.Getenv("KUBECONFIG"); v != "" {
		config.Kubernetes.Kubeconfig = v
	}
	if v := os.Getenv("KUBERNETES_NAMESPACE"); v != "" {
		config.Kubernetes.Namespace = v
	}

	if v := os.Getenv("PROMETHEUS_ADDRESS"); v != "" {
		config.Prometheus.Address = v
	}

	if v := os.Getenv("LOG_LEVEL"); v != "" {
		config.Logging.Level = v
	}
	if v := os.Getenv("LOG_FORMAT"); v != "" {
		config.Logging.Format = v
	}
}

// Validate validates the configuration.
func (c *Config) Validate() error {
	if c.Server.GRPCPort <= 0 || c.Server.GRPCPort > 65535 {
		return fmt.Errorf("invalid gRPC port: %d", c.Server.GRPCPort)
	}
	if c.Server.MetricsPort <= 0 || c.Server.MetricsPort > 65535 {
		return fmt.Errorf("invalid metrics port: %d", c.Server.MetricsPort)
	}
	if c.Prometheus.Address == "" {
		return fmt.Errorf("prometheus address is required")
	}
	if c.Scaling.Horizontal.MinReplicas < 0 {
		return fmt.Errorf("min replicas cannot be negative")
	}
	if c.Scaling.Horizontal.MaxReplicas < c.Scaling.Horizontal.MinReplicas {
		return fmt.Errorf("max replicas must be >= min replicas")
	}
	if c.Scaling.Horizontal.TargetCPUUtilization <= 0 || c.Scaling.Horizontal.TargetCPUUtilization > 100 {
		return fmt.Errorf("invalid target CPU utilization: %f", c.Scaling.Horizontal.TargetCPUUtilization)
	}
	return nil
}
