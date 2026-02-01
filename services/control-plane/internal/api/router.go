// Package api provides the HTTP API for the control plane.
package api

import (
	"log/slog"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	chimiddleware "github.com/go-chi/chi/v5/middleware"
	"github.com/go-chi/cors"

	"github.com/orochi-db/orochi-db/services/control-plane/internal/api/handlers"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/api/middleware"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/auth"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/models"
	"github.com/orochi-db/orochi-db/services/control-plane/internal/services"
)

// RouterConfig holds the configuration for the router.
type RouterConfig struct {
	JWTManager             *auth.JWTManager
	UserService            *services.UserService
	ClusterService         *services.ClusterService
	AdminService           *services.AdminService
	OrganizationService    *services.OrganizationService
	InviteService          *services.InviteService
	DataBrowserService     *services.DataBrowserService
	ClusterSettingsService *services.ClusterSettingsService
	PoolerService          *services.PoolerService
	Logger                 *slog.Logger

	// AllowedOrigins specifies CORS allowed origins.
	// If empty, defaults to environment variable ALLOWED_ORIGINS or localhost only.
	AllowedOrigins []string

	// RequestTimeout is the maximum duration for request processing.
	// Default: 30 seconds
	RequestTimeout time.Duration

	// EnableHSTS enables HTTP Strict Transport Security header.
	// Only enable when running behind HTTPS.
	EnableHSTS bool
}

