/*-------------------------------------------------------------------------
 *
 * webauthn.c
 *    Orochi DB WebAuthn/FIDO2 Passwordless Authentication Implementation
 *
 * This module implements WebAuthn/FIDO2 passwordless authentication:
 *   - Challenge generation with cryptographic randomness
 *   - Credential registration with SPKI public key storage
 *   - Authentication verification with signature validation
 *   - Sign count validation for replay attack protection
 *   - Device/credential lifecycle management
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <string.h>

#include "webauthn.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Memory context for WebAuthn operations */
static MemoryContext WebAuthnContext = NULL;

/* Shared state for caching */
typedef struct WebAuthnSharedState {
    LWLock *challenge_lock;
    int32 active_challenges;
    uint64 total_registrations;
    uint64 total_authentications;
    uint64 failed_authentications;
} WebAuthnSharedState;

static WebAuthnSharedState *webauthn_shared_state = NULL;

/* Forward declarations */
static void generate_random_bytes(uint8 *buffer, int length);
static pg_uuid_t generate_uuid(void);
static char *uuid_to_string(pg_uuid_t uuid);
static bool parse_uuid(const char *str, pg_uuid_t *uuid);

/* ============================================================
 * Initialization
 * ============================================================ */

/*
 * webauthn_init - Initialize WebAuthn module
 */
void webauthn_init(void)
{
    if (WebAuthnContext == NULL) {
        WebAuthnContext =
            AllocSetContextCreate(TopMemoryContext, "WebAuthnContext", ALLOCSET_DEFAULT_SIZES);
    }

    elog(LOG, "WebAuthn authentication module initialized");
}

/*
 * webauthn_shmem_size - Calculate shared memory size needed
 */
Size webauthn_shmem_size(void)
{
    return sizeof(WebAuthnSharedState);
}

/* ============================================================
 * Challenge Management
 * ============================================================ */

/*
 * webauthn_create_registration_challenge - Create challenge for credential
 * registration
 */
WebAuthnChallenge *
webauthn_create_registration_challenge(pg_uuid_t user_id, const char *rp_id, const char *rp_name,
                                       const char *origin,
                                       WebAuthnUserVerification user_verification)
{
    MemoryContext old_context;
    WebAuthnChallenge *challenge;
    StringInfoData query;
    int ret;

    if (rp_id == NULL || rp_name == NULL || origin == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("rp_id, rp_name, and origin are required")));
    }

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    challenge = (WebAuthnChallenge *)palloc0(sizeof(WebAuthnChallenge));

    /* Generate challenge ID and random challenge bytes */
    challenge->challenge_id = generate_uuid();
    generate_random_bytes(challenge->challenge, WEBAUTHN_CHALLENGE_SIZE);

    challenge->challenge_type = WEBAUTHN_CHALLENGE_REGISTRATION;
    challenge->user_id = (pg_uuid_t *)palloc(sizeof(pg_uuid_t));
    memcpy(challenge->user_id, &user_id, sizeof(pg_uuid_t));

    challenge->rp_id = pstrdup(rp_id);
    challenge->rp_name = pstrdup(rp_name);
    challenge->origin = pstrdup(origin);
    challenge->user_verification = user_verification;
    challenge->attestation = WEBAUTHN_ATT_NONE;

    challenge->created_at = GetCurrentTimestamp();
    challenge->expires_at =
        TimestampTzPlusMilliseconds(challenge->created_at, WEBAUTHN_CHALLENGE_TTL_SECONDS * 1000);
    challenge->consumed_at = 0; /* Not yet consumed */

    MemoryContextSwitchTo(old_context);

    /* Store challenge in database */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.webauthn_challenges "
                     "(challenge_id, challenge, challenge_type, user_id, rp_id, rp_name, "
                     "origin, user_verification, expires_at) "
                     "VALUES ('%s', E'\\\\x",
                     uuid_to_string(challenge->challenge_id));

    for (int i = 0; i < WEBAUTHN_CHALLENGE_SIZE; i++)
        appendStringInfo(&query, "%02x", challenge->challenge[i]);

    appendStringInfo(&query,
                     "', 'registration', '%s', '%s', '%s', '%s', '%s', "
                     "NOW() + INTERVAL '%d seconds')",
                     uuid_to_string(user_id), rp_id, rp_name, origin,
                     user_verification == WEBAUTHN_UV_REQUIRED    ? "required"
                     : user_verification == WEBAUTHN_UV_PREFERRED ? "preferred"
                                                                  : "discouraged",
                     WEBAUTHN_CHALLENGE_TTL_SECONDS);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    if (ret != SPI_OK_INSERT) {
        elog(WARNING, "Failed to store WebAuthn challenge");
    }

    if (webauthn_shared_state != NULL) {
        LWLockAcquire(webauthn_shared_state->challenge_lock, LW_EXCLUSIVE);
        webauthn_shared_state->active_challenges++;
        LWLockRelease(webauthn_shared_state->challenge_lock);
    }

    elog(DEBUG1, "Created WebAuthn registration challenge for user %s", uuid_to_string(user_id));

    return challenge;
}

/*
 * webauthn_create_authentication_challenge - Create challenge for
 * authentication
 */
