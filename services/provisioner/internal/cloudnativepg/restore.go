// Package cloudnativepg provides restore functionality for CloudNativePG clusters.
package cloudnativepg

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"

	"github.com/orochi-db/orochi-db/services/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/config"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
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

// CreateBranch creates a database branch using the optimal method available
func (m *RestoreManager) CreateBranch(ctx context.Context, branchSpec *types.BranchSpec) (*unstructured.Unstructured, *types.BranchStatus, error) {
	startTime := time.Now()

	m.logger.Info("creating database branch",
		zap.String("branchName", branchSpec.Name),
		zap.String("sourceCluster", branchSpec.SourceCluster),
		zap.String("sourceNamespace", branchSpec.SourceNamespace),
		zap.String("method", string(branchSpec.Method)),
	)

	// Validate branch spec
	if err := branchSpec.Validate(); err != nil {
		return nil, nil, fmt.Errorf("invalid branch spec: %w", err)
	}

	// Determine target namespace
	targetNamespace := branchSpec.TargetNamespace
	if targetNamespace == "" {
		targetNamespace = branchSpec.SourceNamespace
	}

	// Get source cluster to inherit configuration
	sourceCluster, err := m.clusterManager.GetCluster(ctx, branchSpec.SourceNamespace, branchSpec.SourceCluster)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to get source cluster: %w", err)
	}

	// Build target cluster spec
	targetSpec := m.buildBranchClusterSpec(branchSpec, sourceCluster, targetNamespace)

	var cluster *unstructured.Unstructured
	var method types.BranchMethod

	// Select branching method
	switch branchSpec.Method {
	case types.BranchMethodVolumeSnapshot:
		cluster, err = m.createBranchFromVolumeSnapshot(ctx, branchSpec, targetSpec)
		method = types.BranchMethodVolumeSnapshot
	case types.BranchMethodPITR:
		cluster, err = m.createBranchFromPITR(ctx, branchSpec, targetSpec)
		method = types.BranchMethodPITR
	case types.BranchMethodClone:
		// Use PG18+ file_copy_method=clone with XFS reflinks
		cluster, err = m.CloneCluster(ctx, branchSpec.SourceCluster, branchSpec.SourceNamespace, targetSpec)
		method = types.BranchMethodClone
	case types.BranchMethodPgBasebackup, "":
		// Default to pg_basebackup
		cluster, err = m.CloneCluster(ctx, branchSpec.SourceCluster, branchSpec.SourceNamespace, targetSpec)
		method = types.BranchMethodPgBasebackup
	default:
		return nil, nil, fmt.Errorf("unsupported branch method: %s", branchSpec.Method)
	}

	if err != nil {
		return nil, nil, fmt.Errorf("failed to create branch: %w", err)
	}

	// Build branch status
	status := &types.BranchStatus{
		Phase:            types.BranchPhaseCreating,
		ClusterName:      branchSpec.Name,
		CreationMethod:   method,
		CreationDuration: time.Since(startTime).String(),
	}

	return cluster, status, nil
}

// createBranchFromVolumeSnapshot creates a branch using CSI volume snapshots (instant)
func (m *RestoreManager) createBranchFromVolumeSnapshot(ctx context.Context, branchSpec *types.BranchSpec, targetSpec *types.ClusterSpec) (*unstructured.Unstructured, error) {
	m.logger.Info("creating branch from volume snapshot",
		zap.String("branchName", branchSpec.Name),
		zap.String("sourceCluster", branchSpec.SourceCluster),
	)

	// Build bootstrap configuration for volume snapshot recovery
	bootstrap := map[string]interface{}{
		"recovery": map[string]interface{}{
			"source": branchSpec.SourceCluster,
			"volumeSnapshots": map[string]interface{}{
				"storage": map[string]interface{}{
					"name": fmt.Sprintf("%s-snapshot", branchSpec.SourceCluster),
				},
			},
		},
	}

	// Build external cluster reference for the snapshot source
	externalClusters := []interface{}{
		map[string]interface{}{
			"name": branchSpec.SourceCluster,
			"connectionParameters": map[string]interface{}{
				"host":   fmt.Sprintf("%s-rw.%s.svc", branchSpec.SourceCluster, branchSpec.SourceNamespace),
				"user":   "streaming_replica",
				"dbname": "postgres",
			},
		},
	}

	// Create the target cluster spec
	cluster := m.clusterManager.buildClusterSpec(targetSpec)
	specObj := cluster.Object["spec"].(map[string]interface{})
	specObj["bootstrap"] = bootstrap
	specObj["externalClusters"] = externalClusters

	// Add branch-specific labels
	labels := cluster.GetLabels()
	if labels == nil {
		labels = make(map[string]string)
	}
	labels["orochi.io/branch"] = "true"
	labels["orochi.io/parent-cluster"] = branchSpec.SourceCluster
	labels["orochi.io/branch-method"] = string(types.BranchMethodVolumeSnapshot)
	cluster.SetLabels(labels)

	// Create the cluster
	if err := m.k8sClient.WithRetry(ctx, "create branch from volume snapshot", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, cluster)
	}); err != nil {
		return nil, fmt.Errorf("failed to create branch cluster: %w", err)
	}

	return cluster, nil
}

