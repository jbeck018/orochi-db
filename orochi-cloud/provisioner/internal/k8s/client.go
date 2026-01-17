// Package k8s provides Kubernetes client functionality for the provisioner service.
package k8s

import (
	"context"
	"fmt"
	"time"

	"go.uber.org/zap"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/runtime"
	utilruntime "k8s.io/apimachinery/pkg/util/runtime"
	"k8s.io/apimachinery/pkg/util/wait"
	"k8s.io/client-go/kubernetes"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"

	"github.com/orochi-db/orochi-cloud/provisioner/pkg/config"
)

// Client provides Kubernetes client operations with retry logic
type Client struct {
	cfg        *config.KubernetesConfig
	restConfig *rest.Config
	clientset  *kubernetes.Clientset
	ctrlClient client.Client
	logger     *zap.Logger
	scheme     *runtime.Scheme
}

// NewClient creates a new Kubernetes client
func NewClient(cfg *config.KubernetesConfig, logger *zap.Logger) (*Client, error) {
	var restConfig *rest.Config
	var err error

	if cfg.InCluster {
		restConfig, err = rest.InClusterConfig()
		if err != nil {
			return nil, fmt.Errorf("failed to create in-cluster config: %w", err)
		}
	} else {
		restConfig, err = clientcmd.BuildConfigFromFlags("", cfg.KubeconfigPath)
		if err != nil {
			return nil, fmt.Errorf("failed to build config from kubeconfig: %w", err)
		}
	}

	// Configure rate limiting
	restConfig.QPS = cfg.QPS
	restConfig.Burst = cfg.Burst
	restConfig.Timeout = cfg.Timeout

	// Create clientset for core API operations
	clientset, err := kubernetes.NewForConfig(restConfig)
	if err != nil {
		return nil, fmt.Errorf("failed to create kubernetes clientset: %w", err)
	}

	// Create scheme with standard Kubernetes types
	// We use unstructured objects for CloudNativePG resources
	scheme := runtime.NewScheme()
	utilruntime.Must(clientgoscheme.AddToScheme(scheme))
	utilruntime.Must(corev1.AddToScheme(scheme))

	// Create controller-runtime client for CRD operations
	// Use unstructured objects for CloudNativePG resources
	ctrlClient, err := client.New(restConfig, client.Options{
		Scheme: scheme,
	})
	if err != nil {
		return nil, fmt.Errorf("failed to create controller-runtime client: %w", err)
	}

	return &Client{
		cfg:        cfg,
		restConfig: restConfig,
		clientset:  clientset,
		ctrlClient: ctrlClient,
		logger:     logger,
		scheme:     scheme,
	}, nil
}

// Clientset returns the core Kubernetes clientset
func (c *Client) Clientset() *kubernetes.Clientset {
	return c.clientset
}

// CtrlClient returns the controller-runtime client
func (c *Client) CtrlClient() client.Client {
	return c.ctrlClient
}

// RestConfig returns the REST config
func (c *Client) RestConfig() *rest.Config {
	return c.restConfig
}

// Scheme returns the runtime scheme
func (c *Client) Scheme() *runtime.Scheme {
	return c.scheme
}

// WithRetry executes an operation with retry logic
func (c *Client) WithRetry(ctx context.Context, operation string, fn func() error) error {
	backoff := wait.Backoff{
		Duration: c.cfg.RetryConfig.InitialBackoff,
		Factor:   c.cfg.RetryConfig.BackoffMultiplier,
		Cap:      c.cfg.RetryConfig.MaxBackoff,
		Steps:    c.cfg.RetryConfig.MaxRetries,
	}

	var lastErr error
	err := wait.ExponentialBackoffWithContext(ctx, backoff, func(ctx context.Context) (bool, error) {
		if err := fn(); err != nil {
			lastErr = err
			c.logger.Warn("operation failed, retrying",
				zap.String("operation", operation),
				zap.Error(err),
			)
			return false, nil
		}
		return true, nil
	})

	if err != nil {
		if lastErr != nil {
			return fmt.Errorf("operation %s failed after retries: %w", operation, lastErr)
		}
		return fmt.Errorf("operation %s failed: %w", operation, err)
	}

	return nil
}

// WaitForCondition waits for a condition to be true with timeout using exponential backoff
func (c *Client) WaitForCondition(ctx context.Context, timeout time.Duration, condition func() (bool, error)) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	// Use exponential backoff: start at 100ms, double each time, cap at 10s
	backoff := wait.Backoff{
		Duration: 100 * time.Millisecond,
		Factor:   2.0,
		Jitter:   0.1,
		Cap:      10 * time.Second,
		Steps:    -1, // Unlimited steps (bounded by context timeout)
	}

	return wait.ExponentialBackoffWithContext(ctx, backoff, func(ctx context.Context) (bool, error) {
		return condition()
	})
}

// HealthCheck performs a health check on the Kubernetes API
func (c *Client) HealthCheck(ctx context.Context) error {
	_, err := c.clientset.Discovery().ServerVersion()
	if err != nil {
		return fmt.Errorf("kubernetes API health check failed: %w", err)
	}
	return nil
}

// GetManager creates a controller-runtime manager
func (c *Client) GetManager(ctx context.Context) (ctrl.Manager, error) {
	mgr, err := ctrl.NewManager(c.restConfig, ctrl.Options{
		Scheme: c.scheme,
	})
	if err != nil {
		return nil, fmt.Errorf("failed to create manager: %w", err)
	}
	return mgr, nil
}

// CreateOrUpdate creates or updates a Kubernetes resource
// Note: controller-runtime client is thread-safe, no mutex needed
func (c *Client) CreateOrUpdate(ctx context.Context, obj client.Object) error {
	existing := obj.DeepCopyObject().(client.Object)
	err := c.ctrlClient.Get(ctx, client.ObjectKeyFromObject(obj), existing)
	if err != nil {
		if client.IgnoreNotFound(err) != nil {
			return fmt.Errorf("failed to get resource: %w", err)
		}
		// Resource doesn't exist, create it
		if err := c.ctrlClient.Create(ctx, obj); err != nil {
			return fmt.Errorf("failed to create resource: %w", err)
		}
		return nil
	}

	// Resource exists, update it
	obj.SetResourceVersion(existing.GetResourceVersion())
	if err := c.ctrlClient.Update(ctx, obj); err != nil {
		return fmt.Errorf("failed to update resource: %w", err)
	}

	return nil
}

// Delete deletes a Kubernetes resource
// Note: controller-runtime client is thread-safe, no mutex needed
func (c *Client) Delete(ctx context.Context, obj client.Object) error {
	if err := c.ctrlClient.Delete(ctx, obj); err != nil {
		if client.IgnoreNotFound(err) != nil {
			return fmt.Errorf("failed to delete resource: %w", err)
		}
	}
	return nil
}

// Get retrieves a Kubernetes resource
// Note: controller-runtime client is thread-safe, no mutex needed
func (c *Client) Get(ctx context.Context, key client.ObjectKey, obj client.Object) error {
	return c.ctrlClient.Get(ctx, key, obj)
}

// List lists Kubernetes resources
// Note: controller-runtime client is thread-safe, no mutex needed
func (c *Client) List(ctx context.Context, list client.ObjectList, opts ...client.ListOption) error {
	return c.ctrlClient.List(ctx, list, opts...)
}
