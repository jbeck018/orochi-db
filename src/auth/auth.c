/*-------------------------------------------------------------------------
 *
 * auth.c
 *    Orochi DB Authentication System - Core Implementation
 *
 * This file implements:
 *   - Token validation (JWT and API key)
 *   - Session management
 *   - GUC configuration
 *   - Utility functions
 *   - Background worker registration
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "common/sha2.h"
#include "access/xact.h"

#include "auth.h"

/* ============================================================
 * Global Variables
 * ============================================================ */

/* Global pointer to auth shared state */
OrochiAuthCache *OrochiAuthSharedState = NULL;

/* GUC variables */
bool    orochi_auth_enabled = true;
char   *orochi_auth_jwt_secret = NULL;
int     orochi_auth_token_expiry = OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC;
int     orochi_auth_max_sessions = OROCHI_AUTH_MAX_CACHED_SESSIONS;
int     orochi_auth_access_token_ttl = OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC;
int     orochi_auth_refresh_token_ttl = OROCHI_AUTH_REFRESH_TOKEN_TTL_SEC;
int     orochi_auth_session_ttl = OROCHI_AUTH_SESSION_TTL_SEC;
int     orochi_auth_max_sessions_per_user = 10;
bool    orochi_auth_mfa_required = false;
bool    orochi_auth_rate_limit_enabled = true;
int     orochi_auth_rate_limit_login = 5;
int     orochi_auth_rate_limit_api = 100;
int     orochi_auth_rate_limit_window = 60;
bool    orochi_auth_audit_enabled = true;
int     orochi_auth_audit_retention_days = 90;
char   *orochi_auth_jwt_algorithm = NULL;
int     orochi_auth_jwt_leeway = 60;
int     orochi_auth_password_min_length = 8;
bool    orochi_auth_password_require_uppercase = true;
bool    orochi_auth_password_require_number = true;
bool    orochi_auth_password_require_special = false;
int     orochi_auth_key_rotation_days = OROCHI_AUTH_KEY_ROTATION_DAYS;

/* Session variables (set per connection) */
char   *orochi_auth_current_user_id = NULL;
char   *orochi_auth_current_tenant_id = NULL;
char   *orochi_auth_current_role = NULL;
char   *orochi_auth_current_session_id = NULL;

/* ============================================================
 * GUC Check Function
 * ============================================================ */

/*
 * orochi_auth_check_algorithm
 *    Validate JWT algorithm GUC value
 */
bool
orochi_auth_check_algorithm(char **newval, void **extra, GucSource source)
{
    if (*newval == NULL || strlen(*newval) == 0)
        return true;  /* Allow empty/NULL */

    /* Check for valid algorithm strings */
    if (strcmp(*newval, "HS256") == 0 ||
        strcmp(*newval, "HS384") == 0 ||
        strcmp(*newval, "HS512") == 0 ||
        strcmp(*newval, "RS256") == 0 ||
        strcmp(*newval, "RS384") == 0 ||
        strcmp(*newval, "RS512") == 0 ||
        strcmp(*newval, "ES256") == 0 ||
        strcmp(*newval, "ES384") == 0 ||
        strcmp(*newval, "ES512") == 0)
    {
        return true;
    }

    GUC_check_errdetail("Valid algorithms are: HS256, HS384, HS512, RS256, RS384, RS512, ES256, ES384, ES512");
    return false;
}

/* ============================================================
 * GUC Definitions
 * ============================================================ */

/*
 * orochi_auth_define_gucs
 *    Register all authentication GUC variables
 */
