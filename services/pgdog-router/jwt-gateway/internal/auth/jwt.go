// Package auth provides JWT authentication for the PgDog gateway.
package auth

import (
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

var (
	// ErrInvalidToken is returned when the JWT token is malformed or invalid.
	ErrInvalidToken = errors.New("invalid JWT token")

	// ErrTokenExpired is returned when the JWT token has expired.
	ErrTokenExpired = errors.New("token has expired")

	// ErrInvalidSignature is returned when the JWT signature verification fails.
	ErrInvalidSignature = errors.New("invalid token signature")

	// ErrInvalidIssuer is returned when the token issuer doesn't match expected value.
	ErrInvalidIssuer = errors.New("invalid token issuer")

	// ErrInvalidAudience is returned when the token audience doesn't match.
	ErrInvalidAudience = errors.New("invalid token audience")

	// ErrMissingClaims is returned when required claims are missing.
	ErrMissingClaims = errors.New("missing required claims")

	// ErrPublicKeyNotLoaded is returned when attempting to validate without a key.
	ErrPublicKeyNotLoaded = errors.New("public key not loaded")
)

// ValidatorConfig holds configuration for JWT validation.
type ValidatorConfig struct {
	// PublicKeyPath is the file path to the RSA public key in PEM format.
	PublicKeyPath string

	// PublicKeyURL is an HTTP(S) URL to fetch the public key from.
	PublicKeyURL string

	// Issuer is the expected "iss" claim value.
	Issuer string

	// Audience is the expected "aud" claim value.
	Audience string

	// RequireUserID requires the user_id claim to be present.
	RequireUserID bool

	// RequireTenantID requires the tenant_id claim to be present.
	RequireTenantID bool

	// CacheTTL is the duration to cache validated tokens.
	CacheTTL time.Duration

	// KeyRefreshInterval is how often to refresh the public key from URL.
	KeyRefreshInterval time.Duration
}

// Validator validates JWT tokens using RS256.
type Validator struct {
	config    ValidatorConfig
	publicKey *rsa.PublicKey
	keyMutex  sync.RWMutex

	cache      map[string]*cacheEntry
	cacheMutex sync.RWMutex

	stopChan chan struct{}
}

type cacheEntry struct {
	claims    *OrochiClaims
	expiresAt time.Time
}

// NewValidator creates a new JWT validator with the given configuration.
func NewValidator(config ValidatorConfig) (*Validator, error) {
	v := &Validator{
		config:   config,
		cache:    make(map[string]*cacheEntry),
		stopChan: make(chan struct{}),
	}

	// Load initial public key
	if err := v.loadPublicKey(); err != nil {
		return nil, fmt.Errorf("failed to load public key: %w", err)
	}

	// Start key refresh goroutine if using URL
	if config.PublicKeyURL != "" && config.KeyRefreshInterval > 0 {
		go v.keyRefreshLoop()
	}

	// Start cache cleanup goroutine
	go v.cacheCleanupLoop()

	return v, nil
}

// loadPublicKey loads the RSA public key from file or URL.
func (v *Validator) loadPublicKey() error {
	var keyData []byte
	var err error

	if v.config.PublicKeyPath != "" {
		keyData, err = os.ReadFile(v.config.PublicKeyPath)
		if err != nil {
			return fmt.Errorf("failed to read public key file: %w", err)
		}
	} else if v.config.PublicKeyURL != "" {
		keyData, err = v.fetchPublicKeyFromURL()
		if err != nil {
			return fmt.Errorf("failed to fetch public key from URL: %w", err)
		}
	} else {
		return errors.New("no public key source configured")
	}

	key, err := parseRSAPublicKey(keyData)
	if err != nil {
		return fmt.Errorf("failed to parse public key: %w", err)
	}

	v.keyMutex.Lock()
	v.publicKey = key
	v.keyMutex.Unlock()

	return nil
}

// fetchPublicKeyFromURL fetches the public key from an HTTP(S) URL.
func (v *Validator) fetchPublicKeyFromURL() ([]byte, error) {
	client := &http.Client{Timeout: 10 * time.Second}

	resp, err := client.Get(v.config.PublicKeyURL)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("unexpected status code: %d", resp.StatusCode)
	}

	return io.ReadAll(resp.Body)
}

// parseRSAPublicKey parses a PEM-encoded RSA public key.
func parseRSAPublicKey(pemData []byte) (*rsa.PublicKey, error) {
	block, _ := pem.Decode(pemData)
	if block == nil {
		return nil, errors.New("failed to decode PEM block")
	}

	// Try parsing as PKIX public key first
	pub, err := x509.ParsePKIXPublicKey(block.Bytes)
	if err == nil {
		rsaKey, ok := pub.(*rsa.PublicKey)
		if !ok {
			return nil, errors.New("not an RSA public key")
		}
		return rsaKey, nil
	}

	// Try parsing as PKCS1 public key
	rsaKey, err := x509.ParsePKCS1PublicKey(block.Bytes)
	if err != nil {
		return nil, errors.New("failed to parse RSA public key")
	}

	return rsaKey, nil
}

