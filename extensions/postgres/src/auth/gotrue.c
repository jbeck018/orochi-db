/*-------------------------------------------------------------------------
 *
 * gotrue.c
 *    Orochi DB GoTrue-compatible authentication service implementation
 *
 * This module provides the core authentication functionality including:
 *   - User creation and management
 *   - Password-based authentication
 *   - Session management
 *   - JWT token generation and validation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <string.h>

#include "gotrue.h"

/* Shared memory state */
static OrochiAuthSharedState *auth_shared_state = NULL;

/* GUC variables */
char *orochi_auth_jwt_secret = NULL;
int orochi_auth_jwt_exp = OROCHI_AUTH_DEFAULT_JWT_EXP;
bool orochi_auth_enable_anonymous = true;

/* Forward declarations */
static char *generate_refresh_token(void);
static char *base64url_encode(const unsigned char *input, int input_len);
static unsigned char *base64url_decode(const char *input, int *output_len);
static Jsonb *build_jwt_payload(OrochiAuthUser *user, OrochiAuthSession *session, int exp_seconds);
static char *sign_jwt(const char *header, const char *payload, const char *secret);

/* ============================================================
 * Shared Memory Initialization
 * ============================================================ */

Size orochi_auth_shmem_size(void)
{
    return sizeof(OrochiAuthSharedState);
}

void orochi_auth_shmem_init(void)
{
    bool found;

    auth_shared_state = (OrochiAuthSharedState *)ShmemInitStruct(
        "Orochi Auth State", sizeof(OrochiAuthSharedState), &found);

    if (!found) {
        memset(auth_shared_state, 0, sizeof(OrochiAuthSharedState));
        auth_shared_state->lock = &(GetNamedLWLockTranche("orochi_auth"))->lock;
        auth_shared_state->active_sessions = 0;
        auth_shared_state->last_cleanup = GetCurrentTimestamp();
    }
}

/* ============================================================
 * Token Generation Utilities
 * ============================================================ */

/*
 * orochi_auth_generate_token
 *    Generate a cryptographically secure random token
 */
char *orochi_auth_generate_token(int length)
{
    unsigned char *random_bytes;
    char *token;
    int byte_length;

    if (length <= 0)
        length = OROCHI_AUTH_TOKEN_LENGTH;

    /* Each byte becomes 2 hex characters */
    byte_length = (length + 1) / 2;
    random_bytes = palloc(byte_length);

    if (RAND_bytes(random_bytes, byte_length) != 1) {
        pfree(random_bytes);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to generate random bytes")));
    }

    /* Convert to hex string */
    token = palloc(length * 2 + 1);
    for (int i = 0; i < byte_length && i * 2 < length * 2; i++) {
        snprintf(token + i * 2, 3, "%02x", random_bytes[i]);
    }
    token[length * 2] = '\0';

    pfree(random_bytes);
    return token;
}

/*
 * orochi_auth_generate_otp
 *    Generate a numeric OTP code
 */
char *orochi_auth_generate_otp(int length)
{
    char *otp;
    unsigned char random_bytes[4];
    unsigned int random_value;

    if (length <= 0 || length > 10)
        length = OROCHI_AUTH_OTP_LENGTH;

    if (RAND_bytes(random_bytes, 4) != 1) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("failed to generate random bytes for OTP")));
    }

    /* Convert bytes to integer and take modulo */
    random_value = (random_bytes[0] << 24) | (random_bytes[1] << 16) | (random_bytes[2] << 8) |
                   random_bytes[3];

    /* Generate OTP with leading zeros if needed */
    otp = palloc(length + 1);

    switch (length) {
    case 4:
        snprintf(otp, length + 1, "%04u", random_value % 10000);
        break;
    case 5:
        snprintf(otp, length + 1, "%05u", random_value % 100000);
        break;
    case 6:
        snprintf(otp, length + 1, "%06u", random_value % 1000000);
        break;
    case 7:
        snprintf(otp, length + 1, "%07u", random_value % 10000000);
        break;
    case 8:
        snprintf(otp, length + 1, "%08u", random_value % 100000000);
        break;
    default:
        snprintf(otp, length + 1, "%06u", random_value % 1000000);
    }

    return otp;
}

/*
 * orochi_auth_hash_token
 *    Hash a token using SHA256
 */
char *orochi_auth_hash_token(const char *token)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    char *hex_hash;
    int i;

    if (token == NULL)
        return NULL;

    SHA256((unsigned char *)token, strlen(token), hash);

    hex_hash = palloc(SHA256_DIGEST_LENGTH * 2 + 1);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex_hash + i * 2, 3, "%02x", hash[i]);
    }
    hex_hash[SHA256_DIGEST_LENGTH * 2] = '\0';

    return hex_hash;
}

/*
 * orochi_auth_verify_token_hash
 *    Verify a token against its hash using constant-time comparison
 */
bool orochi_auth_verify_token_hash(const char *token, const char *hash)
{
    char *computed_hash;
    bool result;

    if (token == NULL || hash == NULL)
        return false;

    computed_hash = orochi_auth_hash_token(token);
    if (computed_hash == NULL)
        return false;

    /* Use constant-time comparison to prevent timing attacks */
    result = (strlen(computed_hash) == strlen(hash)) &&
             (CRYPTO_memcmp(computed_hash, hash, strlen(hash)) == 0);

    pfree(computed_hash);
    return result;
}

/*
 * generate_refresh_token
 *    Generate a base64-encoded refresh token
 */
static char *generate_refresh_token(void)
{
    unsigned char random_bytes[32];
    char *token;

    if (RAND_bytes(random_bytes, 32) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to generate refresh token")));
    }

    token = base64url_encode(random_bytes, 32);
    return token;
}

/* ============================================================
 * Base64URL Encoding/Decoding
 * ============================================================ */

