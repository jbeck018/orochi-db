// Package grpc provides gRPC handlers for the autoscaler.
package grpc

import (
	"context"
	"fmt"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/orochi-db/orochi-db/services/autoscaler/internal/k8s"
	"github.com/orochi-db/orochi-db/services/autoscaler/internal/metrics"
	"github.com/orochi-db/orochi-db/services/autoscaler/internal/scaler"
)

// Message types for gRPC - these would normally be generated from proto

// GetClusterStatusRequest requests cluster status.
type GetClusterStatusRequest struct {
	ClusterID string
	Namespace string
}

// ClusterStatus represents cluster scaling status.
type ClusterStatus struct {
	ClusterID        string
	Namespace        string
	CurrentReplicas  int32
	DesiredReplicas  int32
	ReadyReplicas    int32
	CurrentResources *ResourceAllocation
	DesiredResources *ResourceAllocation
	State            ScalingState
	LastScaleTime    string
	CurrentMetrics   *ClusterMetrics
}

// ResourceAllocation represents resource allocation.
type ResourceAllocation struct {
	CPURequest    string
	CPULimit      string
	MemoryRequest string
	MemoryLimit   string
	Storage       string
}

// ScalingState represents the scaling state.
type ScalingState int32

const (
	ScalingStateUnknown     ScalingState = 0
	ScalingStateStable      ScalingState = 1
	ScalingStateScalingUp   ScalingState = 2
	ScalingStateScalingDown ScalingState = 3
	ScalingStateCooldown    ScalingState = 4
	ScalingStateError       ScalingState = 5
)

// ClusterMetrics contains cluster metrics.
type ClusterMetrics struct {
	CPUUtilizationPercent    float64
	MemoryUtilizationPercent float64
	ActiveConnections        int64
	QueryLatencyP50Ms        float64
	QueryLatencyP95Ms        float64
	QueryLatencyP99Ms        float64
	QueriesPerSecond         float64
	ReplicationLagSeconds    float64
	DiskUsageBytes           int64
	DiskUtilizationPercent   float64
	Timestamp                string
}

// ScaleClusterRequest requests a scaling operation.
type ScaleClusterRequest struct {
	ClusterID       string
	Namespace       string
	TargetReplicas  *int32
	TargetResources *ResourceAllocation
	Force           bool
}

// ScaleClusterResponse is the response to a scaling request.
type ScaleClusterResponse struct {
	Success     bool
	Message     string
	OperationID string
	NewState    ScalingState
}

// UpdateScalingPolicyRequest updates a scaling policy.
type UpdateScalingPolicyRequest struct {
	ClusterID string
	Namespace string
	Policy    *ScalingPolicy
}

// UpdateScalingPolicyResponse is the response to policy update.
type UpdateScalingPolicyResponse struct {
	Success bool
	Message string
}

// GetScalingPolicyRequest requests a scaling policy.
type GetScalingPolicyRequest struct {
	ClusterID string
	Namespace string
}

// ScalingPolicy defines scaling behavior.
type ScalingPolicy struct {
	ClusterID  string
	Namespace  string
	Enabled    bool
	Horizontal *HorizontalScalingPolicy
	Vertical   *VerticalScalingPolicy
}

// HorizontalScalingPolicy defines horizontal scaling.
type HorizontalScalingPolicy struct {
	Enabled                    bool
	MinReplicas                int32
	MaxReplicas                int32
	TargetCPUUtilization       float64
	TargetMemoryUtilization    float64
	TargetConnectionsPerPod    int64
	TargetQueryLatencyMs       float64
	ScaleUpCooldownSeconds     int32
	ScaleDownCooldownSeconds   int32
	ScaleUpStep                int32
	ScaleDownStep              int32
	StabilizationWindowSeconds int32
}

// VerticalScalingPolicy defines vertical scaling.
type VerticalScalingPolicy struct {
	Enabled                 bool
	MinResources            *ResourceAllocation
	MaxResources            *ResourceAllocation
	TargetCPUUtilization    float64
	TargetMemoryUtilization float64
	Mode                    string
	CooldownSeconds         int32
}

// ListScalingEventsRequest requests scaling events.
type ListScalingEventsRequest struct {
	ClusterID string
	Namespace string
	Limit     int32
	StartTime string
	EndTime   string
}

