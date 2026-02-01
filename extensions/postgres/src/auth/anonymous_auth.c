/*-------------------------------------------------------------------------
 *
 * anonymous_auth.c
 *    Orochi DB Anonymous authentication with upgradable accounts
 *
 * This module provides anonymous authentication features including:
 *   - Anonymous user creation with minimal identity
 *   - Account upgrade/linking to email or phone
 *   - Data preservation during account linking
 *   - OAuth provider identity linking
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "gotrue.h"

/* Forward declarations */
static void preserve_user_data(pg_uuid_t anonymous_id, pg_uuid_t target_id);
static bool validate_linking_capability(OrochiAuthUser *user);
static void emit_webhook_event(const char *event_type, pg_uuid_t user_id, Jsonb *payload);

/* ============================================================
 * Anonymous User Creation
 * ============================================================ */

/*
 * orochi_auth_sign_in_anonymous
 *    Create an anonymous user and return a session
 *
 * Anonymous users have:
 *   - A UUID identity
 *   - No email or phone
 *   - is_anonymous = true flag
 *   - Can be upgraded to full accounts
 */
OrochiAuthResult *orochi_auth_sign_in_anonymous(void)
{
    OrochiAuthResult *result;
    OrochiAuthUser *user;
    OrochiAuthSession *session;
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;
    MemoryContext user_context;
    MemoryContext old_context;

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    /* Check if anonymous sign-in is enabled */
    config = orochi_auth_get_config();
    if (!config->enable_anonymous_sign_in) {
        result->error_code = "anonymous_disabled";
        result->error_message = "Anonymous sign-in is disabled";
        return result;
    }

    /* Create memory context for user */
    user_context = AllocSetContextCreate(TopMemoryContext, "AuthUser", ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(user_context);

    user = palloc0(sizeof(OrochiAuthUser));
    user->context = user_context;

    MemoryContextSwitchTo(old_context);

    if (SPI_connect() != SPI_OK_CONNECT) {
        MemoryContextDelete(user_context);
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    /* Insert anonymous user */
    initStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO auth.users "
                             "(email, phone, encrypted_password, raw_user_meta_data, "
                             "raw_app_meta_data, role, is_anonymous, created_at, updated_at) "
                             "VALUES (NULL, NULL, NULL, "
                             "'{\"provider\": \"anonymous\"}'::jsonb, "
                             "'{\"provider\": \"anonymous\"}'::jsonb, "
                             "'authenticated', TRUE, NOW(), NOW()) "
                             "RETURNING id, role, created_at");

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        char *id_str;
        Datum uuid_datum;

        id_str = SPI_getvalue(tuple, tupdesc, 1);
        if (id_str) {
            uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&user->id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
        }

        old_context = MemoryContextSwitchTo(user_context);

        user->role = pstrdup("authenticated");
        user->is_anonymous = true;
        user->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 3, &isnull));
        user->updated_at = user->created_at;

        MemoryContextSwitchTo(old_context);
    } else {
        pfree(query.data);
        SPI_finish();
        MemoryContextDelete(user_context);
        result->error_code = "internal_error";
        result->error_message = "Failed to create anonymous user";
        return result;
    }

    pfree(query.data);

    /* Create an identity record for the anonymous provider */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
                 user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
                 user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
                 user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

        appendStringInfo(&query,
                         "INSERT INTO auth.identities "
                         "(user_id, provider_id, provider, identity_data, created_at, "
                         "updated_at) "
                         "VALUES ('%s', '%s', 'anonymous', "
                         "'{\"sub\": \"%s\", \"is_anonymous\": true}'::jsonb, NOW(), NOW())",
                         uuid_str, uuid_str, uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Log audit entry */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
                 user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
                 user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
                 user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

        appendStringInfo(&query,
                         "INSERT INTO auth.audit_log_entries (payload, created_at) "
                         "VALUES ('{\"action\": \"anonymous_sign_in\", \"actor_id\": \"%s\", "
                         "\"log_type\": \"user\"}'::jsonb, NOW())",
                         uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    /* Create session */
    session = orochi_auth_create_session(user->id, NULL, NULL, OROCHI_AAL_1);

    result->user = user;
    result->session = session;
    result->refresh_token = pstrdup(session->refresh_token);
    result->access_token = orochi_auth_generate_jwt(user, session, orochi_auth_jwt_exp);
    result->expires_at = GetCurrentTimestamp() + (orochi_auth_jwt_exp * USECS_PER_SEC);
    result->success = true;

    /* Emit webhook event */
    {
        StringInfoData payload_json;
        Jsonb *payload;

        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
                 user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
                 user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
                 user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

        initStringInfo(&payload_json);
        appendStringInfo(&payload_json, "{\"user_id\": \"%s\", \"is_anonymous\": true}", uuid_str);

        payload = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json.data)));
        emit_webhook_event("user.signed_in", user->id, payload);
        pfree(payload_json.data);
    }

    elog(LOG, "Anonymous user signed in");

    return result;
}

