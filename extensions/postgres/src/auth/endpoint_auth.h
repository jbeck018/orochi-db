/*-------------------------------------------------------------------------
 *
 * endpoint_auth.h
 *    Orochi DB Compute Endpoint Authentication
 *
 * This module implements token-based authentication for compute endpoints:
 *   - JWT token generation and validation
 *   - Connection string generation
 *   - PgCat pooler integration
 *   - Session context injection
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_ENDPOINT_AUTH_H
#define OROCHI_ENDPOINT_AUTH_H

#include "branch_access.h"
#include "postgres.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* ============================================================
 * Constants
 * ============================================================ */

/* Token configuration */
#define ENDPOINT_TOKEN_ID_LEN          64
#define ENDPOINT_TOKEN_SECRET_LEN      32
#define ENDPOINT_DEFAULT_TOKEN_TTL_SEC 3600  /* 1 hour */
#define ENDPOINT_MAX_TOKEN_TTL_SEC     86400 /* 24 hours */

/* Connection limits */
#define ENDPOINT_DEFAULT_MAX_CONN 100
#define ENDPOINT_MIN_CONN         0
#define ENDPOINT_MAX_CONN         10000

/* Endpoint naming */
#define ENDPOINT_NAME_MAX_LEN 63
#define ENDPOINT_HOST_MAX_LEN 255

/* Cache configuration */
#define ENDPOINT_TOKEN_CACHE_SIZE    100000
#define ENDPOINT_TOKEN_CACHE_TTL_SEC 60

/* ============================================================
 * Enumerations
 * ============================================================ */

/*
 * Endpoint credential type
 */
typedef enum EndpointCredentialType {
    ENDPOINT_CRED_PASSWORD = 0, /* Traditional username/password */
    ENDPOINT_CRED_JWT,          /* JWT token (embedded in connection) */
    ENDPOINT_CRED_OPAQUE_TOKEN  /* Opaque token (lookup required) */
} EndpointCredentialType;

/*
 * Pooler mode
 */
typedef enum EndpointPoolerMode {
    ENDPOINT_POOL_SESSION = 0, /* Session pooling */
    ENDPOINT_POOL_TRANSACTION, /* Transaction pooling */
    ENDPOINT_POOL_STATEMENT    /* Statement pooling */
} EndpointPoolerMode;

/*
 * SSL mode
 */
typedef enum EndpointSSLMode {
    ENDPOINT_SSL_DISABLE = 0,
    ENDPOINT_SSL_ALLOW,
    ENDPOINT_SSL_PREFER,
    ENDPOINT_SSL_REQUIRE,
    ENDPOINT_SSL_VERIFY_CA,
    ENDPOINT_SSL_VERIFY_FULL
} EndpointSSLMode;

/*
 * Endpoint status
 */
typedef enum EndpointStatus {
    ENDPOINT_STATUS_CREATING = 0,
    ENDPOINT_STATUS_ACTIVE,
    ENDPOINT_STATUS_SUSPENDED,
    ENDPOINT_STATUS_SCALING,
    ENDPOINT_STATUS_DELETING
} EndpointStatus;

/*
 * Token validation result
 */
typedef enum EndpointTokenValidation {
    ENDPOINT_TOKEN_VALID = 0,
    ENDPOINT_TOKEN_EXPIRED,
    ENDPOINT_TOKEN_INVALID_SIGNATURE,
    ENDPOINT_TOKEN_REVOKED,
    ENDPOINT_TOKEN_INVALID_ENDPOINT,
    ENDPOINT_TOKEN_INVALID_FORMAT,
    ENDPOINT_TOKEN_NOT_FOUND
} EndpointTokenValidation;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiComputeEndpoint - Endpoint configuration
 */
typedef struct OrochiComputeEndpoint {
    pg_uuid_t endpoint_id;
    pg_uuid_t cluster_id;
    pg_uuid_t branch_id;

    /* Endpoint configuration */
    char endpoint_name[ENDPOINT_NAME_MAX_LEN + 1];
    char endpoint_host[ENDPOINT_HOST_MAX_LEN + 1];
    int endpoint_port;

    /* Pooler configuration */
    EndpointPoolerMode pooler_mode;
    int min_connections;
    int max_connections;
    int idle_timeout_seconds;

    /* Security */
    EndpointSSLMode ssl_mode;
    inet **allowed_ips;
    int allowed_ip_count;

    /* State */
    EndpointStatus status;
    double compute_units;

    /* Autoscaling */
    bool autoscale_enabled;
    double autoscale_min_cu;
    double autoscale_max_cu;
    int scale_to_zero_seconds;

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
} OrochiComputeEndpoint;

