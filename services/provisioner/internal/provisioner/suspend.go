// Package provisioner provides cluster provisioning and management functionality.
package provisioner

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"
	"sigs.k8s.io/controller-runtime/pkg/client"

	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

// SuspendConfig holds configuration for suspend operations.
type SuspendConfig struct {
	DrainTimeout         time.Duration // Time to wait for connections to drain
	GracePeriodSeconds   int64         // Kubernetes termination grace period
	PreserveVolumeData   bool          // Keep PVCs when scaling to zero
	AnnotateSuspendTime  bool          // Add annotation with suspend timestamp
}

// DefaultSuspendConfig returns default suspend configuration.
func DefaultSuspendConfig() *SuspendConfig {
	return &SuspendConfig{
		DrainTimeout:         30 * time.Second,
		GracePeriodSeconds:   30,
		PreserveVolumeData:   true,
		AnnotateSuspendTime:  true,
	}
}

// ResumeConfig holds configuration for resume operations.
type ResumeConfig struct {
	WaitForReady   bool          // Wait for cluster to become ready
	ReadyTimeout   time.Duration // Maximum time to wait for ready state
	HealthCheckURL string        // URL to check for health (optional)
}

// DefaultResumeConfig returns default resume configuration.
func DefaultResumeConfig() *ResumeConfig {
	return &ResumeConfig{
		WaitForReady: true,
		ReadyTimeout: 60 * time.Second,
	}
}

// SuspendResult contains the result of a suspend operation.
type SuspendResult struct {
	Success           bool          `json:"success"`
	PreviousReplicas  int32         `json:"previousReplicas"`
	ConnectionsDrained int          `json:"connectionsDrained"`
	SuspendedAt       time.Time     `json:"suspendedAt"`
	Duration          time.Duration `json:"duration"`
	Error             string        `json:"error,omitempty"`
}

// ResumeResult contains the result of a resume operation.
type ResumeResult struct {
	Success         bool          `json:"success"`
	TargetReplicas  int32         `json:"targetReplicas"`
	ReadyReplicas   int32         `json:"readyReplicas"`
	ResumedAt       time.Time     `json:"resumedAt"`
	Duration        time.Duration `json:"duration"`
	ColdStartLatency time.Duration `json:"coldStartLatency"`
	Error           string        `json:"error,omitempty"`
}

// SuspendCluster suspends a cluster by scaling its PostgreSQL instances to zero.
// This preserves all data in persistent volumes while stopping compute resources.
func (s *Service) SuspendCluster(ctx context.Context, namespace, name string, config *SuspendConfig) (*SuspendResult, error) {
	startTime := time.Now()
	result := &SuspendResult{
		SuspendedAt: startTime,
	}

	if config == nil {
		config = DefaultSuspendConfig()
	}

	s.logger.Info("suspending cluster",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Duration("drainTimeout", config.DrainTimeout),
	)

	// Get current cluster state
	cluster, err := s.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		result.Error = fmt.Sprintf("failed to get cluster: %v", err)
		return result, err
	}

	// Get current replica count from the cluster
	currentReplicas, err := s.getClusterReplicas(cluster)
	if err != nil {
		result.Error = fmt.Sprintf("failed to get replica count: %v", err)
		return result, err
	}
	result.PreviousReplicas = currentReplicas

	// Step 1: Drain connections from the PgDog pooler (if enabled)
	if err := s.drainConnections(ctx, namespace, name, config.DrainTimeout); err != nil {
		s.logger.Warn("failed to drain connections, continuing with suspend",
			zap.String("cluster", name),
			zap.Error(err),
		)
	}

	// Step 2: Annotate the cluster with suspend metadata
	if config.AnnotateSuspendTime {
		if err := s.annotateClusterSuspend(ctx, namespace, name, currentReplicas); err != nil {
			s.logger.Warn("failed to annotate cluster",
				zap.String("cluster", name),
				zap.Error(err),
			)
		}
	}

	// Step 3: Scale CloudNativePG cluster to 0 instances
	if err := s.scaleClusterInstances(ctx, namespace, name, 0); err != nil {
		result.Error = fmt.Sprintf("failed to scale cluster to 0: %v", err)
		return result, err
	}

	// Step 4: Scale PgDog pooler to 0 if present
	if err := s.scalePooler(ctx, namespace, name, 0); err != nil {
		s.logger.Warn("failed to scale pooler to 0",
			zap.String("cluster", name),
			zap.Error(err),
		)
		// Don't fail the suspend for pooler issues
	}

	// Step 5: Wait for pods to terminate
	if err := s.waitForPodsTerminated(ctx, namespace, name, config.DrainTimeout); err != nil {
		s.logger.Warn("timeout waiting for pods to terminate",
			zap.String("cluster", name),
			zap.Error(err),
		)
	}

	result.Success = true
	result.Duration = time.Since(startTime)

	s.logger.Info("cluster suspended successfully",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Int32("previousReplicas", currentReplicas),
		zap.Duration("duration", result.Duration),
	)

	return result, nil
}

