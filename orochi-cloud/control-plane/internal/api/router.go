// Package api provides the HTTP API for the control plane.
package api

import (
	"log/slog"
	"time"

	"github.com/go-chi/chi/v5"
	chimiddleware "github.com/go-chi/chi/v5/middleware"
	"github.com/go-chi/cors"

	"github.com/orochi-db/orochi-cloud/control-plane/internal/api/handlers"
	"github.com/orochi-db/orochi-cloud/control-plane/internal/api/middleware"
	"github.com/orochi-db/orochi-cloud/control-plane/internal/auth"
	"github.com/orochi-db/orochi-cloud/control-plane/internal/services"
)

// RouterConfig holds the configuration for the router.
type RouterConfig struct {
	JWTManager     *auth.JWTManager
	UserService    *services.UserService
	ClusterService *services.ClusterService
	Logger         *slog.Logger
}

// NewRouter creates and configures the HTTP router.
func NewRouter(cfg *RouterConfig) *chi.Mux {
	r := chi.NewRouter()

	// Global middleware
	r.Use(chimiddleware.RealIP)
	r.Use(middleware.RequestID)
	r.Use(middleware.Logger(cfg.Logger))
	r.Use(middleware.Recoverer(cfg.Logger))
	r.Use(chimiddleware.Timeout(60 * time.Second))

	// CORS configuration
	r.Use(cors.Handler(cors.Options{
		AllowedOrigins:   []string{"*"}, // Configure appropriately for production
		AllowedMethods:   []string{"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"},
		AllowedHeaders:   []string{"Accept", "Authorization", "Content-Type", "X-Request-ID"},
		ExposedHeaders:   []string{"X-Request-ID"},
		AllowCredentials: true,
		MaxAge:           300,
	}))

	// Rate limiting
	rateLimiter := middleware.NewRateLimiter(100, time.Minute)
	r.Use(rateLimiter.Limit)

	// Create handlers
	authHandler := handlers.NewAuthHandler(cfg.UserService, cfg.Logger)
	clusterHandler := handlers.NewClusterHandler(cfg.ClusterService, cfg.Logger)
	metricsHandler := handlers.NewMetricsHandler(cfg.ClusterService, cfg.Logger)
	healthHandler := handlers.NewHealthHandler(cfg.Logger)

	// Create auth middleware
	authMiddleware := middleware.NewAuthMiddleware(cfg.JWTManager, cfg.UserService)

	// Health endpoints (no auth required)
	r.Get("/health", healthHandler.Health)
	r.Get("/ready", healthHandler.Ready)

	// API v1 routes
	r.Route("/api/v1", func(r chi.Router) {
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

					// Metrics routes
					r.Get("/metrics", metricsHandler.GetClusterMetrics)
					r.Get("/metrics/latest", metricsHandler.GetLatestMetrics)
				})
			})
		})
	})

	return r
}