void
orochi_auth_define_gucs(void)
{
    /* Main enable/disable */
    DefineCustomBoolVariable("orochi.auth_enabled",
                             "Enable Orochi authentication system",
                             NULL,
                             &orochi_auth_enabled,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    /* JWT Configuration */
    DefineCustomStringVariable("orochi.jwt_secret",
                               "JWT signing secret (deprecated, use signing keys)",
                               NULL,
                               &orochi_auth_jwt_secret,
                               NULL,
                               PGC_SIGHUP,
                               GUC_SUPERUSER_ONLY,
                               NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.token_expiry",
                            "Default token expiry time in seconds",
                            NULL,
                            &orochi_auth_token_expiry,
                            OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC,
                            60,
                            86400,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.max_sessions",
                            "Maximum cached sessions",
                            NULL,
                            &orochi_auth_max_sessions,
                            OROCHI_AUTH_MAX_CACHED_SESSIONS,
                            100,
                            100000,
                            PGC_POSTMASTER,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_access_token_ttl",
                            "Access token time-to-live in seconds",
                            NULL,
                            &orochi_auth_access_token_ttl,
                            OROCHI_AUTH_ACCESS_TOKEN_TTL_SEC,
                            60,
                            86400,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_refresh_token_ttl",
                            "Refresh token time-to-live in seconds",
                            NULL,
                            &orochi_auth_refresh_token_ttl,
                            OROCHI_AUTH_REFRESH_TOKEN_TTL_SEC,
                            3600,
                            2592000,  /* 30 days */
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_session_ttl",
                            "Session time-to-live in seconds",
                            NULL,
                            &orochi_auth_session_ttl,
                            OROCHI_AUTH_SESSION_TTL_SEC,
                            3600,
                            604800,  /* 7 days */
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_max_sessions_per_user",
                            "Maximum concurrent sessions per user",
                            NULL,
                            &orochi_auth_max_sessions_per_user,
                            10,
                            1,
                            1000,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_jwt_algorithm",
                               "JWT signing algorithm (RS256, ES256, HS256)",
                               NULL,
                               &orochi_auth_jwt_algorithm,
                               "RS256",
                               PGC_SIGHUP,
                               0,
                               orochi_auth_check_algorithm, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_jwt_leeway",
                            "JWT clock skew tolerance in seconds",
                            NULL,
                            &orochi_auth_jwt_leeway,
                            60,
                            0,
                            300,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    /* MFA Configuration */
    DefineCustomBoolVariable("orochi.auth_mfa_required",
                             "Require MFA for all users",
                             NULL,
                             &orochi_auth_mfa_required,
                             false,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    /* Rate Limiting */
    DefineCustomBoolVariable("orochi.auth_rate_limit_enabled",
                             "Enable authentication rate limiting",
                             NULL,
                             &orochi_auth_rate_limit_enabled,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_rate_limit_login",
                            "Maximum login attempts per window",
                            NULL,
                            &orochi_auth_rate_limit_login,
                            5,
                            1,
                            100,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_rate_limit_api",
                            "Maximum API requests per window",
                            NULL,
                            &orochi_auth_rate_limit_api,
                            100,
                            1,
                            10000,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_rate_limit_window",
                            "Rate limit window in seconds",
                            NULL,
                            &orochi_auth_rate_limit_window,
                            60,
                            1,
                            3600,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    /* Password Configuration */
    DefineCustomIntVariable("orochi.auth_password_min_length",
                            "Minimum password length",
                            NULL,
                            &orochi_auth_password_min_length,
                            8,
                            6,
                            128,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("orochi.auth_password_require_uppercase",
                             "Require uppercase letter in password",
                             NULL,
                             &orochi_auth_password_require_uppercase,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("orochi.auth_password_require_number",
                             "Require number in password",
                             NULL,
                             &orochi_auth_password_require_number,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("orochi.auth_password_require_special",
                             "Require special character in password",
                             NULL,
                             &orochi_auth_password_require_special,
                             false,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    /* Audit Configuration */
    DefineCustomBoolVariable("orochi.auth_audit_enabled",
                             "Enable authentication audit logging",
                             NULL,
                             &orochi_auth_audit_enabled,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("orochi.auth_audit_retention_days",
                            "Audit log retention period in days",
                            NULL,
                            &orochi_auth_audit_retention_days,
                            90,
                            7,
                            3650,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    /* Key Management */
    DefineCustomIntVariable("orochi.auth_key_rotation_days",
                            "Signing key rotation interval in days",
                            NULL,
                            &orochi_auth_key_rotation_days,
                            OROCHI_AUTH_KEY_ROTATION_DAYS,
                            7,
                            365,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    /* Session variables (set per connection, read by RLS policies) */
    DefineCustomStringVariable("orochi.auth_user_id",
                               "Current authenticated user ID",
                               NULL,
                               &orochi_auth_current_user_id,
                               "",
                               PGC_USERSET,
                               GUC_NOT_IN_SAMPLE,
                               NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_tenant_id",
                               "Current tenant ID",
                               NULL,
                               &orochi_auth_current_tenant_id,
                               "",
                               PGC_USERSET,
                               GUC_NOT_IN_SAMPLE,
                               NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_role",
                               "Current user role",
                               NULL,
                               &orochi_auth_current_role,
                               "anon",
                               PGC_USERSET,
                               GUC_NOT_IN_SAMPLE,
                               NULL, NULL, NULL);

    DefineCustomStringVariable("orochi.auth_session_id",
                               "Current session ID",
                               NULL,
                               &orochi_auth_current_session_id,
                               "",
                               PGC_USERSET,
                               GUC_NOT_IN_SAMPLE,
                               NULL, NULL, NULL);
}

/* ============================================================
 * Initialization Functions
 * ============================================================ */

/*
 * orochi_auth_init
 *    Initialize authentication subsystem
 */
void
orochi_auth_init(void)
{
    /* GUCs are defined elsewhere (in main init) */

    elog(LOG, "Orochi Auth subsystem initialized");
}

/*
 * orochi_auth_fini
 *    Cleanup authentication subsystem
 */
void
orochi_auth_fini(void)
{
    /* Uninstall hooks */
    orochi_auth_uninstall_hooks();

    elog(LOG, "Orochi Auth subsystem shutdown");
}

/* ============================================================
 * Token Validation Functions
 * ============================================================ */

/*
 * orochi_auth_hash_token
 *    Hash a token using SHA-256 for cache lookup
 */
void
orochi_auth_hash_token(const char *token, char *hash_out)
{
    pg_sha256_ctx ctx;
    uint8       digest[PG_SHA256_DIGEST_LENGTH];
    int         i;

    pg_sha256_init(&ctx);
    pg_sha256_update(&ctx, (const uint8 *) token, strlen(token));
    pg_sha256_final(&ctx, digest);

    /* Convert to hex string */
    for (i = 0; i < PG_SHA256_DIGEST_LENGTH; i++)
    {
        sprintf(hash_out + (i * 2), "%02x", digest[i]);
    }
    hash_out[OROCHI_AUTH_TOKEN_HASH_SIZE] = '\0';
}

/*
 * orochi_auth_validate_token
 *    Main entry point for token validation
 */
OrochiTokenValidation
orochi_auth_validate_token(const char *token, const char *tenant_id)
{
    OrochiTokenValidation result;
    char                  token_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];

    memset(&result, 0, sizeof(OrochiTokenValidation));

    if (!orochi_auth_enabled)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "AUTH_DISABLED", sizeof(result.error_code));
        strlcpy(result.error_message, "Authentication is disabled",
                sizeof(result.error_message));
        return result;
    }

    if (token == NULL || strlen(token) == 0)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "MISSING_TOKEN", sizeof(result.error_code));
        strlcpy(result.error_message, "No token provided",
                sizeof(result.error_message));
        return result;
    }

    /* Hash token for lookup */
    orochi_auth_hash_token(token, token_hash);

    /* Try fast path (cache lookup) */
    if (orochi_auth_validate_token_fast(token, strlen(token), &result))
    {
        return result;
    }

    /* Cache miss - perform full JWT validation */
    if (OrochiAuthSharedState != NULL)
    {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->cache_misses, 1);
    }

    result = orochi_auth_validate_jwt_full(token, tenant_id);

    /* Cache valid tokens */
    if (result.is_valid)
    {
        orochi_auth_cache_token(&result, token_hash);
    }

    return result;
}

/*
 * orochi_auth_validate_token_fast
 *    Fast path token validation using cache
 */
bool
orochi_auth_validate_token_fast(const char *token,
                                 int token_len,
                                 OrochiTokenValidation *result)
{
    char            token_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    OrochiToken    *cached;
    TimestampTz     now;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return false;

    /* Hash token for lookup */
    orochi_auth_hash_token(token, token_hash);

    /* Try to find in cache */
    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_SHARED);
    cached = orochi_auth_cache_lookup(token_hash);

    if (cached != NULL)
    {
        now = GetCurrentTimestamp();

        /* Check if token is expired */
        if (cached->expires_at < now)
        {
            result->is_valid = false;
            result->is_expired = true;
            strlcpy(result->error_code, "TOKEN_EXPIRED", sizeof(result->error_code));
            strlcpy(result->error_message, "Token has expired",
                    sizeof(result->error_message));
            LWLockRelease(OrochiAuthSharedState->token_cache_lock);
            return true;  /* Found in cache but expired */
        }

        /* Check if token is revoked */
        if (cached->is_revoked)
        {
            result->is_valid = false;
            result->is_revoked = true;
            strlcpy(result->error_code, "TOKEN_REVOKED", sizeof(result->error_code));
            strlcpy(result->error_message, "Token has been revoked",
                    sizeof(result->error_message));
            LWLockRelease(OrochiAuthSharedState->token_cache_lock);
            return true;  /* Found in cache but revoked */
        }

        /* Valid cached token */
        result->is_valid = true;
        strlcpy(result->user_id, cached->user_id, sizeof(result->user_id));
        strlcpy(result->tenant_id, cached->tenant_id, sizeof(result->tenant_id));
        strlcpy(result->session_id, cached->session_id, sizeof(result->session_id));
        result->scopes = cached->scopes;
        result->expires_at = cached->expires_at;

        /* Update stats */
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->cache_hits, 1);

        LWLockRelease(OrochiAuthSharedState->token_cache_lock);
        return true;
    }

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);
    return false;  /* Not found in cache */
}

/*
 * orochi_auth_validate_jwt_full
 *    Full JWT validation with signature verification
 */
OrochiTokenValidation
orochi_auth_validate_jwt_full(const char *token, const char *tenant_id)
{
    OrochiTokenValidation result;
    OrochiSigningKey     *key;
    TimestampTz           now;

    memset(&result, 0, sizeof(OrochiTokenValidation));

    /* Parse JWT claims first */
    if (!orochi_auth_parse_jwt_claims(token, &result))
    {
        result.is_valid = false;
        if (strlen(result.error_code) == 0)
        {
            strlcpy(result.error_code, "INVALID_TOKEN", sizeof(result.error_code));
            strlcpy(result.error_message, "Failed to parse token claims",
                    sizeof(result.error_message));
        }
        return result;
    }

    /* Get signing key for verification */
    key = orochi_auth_get_signing_key(tenant_id);
    if (key == NULL)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "NO_SIGNING_KEY", sizeof(result.error_code));
        strlcpy(result.error_message, "No signing key found for tenant",
                sizeof(result.error_message));
        return result;
    }

    /* Verify signature */
    if (!orochi_auth_verify_jwt_signature(token, key))
    {
        result.is_valid = false;
        strlcpy(result.error_code, "INVALID_SIGNATURE", sizeof(result.error_code));
        strlcpy(result.error_message, "Token signature verification failed",
                sizeof(result.error_message));

        /* Log failed auth attempt */
        if (OrochiAuthSharedState != NULL)
        {
            pg_atomic_fetch_add_u64(&OrochiAuthSharedState->failed_authentications, 1);
        }

        return result;
    }

    /* Check expiration with leeway */
    now = GetCurrentTimestamp();
    if (result.expires_at < now - (orochi_auth_jwt_leeway * USECS_PER_SEC))
    {
        result.is_valid = false;
        result.is_expired = true;
        strlcpy(result.error_code, "TOKEN_EXPIRED", sizeof(result.error_code));
        strlcpy(result.error_message, "Token has expired",
                sizeof(result.error_message));
        return result;
    }

    /* Token is valid */
    result.is_valid = true;

    /* Update stats */
    if (OrochiAuthSharedState != NULL)
    {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->total_auth_requests, 1);
    }

    return result;
}