WebAuthnChallenge *webauthn_create_authentication_challenge(const char *user_email,
                                                            const char *rp_id, const char *origin)
{
    MemoryContext old_context;
    WebAuthnChallenge *challenge;
    StringInfoData query;
    pg_uuid_t user_id;
    bool user_found = false;
    int ret;

    if (rp_id == NULL || origin == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("rp_id and origin are required")));
    }

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    challenge = (WebAuthnChallenge *)palloc0(sizeof(WebAuthnChallenge));

    /* Generate challenge */
    challenge->challenge_id = generate_uuid();
    generate_random_bytes(challenge->challenge, WEBAUTHN_CHALLENGE_SIZE);
    challenge->challenge_type = WEBAUTHN_CHALLENGE_AUTHENTICATION;

    /* Look up user by email if provided */
    if (user_email != NULL && user_email[0] != '\0') {
        initStringInfo(&query);
        appendStringInfo(&query,
                         "SELECT user_id FROM auth.users WHERE email = '%s' AND "
                         "status = 'active'",
                         user_email);

        SPI_connect();
        ret = SPI_execute(query.data, true, 1);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            HeapTuple tuple = SPI_tuptable->vals[0];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;
            Datum user_id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);

            if (!isnull) {
                pg_uuid_t *uuid_ptr = DatumGetUUIDP(user_id_datum);
                memcpy(&user_id, uuid_ptr, sizeof(pg_uuid_t));
                user_found = true;

                challenge->user_id = (pg_uuid_t *)palloc(sizeof(pg_uuid_t));
                memcpy(challenge->user_id, &user_id, sizeof(pg_uuid_t));
            }
        }

        SPI_finish();
        pfree(query.data);
    }

    /* Even if user not found, create challenge to prevent user enumeration */
    challenge->rp_id = pstrdup(rp_id);
    challenge->rp_name = pstrdup("Orochi Cloud");
    challenge->origin = pstrdup(origin);
    challenge->user_verification = WEBAUTHN_UV_PREFERRED;

    challenge->created_at = GetCurrentTimestamp();
    challenge->expires_at =
        TimestampTzPlusMilliseconds(challenge->created_at, WEBAUTHN_CHALLENGE_TTL_SECONDS * 1000);
    challenge->consumed_at = 0;

    MemoryContextSwitchTo(old_context);

    /* Store challenge */
    if (user_found) {
        initStringInfo(&query);
        appendStringInfo(&query,
                         "INSERT INTO auth.webauthn_challenges "
                         "(challenge_id, challenge, challenge_type, user_id, rp_id, rp_name, "
                         "origin, expires_at) VALUES ('%s', E'\\\\x",
                         uuid_to_string(challenge->challenge_id));

        for (int i = 0; i < WEBAUTHN_CHALLENGE_SIZE; i++)
            appendStringInfo(&query, "%02x", challenge->challenge[i]);

        appendStringInfo(&query,
                         "', 'authentication', '%s', '%s', 'Orochi Cloud', '%s', "
                         "NOW() + INTERVAL '%d seconds')",
                         uuid_to_string(user_id), rp_id, origin, WEBAUTHN_CHALLENGE_TTL_SECONDS);

        SPI_connect();
        SPI_execute(query.data, false, 0);
        SPI_finish();
        pfree(query.data);
    }

    elog(DEBUG1, "Created WebAuthn authentication challenge%s",
         user_found ? " for known user" : " (user not found)");

    return challenge;
}

/*
 * webauthn_validate_challenge - Check if challenge is valid and not expired
 */
bool webauthn_validate_challenge(pg_uuid_t challenge_id, WebAuthnChallengeType expected_type)
{
    StringInfoData query;
    bool is_valid = false;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT 1 FROM auth.webauthn_challenges "
                     "WHERE challenge_id = '%s' "
                     "AND challenge_type = '%s' "
                     "AND consumed_at IS NULL "
                     "AND expires_at > NOW()",
                     uuid_to_string(challenge_id),
                     expected_type == WEBAUTHN_CHALLENGE_REGISTRATION ? "registration"
                                                                      : "authentication");

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
        is_valid = true;

    SPI_finish();
    pfree(query.data);

    return is_valid;
}

/*
 * webauthn_consume_challenge - Mark challenge as consumed
 */
void webauthn_consume_challenge(pg_uuid_t challenge_id)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webauthn_challenges SET consumed_at = NOW() "
                     "WHERE challenge_id = '%s'",
                     uuid_to_string(challenge_id));

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    if (webauthn_shared_state != NULL) {
        LWLockAcquire(webauthn_shared_state->challenge_lock, LW_EXCLUSIVE);
        if (webauthn_shared_state->active_challenges > 0)
            webauthn_shared_state->active_challenges--;
        LWLockRelease(webauthn_shared_state->challenge_lock);
    }
}

/* ============================================================
 * Credential Registration
 * ============================================================ */

/*
 * webauthn_complete_registration - Complete credential registration
 */
WebAuthnRegistrationResult *webauthn_complete_registration(
    pg_uuid_t challenge_id, const uint8 *credential_id, int32 credential_id_len,
    const uint8 *public_key, int32 public_key_len, int32 algorithm, int64 sign_count,
    const WebAuthnTransport *transports, int transport_count, const char *device_name)
{
    MemoryContext old_context;
    WebAuthnRegistrationResult *result;
    StringInfoData query;
    StringInfoData cred_hex;
    StringInfoData pubkey_hex;
    StringInfoData transports_array;
    pg_uuid_t new_credential_id;
    pg_uuid_t user_id;
    int ret;
    int i;
    bool found_challenge = false;

    /* Validate inputs */
    if (credential_id == NULL || credential_id_len <= 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("credential_id is required")));
    }

    if (public_key == NULL || public_key_len <= 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("public_key is required")));
    }

    if (credential_id_len > WEBAUTHN_MAX_CREDENTIAL_ID_LEN) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("credential_id exceeds maximum length")));
    }

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    result = (WebAuthnRegistrationResult *)palloc0(sizeof(WebAuthnRegistrationResult));
    result->success = false;

    /* Validate and get challenge */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT user_id FROM auth.webauthn_challenges "
                     "WHERE challenge_id = '%s' "
                     "AND challenge_type = 'registration' "
                     "AND consumed_at IS NULL "
                     "AND expires_at > NOW()",
                     uuid_to_string(challenge_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        Datum user_id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);

        if (!isnull) {
            pg_uuid_t *uuid_ptr = DatumGetUUIDP(user_id_datum);
            memcpy(&user_id, uuid_ptr, sizeof(pg_uuid_t));
            found_challenge = true;
        }
    }

    SPI_finish();
    pfree(query.data);

    if (!found_challenge) {
        result->error_message = pstrdup("Invalid or expired challenge");
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Check for duplicate credential */
    initStringInfo(&cred_hex);
    for (i = 0; i < credential_id_len; i++)
        appendStringInfo(&cred_hex, "%02x", credential_id[i]);

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT 1 FROM auth.webauthn_credentials "
                     "WHERE webauthn_credential_id = E'\\\\x%s'",
                     cred_hex.data);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        SPI_finish();
        pfree(query.data);
        pfree(cred_hex.data);
        result->error_message = pstrdup("Credential already registered");
        MemoryContextSwitchTo(old_context);
        return result;
    }

    SPI_finish();
    pfree(query.data);

    /* Generate new credential ID */
    new_credential_id = generate_uuid();

    /* Build hex strings */
    initStringInfo(&pubkey_hex);
    for (i = 0; i < public_key_len; i++)
        appendStringInfo(&pubkey_hex, "%02x", public_key[i]);

    /* Build transports array */
    initStringInfo(&transports_array);
    appendStringInfoString(&transports_array, "ARRAY[");
    for (i = 0; i < transport_count; i++) {
        if (i > 0)
            appendStringInfoString(&transports_array, ", ");
        appendStringInfo(&transports_array, "'%s'", webauthn_transport_name(transports[i]));
    }
    appendStringInfoString(&transports_array, "]::text[]");

    /* Insert credential */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.webauthn_credentials "
                     "(credential_id, user_id, webauthn_credential_id, public_key_spki, "
                     "public_key_algorithm, sign_count, transports, device_name, status, "
                     "created_at) "
                     "VALUES ('%s', '%s', E'\\\\x%s', E'\\\\x%s', %d, %ld, %s, %s, 'active', "
                     "NOW())",
                     uuid_to_string(new_credential_id), uuid_to_string(user_id), cred_hex.data,
                     pubkey_hex.data, algorithm, (long)sign_count, transports_array.data,
                     device_name ? quote_literal_cstr(device_name) : "NULL");

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret != SPI_OK_INSERT) {
        SPI_finish();
        pfree(query.data);
        pfree(cred_hex.data);
        pfree(pubkey_hex.data);
        pfree(transports_array.data);
        result->error_message = pstrdup("Failed to store credential");
        MemoryContextSwitchTo(old_context);
        return result;
    }

    SPI_finish();

    /* Mark challenge as consumed */
    webauthn_consume_challenge(challenge_id);

    /* Log successful registration */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_logs (user_id, event_category, event_action, "
                     "event_outcome, event_data) VALUES ('%s', 'authentication', "
                     "'webauthn.credential.registered', 'success', "
                     "jsonb_build_object('credential_id', '%s', 'device_name', %s))",
                     uuid_to_string(user_id), uuid_to_string(new_credential_id),
                     device_name ? quote_literal_cstr(device_name) : "'unknown'");

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
    pfree(cred_hex.data);
    pfree(pubkey_hex.data);
    pfree(transports_array.data);

    /* Update statistics */
    if (webauthn_shared_state != NULL) {
        LWLockAcquire(webauthn_shared_state->challenge_lock, LW_EXCLUSIVE);
        webauthn_shared_state->total_registrations++;
        LWLockRelease(webauthn_shared_state->challenge_lock);
    }

    result->success = true;
    memcpy(&result->credential_id, &new_credential_id, sizeof(pg_uuid_t));

    MemoryContextSwitchTo(old_context);

    elog(LOG, "WebAuthn credential registered for user %s", uuid_to_string(user_id));

    return result;
}

