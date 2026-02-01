/*-------------------------------------------------------------------------
 *
 * webauthn.h
 *    Orochi DB WebAuthn/FIDO2 Passwordless Authentication
 *
 * This module implements WebAuthn/FIDO2 passwordless authentication for
 * Orochi DB, providing:
 *   - Credential registration and storage
 *   - Challenge generation and verification
 *   - Device/authenticator management
 *   - Sign count verification for replay attack protection
 *
 * Based on WebAuthn Level 2 specification (W3C Recommendation).
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_WEBAUTHN_H
#define OROCHI_WEBAUTHN_H

#include "postgres.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* ============================================================
 * Constants
 * ============================================================ */

/* Challenge configuration */
#define WEBAUTHN_CHALLENGE_SIZE 32         /* 32 bytes random challenge */
#define WEBAUTHN_CHALLENGE_TTL_SECONDS 300 /* 5 minute expiry */

/* Credential limits */
#define WEBAUTHN_MAX_CREDENTIAL_ID_LEN 1024 /* Max credential ID length */
#define WEBAUTHN_MAX_PUBLIC_KEY_LEN 512     /* Max public key length */
#define WEBAUTHN_MAX_DEVICE_NAME_LEN 255    /* Max device name length */
#define WEBAUTHN_MAX_TRANSPORTS 8           /* Max transport types */

/* COSE algorithm identifiers (RFC 8152) */
#define COSE_ALG_ES256 -7   /* ECDSA with SHA-256 */
#define COSE_ALG_ES384 -35  /* ECDSA with SHA-384 */
#define COSE_ALG_ES512 -36  /* ECDSA with SHA-512 */
#define COSE_ALG_RS256 -257 /* RSASSA-PKCS1-v1_5 with SHA-256 */
#define COSE_ALG_RS384 -258 /* RSASSA-PKCS1-v1_5 with SHA-384 */
#define COSE_ALG_RS512 -259 /* RSASSA-PKCS1-v1_5 with SHA-512 */
#define COSE_ALG_PS256 -37  /* RSASSA-PSS with SHA-256 */
#define COSE_ALG_EDDSA -8   /* EdDSA */

/* Authenticator flags (from authenticatorData) */
#define WEBAUTHN_FLAG_UP 0x01 /* User Present */
#define WEBAUTHN_FLAG_UV 0x04 /* User Verified */
#define WEBAUTHN_FLAG_BE 0x08 /* Backup Eligible */
#define WEBAUTHN_FLAG_BS 0x10 /* Backup State */
#define WEBAUTHN_FLAG_AT 0x40 /* Attested credential data */
#define WEBAUTHN_FLAG_ED 0x80 /* Extension data */

/* ============================================================
 * Enumerations
 * ============================================================ */

/*
 * WebAuthn credential type
 */
typedef enum WebAuthnCredentialType {
  WEBAUTHN_CRED_PASSKEY = 0,   /* Platform/roaming passkey */
  WEBAUTHN_CRED_SECURITY_KEY,  /* Hardware security key (YubiKey) */
  WEBAUTHN_CRED_PLATFORM,      /* Platform authenticator (Touch ID) */
  WEBAUTHN_CRED_CROSS_PLATFORM /* Cross-platform (USB security key) */
} WebAuthnCredentialType;

/*
 * Authenticator/device type
 */
typedef enum WebAuthnDeviceType {
  WEBAUTHN_DEVICE_UNKNOWN = 0,
  WEBAUTHN_DEVICE_WINDOWS_HELLO,
  WEBAUTHN_DEVICE_APPLE_TOUCH_ID,
  WEBAUTHN_DEVICE_APPLE_FACE_ID,
  WEBAUTHN_DEVICE_ANDROID_BIOMETRIC,
  WEBAUTHN_DEVICE_YUBIKEY,
  WEBAUTHN_DEVICE_GOOGLE_TITAN,
  WEBAUTHN_DEVICE_OTHER
} WebAuthnDeviceType;

/*
 * Credential status
 */
