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
#include "utils/jsonb.h"
#include "common/sha2.h"
#include "access/xact.h"

/* OpenSSL includes for cryptographic operations */
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/crypto.h>

#include "auth.h"
#include "jwt.h"

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
    OrochiApiKeyInfo     *key_info;
    char                  key_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    TimestampTz           now;

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

    /* Hash full key for verification */
    orochi_auth_hash_token(api_key, key_hash);

    /* Look up API key in database by hash */
    key_info = orochi_auth_lookup_api_key_db(key_hash);

    if (key_info == NULL)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "API_KEY_NOT_FOUND", sizeof(result.error_code));
        strlcpy(result.error_message, "API key not found",
                sizeof(result.error_message));
        return result;
    }

    /* Check if key is revoked */
    if (key_info->is_revoked)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "API_KEY_REVOKED", sizeof(result.error_code));
        strlcpy(result.error_message, "API key has been revoked",
                sizeof(result.error_message));
        pfree(key_info);
        return result;
    }

    /* Check if key has expired */
    now = GetCurrentTimestamp();
    if (key_info->has_expires && key_info->expires_at < now)
    {
        result.is_valid = false;
        strlcpy(result.error_code, "API_KEY_EXPIRED", sizeof(result.error_code));
        strlcpy(result.error_message, "API key has expired",
                sizeof(result.error_message));
        pfree(key_info);
        return result;
    }

    /* API key is valid - populate result */
    result.is_valid = true;
    result.token_type = OROCHI_TOKEN_API_KEY;
    strlcpy(result.user_id, key_info->user_id, sizeof(result.user_id));
    strlcpy(result.tenant_id, key_info->tenant_id, sizeof(result.tenant_id));
    result.issued_at = key_info->created_at;

    if (key_info->has_expires)
        result.expires_at = key_info->expires_at;
    else
        result.expires_at = 0;  /* No expiry */

    /* Update last_used_at timestamp asynchronously */
    orochi_auth_update_api_key_usage(key_hash);

    /* Track auth request in shared memory */
    if (OrochiAuthSharedState != NULL)
    {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->total_auth_requests, 1);
    }

    pfree(key_info);
    return result;
}

/*
 * orochi_auth_parse_jwt_claims
 *    Parse JWT and extract claims into OrochiTokenValidation struct
 *
 * This function:
 *   1. Extracts the payload portion from the JWT
 *   2. Base64URL decodes the payload
 *   3. Parses the JSON using PostgreSQL's JSONB functions
 *   4. Extracts standard JWT claims (sub, iss, aud, exp, iat, jti)
 *   5. Extracts custom claims (tenant_id, session_id, roles, scopes)
 */