// ListScalingEventsResponse contains scaling events.
type ListScalingEventsResponse struct {
	Events []*ScalingEvent
}

// ScalingEvent represents a scaling event.
type ScalingEvent struct {
	EventID        string
	ClusterID      string
	Namespace      string
	Timestamp      string
	Type           string
	Trigger        string
	FromReplicas   int32
	ToReplicas     int32
	FromResources  *ResourceAllocation
	ToResources    *ResourceAllocation
	Success        bool
	ErrorMessage   string
	MetricsAtEvent *ClusterMetrics
}

// StreamMetricsRequest requests metrics streaming.
type StreamMetricsRequest struct {
	ClusterID       string
	Namespace       string
	IntervalSeconds int32
}

// Handler implements the AutoscalerServiceServer interface.
type Handler struct {
	k8sClient        *k8s.Client
	metricsCollector *metrics.MetricsCollector
	horizontalScaler *scaler.HorizontalScaler
	verticalScaler   *scaler.VerticalScaler
	policyEngine     *scaler.PolicyEngine
	eventRecorder    *scaler.ScalingEventRecorder
}

// HandlerDependencies holds handler dependencies.
type HandlerDependencies struct {
	K8sClient        *k8s.Client
	MetricsCollector *metrics.MetricsCollector
	HorizontalScaler *scaler.HorizontalScaler
	VerticalScaler   *scaler.VerticalScaler
	PolicyEngine     *scaler.PolicyEngine
	EventRecorder    *scaler.ScalingEventRecorder
}

// NewHandler creates a new gRPC handler.
func NewHandler(deps HandlerDependencies) *Handler {
	return &Handler{
		k8sClient:        deps.K8sClient,
		metricsCollector: deps.MetricsCollector,
		horizontalScaler: deps.HorizontalScaler,
		verticalScaler:   deps.VerticalScaler,
		policyEngine:     deps.PolicyEngine,
		eventRecorder:    deps.EventRecorder,
	}
}

// GetClusterStatus returns the current scaling status of a cluster.
func (h *Handler) GetClusterStatus(ctx context.Context, req *GetClusterStatusRequest) (*ClusterStatus, error) {
	if req.ClusterID == "" || req.Namespace == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	// Get Kubernetes status
	k8sStatus, err := h.k8sClient.GetClusterStatus(ctx, req.ClusterID, req.Namespace)
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "cluster not found: %v", err)
	}

	// Get current metrics
	clusterMetrics, err := h.metricsCollector.Collect(ctx, req.ClusterID, req.Namespace)
	if err != nil {
		// Metrics might not be available, continue without them
		clusterMetrics = nil
	}

	// Determine scaling state
	state := ScalingStateStable
	cooldowns := h.policyEngine.GetCooldowns()
	if !cooldowns.CanScaleUp(req.ClusterID, req.Namespace) || !cooldowns.CanScaleDown(req.ClusterID, req.Namespace) {
		state = ScalingStateCooldown
	}
	if k8sStatus.CurrentReplicas != k8sStatus.DesiredReplicas {
		if k8sStatus.DesiredReplicas > k8sStatus.CurrentReplicas {
			state = ScalingStateScalingUp
		} else {
			state = ScalingStateScalingDown
		}
	}

	// Get last scale time from annotation
	lastScaleTime, _, _ := h.k8sClient.GetAnnotation(ctx, "StatefulSet", req.ClusterID, req.Namespace, "autoscaler.orochi.io/last-scale-time")

	response := &ClusterStatus{
		ClusterID:       req.ClusterID,
		Namespace:       req.Namespace,
		CurrentReplicas: k8sStatus.CurrentReplicas,
		DesiredReplicas: k8sStatus.DesiredReplicas,
		ReadyReplicas:   k8sStatus.ReadyReplicas,
		State:           state,
		LastScaleTime:   lastScaleTime,
	}

	if k8sStatus.Resources != nil {
		response.CurrentResources = &ResourceAllocation{
			CPURequest:    k8sStatus.Resources.CPURequest.String(),
			CPULimit:      k8sStatus.Resources.CPULimit.String(),
			MemoryRequest: k8sStatus.Resources.MemoryRequest.String(),
			MemoryLimit:   k8sStatus.Resources.MemoryLimit.String(),
			Storage:       k8sStatus.Resources.Storage.String(),
		}
	}

	if clusterMetrics != nil {
		response.CurrentMetrics = &ClusterMetrics{
			CPUUtilizationPercent:    clusterMetrics.CPUUtilization,
			MemoryUtilizationPercent: clusterMetrics.MemoryUtilization,
			ActiveConnections:        clusterMetrics.ActiveConnections,
			QueryLatencyP50Ms:        clusterMetrics.QueryLatencyP50Ms,
			QueryLatencyP95Ms:        clusterMetrics.QueryLatencyP95Ms,
			QueryLatencyP99Ms:        clusterMetrics.QueryLatencyP99Ms,
			QueriesPerSecond:         clusterMetrics.QueriesPerSecond,
			ReplicationLagSeconds:    clusterMetrics.ReplicationLagSeconds,
			DiskUsageBytes:           clusterMetrics.DiskUsageBytes,
			DiskUtilizationPercent:   clusterMetrics.DiskUtilization,
			Timestamp:                clusterMetrics.Timestamp.Format(time.RFC3339),
		}
	}

	return response, nil
}

