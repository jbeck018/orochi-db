/*-------------------------------------------------------------------------
 *
 * gotrue.h
 *    Orochi DB GoTrue-compatible authentication structures and API
 *
 * This module provides Supabase-style authentication features including:
 *   - User management (create, update, delete)
 *   - Session management with JWT tokens
 *   - Password-based and passwordless auth
 *   - Anonymous authentication with upgrades
 *   - Multi-factor authentication support
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_GOTRUE_H
#define OROCHI_GOTRUE_H

#include "postgres.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* ============================================================
 * Authentication Constants
 * ============================================================ */

#define OROCHI_AUTH_SCHEMA         "auth"
#define OROCHI_AUTH_USERS_TABLE    "auth.users"
#define OROCHI_AUTH_SESSIONS_TABLE "auth.sessions"
#define OROCHI_AUTH_TOKENS_TABLE   "auth.one_time_tokens"

/* Token and password constraints */
#define OROCHI_AUTH_TOKEN_LENGTH        32
#define OROCHI_AUTH_OTP_LENGTH          6
#define OROCHI_AUTH_MIN_PASSWORD_LENGTH 6
#define OROCHI_AUTH_MAX_PASSWORD_LENGTH 256
#define OROCHI_AUTH_MAX_EMAIL_LENGTH    255
#define OROCHI_AUTH_MAX_PHONE_LENGTH    32
#define OROCHI_AUTH_MAX_METADATA_SIZE   65536

/* Session configuration */
#define OROCHI_AUTH_DEFAULT_JWT_EXP   3600   /* 1 hour */
#define OROCHI_AUTH_REFRESH_TOKEN_EXP 604800 /* 7 days */

/* GUC variable for JWT expiration (defined in gotrue.c) */
extern int orochi_auth_jwt_exp;
#define OROCHI_AUTH_MAGIC_LINK_EXP    3600   /* 1 hour */
#define OROCHI_AUTH_OTP_EXP           600    /* 10 minutes */

/* Rate limiting */
#define OROCHI_AUTH_RATE_LIMIT_EMAIL   30  /* Per hour */
#define OROCHI_AUTH_RATE_LIMIT_SMS     30  /* Per hour */
#define OROCHI_AUTH_RATE_LIMIT_REFRESH 150 /* Per hour */

/* ============================================================
 * Authentication Enumerations
 * ============================================================ */

/*
 * Authenticator Assurance Level (AAL) per NIST SP 800-63B
 */
typedef enum OrochiAuthAAL {
    OROCHI_AAL_1 = 1, /* Single-factor authentication */
    OROCHI_AAL_2 = 2, /* Multi-factor authentication */
    OROCHI_AAL_3 = 3  /* Hardware-backed MFA */
} OrochiAuthAAL;

/*
 * MFA Factor Types
 */
typedef enum OrochiAuthFactorType {
    OROCHI_FACTOR_TOTP = 0, /* Time-based OTP (Google Auth, etc.) */
    OROCHI_FACTOR_PHONE,    /* SMS/Voice OTP */
    OROCHI_FACTOR_WEBAUTHN  /* WebAuthn/FIDO2 */
} OrochiAuthFactorType;

/*
 * MFA Factor Status
 */
typedef enum OrochiAuthFactorStatus {
    OROCHI_FACTOR_UNVERIFIED = 0,
    OROCHI_FACTOR_VERIFIED
} OrochiAuthFactorStatus;

/*
 * One-Time Token Types
 */
typedef enum OrochiAuthTokenType {
    OROCHI_TOKEN_CONFIRMATION = 0, /* Email/phone confirmation */
    OROCHI_TOKEN_RECOVERY,         /* Password recovery */
    OROCHI_TOKEN_EMAIL_CHANGE_NEW, /* New email confirmation */
    OROCHI_TOKEN_EMAIL_CHANGE_OLD, /* Old email confirmation */
    OROCHI_TOKEN_PHONE_CHANGE,     /* Phone change confirmation */
    OROCHI_TOKEN_REAUTHENTICATION  /* Re-authentication for sensitive ops */
} OrochiAuthTokenType;