bool
orochi_auth_parse_jwt_claims(const char *token, OrochiTokenValidation *result)
{
    const char     *header_end;
    const char     *payload_end;
    char           *payload_b64;
    int             payload_b64_len;
    char           *payload_json;
    Size            payload_json_len;
    Jsonb          *jsonb;
    JsonbValue     *jval;

    /* JWT format: header.payload.signature */
    header_end = strchr(token, '.');
    if (header_end == NULL)
    {
        strlcpy(result->error_code, "INVALID_FORMAT", sizeof(result->error_code));
        strlcpy(result->error_message, "Invalid JWT format: missing first separator",
                sizeof(result->error_message));
        return false;
    }

    payload_end = strchr(header_end + 1, '.');
    if (payload_end == NULL)
    {
        strlcpy(result->error_code, "INVALID_FORMAT", sizeof(result->error_code));
        strlcpy(result->error_message, "Invalid JWT format: missing second separator",
                sizeof(result->error_message));
        return false;
    }

    /* Extract base64url-encoded payload portion */
    payload_b64_len = payload_end - (header_end + 1);
    if (payload_b64_len <= 0 || payload_b64_len > OROCHI_AUTH_JWT_MAX_SIZE)
    {
        strlcpy(result->error_code, "INVALID_PAYLOAD", sizeof(result->error_code));
        strlcpy(result->error_message, "Invalid JWT payload size",
                sizeof(result->error_message));
        return false;
    }

    payload_b64 = palloc(payload_b64_len + 1);
    memcpy(payload_b64, header_end + 1, payload_b64_len);
    payload_b64[payload_b64_len] = '\0';

    /* Base64URL decode the payload */
    payload_json = orochi_base64url_decode(payload_b64, &payload_json_len);
    pfree(payload_b64);

    if (payload_json == NULL || payload_json_len == 0)
    {
        strlcpy(result->error_code, "DECODE_ERROR", sizeof(result->error_code));
        strlcpy(result->error_message, "Failed to base64url decode JWT payload",
                sizeof(result->error_message));
        return false;
    }

    /* Parse JSON to JSONB */
    PG_TRY();
    {
        jsonb = DatumGetJsonbP(
            DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json)));
    }
    PG_CATCH();
    {
        pfree(payload_json);
        strlcpy(result->error_code, "JSON_PARSE_ERROR", sizeof(result->error_code));
        strlcpy(result->error_message, "Failed to parse JWT payload as JSON",
                sizeof(result->error_message));
        FlushErrorState();
        return false;
    }
    PG_END_TRY();

    pfree(payload_json);

    /* Initialize result with defaults */
    strlcpy(result->user_id, "", sizeof(result->user_id));
    strlcpy(result->tenant_id, "", sizeof(result->tenant_id));
    strlcpy(result->session_id, "", sizeof(result->session_id));
    result->expires_at = 0;
    result->scopes = 0;
    result->role_count = 0;
    result->is_expired = false;
    result->is_revoked = false;

    /*
     * Extract standard JWT claims
     */

    /* sub (subject) -> user_id */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "sub", 3, NULL);
    if (jval != NULL && jval->type == jbvString)
    {
        int copy_len = Min(jval->val.string.len, OROCHI_AUTH_USER_ID_SIZE);
        memcpy(result->user_id, jval->val.string.val, copy_len);
        result->user_id[copy_len] = '\0';
    }

    /* exp (expiration) -> expires_at */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "exp", 3, NULL);
    if (jval != NULL && jval->type == jbvNumeric)
    {
        int64 exp_unix = DatumGetInt64(
            DirectFunctionCall1(numeric_int8, NumericGetDatum(jval->val.numeric)));
        result->expires_at = orochi_unix_to_timestamptz(exp_unix);
    }
    else
    {
        /* No expiration claim - set a default */
        result->expires_at = GetCurrentTimestamp() +
            (orochi_auth_access_token_ttl * USECS_PER_SEC);
    }

    /* iat (issued at) - optional, not stored in result but could be validated */

    /* jti (JWT ID) - optional, could be used for revocation tracking */

    /*
     * Extract custom/application-specific claims
     */

    /* tenant_id - custom claim for multi-tenancy */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "tenant_id", 9, NULL);
    if (jval != NULL && jval->type == jbvString)
    {
        int copy_len = Min(jval->val.string.len, OROCHI_AUTH_TENANT_ID_SIZE);
        memcpy(result->tenant_id, jval->val.string.val, copy_len);
        result->tenant_id[copy_len] = '\0';
    }

    /* session_id - custom claim for session tracking */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "session_id", 10, NULL);
    if (jval != NULL && jval->type == jbvString)
    {
        int copy_len = Min(jval->val.string.len, OROCHI_AUTH_SESSION_ID_SIZE);
        memcpy(result->session_id, jval->val.string.val, copy_len);
        result->session_id[copy_len] = '\0';
    }

    /* scopes - bitmask of permissions */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "scopes", 6, NULL);
    if (jval != NULL && jval->type == jbvNumeric)
    {
        result->scopes = DatumGetInt64(
            DirectFunctionCall1(numeric_int8, NumericGetDatum(jval->val.numeric)));
    }

    /*
     * Extract roles - can be a single string or an array
     */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "role", 4, NULL);
    if (jval != NULL && jval->type == jbvString && result->role_count < OROCHI_AUTH_MAX_ROLES)
    {
        /* Single role as string */
        int copy_len = Min(jval->val.string.len, OROCHI_AUTH_ROLE_NAME_SIZE);
        memcpy(result->roles[0], jval->val.string.val, copy_len);
        result->roles[0][copy_len] = '\0';
        result->role_count = 1;
    }

    /* Also check for "roles" array */
    jval = getKeyJsonValueFromContainer(&jsonb->root, "roles", 5, NULL);
    if (jval != NULL && jval->type == jbvBinary)
    {
        /* roles is an array - iterate through it */
        JsonbIterator  *it;
        JsonbValue      elem;
        JsonbIteratorToken r;

        it = JsonbIteratorInit((JsonbContainer *) jval->val.binary.data);

        while ((r = JsonbIteratorNext(&it, &elem, true)) != WJB_DONE)
        {
            if (r == WJB_ELEM && elem.type == jbvString &&
                result->role_count < OROCHI_AUTH_MAX_ROLES)
            {
                int copy_len = Min(elem.val.string.len, OROCHI_AUTH_ROLE_NAME_SIZE);
                memcpy(result->roles[result->role_count], elem.val.string.val, copy_len);
                result->roles[result->role_count][copy_len] = '\0';
                result->role_count++;
            }
        }
    }

    /*
     * Extract app_metadata.tenant_id as fallback for tenant_id
     * This follows Supabase's pattern where tenant info may be in app_metadata
     */
    if (result->tenant_id[0] == '\0')
    {
        jval = getKeyJsonValueFromContainer(&jsonb->root, "app_metadata", 12, NULL);
        if (jval != NULL && jval->type == jbvBinary)
        {
            JsonbValue *tenant_val = getKeyJsonValueFromContainer(
                (JsonbContainer *) jval->val.binary.data, "tenant_id", 9, NULL);
            if (tenant_val != NULL && tenant_val->type == jbvString)
            {
                int copy_len = Min(tenant_val->val.string.len, OROCHI_AUTH_TENANT_ID_SIZE);
                memcpy(result->tenant_id, tenant_val->val.string.val, copy_len);
                result->tenant_id[copy_len] = '\0';
            }
        }
    }

    /*
     * Extract email for audit/logging purposes
     * Some systems use email as the user identifier
     */
    if (result->user_id[0] == '\0')
    {
        jval = getKeyJsonValueFromContainer(&jsonb->root, "email", 5, NULL);
        if (jval != NULL && jval->type == jbvString)
        {
            int copy_len = Min(jval->val.string.len, OROCHI_AUTH_USER_ID_SIZE);
            memcpy(result->user_id, jval->val.string.val, copy_len);
            result->user_id[copy_len] = '\0';
        }
    }

    return true;
}

