// Package k8s provides Kubernetes resource management functionality.
package k8s

import (
	"context"
	"fmt"

	"go.uber.org/zap"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"sigs.k8s.io/controller-runtime/pkg/client"
)

// ResourceManager manages Kubernetes resources
type ResourceManager struct {
	client *Client
	logger *zap.Logger
}

// NewResourceManager creates a new ResourceManager
func NewResourceManager(client *Client, logger *zap.Logger) *ResourceManager {
	return &ResourceManager{
		client: client,
		logger: logger,
	}
}

// EnsureNamespace ensures a namespace exists
func (r *ResourceManager) EnsureNamespace(ctx context.Context, name string, labels map[string]string) error {
	ns := &corev1.Namespace{
		ObjectMeta: metav1.ObjectMeta{
			Name:   name,
			Labels: labels,
		},
	}

	err := r.client.ctrlClient.Create(ctx, ns)
	if err != nil {
		if errors.IsAlreadyExists(err) {
			return nil
		}
		return fmt.Errorf("failed to create namespace %s: %w", name, err)
	}

	r.logger.Info("created namespace", zap.String("namespace", name))
	return nil
}

// DeleteNamespace deletes a namespace
func (r *ResourceManager) DeleteNamespace(ctx context.Context, name string) error {
	ns := &corev1.Namespace{
		ObjectMeta: metav1.ObjectMeta{
			Name: name,
		},
	}

	err := r.client.ctrlClient.Delete(ctx, ns)
	if err != nil {
		if errors.IsNotFound(err) {
			return nil
		}
		return fmt.Errorf("failed to delete namespace %s: %w", name, err)
	}

	r.logger.Info("deleted namespace", zap.String("namespace", name))
	return nil
}

// CreateSecret creates a Kubernetes secret
func (r *ResourceManager) CreateSecret(ctx context.Context, namespace, name string, data map[string][]byte, secretType corev1.SecretType) error {
	secret := &corev1.Secret{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
		Type: secretType,
		Data: data,
	}

	return r.client.CreateOrUpdate(ctx, secret)
}

// GetSecret retrieves a Kubernetes secret
func (r *ResourceManager) GetSecret(ctx context.Context, namespace, name string) (*corev1.Secret, error) {
	secret := &corev1.Secret{}
	err := r.client.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, secret)
	if err != nil {
		return nil, err
	}
	return secret, nil
}

// DeleteSecret deletes a Kubernetes secret
func (r *ResourceManager) DeleteSecret(ctx context.Context, namespace, name string) error {
	secret := &corev1.Secret{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
	}
	return r.client.Delete(ctx, secret)
}

// CreateConfigMap creates a Kubernetes ConfigMap
func (r *ResourceManager) CreateConfigMap(ctx context.Context, namespace, name string, data map[string]string) error {
	cm := &corev1.ConfigMap{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
		Data: data,
	}

	return r.client.CreateOrUpdate(ctx, cm)
}

// GetConfigMap retrieves a Kubernetes ConfigMap
func (r *ResourceManager) GetConfigMap(ctx context.Context, namespace, name string) (*corev1.ConfigMap, error) {
	cm := &corev1.ConfigMap{}
	err := r.client.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, cm)
	if err != nil {
		return nil, err
	}
	return cm, nil
}

// DeleteConfigMap deletes a Kubernetes ConfigMap
func (r *ResourceManager) DeleteConfigMap(ctx context.Context, namespace, name string) error {
	cm := &corev1.ConfigMap{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
	}
	return r.client.Delete(ctx, cm)
}

// CreatePVC creates a PersistentVolumeClaim
func (r *ResourceManager) CreatePVC(ctx context.Context, namespace, name, storageClass, size string, accessModes []corev1.PersistentVolumeAccessMode) error {
	storageQuantity, err := resource.ParseQuantity(size)
	if err != nil {
		return fmt.Errorf("invalid storage size %s: %w", size, err)
	}

	pvc := &corev1.PersistentVolumeClaim{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
		Spec: corev1.PersistentVolumeClaimSpec{
			AccessModes: accessModes,
			Resources: corev1.VolumeResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceStorage: storageQuantity,
				},
			},
		},
	}

	if storageClass != "" {
		pvc.Spec.StorageClassName = &storageClass
	}

	return r.client.CreateOrUpdate(ctx, pvc)
}

// DeletePVC deletes a PersistentVolumeClaim
func (r *ResourceManager) DeletePVC(ctx context.Context, namespace, name string) error {
	pvc := &corev1.PersistentVolumeClaim{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
	}
	return r.client.Delete(ctx, pvc)
}