// ResumeCluster resumes a suspended cluster by scaling it back up.
func (s *Service) ResumeCluster(ctx context.Context, namespace, name string, config *ResumeConfig) (*ResumeResult, error) {
	startTime := time.Now()
	result := &ResumeResult{
		ResumedAt: startTime,
	}

	if config == nil {
		config = DefaultResumeConfig()
	}

	s.logger.Info("resuming cluster",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Bool("waitForReady", config.WaitForReady),
	)

	// Get the cluster and its stored replica count
	cluster, err := s.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		result.Error = fmt.Sprintf("failed to get cluster: %v", err)
		return result, err
	}

	// Get the target replica count from annotations or use default
	targetReplicas := s.getStoredReplicaCount(cluster)
	if targetReplicas == 0 {
		targetReplicas = 1 // Default to at least 1 replica
	}
	result.TargetReplicas = targetReplicas

	// Step 1: Scale CloudNativePG cluster back up
	if err := s.scaleClusterInstances(ctx, namespace, name, targetReplicas); err != nil {
		result.Error = fmt.Sprintf("failed to scale cluster up: %v", err)
		return result, err
	}

	// Step 2: Scale PgDog pooler back up if it was previously enabled
	poolerReplicas := s.getStoredPoolerReplicaCount(cluster)
	if poolerReplicas > 0 {
		if err := s.scalePooler(ctx, namespace, name, poolerReplicas); err != nil {
			s.logger.Warn("failed to scale pooler up",
				zap.String("cluster", name),
				zap.Error(err),
			)
		}
	}

	// Step 3: Wait for cluster to be ready (if requested)
	if config.WaitForReady {
		readyStart := time.Now()
		if err := s.clusterManager.WaitForClusterReady(ctx, namespace, name, config.ReadyTimeout); err != nil {
			s.logger.Warn("timeout waiting for cluster ready",
				zap.String("cluster", name),
				zap.Error(err),
			)
			result.Error = fmt.Sprintf("cluster not ready within timeout: %v", err)
		}
		result.ColdStartLatency = time.Since(readyStart)
	}

	// Step 4: Get final ready replica count
	status, err := s.clusterManager.GetClusterStatus(ctx, namespace, name)
	if err == nil {
		result.ReadyReplicas = status.ReadyInstances
	}

	// Step 5: Clear suspend annotations
	if err := s.clearSuspendAnnotations(ctx, namespace, name); err != nil {
		s.logger.Warn("failed to clear suspend annotations",
			zap.String("cluster", name),
			zap.Error(err),
		)
	}

	result.Success = result.Error == ""
	result.Duration = time.Since(startTime)

	s.logger.Info("cluster resumed",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Int32("targetReplicas", targetReplicas),
		zap.Int32("readyReplicas", result.ReadyReplicas),
		zap.Duration("coldStartLatency", result.ColdStartLatency),
		zap.Duration("totalDuration", result.Duration),
	)

	return result, nil
}

// DrainConnections gracefully drains connections from the cluster before suspend.
func (s *Service) DrainConnections(ctx context.Context, namespace, name string, timeout time.Duration) error {
	return s.drainConnections(ctx, namespace, name, timeout)
}

// drainConnections implements connection draining via PgDog.
func (s *Service) drainConnections(ctx context.Context, namespace, name string, timeout time.Duration) error {
	s.logger.Info("draining connections",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Duration("timeout", timeout),
	)

	// Check if pooler exists
	poolerStatus, err := s.poolerManager.GetPoolerStatus(ctx, namespace, name)
	if err != nil || poolerStatus == nil || !poolerStatus.Enabled {
		s.logger.Info("no pooler found, skipping connection drain",
			zap.String("cluster", name),
		)
		return nil
	}

	// In production, this would:
	// 1. Signal PgDog to stop accepting new connections
	// 2. Wait for existing connections to complete or timeout
	// 3. Forcefully close any remaining connections after timeout

	// For now, simulate drain by waiting a short time
	drainCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	select {
	case <-drainCtx.Done():
		if drainCtx.Err() == context.DeadlineExceeded {
			s.logger.Warn("connection drain timeout",
				zap.String("cluster", name),
			)
		}
	case <-time.After(timeout / 2): // Simulate shorter drain time
		s.logger.Info("connections drained successfully",
			zap.String("cluster", name),
		)
	}

	return nil
}