/*
 * Authentication Provider Types
 */
typedef enum OrochiAuthProvider {
    OROCHI_PROVIDER_EMAIL = 0,
    OROCHI_PROVIDER_PHONE,
    OROCHI_PROVIDER_ANONYMOUS,
    OROCHI_PROVIDER_GOOGLE,
    OROCHI_PROVIDER_GITHUB,
    OROCHI_PROVIDER_FACEBOOK,
    OROCHI_PROVIDER_APPLE,
    OROCHI_PROVIDER_AZURE,
    OROCHI_PROVIDER_SAML
} OrochiAuthProvider;

/*
 * User Account Status
 */
typedef enum OrochiAuthUserStatus {
    OROCHI_USER_ACTIVE = 0,
    OROCHI_USER_BANNED,
    OROCHI_USER_DELETED
} OrochiAuthUserStatus;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiAuthUser - Core user identity structure
 * Maps to auth.users table
 */
typedef struct OrochiAuthUser {
    /* Primary identification */
    pg_uuid_t id;          /* User UUID */
    pg_uuid_t instance_id; /* Multi-tenant instance */

    /* Authentication identifiers */
    char *email;              /* Email address (unique) */
    char *phone;              /* Phone number (unique) */
    char *encrypted_password; /* Argon2id hashed password */

    /* Email verification */
    TimestampTz email_confirmed_at;    /* Email confirmation time */
    char *email_confirm_token;         /* Pending confirmation token */
    TimestampTz email_confirm_sent_at; /* When token was sent */

    /* Phone verification */
    TimestampTz phone_confirmed_at;    /* Phone confirmation time */
    char *phone_confirm_token;         /* Pending confirmation token */
    TimestampTz phone_confirm_sent_at; /* When token was sent */

    /* Password recovery */
    char *recovery_token;         /* Password recovery token */
    TimestampTz recovery_sent_at; /* When recovery was sent */

    /* Email change workflow */
    char *email_change;                /* New email pending */
    char *email_change_token_new;      /* New email token */
    char *email_change_token_current;  /* Current email token */
    int16 email_change_confirm_status; /* Confirmation status */
    TimestampTz email_change_sent_at;  /* When change was sent */

    /* Phone change workflow */
    char *phone_change;               /* New phone pending */
    char *phone_change_token;         /* Phone change token */
    TimestampTz phone_change_sent_at; /* When change was sent */

    /* Reauthentication */
    char *reauthentication_token; /* Reauth token */
    TimestampTz reauthentication_sent_at;

    /* User metadata (application-defined) */
    Jsonb *raw_app_meta_data;  /* App-level metadata */
    Jsonb *raw_user_meta_data; /* User-level metadata */

    /* Account status flags */
    bool is_super_admin; /* Super admin flag */
    bool is_sso_user;    /* SSO-authenticated */
    bool is_anonymous;   /* Anonymous account */

    /* Role for RBAC */
    char *role; /* User role (default: authenticated) */

    /* Timestamps */
    TimestampTz created_at;      /* Account creation */
    TimestampTz updated_at;      /* Last update */
    TimestampTz last_sign_in_at; /* Last successful sign in */
    TimestampTz banned_until;    /* Ban expiration (NULL = not banned) */
    TimestampTz deleted_at;      /* Soft delete timestamp */

    /* Memory context */
    MemoryContext context; /* Memory context for this user */
} OrochiAuthUser;

/*
 * OrochiAuthSession - Active user session
 * Maps to auth.sessions table
 */