/*
 * Base64URL decode table for JWT signature verification
 */
static const int8 auth_base64url_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/*
 * auth_base64url_decode_internal
 *    Decode a Base64URL encoded string for signature verification
 */
static unsigned char *
auth_base64url_decode_internal(const char *input, Size *output_len)
{
    Size input_len;
    Size decoded_len;
    unsigned char *output;
    Size i, j;
    uint32 buffer;
    int bits;

    if (input == NULL || output_len == NULL)
        return NULL;

    input_len = strlen(input);
    if (input_len == 0)
    {
        *output_len = 0;
        return palloc(1);  /* Return empty buffer */
    }

    /* Calculate output size (generous estimate) */
    decoded_len = ((input_len + 3) * 3) / 4;
    output = palloc(decoded_len + 1);

    buffer = 0;
    bits = 0;
    j = 0;

    for (i = 0; i < input_len; i++)
    {
        int8 val = auth_base64url_decode_table[(unsigned char)input[i]];

        if (val < 0)
        {
            /* Skip padding and whitespace */
            if (input[i] == '=' || input[i] == '\n' || input[i] == '\r')
                continue;

            pfree(output);
            *output_len = 0;
            return NULL;
        }

        buffer = (buffer << 6) | val;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            output[j++] = (unsigned char)((buffer >> bits) & 0xFF);
        }
    }

    *output_len = j;
    return output;
}