/* ============================================================
 * Authentication Verification
 * ============================================================ */

/*
 * webauthn_verify_authentication - Verify authentication assertion
 */
WebAuthnAuthenticationResult *
webauthn_verify_authentication(pg_uuid_t challenge_id, const uint8 *credential_id,
                               int32 credential_id_len, const uint8 *authenticator_data,
                               int32 authenticator_data_len, const uint8 *signature,
                               int32 signature_len, const char *client_data_json,
                               const char *ip_address, const char *user_agent)
{
    MemoryContext old_context;
    WebAuthnAuthenticationResult *result;
    OrochiWebAuthnCredential *credential;
    StringInfoData query;
    StringInfoData cred_hex;
    uint8 client_data_hash[SHA256_DIGEST_LENGTH];
    int64 new_sign_count;
    pg_uuid_t challenge_user_id;
    bool challenge_valid = false;
    int ret;
    int i;

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    result = (WebAuthnAuthenticationResult *)palloc0(sizeof(WebAuthnAuthenticationResult));
    result->result = WEBAUTHN_VERIFY_INTERNAL_ERROR;

    /* Validate challenge exists and get user_id */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT user_id FROM auth.webauthn_challenges "
                     "WHERE challenge_id = '%s' "
                     "AND challenge_type = 'authentication' "
                     "AND consumed_at IS NULL "
                     "AND expires_at > NOW()",
                     uuid_to_string(challenge_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        Datum user_id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);

        if (!isnull) {
            pg_uuid_t *uuid_ptr = DatumGetUUIDP(user_id_datum);
            memcpy(&challenge_user_id, uuid_ptr, sizeof(pg_uuid_t));
            challenge_valid = true;
        }
    }

    SPI_finish();
    pfree(query.data);

    if (!challenge_valid) {
        result->result = WEBAUTHN_VERIFY_INVALID_CHALLENGE;
        result->error_message = pstrdup("Invalid or expired challenge");
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Look up credential by WebAuthn credential ID */
    credential = webauthn_get_credential_by_webauthn_id(credential_id, credential_id_len);

    if (credential == NULL) {
        result->result = WEBAUTHN_VERIFY_CREDENTIAL_NOT_FOUND;
        result->error_message = pstrdup("Credential not found");

        /* Log failed attempt */
        initStringInfo(&cred_hex);
        for (i = 0; i < credential_id_len && i < 32; i++)
            appendStringInfo(&cred_hex, "%02x", credential_id[i]);

        initStringInfo(&query);
        appendStringInfo(&query,
                         "INSERT INTO auth.audit_logs (user_id, event_category, event_action, "
                         "event_outcome, event_data) VALUES ('%s', 'authentication', "
                         "'webauthn.authentication', 'failure', "
                         "jsonb_build_object('reason', 'credential_not_found', 'ip', '%s', "
                         "'partial_cred_id', '%s'))",
                         uuid_to_string(challenge_user_id), ip_address ? ip_address : "unknown",
                         cred_hex.data);

        SPI_connect();
        SPI_execute(query.data, false, 0);
        SPI_finish();
        pfree(query.data);
        pfree(cred_hex.data);

        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Check credential status */
    if (credential->status == WEBAUTHN_STATUS_SUSPENDED) {
        result->result = WEBAUTHN_VERIFY_CREDENTIAL_SUSPENDED;
        result->error_message = pstrdup("Credential is suspended");
        webauthn_free_credential(credential);
        MemoryContextSwitchTo(old_context);
        return result;
    }

    if (credential->status == WEBAUTHN_STATUS_REVOKED) {
        result->result = WEBAUTHN_VERIFY_CREDENTIAL_REVOKED;
        result->error_message = pstrdup("Credential has been revoked");
        webauthn_free_credential(credential);
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Verify user matches challenge */
    if (memcmp(&credential->user_id, &challenge_user_id, sizeof(pg_uuid_t)) != 0) {
        result->result = WEBAUTHN_VERIFY_CREDENTIAL_NOT_FOUND;
        result->error_message = pstrdup("Credential does not belong to user");
        webauthn_free_credential(credential);
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Hash client data JSON */
    webauthn_sha256((const uint8 *)client_data_json, strlen(client_data_json), client_data_hash);

    /* Verify signature */
    if (!webauthn_verify_signature(credential->public_key_spki, credential->public_key_len,
                                   credential->public_key_algorithm, authenticator_data,
                                   authenticator_data_len, client_data_hash, signature,
                                   signature_len)) {
        result->result = WEBAUTHN_VERIFY_INVALID_SIGNATURE;
        result->error_message = pstrdup("Signature verification failed");

        if (webauthn_shared_state != NULL) {
            LWLockAcquire(webauthn_shared_state->challenge_lock, LW_EXCLUSIVE);
            webauthn_shared_state->failed_authentications++;
            LWLockRelease(webauthn_shared_state->challenge_lock);
        }

        webauthn_free_credential(credential);
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Extract and verify sign count */
    new_sign_count = webauthn_extract_sign_count(authenticator_data, authenticator_data_len);

    if (!webauthn_validate_sign_count(credential, new_sign_count)) {
        result->result = WEBAUTHN_VERIFY_REPLAY_DETECTED;
        result->error_message = pstrdup("Potential credential clone detected");

        /* Suspend the credential */
        webauthn_suspend_credential(credential->credential_id,
                                    "Sign count validation failed - potential clone");

        /* Log security event */
        initStringInfo(&query);
        appendStringInfo(&query,
                         "INSERT INTO auth.audit_logs (user_id, event_category, event_action, "
                         "event_outcome, event_data) VALUES ('%s', 'security', "
                         "'webauthn.replay_detected', 'denied', "
                         "jsonb_build_object('credential_id', '%s', 'ip', '%s', "
                         "'expected_sign_count', %ld, 'received_sign_count', %ld))",
                         uuid_to_string(credential->user_id),
                         uuid_to_string(credential->credential_id),
                         ip_address ? ip_address : "unknown", (long)credential->sign_count,
                         (long)new_sign_count);

        SPI_connect();
        SPI_execute(query.data, false, 0);
        SPI_finish();
        pfree(query.data);

        webauthn_free_credential(credential);
        MemoryContextSwitchTo(old_context);
        return result;
    }

    /* Update credential with new sign count and last used info */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webauthn_credentials SET "
                     "sign_count = %ld, last_used_at = NOW(), "
                     "last_used_ip = '%s', last_used_user_agent = %s "
                     "WHERE credential_id = '%s'",
                     (long)new_sign_count, ip_address ? ip_address : "unknown",
                     user_agent ? quote_literal_cstr(user_agent) : "NULL",
                     uuid_to_string(credential->credential_id));

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);

    /* Consume the challenge */
    webauthn_consume_challenge(challenge_id);

    /* Generate session token (32 bytes, hex encoded) */
    {
        uint8 token_bytes[32];
        char *session_token;

        generate_random_bytes(token_bytes, 32);
        session_token = palloc(65);
        for (i = 0; i < 32; i++)
            snprintf(session_token + i * 2, 3, "%02x", token_bytes[i]);
        session_token[64] = '\0';

        result->session_token = session_token;
    }

    /* Log successful authentication */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_logs (user_id, event_category, event_action, "
                     "event_outcome, event_data) VALUES ('%s', 'authentication', "
                     "'webauthn.authentication', 'success', "
                     "jsonb_build_object('credential_id', '%s', 'ip', '%s'))",
                     uuid_to_string(credential->user_id), uuid_to_string(credential->credential_id),
                     ip_address ? ip_address : "unknown");

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);

    /* Update statistics */
    if (webauthn_shared_state != NULL) {
        LWLockAcquire(webauthn_shared_state->challenge_lock, LW_EXCLUSIVE);
        webauthn_shared_state->total_authentications++;
        LWLockRelease(webauthn_shared_state->challenge_lock);
    }

    result->result = WEBAUTHN_VERIFY_SUCCESS;
    memcpy(&result->user_id, &credential->user_id, sizeof(pg_uuid_t));
    memcpy(&result->credential_id, &credential->credential_id, sizeof(pg_uuid_t));

    webauthn_free_credential(credential);
    MemoryContextSwitchTo(old_context);

    elog(LOG, "WebAuthn authentication successful for user %s", uuid_to_string(result->user_id));

    return result;
}

/* ============================================================
 * Cryptographic Operations
 * ============================================================ */

/*
 * webauthn_verify_signature - Verify ECDSA/RSA signature
 */
bool webauthn_verify_signature(const uint8 *public_key, int32 public_key_len, int32 algorithm,
                               const uint8 *authenticator_data, int32 authenticator_data_len,
                               const uint8 *client_data_hash, const uint8 *signature,
                               int32 signature_len)
{
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    const EVP_MD *md = NULL;
    const uint8 *key_ptr;
    uint8 *signed_data = NULL;
    int signed_data_len;
    int verify_result = 0;

    /* Select hash algorithm based on COSE algorithm */
    switch (algorithm) {
    case COSE_ALG_ES256:
    case COSE_ALG_RS256:
    case COSE_ALG_PS256:
        md = EVP_sha256();
        break;
    case COSE_ALG_ES384:
    case COSE_ALG_RS384:
        md = EVP_sha384();
        break;
    case COSE_ALG_ES512:
    case COSE_ALG_RS512:
        md = EVP_sha512();
        break;
    default:
        elog(WARNING, "Unsupported COSE algorithm: %d", algorithm);
        return false;
    }

    /* Parse SPKI public key */
    key_ptr = public_key;
    pkey = d2i_PUBKEY(NULL, &key_ptr, public_key_len);
    if (pkey == NULL) {
        elog(WARNING, "Failed to parse public key");
        return false;
    }

    /* Build signed data: authenticatorData || clientDataHash */
    signed_data_len = authenticator_data_len + SHA256_DIGEST_LENGTH;
    signed_data = palloc(signed_data_len);
    memcpy(signed_data, authenticator_data, authenticator_data_len);
    memcpy(signed_data + authenticator_data_len, client_data_hash, SHA256_DIGEST_LENGTH);

    /* Create verification context */
    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL) {
        EVP_PKEY_free(pkey);
        pfree(signed_data);
        return false;
    }

    /* Verify signature */
    if (EVP_DigestVerifyInit(md_ctx, NULL, md, NULL, pkey) != 1) {
        elog(WARNING, "Failed to initialize signature verification");
        goto cleanup;
    }

    if (EVP_DigestVerifyUpdate(md_ctx, signed_data, signed_data_len) != 1) {
        elog(WARNING, "Failed to update signature verification");
        goto cleanup;
    }

    verify_result = EVP_DigestVerifyFinal(md_ctx, signature, signature_len);

cleanup:
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    pfree(signed_data);

    return (verify_result == 1);
}