/*
 * orochi_auth_is_anonymous
 *    Check if a user is anonymous
 */
bool orochi_auth_is_anonymous(pg_uuid_t user_id)
{
    OrochiAuthUser *user = orochi_auth_get_user(user_id);

    if (user == NULL)
        return false;

    bool is_anon = user->is_anonymous;
    orochi_auth_free_user(user);

    return is_anon;
}

/* ============================================================
 * Account Upgrade/Linking
 * ============================================================ */

/*
 * orochi_auth_link_email
 *    Link an email to an anonymous account, upgrading it
 *
 * This preserves all user data and sessions while adding email identity.
 */
OrochiAuthResult *orochi_auth_link_email(pg_uuid_t user_id, const char *email, const char *password)
{
    OrochiAuthResult *result;
    OrochiAuthUser *user;
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;
    char *encrypted_password = NULL;
    char uuid_str[37];
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    /* Validate inputs */
    if (email == NULL || strlen(email) == 0) {
        result->error_code = "invalid_email";
        result->error_message = "Email is required";
        return result;
    }

    /* Get user */
    user = orochi_auth_get_user(user_id);
    if (user == NULL) {
        result->error_code = "user_not_found";
        result->error_message = "User not found";
        return result;
    }

    /* Verify user is anonymous */
    if (!user->is_anonymous) {
        orochi_auth_free_user(user);
        result->error_code = "not_anonymous";
        result->error_message = "Only anonymous users can link new identities";
        return result;
    }

    /* Check if email is already in use */
    if (SPI_connect() != SPI_OK_CONNECT) {
        orochi_auth_free_user(user);
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    initStringInfo(&query);
    appendStringInfoString(&query,
                           "SELECT id FROM auth.users WHERE email = $1 AND deleted_at IS NULL");

    argvals[0] = CStringGetTextDatum(email);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        pfree(query.data);
        SPI_finish();
        orochi_auth_free_user(user);
        result->error_code = "email_exists";
        result->error_message = "Email is already in use by another account";
        return result;
    }

    pfree(query.data);

    /* Hash password if provided */
    if (password != NULL && strlen(password) > 0) {
        if (strlen(password) < OROCHI_AUTH_MIN_PASSWORD_LENGTH) {
            SPI_finish();
            orochi_auth_free_user(user);
            result->error_code = "weak_password";
            result->error_message = psprintf("Password must be at least %d characters",
                                             OROCHI_AUTH_MIN_PASSWORD_LENGTH);
            return result;
        }
        encrypted_password = orochi_auth_hash_password(password);
    }

    /* Convert UUID to string */
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    /* Update user to add email and clear anonymous flag */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.users SET "
                     "email = %s, "
                     "encrypted_password = %s, "
                     "is_anonymous = FALSE, "
                     "raw_app_meta_data = raw_app_meta_data || "
                     "'{\"provider\": \"email\", \"providers\": [\"email\"]}'::jsonb, "
                     "updated_at = NOW() "
                     "WHERE id = '%s'",
                     quote_literal_cstr(email),
                     encrypted_password ? quote_literal_cstr(encrypted_password) : "NULL",
                     uuid_str);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);

    if (encrypted_password)
        pfree(encrypted_password);

    if (ret != SPI_OK_UPDATE || SPI_processed == 0) {
        SPI_finish();
        orochi_auth_free_user(user);
        result->error_code = "update_failed";
        result->error_message = "Failed to update user";
        return result;
    }

    /* Add email identity */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.identities "
                     "(user_id, provider_id, provider, identity_data, created_at, updated_at) "
                     "VALUES ('%s', %s, 'email', "
                     "'{\"sub\": \"%s\", \"email\": \"%s\", \"email_verified\": "
                     "false}'::jsonb, "
                     "NOW(), NOW()) "
                     "ON CONFLICT (provider, provider_id) DO UPDATE SET "
                     "identity_data = EXCLUDED.identity_data, updated_at = NOW()",
                     uuid_str, quote_literal_cstr(email), uuid_str, email);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Log audit entry */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_log_entries (payload, created_at) "
                     "VALUES ('{\"action\": \"user_linked_email\", \"actor_id\": \"%s\", "
                     "\"email\": \"%s\", \"log_type\": \"user\"}'::jsonb, NOW())",
                     uuid_str, email);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    /* Emit webhook event */
    {
        StringInfoData payload_json;
        Jsonb *payload;

        initStringInfo(&payload_json);
        appendStringInfo(&payload_json,
                         "{\"user_id\": \"%s\", \"email\": \"%s\", "
                         "\"linked_from_anonymous\": true}",
                         uuid_str, email);

        payload = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json.data)));
        emit_webhook_event("user.updated", user_id, payload);
        pfree(payload_json.data);
    }

    /* Reload user */
    orochi_auth_free_user(user);
    user = orochi_auth_get_user(user_id);

    result->user = user;
    result->success = true;

    elog(LOG, "Anonymous user linked email: %s", email);

    return result;
}

