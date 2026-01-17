// Package metrics provides metrics collection functionality for the autoscaler.
package metrics

import (
	"context"
	"fmt"
	"sync"
	"time"
)

// MetricType represents the type of metric being collected.
type MetricType string

const (
	MetricTypeCPU           MetricType = "cpu_utilization"
	MetricTypeMemory        MetricType = "memory_utilization"
	MetricTypeConnections   MetricType = "active_connections"
	MetricTypeQueryLatency  MetricType = "query_latency"
	MetricTypeQPS           MetricType = "queries_per_second"
	MetricTypeReplication   MetricType = "replication_lag"
	MetricTypeDisk          MetricType = "disk_utilization"
	MetricTypeCustom        MetricType = "custom"
)

// ClusterMetrics holds the current metrics for a cluster.
type ClusterMetrics struct {
	ClusterID              string
	Namespace              string
	Timestamp              time.Time
	CPUUtilization         float64
	MemoryUtilization      float64
	ActiveConnections      int64
	QueryLatencyP50Ms      float64
	QueryLatencyP95Ms      float64
	QueryLatencyP99Ms      float64
	QueriesPerSecond       float64
	ReplicationLagSeconds  float64
	DiskUsageBytes         int64
	DiskUtilization        float64
	CustomMetrics          map[string]float64
}

// MetricSample represents a single metric measurement.
type MetricSample struct {
	Timestamp time.Time
	Value     float64
	Labels    map[string]string
}

// MetricHistory holds historical metrics for trend analysis.
type MetricHistory struct {
	mu       sync.RWMutex
	samples  map[string][]MetricSample
	maxAge   time.Duration
	maxCount int
}

// NewMetricHistory creates a new metric history tracker.
func NewMetricHistory(maxAge time.Duration, maxCount int) *MetricHistory {
	return &MetricHistory{
		samples:  make(map[string][]MetricSample),
		maxAge:   maxAge,
		maxCount: maxCount,
	}
}

// Add adds a sample to the history.
func (h *MetricHistory) Add(metricKey string, sample MetricSample) {
	h.mu.Lock()
	defer h.mu.Unlock()

	samples := h.samples[metricKey]
	samples = append(samples, sample)

	// Prune old samples
	cutoff := time.Now().Add(-h.maxAge)
	startIdx := 0
	for i, s := range samples {
		if s.Timestamp.After(cutoff) {
			startIdx = i
			break
		}
	}
	samples = samples[startIdx:]

	// Enforce max count
	if len(samples) > h.maxCount {
		samples = samples[len(samples)-h.maxCount:]
	}

	h.samples[metricKey] = samples
}

// Get retrieves samples for a metric within a time window.
func (h *MetricHistory) Get(metricKey string, since time.Time) []MetricSample {
	h.mu.RLock()
	defer h.mu.RUnlock()

	samples := h.samples[metricKey]
	var result []MetricSample
	for _, s := range samples {
		if s.Timestamp.After(since) || s.Timestamp.Equal(since) {
			result = append(result, s)
		}
	}
	return result
}

// Average calculates the average value for a metric over a time window.
func (h *MetricHistory) Average(metricKey string, since time.Time) (float64, bool) {
	samples := h.Get(metricKey, since)
	if len(samples) == 0 {
		return 0, false
	}

	var sum float64
	for _, s := range samples {
		sum += s.Value
	}
	return sum / float64(len(samples)), true
}

// Trend calculates the trend (rate of change) for a metric.
func (h *MetricHistory) Trend(metricKey string, since time.Time) (float64, bool) {
	samples := h.Get(metricKey, since)
	if len(samples) < 2 {
		return 0, false
	}

	// Simple linear regression
	n := float64(len(samples))
	var sumX, sumY, sumXY, sumX2 float64
	start := samples[0].Timestamp

	for _, s := range samples {
		x := s.Timestamp.Sub(start).Seconds()
		y := s.Value
		sumX += x
		sumY += y
		sumXY += x * y
		sumX2 += x * x
	}

	denominator := n*sumX2 - sumX*sumX
	if denominator == 0 {
		return 0, false
	}

	slope := (n*sumXY - sumX*sumY) / denominator
	return slope, true
}