/*
 * webauthn_validate_sign_count - Validate signature counter
 */
bool webauthn_validate_sign_count(OrochiWebAuthnCredential *credential, int64 new_sign_count)
{
    /*
     * Per WebAuthn spec, if stored sign_count is non-zero, new count must be
     * greater. A sign_count of 0 from the authenticator means counter is not
     * supported.
     */
    if (new_sign_count == 0) {
        /* Counter not supported by authenticator - always valid */
        return true;
    }

    if (credential->sign_count > 0 && new_sign_count <= credential->sign_count) {
        /* Possible cloned authenticator! */
        elog(WARNING, "WebAuthn sign count validation failed: stored=%ld, received=%ld",
             (long)credential->sign_count, (long)new_sign_count);
        return false;
    }

    return true;
}

/*
 * webauthn_extract_sign_count - Extract sign count from authenticator data
 */
int32 webauthn_extract_sign_count(const uint8 *authenticator_data, int32 data_len)
{
    /*
     * authenticatorData structure:
     * - rpIdHash: 32 bytes
     * - flags: 1 byte
     * - signCount: 4 bytes (big-endian)
     */
    if (data_len < 37) {
        elog(WARNING, "authenticatorData too short for sign count");
        return 0;
    }

    /* Sign count is at offset 33, 4 bytes big-endian */
    return ((int32)authenticator_data[33] << 24) | ((int32)authenticator_data[34] << 16) |
           ((int32)authenticator_data[35] << 8) | ((int32)authenticator_data[36]);
}

