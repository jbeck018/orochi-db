// Package scaler provides vertical scaling capabilities.
package scaler

import (
	"context"
	"fmt"
	"sync"
	"time"

	"k8s.io/apimachinery/pkg/api/resource"

	"github.com/orochi-db/orochi-cloud/autoscaler/internal/k8s"
	"github.com/orochi-db/orochi-cloud/autoscaler/internal/metrics"
)

// VerticalScaler handles vertical scaling operations.
type VerticalScaler struct {
	mu                 sync.RWMutex
	k8sClient          *k8s.Client
	metricsCollector   *metrics.MetricsCollector
	policyEngine       *PolicyEngine
	autoscalerMetrics  *metrics.AutoscalerMetrics
	eventRecorder      *ScalingEventRecorder
	evaluationInterval time.Duration
	stopCh             chan struct{}
	doneCh             chan struct{}
	recommendations    map[string]*VerticalRecommendation
}

// VerticalRecommendation holds a vertical scaling recommendation.
type VerticalRecommendation struct {
	ClusterID          string
	Namespace          string
	Timestamp          time.Time
	CurrentCPU         resource.Quantity
	CurrentMemory      resource.Quantity
	RecommendedCPU     resource.Quantity
	RecommendedMemory  resource.Quantity
	CPUUtilization     float64
	MemoryUtilization  float64
	Confidence         float64
	Reason             string
}

// VerticalScalerConfig holds configuration for the vertical scaler.
type VerticalScalerConfig struct {
	EvaluationInterval time.Duration
	CPUIncrement       resource.Quantity
	MemoryIncrement    resource.Quantity
}

// NewVerticalScaler creates a new vertical scaler.
func NewVerticalScaler(
	k8sClient *k8s.Client,
	metricsCollector *metrics.MetricsCollector,
	policyEngine *PolicyEngine,
	autoscalerMetrics *metrics.AutoscalerMetrics,
	eventRecorder *ScalingEventRecorder,
	cfg VerticalScalerConfig,
) *VerticalScaler {
	return &VerticalScaler{
		k8sClient:          k8sClient,
		metricsCollector:   metricsCollector,
		policyEngine:       policyEngine,
		autoscalerMetrics:  autoscalerMetrics,
		eventRecorder:      eventRecorder,
		evaluationInterval: cfg.EvaluationInterval,
		recommendations:    make(map[string]*VerticalRecommendation),
	}
}

