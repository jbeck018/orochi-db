// Package k8s provides Kubernetes client functionality for the autoscaler.
package k8s

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/cache"
	"k8s.io/client-go/tools/clientcmd"
)

// ClientConfig holds Kubernetes client configuration.
type ClientConfig struct {
	InCluster     bool
	Kubeconfig    string
	Namespace     string
	ResyncPeriod  time.Duration
	QPS           float32
	Burst         int
	LabelSelector string
}

// Client provides Kubernetes operations for the autoscaler.
type Client struct {
	clientset       *kubernetes.Clientset
	informerFactory informers.SharedInformerFactory
	namespace       string
	labelSelector   labels.Selector

	// Informers
	statefulSetInformer cache.SharedIndexInformer
	deploymentInformer  cache.SharedIndexInformer
	podInformer         cache.SharedIndexInformer
	serviceInformer     cache.SharedIndexInformer

	// Event handlers
	mu              sync.RWMutex
	eventHandlers   []ResourceEventHandler
	stopCh          chan struct{}
	informerStarted bool
}

// ResourceEventHandler handles resource events.
type ResourceEventHandler interface {
	OnAdd(obj interface{})
	OnUpdate(oldObj, newObj interface{})
	OnDelete(obj interface{})
}

// ClusterStatus represents the status of a database cluster.
type ClusterStatus struct {
	ClusterID       string
	Namespace       string
	CurrentReplicas int32
	DesiredReplicas int32
	ReadyReplicas   int32
	UpdatedReplicas int32
	Resources       *ResourceRequirements
	Conditions      []ClusterCondition
	Pods            []PodInfo
}

// ClusterCondition represents a condition of the cluster.
type ClusterCondition struct {
	Type    string
	Status  string
	Reason  string
	Message string
}

// PodInfo contains information about a pod.
type PodInfo struct {
	Name         string
	Phase        string
	Ready        bool
	RestartCount int32
	CPU          *resource.Quantity
	Memory       *resource.Quantity
}

// ResourceRequirements holds resource requests and limits.
type ResourceRequirements struct {
	CPURequest    resource.Quantity
	CPULimit      resource.Quantity
	MemoryRequest resource.Quantity
	MemoryLimit   resource.Quantity
	Storage       resource.Quantity
}

// NewClient creates a new Kubernetes client.
func NewClient(cfg ClientConfig) (*Client, error) {
	var config *rest.Config
	var err error

	if cfg.InCluster {
		config, err = rest.InClusterConfig()
		if err != nil {
			return nil, fmt.Errorf("failed to create in-cluster config: %w", err)
		}
	} else {
		kubeconfig := cfg.Kubeconfig
		if kubeconfig == "" {
			home, _ := os.UserHomeDir()
			kubeconfig = filepath.Join(home, ".kube", "config")
		}
		config, err = clientcmd.BuildConfigFromFlags("", kubeconfig)
		if err != nil {
			return nil, fmt.Errorf("failed to create config from kubeconfig: %w", err)
		}
	}

	config.QPS = cfg.QPS
	config.Burst = cfg.Burst

	clientset, err := kubernetes.NewForConfig(config)
	if err != nil {
		return nil, fmt.Errorf("failed to create clientset: %w", err)
	}

	// Create informer factory
	var informerFactory informers.SharedInformerFactory
	if cfg.Namespace != "" {
		informerFactory = informers.NewSharedInformerFactoryWithOptions(
			clientset,
			cfg.ResyncPeriod,
			informers.WithNamespace(cfg.Namespace),
		)
	} else {
		informerFactory = informers.NewSharedInformerFactory(clientset, cfg.ResyncPeriod)
	}

	// Parse label selector
	var labelSelector labels.Selector
	if cfg.LabelSelector != "" {
		labelSelector, err = labels.Parse(cfg.LabelSelector)
		if err != nil {
			return nil, fmt.Errorf("failed to parse label selector: %w", err)
		}
	} else {
		labelSelector = labels.Everything()
	}

	client := &Client{
		clientset:           clientset,
		informerFactory:     informerFactory,
		namespace:           cfg.Namespace,
		labelSelector:       labelSelector,
		statefulSetInformer: informerFactory.Apps().V1().StatefulSets().Informer(),
		deploymentInformer:  informerFactory.Apps().V1().Deployments().Informer(),
		podInformer:         informerFactory.Core().V1().Pods().Informer(),
		serviceInformer:     informerFactory.Core().V1().Services().Informer(),
		stopCh:              make(chan struct{}),
	}

	return client, nil
}