static char *base64url_encode(const unsigned char *input, int input_len)
{
    int encoded_len;
    char *encoded;
    int i;

    encoded_len = pg_b64_enc_len(input_len);
    encoded = palloc(encoded_len + 1);

    encoded_len = pg_b64_encode((const char *)input, input_len, encoded, encoded_len);
    encoded[encoded_len] = '\0';

    /* Convert to URL-safe base64 */
    for (i = 0; i < encoded_len; i++) {
        if (encoded[i] == '+')
            encoded[i] = '-';
        else if (encoded[i] == '/')
            encoded[i] = '_';
    }

    /* Remove padding */
    while (encoded_len > 0 && encoded[encoded_len - 1] == '=') {
        encoded[--encoded_len] = '\0';
    }

    return encoded;
}

static unsigned char *base64url_decode(const char *input, int *output_len)
{
    int input_len = strlen(input);
    int padding = (4 - (input_len % 4)) % 4;
    int padded_len = input_len + padding;
    char *padded;
    unsigned char *decoded;
    int i;

    padded = palloc(padded_len + 1);

    /* Convert from URL-safe base64 */
    for (i = 0; i < input_len; i++) {
        if (input[i] == '-')
            padded[i] = '+';
        else if (input[i] == '_')
            padded[i] = '/';
        else
            padded[i] = input[i];
    }

    /* Add padding */
    for (; i < padded_len; i++)
        padded[i] = '=';
    padded[padded_len] = '\0';

    /* Decode */
    *output_len = pg_b64_dec_len(padded_len);
    decoded = palloc(*output_len + 1);
    *output_len = pg_b64_decode(padded, padded_len, (char *)decoded, *output_len);
    decoded[*output_len] = '\0';

    pfree(padded);
    return decoded;
}

/* ============================================================
 * Password Hashing
 * ============================================================ */

/*
 * orochi_auth_hash_password
 *    Hash a password using bcrypt (via PostgreSQL's crypt)
 */
char *orochi_auth_hash_password(const char *password)
{
    StringInfoData query;
    int ret;
    char *hashed = NULL;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];

    if (password == NULL || strlen(password) == 0)
        return NULL;

    /* Use PostgreSQL's crypt function with bcrypt */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("failed to connect to SPI for password hashing")));

    initStringInfo(&query);
    appendStringInfoString(&query, "SELECT crypt($1, gen_salt('bf', 10))");

    argvals[0] = CStringGetTextDatum(password);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        char *result = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (result)
            hashed = pstrdup(result);
    }

    pfree(query.data);
    SPI_finish();

    return hashed;
}

/*
 * orochi_auth_verify_password
 *    Verify a password against its hash
 */
bool orochi_auth_verify_password(const char *password, const char *encrypted_password)
{
    StringInfoData query;
    int ret;
    bool verified = false;
    Oid argtypes[2] = { TEXTOID, TEXTOID };
    Datum argvals[2];

    if (password == NULL || encrypted_password == NULL)
        return false;

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);
    appendStringInfoString(&query, "SELECT crypt($1, $2) = $2");

    argvals[0] = CStringGetTextDatum(password);
    argvals[1] = CStringGetTextDatum(encrypted_password);

    ret = SPI_execute_with_args(query.data, 2, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        verified =
            DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return verified;
}

/* ============================================================
 * User Management
 * ============================================================ */

/*
 * orochi_auth_create_user
 *    Create a new user account
 */
OrochiAuthUser *orochi_auth_create_user(const char *email, const char *phone, const char *password,
                                        Jsonb *user_metadata)
{
    OrochiAuthUser *user;
    StringInfoData query;
    int ret;
    char *encrypted_password = NULL;
    MemoryContext user_context;
    MemoryContext old_context;

    /* Validate inputs */
    if (email == NULL && phone == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("email or phone is required")));

    if (email != NULL && strlen(email) > OROCHI_AUTH_MAX_EMAIL_LENGTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("email too long (max %d characters)", OROCHI_AUTH_MAX_EMAIL_LENGTH)));

    if (phone != NULL && strlen(phone) > OROCHI_AUTH_MAX_PHONE_LENGTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("phone too long (max %d characters)", OROCHI_AUTH_MAX_PHONE_LENGTH)));

    /* Hash password if provided */
    if (password != NULL) {
        if (strlen(password) < OROCHI_AUTH_MIN_PASSWORD_LENGTH)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("password too short (min %d characters)",
                                   OROCHI_AUTH_MIN_PASSWORD_LENGTH)));

        encrypted_password = orochi_auth_hash_password(password);
    }

    /* Create memory context for user */
    user_context = AllocSetContextCreate(TopMemoryContext, "AuthUser", ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(user_context);

    user = palloc0(sizeof(OrochiAuthUser));
    user->context = user_context;

    MemoryContextSwitchTo(old_context);

    /* Insert into database */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to connect to SPI")));

    initStringInfo(&query);
    {
        char *metadata_str;

        if (user_metadata != NULL) {
            Datum jsonb_text = DirectFunctionCall1(jsonb_out, JsonbPGetDatum(user_metadata));
            metadata_str = quote_literal_cstr(DatumGetCString(jsonb_text));
        } else {
            metadata_str = "'{}'";
        }

        appendStringInfo(
            &query,
            "INSERT INTO auth.users "
            "(email, phone, encrypted_password, raw_user_meta_data, "
            "raw_app_meta_data, role, is_anonymous, created_at, updated_at) "
            "VALUES (%s, %s, %s, %s::jsonb, "
            "'{}', 'authenticated', FALSE, NOW(), NOW()) "
            "RETURNING id, email, phone, role, created_at",
            email ? quote_literal_cstr(email) : "NULL", phone ? quote_literal_cstr(phone) : "NULL",
            encrypted_password ? quote_literal_cstr(encrypted_password) : "NULL", metadata_str);
    }

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        Datum uuid_datum;
        char *id_str;

        /* Extract returned values */
        id_str = SPI_getvalue(tuple, tupdesc, 1);
        if (id_str) {
            uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&user->id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
        }

        old_context = MemoryContextSwitchTo(user_context);

        user->email = email ? pstrdup(email) : NULL;
        user->phone = phone ? pstrdup(phone) : NULL;
        user->role = pstrdup("authenticated");
        user->is_anonymous = false;
        user->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        user->updated_at = user->created_at;

        MemoryContextSwitchTo(old_context);
    } else {
        pfree(query.data);
        SPI_finish();
        if (user_context)
            MemoryContextDelete(user_context);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create user")));
    }

    pfree(query.data);
    if (encrypted_password)
        pfree(encrypted_password);
    SPI_finish();

    elog(LOG, "Created auth user with email: %s", email ? email : "(none)");

    return user;
}

