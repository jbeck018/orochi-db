// Package scaler provides horizontal scaling capabilities.
package scaler

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/orochi-db/orochi-cloud/autoscaler/internal/k8s"
	"github.com/orochi-db/orochi-cloud/autoscaler/internal/metrics"
)

// HorizontalScaler handles horizontal scaling operations.
type HorizontalScaler struct {
	mu              sync.RWMutex
	k8sClient       *k8s.Client
	metricsCollector *metrics.MetricsCollector
	policyEngine    *PolicyEngine
	autoscalerMetrics *metrics.AutoscalerMetrics
	eventRecorder   *ScalingEventRecorder
	evaluationInterval time.Duration
	stopCh          chan struct{}
	doneCh          chan struct{}
}

// HorizontalScalerConfig holds configuration for the horizontal scaler.
type HorizontalScalerConfig struct {
	EvaluationInterval time.Duration
}

// NewHorizontalScaler creates a new horizontal scaler.
func NewHorizontalScaler(
	k8sClient *k8s.Client,
	metricsCollector *metrics.MetricsCollector,
	policyEngine *PolicyEngine,
	autoscalerMetrics *metrics.AutoscalerMetrics,
	eventRecorder *ScalingEventRecorder,
	cfg HorizontalScalerConfig,
) *HorizontalScaler {
	return &HorizontalScaler{
		k8sClient:          k8sClient,
		metricsCollector:   metricsCollector,
		policyEngine:       policyEngine,
		autoscalerMetrics:  autoscalerMetrics,
		eventRecorder:      eventRecorder,
		evaluationInterval: cfg.EvaluationInterval,
	}
}

// Start begins the horizontal scaling loop.
func (s *HorizontalScaler) Start(ctx context.Context) error {
	s.stopCh = make(chan struct{})
	s.doneCh = make(chan struct{})

	go func() {
		defer close(s.doneCh)
		ticker := time.NewTicker(s.evaluationInterval)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				if err := s.evaluateAll(ctx); err != nil {
					fmt.Printf("horizontal scaling evaluation error: %v\n", err)
				}
			case <-s.stopCh:
				return
			case <-ctx.Done():
				return
			}
		}
	}()

	return nil
}

// Stop stops the horizontal scaler.
func (s *HorizontalScaler) Stop() error {
	if s.stopCh != nil {
		close(s.stopCh)
		<-s.doneCh
	}
	return nil
}

// evaluateAll evaluates scaling for all tracked clusters.
func (s *HorizontalScaler) evaluateAll(ctx context.Context) error {
	allMetrics, err := s.metricsCollector.CollectAll(ctx)
	if err != nil {
		return fmt.Errorf("failed to collect metrics: %w", err)
	}

	var wg sync.WaitGroup
	errCh := make(chan error, len(allMetrics))

	for key, clusterMetrics := range allMetrics {
		wg.Add(1)
		go func(key string, m *metrics.ClusterMetrics) {
			defer wg.Done()

			if err := s.evaluateCluster(ctx, m); err != nil {
				errCh <- fmt.Errorf("failed to evaluate %s: %w", key, err)
			}
		}(key, clusterMetrics)
	}

	wg.Wait()
	close(errCh)

	var errors []error
	for err := range errCh {
		errors = append(errors, err)
	}

	if len(errors) > 0 {
		return fmt.Errorf("some evaluations failed: %v", errors)
	}

	return nil
}