/*
 * orochi_auth_link_phone
 *    Link a phone to an anonymous account
 */
OrochiAuthResult *orochi_auth_link_phone(pg_uuid_t user_id, const char *phone)
{
    OrochiAuthResult *result;
    OrochiAuthUser *user;
    StringInfoData query;
    int ret;
    char uuid_str[37];
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    if (phone == NULL || strlen(phone) == 0) {
        result->error_code = "invalid_phone";
        result->error_message = "Phone number is required";
        return result;
    }

    user = orochi_auth_get_user(user_id);
    if (user == NULL) {
        result->error_code = "user_not_found";
        result->error_message = "User not found";
        return result;
    }

    if (!user->is_anonymous) {
        orochi_auth_free_user(user);
        result->error_code = "not_anonymous";
        result->error_message = "Only anonymous users can link new identities";
        return result;
    }

    if (SPI_connect() != SPI_OK_CONNECT) {
        orochi_auth_free_user(user);
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    /* Check if phone is already in use */
    initStringInfo(&query);
    appendStringInfoString(&query,
                           "SELECT id FROM auth.users WHERE phone = $1 AND deleted_at IS NULL");

    argvals[0] = CStringGetTextDatum(phone);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        pfree(query.data);
        SPI_finish();
        orochi_auth_free_user(user);
        result->error_code = "phone_exists";
        result->error_message = "Phone number is already in use";
        return result;
    }

    pfree(query.data);

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    /* Update user */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.users SET "
                     "phone = %s, "
                     "is_anonymous = FALSE, "
                     "raw_app_meta_data = raw_app_meta_data || "
                     "'{\"provider\": \"phone\", \"providers\": [\"phone\"]}'::jsonb, "
                     "updated_at = NOW() "
                     "WHERE id = '%s'",
                     quote_literal_cstr(phone), uuid_str);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);

    if (ret != SPI_OK_UPDATE || SPI_processed == 0) {
        SPI_finish();
        orochi_auth_free_user(user);
        result->error_code = "update_failed";
        result->error_message = "Failed to update user";
        return result;
    }

    /* Add phone identity */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.identities "
                     "(user_id, provider_id, provider, identity_data, created_at, updated_at) "
                     "VALUES ('%s', %s, 'phone', "
                     "'{\"sub\": \"%s\", \"phone\": \"%s\", \"phone_verified\": "
                     "false}'::jsonb, "
                     "NOW(), NOW()) "
                     "ON CONFLICT (provider, provider_id) DO UPDATE SET "
                     "identity_data = EXCLUDED.identity_data, updated_at = NOW()",
                     uuid_str, quote_literal_cstr(phone), uuid_str, phone);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Log audit entry */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_log_entries (payload, created_at) "
                     "VALUES ('{\"action\": \"user_linked_phone\", \"actor_id\": \"%s\", "
                     "\"log_type\": \"user\"}'::jsonb, NOW())",
                     uuid_str);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    /* Emit webhook event */
    {
        StringInfoData payload_json;
        Jsonb *payload;

        initStringInfo(&payload_json);
        appendStringInfo(&payload_json,
                         "{\"user_id\": \"%s\", \"phone\": \"%s\", "
                         "\"linked_from_anonymous\": true}",
                         uuid_str, phone);

        payload = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json.data)));
        emit_webhook_event("user.updated", user_id, payload);
        pfree(payload_json.data);
    }

    orochi_auth_free_user(user);
    user = orochi_auth_get_user(user_id);

    result->user = user;
    result->success = true;

    elog(LOG, "Anonymous user linked phone");

    return result;
}

