// Package config provides configuration management for the control plane.
package config

import (
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config holds all configuration for the control plane.
type Config struct {
	Server      ServerConfig
	Database    DatabaseConfig
	JWT         JWTConfig
	GRPC        GRPCConfig
	Provisioner ProvisionerConfig
}

// ProvisionerConfig holds configuration for the provisioner gRPC client.
type ProvisionerConfig struct {
	Addr           string        // gRPC address (e.g., "provisioner:8080" or "localhost:8080")
	Enabled        bool          // Whether real provisioning is enabled
	Timeout        time.Duration // Request timeout
	MaxRetries     int           // Maximum retry attempts
	TLSEnabled     bool          // Whether to use TLS
	Insecure       bool          // Skip TLS verification (for development)
	DefaultTimeout time.Duration // Default timeout for provisioning operations
}

// ServerConfig holds HTTP server configuration.
type ServerConfig struct {
	Host         string
	Port         int
	ReadTimeout  time.Duration
	WriteTimeout time.Duration
	IdleTimeout  time.Duration
}

// DatabaseConfig holds PostgreSQL configuration.
type DatabaseConfig struct {
	Host            string
	Port            int
	User            string
	Password        string
	Database        string
	SSLMode         string
	MaxConns        int32
	MinConns        int32
	MaxConnLifetime time.Duration
	MaxConnIdleTime time.Duration
}

// JWTConfig holds JWT authentication configuration.
type JWTConfig struct {
	Secret          string
	Issuer          string
	AccessTokenTTL  time.Duration
	RefreshTokenTTL time.Duration
	ClockSkewLeeway time.Duration // Tolerance for clock drift between servers
}

// GRPCConfig holds gRPC server configuration.
type GRPCConfig struct {
	Host string
	Port int
}

// Load loads configuration from environment variables with defaults.
func Load() *Config {
	// Load database config, supporting DATABASE_URL (Fly.io standard)
	dbConfig := loadDatabaseConfig()

	// Check if provisioner is enabled (non-empty address)
	provisionerAddr := getEnv("PROVISIONER_GRPC_ADDR", "")
	provisionerEnabled := provisionerAddr != ""

	return &Config{
		Server: ServerConfig{
			Host:         getEnv("SERVER_HOST", "0.0.0.0"),
			Port:         getEnvAsInt("SERVER_PORT", 8080),
			ReadTimeout:  getEnvAsDuration("SERVER_READ_TIMEOUT", 15*time.Second),
			WriteTimeout: getEnvAsDuration("SERVER_WRITE_TIMEOUT", 15*time.Second),
			IdleTimeout:  getEnvAsDuration("SERVER_IDLE_TIMEOUT", 60*time.Second),
		},
		Database: dbConfig,
		JWT: JWTConfig{
			Secret:          getEnv("JWT_SECRET", "change-me-in-production-this-is-not-secure"),
			Issuer:          getEnv("JWT_ISSUER", "orochi-cloud"),
			AccessTokenTTL:  getEnvAsDuration("JWT_ACCESS_TOKEN_TTL", 15*time.Minute),
			RefreshTokenTTL: getEnvAsDuration("JWT_REFRESH_TOKEN_TTL", 7*24*time.Hour),
			ClockSkewLeeway: getEnvAsDuration("JWT_CLOCK_SKEW_LEEWAY", 30*time.Second),
		},
		GRPC: GRPCConfig{
			Host: getEnv("GRPC_HOST", "0.0.0.0"),
			Port: getEnvAsInt("GRPC_PORT", 9090),
		},
		Provisioner: ProvisionerConfig{
			Addr:           provisionerAddr,
			Enabled:        provisionerEnabled,
			Timeout:        getEnvAsDuration("PROVISIONER_TIMEOUT", 30*time.Second),
			MaxRetries:     getEnvAsInt("PROVISIONER_MAX_RETRIES", 3),
			TLSEnabled:     getEnvAsBool("PROVISIONER_TLS_ENABLED", false),
			Insecure:       getEnvAsBool("PROVISIONER_TLS_INSECURE", true),
			DefaultTimeout: getEnvAsDuration("PROVISIONER_DEFAULT_TIMEOUT", 10*time.Minute),
		},
	}
}

// loadDatabaseConfig loads database configuration from DATABASE_URL or individual env vars.
func loadDatabaseConfig() DatabaseConfig {
	// Check for DATABASE_URL first (Fly.io and other PaaS platforms)
	if databaseURL := os.Getenv("DATABASE_URL"); databaseURL != "" {
		return parseDatabaseURL(databaseURL)
	}

	// Fall back to individual environment variables
	return DatabaseConfig{
		Host:            getEnv("DB_HOST", "localhost"),
		Port:            getEnvAsInt("DB_PORT", 5432),
		User:            getEnv("DB_USER", "orochi"),
		Password:        getEnv("DB_PASSWORD", "orochi"),
		Database:        getEnv("DB_NAME", "orochi_cloud"),
		SSLMode:         getEnv("DB_SSL_MODE", "disable"),
		MaxConns:        int32(getEnvAsInt("DB_MAX_CONNS", 25)),
		MinConns:        int32(getEnvAsInt("DB_MIN_CONNS", 5)),
		MaxConnLifetime: getEnvAsDuration("DB_MAX_CONN_LIFETIME", 1*time.Hour),
		MaxConnIdleTime: getEnvAsDuration("DB_MAX_CONN_IDLE_TIME", 30*time.Minute),
	}
}

// parseDatabaseURL parses a DATABASE_URL into a DatabaseConfig.
// Format: postgres://user:password@host:port/database?sslmode=value
func parseDatabaseURL(databaseURL string) DatabaseConfig {
	config := DatabaseConfig{
		Host:            "localhost",
		Port:            5432,
		User:            "postgres",
		Password:        "",
		Database:        "postgres",
		SSLMode:         getEnv("DB_SSL_MODE", "require"), // Default to require for production
		MaxConns:        int32(getEnvAsInt("DB_MAX_CONNS", 25)),
		MinConns:        int32(getEnvAsInt("DB_MIN_CONNS", 5)),
		MaxConnLifetime: getEnvAsDuration("DB_MAX_CONN_LIFETIME", 1*time.Hour),
		MaxConnIdleTime: getEnvAsDuration("DB_MAX_CONN_IDLE_TIME", 30*time.Minute),
	}

	u, err := url.Parse(databaseURL)
	if err != nil {
		return config
	}

	if u.Host != "" {
		host := u.Hostname()
		if host != "" {
			config.Host = host
		}
		portStr := u.Port()
		if portStr != "" {
			if port, err := strconv.Atoi(portStr); err == nil {
				config.Port = port
			}
		}
	}

	if u.User != nil {
		config.User = u.User.Username()
		if password, ok := u.User.Password(); ok {
			config.Password = password
		}
	}

	if u.Path != "" {
		config.Database = strings.TrimPrefix(u.Path, "/")
	}

	// Parse query parameters for sslmode
	query := u.Query()
	if sslmode := query.Get("sslmode"); sslmode != "" {
		config.SSLMode = sslmode
	}

	return config
}

// DSN returns the PostgreSQL connection string.
func (c *DatabaseConfig) DSN() string {
	return "postgres://" + c.User + ":" + c.Password + "@" + c.Host + ":" +
		strconv.Itoa(c.Port) + "/" + c.Database + "?sslmode=" + c.SSLMode
}

// getEnv returns the value of an environment variable or a default value.
func getEnv(key, defaultValue string) string {
	if value, exists := os.LookupEnv(key); exists {
		return value
	}
	return defaultValue
}

// getEnvAsInt returns the value of an environment variable as an integer or a default value.
func getEnvAsInt(key string, defaultValue int) int {
	if value, exists := os.LookupEnv(key); exists {
		if intValue, err := strconv.Atoi(value); err == nil {
			return intValue
		}
	}
	return defaultValue
}

// getEnvAsDuration returns the value of an environment variable as a duration or a default value.
func getEnvAsDuration(key string, defaultValue time.Duration) time.Duration {
	if value, exists := os.LookupEnv(key); exists {
		if duration, err := time.ParseDuration(value); err == nil {
			return duration
		}
	}
	return defaultValue
}

// getEnvAsBool returns the value of an environment variable as a boolean or a default value.
func getEnvAsBool(key string, defaultValue bool) bool {
	if value, exists := os.LookupEnv(key); exists {
		switch strings.ToLower(value) {
		case "true", "1", "yes", "on":
			return true
		case "false", "0", "no", "off":
			return false
		}
	}
	return defaultValue
}
