// Package wakeup provides wake-on-connect functionality for suspended clusters.
package wakeup

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"sync"
	"time"
)

var (
	// ErrClusterSuspended indicates the cluster is suspended and needs to be woken.
	ErrClusterSuspended = errors.New("cluster is suspended")

	// ErrWakeTimeout indicates the wake operation timed out.
	ErrWakeTimeout = errors.New("wake timeout exceeded")

	// ErrMaxQueuedConnections indicates the connection queue is full.
	ErrMaxQueuedConnections = errors.New("maximum queued connections reached")

	// ErrClusterNotFound indicates the cluster was not found.
	ErrClusterNotFound = errors.New("cluster not found")

	// ErrWakeFailed indicates the wake operation failed.
	ErrWakeFailed = errors.New("wake operation failed")
)

// ClusterState represents the state of a cluster.
type ClusterState string

const (
	StateActive     ClusterState = "running"
	StateSuspended  ClusterState = "suspended"
	StateWaking     ClusterState = "waking"
	StateSuspending ClusterState = "suspending"
)

// ClusterStateResponse represents the response from the control plane.
type ClusterStateResponse struct {
	ClusterID   string       `json:"cluster_id"`
	Status      ClusterState `json:"status"`
	IsReady     bool         `json:"is_ready"`
	Message     string       `json:"message,omitempty"`
	SuspendedAt *time.Time   `json:"suspended_at,omitempty"`
}

// Config holds configuration for the wake handler.
type Config struct {
	// ControlPlaneURL is the URL of the control plane API.
	ControlPlaneURL string

	// WakeTimeout is the maximum time to wait for a cluster to wake.
	WakeTimeout time.Duration

	// PollInterval is how often to poll for cluster readiness.
	PollInterval time.Duration

	// MaxQueuedConnections is the maximum number of connections to queue during wake.
	MaxQueuedConnections int

	// ConnectionQueueTimeout is how long a connection can wait in the queue.
	ConnectionQueueTimeout time.Duration

	// ServiceAccountToken is the token used to authenticate with the control plane.
	ServiceAccountToken string
}

// DefaultConfig returns default wake handler configuration.
func DefaultConfig() *Config {
	return &Config{
		ControlPlaneURL:        "http://control-plane:8080",
		WakeTimeout:            60 * time.Second,
		PollInterval:           500 * time.Millisecond,
		MaxQueuedConnections:   100,
		ConnectionQueueTimeout: 65 * time.Second,
	}
}

// QueuedConnection represents a connection waiting for a cluster to wake.
type QueuedConnection struct {
	Conn       net.Conn
	ClusterID  string
	EnqueuedAt time.Time
	Done       chan error
}

// Handler handles wake-on-connect functionality.
type Handler struct {
	config     *Config
	httpClient *http.Client
	logger     *slog.Logger

	// Per-cluster state for managing concurrent wake requests
	clusterStates sync.Map // map[string]*clusterWakeState

	// Connection queue
	queueMu     sync.Mutex
	connections map[string][]*QueuedConnection // clusterID -> queued connections
}

// clusterWakeState tracks the wake state for a single cluster.
type clusterWakeState struct {
	mu          sync.Mutex
	waking      bool
	wakeStarted time.Time
	waiters     []chan error
}

// NewHandler creates a new wake handler.
func NewHandler(config *Config, logger *slog.Logger) *Handler {
	if config == nil {
		config = DefaultConfig()
	}

	return &Handler{
		config: config,
		httpClient: &http.Client{
			Timeout: 10 * time.Second,
		},
		logger:      logger,
		connections: make(map[string][]*QueuedConnection),
	}
}