/*
 * webauthn_extract_flags - Extract flags from authenticator data
 */
bool webauthn_extract_flags(const uint8 *authenticator_data, int32 data_len, bool *user_present,
                            bool *user_verified, bool *backup_eligible, bool *backup_state)
{
    uint8 flags;

    if (data_len < 33) {
        elog(WARNING, "authenticatorData too short for flags");
        return false;
    }

    flags = authenticator_data[32];

    if (user_present)
        *user_present = (flags & WEBAUTHN_FLAG_UP) != 0;
    if (user_verified)
        *user_verified = (flags & WEBAUTHN_FLAG_UV) != 0;
    if (backup_eligible)
        *backup_eligible = (flags & WEBAUTHN_FLAG_BE) != 0;
    if (backup_state)
        *backup_state = (flags & WEBAUTHN_FLAG_BS) != 0;

    return true;
}

/*
 * webauthn_sha256 - Compute SHA-256 hash
 */
void webauthn_sha256(const uint8 *data, int32 data_len, uint8 *hash_out)
{
    SHA256_CTX ctx;

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, data_len);
    SHA256_Final(hash_out, &ctx);
}

/* ============================================================
 * Credential Management
 * ============================================================ */

/*
 * webauthn_get_credential - Retrieve credential by internal ID
 */
OrochiWebAuthnCredential *webauthn_get_credential(pg_uuid_t credential_id)
{
    MemoryContext old_context;
    OrochiWebAuthnCredential *credential = NULL;
    StringInfoData query;
    int ret;

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT credential_id, user_id, webauthn_credential_id, public_key_spki, "
                     "public_key_algorithm, credential_type, device_name, device_type, "
                     "sign_count, backup_eligible, backup_state, status, last_used_at, "
                     "last_used_ip, created_at, revoked_at, revoked_reason "
                     "FROM auth.webauthn_credentials WHERE credential_id = '%s'",
                     uuid_to_string(credential_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        credential = palloc0(sizeof(OrochiWebAuthnCredential));

        /* Parse credential_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 1, &isnull);
            if (!isnull)
                memcpy(&credential->credential_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        /* Parse user_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 2, &isnull);
            if (!isnull)
                memcpy(&credential->user_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        /* Parse webauthn_credential_id (bytea) */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 3, &isnull);
            if (!isnull) {
                bytea *ba = DatumGetByteaP(d);
                credential->credential_id_len = VARSIZE_ANY_EXHDR(ba);
                credential->webauthn_credential_id = palloc(credential->credential_id_len);
                memcpy(credential->webauthn_credential_id, VARDATA_ANY(ba),
                       credential->credential_id_len);
            }
        }

        /* Parse public_key_spki (bytea) */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 4, &isnull);
            if (!isnull) {
                bytea *ba = DatumGetByteaP(d);
                credential->public_key_len = VARSIZE_ANY_EXHDR(ba);
                credential->public_key_spki = palloc(credential->public_key_len);
                memcpy(credential->public_key_spki, VARDATA_ANY(ba), credential->public_key_len);
            }
        }

        /* Parse algorithm */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 5, &isnull);
            if (!isnull)
                credential->public_key_algorithm = DatumGetInt32(d);
        }

        /* Parse sign_count */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 9, &isnull);
            if (!isnull)
                credential->sign_count = DatumGetInt64(d);
        }

        /* Parse status - map string to enum */
        {
            char *status_str = SPI_getvalue(tuple, tupdesc, 12);
            if (status_str) {
                if (strcmp(status_str, "active") == 0)
                    credential->status = WEBAUTHN_STATUS_ACTIVE;
                else if (strcmp(status_str, "suspended") == 0)
                    credential->status = WEBAUTHN_STATUS_SUSPENDED;
                else if (strcmp(status_str, "revoked") == 0)
                    credential->status = WEBAUTHN_STATUS_REVOKED;
                else
                    credential->status = WEBAUTHN_STATUS_PENDING;
            }
        }
    }

    SPI_finish();
    pfree(query.data);

    MemoryContextSwitchTo(old_context);

    return credential;
}

/*
 * webauthn_get_credential_by_webauthn_id - Retrieve by authenticator credential
 * ID
 */