/*
 * get_evp_md_for_algorithm
 *    Get the OpenSSL EVP_MD for a given JWT algorithm
 */
static const EVP_MD *
get_evp_md_for_algorithm(OrochiJwtAlgorithm alg)
{
    switch (alg)
    {
        case OROCHI_JWT_ALG_HS256:
        case OROCHI_JWT_ALG_RS256:
        case OROCHI_JWT_ALG_ES256:
            return EVP_sha256();
        case OROCHI_JWT_ALG_HS384:
        case OROCHI_JWT_ALG_RS384:
        case OROCHI_JWT_ALG_ES384:
            return EVP_sha384();
        case OROCHI_JWT_ALG_HS512:
        case OROCHI_JWT_ALG_RS512:
        case OROCHI_JWT_ALG_ES512:
            return EVP_sha512();
        default:
            return NULL;
    }
}

/*
 * verify_hmac_signature_internal
 *    Verify HMAC-SHA signature (HS256/HS384/HS512)
 *
 * For symmetric algorithms (HMAC), we compute the expected signature
 * using the shared secret and compare it to the provided signature
 * using constant-time comparison to prevent timing attacks.
 */
static bool
verify_hmac_signature_internal(const char *data, Size data_len,
                                const unsigned char *signature, Size sig_len,
                                const char *secret, Size secret_len,
                                OrochiJwtAlgorithm alg)
{
    const EVP_MD *md;
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;

    md = get_evp_md_for_algorithm(alg);
    if (md == NULL)
        return false;

    /* Compute HMAC */
    if (HMAC(md,
             secret, (int)secret_len,
             (unsigned char *)data, data_len,
             hmac_result, &hmac_len) == NULL)
    {
        elog(DEBUG1, "HMAC computation failed");
        return false;
    }

    /* Constant-time comparison to prevent timing attacks */
    if (sig_len != hmac_len)
    {
        elog(DEBUG1, "HMAC signature length mismatch: expected %u, got %zu",
             hmac_len, sig_len);
        return false;
    }

    return CRYPTO_memcmp(hmac_result, signature, hmac_len) == 0;
}

/*
 * verify_rsa_signature_internal
 *    Verify RSA-SHA signature (RS256/RS384/RS512)
 *
 * RSA signatures use asymmetric cryptography. The signing key's public_key
 * field contains the PEM-encoded RSA public key used for verification.
 */
static bool
verify_rsa_signature_internal(const char *data, Size data_len,
                               const unsigned char *signature, Size sig_len,
                               const char *public_key_pem,
                               OrochiJwtAlgorithm alg)
{
    BIO *bio = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    const EVP_MD *md;
    bool result = false;
    int verify_result;

    if (public_key_pem == NULL || strlen(public_key_pem) == 0)
    {
        elog(DEBUG1, "RSA verification failed: no public key provided");
        return false;
    }

    md = get_evp_md_for_algorithm(alg);
    if (md == NULL)
    {
        elog(DEBUG1, "RSA verification failed: unsupported algorithm");
        return false;
    }

    /* Create BIO from public key PEM string */
    bio = BIO_new_mem_buf(public_key_pem, -1);
    if (bio == NULL)
    {
        elog(DEBUG1, "RSA verification failed: BIO creation failed");
        goto cleanup;
    }

    /* Read public key - try generic PUBKEY format first */
    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    if (pkey == NULL)
    {
        /* Try reading as RSA public key specifically */
        RSA *rsa;

        BIO_reset(bio);
        rsa = PEM_read_bio_RSAPublicKey(bio, NULL, NULL, NULL);
        if (rsa != NULL)
        {
            pkey = EVP_PKEY_new();
            if (pkey != NULL)
                EVP_PKEY_assign_RSA(pkey, rsa);
            else
                RSA_free(rsa);
        }
    }

    if (pkey == NULL)
    {
        elog(DEBUG1, "RSA verification failed: could not parse public key");
        goto cleanup;
    }

    /* Verify the key is RSA type */
    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA)
    {
        elog(DEBUG1, "RSA verification failed: key is not RSA type");
        goto cleanup;
    }

    /* Create verification context */
    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL)
    {
        elog(DEBUG1, "RSA verification failed: context creation failed");
        goto cleanup;
    }

    /* Initialize verification */
    if (EVP_DigestVerifyInit(md_ctx, NULL, md, NULL, pkey) != 1)
    {
        elog(DEBUG1, "RSA verification failed: DigestVerifyInit failed");
        goto cleanup;
    }

    /* Update with data to verify */
    if (EVP_DigestVerifyUpdate(md_ctx, data, data_len) != 1)
    {
        elog(DEBUG1, "RSA verification failed: DigestVerifyUpdate failed");
        goto cleanup;
    }

    /* Verify signature */
    verify_result = EVP_DigestVerifyFinal(md_ctx, signature, sig_len);
    result = (verify_result == 1);

    if (!result)
        elog(DEBUG1, "RSA signature verification returned: %d", verify_result);