/*
 * orochi_auth_get_user
 *    Get user by UUID
 */
OrochiAuthUser *orochi_auth_get_user(pg_uuid_t user_id)
{
    OrochiAuthUser *user = NULL;
    StringInfoData query;
    int ret;
    char uuid_str[37];
    MemoryContext user_context;
    MemoryContext old_context;
    Oid argtypes[1] = { UUIDOID };
    Datum argvals[1];

    /* Convert UUID to string for query */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    if (SPI_connect() != SPI_OK_CONNECT)
        return NULL;

    initStringInfo(&query);
    appendStringInfoString(&query,
                           "SELECT id, email, phone, encrypted_password, role, is_anonymous, "
                           "is_super_admin, is_sso_user, email_confirmed_at, phone_confirmed_at, "
                           "created_at, updated_at, last_sign_in_at, banned_until, deleted_at, "
                           "raw_app_meta_data, raw_user_meta_data "
                           "FROM auth.users WHERE id = $1 AND deleted_at IS NULL");

    argvals[0] = UUIDPGetDatum(&user_id);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        user_context = AllocSetContextCreate(TopMemoryContext, "AuthUser", ALLOCSET_DEFAULT_SIZES);
        old_context = MemoryContextSwitchTo(user_context);

        user = palloc0(sizeof(OrochiAuthUser));
        user->context = user_context;

        memcpy(&user->id, &user_id, sizeof(pg_uuid_t));

        user->email = SPI_getvalue(tuple, tupdesc, 2);
        if (user->email)
            user->email = pstrdup(user->email);

        user->phone = SPI_getvalue(tuple, tupdesc, 3);
        if (user->phone)
            user->phone = pstrdup(user->phone);

        user->encrypted_password = SPI_getvalue(tuple, tupdesc, 4);
        if (user->encrypted_password)
            user->encrypted_password = pstrdup(user->encrypted_password);

        user->role = SPI_getvalue(tuple, tupdesc, 5);
        if (user->role)
            user->role = pstrdup(user->role);

        user->is_anonymous = DatumGetBool(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        user->is_super_admin = DatumGetBool(SPI_getbinval(tuple, tupdesc, 7, &isnull));
        user->is_sso_user = DatumGetBool(SPI_getbinval(tuple, tupdesc, 8, &isnull));

        user->email_confirmed_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 9, &isnull));
        user->phone_confirmed_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 10, &isnull));
        user->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 11, &isnull));
        user->updated_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 12, &isnull));
        user->last_sign_in_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 13, &isnull));
        user->banned_until = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 14, &isnull));
        user->deleted_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 15, &isnull));

        MemoryContextSwitchTo(old_context);
    }

    pfree(query.data);
    SPI_finish();

    return user;
}

/*
 * orochi_auth_get_user_by_email
 *    Get user by email address
 */
OrochiAuthUser *orochi_auth_get_user_by_email(const char *email)
{
    OrochiAuthUser *user = NULL;
    StringInfoData query;
    int ret;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];
    pg_uuid_t user_id;

    if (email == NULL)
        return NULL;

    if (SPI_connect() != SPI_OK_CONNECT)
        return NULL;

    initStringInfo(&query);
    appendStringInfoString(&query,
                           "SELECT id FROM auth.users WHERE email = $1 AND deleted_at IS NULL");

    argvals[0] = CStringGetTextDatum(email);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        char *id_str = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (id_str) {
            Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&user_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));

            pfree(query.data);
            SPI_finish();

            return orochi_auth_get_user(user_id);
        }
    }

    pfree(query.data);
    SPI_finish();

    return user;
}

/*
 * orochi_auth_get_user_by_phone
 *    Get user by phone number
 */
OrochiAuthUser *orochi_auth_get_user_by_phone(const char *phone)
{
    OrochiAuthUser *user = NULL;
    StringInfoData query;
    int ret;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];
    pg_uuid_t user_id;

    if (phone == NULL)
        return NULL;

    if (SPI_connect() != SPI_OK_CONNECT)
        return NULL;

    initStringInfo(&query);
    appendStringInfoString(&query,
                           "SELECT id FROM auth.users WHERE phone = $1 AND deleted_at IS NULL");

    argvals[0] = CStringGetTextDatum(phone);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        char *id_str = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (id_str) {
            Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&user_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));

            pfree(query.data);
            SPI_finish();

            return orochi_auth_get_user(user_id);
        }
    }

    pfree(query.data);
    SPI_finish();

    return user;
}

/*
 * orochi_auth_update_user
 *    Update user account details
 */
