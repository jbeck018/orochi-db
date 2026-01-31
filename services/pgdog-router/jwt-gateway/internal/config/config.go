// Package config provides configuration loading for the JWT gateway.
package config

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"gopkg.in/yaml.v3"
)

// Config holds all configuration for the JWT gateway.
type Config struct {
	// Server configuration
	Server ServerConfig `yaml:"server"`

	// JWT configuration
	JWT JWTConfig `yaml:"jwt"`

	// Backend (PgDog) configuration
	Backend BackendConfig `yaml:"backend"`

	// Session configuration
	Session SessionConfig `yaml:"session"`

	// Logging configuration
	Logging LoggingConfig `yaml:"logging"`

	// TLS configuration for SNI-based routing
	TLS TLSConfig `yaml:"tls"`

	// SNI routing configuration
	SNI SNIConfig `yaml:"sni"`

	// Scale-to-zero configuration
	ScaleToZero ScaleToZeroConfig `yaml:"scale_to_zero"`
}

// ServerConfig holds server-specific settings.
type ServerConfig struct {
	// ListenAddr is the address to listen on (e.g., ":5433").
	ListenAddr string `yaml:"listen_addr"`

	// MaxConnections limits concurrent connections.
	MaxConnections int `yaml:"max_connections"`

	// ConnectionTimeout for establishing new connections.
	ConnectionTimeout time.Duration `yaml:"connection_timeout"`

	// ReadTimeout for reading from connections.
	ReadTimeout time.Duration `yaml:"read_timeout"`

	// WriteTimeout for writing to connections.
	WriteTimeout time.Duration `yaml:"write_timeout"`

	// ShutdownTimeout for graceful shutdown.
	ShutdownTimeout time.Duration `yaml:"shutdown_timeout"`
}

// JWTConfig holds JWT validation settings.
type JWTConfig struct {
	// PublicKeyPath is the file path to the RSA public key.
	PublicKeyPath string `yaml:"public_key_path"`

	// PublicKeyURL is the URL to fetch the public key from.
	PublicKeyURL string `yaml:"public_key_url"`

	// Issuer is the expected token issuer.
	Issuer string `yaml:"issuer"`

	// Audience is the expected token audience.
	Audience string `yaml:"audience"`

	// RequireUserID requires user_id claim.
	RequireUserID bool `yaml:"require_user_id"`

	// RequireTenantID requires tenant_id claim.
	RequireTenantID bool `yaml:"require_tenant_id"`

	// CacheTTL for validated tokens.
	CacheTTL time.Duration `yaml:"cache_ttl"`

	// KeyRefreshInterval for refreshing public key from URL.
	KeyRefreshInterval time.Duration `yaml:"key_refresh_interval"`
}

// BackendConfig holds PgDog backend settings.
type BackendConfig struct {
	// Host is the PgDog host.
	Host string `yaml:"host"`

	// Port is the PgDog port.
	Port int `yaml:"port"`

	// ConnectTimeout for connecting to backend.
	ConnectTimeout time.Duration `yaml:"connect_timeout"`

	// Username to use when connecting (if not from JWT).
	Username string `yaml:"username"`

	// Password to use when connecting (if not from JWT).
	Password string `yaml:"password"`
}

// SessionConfig holds session injection settings.
type SessionConfig struct {
	// InjectClaims enables session variable injection.
	InjectClaims bool `yaml:"inject_claims"`

	// InjectionTimeout for SET commands.
	InjectionTimeout time.Duration `yaml:"injection_timeout"`

	// CustomVariables are additional variables to set.
	CustomVariables map[string]string `yaml:"custom_variables"`
}

// LoggingConfig holds logging settings.
type LoggingConfig struct {
	// Level is the log level (debug, info, warn, error).
	Level string `yaml:"level"`

	// Format is the log format (json, text).
	Format string `yaml:"format"`

	// IncludeTimestamp includes timestamps in logs.
	IncludeTimestamp bool `yaml:"include_timestamp"`
}

// TLSConfig holds TLS configuration for secure connections.
type TLSConfig struct {
	// Enabled enables TLS for client connections.
	Enabled bool `yaml:"enabled"`

	// CertFile is the path to the TLS certificate file.
	CertFile string `yaml:"cert_file"`

	// KeyFile is the path to the TLS private key file.
	KeyFile string `yaml:"key_file"`

	// CAFile is the path to the CA certificate file for client verification.
	CAFile string `yaml:"ca_file"`

	// RequireClientCert requires clients to present a certificate.
	RequireClientCert bool `yaml:"require_client_cert"`

	// MinVersion is the minimum TLS version (1.2, 1.3).
	MinVersion string `yaml:"min_version"`
}