// createBranchFromPITR creates a branch at a specific point in time
func (m *RestoreManager) createBranchFromPITR(ctx context.Context, branchSpec *types.BranchSpec, targetSpec *types.ClusterSpec) (*unstructured.Unstructured, error) {
	m.logger.Info("creating branch from PITR",
		zap.String("branchName", branchSpec.Name),
		zap.String("sourceCluster", branchSpec.SourceCluster),
		zap.String("pointInTime", branchSpec.PointInTime),
		zap.String("lsn", branchSpec.LSN),
	)

	options := &types.RestoreOptions{
		RecoveryTargetTime:      branchSpec.PointInTime,
		RecoveryTargetLSN:       branchSpec.LSN,
		RecoveryTargetInclusive: true,
	}

	return m.RestoreFromBackup(ctx, branchSpec.SourceCluster, branchSpec.SourceNamespace, targetSpec, options)
}

// buildBranchClusterSpec builds the cluster spec for a branch
func (m *RestoreManager) buildBranchClusterSpec(branchSpec *types.BranchSpec, sourceCluster *unstructured.Unstructured, targetNamespace string) *types.ClusterSpec {
	// Start with defaults
	spec := types.DefaultClusterSpec(branchSpec.Name, targetNamespace)

	// Override instance count (default to 1 for branches to save resources)
	if branchSpec.Instances > 0 {
		spec.Instances = branchSpec.Instances
	} else {
		spec.Instances = 1
	}

	// Inherit configuration from source cluster if requested
	if branchSpec.Inherit {
		// Extract source cluster configuration
		sourceSpec, found, _ := unstructured.NestedMap(sourceCluster.Object, "spec")
		if found {
			// Inherit storage configuration
			if storageMap, ok, _ := unstructured.NestedMap(sourceSpec, "storage"); ok {
				if size, ok := storageMap["size"].(string); ok {
					spec.Storage.Size = size
				}
				if sc, ok := storageMap["storageClass"].(string); ok {
					spec.Storage.StorageClass = sc
				}
			}

			// Inherit resource configuration
			if resourcesMap, ok, _ := unstructured.NestedMap(sourceSpec, "resources"); ok {
				if requests, ok := resourcesMap["requests"].(map[string]interface{}); ok {
					if cpu, ok := requests["cpu"].(string); ok {
						spec.Resources.CPURequest = cpu
					}
					if mem, ok := requests["memory"].(string); ok {
						spec.Resources.MemoryRequest = mem
					}
				}
				if limits, ok := resourcesMap["limits"].(map[string]interface{}); ok {
					if cpu, ok := limits["cpu"].(string); ok {
						spec.Resources.CPULimit = cpu
					}
					if mem, ok := limits["memory"].(string); ok {
						spec.Resources.MemoryLimit = mem
					}
				}
			}
		}
	}

	// Apply branch-specific labels
	spec.Labels = make(map[string]string)
	spec.Labels["orochi.io/branch"] = "true"
	spec.Labels["orochi.io/parent-cluster"] = branchSpec.SourceCluster
	spec.Labels["orochi.io/parent-namespace"] = branchSpec.SourceNamespace

	// Merge user-provided labels
	for k, v := range branchSpec.Labels {
		spec.Labels[k] = v
	}

	// Apply annotations
	spec.Annotations = make(map[string]string)
	spec.Annotations["orochi.io/branch-created-at"] = time.Now().UTC().Format(time.RFC3339)
	for k, v := range branchSpec.Annotations {
		spec.Annotations[k] = v
	}

	return &spec
}