/*
 * orochi_auth_validate_api_key
 *    Validate an API key
 */
OrochiTokenValidation
orochi_auth_validate_api_key(const char *api_key)
{
    OrochiTokenValidation result;
    char                  key_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    char                  prefix[OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1];

    memset(&result, 0, sizeof(OrochiTokenValidation));

    if (!orochi_auth_enabled)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "AUTH_DISABLED", sizeof(result.error_code));
        strlcpy(result.error_message, "Authentication is disabled",
                sizeof(result.error_message));
        return result;
    }

    if (api_key == NULL || strlen(api_key) < OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "INVALID_API_KEY", sizeof(result.error_code));
        strlcpy(result.error_message, "Invalid API key format",
                sizeof(result.error_message));
        return result;
    }

    /* Extract prefix for lookup */
    strlcpy(prefix, api_key, OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1);

    /* Hash full key for verification */
    orochi_auth_hash_token(api_key, key_hash);

    /*
     * TODO: Look up API key in database by prefix,
     * then verify hash matches stored hash.
     * For now, return invalid.
     */
    result.is_valid = false;
    strlcpy(result.error_code, "API_KEY_NOT_FOUND", sizeof(result.error_code));
    strlcpy(result.error_message, "API key not found or invalid",
            sizeof(result.error_message));

    return result;
}

