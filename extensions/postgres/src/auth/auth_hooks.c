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


/* postgres.h must be included first */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

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
void orochi_auth_client_authentication_hook(Port *port, int status)
{
    const char *api_key = NULL;
    const char *jwt_token = NULL;
    const char *tenant_id = NULL;
    OrochiTokenValidation validation;
    char ip_address[OROCHI_AUTH_IP_SIZE + 1];
    char user_agent[OROCHI_AUTH_USER_AGENT_SIZE + 1];

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
    if (port->raddr.addr.ss_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&port->raddr.addr;
        inet_ntop(AF_INET, &addr->sin_addr, ip_address, sizeof(ip_address));
    } else if (port->raddr.addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&port->raddr.addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ip_address, sizeof(ip_address));
    }

    /* No user agent in PostgreSQL protocol, but could be in options */
    user_agent[0] = '\0';

    /* Check for rate limiting on login attempts */
    if (orochi_auth_rate_limit_enabled) {
        if (!orochi_auth_check_rate_limit(ip_address, "login", orochi_auth_rate_limit_login)) {
            ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
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
    if (jwt_token != NULL && strlen(jwt_token) > 0) {
        validation = orochi_auth_validate_token(jwt_token, tenant_id);

        if (!validation.is_valid) {
            /* Log failed authentication */
            orochi_auth_audit_log(tenant_id, NULL, NULL, OROCHI_AUDIT_LOGIN_FAILED, ip_address,
                                  user_agent, "jwt", NULL);

            ereport(FATAL, (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                            errmsg("JWT token validation failed: %s", validation.error_message),
                            errdetail("Error code: %s", validation.error_code)));
        }

        /* Set session context from validated token */
        orochi_auth_set_session_context(&validation);

        /* Log successful authentication */
        orochi_auth_audit_log(validation.tenant_id, validation.user_id, validation.session_id,
                              OROCHI_AUDIT_LOGIN_SUCCESS, ip_address, user_agent, "jwt", NULL);

        return;
    }

    /* Try API key authentication */
    if (api_key != NULL && strlen(api_key) > 0) {
        validation = orochi_auth_validate_api_key(api_key);

        if (!validation.is_valid) {
            /* Log failed authentication */
            orochi_auth_audit_log(tenant_id, NULL, NULL, OROCHI_AUDIT_LOGIN_FAILED, ip_address,
                                  user_agent, "api_key", NULL);

            ereport(FATAL, (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                            errmsg("API key validation failed: %s", validation.error_message),
                            errdetail("Error code: %s", validation.error_code)));
        }

        /* Set session context from validated API key */
        orochi_auth_set_session_context(&validation);

        /* Log successful authentication */
        orochi_auth_audit_log(validation.tenant_id, validation.user_id, validation.session_id,
                              OROCHI_AUDIT_LOGIN_SUCCESS, ip_address, user_agent, "api_key", NULL);

        return;
    }

    /*
     * No Orochi-specific authentication provided.
     * Connection proceeds with standard PostgreSQL auth only.
     * Set default anonymous context.
     */
    orochi_auth_clear_session_context();

    /* If tenant_id is provided, set it even for anonymous access */
    if (tenant_id != NULL && strlen(tenant_id) > 0) {
        SetConfigOption("orochi.auth_tenant_id", tenant_id, PGC_USERSET, PGC_S_SESSION);
    }
}

/* ============================================================
 * Shared Memory Startup Hook
 * ============================================================ */

/*
 * orochi_auth_shmem_startup_hook
 *    Initialize auth shared memory during startup
 */
static void orochi_auth_shmem_startup_hook(void)
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
static void orochi_auth_register_background_workers(void)
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
    worker.bgw_main_arg = (Datum)0;
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
    worker.bgw_main_arg = (Datum)0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);

    /* Key rotation worker */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "orochi auth key rotation");
    snprintf(worker.bgw_type, BGW_MAXLEN, "orochi auth");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 3600; /* Restart after 1 hour on crash */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "orochi");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "orochi_auth_key_rotation_worker_main");
    worker.bgw_main_arg = (Datum)0;
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
void orochi_auth_install_hooks(void)
{
    /* Install ClientAuthentication hook */
    prev_client_auth_hook = ClientAuthentication_hook;
    ClientAuthentication_hook = orochi_auth_client_authentication_hook;

    /*
     * Note: Shared memory and background workers need to be set up
     * during shared_preload_libraries time. Check if we're in that phase.
     */
    if (process_shared_preload_libraries_in_progress) {
#if PG_VERSION_NUM < 150000
        /* Pre-PG15: Request shared memory directly */
        RequestAddinShmemSpace(orochi_auth_shmem_size());

        /* Request LWLock tranche */
        RequestNamedLWLockTranche(OROCHI_AUTH_LWLOCK_TRANCHE_NAME, OROCHI_AUTH_LWLOCK_COUNT);
#endif
        /* Note: PG15+ shared memory is requested via main init.c shmem_request_hook
         */

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
void orochi_auth_uninstall_hooks(void)
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
typedef struct OrochiConnectionInfo {
    char user[64];
    char tenant_id[OROCHI_AUTH_TENANT_ID_SIZE + 1];
    char pooler_mode[32];
    char password[256];
    bool is_valid;
} OrochiConnectionInfo;

static OrochiConnectionInfo orochi_auth_parse_connection_user(const char *user,
                                                              const char *password)
{
    OrochiConnectionInfo info;
    const char *dot1;
    const char *dot2;
    int user_len;

    memset(&info, 0, sizeof(OrochiConnectionInfo));
    info.is_valid = true;

    if (user == NULL || strlen(user) == 0) {
        info.is_valid = false;
        return info;
    }

    /* Look for dots in username */
    dot1 = strchr(user, '.');
    if (dot1 == NULL) {
        /* Simple username, no tenant */
        strlcpy(info.user, user, sizeof(info.user));
    } else {
        /* Extract user part */
        user_len = dot1 - user;
        if (user_len >= (int)sizeof(info.user))
            user_len = sizeof(info.user) - 1;
        memcpy(info.user, user, user_len);
        info.user[user_len] = '\0';

        /* Look for second dot (pooler mode) */
        dot2 = strchr(dot1 + 1, '.');
        if (dot2 == NULL) {
            /* Only tenant_id after first dot */
            strlcpy(info.tenant_id, dot1 + 1, sizeof(info.tenant_id));
        } else {
            /* tenant_id.pooler_mode */
            int tenant_len = dot2 - (dot1 + 1);
            if (tenant_len >= (int)sizeof(info.tenant_id))
                tenant_len = sizeof(info.tenant_id) - 1;
            memcpy(info.tenant_id, dot1 + 1, tenant_len);
            info.tenant_id[tenant_len] = '\0';

            strlcpy(info.pooler_mode, dot2 + 1, sizeof(info.pooler_mode));
        }
    }

    /* Copy password/token */
    if (password != NULL) {
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
static const char *orochi_auth_extract_bearer_token(const char *auth_header)
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
static bool orochi_auth_is_api_key_format(const char *token)
{
    if (token == NULL || strlen(token) < OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1)
        return false;

    /*
     * API keys typically have a recognizable prefix format:
     *   - orochi_key_xxxxx
     *   - sk_live_xxxxx
     *   - pk_test_xxxxx
     */
    if (strncmp(token, "orochi_", 7) == 0 || strncmp(token, "sk_", 3) == 0 ||
        strncmp(token, "pk_", 3) == 0) {
        return true;
    }

    return false;
}

/*
 * orochi_auth_is_jwt_format
 *    Check if string looks like a JWT (has two dots)
 */
static bool orochi_auth_is_jwt_format(const char *token)
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
 * API Keys Database Operations
 * ============================================================ */

/* OrochiApiKeyInfo is defined in auth.h */

/*
 * orochi_auth_ensure_api_keys_table
 *    Create the API keys table if it doesn't exist
 */
static void orochi_auth_ensure_api_keys_table(void)
{
    static bool table_checked = false;
    int ret;

    const char *create_table_sql = "CREATE TABLE IF NOT EXISTS orochi.orochi_api_keys ("
                                   "    key_id BIGSERIAL PRIMARY KEY,"
                                   "    user_id TEXT NOT NULL,"
                                   "    tenant_id TEXT,"
                                   "    name TEXT NOT NULL,"
                                   "    prefix TEXT NOT NULL,"
                                   "    key_hash TEXT NOT NULL UNIQUE,"
                                   "    scopes BIGINT NOT NULL DEFAULT 0,"
                                   "    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
                                   "    last_used_at TIMESTAMPTZ,"
                                   "    expires_at TIMESTAMPTZ,"
                                   "    is_revoked BOOLEAN NOT NULL DEFAULT FALSE,"
                                   "    revoked_at TIMESTAMPTZ"
                                   ");"
                                   "CREATE INDEX IF NOT EXISTS idx_api_keys_user ON "
                                   "orochi.orochi_api_keys(user_id);"
                                   "CREATE INDEX IF NOT EXISTS idx_api_keys_hash ON "
                                   "orochi.orochi_api_keys(key_hash);"
                                   "CREATE INDEX IF NOT EXISTS idx_api_keys_prefix ON "
                                   "orochi.orochi_api_keys(prefix);";

    /* Only check once per session */
    if (table_checked)
        return;

    SPI_connect();

    /* Check if orochi schema exists first */
    ret = SPI_execute("SELECT 1 FROM pg_namespace WHERE nspname = 'orochi'", true, 1);
    if (ret != SPI_OK_SELECT || SPI_processed == 0) {
        /* Create schema if needed */
        ret = SPI_execute("CREATE SCHEMA IF NOT EXISTS orochi", false, 0);
        if (ret != SPI_OK_UTILITY) {
            SPI_finish();
            elog(WARNING, "Failed to create orochi schema for API keys");
            return;
        }
    }

    /* Create the table */
    ret = SPI_execute(create_table_sql, false, 0);
    if (ret != SPI_OK_UTILITY) {
        SPI_finish();
        elog(WARNING, "Failed to create orochi_api_keys table");
        return;
    }

    SPI_finish();
    table_checked = true;

    elog(DEBUG1, "Orochi API keys table ensured");
}

/*
 * orochi_auth_store_api_key
 *    Store a new API key in the database
 *
 * Parameters:
 *   user_id    - User ID who owns the key
 *   tenant_id  - Tenant ID (may be NULL)
 *   name       - Display name for the key
 *   prefix     - Key prefix for identification (e.g., "orochi_k")
 *   key_hash   - SHA-256 hash of the full API key
 *   scopes     - Permission scopes bitmask
 *   expires_at - Expiration timestamp (0 for no expiry)
 *
 * Returns: The generated key_id, or -1 on error
 */
int64 orochi_auth_store_api_key(const char *user_id, const char *tenant_id, const char *name,
                                const char *prefix, const char *key_hash, int64 scopes,
                                TimestampTz expires_at)
{
    StringInfoData query;
    int ret;
    int64 key_id = -1;

    if (user_id == NULL || name == NULL || prefix == NULL || key_hash == NULL) {
        elog(WARNING, "orochi_auth_store_api_key: NULL parameter");
        return -1;
    }

    /* Ensure the table exists */
    orochi_auth_ensure_api_keys_table();

    initStringInfo(&query);

    if (expires_at != 0) {
        appendStringInfo(&query,
                         "INSERT INTO orochi.orochi_api_keys "
                         "(user_id, tenant_id, name, prefix, key_hash, scopes, expires_at) "
                         "VALUES (%s, %s, %s, %s, %s, %ld, '%s'::timestamptz) "
                         "RETURNING key_id",
                         quote_literal_cstr(user_id),
                         tenant_id ? quote_literal_cstr(tenant_id) : "NULL",
                         quote_literal_cstr(name), quote_literal_cstr(prefix),
                         quote_literal_cstr(key_hash), scopes, timestamptz_to_str(expires_at));
    } else {
        appendStringInfo(&query,
                         "INSERT INTO orochi.orochi_api_keys "
                         "(user_id, tenant_id, name, prefix, key_hash, scopes) "
                         "VALUES (%s, %s, %s, %s, %s, %ld) "
                         "RETURNING key_id",
                         quote_literal_cstr(user_id),
                         tenant_id ? quote_literal_cstr(tenant_id) : "NULL",
                         quote_literal_cstr(name), quote_literal_cstr(prefix),
                         quote_literal_cstr(key_hash), scopes);
    }

    SPI_connect();
    ret = SPI_execute(query.data, false, 1);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        key_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    } else {
        elog(WARNING, "Failed to store API key: SPI result %d", ret);
    }

    SPI_finish();
    pfree(query.data);

    if (key_id > 0)
        elog(DEBUG1, "Stored API key %ld for user %s", key_id, user_id);

    return key_id;
}

/*
 * orochi_auth_revoke_api_key_db
 *    Mark an API key as revoked in the database
 *
 * Parameters:
 *   key_id    - ID of the key to revoke (if known)
 *   prefix    - Key prefix to identify the key (alternative to key_id)
 *   user_id   - User ID for ownership verification
 *
 * Returns: true if key was revoked, false otherwise
 */
bool orochi_auth_revoke_api_key_db(int64 key_id, const char *prefix, const char *user_id)
{
    StringInfoData query;
    int ret;
    bool success = false;

    if (user_id == NULL) {
        elog(WARNING, "orochi_auth_revoke_api_key_db: NULL user_id");
        return false;
    }

    if (key_id <= 0 && (prefix == NULL || strlen(prefix) == 0)) {
        elog(WARNING, "orochi_auth_revoke_api_key_db: must specify key_id or prefix");
        return false;
    }

    /* Ensure the table exists */
    orochi_auth_ensure_api_keys_table();

    initStringInfo(&query);

    if (key_id > 0) {
        /* Revoke by key_id */
        appendStringInfo(&query,
                         "UPDATE orochi.orochi_api_keys "
                         "SET is_revoked = TRUE, revoked_at = NOW() "
                         "WHERE key_id = %ld AND user_id = %s AND is_revoked = FALSE",
                         key_id, quote_literal_cstr(user_id));
    } else {
        /* Revoke by prefix (the key_id passed in is actually the prefix string) */
        appendStringInfo(&query,
                         "UPDATE orochi.orochi_api_keys "
                         "SET is_revoked = TRUE, revoked_at = NOW() "
                         "WHERE prefix = %s AND user_id = %s AND is_revoked = FALSE",
                         quote_literal_cstr(prefix), quote_literal_cstr(user_id));
    }

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_UPDATE && SPI_processed > 0) {
        success = true;
        elog(DEBUG1, "Revoked API key for user %s", user_id);
    } else if (ret == SPI_OK_UPDATE) {
        elog(WARNING, "API key not found or already revoked");
    } else {
        elog(WARNING, "Failed to revoke API key: SPI result %d", ret);
    }

    SPI_finish();
    pfree(query.data);

    return success;
}

/*
 * orochi_auth_update_api_key_usage
 *    Update the last_used_at timestamp for an API key
 */
void orochi_auth_update_api_key_usage(const char *key_hash)
{
    StringInfoData query;

    if (key_hash == NULL)
        return;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE orochi.orochi_api_keys "
                     "SET last_used_at = NOW() "
                     "WHERE key_hash = %s AND is_revoked = FALSE",
                     quote_literal_cstr(key_hash));

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
}

/*
 * orochi_auth_list_api_keys_db
 *    Query API keys for a user from the database
 *
 * Parameters:
 *   user_id       - User ID to list keys for
 *   include_revoked - Whether to include revoked keys
 *   keys_out      - Output array of API key info structures
 *   max_keys      - Maximum number of keys to return
 *
 * Returns: Number of keys found
 */
int orochi_auth_list_api_keys_db(const char *user_id, bool include_revoked,
                                 OrochiApiKeyInfo **keys_out, int max_keys)
{
    StringInfoData query;
    int ret;
    int key_count = 0;
    uint64 i;

    if (user_id == NULL || keys_out == NULL || max_keys <= 0) {
        elog(WARNING, "orochi_auth_list_api_keys_db: invalid parameters");
        return 0;
    }

    /* Ensure the table exists */
    orochi_auth_ensure_api_keys_table();

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT key_id, prefix, name, user_id, tenant_id, "
                     "created_at, last_used_at, expires_at, is_revoked "
                     "FROM orochi.orochi_api_keys "
                     "WHERE user_id = %s",
                     quote_literal_cstr(user_id));

    if (!include_revoked) {
        appendStringInfoString(&query, " AND is_revoked = FALSE");
    }

    appendStringInfo(&query, " ORDER BY created_at DESC LIMIT %d", max_keys);

    SPI_connect();
    ret = SPI_execute(query.data, true, max_keys);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        key_count = (int)SPI_processed;
        *keys_out = (OrochiApiKeyInfo *)palloc0(sizeof(OrochiApiKeyInfo) * key_count);

        for (i = 0; i < SPI_processed; i++) {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            OrochiApiKeyInfo *key = &((*keys_out)[i]);
            char *str_val;

            /* key_id */
            key->key_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));

            /* prefix */
            str_val = SPI_getvalue(tuple, tupdesc, 2);
            if (str_val != NULL) {
                strlcpy(key->prefix, str_val, sizeof(key->prefix));
                pfree(str_val);
            }

            /* name */
            str_val = SPI_getvalue(tuple, tupdesc, 3);
            if (str_val != NULL) {
                strlcpy(key->name, str_val, sizeof(key->name));
                pfree(str_val);
            }

            /* user_id */
            str_val = SPI_getvalue(tuple, tupdesc, 4);
            if (str_val != NULL) {
                strlcpy(key->user_id, str_val, sizeof(key->user_id));
                pfree(str_val);
            }

            /* tenant_id */
            str_val = SPI_getvalue(tuple, tupdesc, 5);
            if (str_val != NULL) {
                strlcpy(key->tenant_id, str_val, sizeof(key->tenant_id));
                pfree(str_val);
            }

            /* created_at */
            key->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 6, &isnull));

            /* last_used_at */
            SPI_getbinval(tuple, tupdesc, 7, &isnull);
            key->has_last_used = !isnull;
            if (!isnull)
                key->last_used_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 7, &isnull));

            /* expires_at */
            SPI_getbinval(tuple, tupdesc, 8, &isnull);
            key->has_expires = !isnull;
            if (!isnull)
                key->expires_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));

            /* is_revoked */
            key->is_revoked = DatumGetBool(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        }
    } else {
        *keys_out = NULL;
    }

    SPI_finish();
    pfree(query.data);

    elog(DEBUG1, "Listed %d API keys for user %s", key_count, user_id);

    return key_count;
}