OrochiWebAuthnCredential *
webauthn_get_credential_by_webauthn_id(const uint8 *webauthn_credential_id, int32 credential_id_len)
{
    MemoryContext old_context;
    OrochiWebAuthnCredential *credential = NULL;
    StringInfoData query;
    StringInfoData cred_hex;
    int ret;
    int i;

    old_context = MemoryContextSwitchTo(WebAuthnContext);

    /* Convert to hex string */
    initStringInfo(&cred_hex);
    for (i = 0; i < credential_id_len; i++)
        appendStringInfo(&cred_hex, "%02x", webauthn_credential_id[i]);

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT credential_id, user_id, webauthn_credential_id, public_key_spki, "
                     "public_key_algorithm, credential_type, device_name, device_type, "
                     "sign_count, backup_eligible, backup_state, status, last_used_at, "
                     "last_used_ip, created_at, revoked_at, revoked_reason "
                     "FROM auth.webauthn_credentials WHERE webauthn_credential_id = "
                     "E'\\\\x%s'",
                     cred_hex.data);

    pfree(cred_hex.data);

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        credential = palloc0(sizeof(OrochiWebAuthnCredential));

        /* Parse same fields as webauthn_get_credential */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 1, &isnull);
            if (!isnull)
                memcpy(&credential->credential_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        {
            Datum d = SPI_getbinval(tuple, tupdesc, 2, &isnull);
            if (!isnull)
                memcpy(&credential->user_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        {
            Datum d = SPI_getbinval(tuple, tupdesc, 3, &isnull);
            if (!isnull) {
                bytea *ba = DatumGetByteaP(d);
                credential->credential_id_len = VARSIZE_ANY_EXHDR(ba);
                credential->webauthn_credential_id = palloc(credential->credential_id_len);
                memcpy(credential->webauthn_credential_id, VARDATA_ANY(ba),
                       credential->credential_id_len);
            }
        }

        {
            Datum d = SPI_getbinval(tuple, tupdesc, 4, &isnull);
            if (!isnull) {
                bytea *ba = DatumGetByteaP(d);
                credential->public_key_len = VARSIZE_ANY_EXHDR(ba);
                credential->public_key_spki = palloc(credential->public_key_len);
                memcpy(credential->public_key_spki, VARDATA_ANY(ba), credential->public_key_len);
            }
        }

        {
            Datum d = SPI_getbinval(tuple, tupdesc, 5, &isnull);
            if (!isnull)
                credential->public_key_algorithm = DatumGetInt32(d);
        }

        {
            Datum d = SPI_getbinval(tuple, tupdesc, 9, &isnull);
            if (!isnull)
                credential->sign_count = DatumGetInt64(d);
        }

        {
            char *status_str = SPI_getvalue(tuple, tupdesc, 12);
            if (status_str) {
                if (strcmp(status_str, "active") == 0)
                    credential->status = WEBAUTHN_STATUS_ACTIVE;
                else if (strcmp(status_str, "suspended") == 0)
                    credential->status = WEBAUTHN_STATUS_SUSPENDED;
                else if (strcmp(status_str, "revoked") == 0)
                    credential->status = WEBAUTHN_STATUS_REVOKED;
                else
                    credential->status = WEBAUTHN_STATUS_PENDING;
            }
        }
    }

    SPI_finish();
    pfree(query.data);

    MemoryContextSwitchTo(old_context);

    return credential;
}

/*
 * webauthn_revoke_credential - Revoke a credential
 */
bool webauthn_revoke_credential(pg_uuid_t user_id, pg_uuid_t credential_id, const char *reason)
{
    StringInfoData query;
    int ret;
    int active_count;

    /* Check that user has at least one other active credential */
    active_count = webauthn_get_active_credential_count(user_id);
    if (active_count <= 1) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("cannot revoke last active credential")));
        return false;
    }

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webauthn_credentials SET "
                     "status = 'revoked', revoked_at = NOW(), revoked_by = '%s', "
                     "revoked_reason = %s "
                     "WHERE credential_id = '%s' AND user_id = '%s' AND status = 'active'",
                     uuid_to_string(user_id), reason ? quote_literal_cstr(reason) : "NULL",
                     uuid_to_string(credential_id), uuid_to_string(user_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    if (ret != SPI_OK_UPDATE || SPI_processed == 0)
        return false;

    /* Log revocation */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_logs (user_id, event_category, event_action, "
                     "event_outcome, event_data) VALUES ('%s', 'authentication', "
                     "'webauthn.credential.revoked', 'success', "
                     "jsonb_build_object('credential_id', '%s', 'reason', %s))",
                     uuid_to_string(user_id), uuid_to_string(credential_id),
                     reason ? quote_literal_cstr(reason) : "'unspecified'");

    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
    pfree(query.data);

    elog(LOG, "WebAuthn credential %s revoked for user %s", uuid_to_string(credential_id),
         uuid_to_string(user_id));

    return true;
}

/*
 * webauthn_suspend_credential - Temporarily suspend a credential
 */
bool webauthn_suspend_credential(pg_uuid_t credential_id, const char *reason)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webauthn_credentials SET status = 'suspended' "
                     "WHERE credential_id = '%s' AND status = 'active'",
                     uuid_to_string(credential_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    return (ret == SPI_OK_UPDATE && SPI_processed > 0);
}

/*
 * webauthn_reactivate_credential - Reactivate a suspended credential
 */
bool webauthn_reactivate_credential(pg_uuid_t credential_id)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.webauthn_credentials SET status = 'active' "
                     "WHERE credential_id = '%s' AND status = 'suspended'",
                     uuid_to_string(credential_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);

    return (ret == SPI_OK_UPDATE && SPI_processed > 0);
}

/*
 * webauthn_get_active_credential_count - Count active credentials for user
 */
int webauthn_get_active_credential_count(pg_uuid_t user_id)
{
    StringInfoData query;
    int ret;
    int count = 0;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT COUNT(*) FROM auth.webauthn_credentials "
                     "WHERE user_id = '%s' AND status = 'active'",
                     uuid_to_string(user_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        count = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 1, &isnull));
    }

    SPI_finish();
    pfree(query.data);

    return count;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * webauthn_credential_type_name - Get string name for credential type
 */
const char *webauthn_credential_type_name(WebAuthnCredentialType type)
{
    switch (type) {
    case WEBAUTHN_CRED_PASSKEY:
        return "passkey";
    case WEBAUTHN_CRED_SECURITY_KEY:
        return "security_key";
    case WEBAUTHN_CRED_PLATFORM:
        return "platform";
    case WEBAUTHN_CRED_CROSS_PLATFORM:
        return "cross_platform";
    default:
        return "unknown";
    }
}

