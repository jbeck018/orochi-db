// Package scaler provides scaling logic for the autoscaler.
package scaler

import (
	"context"
	"fmt"
	"math"
	"sort"
	"sync"
	"time"

	"github.com/orochi-db/orochi-db/services/autoscaler/internal/metrics"
)

// ScalingDecision represents a scaling decision made by the policy engine.
type ScalingDecision struct {
	ClusterID     string
	Namespace     string
	Timestamp     time.Time
	Direction     ScalingDirection
	Type          ScalingType
	FromReplicas  int32
	ToReplicas    int32
	FromResources *ResourceSpec
	ToResources   *ResourceSpec
	Reason        string
	Trigger       string
	Confidence    float64
}

// ScalingDirection indicates the direction of scaling.
type ScalingDirection string

const (
	ScalingDirectionNone ScalingDirection = "none"
	ScalingDirectionUp   ScalingDirection = "up"
	ScalingDirectionDown ScalingDirection = "down"
	ScalingDirectionOut  ScalingDirection = "out"
	ScalingDirectionIn   ScalingDirection = "in"
)

// ScalingType indicates the type of scaling.
type ScalingType string

const (
	ScalingTypeHorizontal ScalingType = "horizontal"
	ScalingTypeVertical   ScalingType = "vertical"
)

// ResourceSpec represents resource specifications.
type ResourceSpec struct {
	CPURequest    string
	CPULimit      string
	MemoryRequest string
	MemoryLimit   string
}

// Policy defines a scaling policy configuration.
type Policy struct {
	ClusterID  string
	Namespace  string
	Enabled    bool
	Horizontal *HorizontalPolicy
	Vertical   *VerticalPolicy
	Rules      []ScalingRule
	Predictive *PredictivePolicy
}

// HorizontalPolicy defines horizontal scaling configuration.
type HorizontalPolicy struct {
	Enabled                 bool
	MinReplicas             int32
	MaxReplicas             int32
	TargetCPUUtilization    float64
	TargetMemoryUtilization float64
	TargetConnectionsPerPod int64
	TargetQueryLatencyMs    float64
	ScaleUpCooldown         time.Duration
	ScaleDownCooldown       time.Duration
	ScaleUpStep             int32
	ScaleDownStep           int32
	StabilizationWindow     time.Duration
}

// VerticalPolicy defines vertical scaling configuration.
type VerticalPolicy struct {
	Enabled                 bool
	MinCPU                  string
	MaxCPU                  string
	MinMemory               string
	MaxMemory               string
	TargetCPUUtilization    float64
	TargetMemoryUtilization float64
	Mode                    VerticalScaleMode
	Cooldown                time.Duration
}

// VerticalScaleMode defines how vertical scaling is applied.
type VerticalScaleMode string

const (
	VerticalScaleModeOff      VerticalScaleMode = "off"
	VerticalScaleModeInitial  VerticalScaleMode = "initial"
	VerticalScaleModeAuto     VerticalScaleMode = "auto"
	VerticalScaleModeRecreate VerticalScaleMode = "recreate"
)

// ScalingRule defines a custom scaling rule.
type ScalingRule struct {
	Name            string
	MetricName      string
	Operator        RuleOperator
	Threshold       float64
	Duration        time.Duration
	Action          RuleAction
	Cooldown        time.Duration
	Priority        int
	conditionMetAt  *time.Time
	lastTriggeredAt *time.Time
}

// RuleOperator defines comparison operators.
type RuleOperator string

const (
	RuleOperatorGT  RuleOperator = "gt"
	RuleOperatorGTE RuleOperator = "gte"
	RuleOperatorLT  RuleOperator = "lt"
	RuleOperatorLTE RuleOperator = "lte"
	RuleOperatorEQ  RuleOperator = "eq"
)

// RuleAction defines an action to take when a rule triggers.
type RuleAction struct {
	Type  ActionType
	Value int32
}

// ActionType defines types of actions.
type ActionType string