// Start starts the informers.
func (c *Client) Start(ctx context.Context) error {
	c.mu.Lock()
	if c.informerStarted {
		c.mu.Unlock()
		return nil
	}
	c.informerStarted = true
	c.mu.Unlock()

	// Add event handlers
	c.statefulSetInformer.AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc:    c.handleAdd,
		UpdateFunc: c.handleUpdate,
		DeleteFunc: c.handleDelete,
	})

	c.deploymentInformer.AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc:    c.handleAdd,
		UpdateFunc: c.handleUpdate,
		DeleteFunc: c.handleDelete,
	})

	// Start informer factory
	c.informerFactory.Start(c.stopCh)

	// Wait for caches to sync
	syncCtx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()

	if !cache.WaitForCacheSync(syncCtx.Done(),
		c.statefulSetInformer.HasSynced,
		c.deploymentInformer.HasSynced,
		c.podInformer.HasSynced,
		c.serviceInformer.HasSynced,
	) {
		return fmt.Errorf("failed to sync informer caches")
	}

	return nil
}

// Stop stops the informers.
func (c *Client) Stop() {
	close(c.stopCh)
}

// RegisterEventHandler registers an event handler.
func (c *Client) RegisterEventHandler(handler ResourceEventHandler) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.eventHandlers = append(c.eventHandlers, handler)
}

func (c *Client) handleAdd(obj interface{}) {
	c.mu.RLock()
	handlers := c.eventHandlers
	c.mu.RUnlock()

	for _, h := range handlers {
		h.OnAdd(obj)
	}
}

func (c *Client) handleUpdate(oldObj, newObj interface{}) {
	c.mu.RLock()
	handlers := c.eventHandlers
	c.mu.RUnlock()

	for _, h := range handlers {
		h.OnUpdate(oldObj, newObj)
	}
}

func (c *Client) handleDelete(obj interface{}) {
	c.mu.RLock()
	handlers := c.eventHandlers
	c.mu.RUnlock()

	for _, h := range handlers {
		h.OnDelete(obj)
	}
}

// GetStatefulSet retrieves a StatefulSet.
func (c *Client) GetStatefulSet(ctx context.Context, name, namespace string) (*appsv1.StatefulSet, error) {
	return c.clientset.AppsV1().StatefulSets(namespace).Get(ctx, name, metav1.GetOptions{})
}

// GetDeployment retrieves a Deployment.
func (c *Client) GetDeployment(ctx context.Context, name, namespace string) (*appsv1.Deployment, error) {
	return c.clientset.AppsV1().Deployments(namespace).Get(ctx, name, metav1.GetOptions{})
}

// GetClusterStatus retrieves the status of a database cluster.
func (c *Client) GetClusterStatus(ctx context.Context, clusterID, namespace string) (*ClusterStatus, error) {
	// Try StatefulSet first (typical for databases)
	sts, err := c.GetStatefulSet(ctx, clusterID, namespace)
	if err == nil {
		return c.statefulSetToClusterStatus(ctx, sts)
	}

	// Fall back to Deployment
	deploy, err := c.GetDeployment(ctx, clusterID, namespace)
	if err == nil {
		return c.deploymentToClusterStatus(ctx, deploy)
	}

	return nil, fmt.Errorf("cluster %s/%s not found", namespace, clusterID)
}

func (c *Client) statefulSetToClusterStatus(ctx context.Context, sts *appsv1.StatefulSet) (*ClusterStatus, error) {
	status := &ClusterStatus{
		ClusterID:       sts.Name,
		Namespace:       sts.Namespace,
		CurrentReplicas: sts.Status.Replicas,
		DesiredReplicas: *sts.Spec.Replicas,
		ReadyReplicas:   sts.Status.ReadyReplicas,
		UpdatedReplicas: sts.Status.UpdatedReplicas,
	}

	// Extract resource requirements from container spec
	if len(sts.Spec.Template.Spec.Containers) > 0 {
		container := sts.Spec.Template.Spec.Containers[0]
		status.Resources = &ResourceRequirements{
			CPURequest:    container.Resources.Requests[corev1.ResourceCPU],
			CPULimit:      container.Resources.Limits[corev1.ResourceCPU],
			MemoryRequest: container.Resources.Requests[corev1.ResourceMemory],
			MemoryLimit:   container.Resources.Limits[corev1.ResourceMemory],
		}
	}

	// Extract storage from volume claim templates
	if len(sts.Spec.VolumeClaimTemplates) > 0 {
		if status.Resources == nil {
			status.Resources = &ResourceRequirements{}
		}
		status.Resources.Storage = sts.Spec.VolumeClaimTemplates[0].Spec.Resources.Requests[corev1.ResourceStorage]
	}

	// Get pod information
	pods, err := c.getPodsForCluster(ctx, sts.Namespace, sts.Spec.Selector.MatchLabels)
	if err == nil {
		status.Pods = pods
	}

	// Convert conditions
	for _, cond := range sts.Status.Conditions {
		status.Conditions = append(status.Conditions, ClusterCondition{
			Type:    string(cond.Type),
			Status:  string(cond.Status),
			Reason:  cond.Reason,
			Message: cond.Message,
		})
	}

	return status, nil
}