typedef enum WebAuthnCredentialStatus {
  WEBAUTHN_STATUS_PENDING = 0, /* Pending verification */
  WEBAUTHN_STATUS_ACTIVE,      /* Active and usable */
  WEBAUTHN_STATUS_SUSPENDED,   /* Temporarily suspended */
  WEBAUTHN_STATUS_REVOKED      /* Permanently revoked */
} WebAuthnCredentialStatus;

/*
 * Challenge type
 */
typedef enum WebAuthnChallengeType {
  WEBAUTHN_CHALLENGE_REGISTRATION = 0,
  WEBAUTHN_CHALLENGE_AUTHENTICATION
} WebAuthnChallengeType;

/*
 * User verification requirement
 */
typedef enum WebAuthnUserVerification {
  WEBAUTHN_UV_REQUIRED = 0, /* UV required */
  WEBAUTHN_UV_PREFERRED,    /* UV preferred but optional */
  WEBAUTHN_UV_DISCOURAGED   /* UV not required */
} WebAuthnUserVerification;

/*
 * Attestation conveyance preference
 */
typedef enum WebAuthnAttestation {
  WEBAUTHN_ATT_NONE = 0,  /* No attestation */
  WEBAUTHN_ATT_INDIRECT,  /* Indirect attestation */
  WEBAUTHN_ATT_DIRECT,    /* Direct attestation */
  WEBAUTHN_ATT_ENTERPRISE /* Enterprise attestation */
} WebAuthnAttestation;

/*
 * Transport types for authenticator
 */
typedef enum WebAuthnTransport {
  WEBAUTHN_TRANSPORT_USB = 0,
  WEBAUTHN_TRANSPORT_NFC,
  WEBAUTHN_TRANSPORT_BLE,
  WEBAUTHN_TRANSPORT_INTERNAL, /* Platform authenticator */
  WEBAUTHN_TRANSPORT_HYBRID    /* Cross-device (caBLE) */
} WebAuthnTransport;

/*
 * Verification result
 */
typedef enum WebAuthnVerifyResult {
  WEBAUTHN_VERIFY_SUCCESS = 0,
  WEBAUTHN_VERIFY_INVALID_CHALLENGE,
  WEBAUTHN_VERIFY_EXPIRED_CHALLENGE,
  WEBAUTHN_VERIFY_CREDENTIAL_NOT_FOUND,
  WEBAUTHN_VERIFY_INVALID_SIGNATURE,
  WEBAUTHN_VERIFY_REPLAY_DETECTED,
  WEBAUTHN_VERIFY_USER_NOT_FOUND,
  WEBAUTHN_VERIFY_CREDENTIAL_SUSPENDED,
  WEBAUTHN_VERIFY_CREDENTIAL_REVOKED,
  WEBAUTHN_VERIFY_INTERNAL_ERROR
} WebAuthnVerifyResult;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiWebAuthnCredential - Stored credential for a user
 */
typedef struct OrochiWebAuthnCredential {
  /* Identifiers */
  pg_uuid_t credential_id; /* Internal credential UUID */
  pg_uuid_t user_id;       /* User who owns this credential */

  /* WebAuthn credential data */
  uint8 *webauthn_credential_id; /* Authenticator's credential ID */
  int32 credential_id_len;       /* Length of credential ID */
  uint8 *public_key_spki;        /* Public key in SPKI format */
  int32 public_key_len;          /* Length of public key */
  int32 public_key_algorithm;    /* COSE algorithm identifier */

  /* Credential metadata */
  WebAuthnCredentialType credential_type;
  char device_name[WEBAUTHN_MAX_DEVICE_NAME_LEN + 1];
  WebAuthnDeviceType device_type;
  WebAuthnTransport transports[WEBAUTHN_MAX_TRANSPORTS];
  int transport_count;

  /* Security counters */
  int64 sign_count;     /* Signature counter */
  bool backup_eligible; /* Can credential be backed up */
  bool backup_state;    /* Is credential currently backed up */

  /* Attestation data (optional) */
  char *attestation_format;    /* e.g., "packed", "tpm" */
  char *attestation_statement; /* JSON attestation data */
  uint8 aaguid[16];            /* Authenticator AAGUID */

  /* Status and timestamps */
  WebAuthnCredentialStatus status;
  TimestampTz last_used_at;
  char *last_used_ip;
  char *last_used_user_agent;
  TimestampTz created_at;
  TimestampTz revoked_at;
  pg_uuid_t *revoked_by; /* User who revoked (if any) */
  char *revoked_reason;
} OrochiWebAuthnCredential;