/*
 * orochi_auth_parse_jwt_claims
 *    Parse JWT and extract claims
 */
bool
orochi_auth_parse_jwt_claims(const char *token, OrochiTokenValidation *result)
{
    const char *header_end;
    const char *payload_end;
    char       *payload;
    int         payload_len;

    /* JWT format: header.payload.signature */
    header_end = strchr(token, '.');
    if (header_end == NULL)
        return false;

    payload_end = strchr(header_end + 1, '.');
    if (payload_end == NULL)
        return false;

    /* Get payload portion */
    payload_len = payload_end - (header_end + 1);
    if (payload_len <= 0 || payload_len > OROCHI_AUTH_JWT_MAX_SIZE)
        return false;

    payload = palloc(payload_len + 1);
    memcpy(payload, header_end + 1, payload_len);
    payload[payload_len] = '\0';

    /*
     * TODO: Base64 decode payload and parse JSON claims.
     * Extract: sub (user_id), tenant_id, exp, iat, jti, roles, etc.
     *
     * For now, we set placeholder values.
     */

    /* Set default/placeholder values */
    strlcpy(result->user_id, "", sizeof(result->user_id));
    strlcpy(result->tenant_id, "", sizeof(result->tenant_id));
    strlcpy(result->session_id, "", sizeof(result->session_id));
    result->expires_at = GetCurrentTimestamp() + (orochi_auth_access_token_ttl * USECS_PER_SEC);
    result->scopes = 0;
    result->role_count = 0;

    pfree(payload);
    return true;
}