cleanup:
    if (md_ctx != NULL)
        EVP_MD_CTX_free(md_ctx);
    if (pkey != NULL)
        EVP_PKEY_free(pkey);
    if (bio != NULL)
        BIO_free(bio);

    return result;
}

/*
 * convert_ecdsa_jwt_sig_to_der
 *    Convert ECDSA signature from JWT format (r||s) to DER format
 *
 * JWT uses a simple concatenation of r and s values (each padded to the
 * curve size), while OpenSSL's EVP_DigestVerify expects DER-encoded
 * signatures. This function converts between the two formats.
 */
static unsigned char *
convert_ecdsa_jwt_sig_to_der(const unsigned char *jwt_sig, Size jwt_sig_len,
                              Size *der_len, OrochiJwtAlgorithm alg)
{
    ECDSA_SIG *ecdsa_sig = NULL;
    BIGNUM *r = NULL, *s = NULL;
    unsigned char *der_sig = NULL;
    unsigned char *p;
    int expected_component_len;
    int actual_der_len;

    /* Determine expected component length based on algorithm/curve */
    switch (alg)
    {
        case OROCHI_JWT_ALG_ES256:
            expected_component_len = 32;  /* P-256: 256 bits = 32 bytes */
            break;
        case OROCHI_JWT_ALG_ES384:
            expected_component_len = 48;  /* P-384: 384 bits = 48 bytes */
            break;
        case OROCHI_JWT_ALG_ES512:
            expected_component_len = 66;  /* P-521: 521 bits = 66 bytes */
            break;
        default:
            return NULL;
    }

    /* JWT ECDSA signature is r||s concatenation */
    if (jwt_sig_len != (Size)(expected_component_len * 2))
    {
        elog(DEBUG1, "ECDSA signature wrong length: expected %d, got %zu",
             expected_component_len * 2, jwt_sig_len);
        return NULL;
    }

    /* Create ECDSA signature structure */
    ecdsa_sig = ECDSA_SIG_new();
    if (ecdsa_sig == NULL)
        return NULL;

    /* Convert r and s from big-endian bytes to BIGNUMs */
    r = BN_bin2bn(jwt_sig, expected_component_len, NULL);
    s = BN_bin2bn(jwt_sig + expected_component_len, expected_component_len, NULL);

    if (r == NULL || s == NULL)
    {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(ecdsa_sig);
        return NULL;
    }

    /* Set r and s in the signature structure (transfers ownership) */
    if (ECDSA_SIG_set0(ecdsa_sig, r, s) != 1)
    {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(ecdsa_sig);
        return NULL;
    }

    /* Convert to DER format - first get the required size */
    actual_der_len = i2d_ECDSA_SIG(ecdsa_sig, NULL);
    if (actual_der_len <= 0)
    {
        ECDSA_SIG_free(ecdsa_sig);
        return NULL;
    }

    der_sig = palloc(actual_der_len);
    p = der_sig;

    actual_der_len = i2d_ECDSA_SIG(ecdsa_sig, &p);
    ECDSA_SIG_free(ecdsa_sig);

    if (actual_der_len <= 0)
    {
        pfree(der_sig);
        return NULL;
    }

    *der_len = actual_der_len;
    return der_sig;
}

