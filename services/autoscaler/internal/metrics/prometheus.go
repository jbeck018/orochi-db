// Package metrics provides Prometheus integration for the autoscaler.
package metrics

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"regexp"
	"strings"
	"time"

	"github.com/prometheus/client_golang/api"
	v1 "github.com/prometheus/client_golang/api/prometheus/v1"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/prometheus/common/model"
)

// validLabelPattern validates label values to prevent PromQL injection.
// Only allows alphanumeric characters, hyphens, underscores, and dots.
var validLabelPattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_\-\.]*$`)

// sanitizePromQLLabel escapes special characters in PromQL label values
// to prevent injection attacks.
func sanitizePromQLLabel(value string) string {
	// First validate that the value matches expected pattern
	if !validLabelPattern.MatchString(value) {
		// If validation fails, escape potentially dangerous characters
		value = strings.ReplaceAll(value, `\`, `\\`)
		value = strings.ReplaceAll(value, `"`, `\"`)
		value = strings.ReplaceAll(value, "\n", `\n`)
		value = strings.ReplaceAll(value, "\r", `\r`)
		value = strings.ReplaceAll(value, "\t", `\t`)
	}
	return value
}

// validateMetricPrefix ensures the metric prefix is safe to use in queries.
func validateMetricPrefix(prefix string) error {
	if prefix == "" {
		return nil
	}
	// Metric prefix should only contain alphanumeric, underscores, and end with underscore
	pattern := regexp.MustCompile(`^[a-zA-Z_][a-zA-Z0-9_]*_?$`)
	if !pattern.MatchString(prefix) {
		return fmt.Errorf("invalid metric prefix: %q", prefix)
	}
	return nil
}

// PrometheusClient wraps the Prometheus API client.
type PrometheusClient struct {
	api          v1.API
	metricPrefix string
	timeout      time.Duration
	logger       *slog.Logger
}

// PrometheusConfig holds Prometheus client configuration.
type PrometheusConfig struct {
	Address      string
	MetricPrefix string
	Timeout      time.Duration
	Logger       *slog.Logger
}

// Default timeouts for HTTP client
const (
	defaultHTTPTimeout         = 30 * time.Second
	defaultHTTPIdleConnTimeout = 90 * time.Second
	defaultQueryTimeout        = 10 * time.Second
)

// NewPrometheusClient creates a new Prometheus client.
func NewPrometheusClient(cfg PrometheusConfig) (*PrometheusClient, error) {
	// Validate metric prefix to prevent injection
	if err := validateMetricPrefix(cfg.MetricPrefix); err != nil {
		return nil, err
	}

	// Set default timeout if not provided
	timeout := cfg.Timeout
	if timeout == 0 {
		timeout = defaultQueryTimeout
	}

	// Create HTTP client with proper timeouts
	httpClient := &http.Client{
		Timeout: defaultHTTPTimeout,
		Transport: &http.Transport{
			MaxIdleConns:        100,
			MaxIdleConnsPerHost: 10,
			IdleConnTimeout:     defaultHTTPIdleConnTimeout,
			DisableCompression:  false,
		},
	}

	client, err := api.NewClient(api.Config{
		Address: cfg.Address,
		Client:  httpClient,
	})
	if err != nil {
		return nil, fmt.Errorf("failed to create Prometheus client: %w", err)
	}

	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	return &PrometheusClient{
		api:          v1.NewAPI(client),
		metricPrefix: cfg.MetricPrefix,
		timeout:      timeout,
		logger:       logger,
	}, nil
}

// QueryClusterMetrics retrieves all metrics for a cluster.
func (c *PrometheusClient) QueryClusterMetrics(ctx context.Context, clusterID, namespace string) (*ClusterMetrics, error) {
	metrics := &ClusterMetrics{
		ClusterID:     clusterID,
		Namespace:     namespace,
		Timestamp:     time.Now(),
		CustomMetrics: make(map[string]float64),
	}

	// Query CPU utilization
	cpu, err := c.queryCPUUtilization(ctx, clusterID, namespace)
	if err != nil {
		return nil, fmt.Errorf("failed to query CPU: %w", err)
	}
	metrics.CPUUtilization = cpu

	// Query memory utilization
	memory, err := c.queryMemoryUtilization(ctx, clusterID, namespace)
	if err != nil {
		return nil, fmt.Errorf("failed to query memory: %w", err)
	}
	metrics.MemoryUtilization = memory

	// Query active connections
	connections, err := c.queryActiveConnections(ctx, clusterID, namespace)
	if err != nil {
		// Non-fatal - connections might not be tracked
		connections = 0
	}
	metrics.ActiveConnections = connections

	// Query latency percentiles
	p50, p95, p99, err := c.queryLatencyPercentiles(ctx, clusterID, namespace)
	if err != nil {
		// Non-fatal - latency might not be tracked
		p50, p95, p99 = 0, 0, 0
	}
	metrics.QueryLatencyP50Ms = p50
	metrics.QueryLatencyP95Ms = p95
	metrics.QueryLatencyP99Ms = p99

	// Query QPS
	qps, err := c.queryQPS(ctx, clusterID, namespace)
	if err != nil {
		qps = 0
	}
	metrics.QueriesPerSecond = qps

	// Query replication lag
	lag, err := c.queryReplicationLag(ctx, clusterID, namespace)
	if err != nil {
		lag = 0
	}
	metrics.ReplicationLagSeconds = lag

	// Query disk usage
	diskBytes, diskPct, err := c.queryDiskUsage(ctx, clusterID, namespace)
	if err != nil {
		diskBytes, diskPct = 0, 0
	}
	metrics.DiskUsageBytes = diskBytes
	metrics.DiskUtilization = diskPct

	return metrics, nil
}

