// Package grpc provides the gRPC server for the provisioner service.
package grpc

import (
	"context"
	"crypto/tls"
	"fmt"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.uber.org/zap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/reflection"

	"github.com/orochi-db/orochi-cloud/provisioner/internal/provisioner"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/config"
)

// Server represents the gRPC server
type Server struct {
	cfg           *config.Config
	grpcServer    *grpc.Server
	healthServer  *health.Server
	service       *provisioner.Service
	handler       *Handler
	logger        *zap.Logger
	metricsServer *http.Server
	metricsMu     sync.Mutex       // protects metricsServer access
	metricsReady  chan struct{}    // signals when metrics server is ready
	stopOnce      sync.Once        // ensures Stop() is only executed once
}

// NewServer creates a new gRPC server
func NewServer(cfg *config.Config, service *provisioner.Service, logger *zap.Logger) (*Server, error) {
	// Create gRPC server options
	opts := []grpc.ServerOption{
		grpc.MaxConcurrentStreams(cfg.Server.MaxConcurrentStreams),
		grpc.KeepaliveParams(keepalive.ServerParameters{
			MaxConnectionIdle:     15 * time.Minute,
			MaxConnectionAge:      30 * time.Minute,
			MaxConnectionAgeGrace: 5 * time.Minute,
			Time:                  5 * time.Minute,
			Timeout:               1 * time.Minute,
		}),
		grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{
			MinTime:             1 * time.Minute,
			PermitWithoutStream: true,
		}),
	}

	// Add TLS if enabled
	if cfg.Server.TLSEnabled {
		cert, err := tls.LoadX509KeyPair(cfg.Server.TLSCertFile, cfg.Server.TLSKeyFile)
		if err != nil {
			return nil, fmt.Errorf("failed to load TLS credentials: %w", err)
		}
		creds := credentials.NewTLS(&tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
		})
		opts = append(opts, grpc.Creds(creds))
	}

	// Add middleware
	opts = append(opts,
		grpc.ChainUnaryInterceptor(
			unaryLoggingInterceptor(logger),
			unaryRecoveryInterceptor(logger),
			unaryMetricsInterceptor(),
		),
		grpc.ChainStreamInterceptor(
			streamLoggingInterceptor(logger),
			streamRecoveryInterceptor(logger),
		),
	)

	// Create gRPC server
	grpcServer := grpc.NewServer(opts...)

	// Create health server
	healthServer := health.NewServer()

	// Create handler
	handler := NewHandler(service, logger)

	return &Server{
		cfg:          cfg,
		grpcServer:   grpcServer,
		healthServer: healthServer,
		service:      service,
		handler:      handler,
		logger:       logger,
		metricsReady: make(chan struct{}),
	}, nil
}

// Start starts the gRPC server
func (s *Server) Start(ctx context.Context) error {
	// Register services
	RegisterProvisionerServer(s.grpcServer, s.handler)
	healthpb.RegisterHealthServer(s.grpcServer, s.healthServer)
	reflection.Register(s.grpcServer)

	// Set health status
	s.healthServer.SetServingStatus("", healthpb.HealthCheckResponse_SERVING)
	s.healthServer.SetServingStatus("provisioner.v1.ProvisionerService", healthpb.HealthCheckResponse_SERVING)

	// Start metrics server if enabled
	if s.cfg.Metrics.Enabled {
		go s.startMetricsServer()
	} else {
		// Signal that metrics is not being used (no need to wait in Stop)
		close(s.metricsReady)
	}

	// Start gRPC server
	addr := fmt.Sprintf(":%d", s.cfg.Server.GRPCPort)
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen on %s: %w", addr, err)
	}

	s.logger.Info("starting gRPC server",
		zap.String("address", addr),
		zap.Bool("tls", s.cfg.Server.TLSEnabled),
	)

	// Handle graceful shutdown
	go func() {
		<-ctx.Done()
		s.logger.Info("shutting down gRPC server")
		s.healthServer.SetServingStatus("", healthpb.HealthCheckResponse_NOT_SERVING)
		s.grpcServer.GracefulStop()
	}()

	if err := s.grpcServer.Serve(listener); err != nil {
		return fmt.Errorf("gRPC server failed: %w", err)
	}

	return nil
}

// Stop stops the gRPC server
func (s *Server) Stop() {
	s.stopOnce.Do(func() {
		s.healthServer.SetServingStatus("", healthpb.HealthCheckResponse_NOT_SERVING)
		s.grpcServer.GracefulStop()

		// Wait for metrics server to be ready (with timeout) before attempting shutdown
		select {
		case <-s.metricsReady:
			// Metrics server is ready, proceed with shutdown
		case <-time.After(5 * time.Second):
			// Timeout waiting for metrics server to be ready
			s.logger.Warn("timeout waiting for metrics server to be ready")
			return
		}

		s.metricsMu.Lock()
		metricsServer := s.metricsServer
		s.metricsMu.Unlock()

		if metricsServer != nil {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if err := metricsServer.Shutdown(ctx); err != nil {
				s.logger.Error("failed to shutdown metrics server", zap.Error(err))
			}
		}
	})
}

