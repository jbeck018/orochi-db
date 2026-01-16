// Package cloudnativepg provides restore functionality for CloudNativePG clusters.
package cloudnativepg

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"

	"github.com/orochi-db/orochi-cloud/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/config"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/types"
)

// RestoreManager manages CloudNativePG cluster restore operations
type RestoreManager struct {
	k8sClient      *k8s.Client
	clusterManager *ClusterManager
	cfg            *config.CloudNativePGConfig
	logger         *zap.Logger
}

// NewRestoreManager creates a new RestoreManager
func NewRestoreManager(k8sClient *k8s.Client, clusterManager *ClusterManager, cfg *config.CloudNativePGConfig, logger *zap.Logger) *RestoreManager {
	return &RestoreManager{
		k8sClient:      k8sClient,
		clusterManager: clusterManager,
		cfg:            cfg,
		logger:         logger,
	}
}

// RestoreFromBackup restores a cluster from a backup
func (m *RestoreManager) RestoreFromBackup(ctx context.Context, sourceCluster, sourceNamespace string, targetSpec *types.ClusterSpec, options *types.RestoreOptions) (*unstructured.Unstructured, error) {
	m.logger.Info("restoring cluster from backup",
		zap.String("sourceCluster", sourceCluster),
		zap.String("sourceNamespace", sourceNamespace),
		zap.String("targetName", targetSpec.Name),
		zap.String("targetNamespace", targetSpec.Namespace),
	)

	// Build the bootstrap configuration for recovery
	bootstrap := m.buildRecoveryBootstrap(sourceCluster, sourceNamespace, options)

	// Create the target cluster spec
	cluster := m.clusterManager.buildClusterSpec(targetSpec)
	specObj := cluster.Object["spec"].(map[string]interface{})
	specObj["bootstrap"] = bootstrap

	// Create the cluster
	if err := m.k8sClient.WithRetry(ctx, "create restored cluster", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, cluster)
	}); err != nil {
		return nil, fmt.Errorf("failed to create restored cluster %s/%s: %w", targetSpec.Namespace, targetSpec.Name, err)
	}

	return cluster, nil
}

// RestoreFromObjectStore restores a cluster from an object store backup
func (m *RestoreManager) RestoreFromObjectStore(ctx context.Context, targetSpec *types.ClusterSpec, barmanConfig *types.BarmanObjectStore, options *types.RestoreOptions) (*unstructured.Unstructured, error) {
	m.logger.Info("restoring cluster from object store",
		zap.String("targetName", targetSpec.Name),
		zap.String("targetNamespace", targetSpec.Namespace),
		zap.String("destinationPath", barmanConfig.DestinationPath),
	)

	// Build the bootstrap configuration for object store recovery
	bootstrap := m.buildObjectStoreRecoveryBootstrap(barmanConfig, options)

	// Create the target cluster spec
	cluster := m.clusterManager.buildClusterSpec(targetSpec)
	specObj := cluster.Object["spec"].(map[string]interface{})
	specObj["bootstrap"] = bootstrap

	// Create the cluster
	if err := m.k8sClient.WithRetry(ctx, "create restored cluster from object store", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, cluster)
	}); err != nil {
		return nil, fmt.Errorf("failed to create restored cluster %s/%s: %w", targetSpec.Namespace, targetSpec.Name, err)
	}

	return cluster, nil
}

// RestorePointInTime performs a point-in-time recovery
func (m *RestoreManager) RestorePointInTime(ctx context.Context, sourceCluster, sourceNamespace string, targetSpec *types.ClusterSpec, targetTime time.Time) (*unstructured.Unstructured, error) {
	options := &types.RestoreOptions{
		RecoveryTargetTime:      targetTime.Format(time.RFC3339),
		RecoveryTargetInclusive: true,
	}

	return m.RestoreFromBackup(ctx, sourceCluster, sourceNamespace, targetSpec, options)
}

// RestoreToLSN restores to a specific LSN
func (m *RestoreManager) RestoreToLSN(ctx context.Context, sourceCluster, sourceNamespace string, targetSpec *types.ClusterSpec, lsn string) (*unstructured.Unstructured, error) {
	options := &types.RestoreOptions{
		RecoveryTargetLSN:       lsn,
		RecoveryTargetInclusive: true,
	}

	return m.RestoreFromBackup(ctx, sourceCluster, sourceNamespace, targetSpec, options)
}

// CloneCluster creates a clone of an existing cluster
func (m *RestoreManager) CloneCluster(ctx context.Context, sourceCluster, sourceNamespace string, targetSpec *types.ClusterSpec) (*unstructured.Unstructured, error) {
	m.logger.Info("cloning cluster",
		zap.String("sourceCluster", sourceCluster),
		zap.String("sourceNamespace", sourceNamespace),
		zap.String("targetName", targetSpec.Name),
		zap.String("targetNamespace", targetSpec.Namespace),
	)

	// Build bootstrap configuration for pg_basebackup clone
	bootstrap := map[string]interface{}{
		"pg_basebackup": map[string]interface{}{
			"source": sourceCluster,
		},
	}

	// Build external cluster reference
	externalClusters := []interface{}{
		map[string]interface{}{
			"name": sourceCluster,
			"connectionParameters": map[string]interface{}{
				"host":   fmt.Sprintf("%s-rw.%s.svc", sourceCluster, sourceNamespace),
				"user":   "streaming_replica",
				"dbname": "postgres",
			},
			"sslMode": "verify-full",
		},
	}

	// Create the target cluster spec
	cluster := m.clusterManager.buildClusterSpec(targetSpec)
	specObj := cluster.Object["spec"].(map[string]interface{})
	specObj["bootstrap"] = bootstrap
	specObj["externalClusters"] = externalClusters

	// Create the cluster
	if err := m.k8sClient.WithRetry(ctx, "create cloned cluster", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, cluster)
	}); err != nil {
		return nil, fmt.Errorf("failed to create cloned cluster %s/%s: %w", targetSpec.Namespace, targetSpec.Name, err)
	}

	return cluster, nil
}