/*
 * OrochiEndpointCredential - Endpoint credential
 */
typedef struct OrochiEndpointCredential {
    pg_uuid_t credential_id;
    pg_uuid_t endpoint_id;

    /* Credential type */
    EndpointCredentialType credential_type;

    /* For password-based */
    char *username;
    char *password_hash; /* bcrypt hash */

    /* For token-based */
    char token_id[ENDPOINT_TOKEN_ID_LEN + 1];
    char *token_hash;

    /* Permissions */
    char db_role[NAMEDATALEN];
    BranchPermissionLevel branch_permission;

    /* Constraints */
    TimestampTz valid_from;
    TimestampTz valid_until;
    inet **ip_allowlist;
    int ip_allowlist_count;
    int max_connections;

    /* Metadata */
    char *description;
    pg_uuid_t *created_by;
    TimestampTz created_at;
    TimestampTz last_used_at;
    int64 use_count;

    /* Revocation */
    TimestampTz revoked_at;
    pg_uuid_t *revoked_by;
} OrochiEndpointCredential;

/*
 * EndpointJWTClaims - JWT token claims for Orochi
 */
typedef struct EndpointJWTClaims {
    /* Standard JWT claims */
    char *iss;                           /* Issuer: "orochi.cloud" */
    char *sub;                           /* Subject: user_id */
    char *aud;                           /* Audience: endpoint_host */
    int64 exp;                           /* Expiration time */
    int64 iat;                           /* Issued at */
    char jti[ENDPOINT_TOKEN_ID_LEN + 1]; /* JWT ID (token_id) */

    /* Orochi-specific claims */
    pg_uuid_t endpoint_id;
    pg_uuid_t branch_id;
    pg_uuid_t cluster_id;
    char db_role[NAMEDATALEN];
    BranchPermissionLevel permission;
} EndpointJWTClaims;

/*
 * EndpointTokenResult - Result of token generation
 */
typedef struct EndpointTokenResult {
    bool success;
    pg_uuid_t credential_id;
    char token_id[ENDPOINT_TOKEN_ID_LEN + 1];
    EndpointJWTClaims *claims;
    char *error_message;
} EndpointTokenResult;

/*
 * EndpointAuthResult - Result of token validation
 */
typedef struct EndpointAuthResult {
    EndpointTokenValidation validation;
    bool authenticated;
    pg_uuid_t user_id;
    pg_uuid_t endpoint_id;
    pg_uuid_t branch_id;
    char db_role[NAMEDATALEN];
    BranchPermissionLevel permission;
    char *error_message;
} EndpointAuthResult;

/*
 * EndpointSessionContext - Context to inject into connection
 */
typedef struct EndpointSessionContext {
    pg_uuid_t user_id;
    pg_uuid_t branch_id;
    pg_uuid_t endpoint_id;
    char db_role[NAMEDATALEN];
    BranchPermissionLevel permission;
} EndpointSessionContext;

/*
 * TokenCacheEntry - Cached token validation
 */
typedef struct TokenCacheEntry {
    char token_id[ENDPOINT_TOKEN_ID_LEN + 1];
    EndpointJWTClaims claims;
    TimestampTz cached_at;
    bool is_valid;
} TokenCacheEntry;

/* ============================================================
 * Function Declarations
 * ============================================================ */

/* Initialization */
extern void endpoint_auth_init(void);
extern Size endpoint_auth_shmem_size(void);

/* Endpoint management */
extern OrochiComputeEndpoint *endpoint_get(pg_uuid_t endpoint_id);
extern OrochiComputeEndpoint *endpoint_get_by_host(const char *host);
extern pg_uuid_t endpoint_create(pg_uuid_t cluster_id, pg_uuid_t branch_id, const char *name,
                                 EndpointPoolerMode pooler_mode, int max_connections);

