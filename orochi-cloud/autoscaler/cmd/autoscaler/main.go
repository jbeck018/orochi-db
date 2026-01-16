// Package main is the entry point for the Orochi Cloud Autoscaler service.
package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/orochi-db/orochi-cloud/autoscaler/internal/grpc"
	"github.com/orochi-db/orochi-cloud/autoscaler/internal/k8s"
	"github.com/orochi-db/orochi-cloud/autoscaler/internal/metrics"
	"github.com/orochi-db/orochi-cloud/autoscaler/internal/scaler"
	"github.com/orochi-db/orochi-cloud/autoscaler/pkg/config"
)

func main() {
	// Parse command-line flags
	configPath := flag.String("config", "", "Path to configuration file")
	flag.Parse()

	// Load configuration
	cfg, err := config.LoadConfig(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load configuration: %v\n", err)
		os.Exit(1)
	}

	// Override with environment variables
	config.LoadConfigFromEnv(cfg)

	// Validate configuration
	if err := cfg.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "Invalid configuration: %v\n", err)
		os.Exit(1)
	}

	// Create context with cancellation
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Initialize Prometheus registry
	registry := prometheus.NewRegistry()
	registry.MustRegister(prometheus.NewGoCollector())
	registry.MustRegister(prometheus.NewProcessCollector(prometheus.ProcessCollectorOpts{}))

	// Initialize autoscaler metrics
	autoscalerMetrics := metrics.NewAutoscalerMetrics(registry)

	// Initialize Prometheus client
	prometheusClient, err := metrics.NewPrometheusClient(metrics.PrometheusConfig{
		Address:      cfg.Prometheus.Address,
		MetricPrefix: cfg.Prometheus.MetricPrefix,
		Timeout:      cfg.Prometheus.Timeout,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create Prometheus client: %v\n", err)
		os.Exit(1)
	}

	// Initialize Kubernetes client
	k8sClient, err := k8s.NewClient(k8s.ClientConfig{
		InCluster:     cfg.Kubernetes.InCluster,
		Kubeconfig:    cfg.Kubernetes.Kubeconfig,
		Namespace:     cfg.Kubernetes.Namespace,
		ResyncPeriod:  cfg.Kubernetes.ResyncPeriod,
		QPS:           cfg.Kubernetes.QPS,
		Burst:         cfg.Kubernetes.Burst,
		LabelSelector: cfg.Kubernetes.LabelSelector,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create Kubernetes client: %v\n", err)
		os.Exit(1)
	}

	// Start Kubernetes informers
	fmt.Println("Starting Kubernetes informers...")
	if err := k8sClient.Start(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start Kubernetes informers: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Kubernetes informers started")

	// Initialize metrics collector
	metricsCollector := metrics.NewMetricsCollector(
		prometheusClient,
		cfg.Prometheus.ScrapeInterval,
		cfg.Scaling.MetricHistoryWindow,
	)

	// Start metrics collection
	fmt.Println("Starting metrics collection...")
	if err := metricsCollector.Start(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start metrics collector: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Metrics collection started")

	// Initialize policy engine
	policyEngine := scaler.NewPolicyEngine()

	// Initialize event recorder
	eventRecorder := scaler.NewScalingEventRecorder(24*time.Hour, 1000)

	// Initialize horizontal scaler
	horizontalScaler := scaler.NewHorizontalScaler(
		k8sClient,
		metricsCollector,
		policyEngine,
		autoscalerMetrics,
		eventRecorder,
		scaler.HorizontalScalerConfig{
			EvaluationInterval: cfg.Scaling.EvaluationInterval,
		},
	)

	// Initialize vertical scaler
	verticalScaler := scaler.NewVerticalScaler(
		k8sClient,
		metricsCollector,
		policyEngine,
		autoscalerMetrics,
		eventRecorder,
		scaler.VerticalScalerConfig{
			EvaluationInterval: cfg.Scaling.EvaluationInterval * 2, // Evaluate vertical less frequently
		},
	)

	// Start horizontal scaler
	fmt.Println("Starting horizontal scaler...")
	if err := horizontalScaler.Start(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start horizontal scaler: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Horizontal scaler started")

	// Start vertical scaler
	fmt.Println("Starting vertical scaler...")
	if err := verticalScaler.Start(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start vertical scaler: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Vertical scaler started")

	// Initialize gRPC server
	grpcServer := grpc.NewServer(
		grpc.ServerConfig{
			Port: cfg.Server.GRPCPort,
		},
		grpc.ServerDependencies{
			K8sClient:        k8sClient,
			MetricsCollector: metricsCollector,
			HorizontalScaler: horizontalScaler,
			VerticalScaler:   verticalScaler,
			PolicyEngine:     policyEngine,
			EventRecorder:    eventRecorder,
		},
	)

	// Start metrics HTTP server
	metricsServer := metrics.NewMetricsServer(cfg.Server.MetricsPort, registry)
	go func() {
		fmt.Printf("Metrics server listening on port %d\n", cfg.Server.MetricsPort)
		if err := metricsServer.Start(); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "Metrics server error: %v\n", err)
		}
	}()

	// Start health server
	healthServer := startHealthServer(cfg.Server.HealthPort)

	// Start gRPC server in goroutine
	go func() {
		fmt.Printf("Starting gRPC server on port %d...\n", cfg.Server.GRPCPort)
		if err := grpcServer.Start(); err != nil {
			fmt.Fprintf(os.Stderr, "gRPC server error: %v\n", err)
			cancel()
		}
	}()

	fmt.Println("Orochi Cloud Autoscaler is running")
	fmt.Printf("  gRPC:    :%d\n", cfg.Server.GRPCPort)
	fmt.Printf("  Metrics: :%d/metrics\n", cfg.Server.MetricsPort)
	fmt.Printf("  Health:  :%d/healthz\n", cfg.Server.HealthPort)

	// Wait for shutdown signal
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case sig := <-sigCh:
		fmt.Printf("\nReceived signal %v, shutting down...\n", sig)
	case <-ctx.Done():
		fmt.Println("\nContext cancelled, shutting down...")
	}

	// Graceful shutdown
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), cfg.Server.ShutdownTimeout)
	defer shutdownCancel()

	// Stop components in reverse order
	fmt.Println("Stopping gRPC server...")
	grpcServer.Stop()

	fmt.Println("Stopping health server...")
	healthServer.Shutdown(shutdownCtx)

	fmt.Println("Stopping metrics server...")
	metricsServer.Shutdown(shutdownCtx)

	fmt.Println("Stopping vertical scaler...")
	verticalScaler.Stop()

	fmt.Println("Stopping horizontal scaler...")
	horizontalScaler.Stop()

	fmt.Println("Stopping metrics collection...")
	metricsCollector.Stop()

	fmt.Println("Stopping Kubernetes client...")
	k8sClient.Stop()

	fmt.Println("Shutdown complete")
}

// startHealthServer starts the health check HTTP server.
func startHealthServer(port int) *http.Server {
	mux := http.NewServeMux()

	// Liveness probe - always returns OK if the process is running
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("ok"))
	})

	// Readiness probe - returns OK if all components are ready
	mux.HandleFunc("/readyz", func(w http.ResponseWriter, r *http.Request) {
		// In a real implementation, check if all components are ready
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("ok"))
	})

	server := &http.Server{
		Addr:    fmt.Sprintf(":%d", port),
		Handler: mux,
	}

	go func() {
		fmt.Printf("Health server listening on port %d\n", port)
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "Health server error: %v\n", err)
		}
	}()

	return server
}