// ScaleCluster triggers a manual scaling operation.
func (h *Handler) ScaleCluster(ctx context.Context, req *ScaleClusterRequest) (*ScaleClusterResponse, error) {
	if req.ClusterID == "" || req.Namespace == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	if req.TargetReplicas == nil && req.TargetResources == nil {
		return nil, status.Error(codes.InvalidArgument, "either target_replicas or target_resources must be specified")
	}

	operationID := fmt.Sprintf("%s-%s-%d", req.Namespace, req.ClusterID, time.Now().UnixNano())

	// Horizontal scaling
	if req.TargetReplicas != nil {
		if err := h.horizontalScaler.Scale(ctx, req.ClusterID, req.Namespace, *req.TargetReplicas, req.Force); err != nil {
			return &ScaleClusterResponse{
				Success:     false,
				Message:     err.Error(),
				OperationID: operationID,
				NewState:    ScalingStateError,
			}, nil
		}
	}

	// Vertical scaling
	if req.TargetResources != nil {
		resources := &scaler.ResourceSpec{
			CPURequest:    req.TargetResources.CPURequest,
			CPULimit:      req.TargetResources.CPULimit,
			MemoryRequest: req.TargetResources.MemoryRequest,
			MemoryLimit:   req.TargetResources.MemoryLimit,
		}
		if err := h.verticalScaler.Scale(ctx, req.ClusterID, req.Namespace, resources, req.Force); err != nil {
			return &ScaleClusterResponse{
				Success:     false,
				Message:     err.Error(),
				OperationID: operationID,
				NewState:    ScalingStateError,
			}, nil
		}
	}

	return &ScaleClusterResponse{
		Success:     true,
		Message:     "scaling operation initiated",
		OperationID: operationID,
		NewState:    ScalingStateScalingUp, // Simplified - should determine actual direction
	}, nil
}

