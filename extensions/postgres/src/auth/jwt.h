/*-------------------------------------------------------------------------
 *
 * jwt.h
 *    JWT (JSON Web Token) validation header for Orochi DB
 *
 * This module provides JWT parsing, validation, and signature verification
 * for the authentication system. It supports:
 *   - HMAC-SHA256/384/512 signature verification
 *   - RSA-SHA256/384/512 signature verification
 *   - Base64URL encoding/decoding
 *   - Token expiry and not-before validation
 *   - PostgreSQL session variable integration for RLS
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JWT_H
#define OROCHI_JWT_H

#include "fmgr.h"
#include "postgres.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"

/* ============================================================
 * JWT Algorithm Types
 * Supported signing algorithms per RFC 7518
 * ============================================================ */
typedef enum OrochiJwtAlgorithm {
  OROCHI_JWT_ALG_NONE = 0, /* Unsigned (not recommended) */
  OROCHI_JWT_HS256,        /* HMAC-SHA256 */
  OROCHI_JWT_HS384,        /* HMAC-SHA384 */
  OROCHI_JWT_HS512,        /* HMAC-SHA512 */
  OROCHI_JWT_RS256,        /* RSA-SHA256 */
  OROCHI_JWT_RS384,        /* RSA-SHA384 */
  OROCHI_JWT_RS512,        /* RSA-SHA512 */
  OROCHI_JWT_ES256,        /* ECDSA-SHA256 */
  OROCHI_JWT_ES384,        /* ECDSA-SHA384 */
  OROCHI_JWT_ES512         /* ECDSA-SHA512 */
} OrochiJwtAlgorithm;

/* ============================================================
 * JWT Validation Status
 * ============================================================ */
typedef enum OrochiJwtStatus {
  OROCHI_JWT_STATUS_VALID = 0,
  OROCHI_JWT_STATUS_INVALID_FORMAT,
  OROCHI_JWT_STATUS_INVALID_HEADER,
  OROCHI_JWT_STATUS_INVALID_PAYLOAD,
  OROCHI_JWT_STATUS_INVALID_SIGNATURE,
  OROCHI_JWT_STATUS_EXPIRED,
  OROCHI_JWT_STATUS_NOT_YET_VALID,
  OROCHI_JWT_STATUS_UNSUPPORTED_ALGORITHM,
  OROCHI_JWT_STATUS_DECODE_ERROR,
  OROCHI_JWT_STATUS_VERIFICATION_ERROR
} OrochiJwtStatus;

/* ============================================================
 * JWT Header Structure
 * Contains algorithm and token type information
 * ============================================================ */
typedef struct OrochiJwtHeader {
  OrochiJwtAlgorithm alg; /* Signing algorithm */
  char *typ;              /* Token type (usually "JWT") */
  char *kid;              /* Key ID for key rotation */
  char *raw_json;         /* Original JSON string */
} OrochiJwtHeader;

/* ============================================================
 * Standard JWT Claims (RFC 7519)
 * ============================================================ */
typedef struct OrochiJwtStandardClaims {
  /* Registered claims */
  char *iss;       /* Issuer */
  char *sub;       /* Subject (user ID) */
  char *aud;       /* Audience */
  TimestampTz exp; /* Expiration time */
  TimestampTz nbf; /* Not before time */
  TimestampTz iat; /* Issued at time */
  char *jti;       /* JWT ID (unique identifier) */

  /* Unix timestamps for direct comparison */
  int64 exp_unix;
  int64 nbf_unix;
  int64 iat_unix;
} OrochiJwtStandardClaims;

/* ============================================================
 * Custom JWT Claims Structure
 * For application-specific claims (auth context)
 * ============================================================ */
typedef struct OrochiJwtCustomClaims {
  /* Supabase/GoTrue compatible claims */
  char *email;      /* User email */
  char *phone;      /* User phone */
  char *role;       /* User role */
  char *session_id; /* Session identifier */
  char *aal;        /* Authenticator Assurance Level */

  /* Metadata */
  Jsonb *app_metadata;  /* Application metadata */
  Jsonb *user_metadata; /* User metadata */
  Jsonb *amr;           /* Authentication Methods References */

  /* Custom claims container */
  Jsonb *custom; /* Additional custom claims */
} OrochiJwtCustomClaims;

/* ============================================================
 * Combined JWT Claims Structure
 * ============================================================ */
typedef struct OrochiJwtClaims {
  OrochiJwtStandardClaims standard;
  OrochiJwtCustomClaims custom;
  Jsonb *raw_payload; /* Full payload as JSONB */
} OrochiJwtClaims;

/* ============================================================
 * JWT Token Structure
 * Complete parsed and validated token
 * ============================================================ */
typedef struct OrochiJwt {
  /* Token parts */
  OrochiJwtHeader *header; /* Decoded header */
  OrochiJwtClaims *claims; /* Decoded claims/payload */
  char *signature;         /* Raw signature bytes */
  Size signature_len;      /* Signature length */

  /* Original token parts (for signature verification) */
  char *header_b64;    /* Base64URL encoded header */
  char *payload_b64;   /* Base64URL encoded payload */
  char *signature_b64; /* Base64URL encoded signature */

  /* Validation state */
  OrochiJwtStatus status; /* Validation status */
  bool is_valid;          /* Overall validity flag */
  char *error_message;    /* Error description if invalid */

  /* Memory context for allocations */
  MemoryContext context;
} OrochiJwt;

/* ============================================================
 * JWT Validation Options
 * ============================================================ */
typedef struct OrochiJwtValidationOptions {
  bool verify_signature; /* Verify cryptographic signature */
  bool verify_exp;       /* Verify expiration */
  bool verify_nbf;       /* Verify not-before */
  bool verify_iat;       /* Verify issued-at */

  const char *required_iss; /* Required issuer (NULL to skip) */
  const char *required_aud; /* Required audience (NULL to skip) */

  int64 clock_skew_seconds; /* Allowed clock skew */
} OrochiJwtValidationOptions;

