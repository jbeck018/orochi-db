// Package grpc provides gRPC handlers for the provisioner service.
package grpc

import (
	"context"
	"fmt"
	"strings"

	"go.uber.org/zap"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/emptypb"
	"google.golang.org/protobuf/types/known/timestamppb"

	pb "github.com/orochi-db/orochi-db/services/provisioner/api/proto"
	"github.com/orochi-db/orochi-db/services/provisioner/internal/provisioner"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

// Handler implements the gRPC provisioner service
type Handler struct {
	pb.UnimplementedProvisionerServiceServer
	service *provisioner.Service
	logger  *zap.Logger
	version string
}

// NewHandler creates a new gRPC handler
func NewHandler(service *provisioner.Service, logger *zap.Logger) *Handler {
	return &Handler{
		service: service,
		logger:  logger,
		version: "1.0.0",
	}
}

// CreateCluster creates a new PostgreSQL cluster
func (h *Handler) CreateCluster(ctx context.Context, req *pb.CreateClusterRequest) (*pb.CreateClusterResponse, error) {
	if req.Spec == nil {
		return nil, status.Error(codes.InvalidArgument, "cluster spec is required")
	}

	spec := h.convertClusterSpec(req.Spec)

	clusterInfo, err := h.service.CreateCluster(ctx, spec, req.WaitForReady, req.TimeoutSeconds)
	if err != nil {
		h.logger.Error("failed to create cluster",
			zap.String("name", spec.Name),
			zap.Error(err),
		)
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.CreateClusterResponse{
		ClusterId: clusterInfo.ClusterID,
		Name:      clusterInfo.Name,
		Namespace: clusterInfo.Namespace,
		Phase:     h.convertPhaseToProto(clusterInfo.Status.Phase),
		Message:   "Cluster created successfully",
	}, nil
}

// UpdateCluster updates an existing cluster
func (h *Handler) UpdateCluster(ctx context.Context, req *pb.UpdateClusterRequest) (*pb.UpdateClusterResponse, error) {
	if req.Spec == nil {
		return nil, status.Error(codes.InvalidArgument, "cluster spec is required")
	}

	spec := h.convertClusterSpec(req.Spec)

	clusterInfo, err := h.service.UpdateCluster(ctx, spec, req.RollingUpdate)
	if err != nil {
		h.logger.Error("failed to update cluster",
			zap.String("name", spec.Name),
			zap.Error(err),
		)
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.UpdateClusterResponse{
		ClusterId: clusterInfo.ClusterID,
		Phase:     h.convertPhaseToProto(clusterInfo.Status.Phase),
		Message:   "Cluster updated successfully",
	}, nil
}

// DeleteCluster deletes a cluster
func (h *Handler) DeleteCluster(ctx context.Context, req *pb.DeleteClusterRequest) (*pb.DeleteClusterResponse, error) {
	if req.ClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id is required")
	}

	// Parse cluster ID (format: namespace/name)
	namespace, name, err := h.parseClusterID(req.ClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.Namespace != "" {
		namespace = req.Namespace
	}

	if err := h.service.DeleteCluster(ctx, namespace, name, req.DeleteBackups, req.DeletePvcs); err != nil {
		h.logger.Error("failed to delete cluster",
			zap.String("clusterId", req.ClusterId),
			zap.Error(err),
		)
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.DeleteClusterResponse{
		Success: true,
		Message: "Cluster deleted successfully",
	}, nil
}

// GetCluster retrieves a cluster
func (h *Handler) GetCluster(ctx context.Context, req *pb.GetClusterRequest) (*pb.GetClusterResponse, error) {
	if req.ClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id is required")
	}

	namespace, name, err := h.parseClusterID(req.ClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.Namespace != "" {
		namespace = req.Namespace
	}

	clusterInfo, err := h.service.GetCluster(ctx, namespace, name)
	if err != nil {
		return nil, status.Error(codes.NotFound, err.Error())
	}

	return &pb.GetClusterResponse{
		Cluster: h.convertClusterInfoToProto(clusterInfo),
	}, nil
}

// ListClusters lists clusters
func (h *Handler) ListClusters(ctx context.Context, req *pb.ListClustersRequest) (*pb.ListClustersResponse, error) {
	clusters, err := h.service.ListClusters(ctx, req.Namespace, req.LabelSelector, req.Limit)
	if err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	protoClusters := make([]*pb.ClusterInfo, 0, len(clusters))
	for _, cluster := range clusters {
		protoClusters = append(protoClusters, h.convertClusterInfoToProto(cluster))
	}

	return &pb.ListClustersResponse{
		Clusters:   protoClusters,
		TotalCount: int32(len(protoClusters)),
	}, nil
}

// CreateBackup creates a backup
func (h *Handler) CreateBackup(ctx context.Context, req *pb.CreateBackupRequest) (*pb.CreateBackupResponse, error) {
	if req.ClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id is required")
	}

	namespace, clusterName, err := h.parseClusterID(req.ClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.Namespace != "" {
		namespace = req.Namespace
	}

	method := types.BackupMethodBarmanObjectStore
	if req.Method == pb.BackupMethod_BACKUP_VOLUME_SNAPSHOT {
		method = types.BackupMethodVolumeSnapshot
	}

	backupInfo, err := h.service.CreateBackup(ctx, namespace, clusterName, req.BackupName, method)
	if err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.CreateBackupResponse{
		BackupId: backupInfo.BackupID,
		Name:     backupInfo.Name,
		Phase:    h.convertBackupPhaseToProto(backupInfo.Phase),
		Message:  "Backup created successfully",
	}, nil
}

// ListBackups lists backups for a cluster
func (h *Handler) ListBackups(ctx context.Context, req *pb.ListBackupsRequest) (*pb.ListBackupsResponse, error) {
	if req.ClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id is required")
	}

	namespace, clusterName, err := h.parseClusterID(req.ClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.Namespace != "" {
		namespace = req.Namespace
	}

	backups, err := h.service.ListBackups(ctx, namespace, clusterName)
	if err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	protoBackups := make([]*pb.BackupInfo, 0, len(backups))
	for _, backup := range backups {
		protoBackups = append(protoBackups, h.convertBackupInfoToProto(backup))
	}

	return &pb.ListBackupsResponse{
		Backups: protoBackups,
	}, nil
}

// DeleteBackup deletes a backup
func (h *Handler) DeleteBackup(ctx context.Context, req *pb.DeleteBackupRequest) (*pb.DeleteBackupResponse, error) {
	if req.BackupId == "" {
		return nil, status.Error(codes.InvalidArgument, "backup_id is required")
	}

	if err := h.service.DeleteBackup(ctx, req.Namespace, req.BackupId); err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.DeleteBackupResponse{
		Success: true,
		Message: "Backup deleted successfully",
	}, nil
}

// RestoreCluster restores a cluster from a backup
func (h *Handler) RestoreCluster(ctx context.Context, req *pb.RestoreClusterRequest) (*pb.RestoreClusterResponse, error) {
	if req.SourceClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "source_cluster_id is required")
	}
	if req.TargetSpec == nil {
		return nil, status.Error(codes.InvalidArgument, "target_spec is required")
	}

	sourceNamespace, _, err := h.parseClusterID(req.SourceClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.SourceNamespace != "" {
		sourceNamespace = req.SourceNamespace
	}

	targetSpec := h.convertClusterSpec(req.TargetSpec)
	options := h.convertRestoreOptions(req.Options)

	clusterInfo, err := h.service.RestoreCluster(ctx, req.SourceClusterId, sourceNamespace, targetSpec, options)
	if err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.RestoreClusterResponse{
		ClusterId: clusterInfo.ClusterID,
		Name:      clusterInfo.Name,
		Phase:     h.convertPhaseToProto(clusterInfo.Status.Phase),
		Message:   "Restore initiated successfully",
	}, nil
}

// GetClusterStatus returns the status of a cluster
func (h *Handler) GetClusterStatus(ctx context.Context, req *pb.GetClusterStatusRequest) (*pb.GetClusterStatusResponse, error) {
	if req.ClusterId == "" {
		return nil, status.Error(codes.InvalidArgument, "cluster_id is required")
	}

	namespace, name, err := h.parseClusterID(req.ClusterId)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if req.Namespace != "" {
		namespace = req.Namespace
	}

	clusterStatus, err := h.service.GetClusterStatus(ctx, namespace, name)
	if err != nil {
		return nil, status.Error(codes.Internal, err.Error())
	}

	return &pb.GetClusterStatusResponse{
		Status: h.convertClusterStatusToProto(clusterStatus),
	}, nil
}

// HealthCheck performs a health check
func (h *Handler) HealthCheck(ctx context.Context, _ *emptypb.Empty) (*pb.HealthCheckResponse, error) {
	err := h.service.HealthCheck(ctx)
	healthy := err == nil

	details := map[string]string{
		"kubernetes": "connected",
	}
	if err != nil {
		details["kubernetes"] = fmt.Sprintf("error: %v", err)
	}

	return &pb.HealthCheckResponse{
		Healthy: healthy,
		Version: h.version,
		Details: details,
	}, nil
}

// Helper functions for converting between types

// parseClusterID parses a cluster ID into namespace and name components.
// Expected format: "namespace/name" (e.g., "production/db-cluster-primary")
// The "/" separator is used to unambiguously separate namespace from name,
// since both can contain hyphens.
// Returns an error if the format is invalid.
func (h *Handler) parseClusterID(clusterID string) (namespace, name string, err error) {
	if clusterID == "" {
		return "", "", fmt.Errorf("cluster ID cannot be empty")
	}

	parts := strings.SplitN(clusterID, "/", 2)
	if len(parts) != 2 {
		return "", "", fmt.Errorf("invalid cluster ID format: expected 'namespace/name', got %q", clusterID)
	}

	namespace = strings.TrimSpace(parts[0])
	name = strings.TrimSpace(parts[1])

	if namespace == "" {
		return "", "", fmt.Errorf("namespace cannot be empty in cluster ID %q", clusterID)
	}
	if name == "" {
		return "", "", fmt.Errorf("name cannot be empty in cluster ID %q", clusterID)
	}

	return namespace, name, nil
}

func (h *Handler) convertClusterSpec(spec *pb.ClusterSpec) *types.ClusterSpec {
	if spec == nil {
		return nil
	}

	result := &types.ClusterSpec{
		Name:            spec.Name,
		Namespace:       spec.Namespace,
		Instances:       spec.Instances,
		PostgresVersion: spec.PostgresVersion,
		ImageName:       spec.ImageName,
		Labels:          spec.Labels,
		Annotations:     spec.Annotations,
	}

	if spec.Resources != nil {
		result.Resources = types.ResourceRequirements{
			CPURequest:    spec.Resources.CpuRequest,
			CPULimit:      spec.Resources.CpuLimit,
			MemoryRequest: spec.Resources.MemoryRequest,
			MemoryLimit:   spec.Resources.MemoryLimit,
			HugePages2Mi:  spec.Resources.Hugepages_2Mi,
			HugePages1Gi:  spec.Resources.Hugepages_1Gi,
		}
	}

	if spec.Storage != nil {
		result.Storage = types.StorageSpec{
			Size:               spec.Storage.Size,
			StorageClass:       spec.Storage.StorageClass,
			ResizeInUseVolumes: spec.Storage.ResizeInUseVolumes,
			WALSize:            spec.Storage.WalSize,
			WALStorageClass:    spec.Storage.WalStorageClass,
		}
	}

	if spec.OrochiConfig != nil {
		result.OrochiConfig = &types.OrochiConfig{
			Enabled:            spec.OrochiConfig.Enabled,
			DefaultShardCount:  spec.OrochiConfig.DefaultShardCount,
			ChunkInterval:      spec.OrochiConfig.ChunkInterval,
			EnableColumnar:     spec.OrochiConfig.EnableColumnar,
			DefaultCompression: types.CompressionType(h.convertCompressionType(spec.OrochiConfig.DefaultCompression)),
			CustomParameters:   spec.OrochiConfig.CustomParameters,
		}

		if spec.OrochiConfig.Tiering != nil {
			result.OrochiConfig.Tiering = &types.TieringConfig{
				Enabled:      spec.OrochiConfig.Tiering.Enabled,
				HotDuration:  spec.OrochiConfig.Tiering.HotDuration,
				WarmDuration: spec.OrochiConfig.Tiering.WarmDuration,
				ColdDuration: spec.OrochiConfig.Tiering.ColdDuration,
			}
			if spec.OrochiConfig.Tiering.S3Config != nil {
				result.OrochiConfig.Tiering.S3Config = &types.S3Config{
					Endpoint:        spec.OrochiConfig.Tiering.S3Config.Endpoint,
					Bucket:          spec.OrochiConfig.Tiering.S3Config.Bucket,
					Region:          spec.OrochiConfig.Tiering.S3Config.Region,
					AccessKeySecret: spec.OrochiConfig.Tiering.S3Config.AccessKeySecret,
				}
			}
		}
	}

	if spec.Monitoring != nil {
		result.Monitoring = &types.MonitoringConfig{
			EnablePodMonitor:     spec.Monitoring.EnablePodMonitor,
			EnablePrometheusRule: spec.Monitoring.EnablePrometheusRule,
			CustomQueries:        spec.Monitoring.CustomQueries,
			ScrapeInterval:       spec.Monitoring.ScrapeInterval,
		}
	}

	if spec.Affinity != nil {
		result.Affinity = &types.AffinityConfig{
			EnablePodAntiAffinity: spec.Affinity.EnablePodAntiAffinity,
			TopologyKey:           spec.Affinity.TopologyKey,
			NodeSelector:          spec.Affinity.NodeSelector,
		}

		for _, t := range spec.Affinity.Tolerations {
			toleration := types.Toleration{
				Key:      t.Key,
				Operator: t.Operator,
				Value:    t.Value,
				Effect:   t.Effect,
			}
			if t.TolerationSeconds > 0 {
				ts := t.TolerationSeconds
				toleration.TolerationSeconds = &ts
			}
			result.Affinity.Tolerations = append(result.Affinity.Tolerations, toleration)
		}
	}

	return result
}

func (h *Handler) convertCompressionType(ct pb.CompressionType) string {
	switch ct {
	case pb.CompressionType_COMPRESSION_LZ4:
		return "lz4"
	case pb.CompressionType_COMPRESSION_ZSTD:
		return "zstd"
	case pb.CompressionType_COMPRESSION_DELTA:
		return "delta"
	case pb.CompressionType_COMPRESSION_GORILLA:
		return "gorilla"
	case pb.CompressionType_COMPRESSION_DICTIONARY:
		return "dictionary"
	case pb.CompressionType_COMPRESSION_RLE:
		return "rle"
	default:
		return "none"
	}
}

func (h *Handler) convertPhaseToProto(phase types.ClusterPhase) pb.ClusterPhase {
	switch phase {
	case types.ClusterPhaseSettingUp:
		return pb.ClusterPhase_PHASE_SETTING_UP
	case types.ClusterPhaseHealthy:
		return pb.ClusterPhase_PHASE_HEALTHY
	case types.ClusterPhaseUnhealthy:
		return pb.ClusterPhase_PHASE_UNHEALTHY
	case types.ClusterPhaseUpgrading:
		return pb.ClusterPhase_PHASE_UPGRADING
	case types.ClusterPhaseFailingOver:
		return pb.ClusterPhase_PHASE_FAILING_OVER
	case types.ClusterPhaseDeleting:
		return pb.ClusterPhase_PHASE_DELETING
	default:
		return pb.ClusterPhase_PHASE_UNKNOWN
	}
}

func (h *Handler) convertBackupPhaseToProto(phase types.BackupPhase) pb.BackupPhase {
	switch phase {
	case types.BackupPhasePending:
		return pb.BackupPhase_BACKUP_PENDING
	case types.BackupPhaseRunning:
		return pb.BackupPhase_BACKUP_RUNNING
	case types.BackupPhaseCompleted:
		return pb.BackupPhase_BACKUP_COMPLETED
	case types.BackupPhaseFailed:
		return pb.BackupPhase_BACKUP_FAILED
	default:
		return pb.BackupPhase_BACKUP_PENDING
	}
}

func (h *Handler) convertClusterInfoToProto(info *types.ClusterInfo) *pb.ClusterInfo {
	return &pb.ClusterInfo{
		ClusterId: info.ClusterID,
		Name:      info.Name,
		Namespace: info.Namespace,
		Status:    h.convertClusterStatusToProto(&info.Status),
		CreatedAt: timestamppb.New(info.CreatedAt),
		UpdatedAt: timestamppb.New(info.UpdatedAt),
	}
}

func (h *Handler) convertClusterStatusToProto(status *types.ClusterStatus) *pb.ClusterStatus {
	protoStatus := &pb.ClusterStatus{
		Phase:                    h.convertPhaseToProto(status.Phase),
		ReadyInstances:           status.ReadyInstances,
		TotalInstances:           status.TotalInstances,
		CurrentPrimary:           status.CurrentPrimary,
		CurrentPrimaryTimestamp:  status.CurrentPrimaryTimestamp,
		FirstRecoverabilityPoint: status.FirstRecoverabilityPoint,
		LastSuccessfulBackup:     status.LastSuccessfulBackup,
		Conditions:               status.Conditions,
	}

	for _, inst := range status.Instances {
		protoStatus.Instances = append(protoStatus.Instances, &pb.InstanceStatus{
			Name:      inst.Name,
			Role:      inst.Role,
			Ready:     inst.Ready,
			PodIp:     inst.PodIP,
			StartTime: timestamppb.New(inst.StartTime),
		})
	}

	return protoStatus
}

func (h *Handler) convertBackupInfoToProto(info *types.BackupInfo) *pb.BackupInfo {
	method := pb.BackupMethod_BACKUP_BARMAN_OBJECT_STORE
	if info.Method == types.BackupMethodVolumeSnapshot {
		method = pb.BackupMethod_BACKUP_VOLUME_SNAPSHOT
	}

	return &pb.BackupInfo{
		BackupId:    info.BackupID,
		Name:        info.Name,
		ClusterName: info.ClusterName,
		Method:      method,
		Phase:       h.convertBackupPhaseToProto(info.Phase),
		StartedAt:   timestamppb.New(info.StartedAt),
		CompletedAt: timestamppb.New(info.CompletedAt),
		BeginWal:    info.BeginWAL,
		EndWal:      info.EndWAL,
		BeginLsn:    info.BeginLSN,
		EndLsn:      info.EndLSN,
		BackupSize:  info.BackupSize,
	}
}

func (h *Handler) convertRestoreOptions(opts *pb.RestoreOptions) *types.RestoreOptions {
	if opts == nil {
		return nil
	}

	return &types.RestoreOptions{
		BackupID:                opts.BackupId,
		RecoveryTargetTime:      opts.RecoveryTargetTime,
		RecoveryTargetLSN:       opts.RecoveryTargetLsn,
		RecoveryTargetName:      opts.RecoveryTargetName,
		RecoveryTargetInclusive: opts.RecoveryTargetInclusive,
	}
}
