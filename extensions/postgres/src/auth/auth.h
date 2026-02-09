/*-------------------------------------------------------------------------
 *
 * auth.h
 *    Orochi DB Authentication System - Core Data Structures and API
 *
 * This module provides:
 *   - JWT-based authentication (Supabase-style)
 *   - Connection pooling auth (Neon-style)
 *   - API key authentication
 *   - Session management
 *   - Shared memory token caching for sub-millisecond validation
 *   - Audit logging for compliance (SOC2, HIPAA, GDPR)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_AUTH_H
#define OROCHI_AUTH_H

#include "postgres.h"
#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

#include "jwt.h"

/* ============================================================
 * Authentication Constants
 * ============================================================ */

#define OROCHI_AUTH_MAX_CACHED_TOKENS     10000
#define OROCHI_AUTH_MAX_CACHED_SESSIONS   5000
#define OROCHI_AUTH_MAX_SIGNING_KEYS      16
#define OROCHI_AUTH_MAX_API_KEYS_PER_USER 100
#define OROCHI_AUTH_TOKEN_HASH_SIZE       64
#define OROCHI_AUTH_SESSION_ID_SIZE       32
#define OROCHI_AUTH_API_KEY_PREFIX_SIZE   8
#define OROCHI_AUTH_USER_ID_SIZE          36 /* UUID string */
#define OROCHI_AUTH_TENANT_ID_SIZE        36
#define OROCHI_AUTH_PROVIDER_NAME_SIZE    64
#define OROCHI_AUTH_ROLE_NAME_SIZE        64
#define OROCHI_AUTH_MAX_ROLES             32
#define OROCHI_AUTH_JWT_MAX_SIZE          8192
#define OROCHI_AUTH_PASSWORD_HASH_SIZE    97 /* Argon2id */
#define OROCHI_AUTH_HMAC_KEY_SIZE         64
#define OROCHI_AUTH_AES_KEY_SIZE          32
#define OROCHI_AUTH_ERROR_CODE_SIZE       32
#define OROCHI_AUTH_ERROR_MSG_SIZE        256
#define OROCHI_AUTH_JTI_SIZE              64
#define OROCHI_AUTH_AAL_SIZE              16
#define OROCHI_AUTH_IP_SIZE               64
#define OROCHI_AUTH_USER_AGENT_SIZE       512
#define OROCHI_AUTH_DEVICE_ID_SIZE        64
#define OROCHI_AUTH_GEO_COUNTRY_SIZE      4
#define OROCHI_AUTH_GEO_CITY_SIZE         128
#define OROCHI_AUTH_RATE_LIMIT_KEY_SIZE   256
#define OROCHI_AUTH_MAX_RATE_LIMITS       1024
#define OROCHI_AUTH_KEY_ID_SIZE           64
#define OROCHI_AUTH_PUBLIC_KEY_SIZE       4096
#define OROCHI_AUTH_PRIVATE_KEY_SIZE      8192

/* Default timeouts and intervals */
#define OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC  900    /* 15 minutes */
#define OROCHI_AUTH_REFRESH_TOKEN_TTL_SEC 604800 /* 7 days */
#define OROCHI_AUTH_SESSION_TTL_SEC       86400  /* 24 hours */
#define OROCHI_AUTH_API_KEY_CLEANUP_SEC   3600   /* 1 hour */
#define OROCHI_AUTH_TOKEN_CLEANUP_SEC     300    /* 5 minutes */
#define OROCHI_AUTH_KEY_ROTATION_DAYS     90
#define OROCHI_AUTH_KEY_OVERLAP_DAYS      30

/* LWLock tranche names */
#define OROCHI_AUTH_LWLOCK_TRANCHE_NAME "orochi_auth"
#define OROCHI_AUTH_LWLOCK_COUNT        5

/* ============================================================
 * Authentication Method Types
 * ============================================================ */