const (
	ActionTypeAddReplicas    ActionType = "add_replicas"
	ActionTypeRemoveReplicas ActionType = "remove_replicas"
	ActionTypeSetReplicas    ActionType = "set_replicas"
	ActionTypeAddPercent     ActionType = "add_percent"
	ActionTypeRemovePercent  ActionType = "remove_percent"
)

// PredictivePolicy defines predictive scaling configuration.
type PredictivePolicy struct {
	Enabled             bool
	LookAheadSeconds    int
	ConfidenceThreshold float64
	ScheduledEvents     []ScheduledEvent
}

// ScheduledEvent defines a scheduled scaling event.
type ScheduledEvent struct {
	Name            string
	CronExpression  string
	TargetReplicas  int32
	TargetResources *ResourceSpec
	DurationMinutes int
}

// CooldownTracker tracks cooldown periods for scaling operations.
type CooldownTracker struct {
	mu           sync.RWMutex
	scaleUp      map[string]time.Time
	scaleDown    map[string]time.Time
	vertical     map[string]time.Time
	ruleSpecific map[string]time.Time
}

// NewCooldownTracker creates a new cooldown tracker.
func NewCooldownTracker() *CooldownTracker {
	return &CooldownTracker{
		scaleUp:      make(map[string]time.Time),
		scaleDown:    make(map[string]time.Time),
		vertical:     make(map[string]time.Time),
		ruleSpecific: make(map[string]time.Time),
	}
}

func (c *CooldownTracker) clusterKey(clusterID, namespace string) string {
	return fmt.Sprintf("%s/%s", namespace, clusterID)
}

// SetScaleUpCooldown sets the scale-up cooldown for a cluster.
func (c *CooldownTracker) SetScaleUpCooldown(clusterID, namespace string, until time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.scaleUp[c.clusterKey(clusterID, namespace)] = until
}

// SetScaleDownCooldown sets the scale-down cooldown for a cluster.
func (c *CooldownTracker) SetScaleDownCooldown(clusterID, namespace string, until time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.scaleDown[c.clusterKey(clusterID, namespace)] = until
}

// SetVerticalCooldown sets the vertical scaling cooldown for a cluster.
func (c *CooldownTracker) SetVerticalCooldown(clusterID, namespace string, until time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.vertical[c.clusterKey(clusterID, namespace)] = until
}

// SetRuleCooldown sets the cooldown for a specific rule.
func (c *CooldownTracker) SetRuleCooldown(clusterID, namespace, ruleName string, until time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	key := fmt.Sprintf("%s/%s/%s", namespace, clusterID, ruleName)
	c.ruleSpecific[key] = until
}

// CanScaleUp checks if scale-up is allowed.
func (c *CooldownTracker) CanScaleUp(clusterID, namespace string) bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	until, ok := c.scaleUp[c.clusterKey(clusterID, namespace)]
	if !ok {
		return true
	}
	return time.Now().After(until)
}

// CanScaleDown checks if scale-down is allowed.
func (c *CooldownTracker) CanScaleDown(clusterID, namespace string) bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	until, ok := c.scaleDown[c.clusterKey(clusterID, namespace)]
	if !ok {
		return true
	}
	return time.Now().After(until)
}

// CanScaleVertical checks if vertical scaling is allowed.
func (c *CooldownTracker) CanScaleVertical(clusterID, namespace string) bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	until, ok := c.vertical[c.clusterKey(clusterID, namespace)]
	if !ok {
		return true
	}
	return time.Now().After(until)
}

// CanTriggerRule checks if a rule can be triggered.
func (c *CooldownTracker) CanTriggerRule(clusterID, namespace, ruleName string) bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	key := fmt.Sprintf("%s/%s/%s", namespace, clusterID, ruleName)
	until, ok := c.ruleSpecific[key]
	if !ok {
		return true
	}
	return time.Now().After(until)
}

// TimeUntilScaleUp returns time until scale-up is allowed.
func (c *CooldownTracker) TimeUntilScaleUp(clusterID, namespace string) time.Duration {
	c.mu.RLock()
	defer c.mu.RUnlock()
	until, ok := c.scaleUp[c.clusterKey(clusterID, namespace)]
	if !ok {
		return 0
	}
	remaining := time.Until(until)
	if remaining < 0 {
		return 0
	}
	return remaining
}

