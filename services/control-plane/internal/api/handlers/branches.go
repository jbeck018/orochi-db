// Package handlers provides HTTP request handlers for the API.
package handlers

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// BranchHandler handles branch-related HTTP requests.
type BranchHandler struct {
	clusterService *services.ClusterService
	logger         *slog.Logger
}

// NewBranchHandler creates a new BranchHandler.
func NewBranchHandler(clusterService *services.ClusterService, logger *slog.Logger) *BranchHandler {
	return &BranchHandler{
		clusterService: clusterService,
		logger:         logger.With("handler", "branch"),
	}
}

// BranchCreateRequest represents a request to create a database branch.
type BranchCreateRequest struct {
	// Name is the branch name
	Name string `json:"name"`
	// Method is the branching method (volumeSnapshot, clone, pg_basebackup, pitr)
	Method string `json:"method,omitempty"`
	// PointInTime for PITR branches (RFC3339 format)
	PointInTime string `json:"pointInTime,omitempty"`
	// LSN for PITR branches
	LSN string `json:"lsn,omitempty"`
	// Instances overrides the instance count (default: 1)
	Instances int32 `json:"instances,omitempty"`
	// Inherit whether to inherit parent cluster configuration
	Inherit bool `json:"inherit,omitempty"`
}

// BranchResponse represents a branch in API responses.
type BranchResponse struct {
	ID               string            `json:"id"`
	Name             string            `json:"name"`
	ParentClusterID  string            `json:"parentClusterId"`
	ParentCluster    string            `json:"parentCluster"`
	Status           string            `json:"status"`
	Method           string            `json:"method"`
	ConnectionString string            `json:"connectionString,omitempty"`
	PoolerConnection string            `json:"poolerConnection,omitempty"`
	CreatedAt        time.Time         `json:"createdAt"`
	Labels           map[string]string `json:"labels,omitempty"`
}

// BranchListResponse represents a list of branches.
type BranchListResponse struct {
	Branches   []*BranchResponse `json:"branches"`
	TotalCount int               `json:"totalCount"`
}

// Validate validates the branch create request.
func (r *BranchCreateRequest) Validate() error {
	if r.Name == "" {
		return errors.New("branch name is required")
	}

	// Validate method if provided
	validMethods := map[string]bool{
		"":               true, // Default
		"volumeSnapshot": true,
		"clone":          true,
		"pg_basebackup":  true,
		"pitr":           true,
	}
	if !validMethods[r.Method] {
		return errors.New("invalid branching method")
	}

	// Validate PITR options
	if r.PointInTime != "" && r.LSN != "" {
		return errors.New("cannot specify both pointInTime and LSN")
	}

	// Validate time format if provided
	if r.PointInTime != "" {
		if _, err := time.Parse(time.RFC3339, r.PointInTime); err != nil {
			return errors.New("invalid time format, use RFC3339")
		}
	}

	return nil
}

// Create creates a new database branch from a cluster.
// POST /api/v1/clusters/{id}/branches
func (h *BranchHandler) Create(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID := chi.URLParam(r, "id")

	// Parse cluster ID
	clusterUUID, err := uuid.Parse(clusterID)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID format",
		})
		return
	}

	// Verify cluster ownership and get cluster info
	cluster, err := h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterUUID, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to get cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to retrieve cluster",
			})
		}
		return
	}

	// Limit request body size
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)

	// Parse request body
	var req BranchCreateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid request body",
		})
		return
	}

	// Validate request
	if err := req.Validate(); err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: err.Error(),
		})
		return
	}

	// Check cluster is running
	if cluster.Status != models.ClusterStatusRunning {
		writeJSON(w, http.StatusConflict, models.APIError{
			Code:    models.ErrCodeConflict,
			Message: "Cluster must be running to create a branch",
		})
		return
	}

	h.logger.Info("creating database branch",
		"cluster_id", clusterID,
		"cluster_name", cluster.Name,
		"branch_name", req.Name,
		"method", req.Method,
		"user_id", user.ID,
	)

	// Build branch response (in production, this would call the provisioner)
	branchID := uuid.New().String()
	response := &BranchResponse{
		ID:              branchID,
		Name:            req.Name,
		ParentClusterID: clusterID,
		ParentCluster:   cluster.Name,
		Status:          "creating",
		Method:          req.Method,
		CreatedAt:       time.Now(),
		Labels: map[string]string{
			"orochi.io/branch":         "true",
			"orochi.io/parent-cluster": cluster.Name,
		},
	}

	// Default method selection based on available features
	// Priority: volumeSnapshot (instant) > clone (PG18 reflinks) > pg_basebackup (always available)
	if response.Method == "" {
		response.Method = "volumeSnapshot" // Instant branching via CSI snapshots
	}

	writeJSON(w, http.StatusAccepted, response)
}

