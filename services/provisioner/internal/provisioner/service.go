// Package provisioner provides the main provisioner service logic.
package provisioner

import (
	"context"
	"fmt"
	"sync"
	"time"

	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/api/errors"

	"github.com/orochi-db/orochi-db/services/provisioner/internal/cloudnativepg"
	"github.com/orochi-db/orochi-db/services/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/config"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

// Service provides cluster provisioning operations
type Service struct {
	cfg             *config.Config
	k8sClient       *k8s.Client
	resourceManager *k8s.ResourceManager
	clusterManager  *cloudnativepg.ClusterManager
	backupManager   *cloudnativepg.BackupManager
	restoreManager  *cloudnativepg.RestoreManager
	templateEngine  *TemplateEngine
	logger          *zap.Logger
	mu              sync.RWMutex
}

// NewService creates a new provisioner service
func NewService(cfg *config.Config, logger *zap.Logger) (*Service, error) {
	// Initialize Kubernetes client
	k8sClient, err := k8s.NewClient(&cfg.Kubernetes, logger)
	if err != nil {
		return nil, fmt.Errorf("failed to create Kubernetes client: %w", err)
	}

	// Initialize resource manager
	resourceManager := k8s.NewResourceManager(k8sClient, logger)

	// Initialize CloudNativePG managers
	clusterManager := cloudnativepg.NewClusterManager(k8sClient, &cfg.CloudNativePG, logger)
	backupManager := cloudnativepg.NewBackupManager(k8sClient, &cfg.CloudNativePG, logger)
	restoreManager := cloudnativepg.NewRestoreManager(k8sClient, clusterManager, &cfg.CloudNativePG, logger)

	// Initialize template engine
	templateEngine, err := NewTemplateEngine(logger)
	if err != nil {
		return nil, fmt.Errorf("failed to create template engine: %w", err)
	}

	return &Service{
		cfg:             cfg,
		k8sClient:       k8sClient,
		resourceManager: resourceManager,
		clusterManager:  clusterManager,
		backupManager:   backupManager,
		restoreManager:  restoreManager,
		templateEngine:  templateEngine,
		logger:          logger,
	}, nil
}

// CreateCluster creates a new PostgreSQL cluster
func (s *Service) CreateCluster(ctx context.Context, spec *types.ClusterSpec, waitForReady bool, timeoutSeconds int32) (*types.ClusterInfo, error) {
	s.logger.Info("creating cluster",
		zap.String("name", spec.Name),
		zap.String("namespace", spec.Namespace),
		zap.Bool("waitForReady", waitForReady),
	)

	// Hold lock only for quick validation and defaults - not during long operations
	s.mu.Lock()
	// Validate the spec
	if err := spec.Validate(); err != nil {
		s.mu.Unlock()
		return nil, fmt.Errorf("invalid cluster spec: %w", err)
	}

	// Apply defaults
	s.applyDefaults(spec)
	s.mu.Unlock()

	// Ensure namespace exists (no lock needed - underlying client is thread-safe)
	if err := s.resourceManager.EnsureNamespace(ctx, spec.Namespace, map[string]string{
		"app.kubernetes.io/managed-by": "orochi-provisioner",
	}); err != nil {
		return nil, fmt.Errorf("failed to ensure namespace: %w", err)
	}

	// Create any required secrets for Orochi configuration
	if spec.OrochiConfig != nil && spec.OrochiConfig.Tiering != nil && spec.OrochiConfig.Tiering.S3Config != nil {
		if err := s.createTieringSecrets(ctx, spec); err != nil {
			return nil, fmt.Errorf("failed to create tiering secrets: %w", err)
		}
	}

	// Create the CloudNativePG cluster (underlying manager handles its own synchronization)
	cluster, err := s.clusterManager.CreateCluster(ctx, spec)
	if err != nil {
		return nil, err
	}

	// Wait for cluster to be ready if requested
	// IMPORTANT: This can take up to 10 minutes - NO lock held during wait
	if waitForReady {
		timeout := time.Duration(timeoutSeconds) * time.Second
		if timeout == 0 {
			timeout = s.cfg.CloudNativePG.ClusterReadyTimeout
		}

		if err := s.clusterManager.WaitForClusterReady(ctx, spec.Namespace, spec.Name, timeout); err != nil {
			return nil, fmt.Errorf("cluster did not become ready: %w", err)
		}

		// Initialize Orochi extension if enabled
		if spec.OrochiConfig != nil && spec.OrochiConfig.Enabled {
			if err := s.initializeOrochiExtension(ctx, spec); err != nil {
				s.logger.Warn("failed to initialize Orochi extension",
					zap.String("cluster", spec.Name),
					zap.Error(err),
				)
			}
		}
	}

	// Create scheduled backup if configured (non-critical, no lock needed)
	if spec.BackupConfig != nil && spec.BackupConfig.ScheduledBackup != nil && spec.BackupConfig.ScheduledBackup.Enabled {
		if _, err := s.backupManager.CreateScheduledBackup(
			ctx,
			spec.Namespace,
			spec.Name,
			spec.BackupConfig.ScheduledBackup.Schedule,
			spec.BackupConfig.ScheduledBackup.Immediate,
		); err != nil {
			s.logger.Warn("failed to create scheduled backup",
				zap.String("cluster", spec.Name),
				zap.Error(err),
			)
		}
	}

	// Create monitoring resources if configured (non-critical, no lock needed)
	if spec.Monitoring != nil && spec.Monitoring.EnablePodMonitor {
		if err := s.createMonitoringResources(ctx, spec); err != nil {
			s.logger.Warn("failed to create monitoring resources",
				zap.String("cluster", spec.Name),
				zap.Error(err),
			)
		}
	}

	// Build and return cluster info
	return s.buildClusterInfo(ctx, cluster, spec)
}

// UpdateCluster updates an existing cluster
func (s *Service) UpdateCluster(ctx context.Context, spec *types.ClusterSpec, rollingUpdate bool) (*types.ClusterInfo, error) {
	s.logger.Info("updating cluster",
		zap.String("name", spec.Name),
		zap.String("namespace", spec.Namespace),
		zap.Bool("rollingUpdate", rollingUpdate),
	)

	// Hold lock only for quick validation and defaults
	s.mu.Lock()
	// Validate the spec
	if err := spec.Validate(); err != nil {
		s.mu.Unlock()
		return nil, fmt.Errorf("invalid cluster spec: %w", err)
	}

	// Apply defaults
	s.applyDefaults(spec)
	s.mu.Unlock()

	// Update the cluster (underlying manager handles its own synchronization)
	cluster, err := s.clusterManager.UpdateCluster(ctx, spec)
	if err != nil {
		return nil, err
	}

	return s.buildClusterInfo(ctx, cluster, spec)
}

// DeleteCluster deletes a cluster
func (s *Service) DeleteCluster(ctx context.Context, namespace, name string, deleteBackups, deletePVCs bool) error {
	s.logger.Info("deleting cluster",
		zap.String("name", name),
		zap.String("namespace", namespace),
		zap.Bool("deleteBackups", deleteBackups),
		zap.Bool("deletePVCs", deletePVCs),
	)

	// No service-level lock needed - underlying managers handle their own synchronization
	// All operations below are independent and can proceed without blocking other service calls

	// Delete scheduled backups
	scheduledBackups, err := s.backupManager.ListScheduledBackups(ctx, namespace, name)
	if err == nil {
		for _, sb := range scheduledBackups.Items {
			sbName := sb.GetName()
			if err := s.backupManager.DeleteScheduledBackup(ctx, namespace, sbName); err != nil {
				s.logger.Warn("failed to delete scheduled backup",
					zap.String("name", sbName),
					zap.Error(err),
				)
			}
		}
	}

	// Delete backups if requested
	if deleteBackups {
		backups, err := s.backupManager.ListBackups(ctx, namespace, name)
		if err == nil {
			for _, backup := range backups.Items {
				backupName := backup.GetName()
				if err := s.backupManager.DeleteBackup(ctx, namespace, backupName); err != nil {
					s.logger.Warn("failed to delete backup",
						zap.String("name", backupName),
						zap.Error(err),
					)
				}
			}
		}
	}

	// Delete the cluster
	if err := s.clusterManager.DeleteCluster(ctx, namespace, name, deletePVCs); err != nil {
		return err
	}

	// Delete PVCs if requested (CloudNativePG might not clean them up automatically)
	if deletePVCs {
		pvcs, err := s.resourceManager.ListPVCs(ctx, namespace, map[string]string{
			"orochi.io/cluster-name": name,
		})
		if err == nil {
			for _, pvc := range pvcs.Items {
				if err := s.resourceManager.DeletePVC(ctx, namespace, pvc.Name); err != nil {
					s.logger.Warn("failed to delete PVC",
						zap.String("name", pvc.Name),
						zap.Error(err),
					)
				}
			}
		}
	}

	return nil
}

// GetCluster retrieves a cluster
func (s *Service) GetCluster(ctx context.Context, namespace, name string) (*types.ClusterInfo, error) {
	// No lock needed - underlying client is thread-safe
	cluster, err := s.clusterManager.GetCluster(ctx, namespace, name)
	if err != nil {
		if errors.IsNotFound(err) {
			return nil, fmt.Errorf("cluster %s/%s not found", namespace, name)
		}
		return nil, err
	}

	return s.buildClusterInfoFromCNPG(ctx, cluster)
}

// ListClusters lists clusters in a namespace
func (s *Service) ListClusters(ctx context.Context, namespace string, labelSelector map[string]string, limit int32) ([]*types.ClusterInfo, error) {
	// No lock needed - underlying client is thread-safe
	clusters, err := s.clusterManager.ListClusters(ctx, namespace, labelSelector)
	if err != nil {
		return nil, err
	}

	result := make([]*types.ClusterInfo, 0, len(clusters.Items))
	for _, cluster := range clusters.Items {
		info, err := s.buildClusterInfoFromCNPG(ctx, &cluster)
		if err != nil {
			s.logger.Warn("failed to build cluster info",
				zap.String("cluster", cluster.GetName()),
				zap.Error(err),
			)
			continue
		}
		result = append(result, info)

		if limit > 0 && int32(len(result)) >= limit {
			break
		}
	}

	return result, nil
}

// GetClusterStatus returns the status of a cluster
func (s *Service) GetClusterStatus(ctx context.Context, namespace, name string) (*types.ClusterStatus, error) {
	// No lock needed - underlying client is thread-safe
	return s.clusterManager.GetClusterStatus(ctx, namespace, name)
}

// CreateBackup creates a backup
func (s *Service) CreateBackup(ctx context.Context, namespace, clusterName, backupName string, method types.BackupMethod) (*types.BackupInfo, error) {
	// No lock needed - underlying manager handles its own synchronization
	backup, err := s.backupManager.CreateBackup(ctx, namespace, clusterName, backupName, method)
	if err != nil {
		return nil, err
	}

	return s.backupManager.GetBackupInfo(ctx, namespace, backup.GetName())
}

// ListBackups lists backups for a cluster
func (s *Service) ListBackups(ctx context.Context, namespace, clusterName string) ([]*types.BackupInfo, error) {
	// No lock needed - underlying client is thread-safe
	backups, err := s.backupManager.ListBackups(ctx, namespace, clusterName)
	if err != nil {
		return nil, err
	}

	result := make([]*types.BackupInfo, 0, len(backups.Items))
	for _, backup := range backups.Items {
		backupName := backup.GetName()
		info, err := s.backupManager.GetBackupInfo(ctx, namespace, backupName)
		if err != nil {
			s.logger.Warn("failed to get backup info",
				zap.String("backup", backupName),
				zap.Error(err),
			)
			continue
		}
		result = append(result, info)
	}

	return result, nil
}

// DeleteBackup deletes a backup
func (s *Service) DeleteBackup(ctx context.Context, namespace, name string) error {
	// No lock needed - underlying manager handles its own synchronization
	return s.backupManager.DeleteBackup(ctx, namespace, name)
}

// RestoreCluster restores a cluster from a backup
func (s *Service) RestoreCluster(ctx context.Context, sourceClusterID, sourceNamespace string, targetSpec *types.ClusterSpec, options *types.RestoreOptions) (*types.ClusterInfo, error) {
	// Validate restore options (no lock needed for validation)
	if err := s.restoreManager.ValidateRestoreOptions(options); err != nil {
		return nil, err
	}

	// Hold lock only for quick validation and defaults
	s.mu.Lock()
	// Validate target spec
	if err := targetSpec.Validate(); err != nil {
		s.mu.Unlock()
		return nil, fmt.Errorf("invalid target cluster spec: %w", err)
	}

	// Apply defaults
	s.applyDefaults(targetSpec)
	s.mu.Unlock()

	// Perform the restore (can be long-running - no lock held)
	cluster, err := s.restoreManager.RestoreFromBackup(ctx, sourceClusterID, sourceNamespace, targetSpec, options)
	if err != nil {
		return nil, err
	}

	return s.buildClusterInfoFromCNPG(ctx, cluster)
}

// HealthCheck performs a health check
func (s *Service) HealthCheck(ctx context.Context) error {
	return s.k8sClient.HealthCheck(ctx)
}

// applyDefaults applies default values to a cluster spec
func (s *Service) applyDefaults(spec *types.ClusterSpec) {
	if spec.PostgresVersion == "" {
		spec.PostgresVersion = s.cfg.CloudNativePG.DefaultPostgresVersion
	}

	if spec.Storage.StorageClass == "" {
		spec.Storage.StorageClass = s.cfg.CloudNativePG.DefaultStorageClass
	}

	if spec.OrochiConfig == nil {
		spec.OrochiConfig = &types.OrochiConfig{
			Enabled:            true,
			DefaultShardCount:  s.cfg.OrochiDefaults.DefaultShardCount,
			ChunkInterval:      s.cfg.OrochiDefaults.DefaultChunkInterval,
			DefaultCompression: types.CompressionType(s.cfg.OrochiDefaults.DefaultCompression),
			EnableColumnar:     s.cfg.OrochiDefaults.EnableColumnarByDefault,
		}
	}
}

// createTieringSecrets creates secrets for tiered storage S3 access
func (s *Service) createTieringSecrets(ctx context.Context, spec *types.ClusterSpec) error {
	s3Config := spec.OrochiConfig.Tiering.S3Config
	if s3Config == nil {
		return nil
	}

	// If no access key secret is specified, assume IAM role-based authentication
	if s3Config.AccessKeySecret == "" {
		s.logger.Info("no S3 access key secret specified, assuming IAM role-based authentication",
			zap.String("cluster", spec.Name),
		)
		return nil
	}

	// Verify the secret exists and is accessible
	secret, err := s.resourceManager.GetSecret(ctx, spec.Namespace, s3Config.AccessKeySecret)
	if err != nil {
		return fmt.Errorf("tiering S3 credentials secret not found: %w", err)
	}

	// Validate the secret has the required keys
	requiredKeys := []string{"AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY"}
	for _, key := range requiredKeys {
		if _, ok := secret.Data[key]; !ok {
			s.logger.Warn("S3 credentials secret missing required key",
				zap.String("secret", s3Config.AccessKeySecret),
				zap.String("missingKey", key),
			)
			return fmt.Errorf("S3 credentials secret %s missing required key: %s", s3Config.AccessKeySecret, key)
		}
	}

	s.logger.Info("validated S3 credentials secret for tiered storage",
		zap.String("cluster", spec.Name),
		zap.String("secret", s3Config.AccessKeySecret),
		zap.String("endpoint", s3Config.Endpoint),
		zap.String("bucket", s3Config.Bucket),
	)

	return nil
}

// initializeOrochiExtension initializes the Orochi extension in the database
func (s *Service) initializeOrochiExtension(ctx context.Context, spec *types.ClusterSpec) error {
	s.logger.Info("initializing Orochi extension",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	// The extension is loaded via shared_preload_libraries, but we need to CREATE EXTENSION
	// This would typically be done via a Job or init container
	// For now, we just log that it should be done

	s.logger.Info("Orochi extension should be created via: CREATE EXTENSION IF NOT EXISTS orochi;",
		zap.String("cluster", spec.Name),
	)

	return nil
}

// createMonitoringResources creates Prometheus monitoring resources
func (s *Service) createMonitoringResources(ctx context.Context, spec *types.ClusterSpec) error {
	s.logger.Info("creating monitoring resources",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	// Create custom queries ConfigMap if specified
	if spec.Monitoring.CustomQueries != nil && len(spec.Monitoring.CustomQueries) > 0 {
		configMapName := fmt.Sprintf("%s-monitoring-queries", spec.Name)
		if err := s.resourceManager.CreateConfigMap(ctx, spec.Namespace, configMapName, spec.Monitoring.CustomQueries); err != nil {
			return fmt.Errorf("failed to create monitoring queries ConfigMap: %w", err)
		}
	}

	return nil
}

// buildClusterInfo builds cluster info from our spec and CNPG cluster
func (s *Service) buildClusterInfo(ctx context.Context, cluster interface{}, spec *types.ClusterSpec) (*types.ClusterInfo, error) {
	status, err := s.clusterManager.GetClusterStatus(ctx, spec.Namespace, spec.Name)
	if err != nil {
		// Cluster might not be ready yet
		status = &types.ClusterStatus{
			Phase:          types.ClusterPhaseSettingUp,
			TotalInstances: spec.Instances,
		}
	}

	return &types.ClusterInfo{
		ClusterID: fmt.Sprintf("%s/%s", spec.Namespace, spec.Name),
		Name:      spec.Name,
		Namespace: spec.Namespace,
		Spec:      *spec,
		Status:    *status,
		CreatedAt: time.Now(),
		UpdatedAt: time.Now(),
	}, nil
}

// buildClusterInfoFromCNPG builds cluster info from a CNPG cluster (unstructured.Unstructured)
func (s *Service) buildClusterInfoFromCNPG(ctx context.Context, cluster interface{}) (*types.ClusterInfo, error) {
	// Handle *unstructured.Unstructured from Kubernetes dynamic client
	unstructuredCluster, ok := cluster.(interface {
		GetName() string
		GetNamespace() string
		GetCreationTimestamp() interface{ Time() interface{} }
	})
	if !ok {
		// Try direct interface access for other types
		type clusterLike interface {
			GetName() string
			GetNamespace() string
		}
		if cl, ok := cluster.(clusterLike); ok {
			name := cl.GetName()
			namespace := cl.GetNamespace()

			status, err := s.clusterManager.GetClusterStatus(ctx, namespace, name)
			if err != nil {
				status = &types.ClusterStatus{
					Phase: types.ClusterPhaseUnknown,
				}
			}

			return &types.ClusterInfo{
				ClusterID: fmt.Sprintf("%s/%s", namespace, name),
				Name:      name,
				Namespace: namespace,
				Status:    *status,
				CreatedAt: time.Now(),
				UpdatedAt: time.Now(),
			}, nil
		}
		return nil, fmt.Errorf("unexpected cluster type: %T", cluster)
	}

	name := unstructuredCluster.GetName()
	namespace := unstructuredCluster.GetNamespace()

	status, err := s.clusterManager.GetClusterStatus(ctx, namespace, name)
	if err != nil {
		status = &types.ClusterStatus{
			Phase: types.ClusterPhaseUnknown,
		}
	}

	return &types.ClusterInfo{
		ClusterID: fmt.Sprintf("%s/%s", namespace, name),
		Name:      name,
		Namespace: namespace,
		Status:    *status,
		CreatedAt: time.Now(),
		UpdatedAt: time.Now(),
	}, nil
}

// GetConnectionString returns the connection string for a cluster
func (s *Service) GetConnectionString(ctx context.Context, namespace, name string) (string, error) {
	return s.clusterManager.GetConnectionString(ctx, namespace, name)
}

// GetPoolerConnectionString returns the connection string via the PgBouncer pooler
func (s *Service) GetPoolerConnectionString(ctx context.Context, namespace, name string) (string, error) {
	return s.clusterManager.GetPoolerConnectionString(ctx, namespace, name)
}

// GetConnectionInfo returns both direct and pooler connection strings
func (s *Service) GetConnectionInfo(ctx context.Context, namespace, name string) (direct, pooler string, err error) {
	return s.clusterManager.GetConnectionInfo(ctx, namespace, name)
}