typedef struct OrochiAuthSession {
    pg_uuid_t id;      /* Session UUID */
    pg_uuid_t user_id; /* User UUID */

    /* Session tokens */
    char *refresh_token;     /* Refresh token */
    char *access_token_hash; /* SHA256 of access token */

    /* Session metadata */
    char *user_agent; /* Client user agent */
    char *ip_address; /* Client IP address */

    /* Authentication level */
    OrochiAuthAAL aal;   /* Authenticator assurance level */
    pg_uuid_t factor_id; /* MFA factor used */
    Jsonb *amr;          /* Authentication methods references */

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
    TimestampTz refreshed_at;
    TimestampTz not_after; /* Session expiration */

    /* Optional tag */
    char *tag; /* Session tag for organization */
} OrochiAuthSession;

/*
 * OrochiMagicLink - Magic link/OTP token structure
 * Maps to auth.one_time_tokens table
 */
typedef struct OrochiMagicLink {
    pg_uuid_t id;      /* Token UUID */
    pg_uuid_t user_id; /* User UUID */

    /* Token details */
    OrochiAuthTokenType token_type; /* Type of token */
    char *token_hash;               /* SHA256 hash of token */

    /* Related entity */
    char *relates_to;  /* Email or phone this token is for */
    char *redirect_to; /* Redirect URL after verification */

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
    TimestampTz expires_at;
    TimestampTz used_at; /* When token was consumed */
} OrochiMagicLink;

/*
 * OrochiAuthMFAFactor - MFA factor configuration
 */
typedef struct OrochiAuthMFAFactor {
    pg_uuid_t id;      /* Factor UUID */
    pg_uuid_t user_id; /* User UUID */

    /* Factor configuration */
    OrochiAuthFactorType factor_type; /* Type of factor */
    OrochiAuthFactorStatus status;    /* Verification status */
    char *friendly_name;              /* User-friendly name */

    /* TOTP-specific */
    char *secret; /* Encrypted TOTP secret */

    /* Phone-specific */
    char *phone; /* Phone number for SMS OTP */

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
} OrochiAuthMFAFactor;

/*
 * OrochiAuthMFAChallenge - MFA challenge for verification
 */
typedef struct OrochiAuthMFAChallenge {
    pg_uuid_t id;        /* Challenge UUID */
    pg_uuid_t factor_id; /* Factor UUID */

    /* Challenge details */
    char *ip_address; /* Client IP */
    char *otp_code;   /* Encrypted OTP code */

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz verified_at;
    TimestampTz expires_at;
} OrochiAuthMFAChallenge;

/*
 * OrochiAuthIdentity - External OAuth provider identity
 */
typedef struct OrochiAuthIdentity {
    pg_uuid_t id;      /* Identity UUID */
    pg_uuid_t user_id; /* User UUID */

    /* Provider information */
    OrochiAuthProvider provider; /* Provider type */
    char *provider_id;           /* ID from provider */
    Jsonb *identity_data;        /* Data from provider */
    char *email;                 /* Email from provider */

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
    TimestampTz last_sign_in_at;
} OrochiAuthIdentity;

/*
 * OrochiAuthConfig - Auth service configuration
 */
typedef struct OrochiAuthConfig {
    pg_uuid_t id;
    pg_uuid_t instance_id;

    /* JWT Configuration */
    char *jwt_secret; /* JWT signing secret */
    int jwt_exp;      /* Token expiration (seconds) */
    char *jwt_aud;    /* JWT audience */

    /* Site configuration */
    char *site_url;          /* Base URL for redirects */
    Jsonb *mailer_url_paths; /* Email URL paths */

    /* Email configuration */
    char *smtp_host;
    int smtp_port;
    char *smtp_user;
    char *smtp_pass; /* Encrypted */
    char *smtp_sender;
    char *smtp_sender_name;

    /* SMS configuration */
    char *sms_provider; /* twilio, messagebird, etc. */
    Jsonb *sms_config;  /* Provider-specific config */

    /* Security settings */
    int password_min_length;
    bool password_require_uppercase;
    bool password_require_lowercase;
    bool password_require_number;
    bool password_require_special;

    /* Feature flags */
    bool disable_signup;
    bool enable_signup_email_confirm;
    bool enable_signup_phone_confirm;
    bool enable_anonymous_sign_in;
    bool enable_manual_linking;
    bool enable_mfa;

    /* Session configuration */
    int session_timebox; /* Max session lifetime */
    int session_inactivity_timeout;

    /* Rate limiting */
    int rate_limit_email_sent;
    int rate_limit_sms_sent;
    int rate_limit_token_refresh;

    /* OAuth providers */
    Jsonb *external_providers;

    /* Timestamps */
    TimestampTz created_at;
    TimestampTz updated_at;
} OrochiAuthConfig;