// List lists all branches for a cluster.
// GET /api/v1/clusters/{id}/branches
func (h *BranchHandler) List(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID := chi.URLParam(r, "id")

	// Parse cluster ID
	clusterUUID, err := uuid.Parse(clusterID)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID format",
		})
		return
	}

	// Verify cluster ownership
	cluster, err := h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterUUID, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to get cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to retrieve cluster",
			})
		}
		return
	}

	h.logger.Info("listing branches",
		"cluster_id", clusterID,
		"cluster_name", cluster.Name,
		"user_id", user.ID,
	)

	// In production, this would query the provisioner for branches
	// For now, return empty list
	response := &BranchListResponse{
		Branches:   []*BranchResponse{},
		TotalCount: 0,
	}

	writeJSON(w, http.StatusOK, response)
}

// Get retrieves a specific branch.
// GET /api/v1/clusters/{id}/branches/{branchId}
func (h *BranchHandler) Get(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID := chi.URLParam(r, "id")
	branchID := chi.URLParam(r, "branchId")

	// Parse cluster ID
	clusterUUID, err := uuid.Parse(clusterID)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID format",
		})
		return
	}

	// Verify cluster ownership
	_, err = h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterUUID, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to get cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to retrieve cluster",
			})
		}
		return
	}

	h.logger.Info("getting branch",
		"cluster_id", clusterID,
		"branch_id", branchID,
		"user_id", user.ID,
	)

	// In production, this would query the provisioner for branch status
	writeJSON(w, http.StatusNotFound, models.APIError{
		Code:    models.ErrCodeNotFound,
		Message: "Branch not found",
	})
}

// Delete deletes a branch.
// DELETE /api/v1/clusters/{id}/branches/{branchId}
func (h *BranchHandler) Delete(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID := chi.URLParam(r, "id")
	branchID := chi.URLParam(r, "branchId")

	// Parse cluster ID
	clusterUUID, err := uuid.Parse(clusterID)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID format",
		})
		return
	}

	// Verify cluster ownership
	_, err = h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterUUID, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to get cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to retrieve cluster",
			})
		}
		return
	}

	h.logger.Info("deleting branch",
		"cluster_id", clusterID,
		"branch_id", branchID,
		"user_id", user.ID,
	)

	// In production, this would call the provisioner to delete the branch
	writeJSON(w, http.StatusNotFound, models.APIError{
		Code:    models.ErrCodeNotFound,
		Message: "Branch not found",
	})
}

// Promote promotes a branch to become a standalone cluster.
// POST /api/v1/clusters/{id}/branches/{branchId}/promote
func (h *BranchHandler) Promote(w http.ResponseWriter, r *http.Request) {
	user, ok := auth.UserFromContext(r.Context())
	if !ok {
		writeJSON(w, http.StatusUnauthorized, models.APIError{
			Code:    models.ErrCodeUnauthorized,
			Message: "Not authenticated",
		})
		return
	}

	clusterID := chi.URLParam(r, "id")
	branchID := chi.URLParam(r, "branchId")

	// Parse cluster ID
	clusterUUID, err := uuid.Parse(clusterID)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, models.APIError{
			Code:    models.ErrCodeBadRequest,
			Message: "Invalid cluster ID format",
		})
		return
	}

	// Verify cluster ownership
	_, err = h.clusterService.GetByIDWithOwnerCheck(r.Context(), clusterUUID, user.ID)
	if err != nil {
		switch {
		case errors.Is(err, models.ErrClusterNotFound):
			writeJSON(w, http.StatusNotFound, models.APIError{
				Code:    models.ErrCodeNotFound,
				Message: "Cluster not found",
			})
		case errors.Is(err, models.ErrForbidden):
			writeJSON(w, http.StatusForbidden, models.APIError{
				Code:    models.ErrCodeForbidden,
				Message: "Access denied",
			})
		default:
			h.logger.Error("failed to get cluster", "error", err)
			writeJSON(w, http.StatusInternalServerError, models.APIError{
				Code:    models.ErrCodeInternal,
				Message: "Failed to retrieve cluster",
			})
		}
		return
	}

	h.logger.Info("promoting branch",
		"cluster_id", clusterID,
		"branch_id", branchID,
		"user_id", user.ID,
	)

	// In production, this would remove the branch labels and convert to standalone cluster
	writeJSON(w, http.StatusNotFound, models.APIError{
		Code:    models.ErrCodeNotFound,
		Message: "Branch not found",
	})
}