// startMetricsServer starts the Prometheus metrics server
func (s *Server) startMetricsServer() {
	mux := http.NewServeMux()
	mux.Handle(s.cfg.Metrics.Path, promhttp.Handler())
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("OK"))
	})

	addr := fmt.Sprintf(":%d", s.cfg.Metrics.Port)

	// Create server with proper timeouts to prevent resource exhaustion
	server := &http.Server{
		Addr:         addr,
		Handler:      mux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Safely set the metricsServer field with mutex protection
	s.metricsMu.Lock()
	s.metricsServer = server
	s.metricsMu.Unlock()

	// Signal that metrics server is ready
	close(s.metricsReady)

	s.logger.Info("starting metrics server",
		zap.String("address", addr),
		zap.String("path", s.cfg.Metrics.Path),
	)

	if err := server.ListenAndServe(); err != http.ErrServerClosed {
		s.logger.Error("metrics server failed", zap.Error(err))
	}
}

// Middleware interceptors

func unaryLoggingInterceptor(logger *zap.Logger) grpc.UnaryServerInterceptor {
	return func(ctx context.Context, req interface{}, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (interface{}, error) {
		start := time.Now()

		resp, err := handler(ctx, req)

		duration := time.Since(start)
		if err != nil {
			logger.Error("gRPC request failed",
				zap.String("method", info.FullMethod),
				zap.Duration("duration", duration),
				zap.Error(err),
			)
		} else {
			logger.Info("gRPC request completed",
				zap.String("method", info.FullMethod),
				zap.Duration("duration", duration),
			)
		}

		return resp, err
	}
}

func unaryRecoveryInterceptor(logger *zap.Logger) grpc.UnaryServerInterceptor {
	return func(ctx context.Context, req interface{}, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (resp interface{}, err error) {
		defer func() {
			if r := recover(); r != nil {
				logger.Error("panic recovered in gRPC handler",
					zap.String("method", info.FullMethod),
					zap.Any("panic", r),
				)
				err = fmt.Errorf("internal server error")
			}
		}()
		return handler(ctx, req)
	}
}

func streamLoggingInterceptor(logger *zap.Logger) grpc.StreamServerInterceptor {
	return func(srv interface{}, ss grpc.ServerStream, info *grpc.StreamServerInfo, handler grpc.StreamHandler) error {
		start := time.Now()

		err := handler(srv, ss)

		duration := time.Since(start)
		if err != nil {
			logger.Error("gRPC stream failed",
				zap.String("method", info.FullMethod),
				zap.Duration("duration", duration),
				zap.Error(err),
			)
		} else {
			logger.Info("gRPC stream completed",
				zap.String("method", info.FullMethod),
				zap.Duration("duration", duration),
			)
		}

		return err
	}
}

func streamRecoveryInterceptor(logger *zap.Logger) grpc.StreamServerInterceptor {
	return func(srv interface{}, ss grpc.ServerStream, info *grpc.StreamServerInfo, handler grpc.StreamHandler) (err error) {
		defer func() {
			if r := recover(); r != nil {
				logger.Error("panic recovered in gRPC stream handler",
					zap.String("method", info.FullMethod),
					zap.Any("panic", r),
				)
				err = fmt.Errorf("internal server error")
			}
		}()
		return handler(srv, ss)
	}
}

// Metrics

var (
	grpcRequestsTotal = prometheus.NewCounterVec(
		prometheus.CounterOpts{
			Name: "provisioner_grpc_requests_total",
			Help: "Total number of gRPC requests",
		},
		[]string{"method", "status"},
	)

	grpcRequestDuration = prometheus.NewHistogramVec(
		prometheus.HistogramOpts{
			Name:    "provisioner_grpc_request_duration_seconds",
			Help:    "Duration of gRPC requests",
			Buckets: prometheus.DefBuckets,
		},
		[]string{"method"},
	)
)

func init() {
	prometheus.MustRegister(grpcRequestsTotal)
	prometheus.MustRegister(grpcRequestDuration)
}

func unaryMetricsInterceptor() grpc.UnaryServerInterceptor {
	return func(ctx context.Context, req interface{}, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (interface{}, error) {
		start := time.Now()

		resp, err := handler(ctx, req)

		duration := time.Since(start)
		status := "ok"
		if err != nil {
			status = "error"
		}

		grpcRequestsTotal.WithLabelValues(info.FullMethod, status).Inc()
		grpcRequestDuration.WithLabelValues(info.FullMethod).Observe(duration.Seconds())

		return resp, err
	}
}

// Placeholder for generated code - this would normally be generated from proto
// RegisterProvisionerServer registers the provisioner service
func RegisterProvisionerServer(s *grpc.Server, srv *Handler) {
	// This is a placeholder - actual registration would use generated code
	// s.RegisterService(&_ProvisionerService_serviceDesc, srv)
}

// Note: grpc_middleware v2 is available but interceptors are created inline