/*
 * WebAuthnChallenge - Challenge for registration or authentication
 */
typedef struct WebAuthnChallenge {
  pg_uuid_t challenge_id; /* Challenge identifier */
  uint8 challenge[WEBAUTHN_CHALLENGE_SIZE];
  WebAuthnChallengeType challenge_type;
  pg_uuid_t *user_id; /* May be NULL for discoverable creds */

  /* Relying Party configuration */
  char *rp_id;   /* e.g., "orochi.cloud" */
  char *rp_name; /* e.g., "Orochi Cloud" */
  char *origin;  /* e.g., "https://orochi.cloud" */

  /* Request configuration */
  WebAuthnUserVerification user_verification;
  WebAuthnAttestation attestation;

  /* Context */
  char *ip_address;
  char *user_agent;

  /* Timestamps */
  TimestampTz created_at;
  TimestampTz expires_at;
  TimestampTz consumed_at; /* NULL if not yet used */
} WebAuthnChallenge;

/*
 * WebAuthnRegistrationOptions - Options for credential creation
 */
typedef struct WebAuthnRegistrationOptions {
  WebAuthnChallenge *challenge;
  pg_uuid_t user_id;
  char *user_name; /* Usually email */
  char *user_display_name;

  /* Public key credential parameters */
  int32 allowed_algorithms[8];
  int algorithm_count;

  /* Authenticator selection */
  WebAuthnCredentialType authenticator_attachment;
  bool require_resident_key;
  WebAuthnUserVerification user_verification;

  /* Timeout in milliseconds */
  int32 timeout_ms;

  /* Exclude existing credentials (prevent re-registration) */
  pg_uuid_t *exclude_credentials;
  int exclude_count;
} WebAuthnRegistrationOptions;

/*
 * WebAuthnAuthenticationOptions - Options for assertion
 */
typedef struct WebAuthnAuthenticationOptions {
  WebAuthnChallenge *challenge;

  /* Allowed credentials (empty for discoverable) */
  uint8 **allowed_credential_ids;
  int32 *allowed_credential_lens;
  int allowed_count;

  /* Configuration */
  WebAuthnUserVerification user_verification;
  int32 timeout_ms;
} WebAuthnAuthenticationOptions;

/*
 * WebAuthnRegistrationResult - Result of credential registration
 */
typedef struct WebAuthnRegistrationResult {
  bool success;
  pg_uuid_t credential_id; /* New credential ID */
  char *error_message;
} WebAuthnRegistrationResult;

/*
 * WebAuthnAuthenticationResult - Result of authentication
 */
typedef struct WebAuthnAuthenticationResult {
  WebAuthnVerifyResult result;
  pg_uuid_t user_id;
  pg_uuid_t credential_id;
  char *session_token; /* If authentication successful */
  char *error_message;
} WebAuthnAuthenticationResult;

/*
 * WebAuthnDeviceInfo - Information about registered device
 */
typedef struct WebAuthnDeviceInfo {
  pg_uuid_t credential_id;
  char *device_name;
  WebAuthnDeviceType device_type;
  WebAuthnCredentialType credential_type;
  WebAuthnTransport transports[WEBAUTHN_MAX_TRANSPORTS];
  int transport_count;
  WebAuthnCredentialStatus status;
  TimestampTz last_used_at;
  TimestampTz created_at;
} WebAuthnDeviceInfo;

/* ============================================================
 * Function Declarations
 * ============================================================ */

/* Initialization */
extern void webauthn_init(void);
extern Size webauthn_shmem_size(void);

/* Challenge generation */
extern WebAuthnChallenge *webauthn_create_registration_challenge(
    pg_uuid_t user_id, const char *rp_id, const char *rp_name,
    const char *origin, WebAuthnUserVerification user_verification);

