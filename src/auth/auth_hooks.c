/*-------------------------------------------------------------------------
 *
 * auth_hooks.c
 *    Orochi DB Authentication System - PostgreSQL Hooks Implementation
 *
 * This file implements:
 *   - ClientAuthentication_hook for custom authentication
 *   - Connection context setup
 *   - API key authentication via connection options
 *   - JWT token extraction from connection parameters
 *   - Background worker registration
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/builtins.h"

#include "auth.h"

/* ============================================================
 * Static Variables
 * ============================================================ */

/* Previous hooks for chaining */
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ============================================================
 * Forward Declarations
 * ============================================================ */

static void orochi_auth_shmem_startup_hook(void);
static void orochi_auth_register_background_workers(void);

/* ============================================================
 * Client Authentication Hook
 * ============================================================ */

/*
 * orochi_auth_client_authentication_hook
 *    Intercept client authentication for custom methods
 *
 * This hook is called after PostgreSQL's standard authentication
 * has completed. We use it to:
 *   1. Extract and validate API keys from connection options
 *   2. Extract and validate JWT tokens from connection options
 *   3. Set up session context variables for RLS
 *   4. Enforce rate limiting on authentication attempts
 */
void
orochi_auth_client_authentication_hook(Port *port, int status)
{
    const char *api_key = NULL;
    const char *jwt_token = NULL;
    const char *tenant_id = NULL;
    OrochiTokenValidation validation;
    char        ip_address[OROCHI_AUTH_IP_SIZE + 1];
    char        user_agent[OROCHI_AUTH_USER_AGENT_SIZE + 1];

    /* Call previous hook if exists */
    if (prev_client_auth_hook)
        prev_client_auth_hook(port, status);

    /* Only process if auth is enabled */
    if (!orochi_auth_enabled)
        return;

    /* Only process successful authentications */
    if (status != STATUS_OK)
        return;

    /* Extract client IP address */
    ip_address[0] = '\0';
    if (port->raddr.addr.ss_family == AF_INET)
    {
        struct sockaddr_in *addr = (struct sockaddr_in *) &port->raddr.addr;
        inet_ntop(AF_INET, &addr->sin_addr, ip_address, sizeof(ip_address));
    }
    else if (port->raddr.addr.ss_family == AF_INET6)
    {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *) &port->raddr.addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ip_address, sizeof(ip_address));
    }

    /* No user agent in PostgreSQL protocol, but could be in options */
    user_agent[0] = '\0';

    /* Check for rate limiting on login attempts */
    if (orochi_auth_rate_limit_enabled)
    {
        if (!orochi_auth_check_rate_limit(ip_address, "login", orochi_auth_rate_limit_login))
        {
            ereport(FATAL,
                    (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                     errmsg("too many authentication attempts"),
                     errdetail("Rate limit exceeded for IP address %s", ip_address),
                     errhint("Please wait before trying again")));
        }
    }

    /*
     * Extract authentication tokens from connection options.
     * These are passed via the 'options' connection parameter:
     *   -c orochi.auth_token=<jwt>
     *   -c orochi.api_key=<key>
     *   -c orochi.tenant_id=<tenant>
     */
    jwt_token = GetConfigOption("orochi.auth_token", true, false);
    api_key = GetConfigOption("orochi.api_key", true, false);
    tenant_id = GetConfigOption("orochi.tenant_id", true, false);

    /* Try JWT token authentication first */
    if (jwt_token != NULL && strlen(jwt_token) > 0)
    {
        validation = orochi_auth_validate_token(jwt_token, tenant_id);

        if (!validation.is_valid)
        {
            /* Log failed authentication */
            orochi_auth_audit_log(tenant_id, NULL, NULL,
                                   OROCHI_AUDIT_LOGIN_FAILED,
                                   ip_address, user_agent,
                                   "jwt", NULL);

            ereport(FATAL,
                    (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                     errmsg("JWT token validation failed: %s", validation.error_message),
                     errdetail("Error code: %s", validation.error_code)));
        }

        /* Set session context from validated token */
        orochi_auth_set_session_context(&validation);

        /* Log successful authentication */
        orochi_auth_audit_log(validation.tenant_id, validation.user_id,
                               validation.session_id,
                               OROCHI_AUDIT_LOGIN_SUCCESS,
                               ip_address, user_agent,
                               "jwt", NULL);

        return;
    }

    /* Try API key authentication */
    if (api_key != NULL && strlen(api_key) > 0)
    {
        validation = orochi_auth_validate_api_key(api_key);

        if (!validation.is_valid)
        {
            /* Log failed authentication */
            orochi_auth_audit_log(tenant_id, NULL, NULL,
                                   OROCHI_AUDIT_LOGIN_FAILED,
                                   ip_address, user_agent,
                                   "api_key", NULL);

            ereport(FATAL,
                    (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                     errmsg("API key validation failed: %s", validation.error_message),
                     errdetail("Error code: %s", validation.error_code)));
        }

        /* Set session context from validated API key */
        orochi_auth_set_session_context(&validation);

        /* Log successful authentication */
        orochi_auth_audit_log(validation.tenant_id, validation.user_id,
                               validation.session_id,
                               OROCHI_AUDIT_LOGIN_SUCCESS,
                               ip_address, user_agent,
                               "api_key", NULL);

        return;
    }

    /*
     * No Orochi-specific authentication provided.
     * Connection proceeds with standard PostgreSQL auth only.
     * Set default anonymous context.
     */
    orochi_auth_clear_session_context();

    /* If tenant_id is provided, set it even for anonymous access */
    if (tenant_id != NULL && strlen(tenant_id) > 0)
    {
        SetConfigOption("orochi.auth_tenant_id", tenant_id,
                        PGC_USERSET, PGC_S_SESSION);
    }
}