bool orochi_auth_update_user(pg_uuid_t user_id, const char *email, const char *phone,
                             const char *password, Jsonb *user_metadata, Jsonb *app_metadata)
{
    StringInfoData query;
    int ret;
    bool updated = false;
    char *encrypted_password = NULL;
    char uuid_str[37];
    Oid argtypes[1] = { UUIDOID };
    Datum argvals[1];
    bool first_update = true;

    /* Convert UUID to string */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    /* Hash password if provided */
    if (password != NULL && strlen(password) > 0) {
        if (strlen(password) < OROCHI_AUTH_MIN_PASSWORD_LENGTH)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("password too short (min %d characters)",
                                   OROCHI_AUTH_MIN_PASSWORD_LENGTH)));

        encrypted_password = orochi_auth_hash_password(password);
    }

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);
    appendStringInfoString(&query, "UPDATE auth.users SET ");

    if (email != NULL) {
        appendStringInfo(&query, "email = %s", quote_literal_cstr(email));
        first_update = false;
    }

    if (phone != NULL) {
        if (!first_update)
            appendStringInfoString(&query, ", ");
        appendStringInfo(&query, "phone = %s", quote_literal_cstr(phone));
        first_update = false;
    }

    if (encrypted_password != NULL) {
        if (!first_update)
            appendStringInfoString(&query, ", ");
        appendStringInfo(&query, "encrypted_password = %s", quote_literal_cstr(encrypted_password));
        first_update = false;
    }

    if (!first_update) {
        appendStringInfoString(&query, ", updated_at = NOW()");
        appendStringInfo(&query, " WHERE id = $1 AND deleted_at IS NULL");

        argvals[0] = UUIDPGetDatum(&user_id);

        ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);
        updated = (ret == SPI_OK_UPDATE && SPI_processed > 0);
    }

    pfree(query.data);
    if (encrypted_password)
        pfree(encrypted_password);
    SPI_finish();

    return updated;
}

/*
 * orochi_auth_delete_user
 *    Delete or soft-delete a user
 */
bool orochi_auth_delete_user(pg_uuid_t user_id, bool soft_delete)
{
    StringInfoData query;
    int ret;
    bool deleted = false;
    Oid argtypes[1] = { UUIDOID };
    Datum argvals[1];

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);

    if (soft_delete) {
        appendStringInfoString(&query,
                               "UPDATE auth.users SET deleted_at = NOW(), updated_at = NOW() "
                               "WHERE id = $1 AND deleted_at IS NULL");
    } else {
        appendStringInfoString(&query, "DELETE FROM auth.users WHERE id = $1");
    }

    argvals[0] = UUIDPGetDatum(&user_id);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);
    deleted = (ret == SPI_OK_UPDATE || ret == SPI_OK_DELETE) && SPI_processed > 0;

    pfree(query.data);
    SPI_finish();

    return deleted;
}

/*
 * orochi_auth_free_user
 *    Free user memory
 */
void orochi_auth_free_user(OrochiAuthUser *user)
{
    if (user == NULL)
        return;

    if (user->context)
        MemoryContextDelete(user->context);
}

/* ============================================================
 * Session Management
 * ============================================================ */

/*
 * orochi_auth_create_session
 *    Create a new user session
 */
OrochiAuthSession *orochi_auth_create_session(pg_uuid_t user_id, const char *user_agent,
                                              const char *ip_address, OrochiAuthAAL aal)
{
    OrochiAuthSession *session;
    StringInfoData query;
    int ret;
    char *refresh_token;
    char uuid_str[37];
    MemoryContext session_context;
    MemoryContext old_context;

    /* Generate refresh token */
    refresh_token = generate_refresh_token();

    /* Convert UUID to string */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    session_context =
        AllocSetContextCreate(TopMemoryContext, "AuthSession", ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(session_context);

    session = palloc0(sizeof(OrochiAuthSession));
    memcpy(&session->user_id, &user_id, sizeof(pg_uuid_t));
    session->refresh_token = pstrdup(refresh_token);
    session->aal = aal;

    if (user_agent)
        session->user_agent = pstrdup(user_agent);
    if (ip_address)
        session->ip_address = pstrdup(ip_address);

    MemoryContextSwitchTo(old_context);

    if (SPI_connect() != SPI_OK_CONNECT) {
        MemoryContextDelete(session_context);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to connect to SPI")));
    }

    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.sessions "
                     "(user_id, refresh_token, aal, user_agent, ip_address, amr, "
                     "created_at, updated_at, refreshed_at) "
                     "VALUES ('%s', %s, 'aal%d', %s, %s, '[]', NOW(), NOW(), NOW()) "
                     "RETURNING id, created_at",
                     uuid_str, quote_literal_cstr(refresh_token), (int)aal,
                     user_agent ? quote_literal_cstr(user_agent) : "NULL",
                     ip_address ? quote_literal_cstr(ip_address) : "NULL");

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
        char *id_str = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        bool isnull;

        if (id_str) {
            Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&session->id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
        }

        session->created_at = DatumGetTimestampTz(
            SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        session->updated_at = session->created_at;
        session->refreshed_at = session->created_at;

        /* Also store in refresh_tokens table */
        pfree(query.data);
        initStringInfo(&query);
        appendStringInfo(&query,
                         "INSERT INTO auth.refresh_tokens (session_id, user_id, "
                         "token, created_at) "
                         "VALUES ('%s', '%s', %s, NOW())",
                         id_str, uuid_str, quote_literal_cstr(refresh_token));

        SPI_execute(query.data, false, 0);

        /* Update shared memory counter */
        if (auth_shared_state) {
            LWLockAcquire(auth_shared_state->lock, LW_EXCLUSIVE);
            auth_shared_state->active_sessions++;
            LWLockRelease(auth_shared_state->lock);
        }
    } else {
        pfree(query.data);
        pfree(refresh_token);
        SPI_finish();
        MemoryContextDelete(session_context);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create session")));
    }

    pfree(query.data);
    pfree(refresh_token);
    SPI_finish();

    return session;
}

/*
 * orochi_auth_revoke_session
 *    Revoke a session
 */