// Percentile calculates a percentile value for a metric.
func (h *MetricHistory) Percentile(metricKey string, since time.Time, p float64) (float64, bool) {
	samples := h.Get(metricKey, since)
	if len(samples) == 0 {
		return 0, false
	}

	// Sort values
	values := make([]float64, len(samples))
	for i, s := range samples {
		values[i] = s.Value
	}
	sortFloat64s(values)

	// Calculate percentile index
	index := (p / 100.0) * float64(len(values)-1)
	lower := int(index)
	upper := lower + 1
	if upper >= len(values) {
		return values[len(values)-1], true
	}

	weight := index - float64(lower)
	return values[lower]*(1-weight) + values[upper]*weight, true
}

// sortFloat64s sorts a slice of float64 values in place.
func sortFloat64s(values []float64) {
	// Simple insertion sort for small arrays
	for i := 1; i < len(values); i++ {
		for j := i; j > 0 && values[j] < values[j-1]; j-- {
			values[j], values[j-1] = values[j-1], values[j]
		}
	}
}

// Collector is the interface for metrics collection.
type Collector interface {
	// Collect gathers metrics for a specific cluster.
	Collect(ctx context.Context, clusterID, namespace string) (*ClusterMetrics, error)

	// CollectAll gathers metrics for all tracked clusters.
	CollectAll(ctx context.Context) (map[string]*ClusterMetrics, error)

	// RegisterCluster registers a cluster for metrics collection.
	RegisterCluster(clusterID, namespace string) error

	// UnregisterCluster removes a cluster from metrics collection.
	UnregisterCluster(clusterID, namespace string) error

	// GetHistory returns the metric history for a cluster.
	GetHistory(clusterID, namespace string) *MetricHistory

	// Start begins background metrics collection.
	Start(ctx context.Context) error

	// Stop halts background metrics collection.
	Stop() error
}

// MetricsCollector implements the Collector interface.
type MetricsCollector struct {
	mu               sync.RWMutex
	prometheusClient *PrometheusClient
	clusters         map[string]clusterInfo
	histories        map[string]*MetricHistory
	scrapeInterval   time.Duration
	historyWindow    time.Duration
	stopCh           chan struct{}
	doneCh           chan struct{}
}

type clusterInfo struct {
	clusterID string
	namespace string
}

// NewMetricsCollector creates a new metrics collector.
func NewMetricsCollector(prometheusClient *PrometheusClient, scrapeInterval, historyWindow time.Duration) *MetricsCollector {
	return &MetricsCollector{
		prometheusClient: prometheusClient,
		clusters:         make(map[string]clusterInfo),
		histories:        make(map[string]*MetricHistory),
		scrapeInterval:   scrapeInterval,
		historyWindow:    historyWindow,
	}
}

// clusterKey generates a unique key for a cluster.
func clusterKey(clusterID, namespace string) string {
	return fmt.Sprintf("%s/%s", namespace, clusterID)
}

// RegisterCluster registers a cluster for metrics collection.
func (c *MetricsCollector) RegisterCluster(clusterID, namespace string) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	key := clusterKey(clusterID, namespace)
	c.clusters[key] = clusterInfo{
		clusterID: clusterID,
		namespace: namespace,
	}
	c.histories[key] = NewMetricHistory(c.historyWindow, 1000)
	return nil
}

// UnregisterCluster removes a cluster from metrics collection.
func (c *MetricsCollector) UnregisterCluster(clusterID, namespace string) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	key := clusterKey(clusterID, namespace)
	delete(c.clusters, key)
	delete(c.histories, key)
	return nil
}

// GetHistory returns the metric history for a cluster.
func (c *MetricsCollector) GetHistory(clusterID, namespace string) *MetricHistory {
	c.mu.RLock()
	defer c.mu.RUnlock()

	key := clusterKey(clusterID, namespace)
	return c.histories[key]
}

// Collect gathers metrics for a specific cluster.
func (c *MetricsCollector) Collect(ctx context.Context, clusterID, namespace string) (*ClusterMetrics, error) {
	metrics, err := c.prometheusClient.QueryClusterMetrics(ctx, clusterID, namespace)
	if err != nil {
		return nil, fmt.Errorf("failed to collect metrics: %w", err)
	}

	// Update history
	key := clusterKey(clusterID, namespace)
	c.mu.RLock()
	history := c.histories[key]
	c.mu.RUnlock()

	if history != nil {
		now := time.Now()
		history.Add(string(MetricTypeCPU), MetricSample{Timestamp: now, Value: metrics.CPUUtilization})
		history.Add(string(MetricTypeMemory), MetricSample{Timestamp: now, Value: metrics.MemoryUtilization})
		history.Add(string(MetricTypeConnections), MetricSample{Timestamp: now, Value: float64(metrics.ActiveConnections)})
		history.Add(string(MetricTypeQueryLatency), MetricSample{Timestamp: now, Value: metrics.QueryLatencyP95Ms})
		history.Add(string(MetricTypeQPS), MetricSample{Timestamp: now, Value: metrics.QueriesPerSecond})
		history.Add(string(MetricTypeReplication), MetricSample{Timestamp: now, Value: metrics.ReplicationLagSeconds})
		history.Add(string(MetricTypeDisk), MetricSample{Timestamp: now, Value: metrics.DiskUtilization})
	}

	return metrics, nil
}

