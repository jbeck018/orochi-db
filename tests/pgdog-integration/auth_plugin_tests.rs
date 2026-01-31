// PgDog Auth Plugin Unit Tests
//
// This file provides a template for unit testing the custom JWT authentication
// plugin for PgDog integration with Orochi-DB.
//
// Run with: cargo test --all-features

#![cfg(test)]

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use jsonwebtoken::{decode, encode, Algorithm, DecodingKey, EncodingKey, Header, Validation};
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};

// ============================================================
// Types and Constants
// ============================================================

const TEST_SECRET: &str = "test-jwt-secret-32-bytes-long!!";
const CLOCK_SKEW_SECONDS: i64 = 30;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Claims {
    pub sub: String,           // Subject (user ID)
    pub role: String,          // User role
    pub exp: usize,            // Expiration time
    pub iat: usize,            // Issued at
    #[serde(skip_serializing_if = "Option::is_none")]
    pub nbf: Option<usize>,    // Not before
    #[serde(skip_serializing_if = "Option::is_none")]
    pub iss: Option<String>,   // Issuer
    #[serde(skip_serializing_if = "Option::is_none")]
    pub aud: Option<String>,   // Audience
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tenant_id: Option<String>, // Custom claim
    #[serde(skip_serializing_if = "Option::is_none")]
    pub email: Option<String>, // Custom claim
}