/*
 * verify_ecdsa_signature_internal
 *    Verify ECDSA-SHA signature (ES256/ES384/ES512)
 *
 * ECDSA signatures use elliptic curve cryptography. The signing key's
 * public_key field contains the PEM-encoded EC public key. JWT uses
 * a different signature format than OpenSSL, so we must convert the
 * signature before verification.
 */
static bool
verify_ecdsa_signature_internal(const char *data, Size data_len,
                                 const unsigned char *signature, Size sig_len,
                                 const char *public_key_pem,
                                 OrochiJwtAlgorithm alg)
{
    BIO *bio = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    const EVP_MD *md;
    unsigned char *der_sig = NULL;
    Size der_sig_len;
    bool result = false;
    int verify_result;

    if (public_key_pem == NULL || strlen(public_key_pem) == 0)
    {
        elog(DEBUG1, "ECDSA verification failed: no public key provided");
        return false;
    }

    md = get_evp_md_for_algorithm(alg);
    if (md == NULL)
    {
        elog(DEBUG1, "ECDSA verification failed: unsupported algorithm");
        return false;
    }

    /* Convert JWT signature format (r||s) to DER format */
    der_sig = convert_ecdsa_jwt_sig_to_der(signature, sig_len, &der_sig_len, alg);
    if (der_sig == NULL)
    {
        elog(DEBUG1, "ECDSA verification failed: signature conversion failed");
        return false;
    }

    /* Create BIO from public key PEM string */
    bio = BIO_new_mem_buf(public_key_pem, -1);
    if (bio == NULL)
    {
        elog(DEBUG1, "ECDSA verification failed: BIO creation failed");
        goto cleanup;
    }

    /* Read public key - try generic PUBKEY format first */
    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    if (pkey == NULL)
    {
        /* Try reading as EC public key specifically */
        EC_KEY *ec;

        BIO_reset(bio);
        ec = PEM_read_bio_EC_PUBKEY(bio, NULL, NULL, NULL);
        if (ec != NULL)
        {
            pkey = EVP_PKEY_new();
            if (pkey != NULL)
                EVP_PKEY_assign_EC_KEY(pkey, ec);
            else
                EC_KEY_free(ec);
        }
    }

    if (pkey == NULL)
    {
        elog(DEBUG1, "ECDSA verification failed: could not parse public key");
        goto cleanup;
    }

    /* Verify the key is EC type */
    if (EVP_PKEY_base_id(pkey) != EVP_PKEY_EC)
    {
        elog(DEBUG1, "ECDSA verification failed: key is not EC type");
        goto cleanup;
    }

    /* Create verification context */
    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL)
    {
        elog(DEBUG1, "ECDSA verification failed: context creation failed");
        goto cleanup;
    }

    /* Initialize verification */
    if (EVP_DigestVerifyInit(md_ctx, NULL, md, NULL, pkey) != 1)
    {
        elog(DEBUG1, "ECDSA verification failed: DigestVerifyInit failed");
        goto cleanup;
    }

    /* Update with data to verify */
    if (EVP_DigestVerifyUpdate(md_ctx, data, data_len) != 1)
    {
        elog(DEBUG1, "ECDSA verification failed: DigestVerifyUpdate failed");
        goto cleanup;
    }

    /* Verify signature (using DER format) */
    verify_result = EVP_DigestVerifyFinal(md_ctx, der_sig, der_sig_len);
    result = (verify_result == 1);

    if (!result)
        elog(DEBUG1, "ECDSA signature verification returned: %d", verify_result);

