// Package cloudnativepg provides integration with the CloudNativePG operator.
package cloudnativepg

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"
	"k8s.io/apimachinery/pkg/runtime/schema"
	"sigs.k8s.io/controller-runtime/pkg/client"

	"github.com/orochi-db/orochi-db/services/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/config"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

// CloudNativePG GVR for Cluster resources
var ClusterGVR = schema.GroupVersionResource{
	Group:    "postgresql.cnpg.io",
	Version:  "v1",
	Resource: "clusters",
}

// ClusterManager manages CloudNativePG Cluster resources
type ClusterManager struct {
	k8sClient *k8s.Client
	cfg       *config.CloudNativePGConfig
	logger    *zap.Logger
}

// NewClusterManager creates a new ClusterManager
func NewClusterManager(k8sClient *k8s.Client, cfg *config.CloudNativePGConfig, logger *zap.Logger) *ClusterManager {
	return &ClusterManager{
		k8sClient: k8sClient,
		cfg:       cfg,
		logger:    logger,
	}
}

// CreateCluster creates a new CloudNativePG cluster
func (m *ClusterManager) CreateCluster(ctx context.Context, spec *types.ClusterSpec) (*unstructured.Unstructured, error) {
	cluster := m.buildClusterSpec(spec)

	m.logger.Info("creating CloudNativePG cluster",
		zap.String("name", spec.Name),
		zap.String("namespace", spec.Namespace),
		zap.Int32("instances", spec.Instances),
	)

	if err := m.k8sClient.WithRetry(ctx, "create cluster", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, cluster)
	}); err != nil {
		return nil, fmt.Errorf("failed to create cluster %s/%s: %w", spec.Namespace, spec.Name, err)
	}

	return cluster, nil
}

// UpdateCluster updates an existing CloudNativePG cluster
func (m *ClusterManager) UpdateCluster(ctx context.Context, spec *types.ClusterSpec) (*unstructured.Unstructured, error) {
	existing, err := m.GetCluster(ctx, spec.Namespace, spec.Name)
	if err != nil {
		return nil, fmt.Errorf("failed to get cluster %s/%s: %w", spec.Namespace, spec.Name, err)
	}

	// Update the spec while preserving metadata
	updated := m.buildClusterSpec(spec)
	updated.SetResourceVersion(existing.GetResourceVersion())
	updated.SetUID(existing.GetUID())
	updated.SetCreationTimestamp(existing.GetCreationTimestamp())

	m.logger.Info("updating CloudNativePG cluster",
		zap.String("name", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	if err := m.k8sClient.WithRetry(ctx, "update cluster", func() error {
		return m.k8sClient.CtrlClient().Update(ctx, updated)
	}); err != nil {
		return nil, fmt.Errorf("failed to update cluster %s/%s: %w", spec.Namespace, spec.Name, err)
	}

	return updated, nil
}

// DeleteCluster deletes a CloudNativePG cluster
func (m *ClusterManager) DeleteCluster(ctx context.Context, namespace, name string, deletePVCs bool) error {
	cluster := &unstructured.Unstructured{}
	cluster.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "Cluster",
	})
	cluster.SetName(name)
	cluster.SetNamespace(namespace)

	m.logger.Info("deleting CloudNativePG cluster",
		zap.String("name", name),
		zap.String("namespace", namespace),
		zap.Bool("deletePVCs", deletePVCs),
	)

	if err := m.k8sClient.Delete(ctx, cluster); err != nil {
		if !errors.IsNotFound(err) {
			return fmt.Errorf("failed to delete cluster %s/%s: %w", namespace, name, err)
		}
	}

	return nil
}

// GetCluster retrieves a CloudNativePG cluster
func (m *ClusterManager) GetCluster(ctx context.Context, namespace, name string) (*unstructured.Unstructured, error) {
	cluster := &unstructured.Unstructured{}
	cluster.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "Cluster",
	})
	if err := m.k8sClient.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, cluster); err != nil {
		return nil, err
	}
	return cluster, nil
}