bool orochi_auth_revoke_session(pg_uuid_t session_id)
{
    StringInfoData query;
    int ret;
    bool revoked = false;
    Oid argtypes[1] = { UUIDOID };
    Datum argvals[1];

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    /* Revoke refresh tokens */
    initStringInfo(&query);
    appendStringInfoString(&query,
                           "UPDATE auth.refresh_tokens SET revoked = TRUE, updated_at = NOW() "
                           "WHERE session_id = $1 AND revoked = FALSE");

    argvals[0] = UUIDPGetDatum(&session_id);

    SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);

    /* Delete session */
    pfree(query.data);
    initStringInfo(&query);
    appendStringInfoString(&query, "DELETE FROM auth.sessions WHERE id = $1");

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);
    revoked = (ret == SPI_OK_DELETE && SPI_processed > 0);

    pfree(query.data);
    SPI_finish();

    /* Update shared memory counter */
    if (revoked && auth_shared_state) {
        LWLockAcquire(auth_shared_state->lock, LW_EXCLUSIVE);
        if (auth_shared_state->active_sessions > 0)
            auth_shared_state->active_sessions--;
        LWLockRelease(auth_shared_state->lock);
    }

    return revoked;
}

/*
 * orochi_auth_free_session
 *    Free session memory
 */
void orochi_auth_free_session(OrochiAuthSession *session)
{
    if (session == NULL)
        return;

    if (session->refresh_token)
        pfree(session->refresh_token);
    if (session->access_token_hash)
        pfree(session->access_token_hash);
    if (session->user_agent)
        pfree(session->user_agent);
    if (session->ip_address)
        pfree(session->ip_address);
    if (session->tag)
        pfree(session->tag);

    pfree(session);
}

/* ============================================================
 * Authentication Functions
 * ============================================================ */

/*
 * orochi_auth_sign_in_password
 *    Authenticate user with email and password
 */
OrochiAuthResult *orochi_auth_sign_in_password(const char *email, const char *password)
{
    OrochiAuthResult *result;
    OrochiAuthUser *user;

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    if (email == NULL || password == NULL) {
        result->error_code = "invalid_credentials";
        result->error_message = "Email and password are required";
        return result;
    }

    /* Find user by email */
    user = orochi_auth_get_user_by_email(email);

    if (user == NULL) {
        result->error_code = "invalid_credentials";
        result->error_message = "Invalid email or password";
        return result;
    }

    /* Check if user is banned */
    if (user->banned_until > 0 && user->banned_until > GetCurrentTimestamp()) {
        result->error_code = "user_banned";
        result->error_message = "User account is banned";
        orochi_auth_free_user(user);
        return result;
    }

    /* Verify password */
    if (user->encrypted_password == NULL ||
        !orochi_auth_verify_password(password, user->encrypted_password)) {
        result->error_code = "invalid_credentials";
        result->error_message = "Invalid email or password";
        orochi_auth_free_user(user);
        return result;
    }

    /* Create session */
    result->session = orochi_auth_create_session(user->id, NULL, NULL, OROCHI_AAL_1);
    result->user = user;
    result->refresh_token = pstrdup(result->session->refresh_token);

    /* Generate JWT */
    result->access_token = orochi_auth_generate_jwt(user, result->session, orochi_auth_jwt_exp);
    result->expires_at = GetCurrentTimestamp() + (orochi_auth_jwt_exp * USECS_PER_SEC);

    /* Update last sign in */
    SPI_connect();
    {
        StringInfoData query;
        Oid argtypes[1] = { UUIDOID };
        Datum argvals[1];

        initStringInfo(&query);
        appendStringInfoString(&query,
                               "UPDATE auth.users SET last_sign_in_at = NOW() WHERE id = $1");

        argvals[0] = UUIDPGetDatum(&user->id);
        SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, false, 0);
        pfree(query.data);
    }
    SPI_finish();

    result->success = true;

    elog(LOG, "User signed in: %s", email);

    return result;
}

/*
 * orochi_auth_refresh_token
 *    Refresh an access token using a refresh token
 */
