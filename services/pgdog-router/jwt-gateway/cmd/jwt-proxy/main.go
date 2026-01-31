// Package main provides the entry point for the JWT validation gateway.
package main

import (
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/auth"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/config"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/proxy"
)

var (
	version   = "1.0.0"
	buildTime = "unknown"
	gitCommit = "unknown"
)

func main() {
	// Parse command line flags
	configFile := flag.String("config", "", "Path to configuration file")
	showVersion := flag.Bool("version", false, "Show version information")
	flag.Parse()

	if *showVersion {
		fmt.Printf("pgdog-jwt-gateway version %s\n", version)
		fmt.Printf("  Build time: %s\n", buildTime)
		fmt.Printf("  Git commit: %s\n", gitCommit)
		os.Exit(0)
	}

	// Load configuration
	cfg, err := loadConfig(*configFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load configuration: %v\n", err)
		os.Exit(1)
	}

	// Validate configuration
	if err := cfg.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "Invalid configuration: %v\n", err)
		os.Exit(1)
	}

	// Set up logging
	logger := setupLogger(cfg.Logging)
	logger.Info("Starting PgDog JWT Gateway",
		"version", version,
		"listen_addr", cfg.Server.ListenAddr,
		"backend", cfg.BackendAddress())

	// Initialize JWT validator
	jwtValidator, err := auth.NewValidator(auth.ValidatorConfig{
		PublicKeyPath:      cfg.JWT.PublicKeyPath,
		PublicKeyURL:       cfg.JWT.PublicKeyURL,
		Issuer:             cfg.JWT.Issuer,
		Audience:           cfg.JWT.Audience,
		RequireUserID:      cfg.JWT.RequireUserID,
		RequireTenantID:    cfg.JWT.RequireTenantID,
		CacheTTL:           cfg.JWT.CacheTTL,
		KeyRefreshInterval: cfg.JWT.KeyRefreshInterval,
	})
	if err != nil {
		logger.Error("Failed to initialize JWT validator", "error", err)
		os.Exit(1)
	}
	defer jwtValidator.Close()

	// Create and start proxy
	p := proxy.NewProxy(cfg, jwtValidator, logger)
	if err := p.Start(); err != nil {
		logger.Error("Failed to start proxy", "error", err)
		os.Exit(1)
	}

	// Wait for shutdown signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	sig := <-sigChan
	logger.Info("Received shutdown signal", "signal", sig)

	// Graceful shutdown
	if err := p.Stop(); err != nil {
		logger.Error("Error during shutdown", "error", err)
		os.Exit(1)
	}

	logger.Info("JWT Gateway stopped")
}

// loadConfig loads configuration from file and environment.
func loadConfig(configFile string) (*config.Config, error) {
	var cfg *config.Config
	var err error

	if configFile != "" {
		cfg, err = config.LoadFromFile(configFile)
		if err != nil {
			return nil, err
		}
	} else {
		cfg = config.DefaultConfig()
	}

	// Override with environment variables
	config.LoadFromEnv(cfg)

	return cfg, nil
}

// setupLogger creates a structured logger based on configuration.
func setupLogger(cfg config.LoggingConfig) *slog.Logger {
	var level slog.Level
	switch cfg.Level {
	case "debug":
		level = slog.LevelDebug
	case "info":
		level = slog.LevelInfo
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	default:
		level = slog.LevelInfo
	}

	opts := &slog.HandlerOptions{
		Level: level,
	}

	var handler slog.Handler
	if cfg.Format == "json" {
		handler = slog.NewJSONHandler(os.Stdout, opts)
	} else {
		handler = slog.NewTextHandler(os.Stdout, opts)
	}

	return slog.New(handler)
}