// buildRecoveryBootstrap builds the bootstrap configuration for recovery from another cluster
func (m *RestoreManager) buildRecoveryBootstrap(sourceCluster, sourceNamespace string, options *types.RestoreOptions) map[string]interface{} {
	recovery := map[string]interface{}{
		"source": sourceCluster,
	}

	// Set recovery target if specified
	if options != nil {
		recoveryTarget := map[string]interface{}{}
		hasTarget := false

		if options.BackupID != "" {
			recovery["backup"] = map[string]interface{}{
				"name": options.BackupID,
			}
		}

		if options.RecoveryTargetTime != "" {
			recoveryTarget["targetTime"] = options.RecoveryTargetTime
			hasTarget = true
		}

		if options.RecoveryTargetLSN != "" {
			recoveryTarget["targetLSN"] = options.RecoveryTargetLSN
			hasTarget = true
		}

		if options.RecoveryTargetName != "" {
			recoveryTarget["targetName"] = options.RecoveryTargetName
			hasTarget = true
		}

		if hasTarget {
			recoveryTarget["targetInclusive"] = options.RecoveryTargetInclusive
			recovery["recoveryTarget"] = recoveryTarget
		}
	}

	return map[string]interface{}{
		"recovery": recovery,
	}
}

// buildObjectStoreRecoveryBootstrap builds the bootstrap configuration for object store recovery
func (m *RestoreManager) buildObjectStoreRecoveryBootstrap(barmanConfig *types.BarmanObjectStore, options *types.RestoreOptions) map[string]interface{} {
	recovery := map[string]interface{}{
		"source": "backup-source",
	}

	// Set recovery target if specified
	if options != nil {
		recoveryTarget := map[string]interface{}{}
		hasTarget := false

		if options.RecoveryTargetTime != "" {
			recoveryTarget["targetTime"] = options.RecoveryTargetTime
			hasTarget = true
		}

		if options.RecoveryTargetLSN != "" {
			recoveryTarget["targetLSN"] = options.RecoveryTargetLSN
			hasTarget = true
		}

		if options.RecoveryTargetName != "" {
			recoveryTarget["targetName"] = options.RecoveryTargetName
			hasTarget = true
		}

		if hasTarget {
			recoveryTarget["targetInclusive"] = options.RecoveryTargetInclusive
			recovery["recoveryTarget"] = recoveryTarget
		}
	}

	return map[string]interface{}{
		"recovery": recovery,
	}
}

// WaitForRestoreComplete waits for the restored cluster to be ready
func (m *RestoreManager) WaitForRestoreComplete(ctx context.Context, namespace, name string, timeout time.Duration) error {
	return m.clusterManager.WaitForClusterReady(ctx, namespace, name, timeout)
}

// ValidateRestoreOptions validates restore options
func (m *RestoreManager) ValidateRestoreOptions(options *types.RestoreOptions) error {
	if options == nil {
		return nil
	}

	// Count the number of recovery targets specified
	targets := 0
	if options.RecoveryTargetTime != "" {
		targets++
		// Validate time format
		if _, err := time.Parse(time.RFC3339, options.RecoveryTargetTime); err != nil {
			return fmt.Errorf("invalid recovery target time format: %w", err)
		}
	}
	if options.RecoveryTargetLSN != "" {
		targets++
	}
	if options.RecoveryTargetName != "" {
		targets++
	}

	if targets > 1 {
		return fmt.Errorf("only one recovery target can be specified (time, lsn, or name)")
	}

	return nil
}

// GetRestoreStatus returns the status of a restore operation
func (m *RestoreManager) GetRestoreStatus(ctx context.Context, namespace, name string) (*RestoreStatus, error) {
	cluster, err := m.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		return nil, err
	}

	phase, _, _ := unstructured.NestedString(cluster.Object, "status", "phase")

	status := &RestoreStatus{
		ClusterName: name,
		Namespace:   namespace,
		Phase:       types.ClusterPhase(phase),
	}

	// Check for recovery-specific conditions
	conditions, found, _ := unstructured.NestedSlice(cluster.Object, "status", "conditions")
	if found {
		for _, c := range conditions {
			if condition, ok := c.(map[string]interface{}); ok {
				condType, _, _ := unstructured.NestedString(condition, "type")
				if condType == "Ready" {
					condStatus, _, _ := unstructured.NestedString(condition, "status")
					status.Ready = condStatus == "True"
					status.Message, _, _ = unstructured.NestedString(condition, "message")
				}
			}
		}
	}

	return status, nil
}

// RestoreStatus represents the status of a restore operation
type RestoreStatus struct {
	ClusterName string             `json:"clusterName"`
	Namespace   string             `json:"namespace"`
	Phase       types.ClusterPhase `json:"phase"`
	Ready       bool               `json:"ready"`
	Message     string             `json:"message,omitempty"`
}