// evaluateCluster evaluates and potentially scales a single cluster.
func (s *HorizontalScaler) evaluateCluster(ctx context.Context, clusterMetrics *metrics.ClusterMetrics) error {
	clusterID := clusterMetrics.ClusterID
	namespace := clusterMetrics.Namespace

	// Get current status
	status, err := s.k8sClient.GetClusterStatus(ctx, clusterID, namespace)
	if err != nil {
		return fmt.Errorf("failed to get cluster status: %w", err)
	}

	// Record current state in metrics
	s.autoscalerMetrics.SetReplicas(clusterID, namespace, status.CurrentReplicas, status.DesiredReplicas)
	s.autoscalerMetrics.SetMetricValue(clusterID, namespace, "cpu_utilization", clusterMetrics.CPUUtilization)
	s.autoscalerMetrics.SetMetricValue(clusterID, namespace, "memory_utilization", clusterMetrics.MemoryUtilization)
	s.autoscalerMetrics.SetMetricValue(clusterID, namespace, "active_connections", float64(clusterMetrics.ActiveConnections))
	s.autoscalerMetrics.SetMetricValue(clusterID, namespace, "query_latency_p95", clusterMetrics.QueryLatencyP95Ms)

	// Evaluate policy
	startTime := time.Now()
	decision, err := s.policyEngine.Evaluate(ctx, clusterID, namespace, clusterMetrics, status.CurrentReplicas)
	evaluationDuration := time.Since(startTime)

	s.autoscalerMetrics.EvaluationDuration.WithLabelValues(clusterID, namespace).Observe(evaluationDuration.Seconds())

	if err != nil {
		return fmt.Errorf("policy evaluation failed: %w", err)
	}

	// Update cooldown status in metrics
	cooldowns := s.policyEngine.GetCooldowns()
	s.autoscalerMetrics.SetCooldownActive(clusterID, namespace, "up", !cooldowns.CanScaleUp(clusterID, namespace))
	s.autoscalerMetrics.SetCooldownActive(clusterID, namespace, "down", !cooldowns.CanScaleDown(clusterID, namespace))

	// No scaling needed
	if decision.Direction == ScalingDirectionNone {
		return nil
	}

	// Record decision
	s.autoscalerMetrics.RecordScalingDecision(clusterID, namespace, string(decision.Direction), string(decision.Type))

	// Execute scaling
	if err := s.scale(ctx, decision); err != nil {
		s.autoscalerMetrics.RecordScalingOperation(clusterID, namespace, string(decision.Direction), string(decision.Type), "failed", time.Since(startTime))
		s.eventRecorder.RecordFailure(decision, err)
		return fmt.Errorf("scaling failed: %w", err)
	}

	s.autoscalerMetrics.RecordScalingOperation(clusterID, namespace, string(decision.Direction), string(decision.Type), "success", time.Since(startTime))
	s.eventRecorder.RecordSuccess(decision)

	return nil
}

// scale executes a horizontal scaling decision.
func (s *HorizontalScaler) scale(ctx context.Context, decision *ScalingDecision) error {
	clusterID := decision.ClusterID
	namespace := decision.Namespace
	targetReplicas := decision.ToReplicas

	fmt.Printf("Scaling %s/%s from %d to %d replicas (reason: %s)\n",
		namespace, clusterID, decision.FromReplicas, targetReplicas, decision.Reason)

	// Try StatefulSet first
	err := s.k8sClient.ScaleStatefulSet(ctx, clusterID, namespace, targetReplicas)
	if err != nil {
		// Fall back to Deployment
		err = s.k8sClient.ScaleDeployment(ctx, clusterID, namespace, targetReplicas)
		if err != nil {
			return fmt.Errorf("failed to scale: %w", err)
		}
	}

	// Update cooldowns
	cooldowns := s.policyEngine.GetCooldowns()
	policy, _ := s.policyEngine.GetPolicy(clusterID, namespace)

	if policy != nil && policy.Horizontal != nil {
		if decision.Direction == ScalingDirectionUp {
			cooldowns.SetScaleUpCooldown(clusterID, namespace, time.Now().Add(policy.Horizontal.ScaleUpCooldown))
		} else if decision.Direction == ScalingDirectionDown {
			cooldowns.SetScaleDownCooldown(clusterID, namespace, time.Now().Add(policy.Horizontal.ScaleDownCooldown))
		}
	}

	// Add scaling annotation
	annotations := map[string]string{
		"autoscaler.orochi.io/last-scale-time":   time.Now().Format(time.RFC3339),
		"autoscaler.orochi.io/last-scale-reason": decision.Reason,
	}
	_ = s.k8sClient.AnnotateResource(ctx, "StatefulSet", clusterID, namespace, annotations)

	return nil
}

// Scale performs a manual scaling operation.
func (s *HorizontalScaler) Scale(ctx context.Context, clusterID, namespace string, targetReplicas int32, force bool) error {
	// Get current status
	status, err := s.k8sClient.GetClusterStatus(ctx, clusterID, namespace)
	if err != nil {
		return fmt.Errorf("failed to get cluster status: %w", err)
	}

	// Check cooldowns unless force is true
	if !force {
		cooldowns := s.policyEngine.GetCooldowns()
		if targetReplicas > status.CurrentReplicas && !cooldowns.CanScaleUp(clusterID, namespace) {
			remaining := cooldowns.TimeUntilScaleUp(clusterID, namespace)
			return fmt.Errorf("scale-up cooldown active, %v remaining", remaining)
		}
		if targetReplicas < status.CurrentReplicas && !cooldowns.CanScaleDown(clusterID, namespace) {
			remaining := cooldowns.TimeUntilScaleDown(clusterID, namespace)
			return fmt.Errorf("scale-down cooldown active, %v remaining", remaining)
		}
	}

	// Check policy bounds
	policy, ok := s.policyEngine.GetPolicy(clusterID, namespace)
	if ok && policy.Horizontal != nil {
		if targetReplicas < policy.Horizontal.MinReplicas {
			return fmt.Errorf("target replicas %d is below minimum %d", targetReplicas, policy.Horizontal.MinReplicas)
		}
		if targetReplicas > policy.Horizontal.MaxReplicas {
			return fmt.Errorf("target replicas %d is above maximum %d", targetReplicas, policy.Horizontal.MaxReplicas)
		}
	}

	// Create decision
	direction := ScalingDirectionNone
	if targetReplicas > status.CurrentReplicas {
		direction = ScalingDirectionUp
	} else if targetReplicas < status.CurrentReplicas {
		direction = ScalingDirectionDown
	}

	decision := &ScalingDecision{
		ClusterID:    clusterID,
		Namespace:    namespace,
		Timestamp:    time.Now(),
		Direction:    direction,
		Type:         ScalingTypeHorizontal,
		FromReplicas: status.CurrentReplicas,
		ToReplicas:   targetReplicas,
		Reason:       "manual scaling request",
		Trigger:      "manual",
		Confidence:   1.0,
	}

	return s.scale(ctx, decision)
}

