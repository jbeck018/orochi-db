// Package cloudnativepg provides backup management for CloudNativePG clusters.
package cloudnativepg

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"
	"k8s.io/apimachinery/pkg/runtime/schema"
	"sigs.k8s.io/controller-runtime/pkg/client"

	"github.com/orochi-db/orochi-cloud/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/config"
	"github.com/orochi-db/orochi-cloud/provisioner/pkg/types"
)

// BackupManager manages CloudNativePG Backup resources
type BackupManager struct {
	k8sClient *k8s.Client
	cfg       *config.CloudNativePGConfig
	logger    *zap.Logger
}

// NewBackupManager creates a new BackupManager
func NewBackupManager(k8sClient *k8s.Client, cfg *config.CloudNativePGConfig, logger *zap.Logger) *BackupManager {
	return &BackupManager{
		k8sClient: k8sClient,
		cfg:       cfg,
		logger:    logger,
	}
}

// CreateBackup creates a new backup for a cluster
func (m *BackupManager) CreateBackup(ctx context.Context, namespace, clusterName, backupName string, method types.BackupMethod) (*unstructured.Unstructured, error) {
	if backupName == "" {
		backupName = fmt.Sprintf("%s-backup-%d", clusterName, time.Now().Unix())
	}

	backup := &unstructured.Unstructured{
		Object: map[string]interface{}{
			"apiVersion": "postgresql.cnpg.io/v1",
			"kind":       "Backup",
			"metadata": map[string]interface{}{
				"name":      backupName,
				"namespace": namespace,
				"labels": map[string]interface{}{
					"app.kubernetes.io/name":       "orochi-db",
					"app.kubernetes.io/instance":   clusterName,
					"app.kubernetes.io/component":  "backup",
					"app.kubernetes.io/managed-by": "orochi-provisioner",
					"orochi.io/cluster-name":       clusterName,
				},
			},
			"spec": map[string]interface{}{
				"cluster": map[string]interface{}{
					"name": clusterName,
				},
				"method": string(method),
			},
		},
	}

	m.logger.Info("creating backup",
		zap.String("name", backupName),
		zap.String("cluster", clusterName),
		zap.String("namespace", namespace),
		zap.String("method", string(method)),
	)

	if err := m.k8sClient.WithRetry(ctx, "create backup", func() error {
		return m.k8sClient.CtrlClient().Create(ctx, backup)
	}); err != nil {
		return nil, fmt.Errorf("failed to create backup %s/%s: %w", namespace, backupName, err)
	}

	return backup, nil
}

// GetBackup retrieves a backup by name
func (m *BackupManager) GetBackup(ctx context.Context, namespace, name string) (*unstructured.Unstructured, error) {
	backup := &unstructured.Unstructured{}
	backup.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "Backup",
	})
	if err := m.k8sClient.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, backup); err != nil {
		return nil, err
	}
	return backup, nil
}

// ListBackups lists backups for a cluster
func (m *BackupManager) ListBackups(ctx context.Context, namespace, clusterName string) (*unstructured.UnstructuredList, error) {
	backupList := &unstructured.UnstructuredList{}
	backupList.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "BackupList",
	})
	opts := []client.ListOption{
		client.InNamespace(namespace),
	}

	if clusterName != "" {
		opts = append(opts, client.MatchingLabels{
			"orochi.io/cluster-name": clusterName,
		})
	}

	if err := m.k8sClient.List(ctx, backupList, opts...); err != nil {
		return nil, fmt.Errorf("failed to list backups: %w", err)
	}

	return backupList, nil
}

// DeleteBackup deletes a backup
func (m *BackupManager) DeleteBackup(ctx context.Context, namespace, name string) error {
	backup := &unstructured.Unstructured{}
	backup.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "Backup",
	})
	backup.SetName(name)
	backup.SetNamespace(namespace)

	m.logger.Info("deleting backup",
		zap.String("name", name),
		zap.String("namespace", namespace),
	)

	if err := m.k8sClient.Delete(ctx, backup); err != nil {
		if !errors.IsNotFound(err) {
			return fmt.Errorf("failed to delete backup %s/%s: %w", namespace, name, err)
		}
	}

	return nil
}