cleanup:
    if (der_sig != NULL)
        pfree(der_sig);
    if (md_ctx != NULL)
        EVP_MD_CTX_free(md_ctx);
    if (pkey != NULL)
        EVP_PKEY_free(pkey);
    if (bio != NULL)
        BIO_free(bio);

    return result;
}

/*
 * parse_jwt_header_algorithm
 *    Extract algorithm from JWT header
 *
 * This parses the JWT header to determine what signing algorithm was used.
 * The algorithm in the token header must match the key's algorithm for
 * verification to succeed.
 */
static OrochiJwtAlgorithm
parse_jwt_header_algorithm(const char *token)
{
    const char *first_dot;
    char *header_b64;
    unsigned char *header_json;
    Size header_len;
    OrochiJwtAlgorithm alg = OROCHI_JWT_ALG_RS256;  /* Default */
    Jsonb *jsonb;
    JsonbValue *alg_val;

    /* Find first dot separating header from payload */
    first_dot = strchr(token, '.');
    if (first_dot == NULL)
        return alg;

    /* Extract base64url-encoded header */
    header_b64 = pnstrdup(token, first_dot - token);

    /* Decode header */
    header_json = auth_base64url_decode_internal(header_b64, &header_len);
    pfree(header_b64);

    if (header_json == NULL)
        return alg;

    /* Parse JSON to extract algorithm */
    PG_TRY();
    {
        jsonb = DatumGetJsonbP(
            DirectFunctionCall1(jsonb_in, CStringGetDatum(header_json)));

        alg_val = getKeyJsonValueFromContainer(&jsonb->root, "alg", 3, NULL);
        if (alg_val != NULL && alg_val->type == jbvString)
        {
            char *alg_str = pnstrdup(alg_val->val.string.val, alg_val->val.string.len);

            /* Map algorithm string to enum */
            if (strcmp(alg_str, "HS256") == 0)
                alg = OROCHI_JWT_ALG_HS256;
            else if (strcmp(alg_str, "HS384") == 0)
                alg = OROCHI_JWT_ALG_HS384;
            else if (strcmp(alg_str, "HS512") == 0)
                alg = OROCHI_JWT_ALG_HS512;
            else if (strcmp(alg_str, "RS256") == 0)
                alg = OROCHI_JWT_ALG_RS256;
            else if (strcmp(alg_str, "RS384") == 0)
                alg = OROCHI_JWT_ALG_RS384;
            else if (strcmp(alg_str, "RS512") == 0)
                alg = OROCHI_JWT_ALG_RS512;
            else if (strcmp(alg_str, "ES256") == 0)
                alg = OROCHI_JWT_ALG_ES256;
            else if (strcmp(alg_str, "ES384") == 0)
                alg = OROCHI_JWT_ALG_ES384;
            else if (strcmp(alg_str, "ES512") == 0)
                alg = OROCHI_JWT_ALG_ES512;

            pfree(alg_str);
        }
    }
    PG_CATCH();
    {
        /* Ignore parse errors, return default algorithm */
        FlushErrorState();
    }
    PG_END_TRY();

    pfree(header_json);
    return alg;
}

/*
 * orochi_auth_verify_jwt_signature
 *    Verify JWT signature using signing key
 *
 * This function implements full JWT signature verification supporting:
 *   - HS256/HS384/HS512: HMAC-SHA symmetric signature verification
 *   - RS256/RS384/RS512: RSA-SHA asymmetric signature verification
 *   - ES256/ES384/ES512: ECDSA-SHA asymmetric signature verification
 *
 * The function:
 *   1. Parses the JWT header to extract the algorithm
 *   2. Verifies the algorithm matches the signing key's expected algorithm
 *   3. Extracts the header.payload portion (the data that was signed)
 *   4. Decodes the Base64URL signature
 *   5. Performs cryptographic verification based on the algorithm type
 *
 * For HMAC algorithms, the key's public_key field contains the shared secret.
 * For RSA/ECDSA algorithms, the key's public_key field contains the PEM-encoded
 * public key.
 *
 * Returns true if the signature is valid, false otherwise.
 */
