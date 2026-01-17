// Package config provides configuration management for the Orochi Cloud CLI.
package config

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/viper"
)

const (
	// DefaultAPIEndpoint is the default Orochi Cloud API endpoint.
	DefaultAPIEndpoint = "https://api.orochi.cloud"

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
	var cfg Config
	if err := viper.Unmarshal(&cfg); err != nil {
		return nil, fmt.Errorf("failed to unmarshal config: %w", err)
	}
	return &cfg, nil
}

// Save writes the current configuration to disk.
func Save() error {
	path, err := configPath()
	if err != nil {
		return err
	}

	if err := viper.WriteConfigAs(path); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}

	// Set restrictive permissions on config file (contains tokens)
	if err := os.Chmod(path, 0600); err != nil {
		return fmt.Errorf("failed to set config file permissions: %w", err)
	}

	return nil
}

// Set sets a configuration value.
func Set(key, value string) error {
	viper.Set(key, value)
	return Save()
}

// Get returns a configuration value.
func Get(key string) string {
	return viper.GetString(key)
}

// GetAll returns all configuration keys and values.
func GetAll() map[string]interface{} {
	return viper.AllSettings()
}

// IsAuthenticated returns true if the user has valid credentials stored.
func IsAuthenticated() bool {
	token := viper.GetString("access_token")
	return token != ""
}

// ClearCredentials removes authentication credentials from config.
func ClearCredentials() error {
	viper.Set("access_token", "")
	viper.Set("refresh_token", "")
	viper.Set("user_email", "")
	viper.Set("user_id", "")
	return Save()
}

// SetCredentials stores authentication credentials.
func SetCredentials(accessToken, refreshToken, email, userID string) error {
	viper.Set("access_token", accessToken)
	viper.Set("refresh_token", refreshToken)
	viper.Set("user_email", email)
	viper.Set("user_id", userID)
	return Save()
}

// GetAccessToken returns the current access token.
func GetAccessToken() string {
	return viper.GetString("access_token")
}

// GetAPIEndpoint returns the API endpoint.
func GetAPIEndpoint() string {
	endpoint := viper.GetString("api_endpoint")
	if endpoint == "" {
		return DefaultAPIEndpoint
	}
	return endpoint
}

// GetOutputFormat returns the configured output format.
func GetOutputFormat() string {
	format := viper.GetString("output_format")
	if format == "" {
		return "table"
	}
	return format
}