OrochiAuthResult *orochi_auth_refresh_token(const char *refresh_token)
{
    OrochiAuthResult *result;
    StringInfoData query;
    int ret;
    pg_uuid_t user_id, session_id;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    if (refresh_token == NULL) {
        result->error_code = "invalid_token";
        result->error_message = "Refresh token is required";
        return result;
    }

    if (SPI_connect() != SPI_OK_CONNECT) {
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    /* Find refresh token */
    initStringInfo(&query);
    appendStringInfoString(&query, "SELECT rt.user_id, rt.session_id FROM auth.refresh_tokens rt "
                                   "JOIN auth.sessions s ON rt.session_id = s.id "
                                   "WHERE rt.token = $1 AND rt.revoked = FALSE "
                                   "AND (s.not_after IS NULL OR s.not_after > NOW())");

    argvals[0] = CStringGetTextDatum(refresh_token);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed == 0) {
        pfree(query.data);
        SPI_finish();
        result->error_code = "invalid_token";
        result->error_message = "Invalid or expired refresh token";
        return result;
    }

    /* Extract user_id and session_id */
    {
        char *user_id_str = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        char *session_id_str = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);
        Datum uuid_datum;

        uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(user_id_str));
        memcpy(&user_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));

        uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(session_id_str));
        memcpy(&session_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
    }

    pfree(query.data);

    /* Generate new refresh token and rotate */
    {
        char *new_refresh_token = generate_refresh_token();

        initStringInfo(&query);
        appendStringInfo(&query,
                         "UPDATE auth.refresh_tokens SET revoked = TRUE, updated_at = NOW() "
                         "WHERE token = %s",
                         quote_literal_cstr(refresh_token));
        SPI_execute(query.data, false, 0);
        pfree(query.data);

        initStringInfo(&query);
        appendStringInfo(&query,
                         "INSERT INTO auth.refresh_tokens (session_id, user_id, token, parent, "
                         "created_at) "
                         "VALUES ((SELECT session_id FROM auth.refresh_tokens WHERE token = "
                         "%s), "
                         "(SELECT user_id FROM auth.refresh_tokens WHERE token = %s), "
                         "%s, %s, NOW())",
                         quote_literal_cstr(refresh_token), quote_literal_cstr(refresh_token),
                         quote_literal_cstr(new_refresh_token), quote_literal_cstr(refresh_token));
        SPI_execute(query.data, false, 0);
        pfree(query.data);

        /* Update session */
        initStringInfo(&query);
        appendStringInfo(&query,
                         "UPDATE auth.sessions SET refresh_token = %s, refreshed_at = NOW(), "
                         "updated_at = NOW() WHERE id = (SELECT session_id FROM "
                         "auth.refresh_tokens "
                         "WHERE token = %s LIMIT 1)",
                         quote_literal_cstr(new_refresh_token), quote_literal_cstr(refresh_token));
        SPI_execute(query.data, false, 0);
        pfree(query.data);

        result->refresh_token = new_refresh_token;
    }

    SPI_finish();

    /* Get user and generate new JWT */
    result->user = orochi_auth_get_user(user_id);
    if (result->user == NULL) {
        result->error_code = "user_not_found";
        result->error_message = "User not found";
        return result;
    }

    /* Create a temporary session object for JWT generation */
    result->session = palloc0(sizeof(OrochiAuthSession));
    memcpy(&result->session->id, &session_id, sizeof(pg_uuid_t));
    memcpy(&result->session->user_id, &user_id, sizeof(pg_uuid_t));
    result->session->aal = OROCHI_AAL_1;

    result->access_token =
        orochi_auth_generate_jwt(result->user, result->session, orochi_auth_jwt_exp);
    result->expires_at = GetCurrentTimestamp() + (orochi_auth_jwt_exp * USECS_PER_SEC);

    result->success = true;

    return result;
}

/*
 * orochi_auth_sign_out
 *    Sign out a user session
 */
bool orochi_auth_sign_out(pg_uuid_t session_id)
{
    return orochi_auth_revoke_session(session_id);
}

/* ============================================================
 * JWT Generation
 * ============================================================ */

/*
 * build_jwt_payload
 *    Build JWT payload from user and session
 */
static Jsonb *build_jwt_payload(OrochiAuthUser *user, OrochiAuthSession *session, int exp_seconds)
{
    StringInfoData payload;
    Datum payload_datum;
    int64 now_unix;
    char uuid_str[37];

    now_unix = (GetCurrentTimestamp() / USECS_PER_SEC) +
               ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

    /* Convert user ID to string */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
             user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
             user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
             user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

    initStringInfo(&payload);
    appendStringInfoChar(&payload, '{');

    appendStringInfo(&payload, "\"aud\": \"authenticated\"");
    appendStringInfo(&payload, ", \"exp\": %ld", now_unix + exp_seconds);
    appendStringInfo(&payload, ", \"iat\": %ld", now_unix);
    appendStringInfo(&payload, ", \"iss\": \"orochi-auth\"");
    appendStringInfo(&payload, ", \"sub\": \"%s\"", uuid_str);

    if (user->email)
        appendStringInfo(&payload, ", \"email\": \"%s\"", user->email);

    if (user->phone)
        appendStringInfo(&payload, ", \"phone\": \"%s\"", user->phone);

    appendStringInfo(&payload, ", \"role\": \"%s\"", user->role ? user->role : "authenticated");

    appendStringInfo(&payload, ", \"aal\": \"aal%d\"", session ? (int)session->aal : 1);

    if (session) {
        char session_uuid_str[37];
        snprintf(session_uuid_str, sizeof(session_uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 session->id.data[0], session->id.data[1], session->id.data[2], session->id.data[3],
                 session->id.data[4], session->id.data[5], session->id.data[6], session->id.data[7],
                 session->id.data[8], session->id.data[9], session->id.data[10],
                 session->id.data[11], session->id.data[12], session->id.data[13],
                 session->id.data[14], session->id.data[15]);

        appendStringInfo(&payload, ", \"session_id\": \"%s\"", session_uuid_str);
    }

    appendStringInfo(&payload, ", \"is_anonymous\": %s", user->is_anonymous ? "true" : "false");

    appendStringInfoChar(&payload, '}');

    payload_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(payload.data));
    pfree(payload.data);

    return DatumGetJsonbP(payload_datum);
}

/*
 * sign_jwt
 *    Sign JWT header and payload with HMAC-SHA256
 */
static char *sign_jwt(const char *header, const char *payload, const char *secret)
{
    char *header_b64;
    char *payload_b64;
    char *signing_input;
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int sig_len;
    char *signature_b64;
    char *token;

    header_b64 = base64url_encode((unsigned char *)header, strlen(header));
    payload_b64 = base64url_encode((unsigned char *)payload, strlen(payload));

    signing_input = psprintf("%s.%s", header_b64, payload_b64);

    if (!HMAC(EVP_sha256(), secret, strlen(secret), (unsigned char *)signing_input,
              strlen(signing_input), signature, &sig_len)) {
        pfree(header_b64);
        pfree(payload_b64);
        pfree(signing_input);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to sign JWT")));
    }

    signature_b64 = base64url_encode(signature, sig_len);

    token = psprintf("%s.%s", signing_input, signature_b64);

    pfree(header_b64);
    pfree(payload_b64);
    pfree(signing_input);
    pfree(signature_b64);

    return token;
}

/*
 * orochi_auth_generate_jwt
 *    Generate a JWT for a user
 */