// ScalingEventRecorder records scaling events for history.
type ScalingEventRecorder struct {
	mu     sync.RWMutex
	events map[string][]ScalingEvent
	maxAge time.Duration
	maxCount int
}

// ScalingEvent represents a recorded scaling event.
type ScalingEvent struct {
	ID            string
	ClusterID     string
	Namespace     string
	Timestamp     time.Time
	Type          ScalingType
	Direction     ScalingDirection
	FromReplicas  int32
	ToReplicas    int32
	FromResources *ResourceSpec
	ToResources   *ResourceSpec
	Trigger       string
	Success       bool
	ErrorMessage  string
	Metrics       *metrics.ClusterMetrics
}

// NewScalingEventRecorder creates a new event recorder.
func NewScalingEventRecorder(maxAge time.Duration, maxCount int) *ScalingEventRecorder {
	return &ScalingEventRecorder{
		events:   make(map[string][]ScalingEvent),
		maxAge:   maxAge,
		maxCount: maxCount,
	}
}

func (r *ScalingEventRecorder) clusterKey(clusterID, namespace string) string {
	return fmt.Sprintf("%s/%s", namespace, clusterID)
}

// RecordSuccess records a successful scaling event.
func (r *ScalingEventRecorder) RecordSuccess(decision *ScalingDecision) {
	r.record(decision, true, "")
}

// RecordFailure records a failed scaling event.
func (r *ScalingEventRecorder) RecordFailure(decision *ScalingDecision, err error) {
	r.record(decision, false, err.Error())
}

func (r *ScalingEventRecorder) record(decision *ScalingDecision, success bool, errorMsg string) {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := r.clusterKey(decision.ClusterID, decision.Namespace)

	event := ScalingEvent{
		ID:           fmt.Sprintf("%s-%d", key, time.Now().UnixNano()),
		ClusterID:    decision.ClusterID,
		Namespace:    decision.Namespace,
		Timestamp:    decision.Timestamp,
		Type:         decision.Type,
		Direction:    decision.Direction,
		FromReplicas: decision.FromReplicas,
		ToReplicas:   decision.ToReplicas,
		Trigger:      decision.Trigger,
		Success:      success,
		ErrorMessage: errorMsg,
	}

	events := r.events[key]
	events = append(events, event)

	// Prune old events
	cutoff := time.Now().Add(-r.maxAge)
	var validEvents []ScalingEvent
	for _, e := range events {
		if e.Timestamp.After(cutoff) {
			validEvents = append(validEvents, e)
		}
	}

	// Enforce max count
	if len(validEvents) > r.maxCount {
		validEvents = validEvents[len(validEvents)-r.maxCount:]
	}

	r.events[key] = validEvents
}

// GetEvents retrieves events for a cluster.
func (r *ScalingEventRecorder) GetEvents(clusterID, namespace string, limit int) []ScalingEvent {
	r.mu.RLock()
	defer r.mu.RUnlock()

	key := r.clusterKey(clusterID, namespace)
	events := r.events[key]

	if limit > 0 && len(events) > limit {
		events = events[len(events)-limit:]
	}

	// Return a copy
	result := make([]ScalingEvent, len(events))
	copy(result, events)
	return result
}

// GetEventsInRange retrieves events within a time range.
func (r *ScalingEventRecorder) GetEventsInRange(clusterID, namespace string, start, end time.Time) []ScalingEvent {
	r.mu.RLock()
	defer r.mu.RUnlock()

	key := r.clusterKey(clusterID, namespace)
	events := r.events[key]

	var result []ScalingEvent
	for _, e := range events {
		if (e.Timestamp.After(start) || e.Timestamp.Equal(start)) &&
			(e.Timestamp.Before(end) || e.Timestamp.Equal(end)) {
			result = append(result, e)
		}
	}

	return result
}