/* Default validation options */
#define OROCHI_JWT_DEFAULT_OPTIONS                                             \
  {.verify_signature = true,                                                   \
   .verify_exp = true,                                                         \
   .verify_nbf = true,                                                         \
   .verify_iat = false,                                                        \
   .required_iss = NULL,                                                       \
   .required_aud = NULL,                                                       \
   .clock_skew_seconds = 0}

/* ============================================================
 * Core JWT Functions
 * ============================================================ */

/*
 * Parse a JWT token string into components
 * Does NOT verify signature - use orochi_jwt_verify for that
 */
extern OrochiJwt *orochi_jwt_parse(const char *token);

/*
 * Decode and validate a JWT token
 * Full validation including signature verification
 */
extern OrochiJwt *orochi_jwt_decode(const char *token, const char *secret,
                                    OrochiJwtAlgorithm expected_alg);

/*
 * Decode with custom validation options
 */
extern OrochiJwt *
orochi_jwt_decode_with_options(const char *token, const char *secret,
                               OrochiJwtAlgorithm expected_alg,
                               OrochiJwtValidationOptions *options);

/*
 * Verify JWT signature
 * Returns true if signature is valid
 */
extern bool orochi_jwt_verify_signature(const char *header_payload,
                                        const char *signature_b64,
                                        const char *secret,
                                        OrochiJwtAlgorithm alg);

/*
 * Free JWT structure
 */
extern void orochi_jwt_free(OrochiJwt *jwt);

/* ============================================================
 * Base64URL Encoding/Decoding
 * ============================================================ */

/*
 * Decode Base64URL string
 * Returns decoded bytes, sets output_len
 */
extern char *orochi_base64url_decode(const char *input, Size *output_len);

/*
 * Encode bytes to Base64URL string
 */
extern char *orochi_base64url_encode(const unsigned char *input,
                                     Size input_len);

/* ============================================================
 * JWT Header Functions
 * ============================================================ */

/*
 * Parse JWT header from JSON
 */
extern OrochiJwtHeader *orochi_jwt_parse_header(const char *json_str);

/*
 * Get algorithm from string name
 */
extern OrochiJwtAlgorithm orochi_jwt_algorithm_from_string(const char *alg_str);

/*
 * Get string name from algorithm
 */
extern const char *orochi_jwt_algorithm_to_string(OrochiJwtAlgorithm alg);

/* ============================================================
 * JWT Claims Functions
 * ============================================================ */

/*
 * Parse JWT claims/payload from JSON
 */
extern OrochiJwtClaims *orochi_jwt_parse_claims(const char *json_str);

/*
 * Get a specific claim value as text
 */
extern char *orochi_jwt_get_claim_text(OrochiJwt *jwt, const char *claim_name);

/*
 * Get a specific claim value as JSONB
 */
extern Jsonb *orochi_jwt_get_claim_jsonb(OrochiJwt *jwt,
                                         const char *claim_name);

/*
 * Check if a claim exists
 */
extern bool orochi_jwt_has_claim(OrochiJwt *jwt, const char *claim_name);

/*
 * Get claim as integer
 */
extern int64 orochi_jwt_get_claim_int(OrochiJwt *jwt, const char *claim_name,
                                      bool *is_null);

/*
 * Get claim as boolean
 */
extern bool orochi_jwt_get_claim_bool(OrochiJwt *jwt, const char *claim_name,
                                      bool *is_null);

/* ============================================================
 * PostgreSQL Session Integration
 * ============================================================ */

/*
 * Set JWT claims in PostgreSQL session variables
 * This enables auth.uid(), auth.jwt(), etc. to work
 */
extern void orochi_jwt_set_session_context(OrochiJwt *jwt);

/*
 * Clear JWT session context
 * Called when releasing connection back to pool
 */
extern void orochi_jwt_clear_session_context(void);

/*
 * Get current session claim value
 */
extern char *orochi_jwt_get_session_claim(const char *claim_name);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Get human-readable error message for status
 */
extern const char *orochi_jwt_status_message(OrochiJwtStatus status);

/*
 * Check if token is expired (with optional clock skew)
 */
extern bool orochi_jwt_is_expired(OrochiJwt *jwt, int64 clock_skew_seconds);

/*
 * Check if token is not yet valid (with optional clock skew)
 */
extern bool orochi_jwt_is_not_yet_valid(OrochiJwt *jwt,
                                        int64 clock_skew_seconds);

/*
 * Convert Unix timestamp to PostgreSQL TimestampTz
 */
extern TimestampTz orochi_unix_to_timestamptz(int64 unix_timestamp);

/*
 * Convert PostgreSQL TimestampTz to Unix timestamp
 */
extern int64 orochi_timestamptz_to_unix(TimestampTz ts);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

/* Validate a JWT and return boolean */
extern Datum orochi_jwt_verify(PG_FUNCTION_ARGS);

/* Decode JWT and return claims as JSONB */
extern Datum orochi_jwt_decode_claims(PG_FUNCTION_ARGS);

/* Get a specific claim from a JWT */
extern Datum orochi_jwt_get_claim(PG_FUNCTION_ARGS);

/* Set auth context from JWT (for connection pooler) */
extern Datum orochi_jwt_set_auth_context(PG_FUNCTION_ARGS);

/* Clear auth context */
extern Datum orochi_jwt_clear_auth_context(PG_FUNCTION_ARGS);

/* Encode claims to JWT */
extern Datum orochi_jwt_encode(PG_FUNCTION_ARGS);

#endif /* OROCHI_JWT_H */