char *orochi_auth_generate_jwt(OrochiAuthUser *user, OrochiAuthSession *session, int exp_seconds)
{
    const char *header = "{\"alg\": \"HS256\", \"typ\": \"JWT\"}";
    Jsonb *payload_jsonb;
    StringInfoData payload_str;
    char *jwt;
    char *secret;

    if (user == NULL)
        return NULL;

    /* Get JWT secret from config or GUC */
    secret = orochi_auth_jwt_secret;
    if (secret == NULL || strlen(secret) == 0) {
        /* Try to get from auth.config table */
        OrochiAuthConfig *config = orochi_auth_get_config();
        if (config && config->jwt_secret)
            secret = config->jwt_secret;
    }

    if (secret == NULL || strlen(secret) == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("JWT secret not configured")));
    }

    payload_jsonb = build_jwt_payload(user, session, exp_seconds);

    initStringInfo(&payload_str);
    (void)JsonbToCString(&payload_str, &payload_jsonb->root, VARSIZE(payload_jsonb));

    jwt = sign_jwt(header, payload_str.data, secret);

    pfree(payload_str.data);

    return jwt;
}

/* ============================================================
 * Configuration
 * ============================================================ */

/*
 * orochi_auth_get_config
 *    Get auth configuration from database
 */
OrochiAuthConfig *orochi_auth_get_config(void)
{
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;

    config = palloc0(sizeof(OrochiAuthConfig));

    /* Set defaults */
    config->jwt_exp = OROCHI_AUTH_DEFAULT_JWT_EXP;
    config->password_min_length = OROCHI_AUTH_MIN_PASSWORD_LENGTH;
    config->enable_anonymous_sign_in = true;
    config->rate_limit_email_sent = OROCHI_AUTH_RATE_LIMIT_EMAIL;
    config->rate_limit_sms_sent = OROCHI_AUTH_RATE_LIMIT_SMS;
    config->rate_limit_token_refresh = OROCHI_AUTH_RATE_LIMIT_REFRESH;

    if (SPI_connect() != SPI_OK_CONNECT)
        return config;

    initStringInfo(&query);
    appendStringInfoString(&query, "SELECT jwt_secret, jwt_exp, site_url, disable_signup, "
                                   "enable_anonymous_sign_in, password_min_length "
                                   "FROM auth.config LIMIT 1");

    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        config->jwt_secret = SPI_getvalue(tuple, tupdesc, 1);
        if (config->jwt_secret)
            config->jwt_secret = pstrdup(config->jwt_secret);

        config->jwt_exp = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
        if (isnull)
            config->jwt_exp = OROCHI_AUTH_DEFAULT_JWT_EXP;

        config->site_url = SPI_getvalue(tuple, tupdesc, 3);
        if (config->site_url)
            config->site_url = pstrdup(config->site_url);

        config->disable_signup = DatumGetBool(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        config->enable_anonymous_sign_in = DatumGetBool(SPI_getbinval(tuple, tupdesc, 5, &isnull));
        if (isnull)
            config->enable_anonymous_sign_in = true;

        config->password_min_length = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 6, &isnull));
        if (isnull)
            config->password_min_length = OROCHI_AUTH_MIN_PASSWORD_LENGTH;
    }

    pfree(query.data);
    SPI_finish();

    return config;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *orochi_auth_aal_name(OrochiAuthAAL aal)
{
    switch (aal) {
    case OROCHI_AAL_1:
        return "aal1";
    case OROCHI_AAL_2:
        return "aal2";
    case OROCHI_AAL_3:
        return "aal3";
    default:
        return "aal1";
    }
}

const char *orochi_auth_factor_type_name(OrochiAuthFactorType type)
{
    switch (type) {
    case OROCHI_FACTOR_TOTP:
        return "totp";
    case OROCHI_FACTOR_PHONE:
        return "phone";
    case OROCHI_FACTOR_WEBAUTHN:
        return "webauthn";
    default:
        return "unknown";
    }
}

const char *orochi_auth_token_type_name(OrochiAuthTokenType type)
{
    switch (type) {
    case OROCHI_TOKEN_CONFIRMATION:
        return "confirmation";
    case OROCHI_TOKEN_RECOVERY:
        return "recovery";
    case OROCHI_TOKEN_EMAIL_CHANGE_NEW:
        return "email_change_new";
    case OROCHI_TOKEN_EMAIL_CHANGE_OLD:
        return "email_change_old";
    case OROCHI_TOKEN_PHONE_CHANGE:
        return "phone_change";
    case OROCHI_TOKEN_REAUTHENTICATION:
        return "reauthentication";
    default:
        return "unknown";
    }
}

const char *orochi_auth_provider_name(OrochiAuthProvider provider)
{
    switch (provider) {
    case OROCHI_PROVIDER_EMAIL:
        return "email";
    case OROCHI_PROVIDER_PHONE:
        return "phone";
    case OROCHI_PROVIDER_ANONYMOUS:
        return "anonymous";
    case OROCHI_PROVIDER_GOOGLE:
        return "google";
    case OROCHI_PROVIDER_GITHUB:
        return "github";
    case OROCHI_PROVIDER_FACEBOOK:
        return "facebook";
    case OROCHI_PROVIDER_APPLE:
        return "apple";
    case OROCHI_PROVIDER_AZURE:
        return "azure";
    case OROCHI_PROVIDER_SAML:
        return "saml";
    default:
        return "unknown";
    }
}