/* ============================================================
 * Shared Memory Startup Hook
 * ============================================================ */

/*
 * orochi_auth_shmem_startup_hook
 *    Initialize auth shared memory during startup
 */
static void
orochi_auth_shmem_startup_hook(void)
{
    /* Call previous hook if exists */
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Initialize auth shared memory */
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    orochi_auth_shmem_init();
    LWLockRelease(AddinShmemInitLock);
}

/* ============================================================
 * Background Worker Registration
 * ============================================================ */

/*
 * orochi_auth_register_background_workers
 *    Register auth background workers
 */
static void
orochi_auth_register_background_workers(void)
{
    BackgroundWorker worker;

    /* Token cleanup worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi auth token cleanup");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi auth");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_auth_token_cleanup_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Session management worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi auth session manager");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi auth");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_auth_session_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Key rotation worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi auth key rotation");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi auth");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 3600;  /* Restart after 1 hour on crash */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_auth_key_rotation_worker_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    elog(LOG, "Orochi Auth background workers registered");
}

/* ============================================================
 * Hook Installation/Uninstallation
 * ============================================================ */

/*
 * orochi_auth_install_hooks
 *    Install authentication hooks
 *
 * This should be called from _PG_init() or orochi_init()
 */
void
orochi_auth_install_hooks(void)
{
    /* Install ClientAuthentication hook */
    prev_client_auth_hook = ClientAuthentication_hook;
    ClientAuthentication_hook = orochi_auth_client_authentication_hook;

    /*
     * Note: Shared memory and background workers need to be set up
     * during shared_preload_libraries time. Check if we're in that phase.
     */
    if (process_shared_preload_libraries_in_progress)
    {
        /* Request shared memory for auth cache */
        RequestAddinShmemSpace(orochi_auth_shmem_size());

        /* Request LWLock tranche */
        RequestNamedLWLockTranche(OROCHI_AUTH_LWLOCK_TRANCHE_NAME,
                                  OROCHI_AUTH_LWLOCK_COUNT);

        /* Install shared memory startup hook */
        prev_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = orochi_auth_shmem_startup_hook;

        /* Register background workers */
        orochi_auth_register_background_workers();
    }

    elog(LOG, "Orochi Auth hooks installed");
}

/*
 * orochi_auth_uninstall_hooks
 *    Uninstall authentication hooks
 *
 * This should be called from _PG_fini()
 */
void
orochi_auth_uninstall_hooks(void)
{
    /* Restore previous hooks */
    ClientAuthentication_hook = prev_client_auth_hook;
    shmem_startup_hook = prev_shmem_startup_hook;

    elog(LOG, "Orochi Auth hooks uninstalled");
}

/* ============================================================
 * Connection String Authentication
 * ============================================================ */

