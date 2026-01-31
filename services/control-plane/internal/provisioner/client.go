// Package provisioner provides a gRPC client for the provisioner service.
package provisioner

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"time"

	pb "github.com/orochi-db/orochi-db/services/control-plane/api/proto"
	"github.com/orochi-db/orochi-db/services/control-plane/pkg/config"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"
)

// Client is a gRPC client for the provisioner service.
type Client struct {
	config config.ProvisionerConfig
	logger *slog.Logger

	mu     sync.RWMutex
	conn   *grpc.ClientConn
	client pb.ProvisionerServiceClient
}

// NewClient creates a new provisioner client.
func NewClient(cfg config.ProvisionerConfig, logger *slog.Logger) (*Client, error) {
	c := &Client{
		config: cfg,
		logger: logger.With("component", "provisioner-client"),
	}

	if cfg.Enabled {
		if err := c.connect(); err != nil {
			return nil, fmt.Errorf("failed to connect to provisioner: %w", err)
		}
	}

	return c, nil
}

// connect establishes the gRPC connection.
func (c *Client) connect() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil {
		return nil
	}

	opts := []grpc.DialOption{
		grpc.WithKeepaliveParams(keepalive.ClientParameters{
			Time:                10 * time.Second,
			Timeout:             5 * time.Second,
			PermitWithoutStream: true,
		}),
	}

	if c.config.TLSEnabled && !c.config.Insecure {
		// TODO: Add TLS credentials
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
	} else {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
	}

	conn, err := grpc.NewClient(c.config.Addr, opts...)
	if err != nil {
		return fmt.Errorf("failed to create gRPC client: %w", err)
	}

	c.conn = conn
	c.client = pb.NewProvisionerServiceClient(conn)

	c.logger.Info("connected to provisioner", "addr", c.config.Addr)
	return nil
}

// Close closes the gRPC connection.
func (c *Client) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil {
		if err := c.conn.Close(); err != nil {
			return fmt.Errorf("failed to close connection: %w", err)
		}
		c.conn = nil
		c.client = nil
	}
	return nil
}

// IsEnabled returns whether the provisioner is enabled.
func (c *Client) IsEnabled() bool {
	return c.config.Enabled
}

// getClient returns the gRPC client, reconnecting if necessary.
func (c *Client) getClient() (pb.ProvisionerServiceClient, error) {
	c.mu.RLock()
	if c.client != nil {
		defer c.mu.RUnlock()
		return c.client, nil
	}
	c.mu.RUnlock()

	if err := c.connect(); err != nil {
		return nil, err
	}

	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.client, nil
}

// CreateCluster creates a new cluster via the provisioner.
func (c *Client) CreateCluster(ctx context.Context, req *pb.CreateClusterRequest) (*pb.CreateClusterResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, c.config.DefaultTimeout)
	defer cancel()

	resp, err := client.CreateCluster(ctx, req)
	if err != nil {
		c.logger.Error("failed to create cluster", "error", err, "name", req.Spec.Name)
		return nil, fmt.Errorf("provisioner.CreateCluster: %w", err)
	}

	c.logger.Info("cluster creation initiated",
		"cluster_id", resp.ClusterId,
		"name", resp.Name,
		"phase", resp.Phase.String(),
	)
	return resp, nil
}

// UpdateCluster updates an existing cluster via the provisioner.
func (c *Client) UpdateCluster(ctx context.Context, req *pb.UpdateClusterRequest) (*pb.UpdateClusterResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, c.config.DefaultTimeout)
	defer cancel()

	resp, err := client.UpdateCluster(ctx, req)
	if err != nil {
		c.logger.Error("failed to update cluster", "error", err, "cluster_id", req.ClusterId)
		return nil, fmt.Errorf("provisioner.UpdateCluster: %w", err)
	}

	c.logger.Info("cluster update initiated",
		"cluster_id", resp.ClusterId,
		"phase", resp.Phase.String(),
	)
	return resp, nil
}

// DeleteCluster deletes a cluster via the provisioner.
func (c *Client) DeleteCluster(ctx context.Context, req *pb.DeleteClusterRequest) (*pb.DeleteClusterResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, c.config.DefaultTimeout)
	defer cancel()

	resp, err := client.DeleteCluster(ctx, req)
	if err != nil {
		c.logger.Error("failed to delete cluster", "error", err, "cluster_id", req.ClusterId)
		return nil, fmt.Errorf("provisioner.DeleteCluster: %w", err)
	}

	c.logger.Info("cluster deletion initiated",
		"cluster_id", req.ClusterId,
		"success", resp.Success,
	)
	return resp, nil
}

// GetCluster retrieves cluster information from the provisioner.
func (c *Client) GetCluster(ctx context.Context, req *pb.GetClusterRequest) (*pb.GetClusterResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, c.config.Timeout)
	defer cancel()

	resp, err := client.GetCluster(ctx, req)
	if err != nil {
		c.logger.Error("failed to get cluster", "error", err, "cluster_id", req.ClusterId)
		return nil, fmt.Errorf("provisioner.GetCluster: %w", err)
	}

	return resp, nil
}

// GetClusterStatus retrieves cluster status from the provisioner.
func (c *Client) GetClusterStatus(ctx context.Context, req *pb.GetClusterStatusRequest) (*pb.GetClusterStatusResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, c.config.Timeout)
	defer cancel()

	resp, err := client.GetClusterStatus(ctx, req)
	if err != nil {
		c.logger.Error("failed to get cluster status", "error", err, "cluster_id", req.ClusterId)
		return nil, fmt.Errorf("provisioner.GetClusterStatus: %w", err)
	}

	return resp, nil
}

// HealthCheck checks the provisioner health.
func (c *Client) HealthCheck(ctx context.Context) (*pb.HealthCheckResponse, error) {
	if !c.config.Enabled {
		return nil, fmt.Errorf("provisioner is not enabled")
	}

	client, err := c.getClient()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()

	resp, err := client.HealthCheck(ctx, nil)
	if err != nil {
		c.logger.Error("provisioner health check failed", "error", err)
		return nil, fmt.Errorf("provisioner.HealthCheck: %w", err)
	}

	return resp, nil
}