typedef enum OrochiAuthMethod {
    OROCHI_AUTH_NONE = 0,
    OROCHI_AUTH_PASSWORD,         /* Email/password */
    OROCHI_AUTH_API_KEY,          /* Service API key */
    OROCHI_AUTH_JWT,              /* JWT bearer token */
    OROCHI_AUTH_OAUTH,            /* OAuth 2.0 provider */
    OROCHI_AUTH_MAGIC_LINK,       /* Passwordless email */
    OROCHI_AUTH_PHONE_OTP,        /* SMS OTP */
    OROCHI_AUTH_SAML,             /* Enterprise SAML */
    OROCHI_AUTH_CERTIFICATE,      /* Client certificate */
    OROCHI_AUTH_CONNECTION_STRING /* Pooler connection string */
} OrochiAuthMethod;

/* ============================================================
 * Token Types
 * ============================================================ */

typedef enum OrochiTokenType {
    OROCHI_TOKEN_ACCESS = 0,     /* Short-lived access token */
    OROCHI_TOKEN_REFRESH,        /* Long-lived refresh token */
    OROCHI_TOKEN_API_KEY,        /* API key (no expiry) */
    OROCHI_TOKEN_CONNECTION,     /* Connection pooler token */
    OROCHI_TOKEN_INVITATION,     /* User invitation token */
    OROCHI_TOKEN_PASSWORD_RESET, /* Password reset token */
    OROCHI_TOKEN_EMAIL_VERIFY    /* Email verification token */
} OrochiTokenType;

/* ============================================================
 * Identity Provider Types
 * ============================================================ */

typedef enum OrochiProviderType {
    OROCHI_PROVIDER_EMAIL = 0, /* Email/password */
    OROCHI_PROVIDER_PHONE,     /* Phone/SMS */
    OROCHI_PROVIDER_GOOGLE,    /* Google OAuth */
    OROCHI_PROVIDER_GITHUB,    /* GitHub OAuth */
    OROCHI_PROVIDER_GITLAB,    /* GitLab OAuth */
    OROCHI_PROVIDER_AZURE,     /* Azure AD */
    OROCHI_PROVIDER_OKTA,      /* Okta */
    OROCHI_PROVIDER_SAML,      /* Generic SAML */
    OROCHI_PROVIDER_OIDC,      /* Generic OIDC */
    OROCHI_PROVIDER_CUSTOM     /* Custom provider */
} OrochiProviderType;

/* ============================================================
 * Session State
 * ============================================================ */

typedef enum OrochiSessionState {
    OROCHI_SESSION_ACTIVE = 0,
    OROCHI_SESSION_IDLE,
    OROCHI_SESSION_EXPIRED,
    OROCHI_SESSION_REVOKED,
    OROCHI_SESSION_MFA_REQUIRED
} OrochiSessionState;

/* ============================================================
 * MFA Factor Types
 * ============================================================ */

typedef enum OrochiMfaType {
    OROCHI_MFA_NONE = 0,
    OROCHI_MFA_TOTP,         /* Time-based OTP (Authenticator apps) */
    OROCHI_MFA_SMS,          /* SMS OTP */
    OROCHI_MFA_EMAIL,        /* Email OTP */
    OROCHI_MFA_WEBAUTHN,     /* WebAuthn/FIDO2 */
    OROCHI_MFA_RECOVERY_CODE /* Recovery codes */
} OrochiMfaType;

/* ============================================================
 * Audit Event Types
 * ============================================================ */