func (c *Client) deploymentToClusterStatus(ctx context.Context, deploy *appsv1.Deployment) (*ClusterStatus, error) {
	status := &ClusterStatus{
		ClusterID:       deploy.Name,
		Namespace:       deploy.Namespace,
		CurrentReplicas: deploy.Status.Replicas,
		DesiredReplicas: *deploy.Spec.Replicas,
		ReadyReplicas:   deploy.Status.ReadyReplicas,
		UpdatedReplicas: deploy.Status.UpdatedReplicas,
	}

	// Extract resource requirements
	if len(deploy.Spec.Template.Spec.Containers) > 0 {
		container := deploy.Spec.Template.Spec.Containers[0]
		status.Resources = &ResourceRequirements{
			CPURequest:    container.Resources.Requests[corev1.ResourceCPU],
			CPULimit:      container.Resources.Limits[corev1.ResourceCPU],
			MemoryRequest: container.Resources.Requests[corev1.ResourceMemory],
			MemoryLimit:   container.Resources.Limits[corev1.ResourceMemory],
		}
	}

	// Get pod information
	pods, err := c.getPodsForCluster(ctx, deploy.Namespace, deploy.Spec.Selector.MatchLabels)
	if err == nil {
		status.Pods = pods
	}

	// Convert conditions
	for _, cond := range deploy.Status.Conditions {
		status.Conditions = append(status.Conditions, ClusterCondition{
			Type:    string(cond.Type),
			Status:  string(cond.Status),
			Reason:  cond.Reason,
			Message: cond.Message,
		})
	}

	return status, nil
}

func (c *Client) getPodsForCluster(ctx context.Context, namespace string, matchLabels map[string]string) ([]PodInfo, error) {
	selector := labels.SelectorFromSet(matchLabels)
	pods, err := c.clientset.CoreV1().Pods(namespace).List(ctx, metav1.ListOptions{
		LabelSelector: selector.String(),
	})
	if err != nil {
		return nil, err
	}

	var podInfos []PodInfo
	for _, pod := range pods.Items {
		ready := false
		for _, cond := range pod.Status.Conditions {
			if cond.Type == corev1.PodReady && cond.Status == corev1.ConditionTrue {
				ready = true
				break
			}
		}

		var restartCount int32
		if len(pod.Status.ContainerStatuses) > 0 {
			restartCount = pod.Status.ContainerStatuses[0].RestartCount
		}

		podInfos = append(podInfos, PodInfo{
			Name:         pod.Name,
			Phase:        string(pod.Status.Phase),
			Ready:        ready,
			RestartCount: restartCount,
		})
	}

	return podInfos, nil
}

// ScaleStatefulSet scales a StatefulSet to the desired replica count.
func (c *Client) ScaleStatefulSet(ctx context.Context, name, namespace string, replicas int32) error {
	patch := []byte(fmt.Sprintf(`{"spec":{"replicas":%d}}`, replicas))
	_, err := c.clientset.AppsV1().StatefulSets(namespace).Patch(
		ctx,
		name,
		types.MergePatchType,
		patch,
		metav1.PatchOptions{},
	)
	return err
}

// ScaleDeployment scales a Deployment to the desired replica count.
func (c *Client) ScaleDeployment(ctx context.Context, name, namespace string, replicas int32) error {
	patch := []byte(fmt.Sprintf(`{"spec":{"replicas":%d}}`, replicas))
	_, err := c.clientset.AppsV1().Deployments(namespace).Patch(
		ctx,
		name,
		types.MergePatchType,
		patch,
		metav1.PatchOptions{},
	)
	return err
}