// ListClusters lists CloudNativePG clusters
func (m *ClusterManager) ListClusters(ctx context.Context, namespace string, labelSelector map[string]string) (*unstructured.UnstructuredList, error) {
	clusterList := &unstructured.UnstructuredList{}
	clusterList.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "ClusterList",
	})
	opts := []client.ListOption{}

	if namespace != "" {
		opts = append(opts, client.InNamespace(namespace))
	}

	if len(labelSelector) > 0 {
		opts = append(opts, client.MatchingLabels(labelSelector))
	}

	if err := m.k8sClient.List(ctx, clusterList, opts...); err != nil {
		return nil, fmt.Errorf("failed to list clusters: %w", err)
	}

	return clusterList, nil
}

// WaitForClusterReady waits for a cluster to be ready
func (m *ClusterManager) WaitForClusterReady(ctx context.Context, namespace, name string, timeout time.Duration) error {
	m.logger.Info("waiting for cluster to be ready",
		zap.String("name", name),
		zap.String("namespace", namespace),
		zap.Duration("timeout", timeout),
	)

	return m.k8sClient.WaitForCondition(ctx, timeout, func() (bool, error) {
		cluster, err := m.GetCluster(ctx, namespace, name)
		if err != nil {
			if errors.IsNotFound(err) {
				return false, nil
			}
			return false, err
		}

		// Check if cluster is healthy
		status, found, err := unstructured.NestedString(cluster.Object, "status", "phase")
		if err != nil || !found {
			return false, nil
		}

		if status == "Cluster in healthy state" {
			return true, nil
		}

		return false, nil
	})
}

// GetClusterStatus returns the status of a cluster
func (m *ClusterManager) GetClusterStatus(ctx context.Context, namespace, name string) (*types.ClusterStatus, error) {
	cluster, err := m.GetCluster(ctx, namespace, name)
	if err != nil {
		return nil, err
	}

	phase, _, _ := unstructured.NestedString(cluster.Object, "status", "phase")
	readyInstances, _, _ := unstructured.NestedInt64(cluster.Object, "status", "readyInstances")
	totalInstances, _, _ := unstructured.NestedInt64(cluster.Object, "status", "instances")
	currentPrimary, _, _ := unstructured.NestedString(cluster.Object, "status", "currentPrimary")
	currentPrimaryTimestamp, _, _ := unstructured.NestedString(cluster.Object, "status", "currentPrimaryTimestamp")
	firstRecoverabilityPoint, _, _ := unstructured.NestedString(cluster.Object, "status", "firstRecoverabilityPoint")

	status := &types.ClusterStatus{
		Phase:                    types.ClusterPhase(phase),
		ReadyInstances:           int32(readyInstances),
		TotalInstances:           int32(totalInstances),
		CurrentPrimary:           currentPrimary,
		CurrentPrimaryTimestamp:  currentPrimaryTimestamp,
		FirstRecoverabilityPoint: firstRecoverabilityPoint,
		LastSuccessfulBackup:     "",
	}

	return status, nil
}