typedef enum OrochiAuditEvent {
    OROCHI_AUDIT_LOGIN_SUCCESS = 0,
    OROCHI_AUDIT_LOGIN_FAILED,
    OROCHI_AUDIT_LOGOUT,
    OROCHI_AUDIT_TOKEN_ISSUED,
    OROCHI_AUDIT_TOKEN_REVOKED,
    OROCHI_AUDIT_TOKEN_REFRESHED,
    OROCHI_AUDIT_PASSWORD_CHANGED,
    OROCHI_AUDIT_PASSWORD_RESET,
    OROCHI_AUDIT_MFA_ENROLLED,
    OROCHI_AUDIT_MFA_VERIFIED,
    OROCHI_AUDIT_MFA_FAILED,
    OROCHI_AUDIT_API_KEY_CREATED,
    OROCHI_AUDIT_API_KEY_REVOKED,
    OROCHI_AUDIT_USER_CREATED,
    OROCHI_AUDIT_USER_UPDATED,
    OROCHI_AUDIT_USER_DELETED,
    OROCHI_AUDIT_ROLE_ASSIGNED,
    OROCHI_AUDIT_ROLE_REVOKED,
    OROCHI_AUDIT_PERMISSION_DENIED,
    OROCHI_AUDIT_RATE_LIMITED,
    OROCHI_AUDIT_SUSPICIOUS_ACTIVITY,
    OROCHI_AUDIT_KEY_ROTATED,
    OROCHI_AUDIT_SESSION_CREATED,
    OROCHI_AUDIT_SESSION_EXPIRED
} OrochiAuditEvent;

/* ============================================================
 * JWT Algorithm Types
 *
 * The canonical enum is defined in jwt.h (included above).
 * These aliases map the OROCHI_JWT_ALG_* names used in auth
 * code to the jwt.h enum values.
 * ============================================================ */

#define OROCHI_JWT_ALG_HS256 OROCHI_JWT_HS256
#define OROCHI_JWT_ALG_HS384 OROCHI_JWT_HS384
#define OROCHI_JWT_ALG_HS512 OROCHI_JWT_HS512
#define OROCHI_JWT_ALG_RS256 OROCHI_JWT_RS256
#define OROCHI_JWT_ALG_RS384 OROCHI_JWT_RS384
#define OROCHI_JWT_ALG_RS512 OROCHI_JWT_RS512
#define OROCHI_JWT_ALG_ES256 OROCHI_JWT_ES256
#define OROCHI_JWT_ALG_ES384 OROCHI_JWT_ES384
#define OROCHI_JWT_ALG_ES512 OROCHI_JWT_ES512

/* ============================================================
 * Core Authentication Context Structure
 * ============================================================ */

typedef struct OrochiAuthContext {
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    char role[OROCHI_AUTH_ROLE_NAME_SIZE + 1];
    char session_token[OROCHI_AUTH_JWT_MAX_SIZE];
    TimestampTz expires_at;
    int64 permissions; /* Bitmask of permissions */
    OrochiAuthMethod auth_method;
    bool is_authenticated;
    bool mfa_verified;
    char aal[OROCHI_AUTH_AAL_SIZE + 1]; /* Authenticator Assurance Level */
} OrochiAuthContext;

/* ============================================================
 * Session Structure
 * ============================================================ */

typedef struct OrochiSession {
    char session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    OrochiSessionState state;
    OrochiAuthMethod auth_method;
    char ip_address[OROCHI_AUTH_IP_SIZE + 1];
    char user_agent[OROCHI_AUTH_USER_AGENT_SIZE + 1];
    char device_id[OROCHI_AUTH_DEVICE_ID_SIZE + 1];
    char geo_country[OROCHI_AUTH_GEO_COUNTRY_SIZE + 1];
    char geo_city[OROCHI_AUTH_GEO_CITY_SIZE + 1];
    TimestampTz created_at;
    TimestampTz last_activity;
    TimestampTz expires_at;
    int refresh_count;
    bool mfa_verified;
    char aal[OROCHI_AUTH_AAL_SIZE + 1];
    uint32 hash_slot; /* Cache hash slot for quick lookup */
} OrochiSession;

/* ============================================================
 * Token Structure (Cached in Shared Memory)
 * ============================================================ */

typedef struct OrochiToken {
    char token_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    OrochiTokenType token_type;
    char session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    TimestampTz issued_at;
    TimestampTz expires_at;
    TimestampTz not_before;
    int64 scopes; /* Bitmask */
    bool is_revoked;
    char jti[OROCHI_AUTH_JTI_SIZE + 1]; /* JWT ID for revocation */
    uint32 hash_slot;                   /* Cache hash slot for quick lookup */
    TimestampTz cached_at;              /* When added to cache */
} OrochiToken;