// TimeUntilScaleDown returns time until scale-down is allowed.
func (c *CooldownTracker) TimeUntilScaleDown(clusterID, namespace string) time.Duration {
	c.mu.RLock()
	defer c.mu.RUnlock()
	until, ok := c.scaleDown[c.clusterKey(clusterID, namespace)]
	if !ok {
		return 0
	}
	remaining := time.Until(until)
	if remaining < 0 {
		return 0
	}
	return remaining
}

// PolicyEngine evaluates scaling policies.
type PolicyEngine struct {
	mu             sync.RWMutex
	policies       map[string]*Policy
	cooldowns      *CooldownTracker
	metricsHistory map[string]*metrics.MetricHistory
	stabilization  map[string]*StabilizationWindow
}

// StabilizationWindow tracks metric values for stabilization.
type StabilizationWindow struct {
	mu       sync.Mutex
	values   []stabilizationSample
	duration time.Duration
}

type stabilizationSample struct {
	timestamp time.Time
	replicas  int32
}

// NewPolicyEngine creates a new policy engine.
func NewPolicyEngine() *PolicyEngine {
	return &PolicyEngine{
		policies:       make(map[string]*Policy),
		cooldowns:      NewCooldownTracker(),
		metricsHistory: make(map[string]*metrics.MetricHistory),
		stabilization:  make(map[string]*StabilizationWindow),
	}
}

func (e *PolicyEngine) clusterKey(clusterID, namespace string) string {
	return fmt.Sprintf("%s/%s", namespace, clusterID)
}

// SetPolicy sets the policy for a cluster.
func (e *PolicyEngine) SetPolicy(policy *Policy) {
	e.mu.Lock()
	defer e.mu.Unlock()
	key := e.clusterKey(policy.ClusterID, policy.Namespace)
	e.policies[key] = policy

	// Initialize stabilization window if needed
	if policy.Horizontal != nil && policy.Horizontal.StabilizationWindow > 0 {
		e.stabilization[key] = &StabilizationWindow{
			duration: policy.Horizontal.StabilizationWindow,
		}
	}
}

// GetPolicy returns the policy for a cluster.
func (e *PolicyEngine) GetPolicy(clusterID, namespace string) (*Policy, bool) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	key := e.clusterKey(clusterID, namespace)
	policy, ok := e.policies[key]
	return policy, ok
}

// RemovePolicy removes the policy for a cluster.
func (e *PolicyEngine) RemovePolicy(clusterID, namespace string) {
	e.mu.Lock()
	defer e.mu.Unlock()
	key := e.clusterKey(clusterID, namespace)
	delete(e.policies, key)
	delete(e.metricsHistory, key)
	delete(e.stabilization, key)
}

// GetCooldowns returns the cooldown tracker.
func (e *PolicyEngine) GetCooldowns() *CooldownTracker {
	return e.cooldowns
}