// buildClusterSpec builds a CloudNativePG Cluster from our spec
func (m *ClusterManager) buildClusterSpec(spec *types.ClusterSpec) *unstructured.Unstructured {
	postgresVersion := spec.PostgresVersion
	if postgresVersion == "" {
		postgresVersion = m.cfg.DefaultPostgresVersion
	}

	imageName := spec.ImageName
	if imageName == "" {
		// Use custom Orochi PostgreSQL image with extension pre-installed
		if m.cfg.DefaultImageCatalog != "" {
			imageName = fmt.Sprintf("%s:%s", m.cfg.DefaultImageCatalog, postgresVersion)
		} else {
			// Use v16 - custom image with orochi extension and all stubs
			imageName = "ghcr.io/jbeck018/orochi-pg:18-v16"
		}
	}

	storageClass := spec.Storage.StorageClass
	if storageClass == "" {
		storageClass = m.cfg.DefaultStorageClass
	}

	cluster := &unstructured.Unstructured{
		Object: map[string]interface{}{
			"apiVersion": "postgresql.cnpg.io/v1",
			"kind":       "Cluster",
			"metadata": map[string]interface{}{
				"name":        spec.Name,
				"namespace":   spec.Namespace,
				"labels":      m.buildLabels(spec),
				"annotations": m.buildAnnotations(spec),
			},
			"spec": map[string]interface{}{
				"instances": int64(spec.Instances),
				"imageName": imageName,
				"imagePullSecrets": []interface{}{
					map[string]interface{}{
						"name": "ghcr-secret",
					},
				},
				"postgresGID":           int64(26),
				"postgresUID":           int64(26),
				"primaryUpdateStrategy": "unsupervised",
				"storage": map[string]interface{}{
					"size":               spec.Storage.Size,
					"storageClass":       storageClass,
					"resizeInUseVolumes": spec.Storage.ResizeInUseVolumes,
				},
				"resources": m.buildResourceRequirements(spec.Resources),
				"postgresql": map[string]interface{}{
					"parameters":               m.buildPostgresParameters(spec),
					"shared_preload_libraries": []interface{}{"orochi"},
				},
				// Bootstrap configuration
				"bootstrap": map[string]interface{}{
					"initdb": map[string]interface{}{
						"postInitSQL": []interface{}{
							"CREATE EXTENSION IF NOT EXISTS orochi",
							"SELECT orochi.initialize()",
						},
					},
				},
			},
		},
	}

	// Configure WAL storage if specified
	if spec.Storage.WALSize != "" {
		walStorageClass := spec.Storage.WALStorageClass
		if walStorageClass == "" {
			walStorageClass = storageClass
		}
		specObj := cluster.Object["spec"].(map[string]interface{})
		specObj["walStorage"] = map[string]interface{}{
			"size":         spec.Storage.WALSize,
			"storageClass": walStorageClass,
		}
	}

	// Configure backup
	if spec.BackupConfig != nil && spec.BackupConfig.BarmanObjectStore != nil {
		specObj := cluster.Object["spec"].(map[string]interface{})
		specObj["backup"] = m.buildBackupConfiguration(spec.BackupConfig)
	}

	// Configure monitoring
	if spec.Monitoring != nil && spec.Monitoring.EnablePodMonitor {
		specObj := cluster.Object["spec"].(map[string]interface{})
		specObj["monitoring"] = map[string]interface{}{
			"enablePodMonitor": spec.Monitoring.EnablePodMonitor,
		}
	}

	// Configure affinity
	if spec.Affinity != nil {
		specObj := cluster.Object["spec"].(map[string]interface{})
		specObj["affinity"] = m.buildAffinityConfiguration(spec.Affinity)
	}

	// Configure connection pooler (PgBouncer)
	if spec.Pooler != nil && spec.Pooler.Enabled {
		specObj := cluster.Object["spec"].(map[string]interface{})
		specObj["pooler"] = m.buildPoolerConfiguration(spec.Pooler)
	}

	// Configure environment variables for S3 tiering credentials
	if spec.OrochiConfig != nil && spec.OrochiConfig.Tiering != nil && spec.OrochiConfig.Tiering.Enabled {
		if spec.OrochiConfig.Tiering.S3Config != nil && spec.OrochiConfig.Tiering.S3Config.AccessKeySecret != "" {
			specObj := cluster.Object["spec"].(map[string]interface{})
			specObj["env"] = []interface{}{
				map[string]interface{}{
					"name": "AWS_ACCESS_KEY_ID",
					"valueFrom": map[string]interface{}{
						"secretKeyRef": map[string]interface{}{
							"name": spec.OrochiConfig.Tiering.S3Config.AccessKeySecret,
							"key":  "AWS_ACCESS_KEY_ID",
						},
					},
				},
				map[string]interface{}{
					"name": "AWS_SECRET_ACCESS_KEY",
					"valueFrom": map[string]interface{}{
						"secretKeyRef": map[string]interface{}{
							"name": spec.OrochiConfig.Tiering.S3Config.AccessKeySecret,
							"key":  "AWS_SECRET_ACCESS_KEY",
						},
					},
				},
			}
		}
	}

	return cluster
}