// SNIConfig holds SNI-based routing configuration.
type SNIConfig struct {
	// Enabled enables SNI-based routing.
	Enabled bool `yaml:"enabled"`

	// DomainPattern is the pattern for parsing hostnames.
	// Default: "{cluster}.{branch}.db.orochi.cloud"
	DomainPattern string `yaml:"domain_pattern"`

	// BaseDomain is the base domain suffix (e.g., "db.orochi.cloud").
	BaseDomain string `yaml:"base_domain"`

	// ControlPlane holds settings for control plane registry lookups.
	ControlPlane ControlPlaneConfig `yaml:"control_plane"`

	// Cache holds settings for backend address caching.
	Cache CacheConfig `yaml:"cache"`

	// DefaultBackend is the fallback backend when SNI lookup fails.
	DefaultBackend string `yaml:"default_backend"`

	// AllowUnknownClusters allows connections to unknown clusters to use default backend.
	AllowUnknownClusters bool `yaml:"allow_unknown_clusters"`
}

// ControlPlaneConfig holds settings for control plane API communication.
type ControlPlaneConfig struct {
	// URL is the base URL of the control plane API.
	URL string `yaml:"url"`

	// APIKey is the authentication key for control plane API.
	APIKey string `yaml:"api_key"`

	// Timeout for control plane API requests.
	Timeout time.Duration `yaml:"timeout"`

	// RetryCount is the number of retries for failed requests.
	RetryCount int `yaml:"retry_count"`

	// RetryDelay is the delay between retries.
	RetryDelay time.Duration `yaml:"retry_delay"`
}

// CacheConfig holds settings for caching cluster backend addresses.
type CacheConfig struct {
	// TTL is the time-to-live for cached entries.
	TTL time.Duration `yaml:"ttl"`

	// NegativeTTL is the time-to-live for negative (not found) cache entries.
	NegativeTTL time.Duration `yaml:"negative_ttl"`

	// MaxSize is the maximum number of entries in the cache.
	MaxSize int `yaml:"max_size"`

	// RefreshAhead enables background refresh before TTL expiration.
	RefreshAhead bool `yaml:"refresh_ahead"`

	// RefreshAheadRatio is the ratio of TTL at which to start background refresh.
	RefreshAheadRatio float64 `yaml:"refresh_ahead_ratio"`
}

// ScaleToZeroConfig holds settings for scale-to-zero functionality.
type ScaleToZeroConfig struct {
	// Enabled enables wake-on-connect for suspended clusters.
	Enabled bool `yaml:"enabled"`

	// ControlPlaneURL is the URL of the control plane API for wake requests.
	ControlPlaneURL string `yaml:"control_plane_url"`

	// ServiceAccountToken is the token used to authenticate with the control plane.
	ServiceAccountToken string `yaml:"service_account_token"`

	// WakeTimeout is the maximum time to wait for a cluster to wake.
	WakeTimeout time.Duration `yaml:"wake_timeout"`

	// PollInterval is how often to poll for cluster readiness during wake.
	PollInterval time.Duration `yaml:"poll_interval"`

	// MaxQueuedConnections is the maximum number of connections to queue during wake.
	MaxQueuedConnections int `yaml:"max_queued_connections"`

	// ConnectionQueueTimeout is how long a connection can wait in the queue.
	ConnectionQueueTimeout time.Duration `yaml:"connection_queue_timeout"`
}