/*
 * OrochiAuthResult - Authentication result structure
 */
typedef struct OrochiAuthResult {
    bool success;               /* Operation succeeded */
    OrochiAuthUser *user;       /* User object */
    OrochiAuthSession *session; /* Session object */
    char *access_token;         /* JWT access token */
    char *refresh_token;        /* Refresh token */
    TimestampTz expires_at;     /* Token expiration */
    char *error_code;           /* Error code if failed */
    char *error_message;        /* Error message if failed */
} OrochiAuthResult;

/* ============================================================
 * Shared Memory State
 * ============================================================ */

#define OROCHI_AUTH_MAX_SESSIONS 1024

typedef struct OrochiAuthSharedState {
    LWLock *lock;
    int active_sessions;
    TimestampTz last_cleanup;
} OrochiAuthSharedState;

/* ============================================================
 * User Management Functions
 * ============================================================ */

/* User CRUD operations */
extern OrochiAuthUser *orochi_auth_create_user(const char *email, const char *phone,
                                               const char *password, Jsonb *user_metadata);

extern OrochiAuthUser *orochi_auth_get_user(pg_uuid_t user_id);
extern OrochiAuthUser *orochi_auth_get_user_by_email(const char *email);
extern OrochiAuthUser *orochi_auth_get_user_by_phone(const char *phone);

extern bool orochi_auth_update_user(pg_uuid_t user_id, const char *email, const char *phone,
                                    const char *password, Jsonb *user_metadata,
                                    Jsonb *app_metadata);

extern bool orochi_auth_delete_user(pg_uuid_t user_id, bool soft_delete);

extern void orochi_auth_free_user(OrochiAuthUser *user);

/* ============================================================
 * Authentication Functions
 * ============================================================ */

/* Password authentication */
extern OrochiAuthResult *orochi_auth_sign_in_password(const char *email, const char *password);

extern OrochiAuthResult *orochi_auth_sign_in_phone_password(const char *phone,
                                                            const char *password);

/* Token refresh */
extern OrochiAuthResult *orochi_auth_refresh_token(const char *refresh_token);

/* Sign out */
extern bool orochi_auth_sign_out(pg_uuid_t session_id);
extern bool orochi_auth_sign_out_all(pg_uuid_t user_id);

/* Password management */
extern bool orochi_auth_update_password(pg_uuid_t user_id, const char *old_password,
                                        const char *new_password);

extern bool orochi_auth_reset_password(const char *token, const char *new_password);

/* Password validation */
extern bool orochi_auth_verify_password(const char *password, const char *encrypted_password);

extern char *orochi_auth_hash_password(const char *password);

/* ============================================================
 * Session Management Functions
 * ============================================================ */

extern OrochiAuthSession *orochi_auth_create_session(pg_uuid_t user_id, const char *user_agent,
                                                     const char *ip_address, OrochiAuthAAL aal);