// Evaluate evaluates the scaling policy for a cluster and returns a decision.
func (e *PolicyEngine) Evaluate(ctx context.Context, clusterID, namespace string, currentMetrics *metrics.ClusterMetrics, currentReplicas int32) (*ScalingDecision, error) {
	// Guard against nil metrics
	if currentMetrics == nil {
		return nil, fmt.Errorf("metrics cannot be nil for cluster %s/%s", namespace, clusterID)
	}

	policy, ok := e.GetPolicy(clusterID, namespace)
	if !ok {
		return nil, fmt.Errorf("no policy found for cluster %s/%s", namespace, clusterID)
	}

	if !policy.Enabled {
		return &ScalingDecision{
			ClusterID:    clusterID,
			Namespace:    namespace,
			Timestamp:    time.Now(),
			Direction:    ScalingDirectionNone,
			FromReplicas: currentReplicas,
			ToReplicas:   currentReplicas,
			Reason:       "scaling disabled",
		}, nil
	}

	// Check custom rules first (highest priority)
	if len(policy.Rules) > 0 {
		decision := e.evaluateRules(policy, currentMetrics, currentReplicas)
		if decision != nil && decision.Direction != ScalingDirectionNone {
			return decision, nil
		}
	}

	// Evaluate horizontal scaling
	if policy.Horizontal != nil && policy.Horizontal.Enabled {
		decision := e.evaluateHorizontal(policy, currentMetrics, currentReplicas)
		if decision != nil && decision.Direction != ScalingDirectionNone {
			return decision, nil
		}
	}

	// No scaling needed
	return &ScalingDecision{
		ClusterID:    clusterID,
		Namespace:    namespace,
		Timestamp:    time.Now(),
		Direction:    ScalingDirectionNone,
		Type:         ScalingTypeHorizontal,
		FromReplicas: currentReplicas,
		ToReplicas:   currentReplicas,
		Reason:       "metrics within target range",
	}, nil
}

// evaluateRules evaluates custom scaling rules.
func (e *PolicyEngine) evaluateRules(policy *Policy, currentMetrics *metrics.ClusterMetrics, currentReplicas int32) *ScalingDecision {
	// Create sorted indices to avoid copying rules (preserving state modifications)
	indices := make([]int, len(policy.Rules))
	for i := range indices {
		indices[i] = i
	}
	sort.Slice(indices, func(i, j int) bool {
		return policy.Rules[indices[i]].Priority > policy.Rules[indices[j]].Priority
	})

	now := time.Now()

	for _, idx := range indices {
		rule := &policy.Rules[idx] // Pointer to original rule to preserve state

		// Check rule cooldown
		if !e.cooldowns.CanTriggerRule(policy.ClusterID, policy.Namespace, rule.Name) {
			continue
		}

		// Get metric value
		metricValue := e.getMetricValue(currentMetrics, rule.MetricName)
		if metricValue == 0 {
			continue
		}

		// Evaluate condition
		conditionMet := e.evaluateCondition(metricValue, rule.Operator, rule.Threshold)

		if conditionMet {
			// Track when condition was first met
			if rule.conditionMetAt == nil {
				t := now
				rule.conditionMetAt = &t
			}

			// Check if condition has been met for required duration
			if now.Sub(*rule.conditionMetAt) >= rule.Duration {
				// Trigger the rule
				newReplicas := e.calculateRuleAction(rule.Action, currentReplicas, policy.Horizontal)

				if newReplicas != currentReplicas {
					direction := ScalingDirectionUp
					if newReplicas < currentReplicas {
						direction = ScalingDirectionDown
					}

					// Check directional cooldowns
					if direction == ScalingDirectionUp && !e.cooldowns.CanScaleUp(policy.ClusterID, policy.Namespace) {
						continue
					}
					if direction == ScalingDirectionDown && !e.cooldowns.CanScaleDown(policy.ClusterID, policy.Namespace) {
						continue
					}

					return &ScalingDecision{
						ClusterID:    policy.ClusterID,
						Namespace:    policy.Namespace,
						Timestamp:    now,
						Direction:    direction,
						Type:         ScalingTypeHorizontal,
						FromReplicas: currentReplicas,
						ToReplicas:   newReplicas,
						Reason:       fmt.Sprintf("rule %s triggered: %s %s %.2f for %v", rule.Name, rule.MetricName, rule.Operator, rule.Threshold, rule.Duration),
						Trigger:      rule.Name,
						Confidence:   1.0,
					}
				}
			}
		} else {
			// Reset condition tracking
			rule.conditionMetAt = nil
		}
	}

	return nil
}