// DefaultConfig returns configuration with default values.
func DefaultConfig() *Config {
	return &Config{
		Server: ServerConfig{
			ListenAddr:        ":5433",
			MaxConnections:    1000,
			ConnectionTimeout: 30 * time.Second,
			ReadTimeout:       5 * time.Minute,
			WriteTimeout:      5 * time.Minute,
			ShutdownTimeout:   30 * time.Second,
		},
		JWT: JWTConfig{
			CacheTTL:           5 * time.Minute,
			KeyRefreshInterval: 1 * time.Hour,
		},
		Backend: BackendConfig{
			Host:           "localhost",
			Port:           5432,
			ConnectTimeout: 10 * time.Second,
		},
		Session: SessionConfig{
			InjectClaims:     true,
			InjectionTimeout: 5 * time.Second,
		},
		Logging: LoggingConfig{
			Level:            "info",
			Format:           "text",
			IncludeTimestamp: true,
		},
		TLS: TLSConfig{
			Enabled:    false,
			MinVersion: "1.2",
		},
		SNI: SNIConfig{
			Enabled:              false,
			DomainPattern:        "{cluster}.{branch}.db.orochi.cloud",
			BaseDomain:           "db.orochi.cloud",
			AllowUnknownClusters: false,
			ControlPlane: ControlPlaneConfig{
				Timeout:    10 * time.Second,
				RetryCount: 3,
				RetryDelay: 1 * time.Second,
			},
			Cache: CacheConfig{
				TTL:               5 * time.Minute,
				NegativeTTL:       30 * time.Second,
				MaxSize:           10000,
				RefreshAhead:      true,
				RefreshAheadRatio: 0.75,
			},
		},
		ScaleToZero: ScaleToZeroConfig{
			Enabled:                false,
			WakeTimeout:            60 * time.Second,
			PollInterval:           500 * time.Millisecond,
			MaxQueuedConnections:   100,
			ConnectionQueueTimeout: 65 * time.Second,
		},
	}
}

// LoadFromFile loads configuration from a YAML file.
func LoadFromFile(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read config file: %w", err)
	}

	config := DefaultConfig()
	if err := yaml.Unmarshal(data, config); err != nil {
		return nil, fmt.Errorf("failed to parse config file: %w", err)
	}

	return config, nil
}

// LoadFromEnv loads configuration from environment variables.
// Environment variables override file configuration.
func LoadFromEnv(config *Config) {
	// Server settings
	if v := os.Getenv("JWT_GATEWAY_LISTEN_ADDR"); v != "" {
		config.Server.ListenAddr = v
	}
	if v := os.Getenv("JWT_GATEWAY_MAX_CONNECTIONS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			config.Server.MaxConnections = n
		}
	}
	if v := os.Getenv("JWT_GATEWAY_CONNECTION_TIMEOUT"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.Server.ConnectionTimeout = d
		}
	}

	// JWT settings
	if v := os.Getenv("JWT_GATEWAY_PUBLIC_KEY_PATH"); v != "" {
		config.JWT.PublicKeyPath = v
	}
	if v := os.Getenv("JWT_GATEWAY_PUBLIC_KEY_URL"); v != "" {
		config.JWT.PublicKeyURL = v
	}
	if v := os.Getenv("JWT_GATEWAY_ISSUER"); v != "" {
		config.JWT.Issuer = v
	}
	if v := os.Getenv("JWT_GATEWAY_AUDIENCE"); v != "" {
		config.JWT.Audience = v
	}
	if v := os.Getenv("JWT_GATEWAY_REQUIRE_USER_ID"); v == "true" {
		config.JWT.RequireUserID = true
	}
	if v := os.Getenv("JWT_GATEWAY_REQUIRE_TENANT_ID"); v == "true" {
		config.JWT.RequireTenantID = true
	}
	if v := os.Getenv("JWT_GATEWAY_CACHE_TTL"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.JWT.CacheTTL = d
		}
	}

	// Backend settings
	if v := os.Getenv("JWT_GATEWAY_BACKEND_HOST"); v != "" {
		config.Backend.Host = v
	}
	if v := os.Getenv("JWT_GATEWAY_BACKEND_PORT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			config.Backend.Port = n
		}
	}
	if v := os.Getenv("JWT_GATEWAY_BACKEND_USERNAME"); v != "" {
		config.Backend.Username = v
	}
	if v := os.Getenv("JWT_GATEWAY_BACKEND_PASSWORD"); v != "" {
		config.Backend.Password = v
	}

	// Session settings
	if v := os.Getenv("JWT_GATEWAY_INJECT_CLAIMS"); v == "false" {
		config.Session.InjectClaims = false
	}

	// Logging settings
	if v := os.Getenv("JWT_GATEWAY_LOG_LEVEL"); v != "" {
		config.Logging.Level = v
	}
	if v := os.Getenv("JWT_GATEWAY_LOG_FORMAT"); v != "" {
		config.Logging.Format = v
	}

	// TLS settings
	if v := os.Getenv("JWT_GATEWAY_TLS_ENABLED"); v == "true" {
		config.TLS.Enabled = true
	}
	if v := os.Getenv("JWT_GATEWAY_TLS_CERT_FILE"); v != "" {
		config.TLS.CertFile = v
	}
	if v := os.Getenv("JWT_GATEWAY_TLS_KEY_FILE"); v != "" {
		config.TLS.KeyFile = v
	}
	if v := os.Getenv("JWT_GATEWAY_TLS_CA_FILE"); v != "" {
		config.TLS.CAFile = v
	}
	if v := os.Getenv("JWT_GATEWAY_TLS_MIN_VERSION"); v != "" {
		config.TLS.MinVersion = v
	}

	// SNI routing settings
	if v := os.Getenv("JWT_GATEWAY_SNI_ENABLED"); v == "true" {
		config.SNI.Enabled = true
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_DOMAIN_PATTERN"); v != "" {
		config.SNI.DomainPattern = v
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_BASE_DOMAIN"); v != "" {
		config.SNI.BaseDomain = v
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_DEFAULT_BACKEND"); v != "" {
		config.SNI.DefaultBackend = v
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_ALLOW_UNKNOWN"); v == "true" {
		config.SNI.AllowUnknownClusters = true
	}

	// SNI Control Plane settings
	if v := os.Getenv("JWT_GATEWAY_CONTROL_PLANE_URL"); v != "" {
		config.SNI.ControlPlane.URL = v
	}
	if v := os.Getenv("JWT_GATEWAY_CONTROL_PLANE_API_KEY"); v != "" {
		config.SNI.ControlPlane.APIKey = v
	}
	if v := os.Getenv("JWT_GATEWAY_CONTROL_PLANE_TIMEOUT"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.SNI.ControlPlane.Timeout = d
		}
	}

	// SNI Cache settings
	if v := os.Getenv("JWT_GATEWAY_SNI_CACHE_TTL"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.SNI.Cache.TTL = d
		}
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_CACHE_NEGATIVE_TTL"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.SNI.Cache.NegativeTTL = d
		}
	}
	if v := os.Getenv("JWT_GATEWAY_SNI_CACHE_MAX_SIZE"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			config.SNI.Cache.MaxSize = n
		}
	}

	// Scale-to-zero settings
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_ENABLED"); v == "true" {
		config.ScaleToZero.Enabled = true
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_CONTROL_PLANE_URL"); v != "" {
		config.ScaleToZero.ControlPlaneURL = v
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_SERVICE_ACCOUNT_TOKEN"); v != "" {
		config.ScaleToZero.ServiceAccountToken = v
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_WAKE_TIMEOUT"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.ScaleToZero.WakeTimeout = d
		}
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_POLL_INTERVAL"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.ScaleToZero.PollInterval = d
		}
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_MAX_QUEUED_CONNECTIONS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			config.ScaleToZero.MaxQueuedConnections = n
		}
	}
	if v := os.Getenv("JWT_GATEWAY_SCALE_TO_ZERO_CONNECTION_QUEUE_TIMEOUT"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			config.ScaleToZero.ConnectionQueueTimeout = d
		}
	}
}