// keyRefreshLoop periodically refreshes the public key from the URL.
func (v *Validator) keyRefreshLoop() {
	ticker := time.NewTicker(v.config.KeyRefreshInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			if err := v.loadPublicKey(); err != nil {
				// Log error but continue with existing key
				fmt.Printf("Warning: failed to refresh public key: %v\n", err)
			}
		case <-v.stopChan:
			return
		}
	}
}

// cacheCleanupLoop periodically removes expired cache entries.
func (v *Validator) cacheCleanupLoop() {
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			v.cleanupCache()
		case <-v.stopChan:
			return
		}
	}
}

// cleanupCache removes expired entries from the cache.
func (v *Validator) cleanupCache() {
	v.cacheMutex.Lock()
	defer v.cacheMutex.Unlock()

	now := time.Now()
	for token, entry := range v.cache {
		if now.After(entry.expiresAt) {
			delete(v.cache, token)
		}
	}
}

// Validate validates a JWT token and returns the extracted claims.
func (v *Validator) Validate(tokenString string) (*OrochiClaims, error) {
	// Check cache first
	if claims := v.getFromCache(tokenString); claims != nil {
		return claims, nil
	}

	v.keyMutex.RLock()
	publicKey := v.publicKey
	v.keyMutex.RUnlock()

	if publicKey == nil {
		return nil, ErrPublicKeyNotLoaded
	}

	// Parse and validate the token
	token, err := jwt.ParseWithClaims(tokenString, &OrochiClaims{}, func(token *jwt.Token) (interface{}, error) {
		// Verify signing method
		if _, ok := token.Method.(*jwt.SigningMethodRSA); !ok {
			return nil, fmt.Errorf("unexpected signing method: %v", token.Header["alg"])
		}
		return publicKey, nil
	})

	if err != nil {
		if errors.Is(err, jwt.ErrTokenExpired) {
			return nil, ErrTokenExpired
		}
		if errors.Is(err, jwt.ErrSignatureInvalid) {
			return nil, ErrInvalidSignature
		}
		return nil, fmt.Errorf("%w: %v", ErrInvalidToken, err)
	}

	claims, ok := token.Claims.(*OrochiClaims)
	if !ok || !token.Valid {
		return nil, ErrInvalidToken
	}

	// Validate issuer
	if v.config.Issuer != "" {
		issuer, err := claims.GetIssuer()
		if err != nil || issuer != v.config.Issuer {
			return nil, ErrInvalidIssuer
		}
	}

	// Validate audience
	if v.config.Audience != "" {
		audience, err := claims.GetAudience()
		if err != nil {
			return nil, ErrInvalidAudience
		}
		found := false
		for _, aud := range audience {
			if aud == v.config.Audience {
				found = true
				break
			}
		}
		if !found {
			return nil, ErrInvalidAudience
		}
	}

	// Validate required claims
	if v.config.RequireUserID && claims.UserID == "" {
		return nil, fmt.Errorf("%w: user_id", ErrMissingClaims)
	}
	if v.config.RequireTenantID && claims.TenantID == "" {
		return nil, fmt.Errorf("%w: tenant_id", ErrMissingClaims)
	}

	// Cache the validated claims
	v.addToCache(tokenString, claims)

	return claims, nil
}

// getFromCache retrieves claims from the cache if not expired.
func (v *Validator) getFromCache(token string) *OrochiClaims {
	if v.config.CacheTTL <= 0 {
		return nil
	}

	v.cacheMutex.RLock()
	defer v.cacheMutex.RUnlock()

	entry, ok := v.cache[token]
	if !ok || time.Now().After(entry.expiresAt) {
		return nil
	}

	return entry.claims
}

// addToCache adds validated claims to the cache.
func (v *Validator) addToCache(token string, claims *OrochiClaims) {
	if v.config.CacheTTL <= 0 {
		return
	}

	v.cacheMutex.Lock()
	defer v.cacheMutex.Unlock()

	// Limit cache size to prevent memory issues
	if len(v.cache) > 10000 {
		// Simple eviction: clear half the cache
		count := 0
		for k := range v.cache {
			delete(v.cache, k)
			count++
			if count >= 5000 {
				break
			}
		}
	}

	v.cache[token] = &cacheEntry{
		claims:    claims,
		expiresAt: time.Now().Add(v.config.CacheTTL),
	}
}

// Close stops background goroutines and cleans up resources.
func (v *Validator) Close() {
	close(v.stopChan)
}