// queryCPUUtilization queries CPU utilization percentage.
func (c *PrometheusClient) queryCPUUtilization(ctx context.Context, clusterID, namespace string) (float64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	query := fmt.Sprintf(`
		avg(
			rate(container_cpu_usage_seconds_total{
				namespace="%s",
				pod=~"%s-.*"
			}[5m])
		) /
		avg(
			kube_pod_container_resource_requests{
				namespace="%s",
				pod=~"%s-.*",
				resource="cpu"
			}
		) * 100
	`, safeNamespace, safeClusterID, safeNamespace, safeClusterID)

	return c.queryScalar(ctx, query)
}

// queryMemoryUtilization queries memory utilization percentage.
func (c *PrometheusClient) queryMemoryUtilization(ctx context.Context, clusterID, namespace string) (float64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	query := fmt.Sprintf(`
		avg(
			container_memory_working_set_bytes{
				namespace="%s",
				pod=~"%s-.*"
			}
		) /
		avg(
			kube_pod_container_resource_requests{
				namespace="%s",
				pod=~"%s-.*",
				resource="memory"
			}
		) * 100
	`, safeNamespace, safeClusterID, safeNamespace, safeClusterID)

	return c.queryScalar(ctx, query)
}

// queryActiveConnections queries the number of active database connections.
func (c *PrometheusClient) queryActiveConnections(ctx context.Context, clusterID, namespace string) (int64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	query := fmt.Sprintf(`
		sum(
			%spg_stat_activity_count{
				namespace="%s",
				cluster="%s",
				state="active"
			}
		)
	`, c.metricPrefix, safeNamespace, safeClusterID)

	val, err := c.queryScalar(ctx, query)
	if err != nil {
		return 0, err
	}
	return int64(val), nil
}

// queryLatencyPercentiles queries query latency percentiles.
func (c *PrometheusClient) queryLatencyPercentiles(ctx context.Context, clusterID, namespace string) (p50, p95, p99 float64, err error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	baseQuery := fmt.Sprintf(`
		histogram_quantile(%%f,
			sum(rate(%spg_query_duration_seconds_bucket{
				namespace="%s",
				cluster="%s"
			}[5m])) by (le)
		) * 1000
	`, c.metricPrefix, safeNamespace, safeClusterID)

	p50, _ = c.queryScalar(ctx, fmt.Sprintf(baseQuery, 0.5))
	p95, _ = c.queryScalar(ctx, fmt.Sprintf(baseQuery, 0.95))
	p99, _ = c.queryScalar(ctx, fmt.Sprintf(baseQuery, 0.99))

	return p50, p95, p99, nil
}

// queryQPS queries the queries per second rate.
func (c *PrometheusClient) queryQPS(ctx context.Context, clusterID, namespace string) (float64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	query := fmt.Sprintf(`
		sum(rate(%spg_stat_statements_calls_total{
			namespace="%s",
			cluster="%s"
		}[5m]))
	`, c.metricPrefix, safeNamespace, safeClusterID)

	return c.queryScalar(ctx, query)
}

// queryReplicationLag queries the replication lag in seconds.
func (c *PrometheusClient) queryReplicationLag(ctx context.Context, clusterID, namespace string) (float64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	query := fmt.Sprintf(`
		max(%spg_replication_lag_seconds{
			namespace="%s",
			cluster="%s"
		})
	`, c.metricPrefix, safeNamespace, safeClusterID)

	return c.queryScalar(ctx, query)
}