// CheckClusterState checks if a cluster is active or suspended.
func (h *Handler) CheckClusterState(ctx context.Context, clusterID string) (*ClusterStateResponse, error) {
	url := fmt.Sprintf("%s/api/v1/internal/clusters/%s/state", h.config.ControlPlaneURL, clusterID)

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	if h.config.ServiceAccountToken != "" {
		req.Header.Set("Authorization", "Bearer "+h.config.ServiceAccountToken)
	}

	resp, err := h.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("failed to check cluster state: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return nil, ErrClusterNotFound
	}

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("unexpected status %d: %s", resp.StatusCode, string(body))
	}

	var result struct {
		State *ClusterStateResponse `json:"state"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("failed to decode response: %w", err)
	}

	return result.State, nil
}

// TriggerWake triggers a wake operation for a suspended cluster.
func (h *Handler) TriggerWake(ctx context.Context, clusterID string) (*ClusterStateResponse, error) {
	url := fmt.Sprintf("%s/api/v1/internal/clusters/%s/wake", h.config.ControlPlaneURL, clusterID)

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	if h.config.ServiceAccountToken != "" {
		req.Header.Set("Authorization", "Bearer "+h.config.ServiceAccountToken)
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := h.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("failed to trigger wake: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return nil, ErrClusterNotFound
	}

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusAccepted {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("wake request failed with status %d: %s", resp.StatusCode, string(body))
	}

	var result struct {
		State *ClusterStateResponse `json:"state"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("failed to decode response: %w", err)
	}

	return result.State, nil
}

// WaitForWake waits for a cluster to become ready after triggering wake.
func (h *Handler) WaitForWake(ctx context.Context, clusterID string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	ticker := time.NewTicker(h.config.PollInterval)
	defer ticker.Stop()

	h.logger.Info("waiting for cluster to wake",
		"cluster_id", clusterID,
		"timeout", timeout,
	)

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			if time.Now().After(deadline) {
				return ErrWakeTimeout
			}

			state, err := h.CheckClusterState(ctx, clusterID)
			if err != nil {
				h.logger.Warn("error checking cluster state during wake",
					"cluster_id", clusterID,
					"error", err,
				)
				continue
			}

			if state.IsReady {
				h.logger.Info("cluster is now ready",
					"cluster_id", clusterID,
					"wake_duration", time.Since(deadline.Add(-timeout)),
				)
				return nil
			}

			if state.Status != StateWaking && state.Status != StateSuspended {
				// Cluster is in an unexpected state
				if state.Status == StateActive {
					return nil // Already active
				}
				return fmt.Errorf("cluster in unexpected state: %s", state.Status)
			}
		}
	}
}

// HandleSuspendedCluster handles a connection attempt to a suspended cluster.
// It triggers a wake, queues the connection, and waits for the cluster to be ready.
func (h *Handler) HandleSuspendedCluster(ctx context.Context, clusterID string, conn net.Conn) error {
	// Check if we can queue this connection
	h.queueMu.Lock()
	queueLen := len(h.connections[clusterID])
	if queueLen >= h.config.MaxQueuedConnections {
		h.queueMu.Unlock()
		return ErrMaxQueuedConnections
	}

	// Create queued connection entry
	qc := &QueuedConnection{
		Conn:       conn,
		ClusterID:  clusterID,
		EnqueuedAt: time.Now(),
		Done:       make(chan error, 1),
	}

	h.connections[clusterID] = append(h.connections[clusterID], qc)
	h.queueMu.Unlock()

	h.logger.Debug("connection queued during wake",
		"cluster_id", clusterID,
		"queue_size", queueLen+1,
	)

	// Ensure wake is triggered (idempotent - only one wake per cluster)
	if err := h.ensureWakeTriggered(ctx, clusterID); err != nil {
		h.dequeueConnection(clusterID, qc)
		return fmt.Errorf("failed to trigger wake: %w", err)
	}

	// Wait for the cluster to be ready or timeout
	select {
	case err := <-qc.Done:
		if err != nil {
			return err
		}
		return nil
	case <-ctx.Done():
		h.dequeueConnection(clusterID, qc)
		return ctx.Err()
	case <-time.After(h.config.ConnectionQueueTimeout):
		h.dequeueConnection(clusterID, qc)
		return ErrWakeTimeout
	}
}