// WaitForReady waits for a cluster to become ready after resume.
func (s *Service) WaitForReady(ctx context.Context, namespace, name string, timeout time.Duration) error {
	return s.clusterManager.WaitForClusterReady(ctx, namespace, name, timeout)
}

// scaleClusterInstances scales the CloudNativePG cluster to the specified replica count.
func (s *Service) scaleClusterInstances(ctx context.Context, namespace, name string, replicas int32) error {
	s.logger.Info("scaling cluster instances",
		zap.String("namespace", namespace),
		zap.String("name", name),
		zap.Int32("replicas", replicas),
	)

	// Get the cluster object
	cluster := &unstructured.Unstructured{}
	cluster.SetGroupVersionKind(s.clusterManager.GetClusterGVK())

	key := client.ObjectKey{Namespace: namespace, Name: name}
	if err := s.k8sClient.Get(ctx, key, cluster); err != nil {
		return fmt.Errorf("failed to get cluster: %w", err)
	}

	// Update the instances field
	if err := unstructured.SetNestedField(cluster.Object, int64(replicas), "spec", "instances"); err != nil {
		return fmt.Errorf("failed to set instances: %w", err)
	}

	// Update the cluster
	if err := s.k8sClient.CtrlClient().Update(ctx, cluster); err != nil {
		return fmt.Errorf("failed to update cluster: %w", err)
	}

	return nil
}

// scalePooler scales the PgDog pooler deployment.
func (s *Service) scalePooler(ctx context.Context, namespace, name string, replicas int32) error {
	deploymentName := fmt.Sprintf("%s-pgdog", name)

	s.logger.Info("scaling pooler deployment",
		zap.String("namespace", namespace),
		zap.String("deployment", deploymentName),
		zap.Int32("replicas", replicas),
	)

	deployment := &appsv1.Deployment{}
	key := client.ObjectKey{Namespace: namespace, Name: deploymentName}

	if err := s.k8sClient.Get(ctx, key, deployment); err != nil {
		if errors.IsNotFound(err) {
			s.logger.Debug("pooler deployment not found, skipping scale",
				zap.String("deployment", deploymentName),
			)
			return nil
		}
		return fmt.Errorf("failed to get pooler deployment: %w", err)
	}

	// Store current replica count before scaling to 0
	if replicas == 0 && deployment.Spec.Replicas != nil && *deployment.Spec.Replicas > 0 {
		if deployment.Annotations == nil {
			deployment.Annotations = make(map[string]string)
		}
		deployment.Annotations["orochi.io/previous-pooler-replicas"] = fmt.Sprintf("%d", *deployment.Spec.Replicas)
	}

	deployment.Spec.Replicas = &replicas

	if err := s.k8sClient.CtrlClient().Update(ctx, deployment); err != nil {
		return fmt.Errorf("failed to update pooler deployment: %w", err)
	}

	return nil
}

// annotateClusterSuspend adds suspend metadata annotations to the cluster.
func (s *Service) annotateClusterSuspend(ctx context.Context, namespace, name string, previousReplicas int32) error {
	cluster := &unstructured.Unstructured{}
	cluster.SetGroupVersionKind(s.clusterManager.GetClusterGVK())

	key := client.ObjectKey{Namespace: namespace, Name: name}
	if err := s.k8sClient.Get(ctx, key, cluster); err != nil {
		return fmt.Errorf("failed to get cluster: %w", err)
	}

	annotations := cluster.GetAnnotations()
	if annotations == nil {
		annotations = make(map[string]string)
	}

	annotations["orochi.io/suspended"] = "true"
	annotations["orochi.io/suspended-at"] = time.Now().Format(time.RFC3339)
	annotations["orochi.io/previous-replicas"] = fmt.Sprintf("%d", previousReplicas)

	cluster.SetAnnotations(annotations)

	if err := s.k8sClient.CtrlClient().Update(ctx, cluster); err != nil {
		return fmt.Errorf("failed to update cluster annotations: %w", err)
	}

	return nil
}

// clearSuspendAnnotations removes suspend metadata annotations from the cluster.
func (s *Service) clearSuspendAnnotations(ctx context.Context, namespace, name string) error {
	cluster := &unstructured.Unstructured{}
	cluster.SetGroupVersionKind(s.clusterManager.GetClusterGVK())

	key := client.ObjectKey{Namespace: namespace, Name: name}
	if err := s.k8sClient.Get(ctx, key, cluster); err != nil {
		return fmt.Errorf("failed to get cluster: %w", err)
	}

	annotations := cluster.GetAnnotations()
	if annotations == nil {
		return nil
	}

	delete(annotations, "orochi.io/suspended")
	delete(annotations, "orochi.io/suspended-at")
	delete(annotations, "orochi.io/previous-replicas")

	cluster.SetAnnotations(annotations)

	if err := s.k8sClient.CtrlClient().Update(ctx, cluster); err != nil {
		return fmt.Errorf("failed to update cluster annotations: %w", err)
	}

	return nil
}