/*
 * orochi_auth_parse_connection_string
 *    Parse Orochi-style connection string for authentication
 *
 * Connection string format:
 *   postgres://[user].[tenant_id]:[password_or_token]@host:port/database
 *
 * The user component can include:
 *   - user.tenant_id (tenant separation)
 *   - user.tenant_id.pooler_mode (for connection pooling)
 *
 * This function extracts these components for authentication.
 */
typedef struct OrochiConnectionInfo
{
    char    user[64];
    char    tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char    pooler_mode[32];
    char    password[256];
    bool    is_valid;
} OrochiConnectionInfo;

static OrochiConnectionInfo
orochi_auth_parse_connection_user(const char *user, const char *password)
{
    OrochiConnectionInfo info;
    const char          *dot1;
    const char          *dot2;
    int                  user_len;

    memset(&info, 0, sizeof(OrochiConnectionInfo));
    info.is_valid = true;

    if (user == NULL || strlen(user) == 0)
    {
        info.is_valid = false;
        return info;
    }

    /* Look for dots in username */
    dot1 = strchr(user, '.');
    if (dot1 == NULL)
    {
        /* Simple username, no tenant */
        strlcpy(info.user, user, sizeof(info.user));
    }
    else
    {
        /* Extract user part */
        user_len = dot1 - user;
        if (user_len >= (int) sizeof(info.user))
            user_len = sizeof(info.user) - 1;
        memcpy(info.user, user, user_len);
        info.user[user_len] = '\0';

        /* Look for second dot (pooler mode) */
        dot2 = strchr(dot1 + 1, '.');
        if (dot2 == NULL)
        {
            /* Only tenant_id after first dot */
            strlcpy(info.tenant_id, dot1 + 1, sizeof(info.tenant_id));
        }
        else
        {
            /* tenant_id.pooler_mode */
            int tenant_len = dot2 - (dot1 + 1);
            if (tenant_len >= (int) sizeof(info.tenant_id))
                tenant_len = sizeof(info.tenant_id) - 1;
            memcpy(info.tenant_id, dot1 + 1, tenant_len);
            info.tenant_id[tenant_len] = '\0';

            strlcpy(info.pooler_mode, dot2 + 1, sizeof(info.pooler_mode));
        }
    }

    /* Copy password/token */
    if (password != NULL)
    {
        strlcpy(info.password, password, sizeof(info.password));
    }

    return info;
}

/* ============================================================
 * Utility Functions for Hooks
 * ============================================================ */

/*
 * orochi_auth_extract_bearer_token
 *    Extract Bearer token from Authorization header-style string
 */
static const char *
orochi_auth_extract_bearer_token(const char *auth_header)
{
    if (auth_header == NULL)
        return NULL;

    /* Skip "Bearer " prefix if present */
    if (strncasecmp(auth_header, "Bearer ", 7) == 0)
        return auth_header + 7;

    return auth_header;
}

/*
 * orochi_auth_is_api_key_format
 *    Check if string looks like an API key (prefix-based)
 */
static bool
orochi_auth_is_api_key_format(const char *token)
{
    if (token == NULL || strlen(token) < OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1)
        return false;

    /*
     * API keys typically have a recognizable prefix format:
     *   - orochi_key_xxxxx
     *   - sk_live_xxxxx
     *   - pk_test_xxxxx
     */
    if (strncmp(token, "orochi_", 7) == 0 ||
        strncmp(token, "sk_", 3) == 0 ||
        strncmp(token, "pk_", 3) == 0)
    {
        return true;
    }

    return false;
}

/*
 * orochi_auth_is_jwt_format
 *    Check if string looks like a JWT (has two dots)
 */
static bool
orochi_auth_is_jwt_format(const char *token)
{
    const char *dot1;
    const char *dot2;

    if (token == NULL || strlen(token) < 10)
        return false;

    dot1 = strchr(token, '.');
    if (dot1 == NULL)
        return false;

    dot2 = strchr(dot1 + 1, '.');
    if (dot2 == NULL)
        return false;

    /* JWT should only have exactly 2 dots */
    if (strchr(dot2 + 1, '.') != NULL)
        return false;

    return true;
}

/* ============================================================
 * Additional SQL Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_jwt);
/*
 * orochi_auth_jwt
 *    Returns current JWT claims as JSONB
 */
