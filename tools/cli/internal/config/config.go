// Package config provides configuration management for the HowlerOps CLI (OrochiDB).
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"

	"github.com/spf13/viper"
	"gopkg.in/yaml.v3"
)

// configMu protects all viper operations which are not thread-safe.
var configMu sync.RWMutex

const (
	// DefaultAPIEndpoint is the default HowlerOps API endpoint.
	DefaultAPIEndpoint = "https://api.howlerops.com"

	// ConfigFileName is the name of the configuration file.
	ConfigFileName = "config"

	// ConfigFileType is the type of the configuration file.
	ConfigFileType = "yaml"
)

// Config holds the CLI configuration.
type Config struct {
	APIEndpoint  string `mapstructure:"api_endpoint"`
	AccessToken  string `mapstructure:"access_token"`
	RefreshToken string `mapstructure:"refresh_token"`
	OutputFormat string `mapstructure:"output_format"`
	UserEmail    string `mapstructure:"user_email"`
	UserID       string `mapstructure:"user_id"`
}

// configDir returns the configuration directory path.
func configDir() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("failed to get home directory: %w", err)
	}
	return filepath.Join(home, ".orochi"), nil
}

// configPath returns the full path to the configuration file.
func configPath() (string, error) {
	dir, err := configDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, ConfigFileName+"."+ConfigFileType), nil
}

// Init initializes the configuration system.
func Init() error {
	dir, err := configDir()
	if err != nil {
		return err
	}

	// Create config directory if it doesn't exist
	if err := os.MkdirAll(dir, 0700); err != nil {
		return fmt.Errorf("failed to create config directory: %w", err)
	}

	configMu.Lock()
	defer configMu.Unlock()

	viper.SetConfigName(ConfigFileName)
	viper.SetConfigType(ConfigFileType)
	viper.AddConfigPath(dir)

	// Set defaults
	viper.SetDefault("api_endpoint", DefaultAPIEndpoint)
	viper.SetDefault("output_format", "table")

	// Environment variable support
	viper.SetEnvPrefix("OROCHI")
	viper.AutomaticEnv()

	// Read config file if it exists
	if err := viper.ReadInConfig(); err != nil {
		if _, ok := err.(viper.ConfigFileNotFoundError); !ok {
			return fmt.Errorf("failed to read config file: %w", err)
		}
	}

	return nil
}

// Load returns the current configuration.
func Load() (*Config, error) {
	configMu.RLock()
	defer configMu.RUnlock()

	var cfg Config
	if err := viper.Unmarshal(&cfg); err != nil {
		return nil, fmt.Errorf("failed to unmarshal config: %w", err)
	}
	return &cfg, nil
}

// Save writes the current configuration to disk.
// This function is thread-safe and uses atomic file creation with proper permissions
// to avoid TOCTOU vulnerabilities.
func Save() error {
	path, err := configPath()
	if err != nil {
		return err
	}

	configMu.Lock()
	defer configMu.Unlock()

	return saveConfigLocked(path)
}

// saveConfigLocked writes the config to disk. Caller must hold configMu.Lock().
// Uses os.OpenFile with explicit mode to atomically create the file with
// restrictive permissions, avoiding TOCTOU race conditions.
func saveConfigLocked(path string) error {
	// Get all settings from viper while we hold the lock
	settings := viper.AllSettings()

	// Marshal settings to YAML
	data, err := yaml.Marshal(settings)
	if err != nil {
		return fmt.Errorf("failed to marshal config: %w", err)
	}

	// Create file atomically with restrictive permissions (0600)
	// This avoids the TOCTOU vulnerability of WriteConfigAs + Chmod
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return fmt.Errorf("failed to create config file: %w", err)
	}
	defer file.Close()

	if _, err := file.Write(data); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}

	return nil
}

// Set sets a configuration value.
func Set(key, value string) error {
	path, err := configPath()
	if err != nil {
		return err
	}

	configMu.Lock()
	defer configMu.Unlock()

	viper.Set(key, value)
	return saveConfigLocked(path)
}

// Get returns a configuration value.
func Get(key string) string {
	configMu.RLock()
	defer configMu.RUnlock()

	return viper.GetString(key)
}

// GetAll returns all configuration keys and values.
func GetAll() map[string]interface{} {
	configMu.RLock()
	defer configMu.RUnlock()

	return viper.AllSettings()
}

// IsAuthenticated returns true if the user has valid credentials stored.
func IsAuthenticated() bool {
	configMu.RLock()
	defer configMu.RUnlock()

	token := viper.GetString("access_token")
	return token != ""
}

// ClearCredentials removes authentication credentials from config.
func ClearCredentials() error {
	path, err := configPath()
	if err != nil {
		return err
	}

	configMu.Lock()
	defer configMu.Unlock()

	viper.Set("access_token", "")
	viper.Set("refresh_token", "")
	viper.Set("user_email", "")
	viper.Set("user_id", "")
	return saveConfigLocked(path)
}

// SetCredentials stores authentication credentials.
func SetCredentials(accessToken, refreshToken, email, userID string) error {
	path, err := configPath()
	if err != nil {
		return err
	}

	configMu.Lock()
	defer configMu.Unlock()

	viper.Set("access_token", accessToken)
	viper.Set("refresh_token", refreshToken)
	viper.Set("user_email", email)
	viper.Set("user_id", userID)
	return saveConfigLocked(path)
}

// GetAccessToken returns the current access token.
func GetAccessToken() string {
	configMu.RLock()
	defer configMu.RUnlock()

	return viper.GetString("access_token")
}

// GetAPIEndpoint returns the API endpoint.
func GetAPIEndpoint() string {
	configMu.RLock()
	defer configMu.RUnlock()

	endpoint := viper.GetString("api_endpoint")
	if endpoint == "" {
		return DefaultAPIEndpoint
	}
	return endpoint
}

// GetOutputFormat returns the configured output format.
func GetOutputFormat() string {
	configMu.RLock()
	defer configMu.RUnlock()

	format := viper.GetString("output_format")
	if format == "" {
		return "table"
	}
	return format
}

// GetOrganization returns the current organization slug.
func GetOrganization() string {
	configMu.RLock()
	defer configMu.RUnlock()

	return viper.GetString("organization")
}

// SetOrganization sets the current organization slug.
func SetOrganization(slug string) error {
	path, err := configPath()
	if err != nil {
		return err
	}

	configMu.Lock()
	defer configMu.Unlock()

	viper.Set("organization", slug)
	return saveConfigLocked(path)
}