// buildLabels builds labels for the cluster including owner/tenant isolation
func (m *ClusterManager) buildLabels(spec *types.ClusterSpec) map[string]interface{} {
	labels := map[string]interface{}{
		"app.kubernetes.io/name":       "orochi-db",
		"app.kubernetes.io/instance":   spec.Name,
		"app.kubernetes.io/component":  "database",
		"app.kubernetes.io/managed-by": "orochi-provisioner",
		"orochi.io/cluster-name":       spec.Name,
	}

	// Add owner/tenant labels for isolation (passed via spec.Labels)
	// These are critical for multi-tenant isolation
	if ownerID, ok := spec.Labels["orochi.io/owner-id"]; ok {
		labels["orochi.io/owner-id"] = ownerID
	}
	if orgID, ok := spec.Labels["orochi.io/organization-id"]; ok {
		labels["orochi.io/organization-id"] = orgID
	}
	if tier, ok := spec.Labels["orochi.io/tier"]; ok {
		labels["orochi.io/tier"] = tier
	}

	for k, v := range spec.Labels {
		labels[k] = v
	}

	return labels
}

// buildAnnotations builds annotations for the cluster
func (m *ClusterManager) buildAnnotations(spec *types.ClusterSpec) map[string]interface{} {
	annotations := map[string]interface{}{
		"orochi.io/provisioned-at": time.Now().UTC().Format(time.RFC3339),
	}

	for k, v := range spec.Annotations {
		annotations[k] = v
	}

	return annotations
}

// buildResourceRequirements builds Kubernetes resource requirements
func (m *ClusterManager) buildResourceRequirements(res types.ResourceRequirements) map[string]interface{} {
	requests := map[string]interface{}{}
	limits := map[string]interface{}{}

	if res.CPURequest != "" {
		requests["cpu"] = res.CPURequest
	}
	if res.CPULimit != "" {
		limits["cpu"] = res.CPULimit
	}
	if res.MemoryRequest != "" {
		requests["memory"] = res.MemoryRequest
	}
	if res.MemoryLimit != "" {
		limits["memory"] = res.MemoryLimit
	}
	if res.HugePages2Mi != "" {
		limits["hugepages-2Mi"] = res.HugePages2Mi
	}
	if res.HugePages1Gi != "" {
		limits["hugepages-1Gi"] = res.HugePages1Gi
	}

	return map[string]interface{}{
		"requests": requests,
		"limits":   limits,
	}
}

// buildPostgresParameters builds PostgreSQL configuration parameters
func (m *ClusterManager) buildPostgresParameters(spec *types.ClusterSpec) map[string]interface{} {
	params := map[string]interface{}{
		// Note: shared_preload_libraries is set at the postgresql level in buildClusterSpec,
		// not in parameters (CloudNativePG manages this separately)

		// Performance defaults
		"max_connections":      "200",
		"shared_buffers":       "256MB",
		"effective_cache_size": "768MB",
		"work_mem":             "16MB",
		"maintenance_work_mem": "64MB",

		// WAL configuration
		"wal_buffers":                  "16MB",
		"max_wal_size":                 "4GB",
		"min_wal_size":                 "1GB",
		"checkpoint_completion_target": "0.9",

		// Query planning
		"random_page_cost":         "1.1",
		"effective_io_concurrency": "200",

		// Logging
		"log_statement":              "ddl",
		"log_min_duration_statement": "1000",

		// PostgreSQL 18+ instant cloning via XFS reflinks (316x faster)
		// This enables copy-on-write cloning when using XFS with reflink support
		"file_copy_method": "clone",
	}

	// Apply Orochi-specific configuration
	if spec.OrochiConfig != nil && spec.OrochiConfig.Enabled {
		if spec.OrochiConfig.DefaultShardCount > 0 {
			params["orochi.default_shard_count"] = fmt.Sprintf("%d", spec.OrochiConfig.DefaultShardCount)
		}
		if spec.OrochiConfig.ChunkInterval != "" {
			params["orochi.default_chunk_interval"] = spec.OrochiConfig.ChunkInterval
		}
		if spec.OrochiConfig.DefaultCompression != "" {
			params["orochi.default_compression"] = string(spec.OrochiConfig.DefaultCompression)
		}
		if spec.OrochiConfig.EnableColumnar {
			params["orochi.enable_columnar"] = "on"
		}

		// Apply tiering configuration
		if spec.OrochiConfig.Tiering != nil && spec.OrochiConfig.Tiering.Enabled {
			params["orochi.tiering_enabled"] = "on"

			if spec.OrochiConfig.Tiering.HotDuration != "" {
				params["orochi.tiering_hot_duration"] = spec.OrochiConfig.Tiering.HotDuration
			}
			if spec.OrochiConfig.Tiering.WarmDuration != "" {
				params["orochi.tiering_warm_duration"] = spec.OrochiConfig.Tiering.WarmDuration
			}
			if spec.OrochiConfig.Tiering.ColdDuration != "" {
				params["orochi.tiering_cold_duration"] = spec.OrochiConfig.Tiering.ColdDuration
			}

			// S3 configuration for cold/frozen tiers
			if spec.OrochiConfig.Tiering.S3Config != nil {
				params["orochi.s3_endpoint"] = spec.OrochiConfig.Tiering.S3Config.Endpoint
				params["orochi.s3_bucket"] = spec.OrochiConfig.Tiering.S3Config.Bucket
				if spec.OrochiConfig.Tiering.S3Config.Region != "" {
					params["orochi.s3_region"] = spec.OrochiConfig.Tiering.S3Config.Region
				}
			}
		}

		// Apply custom parameters (can override defaults)
		for k, v := range spec.OrochiConfig.CustomParameters {
			params[k] = v
		}
	}

	return params
}

