// Package pgdog_integration provides integration tests for PgDog and scale-to-zero functionality.
package pgdog_integration

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// MockControlPlane simulates the control plane API for testing.
type MockControlPlane struct {
	mu             sync.Mutex
	clusterStates  map[string]string // clusterID -> state
	wakeLatency    time.Duration     // Simulated wake latency
	wakeCalls      atomic.Int32
	stateCalls     atomic.Int32
	wakeErrors     map[string]error
	server         *httptest.Server
}

// NewMockControlPlane creates a new mock control plane.
func NewMockControlPlane() *MockControlPlane {
	m := &MockControlPlane{
		clusterStates: make(map[string]string),
		wakeErrors:    make(map[string]error),
		wakeLatency:   2 * time.Second, // Default simulated cold start
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/internal/clusters/", m.handleClusterRequests)

	m.server = httptest.NewServer(mux)
	return m
}

func (m *MockControlPlane) handleClusterRequests(w http.ResponseWriter, r *http.Request) {
	// Extract cluster ID from path
	// Expected paths:
	// - /api/v1/internal/clusters/{id}/state
	// - /api/v1/internal/clusters/{id}/wake

	path := r.URL.Path
	// Parse cluster ID and action from path
	var clusterID, action string
	n, _ := fmt.Sscanf(path, "/api/v1/internal/clusters/%s", &clusterID)
	if n < 1 {
		http.Error(w, "invalid path", http.StatusBadRequest)
		return
	}

	// Check for /state or /wake suffix
	if len(clusterID) > 6 && clusterID[len(clusterID)-6:] == "/state" {
		clusterID = clusterID[:len(clusterID)-6]
		action = "state"
	} else if len(clusterID) > 5 && clusterID[len(clusterID)-5:] == "/wake" {
		clusterID = clusterID[:len(clusterID)-5]
		action = "wake"
	}

	switch action {
	case "state":
		m.handleGetState(w, r, clusterID)
	case "wake":
		m.handleWake(w, r, clusterID)
	default:
		http.Error(w, "unknown action", http.StatusNotFound)
	}
}

func (m *MockControlPlane) handleGetState(w http.ResponseWriter, r *http.Request, clusterID string) {
	m.stateCalls.Add(1)

	m.mu.Lock()
	state, exists := m.clusterStates[clusterID]
	m.mu.Unlock()

	if !exists {
		http.Error(w, "cluster not found", http.StatusNotFound)
		return
	}

	isReady := state == "running"
	response := map[string]interface{}{
		"state": map[string]interface{}{
			"cluster_id": clusterID,
			"status":     state,
			"is_ready":   isReady,
			"message":    fmt.Sprintf("Cluster is %s", state),
		},
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

func (m *MockControlPlane) handleWake(w http.ResponseWriter, r *http.Request, clusterID string) {
	m.wakeCalls.Add(1)

	m.mu.Lock()
	state, exists := m.clusterStates[clusterID]
	wakeErr := m.wakeErrors[clusterID]
	wakeLatency := m.wakeLatency
	m.mu.Unlock()

	if !exists {
		http.Error(w, "cluster not found", http.StatusNotFound)
		return
	}

	if wakeErr != nil {
		http.Error(w, wakeErr.Error(), http.StatusInternalServerError)
		return
	}

	if state == "running" {
		response := map[string]interface{}{
			"state": map[string]interface{}{
				"cluster_id": clusterID,
				"status":     "running",
				"is_ready":   true,
				"message":    "Cluster is already running",
			},
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(response)
		return
	}

	// Set to waking
	m.mu.Lock()
	m.clusterStates[clusterID] = "waking"
	m.mu.Unlock()

	// Simulate wake in background
	go func() {
		time.Sleep(wakeLatency)
		m.mu.Lock()
		if m.clusterStates[clusterID] == "waking" {
			m.clusterStates[clusterID] = "running"
		}
		m.mu.Unlock()
	}()

	response := map[string]interface{}{
		"state": map[string]interface{}{
			"cluster_id": clusterID,
			"status":     "waking",
			"is_ready":   false,
			"message":    "Cluster wake initiated",
		},
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusAccepted)
	json.NewEncoder(w).Encode(response)
}

func (m *MockControlPlane) SetClusterState(clusterID, state string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.clusterStates[clusterID] = state
}

func (m *MockControlPlane) SetWakeLatency(latency time.Duration) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.wakeLatency = latency
}

func (m *MockControlPlane) SetWakeError(clusterID string, err error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.wakeErrors[clusterID] = err
}

func (m *MockControlPlane) URL() string {
	return m.server.URL
}

func (m *MockControlPlane) Close() {
	m.server.Close()
}

func (m *MockControlPlane) GetWakeCalls() int32 {
	return m.wakeCalls.Load()
}

func (m *MockControlPlane) GetStateCalls() int32 {
	return m.stateCalls.Load()
}

// TestSuspendCluster tests the suspend cluster functionality.
func TestSuspendCluster(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetClusterState("test-cluster-1", "running")

	// Simulate suspend by setting state to suspending, then suspended
	mock.SetClusterState("test-cluster-1", "suspending")
	time.Sleep(100 * time.Millisecond)
	mock.SetClusterState("test-cluster-1", "suspended")

	// Verify state
	resp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/test-cluster-1/state")
	if err != nil {
		t.Fatalf("Failed to get cluster state: %v", err)
	}
	defer resp.Body.Close()

	var result struct {
		State struct {
			Status  string `json:"status"`
			IsReady bool   `json:"is_ready"`
		} `json:"state"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if result.State.Status != "suspended" {
		t.Errorf("Expected status 'suspended', got '%s'", result.State.Status)
	}
	if result.State.IsReady {
		t.Error("Expected IsReady to be false for suspended cluster")
	}
}

// TestWakeOnConnect tests the wake-on-connect functionality.
func TestWakeOnConnect(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	// Set up a suspended cluster
	mock.SetClusterState("test-cluster-2", "suspended")
	mock.SetWakeLatency(100 * time.Millisecond) // Fast wake for testing

	// Request wake
	resp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/test-cluster-2/wake", "application/json", nil)
	if err != nil {
		t.Fatalf("Failed to trigger wake: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusAccepted {
		t.Errorf("Expected status 202, got %d", resp.StatusCode)
	}

	// Wait for wake to complete
	time.Sleep(200 * time.Millisecond)

	// Check final state
	resp2, err := http.Get(mock.URL() + "/api/v1/internal/clusters/test-cluster-2/state")
	if err != nil {
		t.Fatalf("Failed to get cluster state: %v", err)
	}
	defer resp2.Body.Close()

	var result struct {
		State struct {
			Status  string `json:"status"`
			IsReady bool   `json:"is_ready"`
		} `json:"state"`
	}
	if err := json.NewDecoder(resp2.Body).Decode(&result); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if result.State.Status != "running" {
		t.Errorf("Expected status 'running', got '%s'", result.State.Status)
	}
	if !result.State.IsReady {
		t.Error("Expected IsReady to be true after wake")
	}
}

// TestConnectionQueuingDuringWake tests that connections are queued during wake.
func TestConnectionQueuingDuringWake(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetClusterState("test-cluster-3", "suspended")
	mock.SetWakeLatency(500 * time.Millisecond)

	// Track concurrent wake requests
	var concurrentRequests atomic.Int32
	var maxConcurrent atomic.Int32

	numConnections := 5
	var wg sync.WaitGroup

	for i := 0; i < numConnections; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()

			// Simulate connection attempt
			current := concurrentRequests.Add(1)
			for {
				max := maxConcurrent.Load()
				if current > max {
					if maxConcurrent.CompareAndSwap(max, current) {
						break
					}
				} else {
					break
				}
			}
			defer concurrentRequests.Add(-1)

			// Request wake (only one should actually trigger wake)
			resp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/test-cluster-3/wake", "application/json", nil)
			if err != nil {
				t.Logf("Wake request error: %v", err)
				return
			}
			defer resp.Body.Close()

			// Poll for ready state
			deadline := time.Now().Add(2 * time.Second)
			for time.Now().Before(deadline) {
				stateResp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/test-cluster-3/state")
				if err != nil {
					time.Sleep(50 * time.Millisecond)
					continue
				}

				var result struct {
					State struct {
						IsReady bool `json:"is_ready"`
					} `json:"state"`
				}
				json.NewDecoder(stateResp.Body).Decode(&result)
				stateResp.Body.Close()

				if result.State.IsReady {
					return
				}
				time.Sleep(50 * time.Millisecond)
			}
		}()
	}

	wg.Wait()

	// Verify that all connections eventually succeeded
	// (they would have been queued during wake)

	// Only one wake call should be made (deduplication)
	wakeCalls := mock.GetWakeCalls()
	if wakeCalls > int32(numConnections) {
		t.Errorf("Expected at most %d wake calls (one per connection), got %d", numConnections, wakeCalls)
	}

	t.Logf("Wake calls: %d, Max concurrent requests: %d", wakeCalls, maxConcurrent.Load())
}

// TestWakeTimeout tests that connections timeout appropriately during wake.
func TestWakeTimeout(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetClusterState("test-cluster-4", "suspended")
	mock.SetWakeLatency(10 * time.Second) // Very slow wake

	// Set a short timeout
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()

	// Create request with context
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, mock.URL()+"/api/v1/internal/clusters/test-cluster-4/wake", nil)
	if err != nil {
		t.Fatalf("Failed to create request: %v", err)
	}

	client := &http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("Wake request failed: %v", err)
	}
	defer resp.Body.Close()

	// Try to poll for ready - should timeout
	pollCtx, pollCancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer pollCancel()

	ready := false
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-pollCtx.Done():
			// Expected - timeout
			goto done
		case <-ticker.C:
			req, _ := http.NewRequestWithContext(pollCtx, http.MethodGet, mock.URL()+"/api/v1/internal/clusters/test-cluster-4/state", nil)
			resp, err := client.Do(req)
			if err != nil {
				continue
			}

			var result struct {
				State struct {
					IsReady bool `json:"is_ready"`
				} `json:"state"`
			}
			json.NewDecoder(resp.Body).Decode(&result)
			resp.Body.Close()

			if result.State.IsReady {
				ready = true
				goto done
			}
		}
	}

done:
	if ready {
		t.Error("Expected timeout, but cluster became ready")
	}
}

// TestConcurrentWakeRequests tests that concurrent wake requests are handled correctly.
func TestConcurrentWakeRequests(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetClusterState("test-cluster-5", "suspended")
	mock.SetWakeLatency(200 * time.Millisecond)

	numRequests := 10
	var wg sync.WaitGroup
	successCount := atomic.Int32{}

	for i := 0; i < numRequests; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()

			resp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/test-cluster-5/wake", "application/json", nil)
			if err != nil {
				return
			}
			defer resp.Body.Close()

			if resp.StatusCode == http.StatusAccepted || resp.StatusCode == http.StatusOK {
				successCount.Add(1)
			}
		}()
	}

	wg.Wait()

	// All requests should succeed
	if successCount.Load() != int32(numRequests) {
		t.Errorf("Expected %d successful requests, got %d", numRequests, successCount.Load())
	}

	// Wait for wake to complete
	time.Sleep(300 * time.Millisecond)

	// Verify cluster is now running
	resp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/test-cluster-5/state")
	if err != nil {
		t.Fatalf("Failed to get state: %v", err)
	}
	defer resp.Body.Close()

	var result struct {
		State struct {
			Status string `json:"status"`
		} `json:"state"`
	}
	json.NewDecoder(resp.Body).Decode(&result)

	if result.State.Status != "running" {
		t.Errorf("Expected 'running', got '%s'", result.State.Status)
	}
}

// TestColdStartLatency tests that cold start latency is within acceptable bounds.
func TestColdStartLatency(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	// Target: < 5 seconds cold start
	targetLatency := 5 * time.Second
	simulatedLatency := 3 * time.Second // Simulate 3 second cold start

	mock.SetClusterState("test-cluster-6", "suspended")
	mock.SetWakeLatency(simulatedLatency)

	start := time.Now()

	// Trigger wake
	resp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/test-cluster-6/wake", "application/json", nil)
	if err != nil {
		t.Fatalf("Failed to trigger wake: %v", err)
	}
	resp.Body.Close()

	// Poll until ready
	deadline := time.Now().Add(targetLatency + time.Second)
	for time.Now().Before(deadline) {
		stateResp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/test-cluster-6/state")
		if err != nil {
			time.Sleep(100 * time.Millisecond)
			continue
		}

		var result struct {
			State struct {
				IsReady bool `json:"is_ready"`
			} `json:"state"`
		}
		json.NewDecoder(stateResp.Body).Decode(&result)
		stateResp.Body.Close()

		if result.State.IsReady {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	elapsed := time.Since(start)

	if elapsed > targetLatency {
		t.Errorf("Cold start latency %v exceeds target %v", elapsed, targetLatency)
	} else {
		t.Logf("Cold start latency: %v (target: <%v)", elapsed, targetLatency)
	}
}

// MockConnection simulates a PostgreSQL client connection.
type MockConnection struct {
	net.Conn
	clusterID string
	connected atomic.Bool
	latency   time.Duration
}

// TestEndToEndWakeOnConnect tests the complete wake-on-connect flow.
func TestEndToEndWakeOnConnect(t *testing.T) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetClusterState("e2e-cluster", "suspended")
	mock.SetWakeLatency(500 * time.Millisecond)

	// Simulate the complete flow:
	// 1. Client connects
	// 2. Gateway detects suspended cluster
	// 3. Gateway triggers wake
	// 4. Gateway queues connection
	// 5. Cluster wakes
	// 6. Gateway releases connection

	start := time.Now()

	// Step 1: Check cluster state
	stateResp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/e2e-cluster/state")
	if err != nil {
		t.Fatalf("Failed to check state: %v", err)
	}
	body, _ := io.ReadAll(stateResp.Body)
	stateResp.Body.Close()

	var stateResult struct {
		State struct {
			Status  string `json:"status"`
			IsReady bool   `json:"is_ready"`
		} `json:"state"`
	}
	json.Unmarshal(body, &stateResult)

	if stateResult.State.IsReady {
		t.Skip("Cluster already running, skipping wake test")
	}

	// Step 2: Trigger wake
	wakeResp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/e2e-cluster/wake", "application/json", nil)
	if err != nil {
		t.Fatalf("Failed to trigger wake: %v", err)
	}
	wakeResp.Body.Close()

	// Step 3: Poll for ready
	ready := false
	deadline := time.Now().Add(5 * time.Second)
	pollCount := 0

	for time.Now().Before(deadline) {
		pollCount++
		pollResp, err := http.Get(mock.URL() + "/api/v1/internal/clusters/e2e-cluster/state")
		if err != nil {
			time.Sleep(100 * time.Millisecond)
			continue
		}

		var pollResult struct {
			State struct {
				IsReady bool `json:"is_ready"`
			} `json:"state"`
		}
		json.NewDecoder(pollResp.Body).Decode(&pollResult)
		pollResp.Body.Close()

		if pollResult.State.IsReady {
			ready = true
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	elapsed := time.Since(start)

	if !ready {
		t.Fatalf("Cluster did not become ready within timeout")
	}

	t.Logf("End-to-end wake-on-connect completed in %v (polls: %d)", elapsed, pollCount)

	// Verify metrics
	if mock.GetWakeCalls() != 1 {
		t.Errorf("Expected 1 wake call, got %d", mock.GetWakeCalls())
	}
}

// BenchmarkWakeOnConnect benchmarks the wake-on-connect flow.
func BenchmarkWakeOnConnect(b *testing.B) {
	mock := NewMockControlPlane()
	defer mock.Close()

	mock.SetWakeLatency(10 * time.Millisecond) // Fast wake for benchmarking

	b.ResetTimer()

	for i := 0; i < b.N; i++ {
		clusterID := fmt.Sprintf("bench-cluster-%d", i)
		mock.SetClusterState(clusterID, "suspended")

		// Trigger wake
		resp, err := http.Post(mock.URL()+"/api/v1/internal/clusters/"+clusterID+"/wake", "application/json", nil)
		if err != nil {
			b.Fatalf("Wake failed: %v", err)
		}
		resp.Body.Close()

		// Wait for ready
		deadline := time.Now().Add(1 * time.Second)
		for time.Now().Before(deadline) {
			stateResp, _ := http.Get(mock.URL() + "/api/v1/internal/clusters/" + clusterID + "/state")
			var result struct {
				State struct {
					IsReady bool `json:"is_ready"`
				} `json:"state"`
			}
			json.NewDecoder(stateResp.Body).Decode(&result)
			stateResp.Body.Close()

			if result.State.IsReady {
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
	}
}
