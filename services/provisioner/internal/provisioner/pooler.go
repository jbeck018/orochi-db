// Package provisioner provides PgDog connection pooler management functionality.
package provisioner

import (
	"context"
	"fmt"

	"go.uber.org/zap"
	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/intstr"
	"sigs.k8s.io/controller-runtime/pkg/client"

	"github.com/orochi-db/orochi-db/services/provisioner/internal/k8s"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

const (
	// PgDog default image
	defaultPgDogImage    = "ghcr.io/pgdog/pgdog"
	defaultPgDogImageTag = "latest"

	// JWT proxy default image
	defaultJWTProxyImage    = "ghcr.io/orochi-db/pgdog-jwt-proxy"
	defaultJWTProxyImageTag = "latest"

	// Default port configurations
	pgdogPostgresPort = 6432
	pgdogAdminPort    = 6433
	pgdogMetricsPort  = 9090
	jwtProxyPort      = 5433
)

// PoolerManager manages PgDog connection pooler deployments
type PoolerManager struct {
	k8sClient      *k8s.Client
	templateEngine *TemplateEngine
	logger         *zap.Logger
}

// NewPoolerManager creates a new PoolerManager
func NewPoolerManager(k8sClient *k8s.Client, templateEngine *TemplateEngine, logger *zap.Logger) *PoolerManager {
	return &PoolerManager{
		k8sClient:      k8sClient,
		templateEngine: templateEngine,
		logger:         logger,
	}
}

// CreatePooler creates all PgDog resources for a cluster
func (m *PoolerManager) CreatePooler(ctx context.Context, spec *types.ClusterSpec) error {
	if spec.Pooler == nil || !spec.Pooler.Enabled {
		m.logger.Debug("pooler not enabled, skipping creation",
			zap.String("cluster", spec.Name),
			zap.String("namespace", spec.Namespace),
		)
		return nil
	}

	m.logger.Info("creating PgDog pooler for cluster",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	// Apply defaults
	m.applyPoolerDefaults(spec)

	// Create ConfigMap with PgDog configuration
	if err := m.CreatePoolerConfigMap(ctx, spec); err != nil {
		return fmt.Errorf("failed to create pooler configmap: %w", err)
	}

	// Create Service
	if err := m.CreatePoolerService(ctx, spec); err != nil {
		return fmt.Errorf("failed to create pooler service: %w", err)
	}

	// Create Deployment
	if err := m.CreatePoolerDeployment(ctx, spec); err != nil {
		return fmt.Errorf("failed to create pooler deployment: %w", err)
	}

	m.logger.Info("successfully created PgDog pooler",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	return nil
}

// CreatePoolerDeployment creates the Kubernetes Deployment for PgDog
func (m *PoolerManager) CreatePoolerDeployment(ctx context.Context, spec *types.ClusterSpec) error {
	deployment := m.buildDeployment(spec)

	err := m.k8sClient.CreateOrUpdate(ctx, deployment)
	if err != nil {
		return fmt.Errorf("failed to create/update PgDog deployment: %w", err)
	}

	m.logger.Info("created PgDog deployment",
		zap.String("name", deployment.Name),
		zap.String("namespace", deployment.Namespace),
	)

	return nil
}

// CreatePoolerService creates the Kubernetes Services for PgDog
func (m *PoolerManager) CreatePoolerService(ctx context.Context, spec *types.ClusterSpec) error {
	// Create main service (ClusterIP for internal access)
	mainService := m.buildMainService(spec)
	if err := m.k8sClient.CreateOrUpdate(ctx, mainService); err != nil {
		return fmt.Errorf("failed to create main PgDog service: %w", err)
	}

	// Create headless service for pod DNS
	headlessService := m.buildHeadlessService(spec)
	if err := m.k8sClient.CreateOrUpdate(ctx, headlessService); err != nil {
		return fmt.Errorf("failed to create headless PgDog service: %w", err)
	}

	m.logger.Info("created PgDog services",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	return nil
}

// CreatePoolerConfigMap creates the ConfigMap containing PgDog configuration
func (m *PoolerManager) CreatePoolerConfigMap(ctx context.Context, spec *types.ClusterSpec) error {
	// Generate pgdog.toml configuration
	pgdogConfig := m.generatePgDogConfig(spec)

	configMap := &corev1.ConfigMap{
		ObjectMeta: metav1.ObjectMeta{
			Name:      m.getPoolerName(spec.Name) + "-config",
			Namespace: spec.Namespace,
			Labels:    m.getPoolerLabels(spec),
		},
		Data: map[string]string{
			"pgdog.toml": pgdogConfig,
		},
	}

	if err := m.k8sClient.CreateOrUpdate(ctx, configMap); err != nil {
		return fmt.Errorf("failed to create PgDog configmap: %w", err)
	}

	m.logger.Info("created PgDog configmap",
		zap.String("name", configMap.Name),
		zap.String("namespace", configMap.Namespace),
	)

	return nil
}

// UpdatePoolerConfig updates the PgDog configuration for a cluster
func (m *PoolerManager) UpdatePoolerConfig(ctx context.Context, spec *types.ClusterSpec) error {
	if spec.Pooler == nil || !spec.Pooler.Enabled {
		return nil
	}

	m.logger.Info("updating PgDog configuration",
		zap.String("cluster", spec.Name),
		zap.String("namespace", spec.Namespace),
	)

	// Update ConfigMap
	if err := m.CreatePoolerConfigMap(ctx, spec); err != nil {
		return fmt.Errorf("failed to update pooler configmap: %w", err)
	}

	// Update Deployment (to pick up new config)
	if err := m.CreatePoolerDeployment(ctx, spec); err != nil {
		return fmt.Errorf("failed to update pooler deployment: %w", err)
	}

	// Trigger rolling restart by updating annotation
	return m.restartPoolerDeployment(ctx, spec)
}

// DeletePooler removes all PgDog resources for a cluster
func (m *PoolerManager) DeletePooler(ctx context.Context, namespace, clusterName string) error {
	m.logger.Info("deleting PgDog pooler",
		zap.String("cluster", clusterName),
		zap.String("namespace", namespace),
	)

	poolerName := m.getPoolerName(clusterName)

	// Delete Deployment
	deployment := &appsv1.Deployment{
		ObjectMeta: metav1.ObjectMeta{
			Name:      poolerName,
			Namespace: namespace,
		},
	}
	if err := m.k8sClient.Delete(ctx, deployment); err != nil && !errors.IsNotFound(err) {
		m.logger.Warn("failed to delete PgDog deployment",
			zap.String("name", poolerName),
			zap.Error(err),
		)
	}

	// Delete Services
	for _, svcSuffix := range []string{"", "-headless"} {
		svc := &corev1.Service{
			ObjectMeta: metav1.ObjectMeta{
				Name:      poolerName + svcSuffix,
				Namespace: namespace,
			},
		}
		if err := m.k8sClient.Delete(ctx, svc); err != nil && !errors.IsNotFound(err) {
			m.logger.Warn("failed to delete PgDog service",
				zap.String("name", poolerName+svcSuffix),
				zap.Error(err),
			)
		}
	}

	// Delete ConfigMap
	configMap := &corev1.ConfigMap{
		ObjectMeta: metav1.ObjectMeta{
			Name:      poolerName + "-config",
			Namespace: namespace,
		},
	}
	if err := m.k8sClient.Delete(ctx, configMap); err != nil && !errors.IsNotFound(err) {
		m.logger.Warn("failed to delete PgDog configmap",
			zap.String("name", poolerName+"-config"),
			zap.Error(err),
		)
	}

	m.logger.Info("deleted PgDog pooler resources",
		zap.String("cluster", clusterName),
		zap.String("namespace", namespace),
	)

	return nil
}

// GetPoolerStatus retrieves the current status of the PgDog deployment
func (m *PoolerManager) GetPoolerStatus(ctx context.Context, namespace, clusterName string) (*types.PoolerStatus, error) {
	poolerName := m.getPoolerName(clusterName)

	// Get Deployment status
	deployment := &appsv1.Deployment{}
	err := m.k8sClient.Get(ctx, client.ObjectKey{
		Namespace: namespace,
		Name:      poolerName,
	}, deployment)

	if err != nil {
		if errors.IsNotFound(err) {
			return &types.PoolerStatus{Enabled: false}, nil
		}
		return nil, fmt.Errorf("failed to get PgDog deployment: %w", err)
	}

	status := &types.PoolerStatus{
		Enabled:       true,
		Replicas:      deployment.Status.Replicas,
		ReadyReplicas: deployment.Status.ReadyReplicas,
		Ready:         deployment.Status.ReadyReplicas == deployment.Status.Replicas && deployment.Status.Replicas > 0,
	}

	// Get Service endpoints
	svc := &corev1.Service{}
	err = m.k8sClient.Get(ctx, client.ObjectKey{
		Namespace: namespace,
		Name:      poolerName,
	}, svc)

	if err == nil {
		// Build endpoint URLs
		status.InternalEndpoint = fmt.Sprintf("%s.%s.svc.cluster.local:%d",
			poolerName, namespace, pgdogPostgresPort)
		status.JWTEndpoint = fmt.Sprintf("%s.%s.svc.cluster.local:%d",
			poolerName, namespace, jwtProxyPort)

		// For LoadBalancer services, get external endpoint
		if svc.Spec.Type == corev1.ServiceTypeLoadBalancer && len(svc.Status.LoadBalancer.Ingress) > 0 {
			ingress := svc.Status.LoadBalancer.Ingress[0]
			if ingress.IP != "" {
				status.Endpoint = fmt.Sprintf("%s:%d", ingress.IP, jwtProxyPort)
			} else if ingress.Hostname != "" {
				status.Endpoint = fmt.Sprintf("%s:%d", ingress.Hostname, jwtProxyPort)
			}
		}
	}

	return status, nil
}

// applyPoolerDefaults applies default values to pooler configuration
func (m *PoolerManager) applyPoolerDefaults(spec *types.ClusterSpec) {
	if spec.Pooler == nil {
		return
	}

	if spec.Pooler.Replicas == 0 {
		spec.Pooler.Replicas = 2
	}

	if spec.Pooler.Mode == "" {
		spec.Pooler.Mode = "transaction"
	}

	if spec.Pooler.MaxPoolSize == 0 {
		spec.Pooler.MaxPoolSize = 100
	}

	if spec.Pooler.MinPoolSize == 0 {
		spec.Pooler.MinPoolSize = 5
	}

	if spec.Pooler.IdleTimeout == 0 {
		spec.Pooler.IdleTimeout = 600
	}

	if spec.Pooler.Image == "" {
		spec.Pooler.Image = defaultPgDogImage
	}

	if spec.Pooler.ImageTag == "" {
		spec.Pooler.ImageTag = defaultPgDogImageTag
	}

	// Set default resource requests/limits if not specified
	if spec.Pooler.Resources.CPURequest == "" {
		spec.Pooler.Resources.CPURequest = "250m"
	}
	if spec.Pooler.Resources.CPULimit == "" {
		spec.Pooler.Resources.CPULimit = "1000m"
	}
	if spec.Pooler.Resources.MemoryRequest == "" {
		spec.Pooler.Resources.MemoryRequest = "256Mi"
	}
	if spec.Pooler.Resources.MemoryLimit == "" {
		spec.Pooler.Resources.MemoryLimit = "1Gi"
	}
}

// buildDeployment builds the Kubernetes Deployment for PgDog
func (m *PoolerManager) buildDeployment(spec *types.ClusterSpec) *appsv1.Deployment {
	poolerName := m.getPoolerName(spec.Name)
	labels := m.getPoolerLabels(spec)

	replicas := spec.Pooler.Replicas

	// Build PgDog container
	pgdogContainer := m.buildPgDogContainer(spec)
	containers := []corev1.Container{pgdogContainer}

	// Add JWT proxy sidecar if JWT auth is enabled
	if spec.Pooler.JWTAuth != nil && spec.Pooler.JWTAuth.Enabled {
		jwtContainer := m.buildJWTProxyContainer(spec)
		containers = append(containers, jwtContainer)
	}

	// Build volumes
	volumes := m.buildVolumes(spec)

	deployment := &appsv1.Deployment{
		ObjectMeta: metav1.ObjectMeta{
			Name:      poolerName,
			Namespace: spec.Namespace,
			Labels:    labels,
		},
		Spec: appsv1.DeploymentSpec{
			Replicas: &replicas,
			Selector: &metav1.LabelSelector{
				MatchLabels: map[string]string{
					"app":                    "pgdog",
					"orochi.io/cluster-name": spec.Name,
				},
			},
			Strategy: appsv1.DeploymentStrategy{
				Type: appsv1.RollingUpdateDeploymentStrategyType,
				RollingUpdate: &appsv1.RollingUpdateDeployment{
					MaxUnavailable: &intstr.IntOrString{Type: intstr.Int, IntVal: 0},
					MaxSurge:       &intstr.IntOrString{Type: intstr.Int, IntVal: 1},
				},
			},
			Template: corev1.PodTemplateSpec{
				ObjectMeta: metav1.ObjectMeta{
					Labels: labels,
					Annotations: map[string]string{
						"prometheus.io/scrape": "true",
						"prometheus.io/port":   fmt.Sprintf("%d", pgdogMetricsPort),
						"prometheus.io/path":   "/metrics",
					},
				},
				Spec: corev1.PodSpec{
					ServiceAccountName: poolerName,
					SecurityContext: &corev1.PodSecurityContext{
						RunAsNonRoot: boolPtr(true),
						RunAsUser:    int64Ptr(1000),
						RunAsGroup:   int64Ptr(1000),
						FSGroup:      int64Ptr(1000),
					},
					Affinity: &corev1.Affinity{
						PodAntiAffinity: &corev1.PodAntiAffinity{
							PreferredDuringSchedulingIgnoredDuringExecution: []corev1.WeightedPodAffinityTerm{
								{
									Weight: 100,
									PodAffinityTerm: corev1.PodAffinityTerm{
										LabelSelector: &metav1.LabelSelector{
											MatchLabels: map[string]string{
												"app":                    "pgdog",
												"orochi.io/cluster-name": spec.Name,
											},
										},
										TopologyKey: "kubernetes.io/hostname",
									},
								},
							},
						},
					},
					Containers:                    containers,
					Volumes:                       volumes,
					TerminationGracePeriodSeconds: int64Ptr(30),
				},
			},
		},
	}

	return deployment
}

// buildPgDogContainer builds the main PgDog container spec
func (m *PoolerManager) buildPgDogContainer(spec *types.ClusterSpec) corev1.Container {
	image := fmt.Sprintf("%s:%s", spec.Pooler.Image, spec.Pooler.ImageTag)

	container := corev1.Container{
		Name:            "pgdog",
		Image:           image,
		ImagePullPolicy: corev1.PullAlways,
		Ports: []corev1.ContainerPort{
			{Name: "postgres", ContainerPort: pgdogPostgresPort, Protocol: corev1.ProtocolTCP},
			{Name: "admin", ContainerPort: pgdogAdminPort, Protocol: corev1.ProtocolTCP},
			{Name: "metrics", ContainerPort: pgdogMetricsPort, Protocol: corev1.ProtocolTCP},
		},
		Env: []corev1.EnvVar{
			{Name: "PGDOG_CONFIG", Value: "/etc/pgdog/pgdog.toml"},
			{Name: "RUST_LOG", Value: "pgdog=info,warn"},
		},
		VolumeMounts: []corev1.VolumeMount{
			{Name: "config", MountPath: "/etc/pgdog", ReadOnly: true},
		},
		Resources: corev1.ResourceRequirements{
			Requests: corev1.ResourceList{
				corev1.ResourceCPU:    resource.MustParse(spec.Pooler.Resources.CPURequest),
				corev1.ResourceMemory: resource.MustParse(spec.Pooler.Resources.MemoryRequest),
			},
			Limits: corev1.ResourceList{
				corev1.ResourceCPU:    resource.MustParse(spec.Pooler.Resources.CPULimit),
				corev1.ResourceMemory: resource.MustParse(spec.Pooler.Resources.MemoryLimit),
			},
		},
		LivenessProbe: &corev1.Probe{
			ProbeHandler: corev1.ProbeHandler{
				TCPSocket: &corev1.TCPSocketAction{
					Port: intstr.FromString("postgres"),
				},
			},
			InitialDelaySeconds: 10,
			PeriodSeconds:       30,
			TimeoutSeconds:      5,
			FailureThreshold:    3,
		},
		ReadinessProbe: &corev1.Probe{
			ProbeHandler: corev1.ProbeHandler{
				TCPSocket: &corev1.TCPSocketAction{
					Port: intstr.FromString("postgres"),
				},
			},
			InitialDelaySeconds: 5,
			PeriodSeconds:       10,
			TimeoutSeconds:      3,
			FailureThreshold:    2,
		},
		SecurityContext: &corev1.SecurityContext{
			AllowPrivilegeEscalation: boolPtr(false),
			ReadOnlyRootFilesystem:   boolPtr(true),
			Capabilities: &corev1.Capabilities{
				Drop: []corev1.Capability{"ALL"},
			},
		},
	}

	// Add TLS volume mount if enabled
	if spec.Pooler.TLSEnabled {
		container.VolumeMounts = append(container.VolumeMounts, corev1.VolumeMount{
			Name:      "tls",
			MountPath: "/etc/pgdog/tls",
			ReadOnly:  true,
		})
	}

	// Add secrets volume mount if JWT auth is enabled
	if spec.Pooler.JWTAuth != nil && spec.Pooler.JWTAuth.Enabled {
		container.VolumeMounts = append(container.VolumeMounts, corev1.VolumeMount{
			Name:      "secrets",
			MountPath: "/etc/pgdog/secrets",
			ReadOnly:  true,
		})
	}

	return container
}

// buildJWTProxyContainer builds the JWT proxy sidecar container spec
func (m *PoolerManager) buildJWTProxyContainer(spec *types.ClusterSpec) corev1.Container {
	image := fmt.Sprintf("%s:%s", defaultJWTProxyImage, defaultJWTProxyImageTag)

	container := corev1.Container{
		Name:            "jwt-proxy",
		Image:           image,
		ImagePullPolicy: corev1.PullAlways,
		Ports: []corev1.ContainerPort{
			{Name: "jwt-postgres", ContainerPort: jwtProxyPort, Protocol: corev1.ProtocolTCP},
		},
		Env: []corev1.EnvVar{
			{Name: "UPSTREAM_HOST", Value: "127.0.0.1"},
			{Name: "UPSTREAM_PORT", Value: fmt.Sprintf("%d", pgdogPostgresPort)},
			{Name: "LISTEN_PORT", Value: fmt.Sprintf("%d", jwtProxyPort)},
			{Name: "LOG_LEVEL", Value: "info"},
		},
		VolumeMounts: []corev1.VolumeMount{
			{Name: "secrets", MountPath: "/etc/pgdog/secrets", ReadOnly: true},
		},
		Resources: corev1.ResourceRequirements{
			Requests: corev1.ResourceList{
				corev1.ResourceCPU:    resource.MustParse("50m"),
				corev1.ResourceMemory: resource.MustParse("64Mi"),
			},
			Limits: corev1.ResourceList{
				corev1.ResourceCPU:    resource.MustParse("250m"),
				corev1.ResourceMemory: resource.MustParse("256Mi"),
			},
		},
		LivenessProbe: &corev1.Probe{
			ProbeHandler: corev1.ProbeHandler{
				TCPSocket: &corev1.TCPSocketAction{
					Port: intstr.FromString("jwt-postgres"),
				},
			},
			InitialDelaySeconds: 5,
			PeriodSeconds:       30,
			TimeoutSeconds:      5,
			FailureThreshold:    3,
		},
		ReadinessProbe: &corev1.Probe{
			ProbeHandler: corev1.ProbeHandler{
				TCPSocket: &corev1.TCPSocketAction{
					Port: intstr.FromString("jwt-postgres"),
				},
			},
			InitialDelaySeconds: 3,
			PeriodSeconds:       10,
			TimeoutSeconds:      3,
			FailureThreshold:    2,
		},
		SecurityContext: &corev1.SecurityContext{
			AllowPrivilegeEscalation: boolPtr(false),
			ReadOnlyRootFilesystem:   boolPtr(true),
			Capabilities: &corev1.Capabilities{
				Drop: []corev1.Capability{"ALL"},
			},
		},
	}

	// Add JWT configuration from spec
	if spec.Pooler.JWTAuth != nil {
		if spec.Pooler.JWTAuth.Issuer != "" {
			container.Env = append(container.Env, corev1.EnvVar{
				Name:  "JWT_ISSUER",
				Value: spec.Pooler.JWTAuth.Issuer,
			})
		}
		if spec.Pooler.JWTAuth.Audience != "" {
			container.Env = append(container.Env, corev1.EnvVar{
				Name:  "JWT_AUDIENCE",
				Value: spec.Pooler.JWTAuth.Audience,
			})
		}
		if spec.Pooler.JWTAuth.PublicKeyPath != "" {
			container.Env = append(container.Env, corev1.EnvVar{
				Name:  "JWT_PUBLIC_KEY_PATH",
				Value: spec.Pooler.JWTAuth.PublicKeyPath,
			})
		}
	}

	// Add TLS volume mount if enabled
	if spec.Pooler.TLSEnabled {
		container.VolumeMounts = append(container.VolumeMounts, corev1.VolumeMount{
			Name:      "tls",
			MountPath: "/etc/pgdog/tls",
			ReadOnly:  true,
		})
	}

	return container
}

// buildVolumes builds the volume specifications for the deployment
func (m *PoolerManager) buildVolumes(spec *types.ClusterSpec) []corev1.Volume {
	poolerName := m.getPoolerName(spec.Name)

	volumes := []corev1.Volume{
		{
			Name: "config",
			VolumeSource: corev1.VolumeSource{
				ConfigMap: &corev1.ConfigMapVolumeSource{
					LocalObjectReference: corev1.LocalObjectReference{
						Name: poolerName + "-config",
					},
				},
			},
		},
	}

	// Add secrets volume if JWT auth is enabled
	if spec.Pooler.JWTAuth != nil && spec.Pooler.JWTAuth.Enabled {
		secretName := spec.Pooler.JWTAuth.SecretName
		if secretName == "" {
			secretName = poolerName + "-secrets"
		}
		volumes = append(volumes, corev1.Volume{
			Name: "secrets",
			VolumeSource: corev1.VolumeSource{
				Secret: &corev1.SecretVolumeSource{
					SecretName:  secretName,
					DefaultMode: int32Ptr(0400),
				},
			},
		})
	}

	// Add TLS volume if enabled
	if spec.Pooler.TLSEnabled {
		volumes = append(volumes, corev1.Volume{
			Name: "tls",
			VolumeSource: corev1.VolumeSource{
				Secret: &corev1.SecretVolumeSource{
					SecretName:  poolerName + "-tls",
					DefaultMode: int32Ptr(0400),
				},
			},
		})
	}

	return volumes
}

// buildMainService builds the main ClusterIP service for PgDog
func (m *PoolerManager) buildMainService(spec *types.ClusterSpec) *corev1.Service {
	poolerName := m.getPoolerName(spec.Name)
	labels := m.getPoolerLabels(spec)

	ports := []corev1.ServicePort{
		{
			Name:       "postgres-direct",
			Port:       pgdogPostgresPort,
			TargetPort: intstr.FromInt(pgdogPostgresPort),
			Protocol:   corev1.ProtocolTCP,
		},
		{
			Name:       "admin",
			Port:       pgdogAdminPort,
			TargetPort: intstr.FromInt(pgdogAdminPort),
			Protocol:   corev1.ProtocolTCP,
		},
		{
			Name:       "metrics",
			Port:       pgdogMetricsPort,
			TargetPort: intstr.FromInt(pgdogMetricsPort),
			Protocol:   corev1.ProtocolTCP,
		},
	}

	// Add JWT proxy port if JWT auth is enabled
	if spec.Pooler.JWTAuth != nil && spec.Pooler.JWTAuth.Enabled {
		ports = append(ports, corev1.ServicePort{
			Name:       "jwt-postgres",
			Port:       jwtProxyPort,
			TargetPort: intstr.FromInt(jwtProxyPort),
			Protocol:   corev1.ProtocolTCP,
		})
	}

	return &corev1.Service{
		ObjectMeta: metav1.ObjectMeta{
			Name:      poolerName,
			Namespace: spec.Namespace,
			Labels:    labels,
		},
		Spec: corev1.ServiceSpec{
			Type: corev1.ServiceTypeClusterIP,
			Selector: map[string]string{
				"app":                    "pgdog",
				"orochi.io/cluster-name": spec.Name,
			},
			Ports: ports,
		},
	}
}

// buildHeadlessService builds the headless service for pod DNS
func (m *PoolerManager) buildHeadlessService(spec *types.ClusterSpec) *corev1.Service {
	poolerName := m.getPoolerName(spec.Name)
	labels := m.getPoolerLabels(spec)

	return &corev1.Service{
		ObjectMeta: metav1.ObjectMeta{
			Name:      poolerName + "-headless",
			Namespace: spec.Namespace,
			Labels:    labels,
		},
		Spec: corev1.ServiceSpec{
			Type:      corev1.ServiceTypeClusterIP,
			ClusterIP: "None",
			Selector: map[string]string{
				"app":                    "pgdog",
				"orochi.io/cluster-name": spec.Name,
			},
			Ports: []corev1.ServicePort{
				{
					Name:       "postgres",
					Port:       pgdogPostgresPort,
					TargetPort: intstr.FromInt(pgdogPostgresPort),
					Protocol:   corev1.ProtocolTCP,
				},
			},
		},
	}
}

// generatePgDogConfig generates the pgdog.toml configuration
func (m *PoolerManager) generatePgDogConfig(spec *types.ClusterSpec) string {
	// Generate configuration based on cluster spec
	config := fmt.Sprintf(`# PgDog Configuration for %s
# Auto-generated by Orochi Provisioner

[general]
host = "0.0.0.0"
port = %d
admin_port = %d

connect_timeout = 5000
query_timeout = 300000
idle_timeout = %d000
checkout_timeout = 5000

workers = 0
tcp_keepalive = true
tcp_keepalive_idle = 60
tcp_keepalive_interval = 15
tcp_keepalive_count = 3

tls = %v
`, spec.Name, pgdogPostgresPort, pgdogAdminPort, spec.Pooler.IdleTimeout, spec.Pooler.TLSEnabled)

	if spec.Pooler.TLSEnabled {
		config += `tls_certificate = "/etc/pgdog/tls/tls.crt"
tls_private_key = "/etc/pgdog/tls/tls.key"
`
	}

	config += fmt.Sprintf(`
prometheus_enabled = true
prometheus_port = %d

log_level = "info"
log_client_connections = true
log_client_disconnections = true

[auth]
auth_type = "scram-sha-256"
`, pgdogMetricsPort)

	// Add pool configuration
	config += fmt.Sprintf(`
[pools]
default_pool_size = 25
min_pool_size = %d
max_pool_size = %d
pool_mode = "%s"
reserve_pool_size = 5
reserve_pool_timeout = 3000
server_lifetime = 3600000
server_idle_timeout = %d000
prepared_statements = true
load_balancing = "least_connections"
`, spec.Pooler.MinPoolSize, spec.Pooler.MaxPoolSize, spec.Pooler.Mode, spec.Pooler.IdleTimeout)

	// Add sharding configuration if enabled
	if spec.Pooler.ShardingEnabled {
		shardCount := spec.Pooler.ShardCount
		if shardCount == 0 {
			shardCount = 32
		}
		config += fmt.Sprintf(`
[sharding]
enabled = true
method = "hash"
shard_count = %d
automatic_sharding = true
default_shard = "any"
`, shardCount)
	}

	// Add query routing configuration
	config += fmt.Sprintf(`
[plugins.query_router]
enabled = true
read_write_splitting = %v
`, spec.Pooler.ReadWriteSplit)

	// Add database cluster configuration
	// Primary (read-write) endpoint
	config += fmt.Sprintf(`
[[clusters]]
name = "primary"

  [[clusters.databases]]
  name = "%s"
  host = "%s-rw.%s.svc.cluster.local"
  port = 5432
  database = "postgres"
  role = "primary"
`, spec.Name, spec.Name, spec.Namespace)

	// Replica (read-only) endpoint for read-write split
	if spec.Pooler.ReadWriteSplit {
		config += fmt.Sprintf(`
  [[clusters.databases]]
  name = "%s_replica"
  host = "%s-ro.%s.svc.cluster.local"
  port = 5432
  database = "postgres"
  role = "replica"
`, spec.Name, spec.Name, spec.Namespace)
	}

	return config
}

// restartPoolerDeployment triggers a rolling restart of the pooler deployment
func (m *PoolerManager) restartPoolerDeployment(ctx context.Context, spec *types.ClusterSpec) error {
	poolerName := m.getPoolerName(spec.Name)

	deployment := &appsv1.Deployment{}
	err := m.k8sClient.Get(ctx, client.ObjectKey{
		Namespace: spec.Namespace,
		Name:      poolerName,
	}, deployment)

	if err != nil {
		if errors.IsNotFound(err) {
			return nil
		}
		return fmt.Errorf("failed to get deployment for restart: %w", err)
	}

	// Update annotation to trigger rolling restart
	if deployment.Spec.Template.Annotations == nil {
		deployment.Spec.Template.Annotations = make(map[string]string)
	}
	deployment.Spec.Template.Annotations["kubectl.kubernetes.io/restartedAt"] = metav1.Now().Format("2006-01-02T15:04:05Z07:00")

	if err := m.k8sClient.CreateOrUpdate(ctx, deployment); err != nil {
		return fmt.Errorf("failed to trigger deployment restart: %w", err)
	}

	m.logger.Info("triggered PgDog deployment restart",
		zap.String("deployment", poolerName),
		zap.String("namespace", spec.Namespace),
	)

	return nil
}

// getPoolerName returns the pooler resource name for a cluster
func (m *PoolerManager) getPoolerName(clusterName string) string {
	return clusterName + "-pgdog"
}

// getPoolerLabels returns the standard labels for pooler resources
func (m *PoolerManager) getPoolerLabels(spec *types.ClusterSpec) map[string]string {
	labels := map[string]string{
		"app":                          "pgdog",
		"app.kubernetes.io/name":       "pgdog",
		"app.kubernetes.io/component":  "connection-pooler",
		"app.kubernetes.io/part-of":    "orochi-db",
		"app.kubernetes.io/managed-by": "orochi-provisioner",
		"orochi.io/cluster-name":       spec.Name,
	}

	// Add user-specified labels
	for k, v := range spec.Labels {
		labels[k] = v
	}

	return labels
}

// Helper functions
func boolPtr(b bool) *bool {
	return &b
}

func int32Ptr(i int32) *int32 {
	return &i
}

func int64Ptr(i int64) *int64 {
	return &i
}