OrochiAuthProvider orochi_auth_parse_provider(const char *name)
{
    if (pg_strcasecmp(name, "email") == 0)
        return OROCHI_PROVIDER_EMAIL;
    if (pg_strcasecmp(name, "phone") == 0)
        return OROCHI_PROVIDER_PHONE;
    if (pg_strcasecmp(name, "anonymous") == 0)
        return OROCHI_PROVIDER_ANONYMOUS;
    if (pg_strcasecmp(name, "google") == 0)
        return OROCHI_PROVIDER_GOOGLE;
    if (pg_strcasecmp(name, "github") == 0)
        return OROCHI_PROVIDER_GITHUB;
    if (pg_strcasecmp(name, "facebook") == 0)
        return OROCHI_PROVIDER_FACEBOOK;
    if (pg_strcasecmp(name, "apple") == 0)
        return OROCHI_PROVIDER_APPLE;
    if (pg_strcasecmp(name, "azure") == 0)
        return OROCHI_PROVIDER_AZURE;
    if (pg_strcasecmp(name, "saml") == 0)
        return OROCHI_PROVIDER_SAML;

    return OROCHI_PROVIDER_EMAIL;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_create_user_sql);
PG_FUNCTION_INFO_V1(orochi_auth_sign_in_sql);
PG_FUNCTION_INFO_V1(orochi_auth_sign_out_sql);
PG_FUNCTION_INFO_V1(orochi_auth_refresh_token_sql);
PG_FUNCTION_INFO_V1(orochi_auth_uid_sql);
PG_FUNCTION_INFO_V1(orochi_auth_role_sql);

Datum orochi_auth_create_user_sql(PG_FUNCTION_ARGS)
{
    text *email_text = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
    text *phone_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);
    text *password_text = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);

    char *email = email_text ? text_to_cstring(email_text) : NULL;
    char *phone = phone_text ? text_to_cstring(phone_text) : NULL;
    char *password = password_text ? text_to_cstring(password_text) : NULL;

    OrochiAuthUser *user = orochi_auth_create_user(email, phone, password, NULL);

    if (user == NULL)
        PG_RETURN_NULL();

    PG_RETURN_POINTER(&user->id);
}

Datum orochi_auth_sign_in_sql(PG_FUNCTION_ARGS)
{
    text *email_text = PG_GETARG_TEXT_PP(0);
    text *password_text = PG_GETARG_TEXT_PP(1);
    char *email = text_to_cstring(email_text);
    char *password = text_to_cstring(password_text);
    OrochiAuthResult *result;
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    result = orochi_auth_sign_in_password(email, password);

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context "
                               "that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (result->success) {
        values[0] = BoolGetDatum(true);
        values[1] = CStringGetTextDatum(result->access_token);
        values[2] = CStringGetTextDatum(result->refresh_token);
        values[3] = TimestampTzGetDatum(result->expires_at);
    } else {
        values[0] = BoolGetDatum(false);
        nulls[1] = true;
        nulls[2] = true;
        nulls[3] = true;
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum orochi_auth_sign_out_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *session_uuid = PG_GETARG_UUID_P(0);
    bool result = orochi_auth_sign_out(*session_uuid);
    PG_RETURN_BOOL(result);
}

Datum orochi_auth_refresh_token_sql(PG_FUNCTION_ARGS)
{
    text *token_text = PG_GETARG_TEXT_PP(0);
    char *token = text_to_cstring(token_text);
    OrochiAuthResult *result;
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    result = orochi_auth_refresh_token(token);

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("function returning record called in context "
                               "that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (result->success) {
        values[0] = BoolGetDatum(true);
        values[1] = CStringGetTextDatum(result->access_token);
        values[2] = CStringGetTextDatum(result->refresh_token);
        values[3] = TimestampTzGetDatum(result->expires_at);
    } else {
        values[0] = BoolGetDatum(false);
        nulls[1] = true;
        nulls[2] = true;
        nulls[3] = true;
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum orochi_auth_uid_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t uid = orochi_auth_uid();

    /* Check if UID is all zeros (not authenticated) */
    bool all_zero = true;
    for (int i = 0; i < 16; i++) {
        if (uid.data[i] != 0) {
            all_zero = false;
            break;
        }
    }

    if (all_zero)
        PG_RETURN_NULL();

    PG_RETURN_UUID_P(&uid);
}

Datum orochi_auth_role_sql(PG_FUNCTION_ARGS)
{
    char *role = orochi_auth_role();

    if (role == NULL)
        PG_RETURN_TEXT_P(cstring_to_text("anon"));

    PG_RETURN_TEXT_P(cstring_to_text(role));
}

/* ============================================================
 * JWT Context Functions
 * ============================================================ */

/*
 * orochi_auth_uid
 *    Get current user ID from JWT context
 */
pg_uuid_t orochi_auth_uid(void)
{
    pg_uuid_t uid;
    const char *sub;

    memset(&uid, 0, sizeof(pg_uuid_t));

    sub = GetConfigOption("request.jwt.claim.sub", true, false);
    if (sub != NULL && strlen(sub) > 0) {
        Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(sub));
        memcpy(&uid, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
    }

    return uid;
}

/*
 * orochi_auth_role
 *    Get current user role from JWT context
 */
char *orochi_auth_role(void)
{
    const char *role;

    role = GetConfigOption("request.jwt.claim.role", true, false);
    if (role == NULL || strlen(role) == 0)
        return "anon";

    return pstrdup(role);
}

/*
 * orochi_auth_email
 *    Get current user email from JWT context
 */
char *orochi_auth_email(void)
{
    const char *email;

    email = GetConfigOption("request.jwt.claim.email", true, false);
    if (email == NULL || strlen(email) == 0)
        return NULL;

    return pstrdup(email);
}

/*
 * orochi_auth_aal
 *    Get current AAL from JWT context
 */
OrochiAuthAAL orochi_auth_aal(void)
{
    const char *aal;

    aal = GetConfigOption("request.jwt.claim.aal", true, false);
    if (aal == NULL || strlen(aal) == 0)
        return OROCHI_AAL_1;

    if (strcmp(aal, "aal2") == 0)
        return OROCHI_AAL_2;
    if (strcmp(aal, "aal3") == 0)
        return OROCHI_AAL_3;

    return OROCHI_AAL_1;
}
