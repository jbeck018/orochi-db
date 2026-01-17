// Package main provides the entry point for the provisioner service.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"

	"github.com/orochi-db/orochi-cloud/provisioner/internal/grpc"
	"github.com/orochi-db/orochi-cloud/provisioner/internal/provisioner"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/config"
)

var (
	version   = "dev"
	commit    = "unknown"
	buildTime = "unknown"
)

func main() {
	// Parse command line flags
	configFile := flag.String("config", "", "Path to configuration file")
	showVersion := flag.Bool("version", false, "Show version information")
	flag.Parse()

	if *showVersion {
		fmt.Printf("Orochi Provisioner %s (commit: %s, built: %s)\n", version, commit, buildTime)
		os.Exit(0)
	}

	// Load configuration
	cfg, err := config.LoadConfig(*configFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load configuration: %v\n", err)
		os.Exit(1)
	}

	// Initialize logger
	logger, closeLogFiles, err := initLogger(cfg.Logging)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to initialize logger: %v\n", err)
		os.Exit(1)
	}
	defer func() {
		logger.Sync()
		closeLogFiles()
	}()

	logger.Info("starting Orochi Provisioner",
		zap.String("version", version),
		zap.String("commit", commit),
		zap.String("buildTime", buildTime),
	)

	// Create context with cancellation
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Handle shutdown signals
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		sig := <-sigChan
		logger.Info("received shutdown signal", zap.String("signal", sig.String()))
		cancel()
	}()

	// Initialize provisioner service
	service, err := provisioner.NewService(cfg, logger)
	if err != nil {
		logger.Fatal("failed to create provisioner service", zap.Error(err))
	}

	// Perform initial health check
	if err := service.HealthCheck(ctx); err != nil {
		logger.Warn("initial health check failed, continuing anyway", zap.Error(err))
	}

	// Initialize and start gRPC server
	server, err := grpc.NewServer(cfg, service, logger)
	if err != nil {
		logger.Fatal("failed to create gRPC server", zap.Error(err))
	}

	// Start server (blocks until context is cancelled)
	if err := server.Start(ctx); err != nil {
		logger.Fatal("gRPC server failed", zap.Error(err))
	}

	logger.Info("provisioner shutdown complete")
}

// initLogger initializes the logger and returns a cleanup function to close any opened log files.
// The cleanup function should be called during shutdown to prevent file handle leaks.
func initLogger(cfg config.LoggingConfig) (*zap.Logger, func(), error) {
	var level zapcore.Level
	if err := level.UnmarshalText([]byte(cfg.Level)); err != nil {
		level = zapcore.InfoLevel
	}

	var encoderConfig zapcore.EncoderConfig
	if cfg.Format == "console" {
		encoderConfig = zap.NewDevelopmentEncoderConfig()
	} else {
		encoderConfig = zap.NewProductionEncoderConfig()
	}
	encoderConfig.EncodeTime = zapcore.ISO8601TimeEncoder

	var encoder zapcore.Encoder
	if cfg.Format == "console" {
		encoder = zapcore.NewConsoleEncoder(encoderConfig)
	} else {
		encoder = zapcore.NewJSONEncoder(encoderConfig)
	}

	outputPaths := cfg.OutputPaths
	if len(outputPaths) == 0 {
		outputPaths = []string{"stdout"}
	}

	// Track opened files for cleanup
	var openedFiles []*os.File

	// Create writers for output paths
	writers := make([]zapcore.WriteSyncer, 0, len(outputPaths))
	for _, path := range outputPaths {
		switch path {
		case "stdout":
			writers = append(writers, zapcore.AddSync(os.Stdout))
		case "stderr":
			writers = append(writers, zapcore.AddSync(os.Stderr))
		default:
			file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
			if err != nil {
				// Close any files we've already opened before returning error
				for _, f := range openedFiles {
					f.Close()
				}
				return nil, nil, fmt.Errorf("failed to open log file %s: %w", path, err)
			}
			openedFiles = append(openedFiles, file)
			writers = append(writers, zapcore.AddSync(file))
		}
	}

	// Create cleanup function to close all opened log files
	closeLogFiles := func() {
		for _, f := range openedFiles {
			f.Close()
		}
	}

	core := zapcore.NewCore(
		encoder,
		zapcore.NewMultiWriteSyncer(writers...),
		level,
	)

	return zap.New(core, zap.AddCaller(), zap.AddStacktrace(zapcore.ErrorLevel)), closeLogFiles, nil
}