/* ============================================================
 * Signing Key Structure
 * ============================================================ */

typedef struct OrochiSigningKey {
    char key_id[OROCHI_AUTH_KEY_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    OrochiJwtAlgorithm algorithm;
    char public_key[OROCHI_AUTH_PUBLIC_KEY_SIZE + 1];
    char private_key_encrypted[OROCHI_AUTH_PRIVATE_KEY_SIZE + 1];
    TimestampTz created_at;
    TimestampTz expires_at;
    TimestampTz rotated_at;
    bool is_current;
    bool is_active;
    int version;
} OrochiSigningKey;

/* ============================================================
 * Rate Limit Entry
 * ============================================================ */

typedef struct OrochiRateLimit {
    char key[OROCHI_AUTH_RATE_LIMIT_KEY_SIZE + 1]; /* user_id:action or ip:action */
    int count;
    int limit;
    TimestampTz window_start;
    int window_seconds;
    bool is_blocked;
    TimestampTz blocked_until;
} OrochiRateLimit;

/* ============================================================
 * Audit Log Entry
 * ============================================================ */

typedef struct OrochiAuditEntry {
    int64 audit_id;
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    OrochiAuditEvent event_type;
    char ip_address[OROCHI_AUTH_IP_SIZE + 1];
    char user_agent[OROCHI_AUTH_USER_AGENT_SIZE + 1];
    char resource_type[64];
    char resource_id[256];
    TimestampTz created_at;
    bool is_sensitive; /* Mask in logs */
} OrochiAuditEntry;

/* ============================================================
 * Token Validation Result
 * ============================================================ */

typedef struct OrochiTokenValidation {
    bool is_valid;
    char error_code[OROCHI_AUTH_ERROR_CODE_SIZE + 1];
    char error_message[OROCHI_AUTH_ERROR_MSG_SIZE + 1];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char session_id[OROCHI_AUTH_SESSION_ID_SIZE + 1];
    OrochiTokenType token_type;
    int64 scopes;
    TimestampTz issued_at;
    TimestampTz expires_at;
    bool is_expired;
    bool is_revoked;
    char roles[OROCHI_AUTH_MAX_ROLES][OROCHI_AUTH_ROLE_NAME_SIZE + 1];
    int role_count;
} OrochiTokenValidation;

/* ============================================================
 * Authentication Result
 * ============================================================ */

typedef struct OrochiAuthResult {
    bool success;
    char error_code[OROCHI_AUTH_ERROR_CODE_SIZE + 1];
    char error_message[OROCHI_AUTH_ERROR_MSG_SIZE + 1];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    OrochiSession *session;
    char access_token[OROCHI_AUTH_JWT_MAX_SIZE];
    char refresh_token[256];
    int access_token_expires_in;
    int refresh_token_expires_in;
    bool mfa_required;
    OrochiMfaType mfa_type;
} OrochiAuthResult;

/* ============================================================
 * Shared Memory State for Auth Cache
 * ============================================================ */

typedef struct OrochiAuthCache {
    /* Lock tranches */
    LWLock *main_lock;
    LWLock *token_cache_lock;
    LWLock *session_cache_lock;
    LWLock *rate_limit_lock;
    LWLock *signing_key_lock;

    /* Token cache (FIFO eviction) */
    int token_cache_count;
    int token_cache_head; /* Oldest entry index */
    int token_cache_tail; /* Newest entry index */
    OrochiToken token_cache[OROCHI_AUTH_MAX_CACHED_TOKENS];

    /* Session cache */
    int session_cache_count;
    int session_cache_head;
    int session_cache_tail;
    OrochiSession session_cache[OROCHI_AUTH_MAX_CACHED_SESSIONS];

    /* Signing keys (rotated periodically) */
    int signing_key_count;
    OrochiSigningKey signing_keys[OROCHI_AUTH_MAX_SIGNING_KEYS];

    /* Rate limiting state */
    int rate_limit_count;
    OrochiRateLimit rate_limits[OROCHI_AUTH_MAX_RATE_LIMITS];

    /* Statistics */
    pg_atomic_uint64 total_auth_requests;
    pg_atomic_uint64 cache_hits;
    pg_atomic_uint64 cache_misses;
    pg_atomic_uint64 failed_authentications;
    pg_atomic_uint64 tokens_issued;
    pg_atomic_uint64 tokens_revoked;
    pg_atomic_uint64 sessions_created;
    pg_atomic_uint64 sessions_expired;

    /* Timestamps */
    TimestampTz last_key_rotation;
    TimestampTz last_cleanup;
    TimestampTz startup_time;

    bool is_initialized;
} OrochiAuthCache;

/* ============================================================
 * Global State
 * ============================================================ */

/* Global pointer to auth shared state */
extern OrochiAuthCache *OrochiAuthSharedState;

/* GUC variables */
extern bool orochi_auth_enabled;
extern char *orochi_auth_jwt_secret;
extern int orochi_auth_token_expiry;
extern int orochi_auth_max_sessions;
extern int orochi_auth_access_token_ttl;
extern int orochi_auth_refresh_token_ttl;
extern int orochi_auth_session_ttl;
extern int orochi_auth_max_sessions_per_user;
extern bool orochi_auth_mfa_required;
extern bool orochi_auth_rate_limit_enabled;
extern int orochi_auth_rate_limit_login;
extern int orochi_auth_rate_limit_api;
extern int orochi_auth_rate_limit_window;
extern bool orochi_auth_audit_enabled;
extern int orochi_auth_audit_retention_days;
extern char *orochi_auth_jwt_algorithm;
extern int orochi_auth_jwt_leeway;
extern int orochi_auth_password_min_length;
extern bool orochi_auth_password_require_uppercase;
extern bool orochi_auth_password_require_number;
extern bool orochi_auth_password_require_special;
extern int orochi_auth_key_rotation_days;

/* Session variables (set per connection) */
extern char *orochi_auth_current_user_id;
extern char *orochi_auth_current_tenant_id;
extern char *orochi_auth_current_role;
extern char *orochi_auth_current_session_id;

/* ============================================================
 * Initialization Functions
 * ============================================================ */

/*
 * Calculate shared memory size requirements
 */
extern Size orochi_auth_shmem_size(void);

/*
 * Initialize auth shared memory structures
 */
extern void orochi_auth_shmem_init(void);

/*
 * Define GUC configuration parameters
 */
extern void orochi_auth_define_gucs(void);

/*
 * Initialize authentication subsystem
 */
extern void orochi_auth_init(void);

/*
 * Cleanup authentication subsystem
 */
extern void orochi_auth_fini(void);

/* ============================================================
 * Token Validation Functions
 * ============================================================ */

/*
 * Validate a JWT token
 * Returns validation result with user context
 */
extern OrochiTokenValidation orochi_auth_validate_token(const char *token, const char *tenant_id);

/*
 * Fast path token validation (cache lookup only)
 * Returns true if token found in cache and valid
 */
extern bool orochi_auth_validate_token_fast(const char *token, int token_len,
                                            OrochiTokenValidation *result);

/*
 * Full JWT validation with signature verification
 */
extern OrochiTokenValidation orochi_auth_validate_jwt_full(const char *token,
                                                           const char *tenant_id);

/*
 * Validate an API key
 */
extern OrochiTokenValidation orochi_auth_validate_api_key(const char *api_key);

/*
 * API Key Info structure for database operations
 */
typedef struct OrochiApiKeyInfo {
    int64 key_id;
    char prefix[OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1];
    char name[256];
    char user_id[OROCHI_AUTH_USER_ID_SIZE + 1];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    TimestampTz created_at;
    TimestampTz last_used_at;
    TimestampTz expires_at;
    bool is_revoked;
    bool has_last_used;
    bool has_expires;
} OrochiApiKeyInfo;

/*
 * Look up an API key by its hash in the database
 * Returns: The API key info if found and valid, NULL otherwise
 */
extern OrochiApiKeyInfo *orochi_auth_lookup_api_key_db(const char *key_hash);

/*
 * Update the last_used_at timestamp for an API key
 */
extern void orochi_auth_update_api_key_usage(const char *key_hash);

/*
 * Hash a token for cache lookup (SHA-256)
 */
extern void orochi_auth_hash_token(const char *token, char *hash_out);

/* ============================================================
 * Cache Management Functions
 * ============================================================ */

/*
 * Look up token in cache
 * Returns NULL if not found
 */
extern OrochiToken *orochi_auth_cache_lookup(const char *token_hash);

/*
 * Add token to cache
 */
extern void orochi_auth_cache_token(OrochiTokenValidation *validation, const char *token_hash);

/*
 * Invalidate token in cache
 */
extern void orochi_auth_cache_invalidate_token(const char *token_hash);

/*
 * Invalidate all tokens for a user
 */
extern void orochi_auth_cache_invalidate_user(const char *user_id);

/*
 * Cleanup expired tokens from cache
 */
extern void orochi_auth_cleanup_expired_tokens(void);

/*
 * Look up session in cache
 */
extern OrochiSession *orochi_auth_session_cache_lookup(const char *session_id);

/*
 * Add session to cache
 */
extern void orochi_auth_cache_session(OrochiSession *session);

/*
 * Invalidate session in cache
 */
extern void orochi_auth_cache_invalidate_session(const char *session_id);

/*
 * Cleanup expired sessions from cache
 */
extern void orochi_auth_cleanup_expired_sessions(void);

/* ============================================================
 * Session Management Functions
 * ============================================================ */

/*
 * Create a new session
 */
extern OrochiSession *orochi_auth_create_session(const char *user_id, const char *tenant_id,
                                                 OrochiAuthMethod auth_method,
                                                 const char *ip_address, const char *user_agent);

/*
 * Get session by ID
 */
extern OrochiSession *orochi_auth_get_session(const char *session_id);

/*
 * Update session activity timestamp
 */
extern void orochi_auth_touch_session(const char *session_id);

/*
 * Revoke a session
 */
extern void orochi_auth_revoke_session(const char *session_id);

/*
 * Update session states (called by background worker)
 */
extern void orochi_auth_update_session_states(void);

/*
 * Enforce per-user session limits
 */
extern void orochi_auth_enforce_session_limits(void);

/* ============================================================
 * Signing Key Management Functions
 * ============================================================ */

/*
 * Get current signing key for tenant
 */
extern OrochiSigningKey *orochi_auth_get_signing_key(const char *tenant_id);

/*
 * Get signing key by key ID
 */
extern OrochiSigningKey *orochi_auth_get_signing_key_by_id(const char *key_id);

/*
 * Check and rotate signing keys if needed
 */
extern void orochi_auth_check_key_rotation(void);

/*
 * Refresh signing key cache from database
 */
extern void orochi_auth_refresh_signing_key_cache(void);

/*
 * Add signing key to cache
 */
extern void orochi_auth_cache_signing_key(OrochiSigningKey *key);

/* ============================================================
 * Rate Limiting Functions
 * ============================================================ */

/*
 * Check rate limit for a key/action combination
 * Returns true if request is allowed
 */
extern bool orochi_auth_check_rate_limit(const char *key, const char *action, int limit);

/*
 * Record a rate limit hit
 */
extern void orochi_auth_record_rate_limit(const char *key, const char *action);

/*
 * Clear rate limit for a key
 */
extern void orochi_auth_clear_rate_limit(const char *key, const char *action);

/*
 * Cleanup expired rate limits
 */
extern void orochi_auth_cleanup_rate_limits(void);

/* ============================================================
 * Audit Logging Functions
 * ============================================================ */

/*
 * Log an authentication event
 */
extern void orochi_auth_audit_log(const char *tenant_id, const char *user_id,
                                  const char *session_id, OrochiAuditEvent event_type,
                                  const char *ip_address, const char *user_agent,
                                  const char *resource_type, const char *resource_id);

/*
 * Ensure audit log partitions exist
 */
extern void orochi_auth_ensure_audit_partitions(void);

/*
 * Archive old audit log partitions
 */
extern void orochi_auth_archive_audit_partitions(void);

/* ============================================================
 * Session Context Functions
 * ============================================================ */

/*
 * Set PostgreSQL session variables with auth context
 */
extern void orochi_auth_set_session_context(OrochiTokenValidation *auth);

/*
 * Clear session context
 */
extern void orochi_auth_clear_session_context(void);

/*
 * Get current authenticated user ID (or NULL)
 */
extern char *orochi_auth_get_current_user_id(void);

/*
 * Get current tenant ID (or NULL)
 */
extern char *orochi_auth_get_current_tenant_id(void);

/*
 * Get current role
 */
extern char *orochi_auth_get_current_role(void);

/* ============================================================
 * Background Worker Functions
 * ============================================================ */

/*
 * Session cleanup background worker main function
 */
extern void orochi_auth_session_worker_main(Datum main_arg);

/*
 * Token cleanup background worker main function
 */
extern void orochi_auth_token_cleanup_worker_main(Datum main_arg);

/*
 * Key rotation background worker main function
 */
extern void orochi_auth_key_rotation_worker_main(Datum main_arg);

/*
 * Process token revocation queue
 */
extern void orochi_auth_process_revocation_queue(void);

/* ============================================================
 * Hook Functions
 * ============================================================ */

/*
 * Install authentication hooks
 */
extern void orochi_auth_install_hooks(void);

/*
 * Uninstall authentication hooks
 */
extern void orochi_auth_uninstall_hooks(void);

/*
 * Client authentication hook
 */
extern void orochi_auth_client_authentication_hook(Port *port, int status);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Generate a secure random session ID
 */
extern void orochi_auth_generate_session_id(char *session_id_out);

/*
 * Generate a secure random token
 */
extern void orochi_auth_generate_token(char *token_out, int length);

/*
 * Verify JWT signature
 */
extern bool orochi_auth_verify_jwt_signature(const char *token, OrochiSigningKey *key);

/*
 * Parse JWT claims
 */
extern bool orochi_auth_parse_jwt_claims(const char *token, OrochiTokenValidation *result);

/*
 * Get algorithm from string
 */
extern OrochiJwtAlgorithm orochi_auth_algorithm_from_string(const char *alg);

/*
 * Get string from algorithm
 */
extern const char *orochi_auth_algorithm_to_string(OrochiJwtAlgorithm alg);

/*
 * Get auth method name
 */
extern const char *orochi_auth_method_name(OrochiAuthMethod method);

/*
 * Get session state name
 */
extern const char *orochi_auth_session_state_name(OrochiSessionState state);

/*
 * Get audit event name
 */
extern const char *orochi_auth_audit_event_name(OrochiAuditEvent event);

/*
 * Check GUC algorithm value
 */
extern bool orochi_auth_check_algorithm(char **newval, void **extra, GucSource source);

/*
 * Get auth statistics
 */
extern void orochi_auth_get_stats(uint64 *total_requests, uint64 *cache_hits, uint64 *cache_misses,
                                  uint64 *failed_auths, uint64 *tokens_issued,
                                  uint64 *tokens_revoked);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

/* Authentication functions */
extern Datum orochi_auth_uid(PG_FUNCTION_ARGS);
extern Datum orochi_auth_role(PG_FUNCTION_ARGS);
extern Datum orochi_auth_jwt(PG_FUNCTION_ARGS);
extern Datum orochi_auth_verify_token(PG_FUNCTION_ARGS);
extern Datum orochi_auth_create_api_key(PG_FUNCTION_ARGS);
extern Datum orochi_auth_revoke_api_key(PG_FUNCTION_ARGS);
extern Datum orochi_auth_list_api_keys(PG_FUNCTION_ARGS);
extern Datum orochi_auth_stats(PG_FUNCTION_ARGS);

#endif /* OROCHI_AUTH_H */
