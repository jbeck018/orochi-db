// Package main provides the entry point for the Orochi Cloud control plane server.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/reflection"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/api"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/db"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
	"github.com/orochi-db/orochi-db/services/control-plane/pkg/config"
)

func main() {
	// Initialize logger
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))
	slog.SetDefault(logger)

	logger.Info("starting Orochi Cloud control plane")

	// Load configuration
	cfg := config.Load()

	// Create context that listens for shutdown signals
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Handle shutdown signals
	signalChan := make(chan os.Signal, 1)
	signal.Notify(signalChan, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		sig := <-signalChan
		logger.Info("received shutdown signal", "signal", sig)
		cancel()

		// Wait for second signal to force immediate shutdown
		sig = <-signalChan
		logger.Warn("received second signal, forcing immediate shutdown", "signal", sig)
		os.Exit(1)
	}()

	// Run the server
	if err := run(ctx, cfg, logger); err != nil {
		logger.Error("server error", "error", err)
		signal.Stop(signalChan)
		os.Exit(1)
	}

	// Stop signal handling and clean up
	signal.Stop(signalChan)
	logger.Info("server shutdown complete")
}

func run(ctx context.Context, cfg *config.Config, logger *slog.Logger) error {
	// Connect to database
	logger.Info("connecting to database", "host", cfg.Database.Host, "database", cfg.Database.Database)

	database, err := db.New(ctx, &cfg.Database)
	if err != nil {
		return fmt.Errorf("failed to connect to database: %w", err)
	}
	// Use a closure to track if database was closed during shutdown
	dbClosed := false
	defer func() {
		if !dbClosed {
			logger.Info("closing database connection (defer)")
			database.Close()
		}
	}()

	// Run migrations
	logger.Info("running database migrations")
	if err := database.RunMigrations(ctx); err != nil {
		return fmt.Errorf("failed to run migrations: %w", err)
	}

	// Initialize JWT manager
	jwtManager, err := auth.NewJWTManager(&cfg.JWT)
	if err != nil {
		return fmt.Errorf("failed to initialize JWT manager: %w", err)
	}

	// Initialize services
	userService := services.NewUserService(database, jwtManager, logger)
	clusterService := services.NewClusterService(database, logger)

	// Create HTTP router
	router := api.NewRouter(&api.RouterConfig{
		JWTManager:     jwtManager,
		UserService:    userService,
		ClusterService: clusterService,
		Logger:         logger,
	})

	// Create HTTP server
	httpAddr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	httpServer := &http.Server{
		Addr:         httpAddr,
		Handler:      router,
		ReadTimeout:  cfg.Server.ReadTimeout,
		WriteTimeout: cfg.Server.WriteTimeout,
		IdleTimeout:  cfg.Server.IdleTimeout,
	}

	// Create gRPC server
	grpcServer := grpc.NewServer()
	reflection.Register(grpcServer) // Enable reflection for debugging

	// Register gRPC services here
	// Example: pb.RegisterControlPlaneServer(grpcServer, newGRPCHandler())

	grpcAddr := fmt.Sprintf("%s:%d", cfg.GRPC.Host, cfg.GRPC.Port)
	grpcListener, err := net.Listen("tcp", grpcAddr)
	if err != nil {
		return fmt.Errorf("failed to create gRPC listener: %w", err)
	}
	// Ensure listener is closed on early return
	grpcListenerClosed := false
	defer func() {
		if !grpcListenerClosed {
			logger.Info("closing gRPC listener (defer)")
			grpcListener.Close()
		}
	}()

	// Error channel for server errors
	errChan := make(chan error, 2)

	// Start HTTP server
	go func() {
		logger.Info("starting HTTP server", "addr", httpAddr)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errChan <- fmt.Errorf("HTTP server error: %w", err)
		}
	}()

	// Start gRPC server
	go func() {
		logger.Info("starting gRPC server", "addr", grpcAddr)
		if err := grpcServer.Serve(grpcListener); err != nil {
			errChan <- fmt.Errorf("gRPC server error: %w", err)
		}
	}()

	// Wait for shutdown signal or error
	select {
	case <-ctx.Done():
		logger.Info("initiating graceful shutdown")
	case err := <-errChan:
		return err
	}

	// Graceful shutdown with timeout
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer shutdownCancel()

	// Create error channel for shutdown errors
	shutdownErrChan := make(chan error, 2)

	// Shutdown HTTP server concurrently
	go func() {
		logger.Info("shutting down HTTP server")
		if err := httpServer.Shutdown(shutdownCtx); err != nil {
			shutdownErrChan <- fmt.Errorf("HTTP server shutdown error: %w", err)
		} else {
			shutdownErrChan <- nil
		}
	}()

	// Shutdown gRPC server concurrently
	go func() {
		logger.Info("shutting down gRPC server")
		// GracefulStop blocks until all RPCs complete, so run with timeout awareness
		stopped := make(chan struct{})
		go func() {
			grpcServer.GracefulStop()
			close(stopped)
		}()

		select {
		case <-stopped:
			grpcListenerClosed = true // GracefulStop closes the listener
			shutdownErrChan <- nil
		case <-shutdownCtx.Done():
			logger.Warn("gRPC graceful shutdown timed out, forcing stop")
			grpcServer.Stop()
			grpcListenerClosed = true
			shutdownErrChan <- fmt.Errorf("gRPC server shutdown timed out")
		}
	}()

	// Wait for both servers to shutdown
	var shutdownErrs []error
	for i := 0; i < 2; i++ {
		if err := <-shutdownErrChan; err != nil {
			shutdownErrs = append(shutdownErrs, err)
		}
	}

	// Close database connection explicitly during shutdown
	logger.Info("closing database connection")
	database.Close()
	dbClosed = true

	// Report any shutdown errors
	if len(shutdownErrs) > 0 {
		for _, err := range shutdownErrs {
			logger.Error("shutdown error", "error", err)
		}
	}

	return nil
}