// ensureWakeTriggered ensures a wake is triggered for the cluster.
// Only one wake operation will be started per cluster.
func (h *Handler) ensureWakeTriggered(ctx context.Context, clusterID string) error {
	// Get or create cluster wake state
	stateI, _ := h.clusterStates.LoadOrStore(clusterID, &clusterWakeState{})
	state := stateI.(*clusterWakeState)

	state.mu.Lock()
	if state.waking {
		// Wake already in progress, add ourselves as a waiter
		waiter := make(chan error, 1)
		state.waiters = append(state.waiters, waiter)
		state.mu.Unlock()

		// Wait for the wake to complete
		select {
		case err := <-waiter:
			return err
		case <-ctx.Done():
			return ctx.Err()
		}
	}

	// Start the wake
	state.waking = true
	state.wakeStarted = time.Now()
	state.mu.Unlock()

	h.logger.Info("triggering wake for cluster",
		"cluster_id", clusterID,
	)

	// Trigger wake in background
	go func() {
		wakeCtx, cancel := context.WithTimeout(context.Background(), h.config.WakeTimeout)
		defer cancel()

		var wakeErr error

		// Trigger the wake
		_, err := h.TriggerWake(wakeCtx, clusterID)
		if err != nil {
			wakeErr = fmt.Errorf("failed to trigger wake: %w", err)
		} else {
			// Wait for cluster to be ready
			if err := h.WaitForWake(wakeCtx, clusterID, h.config.WakeTimeout); err != nil {
				wakeErr = err
			}
		}

		// Notify all waiters
		state.mu.Lock()
		for _, waiter := range state.waiters {
			select {
			case waiter <- wakeErr:
			default:
			}
		}
		state.waiters = nil
		state.waking = false
		state.mu.Unlock()

		// Signal all queued connections
		h.signalQueuedConnections(clusterID, wakeErr)

		// Clean up cluster state after a delay
		time.AfterFunc(10*time.Second, func() {
			h.clusterStates.Delete(clusterID)
		})
	}()

	return nil
}

// signalQueuedConnections signals all queued connections for a cluster.
func (h *Handler) signalQueuedConnections(clusterID string, err error) {
	h.queueMu.Lock()
	defer h.queueMu.Unlock()

	connections := h.connections[clusterID]
	for _, qc := range connections {
		select {
		case qc.Done <- err:
		default:
		}
	}
	delete(h.connections, clusterID)

	if err == nil {
		h.logger.Info("cluster wake complete, released queued connections",
			"cluster_id", clusterID,
			"connections_released", len(connections),
		)
	} else {
		h.logger.Warn("cluster wake failed, releasing queued connections with error",
			"cluster_id", clusterID,
			"connections_released", len(connections),
			"error", err,
		)
	}
}

// dequeueConnection removes a connection from the queue.
func (h *Handler) dequeueConnection(clusterID string, qc *QueuedConnection) {
	h.queueMu.Lock()
	defer h.queueMu.Unlock()

	connections := h.connections[clusterID]
	for i, c := range connections {
		if c == qc {
			h.connections[clusterID] = append(connections[:i], connections[i+1:]...)
			break
		}
	}
}

// GetQueuedConnectionCount returns the number of queued connections for a cluster.
func (h *Handler) GetQueuedConnectionCount(clusterID string) int {
	h.queueMu.Lock()
	defer h.queueMu.Unlock()
	return len(h.connections[clusterID])
}

// GetTotalQueuedConnections returns the total number of queued connections.
func (h *Handler) GetTotalQueuedConnections() int {
	h.queueMu.Lock()
	defer h.queueMu.Unlock()

	total := 0
	for _, conns := range h.connections {
		total += len(conns)
	}
	return total
}

// IsClusterWaking checks if a wake operation is in progress for a cluster.
func (h *Handler) IsClusterWaking(clusterID string) bool {
	stateI, ok := h.clusterStates.Load(clusterID)
	if !ok {
		return false
	}

	state := stateI.(*clusterWakeState)
	state.mu.Lock()
	defer state.mu.Unlock()
	return state.waking
}

// Stats returns current wake handler statistics.
type Stats struct {
	TotalQueuedConnections int            `json:"total_queued_connections"`
	ClustersWaking         int            `json:"clusters_waking"`
	QueuedByCluster        map[string]int `json:"queued_by_cluster"`
}

// GetStats returns current statistics.
func (h *Handler) GetStats() Stats {
	h.queueMu.Lock()
	defer h.queueMu.Unlock()

	stats := Stats{
		QueuedByCluster: make(map[string]int),
	}

	for clusterID, conns := range h.connections {
		stats.TotalQueuedConnections += len(conns)
		stats.QueuedByCluster[clusterID] = len(conns)
	}

	h.clusterStates.Range(func(key, value interface{}) bool {
		state := value.(*clusterWakeState)
		state.mu.Lock()
		if state.waking {
			stats.ClustersWaking++
		}
		state.mu.Unlock()
		return true
	})

	return stats
}