// buildBackupConfiguration builds the backup configuration
func (m *ClusterManager) buildBackupConfiguration(backupCfg *types.BackupConfiguration) map[string]interface{} {
	if backupCfg.BarmanObjectStore == nil {
		return nil
	}

	barman := backupCfg.BarmanObjectStore
	barmanConfig := map[string]interface{}{
		"destinationPath": barman.DestinationPath,
	}

	if barman.Endpoint != "" {
		barmanConfig["endpointURL"] = barman.Endpoint
	}

	// Configure S3 credentials
	if barman.S3Credentials != nil {
		if barman.S3Credentials.InheritFromIAMRole {
			barmanConfig["s3Credentials"] = map[string]interface{}{
				"inheritFromIAMRole": true,
			}
		} else {
			barmanConfig["s3Credentials"] = map[string]interface{}{
				"accessKeyId": map[string]interface{}{
					"name": barman.S3Credentials.AccessKeyIDSecret,
					"key":  "ACCESS_KEY_ID",
				},
				"secretAccessKey": map[string]interface{}{
					"name": barman.S3Credentials.SecretAccessKeySecret,
					"key":  "SECRET_ACCESS_KEY",
				},
			}
		}
	}

	// Configure WAL archiving
	if barman.WAL != nil {
		barmanConfig["wal"] = map[string]interface{}{
			"compression": barman.WAL.Compression,
		}
	}

	// Configure data backup
	if barman.Data != nil {
		barmanConfig["data"] = map[string]interface{}{
			"compression":         barman.Data.Compression,
			"jobs":                int64(barman.Data.Jobs),
			"immediateCheckpoint": barman.Data.ImmediateCheckpoint,
		}
	}

	backup := map[string]interface{}{
		"barmanObjectStore": barmanConfig,
	}

	if backupCfg.RetentionPolicy != "" {
		backup["retentionPolicy"] = backupCfg.RetentionPolicy
	}

	return backup
}

// buildAffinityConfiguration builds affinity configuration
func (m *ClusterManager) buildAffinityConfiguration(affinity *types.AffinityConfig) map[string]interface{} {
	config := map[string]interface{}{
		"enablePodAntiAffinity": affinity.EnablePodAntiAffinity,
		"topologyKey":           affinity.TopologyKey,
	}

	if len(affinity.NodeSelector) > 0 {
		nodeSelector := map[string]interface{}{}
		for k, v := range affinity.NodeSelector {
			nodeSelector[k] = v
		}
		config["nodeSelector"] = nodeSelector
	}

	if len(affinity.Tolerations) > 0 {
		tolerations := make([]interface{}, 0, len(affinity.Tolerations))
		for _, t := range affinity.Tolerations {
			toleration := map[string]interface{}{
				"key":      t.Key,
				"operator": t.Operator,
				"value":    t.Value,
				"effect":   t.Effect,
			}
			if t.TolerationSeconds != nil {
				toleration["tolerationSeconds"] = *t.TolerationSeconds
			}
			tolerations = append(tolerations, toleration)
		}
		config["tolerations"] = tolerations
	}

	return config
}