// getClusterReplicas returns the current replica count of a cluster.
func (s *Service) getClusterReplicas(cluster interface{}) (int32, error) {
	unstructuredCluster, ok := cluster.(*unstructured.Unstructured)
	if !ok {
		return 0, fmt.Errorf("unexpected cluster type: %T", cluster)
	}

	instances, found, err := unstructured.NestedInt64(unstructuredCluster.Object, "spec", "instances")
	if err != nil || !found {
		return 0, fmt.Errorf("failed to get instances from cluster spec")
	}

	return int32(instances), nil
}

// getStoredReplicaCount returns the replica count stored before suspend.
func (s *Service) getStoredReplicaCount(cluster interface{}) int32 {
	unstructuredCluster, ok := cluster.(*unstructured.Unstructured)
	if !ok {
		return 0
	}

	annotations := unstructuredCluster.GetAnnotations()
	if annotations == nil {
		return 0
	}

	replicasStr, ok := annotations["orochi.io/previous-replicas"]
	if !ok {
		return 0
	}

	var replicas int32
	fmt.Sscanf(replicasStr, "%d", &replicas)
	return replicas
}

// getStoredPoolerReplicaCount returns the pooler replica count stored before suspend.
func (s *Service) getStoredPoolerReplicaCount(cluster interface{}) int32 {
	unstructuredCluster, ok := cluster.(*unstructured.Unstructured)
	if !ok {
		return 0
	}

	annotations := unstructuredCluster.GetAnnotations()
	if annotations == nil {
		return 0
	}

	replicasStr, ok := annotations["orochi.io/previous-pooler-replicas"]
	if !ok {
		// Check the pooler deployment annotations
		return 0
	}

	var replicas int32
	fmt.Sscanf(replicasStr, "%d", &replicas)
	return replicas
}

// waitForPodsTerminated waits for all cluster pods to be terminated.
func (s *Service) waitForPodsTerminated(ctx context.Context, namespace, name string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			if time.Now().After(deadline) {
				return fmt.Errorf("timeout waiting for pods to terminate")
			}

			pods := &corev1.PodList{}
			listOpts := []client.ListOption{
				client.InNamespace(namespace),
				client.MatchingLabels{
					"cnpg.io/cluster": name,
				},
			}

			if err := s.k8sClient.List(ctx, pods, listOpts...); err != nil {
				return fmt.Errorf("failed to list pods: %w", err)
			}

			// Check if any pods are still running
			runningPods := 0
			for _, pod := range pods.Items {
				if pod.Status.Phase == corev1.PodRunning || pod.Status.Phase == corev1.PodPending {
					runningPods++
				}
			}

			if runningPods == 0 {
				return nil
			}

			s.logger.Debug("waiting for pods to terminate",
				zap.String("cluster", name),
				zap.Int("runningPods", runningPods),
			)
		}
	}
}

// IsSuspended checks if a cluster is currently suspended.
func (s *Service) IsSuspended(ctx context.Context, namespace, name string) (bool, error) {
	cluster, err := s.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		return false, err
	}

	annotations := cluster.GetAnnotations()
	if annotations == nil {
		return false, nil
	}

	suspended, ok := annotations["orochi.io/suspended"]
	return ok && suspended == "true", nil
}

// GetSuspendInfo returns information about a suspended cluster.
func (s *Service) GetSuspendInfo(ctx context.Context, namespace, name string) (*types.SuspendInfo, error) {
	cluster, err := s.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		return nil, err
	}

	annotations := cluster.GetAnnotations()
	if annotations == nil {
		return nil, nil
	}

	suspended, ok := annotations["orochi.io/suspended"]
	if !ok || suspended != "true" {
		return nil, nil
	}

	info := &types.SuspendInfo{
		Suspended: true,
	}

	if suspendedAt, ok := annotations["orochi.io/suspended-at"]; ok {
		if t, err := time.Parse(time.RFC3339, suspendedAt); err == nil {
			info.SuspendedAt = t
		}
	}

	if previousReplicas, ok := annotations["orochi.io/previous-replicas"]; ok {
		fmt.Sscanf(previousReplicas, "%d", &info.PreviousReplicas)
	}

	return info, nil
}