// NewRouter creates and configures the HTTP router.
func NewRouter(cfg *RouterConfig) *chi.Mux {
	r := chi.NewRouter()

	// Set default timeout if not configured
	requestTimeout := cfg.RequestTimeout
	if requestTimeout == 0 {
		requestTimeout = 30 * time.Second
	}

	// Determine allowed origins for CORS
	allowedOrigins := cfg.AllowedOrigins
	if len(allowedOrigins) == 0 {
		// Check environment variable
		if envOrigins := os.Getenv("ALLOWED_ORIGINS"); envOrigins != "" {
			allowedOrigins = strings.Split(envOrigins, ",")
			for i := range allowedOrigins {
				allowedOrigins[i] = strings.TrimSpace(allowedOrigins[i])
			}
		} else {
			// Default to localhost only for security
			allowedOrigins = []string{
				"http://localhost:3000",
				"http://localhost:5173",
				"http://127.0.0.1:3000",
				"http://127.0.0.1:5173",
			}
		}
	}

	// Global middleware - order matters!
	// 1. RealIP must be first to get correct client IP
	r.Use(chimiddleware.RealIP)

	// 2. Request ID for tracing
	r.Use(middleware.RequestID)

	// 3. Panic recovery (early to catch any middleware panics)
	r.Use(middleware.Recoverer(cfg.Logger))

	// 4. Security headers - applies to all responses
	r.Use(middleware.SecurityHeaders)

	// 5. CORS - must be before other response-writing middleware
	// Note: AllowCredentials requires specific origins, not wildcards
	r.Use(cors.Handler(cors.Options{
		AllowedOrigins:   allowedOrigins,
		AllowedMethods:   []string{"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"},
		AllowedHeaders:   []string{"Accept", "Authorization", "Content-Type", "X-Request-ID", "X-CSRF-Token"},
		ExposedHeaders:   []string{"X-Request-ID", "X-RateLimit-Limit", "X-RateLimit-Remaining", "X-RateLimit-Reset"},
		AllowCredentials: true,
		MaxAge:           300, // 5 minutes
	}))

	// 6. Request timeout - after CORS to ensure preflight responses work
	r.Use(chimiddleware.Timeout(requestTimeout))

	// 7. Logging - after timeout to capture accurate durations
	r.Use(middleware.Logger(cfg.Logger))

	// 8. Rate limiting
	rateLimiter := middleware.NewRateLimiter(100, time.Minute)
	r.Use(rateLimiter.Limit)

	// Create handlers
	authHandler := handlers.NewAuthHandler(cfg.UserService, cfg.OrganizationService, cfg.InviteService, cfg.Logger)
	clusterHandler := handlers.NewClusterHandler(cfg.ClusterService, cfg.Logger)
	metricsHandler := handlers.NewMetricsHandler(cfg.ClusterService, cfg.Logger)
	healthHandler := handlers.NewHealthHandler(cfg.Logger)
	adminHandler := handlers.NewAdminHandler(cfg.AdminService, cfg.Logger)
	organizationHandler := handlers.NewOrganizationHandler(cfg.OrganizationService, cfg.Logger)
	inviteHandler := handlers.NewInviteHandler(cfg.InviteService, cfg.OrganizationService, cfg.Logger)
	dataBrowserHandler := handlers.NewDataBrowserHandler(cfg.DataBrowserService, cfg.ClusterService, cfg.Logger)
	clusterSettingsHandler := handlers.NewClusterSettingsHandler(cfg.ClusterSettingsService, cfg.ClusterService, cfg.Logger)
	branchHandler := handlers.NewBranchHandler(cfg.ClusterService, cfg.Logger)
	poolerHandler := handlers.NewPoolerHandler(cfg.PoolerService, cfg.ClusterService, cfg.Logger)

	// Create auth middleware
	authMiddleware := middleware.NewAuthMiddleware(cfg.JWTManager, cfg.UserService)

	// Health endpoints (no auth required)
	r.Get("/health", healthHandler.Health)
	r.Get("/ready", healthHandler.Ready)

	// Custom 404 handler
	r.NotFound(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte(`{"code":"NOT_FOUND","message":"The requested resource was not found"}`))
	})

	// Custom 405 handler
	r.MethodNotAllowed(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusMethodNotAllowed)
		w.Write([]byte(`{"code":"METHOD_NOT_ALLOWED","message":"The request method is not allowed for this resource"}`))
	})

	// API v1 routes
	r.Route("/api/v1", func(r chi.Router) {
		// API info endpoint (no auth required)
		r.Get("/", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusOK)
			w.Write([]byte(`{"version":"v1","status":"available","documentation":"/api/v1/docs"}`))
		})

		// Auth routes (no auth required)
		r.Route("/auth", func(r chi.Router) {
			r.Post("/register", authHandler.Register)
			r.Post("/login", authHandler.Login)
			r.Post("/refresh", authHandler.RefreshToken)

			// Protected auth routes
			r.Group(func(r chi.Router) {
				r.Use(authMiddleware.Authenticate)
				r.Get("/me", authHandler.GetCurrentUser)
			})
		})

		// Public invite routes (view invite by token - no auth required)
		r.Get("/invites/{token}", inviteHandler.GetInviteByToken)

		// Protected routes
		r.Group(func(r chi.Router) {
			r.Use(authMiddleware.Authenticate)

			// Cluster routes
			r.Route("/clusters", func(r chi.Router) {
				r.Get("/", clusterHandler.List)
				r.Post("/", clusterHandler.Create)

				r.Route("/{id}", func(r chi.Router) {
					r.Get("/", clusterHandler.Get)
					r.Patch("/", clusterHandler.Update)
					r.Delete("/", clusterHandler.Delete)
					r.Post("/scale", clusterHandler.Scale)

					// Scale-to-zero routes
					r.Post("/suspend", clusterHandler.Suspend)
					r.Post("/wake", clusterHandler.Wake)
					r.Get("/state", clusterHandler.GetState)

					// Metrics routes
					r.Get("/metrics", metricsHandler.GetClusterMetrics)
					r.Get("/metrics/latest", metricsHandler.GetLatestMetrics)

					// Data browser routes
					r.Route("/data", func(r chi.Router) {
						r.Get("/tables", dataBrowserHandler.ListTables)
						r.Get("/tables/{schema}/{table}", dataBrowserHandler.GetTableSchema)
						r.Get("/tables/{schema}/{table}/data", dataBrowserHandler.GetTableData)
						r.Post("/query", dataBrowserHandler.ExecuteSQL)
						r.Get("/history", dataBrowserHandler.GetQueryHistory)
						r.Get("/stats", dataBrowserHandler.GetInternalStats)
					})

					// Cluster settings routes
					r.Get("/settings", clusterSettingsHandler.GetSettings)
					r.Patch("/settings", clusterSettingsHandler.UpdateSettings)
					r.Get("/recommendations", clusterSettingsHandler.GetRecommendations)
					r.Post("/recommendations/{recId}/dismiss", clusterSettingsHandler.DismissRecommendation)
					r.Post("/recommendations/{recId}/apply", clusterSettingsHandler.ApplyRecommendation)

					// Branch routes (instant database cloning)
					r.Route("/branches", func(r chi.Router) {
						r.Get("/", branchHandler.List)
						r.Post("/", branchHandler.Create)

						r.Route("/{branchId}", func(r chi.Router) {
							r.Get("/", branchHandler.Get)
							r.Delete("/", branchHandler.Delete)
							r.Post("/promote", branchHandler.Promote)
						})
					})

					// Pooler routes (PgDog connection pooler management)
					r.Route("/pooler", func(r chi.Router) {
						r.Get("/", poolerHandler.GetPoolerStatus)
						r.Patch("/", poolerHandler.UpdatePoolerConfig)
						r.Post("/reload", poolerHandler.ReloadPoolerConfig)
						r.Get("/stats", poolerHandler.GetPoolerStats)
						r.Get("/clients", poolerHandler.GetPoolerClients)
						r.Get("/pools", poolerHandler.GetPoolerPools)
					})
				})
			})

			// Organization routes
			r.Route("/organizations", func(r chi.Router) {
				r.Get("/", organizationHandler.List)
				r.Post("/", organizationHandler.Create)

				r.Route("/{id}", func(r chi.Router) {
					r.Get("/", organizationHandler.Get)
					r.Patch("/", organizationHandler.Update)
					r.Delete("/", organizationHandler.Delete)

					// Members routes
					r.Get("/members", organizationHandler.GetMembers)
					r.Post("/members", organizationHandler.AddMember)
					r.Delete("/members/{memberId}", organizationHandler.RemoveMember)

					// Invite routes for organization
					r.Get("/invites", inviteHandler.ListInvites)
					r.Post("/invites", inviteHandler.CreateInvite)
					r.Delete("/invites/{inviteId}", inviteHandler.RevokeInvite)
					r.Post("/invites/{inviteId}/resend", inviteHandler.ResendInvite)
				})
			})

			// User invite routes (accept invites, list my invites)
			r.Route("/invites", func(r chi.Router) {
				r.Get("/me", inviteHandler.ListMyInvites)
				r.Post("/{token}/accept", inviteHandler.AcceptInvite)
			})

			// User profile routes
			r.Route("/users/me", func(r chi.Router) {
				r.Post("/avatar", authHandler.UploadAvatar)
				r.Delete("/avatar", authHandler.DeleteAvatar)
			})
		})

		// Internal API routes (for service-to-service communication)
		// These routes use API key authentication instead of JWT
		r.Route("/internal", func(r chi.Router) {
			// Cluster state endpoints for JWT gateway wake-on-connect
			r.Route("/clusters/{id}", func(r chi.Router) {
				r.Get("/state", clusterHandler.GetStateInternal)
				r.Post("/wake", clusterHandler.WakeInternal)
			})
		})

		// Admin routes (requires admin role)
		r.Group(func(r chi.Router) {
			r.Use(authMiddleware.Authenticate)
			r.Use(authMiddleware.RequireRole(models.UserRoleAdmin))

			r.Route("/admin", func(r chi.Router) {
				// Stats
				r.Get("/stats", adminHandler.GetStats)

				// User management
				r.Route("/users", func(r chi.Router) {
					r.Get("/", adminHandler.ListUsers)
					r.Get("/{id}", adminHandler.GetUser)
					r.Patch("/{id}/role", adminHandler.UpdateUserRole)
					r.Patch("/{id}/active", adminHandler.SetUserActive)
				})

				// Cluster management
				r.Route("/clusters", func(r chi.Router) {
					r.Get("/", adminHandler.ListClusters)
					r.Get("/{id}", adminHandler.GetCluster)
					r.Delete("/{id}/force", adminHandler.ForceDeleteCluster)
				})
			})
		})
	})

	return r
}