/*
 * orochi_auth_verify_jwt_signature
 *    Verify JWT signature using signing key
 */
bool
orochi_auth_verify_jwt_signature(const char *token, OrochiSigningKey *key)
{
    const char *sig_start;

    if (token == NULL || key == NULL)
        return false;

    /* Find signature portion */
    sig_start = strrchr(token, '.');
    if (sig_start == NULL)
        return false;

    /*
     * TODO: Implement actual signature verification based on algorithm:
     * - HS256/384/512: HMAC verification
     * - RS256/384/512: RSA signature verification
     * - ES256/384/512: ECDSA signature verification
     *
     * For now, return true (placeholder).
     */

    return true;
}

/* ============================================================
 * Session Management Functions
 * ============================================================ */

/*
 * orochi_auth_generate_session_id
 *    Generate a secure random session ID
 */
void
orochi_auth_generate_session_id(char *session_id_out)
{
    pg_uuid_t   uuid;

    /*
     * Generate a random UUID for session ID.
     * We use the UUID generation functions.
     */
    if (!pg_strong_random(&uuid, sizeof(pg_uuid_t)))
    {
        /* Fallback to timestamp-based if strong random fails */
        TimestampTz now = GetCurrentTimestamp();
        snprintf(session_id_out, OROCHI_AUTH_SESSION_ID_SIZE + 1,
                 "%016lx%016lx",
                 (unsigned long) now,
                 (unsigned long) MyProcPid);
        return;
    }

    /* Format as hex string without dashes */
    snprintf(session_id_out, OROCHI_AUTH_SESSION_ID_SIZE + 1,
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3],
             uuid.data[4], uuid.data[5], uuid.data[6], uuid.data[7],
             uuid.data[8], uuid.data[9], uuid.data[10], uuid.data[11],
             uuid.data[12], uuid.data[13], uuid.data[14], uuid.data[15]);
}

/*
 * orochi_auth_generate_token
 *    Generate a secure random token
 */
void
orochi_auth_generate_token(char *token_out, int length)
{
    uint8      *random_bytes;
    int         bytes_needed;
    int         i;

    bytes_needed = (length + 1) / 2;  /* 2 hex chars per byte */
    random_bytes = palloc(bytes_needed);

    if (!pg_strong_random(random_bytes, bytes_needed))
    {
        /* Fallback */
        for (i = 0; i < bytes_needed; i++)
            random_bytes[i] = (uint8) (rand() % 256);
    }

    for (i = 0; i < bytes_needed && i * 2 < length; i++)
    {
        sprintf(token_out + (i * 2), "%02x", random_bytes[i]);
    }
    token_out[length] = '\0';

    pfree(random_bytes);
}

/*
 * orochi_auth_create_session
 *    Create a new session
 */
OrochiSession *
orochi_auth_create_session(const char *user_id,
                            const char *tenant_id,
                            OrochiAuthMethod auth_method,
                            const char *ip_address,
                            const char *user_agent)
{
    OrochiSession *session;
    TimestampTz    now;

    session = palloc0(sizeof(OrochiSession));
    now = GetCurrentTimestamp();

    /* Generate session ID */
    orochi_auth_generate_session_id(session->session_id);

    /* Set session properties */
    if (user_id)
        strlcpy(session->user_id, user_id, sizeof(session->user_id));
    if (tenant_id)
        strlcpy(session->tenant_id, tenant_id, sizeof(session->tenant_id));

    session->state = OROCHI_SESSION_ACTIVE;
    session->auth_method = auth_method;

    if (ip_address)
        strlcpy(session->ip_address, ip_address, sizeof(session->ip_address));
    if (user_agent)
        strlcpy(session->user_agent, user_agent, sizeof(session->user_agent));

    session->created_at = now;
    session->last_activity = now;
    session->expires_at = now + (orochi_auth_session_ttl * USECS_PER_SEC);
    session->refresh_count = 0;
    session->mfa_verified = false;
    strlcpy(session->aal, "aal1", sizeof(session->aal));

    /* Cache the session */
    orochi_auth_cache_session(session);

    /* Update stats */
    if (OrochiAuthSharedState != NULL)
    {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->sessions_created, 1);
    }

    /* Audit log */
    if (orochi_auth_audit_enabled)
    {
        orochi_auth_audit_log(tenant_id, user_id, session->session_id,
                               OROCHI_AUDIT_SESSION_CREATED,
                               ip_address, user_agent, "session", session->session_id);
    }

    return session;
}