// ListPVCs lists PersistentVolumeClaims in a namespace with label selector
func (r *ResourceManager) ListPVCs(ctx context.Context, namespace string, labelSelector map[string]string) (*corev1.PersistentVolumeClaimList, error) {
	pvcList := &corev1.PersistentVolumeClaimList{}
	opts := []client.ListOption{
		client.InNamespace(namespace),
	}

	if len(labelSelector) > 0 {
		opts = append(opts, client.MatchingLabels(labelSelector))
	}

	if err := r.client.List(ctx, pvcList, opts...); err != nil {
		return nil, err
	}
	return pvcList, nil
}

// CreateService creates a Kubernetes Service
func (r *ResourceManager) CreateService(ctx context.Context, namespace, name string, spec corev1.ServiceSpec, labels map[string]string) error {
	svc := &corev1.Service{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
			Labels:    labels,
		},
		Spec: spec,
	}

	return r.client.CreateOrUpdate(ctx, svc)
}

// DeleteService deletes a Kubernetes Service
func (r *ResourceManager) DeleteService(ctx context.Context, namespace, name string) error {
	svc := &corev1.Service{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: namespace,
		},
	}
	return r.client.Delete(ctx, svc)
}

// GetServiceEndpoint returns the endpoint for a service
func (r *ResourceManager) GetServiceEndpoint(ctx context.Context, namespace, name string) (string, error) {
	svc := &corev1.Service{}
	err := r.client.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, svc)
	if err != nil {
		return "", err
	}

	// For ClusterIP services
	if svc.Spec.Type == corev1.ServiceTypeClusterIP {
		if len(svc.Spec.Ports) > 0 {
			return fmt.Sprintf("%s:%d", svc.Spec.ClusterIP, svc.Spec.Ports[0].Port), nil
		}
		return svc.Spec.ClusterIP, nil
	}

	// For LoadBalancer services
	if svc.Spec.Type == corev1.ServiceTypeLoadBalancer {
		if len(svc.Status.LoadBalancer.Ingress) > 0 {
			ingress := svc.Status.LoadBalancer.Ingress[0]
			if ingress.IP != "" {
				if len(svc.Spec.Ports) > 0 {
					return fmt.Sprintf("%s:%d", ingress.IP, svc.Spec.Ports[0].Port), nil
				}
				return ingress.IP, nil
			}
			if ingress.Hostname != "" {
				if len(svc.Spec.Ports) > 0 {
					return fmt.Sprintf("%s:%d", ingress.Hostname, svc.Spec.Ports[0].Port), nil
				}
				return ingress.Hostname, nil
			}
		}
	}

	return "", fmt.Errorf("no endpoint available for service %s/%s", namespace, name)
}

// ListPods lists pods in a namespace with label selector
func (r *ResourceManager) ListPods(ctx context.Context, namespace string, labelSelector map[string]string) (*corev1.PodList, error) {
	podList := &corev1.PodList{}
	opts := []client.ListOption{
		client.InNamespace(namespace),
	}

	if len(labelSelector) > 0 {
		opts = append(opts, client.MatchingLabels(labelSelector))
	}

	if err := r.client.List(ctx, podList, opts...); err != nil {
		return nil, err
	}
	return podList, nil
}

// GetPod retrieves a pod by name
func (r *ResourceManager) GetPod(ctx context.Context, namespace, name string) (*corev1.Pod, error) {
	pod := &corev1.Pod{}
	err := r.client.Get(ctx, client.ObjectKey{Namespace: namespace, Name: name}, pod)
	if err != nil {
		return nil, err
	}
	return pod, nil
}

// WaitForPodReady waits for a pod to be ready
func (r *ResourceManager) WaitForPodReady(ctx context.Context, namespace, name string) error {
	return r.client.WaitForCondition(ctx, r.client.cfg.Timeout, func() (bool, error) {
		pod, err := r.GetPod(ctx, namespace, name)
		if err != nil {
			if errors.IsNotFound(err) {
				return false, nil
			}
			return false, err
		}

		for _, cond := range pod.Status.Conditions {
			if cond.Type == corev1.PodReady && cond.Status == corev1.ConditionTrue {
				return true, nil
			}
		}
		return false, nil
	})
}

// GetEvents retrieves events for an object
func (r *ResourceManager) GetEvents(ctx context.Context, namespace, name, kind string) (*corev1.EventList, error) {
	events, err := r.client.Clientset().CoreV1().Events(namespace).List(ctx, metav1.ListOptions{
		FieldSelector: fmt.Sprintf("involvedObject.name=%s,involvedObject.kind=%s", name, kind),
	})
	if err != nil {
		return nil, err
	}
	return events, nil
}