Datum
orochi_auth_jwt(PG_FUNCTION_ARGS)
{
    const char *jwt_token;
    text       *result;

    jwt_token = GetConfigOption("orochi.auth_token", true, false);

    if (jwt_token == NULL || strlen(jwt_token) == 0)
        PG_RETURN_NULL();

    /*
     * TODO: Parse JWT and return claims as JSONB.
     * For now, return empty JSON object.
     */
    result = cstring_to_text("{}");
    PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(orochi_auth_verify_token);
/*
 * orochi_auth_verify_token
 *    Verify a JWT token and return validation result
 */
Datum
orochi_auth_verify_token(PG_FUNCTION_ARGS)
{
    text                 *token_text;
    char                 *token;
    OrochiTokenValidation validation;
    TupleDesc             tupdesc;
    Datum                 values[6];
    bool                  nulls[6];
    HeapTuple             tuple;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    token_text = PG_GETARG_TEXT_PP(0);
    token = text_to_cstring(token_text);

    validation = orochi_auth_validate_token(token, NULL);

    /* Build result tuple */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    values[0] = BoolGetDatum(validation.is_valid);
    values[1] = CStringGetTextDatum(validation.error_code);
    values[2] = CStringGetTextDatum(validation.error_message);

    if (validation.is_valid)
    {
        values[3] = CStringGetTextDatum(validation.user_id);
        values[4] = CStringGetTextDatum(validation.tenant_id);
        values[5] = TimestampTzGetDatum(validation.expires_at);
    }
    else
    {
        nulls[3] = true;
        nulls[4] = true;
        nulls[5] = true;
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(orochi_auth_create_api_key);
/*
 * orochi_auth_create_api_key
 *    Create a new API key for the current user
 */
Datum
orochi_auth_create_api_key(PG_FUNCTION_ARGS)
{
    text       *name_text;
    char       *name;
    char       *user_id;
    char        api_key[128];
    char        prefix[OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1];

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("API key name cannot be null")));

    name_text = PG_GETARG_TEXT_PP(0);
    name = text_to_cstring(name_text);

    /* Get current user */
    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be authenticated to create API key")));

    /* Generate API key */
    snprintf(prefix, sizeof(prefix), "orochi_k");
    orochi_auth_generate_token(api_key + 9, 32);
    memcpy(api_key, prefix, 8);
    api_key[8] = '_';
    api_key[41] = '\0';

    /*
     * TODO: Store API key hash in database.
     * For now, just return the generated key.
     */

    /* Audit log */
    orochi_auth_audit_log(orochi_auth_get_current_tenant_id(),
                           user_id, NULL,
                           OROCHI_AUDIT_API_KEY_CREATED,
                           NULL, NULL,
                           "api_key", name);

    PG_RETURN_TEXT_P(cstring_to_text(api_key));
}

PG_FUNCTION_INFO_V1(orochi_auth_revoke_api_key);
/*
 * orochi_auth_revoke_api_key
 *    Revoke an API key
 */
Datum
orochi_auth_revoke_api_key(PG_FUNCTION_ARGS)
{
    text       *key_id_text;
    char       *key_id;
    char       *user_id;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("API key ID cannot be null")));

    key_id_text = PG_GETARG_TEXT_PP(0);
    key_id = text_to_cstring(key_id_text);

    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be authenticated to revoke API key")));

    /*
     * TODO: Mark API key as revoked in database.
     */

    /* Audit log */
    orochi_auth_audit_log(orochi_auth_get_current_tenant_id(),
                           user_id, NULL,
                           OROCHI_AUDIT_API_KEY_REVOKED,
                           NULL, NULL,
                           "api_key", key_id);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(orochi_auth_list_api_keys);
/*
 * orochi_auth_list_api_keys
 *    List API keys for the current user
 */
Datum
orochi_auth_list_api_keys(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    TupleDesc        tupdesc;
    char            *user_id;

    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be authenticated to list API keys")));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context that cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /*
         * TODO: Query database for API keys.
         * For now, return empty set.
         */
        funcctx->max_calls = 0;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        /* Would return API key info here */
        SRF_RETURN_NEXT(funcctx, (Datum) 0);
    }

    SRF_RETURN_DONE(funcctx);
}
