package api

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// TestContextCancellation verifies that API calls respect context cancellation.
func TestContextCancellation(t *testing.T) {
	// Create a test server that delays the response
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Simulate a slow response
		time.Sleep(2 * time.Second)
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"clusters":[]}`))
	}))
	defer server.Close()

	// Create client pointing to test server
	client := &Client{
		baseURL: server.URL,
		httpClient: &http.Client{
			Timeout:   30 * time.Second,
			Transport: newHTTPTransport(),
		},
		token: "test-token",
	}

	// Create a context that will be cancelled immediately
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // Cancel immediately

	// Try to list clusters with cancelled context
	_, err := client.ListClusters(ctx)

	// Should get a context cancellation error
	if err == nil {
		t.Fatal("Expected error due to cancelled context, got nil")
	}

	if ctx.Err() != context.Canceled {
		t.Errorf("Expected context.Canceled error, got: %v", ctx.Err())
	}
}

// TestContextTimeout verifies that API calls respect context timeouts.
func TestContextTimeout(t *testing.T) {
	// Create a test server that delays the response
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Simulate a slow response (2 seconds)
		time.Sleep(2 * time.Second)
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"clusters":[]}`))
	}))
	defer server.Close()

	// Create client pointing to test server
	client := &Client{
		baseURL: server.URL,
		httpClient: &http.Client{
			Timeout:   30 * time.Second,
			Transport: newHTTPTransport(),
		},
		token: "test-token",
	}

	// Create a context with a short timeout (100ms)
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()

	// Try to list clusters - should timeout
	_, err := client.ListClusters(ctx)

	// Should get a timeout error
	if err == nil {
		t.Fatal("Expected timeout error, got nil")
	}
}

// TestContextPropagationAllMethods verifies all API methods accept and use context.
func TestContextPropagationAllMethods(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Check if Authorization header is present
		if r.Header.Get("Authorization") == "" {
			w.WriteHeader(http.StatusUnauthorized)
			w.Write([]byte(`{"code":"unauthorized","message":"missing token"}`))
			return
		}

		// Return appropriate responses based on method and path
		switch r.Method + " " + r.URL.Path {
		case "POST /v1/auth/login":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"access_token":"token","refresh_token":"refresh","user":{"id":"1","email":"test@example.com","name":"Test"}}`))
		case "POST /v1/auth/logout":
			w.WriteHeader(http.StatusOK)
		case "GET /v1/users/me":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"id":"1","email":"test@example.com","name":"Test"}`))
		case "GET /v1/clusters":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"clusters":[]}`))
		case "GET /v1/clusters/test-cluster":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"id":"1","name":"test-cluster","size":"small","region":"us-east-1","replicas":1,"status":"running"}`))
		case "POST /v1/clusters":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"id":"1","name":"test-cluster","size":"small","region":"us-east-1","replicas":1,"status":"creating"}`))
		case "DELETE /v1/clusters/test-cluster":
			w.WriteHeader(http.StatusOK)
		case "PATCH /v1/clusters/test-cluster/scale":
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"id":"1","name":"test-cluster","size":"small","region":"us-east-1","replicas":3,"status":"scaling"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()

	client := &Client{
		baseURL: server.URL,
		httpClient: &http.Client{
			Timeout:   30 * time.Second,
			Transport: newHTTPTransport(),
		},
		token: "test-token",
	}

	ctx := context.Background()

	// Test Login
	t.Run("Login", func(t *testing.T) {
		_, err := client.Login(ctx, "test@example.com", "password")
		if err != nil {
			t.Errorf("Login failed: %v", err)
		}
	})

	// Test Logout
	t.Run("Logout", func(t *testing.T) {
		err := client.Logout(ctx)
		if err != nil {
			t.Errorf("Logout failed: %v", err)
		}
	})

	// Test GetCurrentUser
	t.Run("GetCurrentUser", func(t *testing.T) {
		_, err := client.GetCurrentUser(ctx)
		if err != nil {
			t.Errorf("GetCurrentUser failed: %v", err)
		}
	})

	// Test ListClusters
	t.Run("ListClusters", func(t *testing.T) {
		_, err := client.ListClusters(ctx)
		if err != nil {
			t.Errorf("ListClusters failed: %v", err)
		}
	})

	// Test GetCluster
	t.Run("GetCluster", func(t *testing.T) {
		_, err := client.GetCluster(ctx, "test-cluster")
		if err != nil {
			t.Errorf("GetCluster failed: %v", err)
		}
	})

	// Test CreateCluster
	t.Run("CreateCluster", func(t *testing.T) {
		_, err := client.CreateCluster(ctx, &CreateClusterRequest{
			Name:     "test-cluster",
			Size:     ClusterSizeSmall,
			Region:   "us-east-1",
			Replicas: 1,
		})
		if err != nil {
			t.Errorf("CreateCluster failed: %v", err)
		}
	})

	// Test DeleteCluster
	t.Run("DeleteCluster", func(t *testing.T) {
		err := client.DeleteCluster(ctx, "test-cluster")
		if err != nil {
			t.Errorf("DeleteCluster failed: %v", err)
		}
	})

	// Test ScaleCluster
	t.Run("ScaleCluster", func(t *testing.T) {
		_, err := client.ScaleCluster(ctx, "test-cluster", 3)
		if err != nil {
			t.Errorf("ScaleCluster failed: %v", err)
		}
	})

	// Test GetConnectionString
	t.Run("GetConnectionString", func(t *testing.T) {
		_, err := client.GetConnectionString(ctx, "test-cluster")
		if err != nil {
			t.Errorf("GetConnectionString failed: %v", err)
		}
	})
}