// WaitForBackupComplete waits for a backup to complete
func (m *BackupManager) WaitForBackupComplete(ctx context.Context, namespace, name string, timeout time.Duration) error {
	m.logger.Info("waiting for backup to complete",
		zap.String("name", name),
		zap.String("namespace", namespace),
		zap.Duration("timeout", timeout),
	)

	return m.k8sClient.WaitForCondition(ctx, timeout, func() (bool, error) {
		backup, err := m.GetBackup(ctx, namespace, name)
		if err != nil {
			if errors.IsNotFound(err) {
				return false, nil
			}
			return false, err
		}

		phase, _, _ := unstructured.NestedString(backup.Object, "status", "phase")
		switch phase {
		case "completed":
			return true, nil
		case "failed":
			errorMsg, _, _ := unstructured.NestedString(backup.Object, "status", "error")
			return false, fmt.Errorf("backup failed: %s", errorMsg)
		default:
			return false, nil
		}
	})
}

// GetBackupInfo returns backup information
func (m *BackupManager) GetBackupInfo(ctx context.Context, namespace, name string) (*types.BackupInfo, error) {
	backup, err := m.GetBackup(ctx, namespace, name)
	if err != nil {
		return nil, err
	}

	uid, _, _ := unstructured.NestedString(backup.Object, "metadata", "uid")
	backupName, _, _ := unstructured.NestedString(backup.Object, "metadata", "name")
	clusterName, _, _ := unstructured.NestedString(backup.Object, "spec", "cluster", "name")
	method, _, _ := unstructured.NestedString(backup.Object, "spec", "method")
	phase, _, _ := unstructured.NestedString(backup.Object, "status", "phase")
	beginWal, _, _ := unstructured.NestedString(backup.Object, "status", "beginWal")
	endWal, _, _ := unstructured.NestedString(backup.Object, "status", "endWal")
	beginLSN, _, _ := unstructured.NestedString(backup.Object, "status", "beginLSN")
	endLSN, _, _ := unstructured.NestedString(backup.Object, "status", "endLSN")

	info := &types.BackupInfo{
		BackupID:    uid,
		Name:        backupName,
		ClusterName: clusterName,
		Method:      types.BackupMethod(method),
		Phase:       m.convertBackupPhase(phase),
		BeginWAL:    beginWal,
		EndWAL:      endWal,
		BeginLSN:    beginLSN,
		EndLSN:      endLSN,
	}

	// Parse timestamps
	startedAtStr, found, _ := unstructured.NestedString(backup.Object, "status", "startedAt")
	if found && startedAtStr != "" {
		if t, err := time.Parse(time.RFC3339, startedAtStr); err == nil {
			info.StartedAt = t
		}
	}

	stoppedAtStr, found, _ := unstructured.NestedString(backup.Object, "status", "stoppedAt")
	if found && stoppedAtStr != "" {
		if t, err := time.Parse(time.RFC3339, stoppedAtStr); err == nil {
			info.CompletedAt = t
		}
	}

	return info, nil
}

// CreateScheduledBackup creates a scheduled backup for a cluster
func (m *BackupManager) CreateScheduledBackup(ctx context.Context, namespace, clusterName string, schedule string, immediate bool) (*unstructured.Unstructured, error) {
	name := fmt.Sprintf("%s-scheduled", clusterName)

	scheduledBackup := &unstructured.Unstructured{
		Object: map[string]interface{}{
			"apiVersion": "postgresql.cnpg.io/v1",
			"kind":       "ScheduledBackup",
			"metadata": map[string]interface{}{
				"name":      name,
				"namespace": namespace,
				"labels": map[string]interface{}{
					"app.kubernetes.io/name":       "orochi-db",
					"app.kubernetes.io/instance":   clusterName,
					"app.kubernetes.io/component":  "scheduled-backup",
					"app.kubernetes.io/managed-by": "orochi-provisioner",
					"orochi.io/cluster-name":       clusterName,
				},
			},
			"spec": map[string]interface{}{
				"schedule":  schedule,
				"immediate": immediate,
				"cluster": map[string]interface{}{
					"name": clusterName,
				},
				"backupOwnerReference": "cluster",
			},
		},
	}

	m.logger.Info("creating scheduled backup",
		zap.String("name", name),
		zap.String("cluster", clusterName),
		zap.String("namespace", namespace),
		zap.String("schedule", schedule),
	)

	if err := m.k8sClient.CreateOrUpdate(ctx, scheduledBackup); err != nil {
		return nil, fmt.Errorf("failed to create scheduled backup %s/%s: %w", namespace, name, err)
	}

	return scheduledBackup, nil
}