// CollectAll gathers metrics for all tracked clusters.
func (c *MetricsCollector) CollectAll(ctx context.Context) (map[string]*ClusterMetrics, error) {
	c.mu.RLock()
	clusters := make([]clusterInfo, 0, len(c.clusters))
	for _, info := range c.clusters {
		clusters = append(clusters, info)
	}
	c.mu.RUnlock()

	result := make(map[string]*ClusterMetrics)
	var mu sync.Mutex
	var wg sync.WaitGroup
	errCh := make(chan error, len(clusters))

	for _, info := range clusters {
		wg.Add(1)
		go func(info clusterInfo) {
			defer wg.Done()

			metrics, err := c.Collect(ctx, info.clusterID, info.namespace)
			if err != nil {
				errCh <- fmt.Errorf("failed to collect metrics for %s/%s: %w",
					info.namespace, info.clusterID, err)
				return
			}

			mu.Lock()
			result[clusterKey(info.clusterID, info.namespace)] = metrics
			mu.Unlock()
		}(info)
	}

	wg.Wait()
	close(errCh)

	// Collect any errors
	var errors []error
	for err := range errCh {
		errors = append(errors, err)
	}

	if len(errors) > 0 {
		return result, fmt.Errorf("some metrics collection failed: %v", errors)
	}

	return result, nil
}

// Start begins background metrics collection.
func (c *MetricsCollector) Start(ctx context.Context) error {
	c.stopCh = make(chan struct{})
	c.doneCh = make(chan struct{})

	go func() {
		defer close(c.doneCh)
		ticker := time.NewTicker(c.scrapeInterval)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				if _, err := c.CollectAll(ctx); err != nil {
					// Log error but continue collecting
					fmt.Printf("metrics collection error: %v\n", err)
				}
			case <-c.stopCh:
				return
			case <-ctx.Done():
				return
			}
		}
	}()

	return nil
}

// Stop halts background metrics collection.
func (c *MetricsCollector) Stop() error {
	if c.stopCh != nil {
		close(c.stopCh)
		<-c.doneCh
	}
	return nil
}

// MetricAggregator provides methods for aggregating metrics across pods.
type MetricAggregator struct{}

// NewMetricAggregator creates a new metric aggregator.
func NewMetricAggregator() *MetricAggregator {
	return &MetricAggregator{}
}

// AggregateAverage calculates the average of values.
func (a *MetricAggregator) AggregateAverage(values []float64) float64 {
	if len(values) == 0 {
		return 0
	}
	var sum float64
	for _, v := range values {
		sum += v
	}
	return sum / float64(len(values))
}

// AggregateMax returns the maximum value.
func (a *MetricAggregator) AggregateMax(values []float64) float64 {
	if len(values) == 0 {
		return 0
	}
	max := values[0]
	for _, v := range values[1:] {
		if v > max {
			max = v
		}
	}
	return max
}

// AggregateSum returns the sum of values.
func (a *MetricAggregator) AggregateSum(values []float64) float64 {
	var sum float64
	for _, v := range values {
		sum += v
	}
	return sum
}

// AggregatePercentile calculates a percentile of values.
func (a *MetricAggregator) AggregatePercentile(values []float64, p float64) float64 {
	if len(values) == 0 {
		return 0
	}

	sorted := make([]float64, len(values))
	copy(sorted, values)
	sortFloat64s(sorted)

	index := (p / 100.0) * float64(len(sorted)-1)
	lower := int(index)
	upper := lower + 1
	if upper >= len(sorted) {
		return sorted[len(sorted)-1]
	}

	weight := index - float64(lower)
	return sorted[lower]*(1-weight) + sorted[upper]*weight
}