// UpdateScalingPolicy updates the scaling policy for a cluster.
func (h *Handler) UpdateScalingPolicy(ctx context.Context, req *UpdateScalingPolicyRequest) (*UpdateScalingPolicyResponse, error) {
	if req.ClusterID == "" || req.Namespace == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	if req.Policy == nil {
		return nil, status.Error(codes.InvalidArgument, "policy is required")
	}

	// Convert to internal policy
	policy := &scaler.Policy{
		ClusterID: req.ClusterID,
		Namespace: req.Namespace,
		Enabled:   req.Policy.Enabled,
	}

	if req.Policy.Horizontal != nil {
		policy.Horizontal = &scaler.HorizontalPolicy{
			Enabled:                 req.Policy.Horizontal.Enabled,
			MinReplicas:             req.Policy.Horizontal.MinReplicas,
			MaxReplicas:             req.Policy.Horizontal.MaxReplicas,
			TargetCPUUtilization:    req.Policy.Horizontal.TargetCPUUtilization,
			TargetMemoryUtilization: req.Policy.Horizontal.TargetMemoryUtilization,
			TargetConnectionsPerPod: req.Policy.Horizontal.TargetConnectionsPerPod,
			TargetQueryLatencyMs:    req.Policy.Horizontal.TargetQueryLatencyMs,
			ScaleUpCooldown:         time.Duration(req.Policy.Horizontal.ScaleUpCooldownSeconds) * time.Second,
			ScaleDownCooldown:       time.Duration(req.Policy.Horizontal.ScaleDownCooldownSeconds) * time.Second,
			ScaleUpStep:             req.Policy.Horizontal.ScaleUpStep,
			ScaleDownStep:           req.Policy.Horizontal.ScaleDownStep,
			StabilizationWindow:     time.Duration(req.Policy.Horizontal.StabilizationWindowSeconds) * time.Second,
		}
	}

	if req.Policy.Vertical != nil {
		policy.Vertical = &scaler.VerticalPolicy{
			Enabled:                 req.Policy.Vertical.Enabled,
			TargetCPUUtilization:    req.Policy.Vertical.TargetCPUUtilization,
			TargetMemoryUtilization: req.Policy.Vertical.TargetMemoryUtilization,
			Mode:                    scaler.VerticalScaleMode(req.Policy.Vertical.Mode),
			Cooldown:                time.Duration(req.Policy.Vertical.CooldownSeconds) * time.Second,
		}
		if req.Policy.Vertical.MinResources != nil {
			policy.Vertical.MinCPU = req.Policy.Vertical.MinResources.CPURequest
			policy.Vertical.MinMemory = req.Policy.Vertical.MinResources.MemoryRequest
		}
		if req.Policy.Vertical.MaxResources != nil {
			policy.Vertical.MaxCPU = req.Policy.Vertical.MaxResources.CPURequest
			policy.Vertical.MaxMemory = req.Policy.Vertical.MaxResources.MemoryRequest
		}
	}

	h.policyEngine.SetPolicy(policy)

	// Register cluster for metrics collection
	if err := h.metricsCollector.RegisterCluster(req.ClusterID, req.Namespace); err != nil {
		return &UpdateScalingPolicyResponse{
			Success: false,
			Message: fmt.Sprintf("failed to register cluster for metrics: %v", err),
		}, nil
	}

	return &UpdateScalingPolicyResponse{
		Success: true,
		Message: "policy updated successfully",
	}, nil
}

// GetScalingPolicy returns the current scaling policy for a cluster.
func (h *Handler) GetScalingPolicy(ctx context.Context, req *GetScalingPolicyRequest) (*ScalingPolicy, error) {
	if req.ClusterID == "" || req.Namespace == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	policy, ok := h.policyEngine.GetPolicy(req.ClusterID, req.Namespace)
	if !ok {
		return nil, status.Error(codes.NotFound, "policy not found")
	}

	response := &ScalingPolicy{
		ClusterID: policy.ClusterID,
		Namespace: policy.Namespace,
		Enabled:   policy.Enabled,
	}

	if policy.Horizontal != nil {
		response.Horizontal = &HorizontalScalingPolicy{
			Enabled:                    policy.Horizontal.Enabled,
			MinReplicas:                policy.Horizontal.MinReplicas,
			MaxReplicas:                policy.Horizontal.MaxReplicas,
			TargetCPUUtilization:       policy.Horizontal.TargetCPUUtilization,
			TargetMemoryUtilization:    policy.Horizontal.TargetMemoryUtilization,
			TargetConnectionsPerPod:    policy.Horizontal.TargetConnectionsPerPod,
			TargetQueryLatencyMs:       policy.Horizontal.TargetQueryLatencyMs,
			ScaleUpCooldownSeconds:     int32(policy.Horizontal.ScaleUpCooldown.Seconds()),
			ScaleDownCooldownSeconds:   int32(policy.Horizontal.ScaleDownCooldown.Seconds()),
			ScaleUpStep:                policy.Horizontal.ScaleUpStep,
			ScaleDownStep:              policy.Horizontal.ScaleDownStep,
			StabilizationWindowSeconds: int32(policy.Horizontal.StabilizationWindow.Seconds()),
		}
	}

	if policy.Vertical != nil {
		response.Vertical = &VerticalScalingPolicy{
			Enabled:                 policy.Vertical.Enabled,
			TargetCPUUtilization:    policy.Vertical.TargetCPUUtilization,
			TargetMemoryUtilization: policy.Vertical.TargetMemoryUtilization,
			Mode:                    string(policy.Vertical.Mode),
			CooldownSeconds:         int32(policy.Vertical.Cooldown.Seconds()),
			MinResources: &ResourceAllocation{
				CPURequest:    policy.Vertical.MinCPU,
				MemoryRequest: policy.Vertical.MinMemory,
			},
			MaxResources: &ResourceAllocation{
				CPURequest:    policy.Vertical.MaxCPU,
				MemoryRequest: policy.Vertical.MaxMemory,
			},
		}
	}

	return response, nil
}