extern OrochiAuthSession *orochi_auth_get_session(pg_uuid_t session_id);
extern List *orochi_auth_get_user_sessions(pg_uuid_t user_id);
extern bool orochi_auth_revoke_session(pg_uuid_t session_id);
extern bool orochi_auth_revoke_all_sessions(pg_uuid_t user_id);
extern void orochi_auth_cleanup_expired_sessions(void);
extern void orochi_auth_free_session(OrochiAuthSession *session);

/* ============================================================
 * JWT Functions
 * ============================================================ */

extern char *orochi_auth_generate_jwt(OrochiAuthUser *user, OrochiAuthSession *session,
                                      int exp_seconds);

extern bool orochi_auth_validate_jwt(const char *token, Jsonb **claims_out);

extern bool orochi_auth_set_jwt_context(const char *token);
extern void orochi_auth_clear_jwt_context(void);

/* JWT context accessors (from session variables) */
extern pg_uuid_t orochi_auth_uid(void);
extern char *orochi_auth_role(void);
extern char *orochi_auth_email(void);
extern Jsonb *orochi_auth_jwt(void);
extern OrochiAuthAAL orochi_auth_aal(void);

/* ============================================================
 * MFA Functions
 * ============================================================ */

extern OrochiAuthMFAFactor *orochi_auth_enroll_factor(pg_uuid_t user_id, OrochiAuthFactorType type,
                                                      const char *friendly_name);

extern bool orochi_auth_verify_factor(pg_uuid_t factor_id, const char *code);
extern bool orochi_auth_unenroll_factor(pg_uuid_t factor_id);
extern List *orochi_auth_list_factors(pg_uuid_t user_id);

extern OrochiAuthMFAChallenge *orochi_auth_create_challenge(pg_uuid_t factor_id,
                                                            const char *ip_address);
extern bool orochi_auth_verify_challenge(pg_uuid_t challenge_id, const char *code);

/* ============================================================
 * Identity Linking Functions
 * ============================================================ */

extern OrochiAuthIdentity *orochi_auth_link_identity(pg_uuid_t user_id, OrochiAuthProvider provider,
                                                     const char *provider_id, Jsonb *identity_data);

extern bool orochi_auth_unlink_identity(pg_uuid_t identity_id);
extern List *orochi_auth_list_identities(pg_uuid_t user_id);

/* ============================================================
 * Token Generation Utilities
 * ============================================================ */

extern char *orochi_auth_generate_token(int length);
extern char *orochi_auth_generate_otp(int length);
extern char *orochi_auth_hash_token(const char *token);
extern bool orochi_auth_verify_token_hash(const char *token, const char *hash);

/* ============================================================
 * Configuration Functions
 * ============================================================ */

extern OrochiAuthConfig *orochi_auth_get_config(void);
extern bool orochi_auth_update_config(OrochiAuthConfig *config);

/* ============================================================
 * Shared Memory Functions
 * ============================================================ */

extern Size orochi_auth_shmem_size(void);
extern void orochi_auth_shmem_init(void);

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

/* User management */
extern Datum orochi_auth_create_user_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_get_user_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_update_user_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_delete_user_sql(PG_FUNCTION_ARGS);

/* Authentication */
extern Datum orochi_auth_sign_in_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_sign_out_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_refresh_token_sql(PG_FUNCTION_ARGS);

/* Session management */
extern Datum orochi_auth_get_session_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_list_sessions_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_revoke_session_sql(PG_FUNCTION_ARGS);

/* JWT context */
extern Datum orochi_auth_uid_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_role_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_jwt_sql(PG_FUNCTION_ARGS);
extern Datum orochi_auth_set_jwt_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *orochi_auth_aal_name(OrochiAuthAAL aal);
extern const char *orochi_auth_factor_type_name(OrochiAuthFactorType type);
extern const char *orochi_auth_token_type_name(OrochiAuthTokenType type);
extern const char *orochi_auth_provider_name(OrochiAuthProvider provider);
extern OrochiAuthProvider orochi_auth_parse_provider(const char *name);

#endif /* OROCHI_GOTRUE_H */