/*
 * orochi_auth_link_oauth_identity
 *    Link an OAuth provider identity to an anonymous account
 */
OrochiAuthResult *orochi_auth_link_oauth_identity(pg_uuid_t user_id, OrochiAuthProvider provider,
                                                  const char *provider_id, Jsonb *identity_data)
{
    OrochiAuthResult *result;
    OrochiAuthUser *user;
    StringInfoData query;
    int ret;
    char uuid_str[37];
    const char *provider_name;
    StringInfoData identity_json;

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    if (provider_id == NULL || strlen(provider_id) == 0) {
        result->error_code = "invalid_provider_id";
        result->error_message = "Provider ID is required";
        return result;
    }

    user = orochi_auth_get_user(user_id);
    if (user == NULL) {
        result->error_code = "user_not_found";
        result->error_message = "User not found";
        return result;
    }

    if (!user->is_anonymous) {
        orochi_auth_free_user(user);
        result->error_code = "not_anonymous";
        result->error_message = "Only anonymous users can link new identities via this method";
        return result;
    }

    provider_name = orochi_auth_provider_name(provider);

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    if (SPI_connect() != SPI_OK_CONNECT) {
        orochi_auth_free_user(user);
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    /* Check if identity already exists */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT user_id FROM auth.identities "
                     "WHERE provider = %s AND provider_id = %s",
                     quote_literal_cstr(provider_name), quote_literal_cstr(provider_id));

    ret = SPI_execute(query.data, true, 1);
    pfree(query.data);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        SPI_finish();
        orochi_auth_free_user(user);
        result->error_code = "identity_exists";
        result->error_message = "This identity is already linked to another account";
        return result;
    }

    /* Serialize identity data */
    if (identity_data) {
        initStringInfo(&identity_json);
        (void)JsonbToCString(&identity_json, &identity_data->root, VARSIZE(identity_data));
    } else {
        initStringInfo(&identity_json);
        appendStringInfo(&identity_json, "{\"sub\": \"%s\"}", provider_id);
    }

    /* Update user to clear anonymous flag */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.users SET "
                     "is_anonymous = FALSE, "
                     "is_sso_user = TRUE, "
                     "raw_app_meta_data = raw_app_meta_data || "
                     "'{\"provider\": \"%s\", \"providers\": [\"%s\"]}'::jsonb, "
                     "updated_at = NOW() "
                     "WHERE id = '%s'",
                     provider_name, provider_name, uuid_str);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Insert identity */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.identities "
                     "(user_id, provider_id, provider, identity_data, created_at, updated_at) "
                     "VALUES ('%s', %s, %s, %s::jsonb, NOW(), NOW())",
                     uuid_str, quote_literal_cstr(provider_id), quote_literal_cstr(provider_name),
                     quote_literal_cstr(identity_json.data));

    SPI_execute(query.data, false, 0);
    pfree(query.data);
    pfree(identity_json.data);

    /* Log audit entry */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "INSERT INTO auth.audit_log_entries (payload, created_at) "
                     "VALUES ('{\"action\": \"user_linked_oauth\", \"actor_id\": \"%s\", "
                     "\"provider\": \"%s\", \"log_type\": \"user\"}'::jsonb, NOW())",
                     uuid_str, provider_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    /* Emit webhook event */
    {
        StringInfoData payload_json;
        Jsonb *payload;

        initStringInfo(&payload_json);
        appendStringInfo(&payload_json,
                         "{\"user_id\": \"%s\", \"provider\": \"%s\", "
                         "\"linked_from_anonymous\": true}",
                         uuid_str, provider_name);

        payload = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(payload_json.data)));
        emit_webhook_event("user.updated", user_id, payload);
        pfree(payload_json.data);
    }

    orochi_auth_free_user(user);
    user = orochi_auth_get_user(user_id);

    result->user = user;
    result->success = true;

    elog(LOG, "Anonymous user linked OAuth identity: %s", provider_name);

    return result;
}

/*
 * orochi_auth_convert_anonymous_to_permanent
 *    Convert an anonymous user to a permanent account without linking identity
 *    This just removes the anonymous flag, requiring later identity linking
 */
bool orochi_auth_convert_anonymous_to_permanent(pg_uuid_t user_id)
{
    OrochiAuthUser *user;
    StringInfoData query;
    int ret;
    char uuid_str[37];

    user = orochi_auth_get_user(user_id);
    if (user == NULL)
        return false;

    if (!user->is_anonymous) {
        orochi_auth_free_user(user);
        return true; /* Already permanent */
    }

    orochi_auth_free_user(user);

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    if (SPI_connect() != SPI_OK_CONNECT)
        return false;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE auth.users SET is_anonymous = FALSE, updated_at = NOW() "
                     "WHERE id = '%s' AND is_anonymous = TRUE",
                     uuid_str);

    ret = SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    return (ret == SPI_OK_UPDATE && SPI_processed > 0);
}

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * emit_webhook_event
 *    Emit a webhook event for auth actions
 */