// evaluateHorizontal evaluates horizontal scaling based on target metrics.
func (e *PolicyEngine) evaluateHorizontal(policy *Policy, currentMetrics *metrics.ClusterMetrics, currentReplicas int32) *ScalingDecision {
	h := policy.Horizontal
	now := time.Now()

	// Calculate desired replicas based on each metric
	var desiredReplicas int32
	var trigger string
	var maxUtilization float64

	// CPU-based scaling
	// Use math.Ceil to round up - ensures adequate capacity when scaling up
	if currentMetrics.CPUUtilization > 0 && h.TargetCPUUtilization > 0 {
		cpuRatio := currentMetrics.CPUUtilization / h.TargetCPUUtilization
		cpuDesired := int32(math.Ceil(float64(currentReplicas) * cpuRatio))
		if cpuDesired > desiredReplicas {
			desiredReplicas = cpuDesired
			trigger = "cpu_utilization"
			maxUtilization = currentMetrics.CPUUtilization
		}
	}

	// Memory-based scaling
	if currentMetrics.MemoryUtilization > 0 && h.TargetMemoryUtilization > 0 {
		memRatio := currentMetrics.MemoryUtilization / h.TargetMemoryUtilization
		memDesired := int32(math.Ceil(float64(currentReplicas) * memRatio))
		if memDesired > desiredReplicas {
			desiredReplicas = memDesired
			trigger = "memory_utilization"
			maxUtilization = currentMetrics.MemoryUtilization
		}
	}

	// Connection-based scaling
	if currentMetrics.ActiveConnections > 0 && h.TargetConnectionsPerPod > 0 {
		// Use floating-point ceiling division to avoid integer overflow on large values
		connDesired := int32(math.Ceil(float64(currentMetrics.ActiveConnections) / float64(h.TargetConnectionsPerPod)))
		if connDesired > desiredReplicas {
			desiredReplicas = connDesired
			trigger = "active_connections"
		}
	}

	// Latency-based scaling
	if currentMetrics.QueryLatencyP95Ms > 0 && h.TargetQueryLatencyMs > 0 {
		if currentMetrics.QueryLatencyP95Ms > h.TargetQueryLatencyMs {
			latencyRatio := currentMetrics.QueryLatencyP95Ms / h.TargetQueryLatencyMs
			latencyDesired := int32(math.Ceil(float64(currentReplicas) * latencyRatio))
			if latencyDesired > desiredReplicas {
				desiredReplicas = latencyDesired
				trigger = "query_latency"
			}
		}
	}

	// If no scaling is needed based on metrics, use current replicas
	if desiredReplicas == 0 {
		desiredReplicas = currentReplicas
	}

	// Apply stabilization window for scale-down
	if desiredReplicas < currentReplicas {
		key := e.clusterKey(policy.ClusterID, policy.Namespace)
		e.mu.RLock()
		sw := e.stabilization[key]
		e.mu.RUnlock()

		if sw != nil {
			desiredReplicas = sw.getStabilizedValue(now, currentReplicas, desiredReplicas)
		}
	}

	// Apply step limits
	if desiredReplicas > currentReplicas {
		maxIncrease := currentReplicas + h.ScaleUpStep
		if desiredReplicas > maxIncrease {
			desiredReplicas = maxIncrease
		}
	} else if desiredReplicas < currentReplicas {
		maxDecrease := currentReplicas - h.ScaleDownStep
		if desiredReplicas < maxDecrease {
			desiredReplicas = maxDecrease
		}
	}

	// Apply min/max bounds
	if desiredReplicas < h.MinReplicas {
		desiredReplicas = h.MinReplicas
	}
	if desiredReplicas > h.MaxReplicas {
		desiredReplicas = h.MaxReplicas
	}

	// Determine direction and check cooldowns
	var direction ScalingDirection
	if desiredReplicas > currentReplicas {
		if !e.cooldowns.CanScaleUp(policy.ClusterID, policy.Namespace) {
			return nil
		}
		direction = ScalingDirectionUp
	} else if desiredReplicas < currentReplicas {
		if !e.cooldowns.CanScaleDown(policy.ClusterID, policy.Namespace) {
			return nil
		}
		direction = ScalingDirectionDown
	} else {
		direction = ScalingDirectionNone
	}

	return &ScalingDecision{
		ClusterID:    policy.ClusterID,
		Namespace:    policy.Namespace,
		Timestamp:    now,
		Direction:    direction,
		Type:         ScalingTypeHorizontal,
		FromReplicas: currentReplicas,
		ToReplicas:   desiredReplicas,
		Reason:       fmt.Sprintf("target metric %s at %.2f%%", trigger, maxUtilization),
		Trigger:      trigger,
		Confidence:   1.0,
	}
}

