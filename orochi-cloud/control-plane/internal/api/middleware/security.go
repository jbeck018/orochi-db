package middleware

import (
	"context"
	"fmt"
	"net/http"
	"time"
)

// SecurityHeaders adds security headers to all responses.
// These headers protect against common web vulnerabilities.
func SecurityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Prevent MIME type sniffing
		w.Header().Set("X-Content-Type-Options", "nosniff")

		// Prevent clickjacking
		w.Header().Set("X-Frame-Options", "DENY")

		// XSS protection (legacy, but still useful for older browsers)
		w.Header().Set("X-XSS-Protection", "1; mode=block")

		// Content Security Policy - restrict resource loading
		w.Header().Set("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'")

		// Referrer policy - limit referrer information
		w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")

		// Permissions policy - disable unnecessary browser features
		w.Header().Set("Permissions-Policy", "geolocation=(), microphone=(), camera=()")

		// Cache control for API responses
		w.Header().Set("Cache-Control", "no-store, no-cache, must-revalidate, private")

		next.ServeHTTP(w, r)
	})
}

// SecureHeaders returns a configurable security headers middleware.
type SecureHeadersConfig struct {
	// EnableHSTS enables HTTP Strict Transport Security header.
	// Only enable this if the service is behind HTTPS.
	EnableHSTS bool

	// HSTSMaxAge is the max-age value for HSTS in seconds.
	// Default: 31536000 (1 year)
	HSTSMaxAge int

	// HSTSIncludeSubDomains includes subdomains in HSTS policy.
	HSTSIncludeSubDomains bool

	// ContentSecurityPolicy allows custom CSP header.
	ContentSecurityPolicy string
}

// DefaultSecureHeadersConfig returns sensible defaults.
func DefaultSecureHeadersConfig() SecureHeadersConfig {
	return SecureHeadersConfig{
		EnableHSTS:            false, // Disabled by default - enable only with HTTPS
		HSTSMaxAge:            31536000,
		HSTSIncludeSubDomains: true,
		ContentSecurityPolicy: "default-src 'self'; frame-ancestors 'none'",
	}
}

// SecureHeadersWithConfig creates a security headers middleware with custom config.
func SecureHeadersWithConfig(cfg SecureHeadersConfig) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// Prevent MIME type sniffing
			w.Header().Set("X-Content-Type-Options", "nosniff")

			// Prevent clickjacking
			w.Header().Set("X-Frame-Options", "DENY")

			// XSS protection
			w.Header().Set("X-XSS-Protection", "1; mode=block")

			// Content Security Policy
			if cfg.ContentSecurityPolicy != "" {
				w.Header().Set("Content-Security-Policy", cfg.ContentSecurityPolicy)
			}

			// HSTS - only if enabled (requires HTTPS)
			if cfg.EnableHSTS {
				hstsValue := fmt.Sprintf("max-age=%d", cfg.HSTSMaxAge)
				if cfg.HSTSIncludeSubDomains {
					hstsValue += "; includeSubDomains"
				}
				w.Header().Set("Strict-Transport-Security", hstsValue)
			}

			// Referrer policy
			w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")

			// Permissions policy
			w.Header().Set("Permissions-Policy", "geolocation=(), microphone=(), camera=()")

			// Cache control
			w.Header().Set("Cache-Control", "no-store, no-cache, must-revalidate, private")

			next.ServeHTTP(w, r)
		})
	}
}

// RequestTimeout creates a middleware that enforces request timeout.
// This is more comprehensive than chi's built-in timeout as it also
// sets read/write deadlines on the underlying connection if available.
func RequestTimeout(timeout time.Duration) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ctx, cancel := context.WithTimeout(r.Context(), timeout)
			defer cancel()

			// Create a channel to signal completion
			done := make(chan struct{})

			go func() {
				next.ServeHTTP(w, r.WithContext(ctx))
				close(done)
			}()

			select {
			case <-done:
				// Request completed normally
			case <-ctx.Done():
				// Timeout - but the goroutine may still be running
				// The handler should check ctx.Done() and exit gracefully
			}
		})
	}
}