impl Default for Claims {
    fn default() -> Self {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs() as usize;

        Claims {
            sub: String::new(),
            role: "authenticated".to_string(),
            exp: now + 3600, // 1 hour from now
            iat: now,
            nbf: None,
            iss: None,
            aud: None,
            tenant_id: None,
            email: None,
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum AuthError {
    TokenExpired,
    TokenNotYetValid,
    InvalidSignature,
    MalformedToken,
    MissingClaim(String),
    InvalidIssuer,
    InvalidAudience,
    UnsupportedAlgorithm,
    CacheError,
}

// ============================================================
// Mock Token Cache
// ============================================================

pub struct TokenCache {
    cache: RwLock<HashMap<String, (Claims, u64)>>,
    max_size: usize,
    ttl_seconds: u64,
}

impl TokenCache {
    pub fn new(max_size: usize, ttl_seconds: u64) -> Self {
        TokenCache {
            cache: RwLock::new(HashMap::new()),
            max_size,
            ttl_seconds,
        }
    }

    pub fn get(&self, token_hash: &str) -> Option<Claims> {
        let cache = self.cache.read();
        if let Some((claims, cached_at)) = cache.get(token_hash) {
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs();

            if now - cached_at < self.ttl_seconds {
                return Some(claims.clone());
            }
        }
        None
    }

    pub fn put(&self, token_hash: String, claims: Claims) {
        let mut cache = self.cache.write();

        // Evict if at capacity (simple LRU would be better)
        if cache.len() >= self.max_size {
            if let Some(key) = cache.keys().next().cloned() {
                cache.remove(&key);
            }
        }

        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();

        cache.insert(token_hash, (claims, now));
    }

    pub fn invalidate(&self, token_hash: &str) {
        let mut cache = self.cache.write();
        cache.remove(token_hash);
    }

    pub fn clear_expired(&self) {
        let mut cache = self.cache.write();
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();

        cache.retain(|_, (_, cached_at)| now - *cached_at < self.ttl_seconds);
    }

    pub fn len(&self) -> usize {
        self.cache.read().len()
    }
}

// ============================================================
// JWT Validation Functions
// ============================================================

pub fn validate_jwt(
    token: &str,
    secret: &str,
    expected_algorithm: Algorithm,
) -> Result<Claims, AuthError> {
    let decoding_key = DecodingKey::from_secret(secret.as_bytes());
    let mut validation = Validation::new(expected_algorithm);
    validation.leeway = CLOCK_SKEW_SECONDS as u64;

    match decode::<Claims>(token, &decoding_key, &validation) {
        Ok(token_data) => Ok(token_data.claims),
        Err(e) => {
            match e.kind() {
                jsonwebtoken::errors::ErrorKind::ExpiredSignature => Err(AuthError::TokenExpired),
                jsonwebtoken::errors::ErrorKind::ImmatureSignature => Err(AuthError::TokenNotYetValid),
                jsonwebtoken::errors::ErrorKind::InvalidSignature => Err(AuthError::InvalidSignature),
                jsonwebtoken::errors::ErrorKind::InvalidAlgorithm => Err(AuthError::UnsupportedAlgorithm),
                _ => Err(AuthError::MalformedToken),
            }
        }
    }
}

pub fn validate_jwt_with_options(
    token: &str,
    secret: &str,
    expected_algorithm: Algorithm,
    required_issuer: Option<&str>,
    required_audience: Option<&str>,
) -> Result<Claims, AuthError> {
    let claims = validate_jwt(token, secret, expected_algorithm)?;

    // Check issuer
    if let Some(required_iss) = required_issuer {
        match &claims.iss {
            Some(iss) if iss == required_iss => {}
            _ => return Err(AuthError::InvalidIssuer),
        }
    }

    // Check audience
    if let Some(required_aud) = required_audience {
        match &claims.aud {
            Some(aud) if aud == required_aud => {}
            _ => return Err(AuthError::InvalidAudience),
        }
    }

    Ok(claims)
}

fn create_token(claims: &Claims, secret: &str) -> String {
    encode(
        &Header::default(),
        claims,
        &EncodingKey::from_secret(secret.as_bytes()),
    )
    .unwrap()
}

fn hash_token(token: &str) -> String {
    use std::collections::hash_map::DefaultHasher;
    use std::hash::{Hash, Hasher};

    let mut hasher = DefaultHasher::new();
    token.hash(&mut hasher);
    format!("{:x}", hasher.finish())
}

// ============================================================
// JWT-001: Valid HS256 token parsing
// ============================================================

#[test]
fn test_valid_hs256_token_parsing() {
    // Arrange
    let claims = Claims {
        sub: "user-123".to_string(),
        role: "authenticated".to_string(),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert
    assert!(result.is_ok(), "Token validation should succeed");
    let parsed = result.unwrap();
    assert_eq!(parsed.sub, "user-123");
    assert_eq!(parsed.role, "authenticated");
}

// ============================================================
// JWT-004: Expired token rejection
// ============================================================

#[test]
fn test_expired_token_rejection() {
    // Arrange
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs() as usize;

    let claims = Claims {
        sub: "user-123".to_string(),
        exp: now - 3600, // Expired 1 hour ago
        iat: now - 7200,
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert
    assert!(matches!(result, Err(AuthError::TokenExpired)));
}

// ============================================================
// JWT-005: Invalid signature rejection
// ============================================================

#[test]
fn test_invalid_signature_rejection() {
    // Arrange
    let claims = Claims {
        sub: "user-123".to_string(),
        ..Default::default()
    };
    let token = create_token(&claims, "wrong-secret-key-here!!");

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert
    assert!(matches!(result, Err(AuthError::InvalidSignature)));
}

// ============================================================
// JWT-006: Malformed token rejection
// ============================================================

#[test]
fn test_malformed_token_rejection() {
    // Arrange
    let malformed_tokens = vec![
        "",
        "not.a.jwt",
        "only-one-part",
        "two.parts",
        "invalid.base64.signature",
        "eyJhbGciOiJIUzI1NiJ9.invalid-payload.signature",
    ];

    for token in malformed_tokens {
        // Act
        let result = validate_jwt(token, TEST_SECRET, Algorithm::HS256);

        // Assert
        assert!(result.is_err(), "Token '{}' should be rejected", token);
    }
}

// ============================================================
// JWT-008: Not-before (nbf) validation
// ============================================================

#[test]
fn test_nbf_validation() {
    // Arrange
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs() as usize;

    let claims = Claims {
        sub: "user-123".to_string(),
        nbf: Some(now + 3600), // Valid in 1 hour
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert
    assert!(matches!(result, Err(AuthError::TokenNotYetValid)));
}

// ============================================================
// JWT-009: Issuer (iss) validation
// ============================================================

#[test]
fn test_issuer_validation() {
    // Arrange
    let claims = Claims {
        sub: "user-123".to_string(),
        iss: Some("wrong-issuer".to_string()),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt_with_options(
        &token,
        TEST_SECRET,
        Algorithm::HS256,
        Some("expected-issuer"),
        None,
    );

    // Assert
    assert!(matches!(result, Err(AuthError::InvalidIssuer)));
}

// ============================================================
// JWT-010: Audience (aud) validation
// ============================================================

#[test]
fn test_audience_validation() {
    // Arrange
    let claims = Claims {
        sub: "user-123".to_string(),
        aud: Some("wrong-audience".to_string()),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt_with_options(
        &token,
        TEST_SECRET,
        Algorithm::HS256,
        None,
        Some("expected-audience"),
    );

    // Assert
    assert!(matches!(result, Err(AuthError::InvalidAudience)));
}

// ============================================================
// JWT-011: Algorithm substitution attack
// ============================================================

#[test]
fn test_algorithm_substitution_attack() {
    // Arrange - token signed with RS256 but we expect HS256
    let claims = Claims {
        sub: "user-123".to_string(),
        ..Default::default()
    };

    // Create token with HS256
    let token = create_token(&claims, TEST_SECRET);

    // Try to validate expecting RS256 (should fail)
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::RS256);

    // Assert
    assert!(result.is_err(), "Algorithm mismatch should be rejected");
}

// ============================================================
// JWT-012: Clock skew tolerance
// ============================================================

#[test]
fn test_clock_skew_tolerance() {
    // Arrange - token expired just 10 seconds ago (within 30s skew)
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs() as usize;

    let claims = Claims {
        sub: "user-123".to_string(),
        exp: now - 10, // Expired 10 seconds ago
        iat: now - 3600,
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert - should be accepted due to 30s leeway
    assert!(result.is_ok(), "Token within clock skew should be accepted");
}

// ============================================================
// CACHE-001: Cache hit for valid token
// ============================================================

#[test]
fn test_cache_hit_for_valid_token() {
    // Arrange
    let cache = TokenCache::new(100, 300);
    let claims = Claims {
        sub: "user-123".to_string(),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);
    let token_hash = hash_token(&token);

    // Pre-populate cache
    cache.put(token_hash.clone(), claims.clone());

    // Act
    let cached = cache.get(&token_hash);

    // Assert
    assert!(cached.is_some());
    assert_eq!(cached.unwrap().sub, "user-123");
}

// ============================================================
// CACHE-002: Cache miss for new token
// ============================================================

#[test]
fn test_cache_miss_for_new_token() {
    // Arrange
    let cache = TokenCache::new(100, 300);
    let token = "new-token";
    let token_hash = hash_token(token);

    // Act
    let cached = cache.get(&token_hash);

    // Assert
    assert!(cached.is_none());
}

// ============================================================
// CACHE-003: Cache eviction on expiry
// ============================================================

#[test]
fn test_cache_eviction_on_expiry() {
    // Arrange - cache with 1 second TTL
    let cache = TokenCache::new(100, 1);
    let claims = Claims {
        sub: "user-123".to_string(),
        ..Default::default()
    };
    let token_hash = "test-hash".to_string();

    cache.put(token_hash.clone(), claims);

    // Wait for TTL to expire
    std::thread::sleep(Duration::from_secs(2));

    // Act
    let cached = cache.get(&token_hash);

    // Assert
    assert!(cached.is_none(), "Expired entry should not be returned");
}

// ============================================================
// CACHE-004: Cache size limits
// ============================================================

#[test]
fn test_cache_size_limits() {
    // Arrange - cache with max 3 entries
    let cache = TokenCache::new(3, 300);

    // Fill cache
    for i in 0..5 {
        let claims = Claims {
            sub: format!("user-{}", i),
            ..Default::default()
        };
        cache.put(format!("hash-{}", i), claims);
    }

    // Assert - cache should not exceed max size
    assert!(cache.len() <= 3, "Cache should respect max size limit");
}

// ============================================================
// CACHE-005: Cache invalidation on revocation
// ============================================================

#[test]
fn test_cache_invalidation_on_revocation() {
    // Arrange
    let cache = TokenCache::new(100, 300);
    let claims = Claims {
        sub: "user-123".to_string(),
        ..Default::default()
    };
    let token_hash = "revoked-token-hash".to_string();

    cache.put(token_hash.clone(), claims);
    assert!(cache.get(&token_hash).is_some());

    // Act - revoke/invalidate
    cache.invalidate(&token_hash);

    // Assert
    assert!(cache.get(&token_hash).is_none(), "Revoked token should be removed");
}

// ============================================================
// CACHE-006: Thread-safe concurrent access
// ============================================================

#[test]
fn test_thread_safe_concurrent_access() {
    use std::thread;

    // Arrange
    let cache = Arc::new(TokenCache::new(1000, 300));
    let mut handles = vec![];

    // Act - spawn multiple threads doing concurrent reads/writes
    for i in 0..10 {
        let cache_clone = Arc::clone(&cache);
        handles.push(thread::spawn(move || {
            for j in 0..100 {
                let claims = Claims {
                    sub: format!("user-{}-{}", i, j),
                    ..Default::default()
                };
                let hash = format!("hash-{}-{}", i, j);
                cache_clone.put(hash.clone(), claims);
                let _ = cache_clone.get(&hash);
            }
        }));
    }

    // Wait for all threads
    for handle in handles {
        handle.join().expect("Thread panicked");
    }

    // Assert - no panics, cache is intact
    assert!(cache.len() > 0, "Cache should have entries after concurrent access");
}

// ============================================================
// SESS-005: Custom claims extraction
// ============================================================

#[test]
fn test_custom_claims_extraction() {
    // Arrange
    let claims = Claims {
        sub: "user-123".to_string(),
        tenant_id: Some("tenant-456".to_string()),
        email: Some("user@example.com".to_string()),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert
    assert!(result.is_ok());
    let parsed = result.unwrap();
    assert_eq!(parsed.tenant_id, Some("tenant-456".to_string()));
    assert_eq!(parsed.email, Some("user@example.com".to_string()));
}

// ============================================================
// SESS-008: SQL injection in claim values
// ============================================================

#[test]
fn test_sql_injection_in_claim_values() {
    // Arrange - claims with SQL injection attempt
    let claims = Claims {
        sub: "user-123'; DROP TABLE users; --".to_string(),
        tenant_id: Some("tenant' OR '1'='1".to_string()),
        ..Default::default()
    };
    let token = create_token(&claims, TEST_SECRET);

    // Act
    let result = validate_jwt(&token, TEST_SECRET, Algorithm::HS256);

    // Assert - token parsing should work, but values need escaping when used
    assert!(result.is_ok());
    let parsed = result.unwrap();

    // The raw values contain the injection attempt
    assert!(parsed.sub.contains("DROP TABLE"));

    // When setting session variables, these MUST be escaped
    // This test validates that we preserve the original value
    // Actual escaping is done in the session variable injection layer
}

// ============================================================
// Benchmark: Token validation performance
// ============================================================

#[cfg(feature = "bench")]
mod benches {
    use super::*;
    use test::Bencher;

    #[bench]
    fn bench_token_validation(b: &mut Bencher) {
        let claims = Claims {
            sub: "user-123".to_string(),
            ..Default::default()
        };
        let token = create_token(&claims, TEST_SECRET);

        b.iter(|| {
            validate_jwt(&token, TEST_SECRET, Algorithm::HS256).unwrap()
        });
    }

    #[bench]
    fn bench_cache_lookup(b: &mut Bencher) {
        let cache = TokenCache::new(1000, 300);
        let claims = Claims {
            sub: "user-123".to_string(),
            ..Default::default()
        };
        let token_hash = "bench-hash".to_string();
        cache.put(token_hash.clone(), claims);

        b.iter(|| {
            cache.get(&token_hash)
        });
    }
}