/*
 * webauthn_device_type_name - Get string name for device type
 */
const char *webauthn_device_type_name(WebAuthnDeviceType type)
{
    switch (type) {
    case WEBAUTHN_DEVICE_WINDOWS_HELLO:
        return "windows_hello";
    case WEBAUTHN_DEVICE_APPLE_TOUCH_ID:
        return "apple_touch_id";
    case WEBAUTHN_DEVICE_APPLE_FACE_ID:
        return "apple_face_id";
    case WEBAUTHN_DEVICE_ANDROID_BIOMETRIC:
        return "android_biometric";
    case WEBAUTHN_DEVICE_YUBIKEY:
        return "yubikey";
    case WEBAUTHN_DEVICE_GOOGLE_TITAN:
        return "google_titan";
    case WEBAUTHN_DEVICE_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

/*
 * webauthn_status_name - Get string name for credential status
 */
const char *webauthn_status_name(WebAuthnCredentialStatus status)
{
    switch (status) {
    case WEBAUTHN_STATUS_PENDING:
        return "pending_verification";
    case WEBAUTHN_STATUS_ACTIVE:
        return "active";
    case WEBAUTHN_STATUS_SUSPENDED:
        return "suspended";
    case WEBAUTHN_STATUS_REVOKED:
        return "revoked";
    default:
        return "unknown";
    }
}

/*
 * webauthn_transport_name - Get string name for transport type
 */
const char *webauthn_transport_name(WebAuthnTransport transport)
{
    switch (transport) {
    case WEBAUTHN_TRANSPORT_USB:
        return "usb";
    case WEBAUTHN_TRANSPORT_NFC:
        return "nfc";
    case WEBAUTHN_TRANSPORT_BLE:
        return "ble";
    case WEBAUTHN_TRANSPORT_INTERNAL:
        return "internal";
    case WEBAUTHN_TRANSPORT_HYBRID:
        return "hybrid";
    default:
        return "unknown";
    }
}

/*
 * webauthn_verify_result_message - Get human-readable message for verify result
 */
const char *webauthn_verify_result_message(WebAuthnVerifyResult result)
{
    switch (result) {
    case WEBAUTHN_VERIFY_SUCCESS:
        return "Authentication successful";
    case WEBAUTHN_VERIFY_INVALID_CHALLENGE:
        return "Invalid or expired challenge";
    case WEBAUTHN_VERIFY_EXPIRED_CHALLENGE:
        return "Challenge has expired";
    case WEBAUTHN_VERIFY_CREDENTIAL_NOT_FOUND:
        return "Credential not found";
    case WEBAUTHN_VERIFY_INVALID_SIGNATURE:
        return "Signature verification failed";
    case WEBAUTHN_VERIFY_REPLAY_DETECTED:
        return "Potential credential clone detected";
    case WEBAUTHN_VERIFY_USER_NOT_FOUND:
        return "User not found";
    case WEBAUTHN_VERIFY_CREDENTIAL_SUSPENDED:
        return "Credential is suspended";
    case WEBAUTHN_VERIFY_CREDENTIAL_REVOKED:
        return "Credential has been revoked";
    case WEBAUTHN_VERIFY_INTERNAL_ERROR:
        return "Internal error";
    default:
        return "Unknown error";
    }
}

/*
 * webauthn_cleanup_expired_challenges - Remove expired challenges
 */
void webauthn_cleanup_expired_challenges(void)
{
    StringInfoData query;
    int ret;

    initStringInfo(&query);
    appendStringInfoString(&query, "DELETE FROM auth.webauthn_challenges WHERE expires_at < NOW()");

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_DELETE) {
        elog(DEBUG1, "Cleaned up %lu expired WebAuthn challenges", SPI_processed);
    }

    SPI_finish();
    pfree(query.data);
}

/*
 * webauthn_free_credential - Free credential structure
 */
void webauthn_free_credential(OrochiWebAuthnCredential *credential)
{
    if (credential == NULL)
        return;

    if (credential->webauthn_credential_id)
        pfree(credential->webauthn_credential_id);
    if (credential->public_key_spki)
        pfree(credential->public_key_spki);
    if (credential->attestation_format)
        pfree(credential->attestation_format);
    if (credential->attestation_statement)
        pfree(credential->attestation_statement);
    if (credential->last_used_ip)
        pfree(credential->last_used_ip);
    if (credential->last_used_user_agent)
        pfree(credential->last_used_user_agent);
    if (credential->revoked_by)
        pfree(credential->revoked_by);
    if (credential->revoked_reason)
        pfree(credential->revoked_reason);

    pfree(credential);
}

/*
 * webauthn_free_challenge - Free challenge structure
 */
void webauthn_free_challenge(WebAuthnChallenge *challenge)
{
    if (challenge == NULL)
        return;

    if (challenge->user_id)
        pfree(challenge->user_id);
    if (challenge->rp_id)
        pfree(challenge->rp_id);
    if (challenge->rp_name)
        pfree(challenge->rp_name);
    if (challenge->origin)
        pfree(challenge->origin);
    if (challenge->ip_address)
        pfree(challenge->ip_address);
    if (challenge->user_agent)
        pfree(challenge->user_agent);

    pfree(challenge);
}

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

/*
 * generate_random_bytes - Generate cryptographically secure random bytes
 */
static void generate_random_bytes(uint8 *buffer, int length)
{
    if (RAND_bytes(buffer, length) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to generate random bytes")));
    }
}

/*
 * generate_uuid - Generate a new UUID
 */
static pg_uuid_t generate_uuid(void)
{
    pg_uuid_t uuid;

    generate_random_bytes(uuid.data, UUID_LEN);

    /* Set version 4 (random) */
    uuid.data[6] = (uuid.data[6] & 0x0f) | 0x40;
    /* Set variant (RFC 4122) */
    uuid.data[8] = (uuid.data[8] & 0x3f) | 0x80;

    return uuid;
}

/*
 * uuid_to_string - Convert UUID to string representation
 */
static char *uuid_to_string(pg_uuid_t uuid)
{
    char *str = palloc(37);

    snprintf(str, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3], uuid.data[4], uuid.data[5],
             uuid.data[6], uuid.data[7], uuid.data[8], uuid.data[9], uuid.data[10], uuid.data[11],
             uuid.data[12], uuid.data[13], uuid.data[14], uuid.data[15]);

    return str;
}