// ListScalingEvents returns recent scaling events for a cluster.
func (h *Handler) ListScalingEvents(ctx context.Context, req *ListScalingEventsRequest) (*ListScalingEventsResponse, error) {
	if req.ClusterID == "" || req.Namespace == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	limit := int(req.Limit)
	if limit <= 0 {
		limit = 100
	}

	var events []scaler.ScalingEvent
	if req.StartTime != "" && req.EndTime != "" {
		start, err := time.Parse(time.RFC3339, req.StartTime)
		if err != nil {
			return nil, status.Error(codes.InvalidArgument, "invalid start_time format")
		}
		end, err := time.Parse(time.RFC3339, req.EndTime)
		if err != nil {
			return nil, status.Error(codes.InvalidArgument, "invalid end_time format")
		}
		events = h.eventRecorder.GetEventsInRange(req.ClusterID, req.Namespace, start, end)
	} else {
		events = h.eventRecorder.GetEvents(req.ClusterID, req.Namespace, limit)
	}

	response := &ListScalingEventsResponse{
		Events: make([]*ScalingEvent, 0, len(events)),
	}

	for _, e := range events {
		event := &ScalingEvent{
			EventID:      e.ID,
			ClusterID:    e.ClusterID,
			Namespace:    e.Namespace,
			Timestamp:    e.Timestamp.Format(time.RFC3339),
			Type:         string(e.Type),
			Trigger:      e.Trigger,
			FromReplicas: e.FromReplicas,
			ToReplicas:   e.ToReplicas,
			Success:      e.Success,
			ErrorMessage: e.ErrorMessage,
		}
		response.Events = append(response.Events, event)
	}

	return response, nil
}

// StreamMetrics streams real-time metrics for a cluster.
func (h *Handler) StreamMetrics(req *StreamMetricsRequest, stream AutoscalerService_StreamMetricsServer) error {
	if req.ClusterID == "" || req.Namespace == "" {
		return status.Error(codes.InvalidArgument, "cluster_id and namespace are required")
	}

	interval := time.Duration(req.IntervalSeconds) * time.Second
	if interval < 5*time.Second {
		interval = 5 * time.Second
	}

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	ctx := stream.Context()

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			clusterMetrics, err := h.metricsCollector.Collect(ctx, req.ClusterID, req.Namespace)
			if err != nil {
				// Log error but continue streaming
				fmt.Printf("failed to collect metrics for stream: %v\n", err)
				continue
			}

			metrics := &ClusterMetrics{
				CPUUtilizationPercent:    clusterMetrics.CPUUtilization,
				MemoryUtilizationPercent: clusterMetrics.MemoryUtilization,
				ActiveConnections:        clusterMetrics.ActiveConnections,
				QueryLatencyP50Ms:        clusterMetrics.QueryLatencyP50Ms,
				QueryLatencyP95Ms:        clusterMetrics.QueryLatencyP95Ms,
				QueryLatencyP99Ms:        clusterMetrics.QueryLatencyP99Ms,
				QueriesPerSecond:         clusterMetrics.QueriesPerSecond,
				ReplicationLagSeconds:    clusterMetrics.ReplicationLagSeconds,
				DiskUsageBytes:           clusterMetrics.DiskUsageBytes,
				DiskUtilizationPercent:   clusterMetrics.DiskUtilization,
				Timestamp:                clusterMetrics.Timestamp.Format(time.RFC3339),
			}

			if err := stream.Send(metrics); err != nil {
				return err
			}
		}
	}
}