// getMetricValue retrieves a metric value by name.
func (e *PolicyEngine) getMetricValue(m *metrics.ClusterMetrics, metricName string) float64 {
	// Guard against nil metrics pointer
	if m == nil {
		return 0
	}

	switch metricName {
	case "cpu_utilization":
		return m.CPUUtilization
	case "memory_utilization":
		return m.MemoryUtilization
	case "active_connections":
		return float64(m.ActiveConnections)
	case "query_latency_p50":
		return m.QueryLatencyP50Ms
	case "query_latency_p95":
		return m.QueryLatencyP95Ms
	case "query_latency_p99":
		return m.QueryLatencyP99Ms
	case "queries_per_second":
		return m.QueriesPerSecond
	case "replication_lag":
		return m.ReplicationLagSeconds
	case "disk_utilization":
		return m.DiskUtilization
	default:
		// Guard against nil CustomMetrics map
		if m.CustomMetrics != nil {
			if v, ok := m.CustomMetrics[metricName]; ok {
				return v
			}
		}
		return 0
	}
}

// evaluateCondition evaluates a rule condition.
func (e *PolicyEngine) evaluateCondition(value float64, op RuleOperator, threshold float64) bool {
	switch op {
	case RuleOperatorGT:
		return value > threshold
	case RuleOperatorGTE:
		return value >= threshold
	case RuleOperatorLT:
		return value < threshold
	case RuleOperatorLTE:
		return value <= threshold
	case RuleOperatorEQ:
		return value == threshold
	default:
		return false
	}
}

// calculateRuleAction calculates the new replica count based on a rule action.
func (e *PolicyEngine) calculateRuleAction(action RuleAction, currentReplicas int32, h *HorizontalPolicy) int32 {
	var newReplicas int32

	switch action.Type {
	case ActionTypeAddReplicas:
		newReplicas = currentReplicas + action.Value
	case ActionTypeRemoveReplicas:
		newReplicas = currentReplicas - action.Value
	case ActionTypeSetReplicas:
		newReplicas = action.Value
	case ActionTypeAddPercent:
		increase := int32(float64(currentReplicas) * float64(action.Value) / 100.0)
		if increase < 1 {
			increase = 1
		}
		newReplicas = currentReplicas + increase
	case ActionTypeRemovePercent:
		decrease := int32(float64(currentReplicas) * float64(action.Value) / 100.0)
		if decrease < 1 {
			decrease = 1
		}
		newReplicas = currentReplicas - decrease
	default:
		return currentReplicas
	}

	// Apply bounds if horizontal policy exists
	if h != nil {
		if newReplicas < h.MinReplicas {
			newReplicas = h.MinReplicas
		}
		if newReplicas > h.MaxReplicas {
			newReplicas = h.MaxReplicas
		}
	}

	if newReplicas < 1 {
		newReplicas = 1
	}

	return newReplicas
}

// getStabilizedValue returns the stabilized replica count.
func (sw *StabilizationWindow) getStabilizedValue(now time.Time, current, desired int32) int32 {
	sw.mu.Lock()
	defer sw.mu.Unlock()

	// Add current sample
	sw.values = append(sw.values, stabilizationSample{
		timestamp: now,
		replicas:  desired,
	})

	// Remove old samples
	cutoff := now.Add(-sw.duration)
	var validValues []stabilizationSample
	for _, v := range sw.values {
		if v.timestamp.After(cutoff) {
			validValues = append(validValues, v)
		}
	}
	sw.values = validValues

	// For scale-down, use the maximum value in the window
	maxReplicas := desired
	for _, v := range sw.values {
		if v.replicas > maxReplicas {
			maxReplicas = v.replicas
		}
	}

	return maxReplicas
}