extern WebAuthnChallenge *
webauthn_create_authentication_challenge(const char *user_email,
                                         const char *rp_id, const char *origin);

extern bool webauthn_validate_challenge(pg_uuid_t challenge_id,
                                        WebAuthnChallengeType expected_type);

extern void webauthn_consume_challenge(pg_uuid_t challenge_id);

/* Credential registration */
extern WebAuthnRegistrationResult *webauthn_complete_registration(
    pg_uuid_t challenge_id, const uint8 *credential_id, int32 credential_id_len,
    const uint8 *public_key, int32 public_key_len, int32 algorithm,
    int64 sign_count, const WebAuthnTransport *transports, int transport_count,
    const char *device_name);

/* Authentication verification */
extern WebAuthnAuthenticationResult *webauthn_verify_authentication(
    pg_uuid_t challenge_id, const uint8 *credential_id, int32 credential_id_len,
    const uint8 *authenticator_data, int32 authenticator_data_len,
    const uint8 *signature, int32 signature_len, const char *client_data_json,
    const char *ip_address, const char *user_agent);

/* Signature verification (crypto operations) */
extern bool webauthn_verify_signature(
    const uint8 *public_key, int32 public_key_len, int32 algorithm,
    const uint8 *authenticator_data, int32 authenticator_data_len,
    const uint8 *client_data_hash, const uint8 *signature, int32 signature_len);

/* Sign count validation */
extern bool webauthn_validate_sign_count(OrochiWebAuthnCredential *credential,
                                         int64 new_sign_count);

/* Credential management */
extern OrochiWebAuthnCredential *
webauthn_get_credential(pg_uuid_t credential_id);

extern OrochiWebAuthnCredential *
webauthn_get_credential_by_webauthn_id(const uint8 *webauthn_credential_id,
                                       int32 credential_id_len);

extern List *webauthn_get_user_credentials(pg_uuid_t user_id);

extern bool webauthn_revoke_credential(pg_uuid_t user_id,
                                       pg_uuid_t credential_id,
                                       const char *reason);

extern bool webauthn_suspend_credential(pg_uuid_t credential_id,
                                        const char *reason);

extern bool webauthn_reactivate_credential(pg_uuid_t credential_id);

extern int webauthn_get_active_credential_count(pg_uuid_t user_id);

/* Device listing */
extern List *webauthn_list_user_devices(pg_uuid_t user_id);
extern void webauthn_free_device_list(List *devices);

/* Utility functions */
extern const char *webauthn_credential_type_name(WebAuthnCredentialType type);
extern const char *webauthn_device_type_name(WebAuthnDeviceType type);
extern const char *webauthn_status_name(WebAuthnCredentialStatus status);
extern const char *webauthn_transport_name(WebAuthnTransport transport);
extern const char *webauthn_verify_result_message(WebAuthnVerifyResult result);

extern int32 webauthn_extract_sign_count(const uint8 *authenticator_data,
                                         int32 data_len);

extern bool webauthn_extract_flags(const uint8 *authenticator_data,
                                   int32 data_len, bool *user_present,
                                   bool *user_verified, bool *backup_eligible,
                                   bool *backup_state);

/* Hash functions */
extern void webauthn_sha256(const uint8 *data, int32 data_len, uint8 *hash_out);

/* Cleanup */
extern void webauthn_cleanup_expired_challenges(void);
extern void webauthn_free_credential(OrochiWebAuthnCredential *credential);
extern void webauthn_free_challenge(WebAuthnChallenge *challenge);

/* SQL-callable functions */
extern Datum webauthn_begin_registration(PG_FUNCTION_ARGS);
extern Datum webauthn_complete_registration_sql(PG_FUNCTION_ARGS);
extern Datum webauthn_begin_authentication(PG_FUNCTION_ARGS);
extern Datum webauthn_verify_authentication_sql(PG_FUNCTION_ARGS);
extern Datum webauthn_list_credentials(PG_FUNCTION_ARGS);
extern Datum webauthn_revoke_credential_sql(PG_FUNCTION_ARGS);

#endif /* OROCHI_WEBAUTHN_H */