static void emit_webhook_event(const char *event_type, pg_uuid_t user_id, Jsonb *payload)
{
    StringInfoData notify_payload;
    StringInfoData payload_str;
    char uuid_str[37];

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3], user_id.data[4],
             user_id.data[5], user_id.data[6], user_id.data[7], user_id.data[8], user_id.data[9],
             user_id.data[10], user_id.data[11], user_id.data[12], user_id.data[13],
             user_id.data[14], user_id.data[15]);

    initStringInfo(&payload_str);
    if (payload)
        (void)JsonbToCString(&payload_str, &payload->root, VARSIZE(payload));
    else
        appendStringInfoString(&payload_str, "{}");

    initStringInfo(&notify_payload);
    appendStringInfo(&notify_payload,
                     "{\"event\": \"%s\", \"user_id\": \"%s\", \"timestamp\": "
                     "\"%s\", \"data\": %s}",
                     event_type, uuid_str, timestamptz_to_str(GetCurrentTimestamp()),
                     payload_str.data);

    pfree(payload_str.data);

    /* Send via pg_notify */
    if (SPI_connect() == SPI_OK_CONNECT) {
        StringInfoData notify_query;

        initStringInfo(&notify_query);
        appendStringInfo(&notify_query, "SELECT pg_notify('auth_webhook', %s)",
                         quote_literal_cstr(notify_payload.data));

        SPI_execute(notify_query.data, false, 0);
        pfree(notify_query.data);
        SPI_finish();
    }

    pfree(notify_payload.data);
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_sign_in_anonymous_sql);
PG_FUNCTION_INFO_V1(orochi_auth_link_email_sql);
PG_FUNCTION_INFO_V1(orochi_auth_link_phone_sql);
PG_FUNCTION_INFO_V1(orochi_auth_is_anonymous_sql);

Datum orochi_auth_sign_in_anonymous_sql(PG_FUNCTION_ARGS)
{
    OrochiAuthResult *result;
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    result = orochi_auth_sign_in_anonymous();

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

Datum orochi_auth_link_email_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_uuid = PG_GETARG_UUID_P(0);
    text *email_text = PG_GETARG_TEXT_PP(1);
    text *password_text = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);

    char *email = text_to_cstring(email_text);
    char *password = password_text ? text_to_cstring(password_text) : NULL;

    OrochiAuthResult *result = orochi_auth_link_email(*user_uuid, email, password);

    if (result->success)
        PG_RETURN_JSONB_P(DirectFunctionCall1(
            jsonb_in, CStringGetDatum("{\"success\": true, \"message\": \"Email "
                                      "linked successfully\"}")));
    else {
        StringInfoData error_json;
        initStringInfo(&error_json);
        appendStringInfo(&error_json,
                         "{\"success\": false, \"error\": \"%s\", \"message\": \"%s\"}",
                         result->error_code ? result->error_code : "unknown",
                         result->error_message ? result->error_message : "Unknown error");

        PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(error_json.data)));
    }
}

Datum orochi_auth_link_phone_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_uuid = PG_GETARG_UUID_P(0);
    text *phone_text = PG_GETARG_TEXT_PP(1);

    char *phone = text_to_cstring(phone_text);

    OrochiAuthResult *result = orochi_auth_link_phone(*user_uuid, phone);

    if (result->success)
        PG_RETURN_JSONB_P(DirectFunctionCall1(
            jsonb_in, CStringGetDatum("{\"success\": true, \"message\": \"Phone "
                                      "linked successfully\"}")));
    else {
        StringInfoData error_json;
        initStringInfo(&error_json);
        appendStringInfo(&error_json,
                         "{\"success\": false, \"error\": \"%s\", \"message\": \"%s\"}",
                         result->error_code ? result->error_code : "unknown",
                         result->error_message ? result->error_message : "Unknown error");

        PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in, CStringGetDatum(error_json.data)));
    }
}

Datum orochi_auth_is_anonymous_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_uuid = PG_GETARG_UUID_P(0);
    bool is_anon = orochi_auth_is_anonymous(*user_uuid);
    PG_RETURN_BOOL(is_anon);
}