// buildPoolerConfiguration builds PgBouncer connection pooler configuration
func (m *ClusterManager) buildPoolerConfiguration(pooler *types.ConnectionPoolerSpec) map[string]interface{} {
	config := map[string]interface{}{
		"instances": int64(pooler.Instances),
		"type":      "rw", // Read-write pooler by default
	}

	if pooler.Type != "" {
		config["type"] = pooler.Type
	}

	if pooler.PgBouncer != nil {
		pgbouncer := map[string]interface{}{
			"poolMode": string(pooler.PgBouncer.PoolMode),
		}

		if pooler.PgBouncer.DefaultPoolSize > 0 {
			pgbouncer["defaultPoolSize"] = int64(pooler.PgBouncer.DefaultPoolSize)
		}

		if pooler.PgBouncer.MaxClientConn > 0 {
			pgbouncer["maxClientConn"] = int64(pooler.PgBouncer.MaxClientConn)
		}

		if len(pooler.PgBouncer.Parameters) > 0 {
			params := map[string]interface{}{}
			for k, v := range pooler.PgBouncer.Parameters {
				params[k] = v
			}
			pgbouncer["parameters"] = params
		}

		config["pgbouncer"] = pgbouncer
	}

	return config
}

// GetConnectionString returns the connection string for a cluster
func (m *ClusterManager) GetConnectionString(ctx context.Context, namespace, name string) (string, error) {
	cluster, err := m.GetCluster(ctx, namespace, name)
	if err != nil {
		return "", err
	}

	clusterName, _, _ := unstructured.NestedString(cluster.Object, "metadata", "name")

	// Get the secret containing the connection info
	secretName := fmt.Sprintf("%s-app", clusterName)
	secret := &corev1.Secret{}
	if err := m.k8sClient.Get(ctx, client.ObjectKey{Namespace: namespace, Name: secretName}, secret); err != nil {
		return "", fmt.Errorf("failed to get connection secret: %w", err)
	}

	if uri, ok := secret.Data["uri"]; ok {
		return string(uri), nil
	}

	return "", fmt.Errorf("connection URI not found in secret %s", secretName)
}

// GetPoolerConnectionString returns the connection string via the PgBouncer pooler
func (m *ClusterManager) GetPoolerConnectionString(ctx context.Context, namespace, name string) (string, error) {
	// The pooler service is named <cluster-name>-pooler-rw for read-write pooler
	poolerSecretName := fmt.Sprintf("%s-pooler-rw", name)
	secret := &corev1.Secret{}
	if err := m.k8sClient.Get(ctx, client.ObjectKey{Namespace: namespace, Name: poolerSecretName}, secret); err != nil {
		// Fall back to regular connection if pooler secret doesn't exist
		m.logger.Debug("pooler secret not found, falling back to direct connection",
			zap.String("cluster", name),
			zap.Error(err),
		)
		return m.GetConnectionString(ctx, namespace, name)
	}

	if uri, ok := secret.Data["uri"]; ok {
		return string(uri), nil
	}

	// Fall back to regular connection if URI not found
	return m.GetConnectionString(ctx, namespace, name)
}

// GetConnectionInfo returns both direct and pooler connection strings
func (m *ClusterManager) GetConnectionInfo(ctx context.Context, namespace, name string) (direct, pooler string, err error) {
	direct, err = m.GetConnectionString(ctx, namespace, name)
	if err != nil {
		return "", "", err
	}

	// Try to get pooler connection, don't fail if it doesn't exist
	pooler, _ = m.GetPoolerConnectionString(ctx, namespace, name)

	return direct, pooler, nil
}

// ScaleCluster scales the number of instances in a cluster
func (m *ClusterManager) ScaleCluster(ctx context.Context, namespace, name string, instances int32) error {
	cluster, err := m.GetCluster(ctx, namespace, name)
	if err != nil {
		return fmt.Errorf("failed to get cluster: %w", err)
	}

	if err := unstructured.SetNestedField(cluster.Object, int64(instances), "spec", "instances"); err != nil {
		return fmt.Errorf("failed to set instances: %w", err)
	}

	return m.k8sClient.CtrlClient().Update(ctx, cluster)
}

// GetClusterGVK returns the GroupVersionKind for CloudNativePG Cluster resources.
func (m *ClusterManager) GetClusterGVK() schema.GroupVersionKind {
	return schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "Cluster",
	}
}