/*
 * orochi_auth_get_session
 *    Get session by ID
 */
OrochiSession *
orochi_auth_get_session(const char *session_id)
{
    return orochi_auth_session_cache_lookup(session_id);
}

/*
 * orochi_auth_touch_session
 *    Update session activity timestamp
 */
void
orochi_auth_touch_session(const char *session_id)
{
    OrochiSession *session;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    session = orochi_auth_session_cache_lookup(session_id);
    if (session != NULL)
    {
        session->last_activity = GetCurrentTimestamp();
        if (session->state == OROCHI_SESSION_IDLE)
            session->state = OROCHI_SESSION_ACTIVE;
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_revoke_session
 *    Revoke a session
 */
void
orochi_auth_revoke_session(const char *session_id)
{
    OrochiSession *session;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    session = orochi_auth_session_cache_lookup(session_id);
    if (session != NULL)
    {
        session->state = OROCHI_SESSION_REVOKED;
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);

    /* Audit log */
    if (orochi_auth_audit_enabled && session != NULL)
    {
        orochi_auth_audit_log(session->tenant_id, session->user_id, session_id,
                               OROCHI_AUDIT_LOGOUT,
                               session->ip_address, session->user_agent,
                               "session", session_id);
    }
}

/* ============================================================
 * Session Context Functions
 * ============================================================ */

/*
 * orochi_auth_set_session_context
 *    Set PostgreSQL session variables with auth context
 */
void
orochi_auth_set_session_context(OrochiTokenValidation *auth)
{
    if (auth == NULL || !auth->is_valid)
        return;

    /* Set user ID */
    SetConfigOption("orochi.auth_user_id", auth->user_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set tenant ID */
    SetConfigOption("orochi.auth_tenant_id", auth->tenant_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set session ID */
    SetConfigOption("orochi.auth_session_id", auth->session_id,
                    PGC_USERSET, PGC_S_SESSION);

    /* Set role for RLS */
    if (auth->role_count > 0)
    {
        SetConfigOption("orochi.auth_role", auth->roles[0],
                        PGC_USERSET, PGC_S_SESSION);
    }
    else
    {
        SetConfigOption("orochi.auth_role", "authenticated",
                        PGC_USERSET, PGC_S_SESSION);
    }
}

/*
 * orochi_auth_clear_session_context
 *    Clear session context
 */
void
orochi_auth_clear_session_context(void)
{
    SetConfigOption("orochi.auth_user_id", "",
                    PGC_USERSET, PGC_S_SESSION);
    SetConfigOption("orochi.auth_tenant_id", "",
                    PGC_USERSET, PGC_S_SESSION);
    SetConfigOption("orochi.auth_session_id", "",
                    PGC_USERSET, PGC_S_SESSION);
    SetConfigOption("orochi.auth_role", "anon",
                    PGC_USERSET, PGC_S_SESSION);
}

/*
 * orochi_auth_get_current_user_id
 *    Get current authenticated user ID
 */
char *
orochi_auth_get_current_user_id(void)
{
    const char *user_id;

    user_id = GetConfigOption("orochi.auth_user_id", true, false);
    if (user_id == NULL || strlen(user_id) == 0)
        return NULL;

    return pstrdup(user_id);
}

/*
 * orochi_auth_get_current_tenant_id
 *    Get current tenant ID
 */
char *
orochi_auth_get_current_tenant_id(void)
{
    const char *tenant_id;

    tenant_id = GetConfigOption("orochi.auth_tenant_id", true, false);
    if (tenant_id == NULL || strlen(tenant_id) == 0)
        return NULL;

    return pstrdup(tenant_id);
}

/*
 * orochi_auth_get_current_role
 *    Get current role
 */
char *
orochi_auth_get_current_role(void)
{
    const char *role;

    role = GetConfigOption("orochi.auth_role", true, false);
    if (role == NULL || strlen(role) == 0)
        return pstrdup("anon");

    return pstrdup(role);
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * orochi_auth_algorithm_from_string
 *    Convert algorithm string to enum
 */
OrochiJwtAlgorithm
orochi_auth_algorithm_from_string(const char *alg)
{
    if (alg == NULL)
        return OROCHI_JWT_ALG_RS256;

    if (strcmp(alg, "HS256") == 0) return OROCHI_JWT_ALG_HS256;
    if (strcmp(alg, "HS384") == 0) return OROCHI_JWT_ALG_HS384;
    if (strcmp(alg, "HS512") == 0) return OROCHI_JWT_ALG_HS512;
    if (strcmp(alg, "RS256") == 0) return OROCHI_JWT_ALG_RS256;
    if (strcmp(alg, "RS384") == 0) return OROCHI_JWT_ALG_RS384;
    if (strcmp(alg, "RS512") == 0) return OROCHI_JWT_ALG_RS512;
    if (strcmp(alg, "ES256") == 0) return OROCHI_JWT_ALG_ES256;
    if (strcmp(alg, "ES384") == 0) return OROCHI_JWT_ALG_ES384;
    if (strcmp(alg, "ES512") == 0) return OROCHI_JWT_ALG_ES512;

    return OROCHI_JWT_ALG_RS256;  /* Default */
}

/*
 * orochi_auth_algorithm_to_string
 *    Convert algorithm enum to string
 */
const char *
orochi_auth_algorithm_to_string(OrochiJwtAlgorithm alg)
{
    switch (alg)
    {
        case OROCHI_JWT_ALG_HS256: return "HS256";
        case OROCHI_JWT_ALG_HS384: return "HS384";
        case OROCHI_JWT_ALG_HS512: return "HS512";
        case OROCHI_JWT_ALG_RS256: return "RS256";
        case OROCHI_JWT_ALG_RS384: return "RS384";
        case OROCHI_JWT_ALG_RS512: return "RS512";
        case OROCHI_JWT_ALG_ES256: return "ES256";
        case OROCHI_JWT_ALG_ES384: return "ES384";
        case OROCHI_JWT_ALG_ES512: return "ES512";
        default: return "RS256";
    }
}

/*
 * orochi_auth_method_name
 *    Get auth method name
 */
const char *
orochi_auth_method_name(OrochiAuthMethod method)
{
    switch (method)
    {
        case OROCHI_AUTH_NONE:              return "none";
        case OROCHI_AUTH_PASSWORD:          return "password";
        case OROCHI_AUTH_API_KEY:           return "api_key";
        case OROCHI_AUTH_JWT:               return "jwt";
        case OROCHI_AUTH_OAUTH:             return "oauth";
        case OROCHI_AUTH_MAGIC_LINK:        return "magic_link";
        case OROCHI_AUTH_PHONE_OTP:         return "phone_otp";
        case OROCHI_AUTH_SAML:              return "saml";
        case OROCHI_AUTH_CERTIFICATE:       return "certificate";
        case OROCHI_AUTH_CONNECTION_STRING: return "connection_string";
        default: return "unknown";
    }
}

/*
 * orochi_auth_session_state_name
 *    Get session state name
 */
const char *
orochi_auth_session_state_name(OrochiSessionState state)
{
    switch (state)
    {
        case OROCHI_SESSION_ACTIVE:         return "active";
        case OROCHI_SESSION_IDLE:           return "idle";
        case OROCHI_SESSION_EXPIRED:        return "expired";
        case OROCHI_SESSION_REVOKED:        return "revoked";
        case OROCHI_SESSION_MFA_REQUIRED:   return "mfa_required";
        default: return "unknown";
    }
}

/*
 * orochi_auth_audit_event_name
 *    Get audit event name
 */
const char *
orochi_auth_audit_event_name(OrochiAuditEvent event)
{
    switch (event)
    {
        case OROCHI_AUDIT_LOGIN_SUCCESS:        return "login_success";
        case OROCHI_AUDIT_LOGIN_FAILED:         return "login_failed";
        case OROCHI_AUDIT_LOGOUT:               return "logout";
        case OROCHI_AUDIT_TOKEN_ISSUED:         return "token_issued";
        case OROCHI_AUDIT_TOKEN_REVOKED:        return "token_revoked";
        case OROCHI_AUDIT_TOKEN_REFRESHED:      return "token_refreshed";
        case OROCHI_AUDIT_PASSWORD_CHANGED:     return "password_changed";
        case OROCHI_AUDIT_PASSWORD_RESET:       return "password_reset";
        case OROCHI_AUDIT_MFA_ENROLLED:         return "mfa_enrolled";
        case OROCHI_AUDIT_MFA_VERIFIED:         return "mfa_verified";
        case OROCHI_AUDIT_MFA_FAILED:           return "mfa_failed";
        case OROCHI_AUDIT_API_KEY_CREATED:      return "api_key_created";
        case OROCHI_AUDIT_API_KEY_REVOKED:      return "api_key_revoked";
        case OROCHI_AUDIT_USER_CREATED:         return "user_created";
        case OROCHI_AUDIT_USER_UPDATED:         return "user_updated";
        case OROCHI_AUDIT_USER_DELETED:         return "user_deleted";
        case OROCHI_AUDIT_ROLE_ASSIGNED:        return "role_assigned";
        case OROCHI_AUDIT_ROLE_REVOKED:         return "role_revoked";
        case OROCHI_AUDIT_PERMISSION_DENIED:    return "permission_denied";
        case OROCHI_AUDIT_RATE_LIMITED:         return "rate_limited";
        case OROCHI_AUDIT_SUSPICIOUS_ACTIVITY:  return "suspicious_activity";
        case OROCHI_AUDIT_KEY_ROTATED:          return "key_rotated";
        case OROCHI_AUDIT_SESSION_CREATED:      return "session_created";
        case OROCHI_AUDIT_SESSION_EXPIRED:      return "session_expired";
        default: return "unknown";
    }
}

/*
 * orochi_auth_get_stats
 *    Get authentication statistics
 */
void
orochi_auth_get_stats(uint64 *total_requests,
                       uint64 *cache_hits,
                       uint64 *cache_misses,
                       uint64 *failed_auths,
                       uint64 *tokens_issued,
                       uint64 *tokens_revoked)
{
    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
    {
        if (total_requests) *total_requests = 0;
        if (cache_hits) *cache_hits = 0;
        if (cache_misses) *cache_misses = 0;
        if (failed_auths) *failed_auths = 0;
        if (tokens_issued) *tokens_issued = 0;
        if (tokens_revoked) *tokens_revoked = 0;
        return;
    }

    if (total_requests)
        *total_requests = pg_atomic_read_u64(&OrochiAuthSharedState->total_auth_requests);
    if (cache_hits)
        *cache_hits = pg_atomic_read_u64(&OrochiAuthSharedState->cache_hits);
    if (cache_misses)
        *cache_misses = pg_atomic_read_u64(&OrochiAuthSharedState->cache_misses);
    if (failed_auths)
        *failed_auths = pg_atomic_read_u64(&OrochiAuthSharedState->failed_authentications);
    if (tokens_issued)
        *tokens_issued = pg_atomic_read_u64(&OrochiAuthSharedState->tokens_issued);
    if (tokens_revoked)
        *tokens_revoked = pg_atomic_read_u64(&OrochiAuthSharedState->tokens_revoked);
}

/* ============================================================
 * SQL Function Implementations
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_uid);
/*
 * orochi_auth_uid
 *    Returns the current authenticated user ID
 */
Datum
orochi_auth_uid(PG_FUNCTION_ARGS)
{
    char *user_id;

    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        PG_RETURN_NULL();

    PG_RETURN_TEXT_P(cstring_to_text(user_id));
}

PG_FUNCTION_INFO_V1(orochi_auth_role);
/*
 * orochi_auth_role
 *    Returns the current user role
 */
Datum
orochi_auth_role(PG_FUNCTION_ARGS)
{
    char *role;

    role = orochi_auth_get_current_role();
    PG_RETURN_TEXT_P(cstring_to_text(role));
}

PG_FUNCTION_INFO_V1(orochi_auth_stats);
/*
 * orochi_auth_stats
 *    Returns authentication statistics
 */
Datum
orochi_auth_stats(PG_FUNCTION_ARGS)
{
    uint64      total_requests, cache_hits, cache_misses;
    uint64      failed_auths, tokens_issued, tokens_revoked;
    TupleDesc   tupdesc;
    Datum       values[6];
    bool        nulls[6];
    HeapTuple   tuple;

    orochi_auth_get_stats(&total_requests, &cache_hits, &cache_misses,
                           &failed_auths, &tokens_issued, &tokens_revoked);

    /* Build result tuple */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    values[0] = Int64GetDatum(total_requests);
    values[1] = Int64GetDatum(cache_hits);
    values[2] = Int64GetDatum(cache_misses);
    values[3] = Int64GetDatum(failed_auths);
    values[4] = Int64GetDatum(tokens_issued);
    values[5] = Int64GetDatum(tokens_revoked);

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