// GetScheduledBackup retrieves a scheduled backup
func (m *BackupManager) GetScheduledBackup(ctx context.Context, namespace, name string) (*unstructured.Unstructured, error) {
	scheduledBackup := &unstructured.Unstructured{}
	scheduledBackup.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "ScheduledBackup",
	})
	if err := m.k8sClient.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, scheduledBackup); err != nil {
		return nil, err
	}
	return scheduledBackup, nil
}

// DeleteScheduledBackup deletes a scheduled backup
func (m *BackupManager) DeleteScheduledBackup(ctx context.Context, namespace, name string) error {
	scheduledBackup := &unstructured.Unstructured{}
	scheduledBackup.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "ScheduledBackup",
	})
	scheduledBackup.SetName(name)
	scheduledBackup.SetNamespace(namespace)

	m.logger.Info("deleting scheduled backup",
		zap.String("name", name),
		zap.String("namespace", namespace),
	)

	if err := m.k8sClient.Delete(ctx, scheduledBackup); err != nil {
		if !errors.IsNotFound(err) {
			return fmt.Errorf("failed to delete scheduled backup %s/%s: %w", namespace, name, err)
		}
	}

	return nil
}

// ListScheduledBackups lists scheduled backups for a cluster
func (m *BackupManager) ListScheduledBackups(ctx context.Context, namespace, clusterName string) (*unstructured.UnstructuredList, error) {
	scheduledBackupList := &unstructured.UnstructuredList{}
	scheduledBackupList.SetGroupVersionKind(schema.GroupVersionKind{
		Group:   "postgresql.cnpg.io",
		Version: "v1",
		Kind:    "ScheduledBackupList",
	})
	opts := []client.ListOption{
		client.InNamespace(namespace),
	}

	if clusterName != "" {
		opts = append(opts, client.MatchingLabels{
			"orochi.io/cluster-name": clusterName,
		})
	}

	if err := m.k8sClient.List(ctx, scheduledBackupList, opts...); err != nil {
		return nil, fmt.Errorf("failed to list scheduled backups: %w", err)
	}

	return scheduledBackupList, nil
}

// convertBackupPhase converts CloudNativePG backup phase string to our type
func (m *BackupManager) convertBackupPhase(phase string) types.BackupPhase {
	switch phase {
	case "pending":
		return types.BackupPhasePending
	case "running":
		return types.BackupPhaseRunning
	case "completed":
		return types.BackupPhaseCompleted
	case "failed":
		return types.BackupPhaseFailed
	default:
		return types.BackupPhasePending
	}
}

// PruneBackups deletes backups older than the retention period
func (m *BackupManager) PruneBackups(ctx context.Context, namespace, clusterName string, retentionDays int) (int, error) {
	backups, err := m.ListBackups(ctx, namespace, clusterName)
	if err != nil {
		return 0, err
	}

	cutoff := time.Now().AddDate(0, 0, -retentionDays)
	pruned := 0

	for _, backup := range backups.Items {
		phase, _, _ := unstructured.NestedString(backup.Object, "status", "phase")
		if phase == "completed" {
			stoppedAtStr, found, _ := unstructured.NestedString(backup.Object, "status", "stoppedAt")
			if found && stoppedAtStr != "" {
				if t, err := time.Parse(time.RFC3339, stoppedAtStr); err == nil && t.Before(cutoff) {
					backupName := backup.GetName()
					if err := m.DeleteBackup(ctx, namespace, backupName); err != nil {
						m.logger.Warn("failed to prune backup",
							zap.String("name", backupName),
							zap.Error(err),
						)
						continue
					}
					pruned++
				}
			}
		}
	}

	return pruned, nil
}