// Validate checks the configuration for errors.
func (c *Config) Validate() error {
	if c.Server.ListenAddr == "" {
		return fmt.Errorf("server.listen_addr is required")
	}

	if c.JWT.PublicKeyPath == "" && c.JWT.PublicKeyURL == "" {
		return fmt.Errorf("jwt.public_key_path or jwt.public_key_url is required")
	}

	// Backend is required when SNI routing is disabled
	if !c.SNI.Enabled {
		if c.Backend.Host == "" {
			return fmt.Errorf("backend.host is required when SNI routing is disabled")
		}
		if c.Backend.Port <= 0 || c.Backend.Port > 65535 {
			return fmt.Errorf("backend.port must be between 1 and 65535")
		}
	}

	// TLS validation
	if c.TLS.Enabled {
		if c.TLS.CertFile == "" {
			return fmt.Errorf("tls.cert_file is required when TLS is enabled")
		}
		if c.TLS.KeyFile == "" {
			return fmt.Errorf("tls.key_file is required when TLS is enabled")
		}
	}

	// SNI routing requires TLS
	if c.SNI.Enabled {
		if !c.TLS.Enabled {
			return fmt.Errorf("tls must be enabled for SNI routing")
		}
		if c.SNI.BaseDomain == "" {
			return fmt.Errorf("sni.base_domain is required for SNI routing")
		}
		if c.SNI.ControlPlane.URL == "" {
			return fmt.Errorf("sni.control_plane.url is required for SNI routing")
		}
	}

	return nil
}

// BackendAddress returns the backend address in host:port format.
func (c *Config) BackendAddress() string {
	return fmt.Sprintf("%s:%d", c.Backend.Host, c.Backend.Port)
}
