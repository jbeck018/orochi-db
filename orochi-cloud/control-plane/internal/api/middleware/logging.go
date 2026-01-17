package middleware

import (
	"context"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/google/uuid"

	"github.com/orochi-db/orochi-cloud/control-plane/internal/auth"
)

// RequestIDKey is the context key for request ID.
type requestIDKey struct{}

// RequestID middleware adds a unique request ID to each request.
func RequestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestID := r.Header.Get("X-Request-ID")
		if requestID == "" {
			requestID = uuid.New().String()
		}

		w.Header().Set("X-Request-ID", requestID)

		// Use context.WithValue to properly preserve context chain
		// This ensures cancellation, deadlines, and errors propagate correctly
		ctx := context.WithValue(r.Context(), requestIDKey{}, requestID)

		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// GetRequestID retrieves the request ID from context.
func GetRequestID(r *http.Request) string {
	if id, ok := r.Context().Value(requestIDKey{}).(string); ok {
		return id
	}
	return ""
}

// responseWriter wraps http.ResponseWriter to capture status code.
type responseWriter struct {
	http.ResponseWriter
	statusCode int
	bytes      int
}

func newResponseWriter(w http.ResponseWriter) *responseWriter {
	return &responseWriter{
		ResponseWriter: w,
		statusCode:     http.StatusOK,
	}
}

func (rw *responseWriter) WriteHeader(code int) {
	rw.statusCode = code
	rw.ResponseWriter.WriteHeader(code)
}

func (rw *responseWriter) Write(b []byte) (int, error) {
	n, err := rw.ResponseWriter.Write(b)
	rw.bytes += n
	return n, err
}

// Logger creates a logging middleware.
func Logger(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			wrapped := newResponseWriter(w)

			defer func() {
				duration := time.Since(start)
				requestID := r.Header.Get("X-Request-ID")

				attrs := []any{
					"method", r.Method,
					"path", r.URL.Path,
					"status", wrapped.statusCode,
					"duration_ms", duration.Milliseconds(),
					"bytes", wrapped.bytes,
					"remote_addr", r.RemoteAddr,
					"user_agent", r.UserAgent(),
				}

				if requestID != "" {
					attrs = append(attrs, "request_id", requestID)
				}

				// Add user info if authenticated
				if claims, ok := auth.ClaimsFromContext(r.Context()); ok {
					attrs = append(attrs, "user_id", claims.UserID.String())
				}

				// Log at appropriate level based on status
				switch {
				case wrapped.statusCode >= 500:
					logger.Error("request completed", attrs...)
				case wrapped.statusCode >= 400:
					logger.Warn("request completed", attrs...)
				default:
					logger.Info("request completed", attrs...)
				}
			}()

			next.ServeHTTP(wrapped, r)
		})
	}
}

// Recoverer recovers from panics and logs them.
func Recoverer(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			defer func() {
				if rec := recover(); rec != nil {
					requestID := r.Header.Get("X-Request-ID")

					logger.Error("panic recovered",
						"error", rec,
						"method", r.Method,
						"path", r.URL.Path,
						"request_id", requestID,
					)

					w.Header().Set("Content-Type", "application/json")
					w.WriteHeader(http.StatusInternalServerError)
					w.Write([]byte(`{"code":"INTERNAL_ERROR","message":"Internal server error"}`))
				}
			}()

			next.ServeHTTP(w, r)
		})
	}
}

// RateLimiter provides basic rate limiting.
// For production, use a more sophisticated implementation with Redis.
type RateLimiter struct {
	mu       sync.Mutex
	requests map[string][]time.Time
	limit    int
	window   time.Duration
}

// NewRateLimiter creates a new rate limiter.
func NewRateLimiter(limit int, window time.Duration) *RateLimiter {
	return &RateLimiter{
		requests: make(map[string][]time.Time),
		limit:    limit,
		window:   window,
	}
}

// Limit returns middleware that rate limits requests by IP.
// Thread-safe: uses mutex to protect concurrent map access.
func (rl *RateLimiter) Limit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ip := r.RemoteAddr
		now := time.Now()
		cutoff := now.Add(-rl.window)

		rl.mu.Lock()

		// Clean old requests
		var valid []time.Time
		for _, t := range rl.requests[ip] {
			if t.After(cutoff) {
				valid = append(valid, t)
			}
		}

		// Remove empty entries to prevent unbounded memory growth
		if len(valid) == 0 {
			delete(rl.requests, ip)
		} else {
			rl.requests[ip] = valid
		}

		// Check limit
		if len(rl.requests[ip]) >= rl.limit {
			rl.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Retry-After", "60")
			w.WriteHeader(http.StatusTooManyRequests)
			w.Write([]byte(`{"code":"RATE_LIMITED","message":"Too many requests"}`))
			return
		}

		// Record request
		rl.requests[ip] = append(rl.requests[ip], now)
		rl.mu.Unlock()

		next.ServeHTTP(w, r)
	})
}