/*
 * orochi_auth_lookup_api_key_db
 *    Look up an API key by its hash
 *
 * Returns: The API key info if found and valid, NULL otherwise
 */
OrochiApiKeyInfo *orochi_auth_lookup_api_key_db(const char *key_hash)
{
    StringInfoData query;
    OrochiApiKeyInfo *key = NULL;
    int ret;

    if (key_hash == NULL)
        return NULL;

    /* Ensure the table exists */
    orochi_auth_ensure_api_keys_table();

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT key_id, prefix, name, user_id, tenant_id, "
                     "created_at, last_used_at, expires_at, is_revoked "
                     "FROM orochi.orochi_api_keys "
                     "WHERE key_hash = %s",
                     quote_literal_cstr(key_hash));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        char *str_val;

        key = (OrochiApiKeyInfo *)palloc0(sizeof(OrochiApiKeyInfo));

        /* key_id */
        key->key_id = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));

        /* prefix */
        str_val = SPI_getvalue(tuple, tupdesc, 2);
        if (str_val != NULL) {
            strlcpy(key->prefix, str_val, sizeof(key->prefix));
            pfree(str_val);
        }

        /* name */
        str_val = SPI_getvalue(tuple, tupdesc, 3);
        if (str_val != NULL) {
            strlcpy(key->name, str_val, sizeof(key->name));
            pfree(str_val);
        }

        /* user_id */
        str_val = SPI_getvalue(tuple, tupdesc, 4);
        if (str_val != NULL) {
            strlcpy(key->user_id, str_val, sizeof(key->user_id));
            pfree(str_val);
        }

        /* tenant_id */
        str_val = SPI_getvalue(tuple, tupdesc, 5);
        if (str_val != NULL) {
            strlcpy(key->tenant_id, str_val, sizeof(key->tenant_id));
            pfree(str_val);
        }

        /* created_at */
        key->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 6, &isnull));

        /* last_used_at */
        SPI_getbinval(tuple, tupdesc, 7, &isnull);
        key->has_last_used = !isnull;
        if (!isnull)
            key->last_used_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 7, &isnull));

        /* expires_at */
        SPI_getbinval(tuple, tupdesc, 8, &isnull);
        key->has_expires = !isnull;
        if (!isnull)
            key->expires_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));

        /* is_revoked */
        key->is_revoked = DatumGetBool(SPI_getbinval(tuple, tupdesc, 9, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return key;
}

/* ============================================================
 * Additional SQL Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_jwt);
/*
 * orochi_auth_jwt
 *    Returns current JWT claims as JSONB
 */
Datum orochi_auth_jwt(PG_FUNCTION_ARGS)
{
    const char *jwt_token;
    text *result;

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
Datum orochi_auth_verify_token(PG_FUNCTION_ARGS)
{
    text *token_text;
    char *token;
    OrochiTokenValidation validation;
    TupleDesc tupdesc;
    Datum values[6];
    bool nulls[6];
    HeapTuple tuple;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    token_text = PG_GETARG_TEXT_PP(0);
    token = text_to_cstring(token_text);

    validation = orochi_auth_validate_token(token, NULL);

    /* Build result tuple */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context that "
                               "cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    values[0] = BoolGetDatum(validation.is_valid);
    values[1] = CStringGetTextDatum(validation.error_code);
    values[2] = CStringGetTextDatum(validation.error_message);

    if (validation.is_valid) {
        values[3] = CStringGetTextDatum(validation.user_id);
        values[4] = CStringGetTextDatum(validation.tenant_id);
        values[5] = TimestampTzGetDatum(validation.expires_at);
    } else {
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
Datum orochi_auth_create_api_key(PG_FUNCTION_ARGS)
{
    text *name_text;
    char *name;
    char *user_id;
    char *tenant_id;
    char api_key[128];
    char prefix[OROCHI_AUTH_API_KEY_PREFIX_SIZE + 1];
    char key_hash[OROCHI_AUTH_TOKEN_HASH_SIZE + 1];
    int64 key_id;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("API key name cannot be null")));

    name_text = PG_GETARG_TEXT_PP(0);
    name = text_to_cstring(name_text);

    /* Get current user */
    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("must be authenticated to create API key")));

    /* Get current tenant */
    tenant_id = orochi_auth_get_current_tenant_id();

    /* Generate API key with prefix */
    snprintf(prefix, sizeof(prefix), "orochi_k");
    orochi_auth_generate_token(api_key + 9, 32);
    memcpy(api_key, prefix, 8);
    api_key[8] = '_';
    api_key[41] = '\0';

    /* Hash the API key for secure storage */
    orochi_auth_hash_token(api_key, key_hash);

    /* Store API key hash in database */
    key_id = orochi_auth_store_api_key(user_id, tenant_id, name, prefix, key_hash,
                                       0,  /* default scopes */
                                       0); /* no expiry */

    if (key_id < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to store API key in database")));

    /* Audit log */
    orochi_auth_audit_log(tenant_id, user_id, NULL, OROCHI_AUDIT_API_KEY_CREATED, NULL, NULL,
                          "api_key", name);

    PG_RETURN_TEXT_P(cstring_to_text(api_key));
}

PG_FUNCTION_INFO_V1(orochi_auth_revoke_api_key);
/*
 * orochi_auth_revoke_api_key
 *    Revoke an API key
 *
 * The key_id parameter can be either:
 *   - A numeric key ID (as text)
 *   - An API key prefix string (e.g., "orochi_k")
 */
Datum orochi_auth_revoke_api_key(PG_FUNCTION_ARGS)
{
    text *key_id_text;
    char *key_id_str;
    char *user_id;
    int64 numeric_key_id = 0;
    char *endptr;
    bool revoked;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("API key ID cannot be null")));

    key_id_text = PG_GETARG_TEXT_PP(0);
    key_id_str = text_to_cstring(key_id_text);

    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("must be authenticated to revoke API key")));

    /* Try to parse as numeric ID first */
    numeric_key_id = strtoll(key_id_str, &endptr, 10);
    if (*endptr != '\0') {
        /* Not a number, treat as prefix string */
        numeric_key_id = 0;
    }

    /* Mark API key as revoked in database */
    if (numeric_key_id > 0) {
        revoked = orochi_auth_revoke_api_key_db(numeric_key_id, NULL, user_id);
    } else {
        revoked = orochi_auth_revoke_api_key_db(0, key_id_str, user_id);
    }

    if (!revoked)
        ereport(ERROR,
                (errcode(ERRCODE_NO_DATA_FOUND), errmsg("API key not found or already revoked"),
                 errdetail("Key identifier: %s", key_id_str)));

    /* Audit log */
    orochi_auth_audit_log(orochi_auth_get_current_tenant_id(), user_id, NULL,
                          OROCHI_AUDIT_API_KEY_REVOKED, NULL, NULL, "api_key", key_id_str);

    PG_RETURN_VOID();
}