// ListBranches lists all branches for a source cluster
func (m *RestoreManager) ListBranches(ctx context.Context, sourceCluster, sourceNamespace string) ([]*types.BranchInfo, error) {
	m.logger.Info("listing branches",
		zap.String("sourceCluster", sourceCluster),
		zap.String("sourceNamespace", sourceNamespace),
	)

	// Find clusters with branch labels pointing to this source
	labelSelector := map[string]string{
		"orochi.io/branch":         "true",
		"orochi.io/parent-cluster": sourceCluster,
	}

	clusters, err := m.clusterManager.ListClusters(ctx, "", labelSelector)
	if err != nil {
		return nil, fmt.Errorf("failed to list branch clusters: %w", err)
	}

	branches := make([]*types.BranchInfo, 0, len(clusters.Items))
	for _, cluster := range clusters.Items {
		name, _, _ := unstructured.NestedString(cluster.Object, "metadata", "name")
		namespace, _, _ := unstructured.NestedString(cluster.Object, "metadata", "namespace")
		phase, _, _ := unstructured.NestedString(cluster.Object, "status", "phase")
		creationTimestamp := cluster.GetCreationTimestamp()
		labels := cluster.GetLabels()

		branchInfo := &types.BranchInfo{
			BranchID:        string(cluster.GetUID()),
			Name:            name,
			ParentCluster:   sourceCluster,
			ParentNamespace: sourceNamespace,
			Namespace:       namespace,
			Status: types.BranchStatus{
				Phase:       types.BranchPhase(m.mapClusterPhaseToBranchPhase(phase)),
				ClusterName: name,
			},
			CreatedAt: creationTimestamp.Time,
			Labels:    labels,
		}

		// Get branch method from labels
		if method, ok := labels["orochi.io/branch-method"]; ok {
			branchInfo.Status.CreationMethod = types.BranchMethod(method)
		}

		branches = append(branches, branchInfo)
	}

	return branches, nil
}

// DeleteBranch deletes a branch
func (m *RestoreManager) DeleteBranch(ctx context.Context, branchName, namespace string) error {
	m.logger.Info("deleting branch",
		zap.String("branchName", branchName),
		zap.String("namespace", namespace),
	)

	// Verify this is actually a branch
	cluster, err := m.clusterManager.GetCluster(ctx, namespace, branchName)
	if err != nil {
		return fmt.Errorf("failed to get branch cluster: %w", err)
	}

	labels := cluster.GetLabels()
	if labels["orochi.io/branch"] != "true" {
		return fmt.Errorf("cluster %s is not a branch", branchName)
	}

	// Delete the cluster
	return m.clusterManager.DeleteCluster(ctx, namespace, branchName, true)
}

// GetBranchStatus returns the status of a branch
func (m *RestoreManager) GetBranchStatus(ctx context.Context, branchName, namespace string) (*types.BranchStatus, error) {
	cluster, err := m.clusterManager.GetCluster(ctx, namespace, branchName)
	if err != nil {
		return nil, err
	}

	phase, _, _ := unstructured.NestedString(cluster.Object, "status", "phase")
	labels := cluster.GetLabels()

	status := &types.BranchStatus{
		Phase:       types.BranchPhase(m.mapClusterPhaseToBranchPhase(phase)),
		ClusterName: branchName,
	}

	if method, ok := labels["orochi.io/branch-method"]; ok {
		status.CreationMethod = types.BranchMethod(method)
	}

	// Try to get connection string
	if connStr, err := m.clusterManager.GetConnectionString(ctx, namespace, branchName); err == nil {
		status.ConnectionString = connStr
	}

	// Try to get pooler connection
	if poolerStr, err := m.clusterManager.GetPoolerConnectionString(ctx, namespace, branchName); err == nil {
		status.PoolerConnection = poolerStr
	}

	return status, nil
}

// mapClusterPhaseToBranchPhase maps cluster phases to branch phases
func (m *RestoreManager) mapClusterPhaseToBranchPhase(clusterPhase string) string {
	switch clusterPhase {
	case "Cluster in healthy state":
		return string(types.BranchPhaseReady)
	case "Setting up":
		return string(types.BranchPhaseCreating)
	case "Deleting":
		return string(types.BranchPhaseDeleting)
	case "Cluster in unhealthy state":
		return string(types.BranchPhaseFailed)
	default:
		return string(types.BranchPhasePending)
	}
}