bool
orochi_auth_verify_jwt_signature(const char *token, OrochiSigningKey *key)
{
    const char *first_dot;
    const char *second_dot;
    char *header_payload;
    char *signature_b64;
    unsigned char *signature;
    Size sig_len;
    Size header_payload_len;
    OrochiJwtAlgorithm token_alg;
    bool result = false;

    if (token == NULL || key == NULL)
        return false;

    /* Find the two dots separating header.payload.signature */
    first_dot = strchr(token, '.');
    if (first_dot == NULL)
    {
        elog(DEBUG1, "JWT verification failed: invalid format (no first dot)");
        return false;
    }

    second_dot = strchr(first_dot + 1, '.');
    if (second_dot == NULL)
    {
        elog(DEBUG1, "JWT verification failed: invalid format (no second dot)");
        return false;
    }

    /* Parse algorithm from JWT header */
    token_alg = parse_jwt_header_algorithm(token);

    /*
     * Verify algorithm matches key's expected algorithm.
     * This prevents algorithm substitution attacks where an attacker
     * might try to use a different (weaker) algorithm.
     */
    if (token_alg != key->algorithm)
    {
        elog(DEBUG1, "JWT verification failed: algorithm mismatch (token: %d, key: %d)",
             token_alg, key->algorithm);
        return false;
    }

    /* Extract header.payload portion (everything before last dot) */
    header_payload_len = second_dot - token;
    header_payload = pnstrdup(token, header_payload_len);

    /* Extract base64url-encoded signature */
    signature_b64 = pstrdup(second_dot + 1);

    /* Decode signature from Base64URL */
    signature = auth_base64url_decode_internal(signature_b64, &sig_len);
    if (signature == NULL)
    {
        elog(DEBUG1, "JWT verification failed: could not decode signature");
        pfree(header_payload);
        pfree(signature_b64);
        return false;
    }

    /* Verify signature based on algorithm type */
    switch (key->algorithm)
    {
        case OROCHI_JWT_ALG_HS256:
        case OROCHI_JWT_ALG_HS384:
        case OROCHI_JWT_ALG_HS512:
            /*
             * For HMAC algorithms, the key's public_key field contains
             * the shared secret. In a production system with encrypted
             * private keys, the secret would be decrypted here.
             */
            if (key->public_key[0] == '\0')
            {
                elog(DEBUG1, "HMAC verification failed: no secret key provided");
                result = false;
            }
            else
            {
                result = verify_hmac_signature_internal(
                    header_payload, header_payload_len,
                    signature, sig_len,
                    key->public_key, strlen(key->public_key),
                    key->algorithm);
            }
            break;

        case OROCHI_JWT_ALG_RS256:
        case OROCHI_JWT_ALG_RS384:
        case OROCHI_JWT_ALG_RS512:
            /*
             * RSA signature verification using the public key.
             * The public_key field should contain a PEM-encoded RSA public key.
             */
            result = verify_rsa_signature_internal(
                header_payload, header_payload_len,
                signature, sig_len,
                key->public_key,
                key->algorithm);
            break;

        case OROCHI_JWT_ALG_ES256:
        case OROCHI_JWT_ALG_ES384:
        case OROCHI_JWT_ALG_ES512:
            /*
             * ECDSA signature verification using the public key.
             * The public_key field should contain a PEM-encoded EC public key.
             * Note: JWT uses a different signature format than OpenSSL,
             * so the signature is converted before verification.
             */
            result = verify_ecdsa_signature_internal(
                header_payload, header_payload_len,
                signature, sig_len,
                key->public_key,
                key->algorithm);
            break;

        default:
            elog(DEBUG1, "JWT verification failed: unsupported algorithm %d",
                 key->algorithm);
            result = false;
            break;
    }

    /* Cleanup */
    pfree(header_payload);
    pfree(signature_b64);
    pfree(signature);

    return result;
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