// queryDiskUsage queries disk usage bytes and percentage.
func (c *PrometheusClient) queryDiskUsage(ctx context.Context, clusterID, namespace string) (int64, float64, error) {
	// Sanitize inputs to prevent PromQL injection
	safeNamespace := sanitizePromQLLabel(namespace)
	safeClusterID := sanitizePromQLLabel(clusterID)

	bytesQuery := fmt.Sprintf(`
		sum(%spg_database_size_bytes{
			namespace="%s",
			cluster="%s"
		})
	`, c.metricPrefix, safeNamespace, safeClusterID)

	pctQuery := fmt.Sprintf(`
		sum(kubelet_volume_stats_used_bytes{
			namespace="%s",
			persistentvolumeclaim=~"%s-.*"
		}) /
		sum(kubelet_volume_stats_capacity_bytes{
			namespace="%s",
			persistentvolumeclaim=~"%s-.*"
		}) * 100
	`, safeNamespace, safeClusterID, safeNamespace, safeClusterID)

	bytes, _ := c.queryScalar(ctx, bytesQuery)
	pct, _ := c.queryScalar(ctx, pctQuery)

	return int64(bytes), pct, nil
}

// queryScalar executes a query and returns a scalar result.
func (c *PrometheusClient) queryScalar(ctx context.Context, query string) (float64, error) {
	ctx, cancel := context.WithTimeout(ctx, c.timeout)
	defer cancel()

	result, warnings, err := c.api.Query(ctx, query, time.Now())
	if err != nil {
		return 0, fmt.Errorf("query failed: %w", err)
	}

	if len(warnings) > 0 {
		c.logger.Warn("prometheus query returned warnings",
			"warnings", warnings,
			"query_prefix", query[:min(50, len(query))],
		)
	}

	switch v := result.(type) {
	case model.Vector:
		if len(v) == 0 {
			return 0, fmt.Errorf("no data returned")
		}
		return float64(v[0].Value), nil
	case *model.Scalar:
		return float64(v.Value), nil
	default:
		return 0, fmt.Errorf("unexpected result type: %T", result)
	}
}

// QueryRange executes a range query.
func (c *PrometheusClient) QueryRange(ctx context.Context, query string, start, end time.Time, step time.Duration) ([]MetricSample, error) {
	ctx, cancel := context.WithTimeout(ctx, c.timeout)
	defer cancel()

	result, warnings, err := c.api.QueryRange(ctx, query, v1.Range{
		Start: start,
		End:   end,
		Step:  step,
	})
	if err != nil {
		return nil, fmt.Errorf("range query failed: %w", err)
	}

	if len(warnings) > 0 {
		c.logger.Warn("prometheus range query returned warnings",
			"warnings", warnings,
			"query_prefix", query[:min(50, len(query))],
			"start", start,
			"end", end,
		)
	}

	matrix, ok := result.(model.Matrix)
	if !ok {
		return nil, fmt.Errorf("unexpected result type: %T", result)
	}

	var samples []MetricSample
	for _, stream := range matrix {
		labels := make(map[string]string)
		for k, v := range stream.Metric {
			labels[string(k)] = string(v)
		}
		for _, sp := range stream.Values {
			samples = append(samples, MetricSample{
				Timestamp: sp.Timestamp.Time(),
				Value:     float64(sp.Value),
				Labels:    labels,
			})
		}
	}

	return samples, nil
}

// AutoscalerMetrics holds Prometheus metrics for the autoscaler itself.
type AutoscalerMetrics struct {
	ScalingDecisions   *prometheus.CounterVec
	ScalingOperations  *prometheus.CounterVec
	ScalingLatency     *prometheus.HistogramVec
	CurrentReplicas    *prometheus.GaugeVec
	DesiredReplicas    *prometheus.GaugeVec
	MetricValue        *prometheus.GaugeVec
	CooldownActive     *prometheus.GaugeVec
	EvaluationDuration *prometheus.HistogramVec
}