// Start begins the vertical scaling loop.
func (s *VerticalScaler) Start(ctx context.Context) error {
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
					fmt.Printf("vertical scaling evaluation error: %v\n", err)
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

// Stop stops the vertical scaler.
func (s *VerticalScaler) Stop() error {
	if s.stopCh != nil {
		close(s.stopCh)
		<-s.doneCh
	}
	return nil
}

// evaluateAll evaluates vertical scaling for all tracked clusters.
func (s *VerticalScaler) evaluateAll(ctx context.Context) error {
	allMetrics, err := s.metricsCollector.CollectAll(ctx)
	if err != nil {
		return fmt.Errorf("failed to collect metrics: %w", err)
	}

	for key, clusterMetrics := range allMetrics {
		if err := s.evaluateCluster(ctx, clusterMetrics); err != nil {
			fmt.Printf("failed to evaluate vertical scaling for %s: %v\n", key, err)
		}
	}

	return nil
}

// evaluateCluster evaluates vertical scaling for a single cluster.
func (s *VerticalScaler) evaluateCluster(ctx context.Context, clusterMetrics *metrics.ClusterMetrics) error {
	clusterID := clusterMetrics.ClusterID
	namespace := clusterMetrics.Namespace

	// Get policy
	policy, ok := s.policyEngine.GetPolicy(clusterID, namespace)
	if !ok || policy.Vertical == nil || !policy.Vertical.Enabled {
		return nil
	}

	// Check cooldown
	cooldowns := s.policyEngine.GetCooldowns()
	if !cooldowns.CanScaleVertical(clusterID, namespace) {
		return nil
	}

	// Get current status
	status, err := s.k8sClient.GetClusterStatus(ctx, clusterID, namespace)
	if err != nil {
		return fmt.Errorf("failed to get cluster status: %w", err)
	}

	if status.Resources == nil {
		return nil
	}

	// Calculate recommendation
	recommendation := s.calculateRecommendation(policy.Vertical, status.Resources, clusterMetrics)
	if recommendation == nil {
		return nil
	}

	// Store recommendation
	key := fmt.Sprintf("%s/%s", namespace, clusterID)
	s.mu.Lock()
	s.recommendations[key] = recommendation
	s.mu.Unlock()

	// Apply based on mode
	switch policy.Vertical.Mode {
	case VerticalScaleModeAuto, VerticalScaleModeRecreate:
		return s.applyRecommendation(ctx, recommendation, policy.Vertical)
	case VerticalScaleModeInitial:
		// Only logged, not applied automatically
		fmt.Printf("Vertical scaling recommendation for %s/%s: CPU %s -> %s, Memory %s -> %s\n",
			namespace, clusterID,
			recommendation.CurrentCPU.String(), recommendation.RecommendedCPU.String(),
			recommendation.CurrentMemory.String(), recommendation.RecommendedMemory.String())
		return nil
	default:
		return nil
	}
}

// calculateRecommendation calculates a vertical scaling recommendation.
func (s *VerticalScaler) calculateRecommendation(
	policy *VerticalPolicy,
	currentResources *k8s.ResourceRequirements,
	clusterMetrics *metrics.ClusterMetrics,
) *VerticalRecommendation {

	cpuUtil := clusterMetrics.CPUUtilization
	memUtil := clusterMetrics.MemoryUtilization

	// Parse policy bounds
	minCPU := resource.MustParse(policy.MinCPU)
	maxCPU := resource.MustParse(policy.MaxCPU)
	minMemory := resource.MustParse(policy.MinMemory)
	maxMemory := resource.MustParse(policy.MaxMemory)

	// Calculate recommended CPU
	recommendedCPU := currentResources.CPURequest
	if cpuUtil > policy.TargetCPUUtilization*1.1 {
		// Need more CPU
		currentMillis := currentResources.CPURequest.MilliValue()
		targetMillis := int64(float64(currentMillis) * (cpuUtil / policy.TargetCPUUtilization))
		recommendedCPU = *resource.NewMilliQuantity(targetMillis, resource.DecimalSI)
	} else if cpuUtil < policy.TargetCPUUtilization*0.5 && cpuUtil > 0 {
		// Can reduce CPU
		currentMillis := currentResources.CPURequest.MilliValue()
		targetMillis := int64(float64(currentMillis) * (cpuUtil / policy.TargetCPUUtilization) * 1.2) // 20% buffer
		recommendedCPU = *resource.NewMilliQuantity(targetMillis, resource.DecimalSI)
	}

	// Enforce CPU bounds
	if recommendedCPU.Cmp(minCPU) < 0 {
		recommendedCPU = minCPU
	}
	if recommendedCPU.Cmp(maxCPU) > 0 {
		recommendedCPU = maxCPU
	}

	// Calculate recommended Memory
	recommendedMemory := currentResources.MemoryRequest
	if memUtil > policy.TargetMemoryUtilization*1.1 {
		// Need more memory
		currentBytes := currentResources.MemoryRequest.Value()
		targetBytes := int64(float64(currentBytes) * (memUtil / policy.TargetMemoryUtilization))
		recommendedMemory = *resource.NewQuantity(targetBytes, resource.BinarySI)
	} else if memUtil < policy.TargetMemoryUtilization*0.5 && memUtil > 0 {
		// Can reduce memory
		currentBytes := currentResources.MemoryRequest.Value()
		targetBytes := int64(float64(currentBytes) * (memUtil / policy.TargetMemoryUtilization) * 1.2) // 20% buffer
		recommendedMemory = *resource.NewQuantity(targetBytes, resource.BinarySI)
	}

	// Enforce Memory bounds
	if recommendedMemory.Cmp(minMemory) < 0 {
		recommendedMemory = minMemory
	}
	if recommendedMemory.Cmp(maxMemory) > 0 {
		recommendedMemory = maxMemory
	}

	// Check if change is significant (at least 10%)
	cpuChange := float64(recommendedCPU.MilliValue()-currentResources.CPURequest.MilliValue()) / float64(currentResources.CPURequest.MilliValue())
	memChange := float64(recommendedMemory.Value()-currentResources.MemoryRequest.Value()) / float64(currentResources.MemoryRequest.Value())

	if abs(cpuChange) < 0.10 && abs(memChange) < 0.10 {
		return nil // Change not significant enough
	}

	// Determine reason
	var reason string
	if cpuUtil > policy.TargetCPUUtilization {
		reason = fmt.Sprintf("CPU utilization %.1f%% exceeds target %.1f%%", cpuUtil, policy.TargetCPUUtilization)
	} else if memUtil > policy.TargetMemoryUtilization {
		reason = fmt.Sprintf("Memory utilization %.1f%% exceeds target %.1f%%", memUtil, policy.TargetMemoryUtilization)
	} else {
		reason = fmt.Sprintf("Resource utilization (CPU: %.1f%%, Memory: %.1f%%) allows downsizing", cpuUtil, memUtil)
	}

	return &VerticalRecommendation{
		ClusterID:         clusterMetrics.ClusterID,
		Namespace:         clusterMetrics.Namespace,
		Timestamp:         time.Now(),
		CurrentCPU:        currentResources.CPURequest,
		CurrentMemory:     currentResources.MemoryRequest,
		RecommendedCPU:    recommendedCPU,
		RecommendedMemory: recommendedMemory,
		CPUUtilization:    cpuUtil,
		MemoryUtilization: memUtil,
		Confidence:        0.8, // Could be improved with historical data
		Reason:            reason,
	}
}

func abs(x float64) float64 {
	if x < 0 {
		return -x
	}
	return x
}

// applyRecommendation applies a vertical scaling recommendation.
func (s *VerticalScaler) applyRecommendation(ctx context.Context, rec *VerticalRecommendation, policy *VerticalPolicy) error {
	fmt.Printf("Applying vertical scaling for %s/%s: CPU %s -> %s, Memory %s -> %s (reason: %s)\n",
		rec.Namespace, rec.ClusterID,
		rec.CurrentCPU.String(), rec.RecommendedCPU.String(),
		rec.CurrentMemory.String(), rec.RecommendedMemory.String(),
		rec.Reason)

	newResources := &k8s.ResourceRequirements{
		CPURequest:    rec.RecommendedCPU,
		CPULimit:      rec.RecommendedCPU,
		MemoryRequest: rec.RecommendedMemory,
		MemoryLimit:   rec.RecommendedMemory,
	}

	// Try StatefulSet first
	err := s.k8sClient.UpdateStatefulSetResources(ctx, rec.ClusterID, rec.Namespace, newResources)
	if err != nil {
		// Fall back to Deployment
		err = s.k8sClient.UpdateDeploymentResources(ctx, rec.ClusterID, rec.Namespace, newResources)
		if err != nil {
			return fmt.Errorf("failed to update resources: %w", err)
		}
	}

	// Update cooldown
	cooldowns := s.policyEngine.GetCooldowns()
	cooldowns.SetVerticalCooldown(rec.ClusterID, rec.Namespace, time.Now().Add(policy.Cooldown))

	// Record event
	direction := ScalingDirectionUp
	if rec.RecommendedCPU.Cmp(rec.CurrentCPU) < 0 && rec.RecommendedMemory.Cmp(rec.CurrentMemory) <= 0 {
		direction = ScalingDirectionDown
	}

	decision := &ScalingDecision{
		ClusterID: rec.ClusterID,
		Namespace: rec.Namespace,
		Timestamp: rec.Timestamp,
		Direction: direction,
		Type:      ScalingTypeVertical,
		FromResources: &ResourceSpec{
			CPURequest:    rec.CurrentCPU.String(),
			MemoryRequest: rec.CurrentMemory.String(),
		},
		ToResources: &ResourceSpec{
			CPURequest:    rec.RecommendedCPU.String(),
			MemoryRequest: rec.RecommendedMemory.String(),
		},
		Reason:     rec.Reason,
		Trigger:    "vertical_autoscaler",
		Confidence: rec.Confidence,
	}

	s.eventRecorder.RecordSuccess(decision)

	// Record metrics
	s.autoscalerMetrics.RecordScalingDecision(rec.ClusterID, rec.Namespace, string(direction), string(ScalingTypeVertical))
	s.autoscalerMetrics.RecordScalingOperation(rec.ClusterID, rec.Namespace, string(direction), string(ScalingTypeVertical), "success", 0)

	return nil
}

// Scale performs a manual vertical scaling operation.
func (s *VerticalScaler) Scale(ctx context.Context, clusterID, namespace string, resources *ResourceSpec, force bool) error {
	// Get current status
	status, err := s.k8sClient.GetClusterStatus(ctx, clusterID, namespace)
	if err != nil {
		return fmt.Errorf("failed to get cluster status: %w", err)
	}

	// Check cooldown unless force is true
	if !force {
		cooldowns := s.policyEngine.GetCooldowns()
		if !cooldowns.CanScaleVertical(clusterID, namespace) {
			return fmt.Errorf("vertical scaling cooldown active")
		}
	}

	// Validate resources
	policy, ok := s.policyEngine.GetPolicy(clusterID, namespace)
	if ok && policy.Vertical != nil {
		cpuReq := resource.MustParse(resources.CPURequest)
		memReq := resource.MustParse(resources.MemoryRequest)
		minCPU := resource.MustParse(policy.Vertical.MinCPU)
		maxCPU := resource.MustParse(policy.Vertical.MaxCPU)
		minMemory := resource.MustParse(policy.Vertical.MinMemory)
		maxMemory := resource.MustParse(policy.Vertical.MaxMemory)

		if cpuReq.Cmp(minCPU) < 0 {
			return fmt.Errorf("CPU request %s is below minimum %s", resources.CPURequest, policy.Vertical.MinCPU)
		}
		if cpuReq.Cmp(maxCPU) > 0 {
			return fmt.Errorf("CPU request %s is above maximum %s", resources.CPURequest, policy.Vertical.MaxCPU)
		}
		if memReq.Cmp(minMemory) < 0 {
			return fmt.Errorf("Memory request %s is below minimum %s", resources.MemoryRequest, policy.Vertical.MinMemory)
		}
		if memReq.Cmp(maxMemory) > 0 {
			return fmt.Errorf("Memory request %s is above maximum %s", resources.MemoryRequest, policy.Vertical.MaxMemory)
		}
	}

	// Parse resources
	cpuReq := resource.MustParse(resources.CPURequest)
	cpuLimit := cpuReq
	if resources.CPULimit != "" {
		cpuLimit = resource.MustParse(resources.CPULimit)
	}
	memReq := resource.MustParse(resources.MemoryRequest)
	memLimit := memReq
	if resources.MemoryLimit != "" {
		memLimit = resource.MustParse(resources.MemoryLimit)
	}

	newResources := &k8s.ResourceRequirements{
		CPURequest:    cpuReq,
		CPULimit:      cpuLimit,
		MemoryRequest: memReq,
		MemoryLimit:   memLimit,
	}

	// Apply
	err = s.k8sClient.UpdateStatefulSetResources(ctx, clusterID, namespace, newResources)
	if err != nil {
		err = s.k8sClient.UpdateDeploymentResources(ctx, clusterID, namespace, newResources)
		if err != nil {
			return fmt.Errorf("failed to update resources: %w", err)
		}
	}

	// Update cooldown
	if policy != nil && policy.Vertical != nil {
		cooldowns := s.policyEngine.GetCooldowns()
		cooldowns.SetVerticalCooldown(clusterID, namespace, time.Now().Add(policy.Vertical.Cooldown))
	}

	// Record event
	var fromResources *ResourceSpec
	if status.Resources != nil {
		fromResources = &ResourceSpec{
			CPURequest:    status.Resources.CPURequest.String(),
			CPULimit:      status.Resources.CPULimit.String(),
			MemoryRequest: status.Resources.MemoryRequest.String(),
			MemoryLimit:   status.Resources.MemoryLimit.String(),
		}
	}

	decision := &ScalingDecision{
		ClusterID:     clusterID,
		Namespace:     namespace,
		Timestamp:     time.Now(),
		Direction:     ScalingDirectionNone, // Manual
		Type:          ScalingTypeVertical,
		FromResources: fromResources,
		ToResources:   resources,
		Reason:        "manual scaling request",
		Trigger:       "manual",
		Confidence:    1.0,
	}

	s.eventRecorder.RecordSuccess(decision)

	return nil
}

// GetRecommendation returns the latest recommendation for a cluster.
func (s *VerticalScaler) GetRecommendation(clusterID, namespace string) (*VerticalRecommendation, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := fmt.Sprintf("%s/%s", namespace, clusterID)
	rec, ok := s.recommendations[key]
	return rec, ok
}