// UpdateStatefulSetResources updates the resource requirements of a StatefulSet.
func (c *Client) UpdateStatefulSetResources(ctx context.Context, name, namespace string, resources *ResourceRequirements) error {
	sts, err := c.GetStatefulSet(ctx, name, namespace)
	if err != nil {
		return err
	}

	if len(sts.Spec.Template.Spec.Containers) == 0 {
		return fmt.Errorf("no containers found in StatefulSet")
	}

	// Update the first container's resources
	sts.Spec.Template.Spec.Containers[0].Resources = corev1.ResourceRequirements{
		Requests: corev1.ResourceList{
			corev1.ResourceCPU:    resources.CPURequest,
			corev1.ResourceMemory: resources.MemoryRequest,
		},
		Limits: corev1.ResourceList{
			corev1.ResourceCPU:    resources.CPULimit,
			corev1.ResourceMemory: resources.MemoryLimit,
		},
	}

	_, err = c.clientset.AppsV1().StatefulSets(namespace).Update(ctx, sts, metav1.UpdateOptions{})
	return err
}

// UpdateDeploymentResources updates the resource requirements of a Deployment.
func (c *Client) UpdateDeploymentResources(ctx context.Context, name, namespace string, resources *ResourceRequirements) error {
	deploy, err := c.GetDeployment(ctx, name, namespace)
	if err != nil {
		return err
	}

	if len(deploy.Spec.Template.Spec.Containers) == 0 {
		return fmt.Errorf("no containers found in Deployment")
	}

	// Update the first container's resources
	deploy.Spec.Template.Spec.Containers[0].Resources = corev1.ResourceRequirements{
		Requests: corev1.ResourceList{
			corev1.ResourceCPU:    resources.CPURequest,
			corev1.ResourceMemory: resources.MemoryRequest,
		},
		Limits: corev1.ResourceList{
			corev1.ResourceCPU:    resources.CPULimit,
			corev1.ResourceMemory: resources.MemoryLimit,
		},
	}

	_, err = c.clientset.AppsV1().Deployments(namespace).Update(ctx, deploy, metav1.UpdateOptions{})
	return err
}

// WaitForScaleComplete waits for scaling to complete.
func (c *Client) WaitForScaleComplete(ctx context.Context, clusterID, namespace string, targetReplicas int32, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return fmt.Errorf("timeout waiting for scale to complete")
		case <-ticker.C:
			status, err := c.GetClusterStatus(ctx, clusterID, namespace)
			if err != nil {
				continue
			}

			if status.ReadyReplicas == targetReplicas {
				return nil
			}
		}
	}
}

// ListClusters lists all database clusters in the namespace.
func (c *Client) ListClusters(ctx context.Context, namespace string) ([]string, error) {
	var clusters []string

	// List StatefulSets
	stsList, err := c.clientset.AppsV1().StatefulSets(namespace).List(ctx, metav1.ListOptions{
		LabelSelector: c.labelSelector.String(),
	})
	if err != nil {
		return nil, err
	}

	for _, sts := range stsList.Items {
		clusters = append(clusters, sts.Name)
	}

	return clusters, nil
}

// AnnotateResource adds annotations to a resource.
func (c *Client) AnnotateResource(ctx context.Context, kind, name, namespace string, annotations map[string]string) error {
	annotationsJSON := "{"
	first := true
	for k, v := range annotations {
		if !first {
			annotationsJSON += ","
		}
		annotationsJSON += fmt.Sprintf(`"%s":"%s"`, k, v)
		first = false
	}
	annotationsJSON += "}"

	patch := []byte(fmt.Sprintf(`{"metadata":{"annotations":%s}}`, annotationsJSON))

	switch kind {
	case "StatefulSet":
		_, err := c.clientset.AppsV1().StatefulSets(namespace).Patch(
			ctx, name, types.MergePatchType, patch, metav1.PatchOptions{})
		return err
	case "Deployment":
		_, err := c.clientset.AppsV1().Deployments(namespace).Patch(
			ctx, name, types.MergePatchType, patch, metav1.PatchOptions{})
		return err
	default:
		return fmt.Errorf("unsupported resource kind: %s", kind)
	}
}

// GetAnnotation retrieves an annotation from a resource.
func (c *Client) GetAnnotation(ctx context.Context, kind, name, namespace, key string) (string, bool, error) {
	switch kind {
	case "StatefulSet":
		sts, err := c.GetStatefulSet(ctx, name, namespace)
		if err != nil {
			return "", false, err
		}
		val, ok := sts.Annotations[key]
		return val, ok, nil
	case "Deployment":
		deploy, err := c.GetDeployment(ctx, name, namespace)
		if err != nil {
			return "", false, err
		}
		val, ok := deploy.Annotations[key]
		return val, ok, nil
	default:
		return "", false, fmt.Errorf("unsupported resource kind: %s", kind)
	}
}