// NewAutoscalerMetrics creates and registers autoscaler metrics.
func NewAutoscalerMetrics(registry prometheus.Registerer) *AutoscalerMetrics {
	m := &AutoscalerMetrics{
		ScalingDecisions: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Name: "autoscaler_scaling_decisions_total",
				Help: "Total number of scaling decisions made",
			},
			[]string{"cluster", "namespace", "direction", "type"},
		),
		ScalingOperations: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Name: "autoscaler_scaling_operations_total",
				Help: "Total number of scaling operations executed",
			},
			[]string{"cluster", "namespace", "direction", "type", "result"},
		),
		ScalingLatency: prometheus.NewHistogramVec(
			prometheus.HistogramOpts{
				Name:    "autoscaler_scaling_operation_duration_seconds",
				Help:    "Time taken to complete scaling operations",
				Buckets: prometheus.ExponentialBuckets(1, 2, 10),
			},
			[]string{"cluster", "namespace", "type"},
		),
		CurrentReplicas: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "autoscaler_current_replicas",
				Help: "Current number of replicas",
			},
			[]string{"cluster", "namespace"},
		),
		DesiredReplicas: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "autoscaler_desired_replicas",
				Help: "Desired number of replicas",
			},
			[]string{"cluster", "namespace"},
		),
		MetricValue: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "autoscaler_metric_value",
				Help: "Current metric values used for scaling decisions",
			},
			[]string{"cluster", "namespace", "metric"},
		),
		CooldownActive: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "autoscaler_cooldown_active",
				Help: "Whether a cooldown period is active (1) or not (0)",
			},
			[]string{"cluster", "namespace", "direction"},
		),
		EvaluationDuration: prometheus.NewHistogramVec(
			prometheus.HistogramOpts{
				Name:    "autoscaler_evaluation_duration_seconds",
				Help:    "Time taken to evaluate scaling decisions",
				Buckets: prometheus.ExponentialBuckets(0.001, 2, 10),
			},
			[]string{"cluster", "namespace"},
		),
	}

	registry.MustRegister(
		m.ScalingDecisions,
		m.ScalingOperations,
		m.ScalingLatency,
		m.CurrentReplicas,
		m.DesiredReplicas,
		m.MetricValue,
		m.CooldownActive,
		m.EvaluationDuration,
	)

	return m
}

// RecordScalingDecision records a scaling decision.
func (m *AutoscalerMetrics) RecordScalingDecision(cluster, namespace, direction, scalingType string) {
	m.ScalingDecisions.WithLabelValues(cluster, namespace, direction, scalingType).Inc()
}

// RecordScalingOperation records a scaling operation result.
func (m *AutoscalerMetrics) RecordScalingOperation(cluster, namespace, direction, scalingType, result string, duration time.Duration) {
	m.ScalingOperations.WithLabelValues(cluster, namespace, direction, scalingType, result).Inc()
	m.ScalingLatency.WithLabelValues(cluster, namespace, scalingType).Observe(duration.Seconds())
}

// SetReplicas sets the current and desired replica counts.
func (m *AutoscalerMetrics) SetReplicas(cluster, namespace string, current, desired int32) {
	m.CurrentReplicas.WithLabelValues(cluster, namespace).Set(float64(current))
	m.DesiredReplicas.WithLabelValues(cluster, namespace).Set(float64(desired))
}

// SetMetricValue sets a metric value used for scaling.
func (m *AutoscalerMetrics) SetMetricValue(cluster, namespace, metric string, value float64) {
	m.MetricValue.WithLabelValues(cluster, namespace, metric).Set(value)
}

// SetCooldownActive sets whether a cooldown is active.
func (m *AutoscalerMetrics) SetCooldownActive(cluster, namespace, direction string, active bool) {
	val := 0.0
	if active {
		val = 1.0
	}
	m.CooldownActive.WithLabelValues(cluster, namespace, direction).Set(val)
}

// MetricsServer serves the /metrics endpoint.
type MetricsServer struct {
	server *http.Server
}

// Server timeout constants
const (
	serverReadTimeout     = 10 * time.Second
	serverWriteTimeout    = 30 * time.Second
	serverIdleTimeout     = 60 * time.Second
	serverReadHeaderTime  = 5 * time.Second
	serverMaxHeaderBytes  = 1 << 20 // 1 MB
)

// NewMetricsServer creates a new metrics HTTP server.
func NewMetricsServer(port int, registry *prometheus.Registry) *MetricsServer {
	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.HandlerFor(registry, promhttp.HandlerOpts{
		EnableOpenMetrics: true,
		Timeout:           serverWriteTimeout - time.Second, // Leave headroom for response writing
	}))

	return &MetricsServer{
		server: &http.Server{
			Addr:              fmt.Sprintf(":%d", port),
			Handler:           mux,
			ReadTimeout:       serverReadTimeout,
			ReadHeaderTimeout: serverReadHeaderTime,
			WriteTimeout:      serverWriteTimeout,
			IdleTimeout:       serverIdleTimeout,
			MaxHeaderBytes:    serverMaxHeaderBytes,
		},
	}
}

// Start starts the metrics server.
func (s *MetricsServer) Start() error {
	return s.server.ListenAndServe()
}

// Shutdown gracefully shuts down the metrics server.
func (s *MetricsServer) Shutdown(ctx context.Context) error {
	return s.server.Shutdown(ctx)
}