extern bool endpoint_update_status(pg_uuid_t endpoint_id, EndpointStatus new_status);

extern bool endpoint_delete(pg_uuid_t endpoint_id);

/* Token generation */
extern EndpointTokenResult *endpoint_generate_token(pg_uuid_t endpoint_id, pg_uuid_t user_id,
                                                    const char *db_role,
                                                    BranchPermissionLevel permission,
                                                    int valid_duration_sec);

extern char *endpoint_generate_connection_string(pg_uuid_t credential_id, const char *signed_token,
                                                 const char *database, const char *driver_type);

/* Token validation */
extern EndpointAuthResult *endpoint_validate_token(const char *token, const char *endpoint_host,
                                                   const char *client_ip);

extern EndpointAuthResult *endpoint_validate_password(const char *username, const char *password,
                                                      pg_uuid_t endpoint_id);

extern bool endpoint_validate_jwt_signature(const char *token, const uint8 *public_key,
                                            int public_key_len);

/* Credential management */
extern OrochiEndpointCredential *endpoint_get_credential(pg_uuid_t credential_id);
extern OrochiEndpointCredential *endpoint_get_credential_by_token_id(const char *token_id);
extern OrochiEndpointCredential *endpoint_get_credential_by_username(pg_uuid_t endpoint_id,
                                                                     const char *username);

extern bool endpoint_create_password_credential(pg_uuid_t endpoint_id, const char *username,
                                                const char *password, const char *db_role,
                                                BranchPermissionLevel permission,
                                                pg_uuid_t created_by);

extern bool endpoint_revoke_credential(pg_uuid_t credential_id, pg_uuid_t revoked_by);

extern bool endpoint_rotate_token(pg_uuid_t credential_id, pg_uuid_t rotated_by);

extern void endpoint_record_credential_use(pg_uuid_t credential_id);

/* Session context */
extern EndpointSessionContext *endpoint_create_session_context(EndpointJWTClaims *claims);

extern char **endpoint_get_startup_commands(EndpointSessionContext *context, int *command_count);

extern bool endpoint_set_session_variables(EndpointSessionContext *context);

/* PgCat integration */
extern char *endpoint_generate_pgcat_config(pg_uuid_t endpoint_id);

extern bool endpoint_update_pgcat_routing(pg_uuid_t endpoint_id);

/* Caching */
extern void endpoint_cache_token(const char *token_id, EndpointJWTClaims *claims);
extern EndpointJWTClaims *endpoint_get_cached_token(const char *token_id);
extern void endpoint_invalidate_token_cache(const char *token_id);
extern void endpoint_clear_token_cache(void);

/* Utility functions */
extern const char *endpoint_credential_type_name(EndpointCredentialType type);
extern const char *endpoint_pooler_mode_name(EndpointPoolerMode mode);
extern const char *endpoint_ssl_mode_name(EndpointSSLMode mode);
extern const char *endpoint_status_name(EndpointStatus status);
extern const char *endpoint_token_validation_message(EndpointTokenValidation result);

extern EndpointPoolerMode endpoint_parse_pooler_mode(const char *str);
extern EndpointSSLMode endpoint_parse_ssl_mode(const char *str);
extern EndpointStatus endpoint_parse_status(const char *str);

/* Password hashing */
extern char *endpoint_hash_password(const char *password);
extern bool endpoint_verify_password(const char *password, const char *hash);

/* Cleanup */
extern void endpoint_free(OrochiComputeEndpoint *endpoint);
extern void endpoint_free_credential(OrochiEndpointCredential *cred);
extern void endpoint_free_auth_result(EndpointAuthResult *result);
extern void endpoint_free_token_result(EndpointTokenResult *result);
extern void endpoint_free_session_context(EndpointSessionContext *context);

/* SQL-callable functions */
extern Datum endpoint_generate_token_sql(PG_FUNCTION_ARGS);
extern Datum endpoint_validate_token_sql(PG_FUNCTION_ARGS);
extern Datum endpoint_create_credential_sql(PG_FUNCTION_ARGS);
extern Datum endpoint_get_connection_string_sql(PG_FUNCTION_ARGS);
extern Datum endpoint_revoke_credential_sql(PG_FUNCTION_ARGS);

#endif /* OROCHI_ENDPOINT_AUTH_H */