/*
 * parse_uuid - Parse UUID string into binary format
 *
 * Available for credential UUID parsing. Complement to uuid_to_string().
 */
static bool parse_uuid(const char *str, pg_uuid_t *uuid)
{
    /* Basic UUID parsing - format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    int i, j;
    const char *p = str;

    if (strlen(str) != 36)
        return false;

    j = 0;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (str[i] != '-')
                return false;
            continue;
        }

        int high, low;
        char c = str[i];

        if (c >= '0' && c <= '9')
            high = c - '0';
        else if (c >= 'a' && c <= 'f')
            high = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            high = c - 'A' + 10;
        else
            return false;

        i++;
        c = str[i];

        if (c >= '0' && c <= '9')
            low = c - '0';
        else if (c >= 'a' && c <= 'f')
            low = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            low = c - 'A' + 10;
        else
            return false;

        uuid->data[j++] = (high << 4) | low;
    }

    return (j == 16);
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(webauthn_begin_registration);
PG_FUNCTION_INFO_V1(webauthn_complete_registration_sql);
PG_FUNCTION_INFO_V1(webauthn_begin_authentication);
PG_FUNCTION_INFO_V1(webauthn_verify_authentication_sql);
PG_FUNCTION_INFO_V1(webauthn_list_credentials);
PG_FUNCTION_INFO_V1(webauthn_revoke_credential_sql);

/*
 * webauthn_begin_registration - SQL function to start registration
 */
Datum webauthn_begin_registration(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
    text *rp_id = PG_GETARG_TEXT_PP(1);
    text *rp_name = PG_GETARG_TEXT_PP(2);
    text *origin = PG_GETARG_TEXT_PP(3);

    WebAuthnChallenge *challenge;
    StringInfoData result;

    challenge = webauthn_create_registration_challenge(
        *user_id, text_to_cstring(rp_id), text_to_cstring(rp_name), text_to_cstring(origin),
        WEBAUTHN_UV_PREFERRED);

    /* Build JSON response */
    initStringInfo(&result);
    appendStringInfo(&result, "{\"challenge_id\": \"%s\", \"challenge\": \"",
                     uuid_to_string(challenge->challenge_id));

    /* Base64 encode challenge bytes */
    for (int i = 0; i < WEBAUTHN_CHALLENGE_SIZE; i++)
        appendStringInfo(&result, "%02x", challenge->challenge[i]);

    appendStringInfo(&result,
                     "\", \"rp_id\": \"%s\", \"rp_name\": \"%s\", "
                     "\"user_id\": \"%s\", \"timeout\": %d}",
                     challenge->rp_id, challenge->rp_name, uuid_to_string(*user_id),
                     WEBAUTHN_CHALLENGE_TTL_SECONDS * 1000);

    webauthn_free_challenge(challenge);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * webauthn_begin_authentication - SQL function to start authentication
 */
Datum webauthn_begin_authentication(PG_FUNCTION_ARGS)
{
    text *email = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
    text *rp_id = PG_GETARG_TEXT_PP(1);
    text *origin = PG_GETARG_TEXT_PP(2);

    WebAuthnChallenge *challenge;
    StringInfoData result;

    challenge = webauthn_create_authentication_challenge(
        email ? text_to_cstring(email) : NULL, text_to_cstring(rp_id), text_to_cstring(origin));

    initStringInfo(&result);
    appendStringInfo(&result, "{\"challenge_id\": \"%s\", \"challenge\": \"",
                     uuid_to_string(challenge->challenge_id));

    for (int i = 0; i < WEBAUTHN_CHALLENGE_SIZE; i++)
        appendStringInfo(&result, "%02x", challenge->challenge[i]);

    appendStringInfo(&result, "\", \"rp_id\": \"%s\", \"timeout\": %d}", challenge->rp_id,
                     WEBAUTHN_CHALLENGE_TTL_SECONDS * 1000);

    webauthn_free_challenge(challenge);

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * webauthn_list_credentials - List user's credentials
 */
Datum webauthn_list_credentials(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    if (SRF_IS_FIRSTCALL()) {
        pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
        TupleDesc tupdesc;
        StringInfoData query;
        int ret;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Build query */
        initStringInfo(&query);
        appendStringInfo(&query,
                         "SELECT credential_id, device_name, device_type, credential_type, "
                         "status, last_used_at, created_at "
                         "FROM auth.webauthn_credentials WHERE user_id = '%s' "
                         "ORDER BY created_at DESC",
                         uuid_to_string(*user_id));

        SPI_connect();
        ret = SPI_execute(query.data, true, 0);

        if (ret == SPI_OK_SELECT) {
            funcctx->max_calls = SPI_processed;
            funcctx->user_fctx = SPI_tuptable;

            /* Build tuple descriptor */
            tupdesc = CreateTemplateTupleDesc(7);
            TupleDescInitEntry(tupdesc, 1, "credential_id", UUIDOID, -1, 0);
            TupleDescInitEntry(tupdesc, 2, "device_name", TEXTOID, -1, 0);
            TupleDescInitEntry(tupdesc, 3, "device_type", TEXTOID, -1, 0);
            TupleDescInitEntry(tupdesc, 4, "credential_type", TEXTOID, -1, 0);
            TupleDescInitEntry(tupdesc, 5, "status", TEXTOID, -1, 0);
            TupleDescInitEntry(tupdesc, 6, "last_used_at", TIMESTAMPTZOID, -1, 0);
            TupleDescInitEntry(tupdesc, 7, "created_at", TIMESTAMPTZOID, -1, 0);

            funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        } else {
            funcctx->max_calls = 0;
        }

        pfree(query.data);
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();

    if (funcctx->call_cntr < funcctx->max_calls) {
        SPITupleTable *tuptable = (SPITupleTable *)funcctx->user_fctx;
        HeapTuple tuple = tuptable->vals[funcctx->call_cntr];
        Datum result;

        result = HeapTupleGetDatum(tuple);
        SRF_RETURN_NEXT(funcctx, result);
    } else {
        SPI_finish();
        SRF_RETURN_DONE(funcctx);
    }
}

/*
 * webauthn_revoke_credential_sql - SQL function to revoke credential
 */
Datum webauthn_revoke_credential_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
    pg_uuid_t *credential_id = PG_GETARG_UUID_P(1);
    text *reason = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);

    bool success = webauthn_revoke_credential(*user_id, *credential_id,
                                              reason ? text_to_cstring(reason) : NULL);

    PG_RETURN_BOOL(success);
}