/*
 * Context structure for orochi_auth_list_api_keys SRF
 */
typedef struct ApiKeyListContext {
    OrochiApiKeyInfo *keys;
    int key_count;
} ApiKeyListContext;

PG_FUNCTION_INFO_V1(orochi_auth_list_api_keys);
/*
 * orochi_auth_list_api_keys
 *    List API keys for the current user
 *
 * Returns a set of records with columns:
 *   - key_id (bigint)
 *   - prefix (text)
 *   - name (text)
 *   - created_at (timestamptz)
 *   - last_used_at (timestamptz, nullable)
 *   - is_revoked (boolean)
 */
Datum orochi_auth_list_api_keys(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    TupleDesc tupdesc;
    char *user_id;
    ApiKeyListContext *ctx;

    user_id = orochi_auth_get_current_user_id();
    if (user_id == NULL)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("must be authenticated to list API keys")));

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext oldcontext;
        OrochiApiKeyInfo *keys = NULL;
        int key_count;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("function returning record called in context that "
                                   "cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /* Query database for API keys */
        key_count = orochi_auth_list_api_keys_db(user_id, false, /* don't include revoked */
                                                 &keys, OROCHI_AUTH_MAX_API_KEYS_PER_USER);

        /* Store context for subsequent calls */
        ctx = (ApiKeyListContext *)palloc(sizeof(ApiKeyListContext));
        ctx->keys = keys;
        ctx->key_count = key_count;

        funcctx->user_fctx = ctx;
        funcctx->max_calls = key_count;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    ctx = (ApiKeyListContext *)funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls) {
        Datum values[6];
        bool nulls[6];
        HeapTuple tuple;
        OrochiApiKeyInfo *key;

        key = &(ctx->keys[funcctx->call_cntr]);

        memset(nulls, 0, sizeof(nulls));

        /* key_id */
        values[0] = Int64GetDatum(key->key_id);

        /* prefix */
        values[1] = CStringGetTextDatum(key->prefix);

        /* name */
        values[2] = CStringGetTextDatum(key->name);

        /* created_at */
        values[3] = TimestampTzGetDatum(key->created_at);

        /* last_used_at (nullable) */
        if (key->has_last_used)
            values[4] = TimestampTzGetDatum(key->last_used_at);
        else
            nulls[4] = true;

        /* is_revoked */
        values[5] = BoolGetDatum(key->is_revoked);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}
