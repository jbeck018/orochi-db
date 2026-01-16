/*-------------------------------------------------------------------------
 *
 * magic_link.c
 *    Orochi DB Magic Link and OTP token handling
 *
 * This module provides passwordless authentication including:
 *   - Magic link email generation and verification
 *   - Phone OTP (SMS) generation and verification
 *   - Password recovery token handling
 *   - Email/phone change confirmation tokens
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <openssl/sha.h>
#include <string.h>

#include "gotrue.h"

/* Forward declarations */
static bool rate_limit_check(pg_uuid_t user_id, OrochiAuthTokenType token_type,
                              int limit_per_hour);
static void queue_email_notification(const char *email, const char *template_name,
                                      Jsonb *template_data);
static void queue_sms_notification(const char *phone, const char *message,
                                    const char *channel);

/* ============================================================
 * Magic Link Token Operations
 * ============================================================ */

/*
 * orochi_auth_send_magic_link
 *    Generate and send a magic link to the user's email
 */
OrochiMagicLink *
orochi_auth_send_magic_link(const char *email, const char *redirect_to)
{
    OrochiMagicLink *magic_link;
    OrochiAuthUser *user;
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;
    char *token;
    char *token_hash;
    char *site_url;
    char *link_url;
    Jsonb *template_data;
    StringInfoData template_json;
    MemoryContext magic_link_context;
    MemoryContext old_context;

    if (email == NULL || strlen(email) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("email is required for magic link")));

    /* Get auth config */
    config = orochi_auth_get_config();
    site_url = config->site_url ? config->site_url : "http://localhost:3000";

    /* Find or create user */
    user = orochi_auth_get_user_by_email(email);

    if (user == NULL)
    {
        /* Check if signup is disabled */
        if (config->disable_signup)
        {
            /* Return success to prevent email enumeration */
            elog(NOTICE, "Magic link requested for non-existent email (signup disabled)");
            return NULL;
        }

        /* Create new user without password */
        user = orochi_auth_create_user(email, NULL, NULL, NULL);
    }

    /* Check rate limiting */
    if (!rate_limit_check(user->id, OROCHI_TOKEN_CONFIRMATION,
                          config->rate_limit_email_sent))
    {
        orochi_auth_free_user(user);
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("too many magic link requests, please try again later")));
    }

    /* Create memory context for magic link */
    magic_link_context = AllocSetContextCreate(TopMemoryContext,
                                                "MagicLink",
                                                ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(magic_link_context);

    magic_link = palloc0(sizeof(OrochiMagicLink));

    /* Generate secure token */
    token = orochi_auth_generate_token(OROCHI_AUTH_TOKEN_LENGTH);
    token_hash = orochi_auth_hash_token(token);

    magic_link->token_type = OROCHI_TOKEN_CONFIRMATION;
    magic_link->token_hash = pstrdup(token_hash);
    magic_link->relates_to = pstrdup(email);
    magic_link->redirect_to = redirect_to ? pstrdup(redirect_to) : pstrdup(site_url);
    memcpy(&magic_link->user_id, &user->id, sizeof(pg_uuid_t));

    MemoryContextSwitchTo(old_context);

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        orochi_auth_free_user(user);
        MemoryContextDelete(magic_link_context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));
    }

    /* Delete existing token for this user/type */
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
            "DELETE FROM auth.one_time_tokens WHERE user_id = '%s' "
            "AND token_type = 'confirmation_token'",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Insert new token */
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
            "INSERT INTO auth.one_time_tokens "
            "(user_id, token_type, token_hash, relates_to, created_at, expires_at) "
            "VALUES ('%s', 'confirmation_token', %s, %s, NOW(), NOW() + INTERVAL '%d seconds') "
            "RETURNING id, created_at, expires_at",
            uuid_str,
            quote_literal_cstr(token_hash),
            quote_literal_cstr(email),
            OROCHI_AUTH_MAGIC_LINK_EXP);
    }

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        char *id_str = SPI_getvalue(SPI_tuptable->vals[0],
                                     SPI_tuptable->tupdesc, 1);
        bool isnull;

        if (id_str)
        {
            Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&magic_link->id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
        }

        magic_link->created_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        magic_link->expires_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull));
    }

    pfree(query.data);

    /* Build magic link URL */
    link_url = psprintf("%s/auth/v1/verify?token=%s&type=magiclink&redirect_to=%s",
                        site_url, token,
                        redirect_to ? redirect_to : site_url);

    /* Build template data for email */
    initStringInfo(&template_json);
    appendStringInfo(&template_json,
        "{\"magic_link\": \"%s\", \"token\": \"%s\", \"email\": \"%s\", "
        "\"redirect_to\": \"%s\", \"expires_in\": %d}",
        link_url, token, email,
        redirect_to ? redirect_to : site_url,
        OROCHI_AUTH_MAGIC_LINK_EXP);

    template_data = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
                                    CStringGetDatum(template_json.data)));
    pfree(template_json.data);

    /* Queue email notification using pg_notify */
    queue_email_notification(email, "magic_link", template_data);

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
            "VALUES ('{\"action\": \"magic_link_sent\", \"actor_id\": \"%s\", "
            "\"log_type\": \"user\"}'::jsonb, NOW())",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    pfree(token);
    pfree(token_hash);
    pfree(link_url);
    orochi_auth_free_user(user);

    elog(LOG, "Magic link sent to: %s", email);

    return magic_link;
}

/*
 * orochi_auth_send_phone_otp
 *    Generate and send an OTP to the user's phone
 */
OrochiMagicLink *
orochi_auth_send_phone_otp(const char *phone, const char *channel)
{
    OrochiMagicLink *otp_token;
    OrochiAuthUser *user;
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;
    char *otp;
    char *otp_hash;
    char *message;
    MemoryContext otp_context;
    MemoryContext old_context;

    if (phone == NULL || strlen(phone) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("phone number is required for OTP")));

    /* Default channel to SMS */
    if (channel == NULL)
        channel = "sms";

    /* Get auth config */
    config = orochi_auth_get_config();

    /* Find or create user */
    user = orochi_auth_get_user_by_phone(phone);

    if (user == NULL)
    {
        if (config->disable_signup)
        {
            elog(NOTICE, "OTP requested for non-existent phone (signup disabled)");
            return NULL;
        }

        user = orochi_auth_create_user(NULL, phone, NULL, NULL);
    }

    /* Check rate limiting */
    if (!rate_limit_check(user->id, OROCHI_TOKEN_CONFIRMATION,
                          config->rate_limit_sms_sent))
    {
        orochi_auth_free_user(user);
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("too many OTP requests, please try again later")));
    }

    /* Create memory context */
    otp_context = AllocSetContextCreate(TopMemoryContext,
                                         "PhoneOTP",
                                         ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(otp_context);

    otp_token = palloc0(sizeof(OrochiMagicLink));

    /* Generate 6-digit OTP */
    otp = orochi_auth_generate_otp(OROCHI_AUTH_OTP_LENGTH);
    otp_hash = orochi_auth_hash_token(otp);

    otp_token->token_type = OROCHI_TOKEN_CONFIRMATION;
    otp_token->token_hash = pstrdup(otp_hash);
    otp_token->relates_to = pstrdup(phone);
    memcpy(&otp_token->user_id, &user->id, sizeof(pg_uuid_t));

    MemoryContextSwitchTo(old_context);

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        orochi_auth_free_user(user);
        MemoryContextDelete(otp_context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));
    }

    /* Delete existing token */
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
            "DELETE FROM auth.one_time_tokens WHERE user_id = '%s' "
            "AND token_type = 'confirmation_token'",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Insert new token */
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
            "INSERT INTO auth.one_time_tokens "
            "(user_id, token_type, token_hash, relates_to, created_at, expires_at) "
            "VALUES ('%s', 'confirmation_token', %s, %s, NOW(), NOW() + INTERVAL '%d seconds') "
            "RETURNING id, created_at, expires_at",
            uuid_str,
            quote_literal_cstr(otp_hash),
            quote_literal_cstr(phone),
            OROCHI_AUTH_OTP_EXP);
    }

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        char *id_str = SPI_getvalue(SPI_tuptable->vals[0],
                                     SPI_tuptable->tupdesc, 1);
        bool isnull;

        if (id_str)
        {
            Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(id_str));
            memcpy(&otp_token->id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
        }

        otp_token->created_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        otp_token->expires_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull));
    }

    pfree(query.data);

    /* Build SMS message */
    message = psprintf("Your verification code is: %s. Valid for %d minutes.",
                       otp, OROCHI_AUTH_OTP_EXP / 60);

    /* Queue SMS notification */
    queue_sms_notification(phone, message, channel);

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
            "VALUES ('{\"action\": \"phone_otp_sent\", \"actor_id\": \"%s\", "
            "\"log_type\": \"user\"}'::jsonb, NOW())",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    pfree(otp);
    pfree(otp_hash);
    pfree(message);
    orochi_auth_free_user(user);

    elog(LOG, "OTP sent to phone: %s via %s", phone, channel);

    return otp_token;
}

/*
 * orochi_auth_verify_otp
 *    Verify a magic link or OTP token
 */
OrochiAuthResult *
orochi_auth_verify_otp(const char *token,
                        const char *token_type,
                        const char *email,
                        const char *phone)
{
    OrochiAuthResult *result;
    StringInfoData query;
    int ret;
    char *token_hash;
    pg_uuid_t user_id;
    OrochiAuthUser *user;
    OrochiAuthSession *session;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];
    const char *db_token_type;

    result = palloc0(sizeof(OrochiAuthResult));
    result->success = false;

    if (token == NULL || strlen(token) == 0)
    {
        result->error_code = "invalid_token";
        result->error_message = "Token is required";
        return result;
    }

    /* Map token type to database enum */
    if (strcmp(token_type, "magiclink") == 0 || strcmp(token_type, "signup") == 0 ||
        strcmp(token_type, "sms") == 0)
    {
        db_token_type = "confirmation_token";
    }
    else if (strcmp(token_type, "recovery") == 0)
    {
        db_token_type = "recovery_token";
    }
    else if (strcmp(token_type, "phone_change") == 0)
    {
        db_token_type = "phone_change_token";
    }
    else if (strcmp(token_type, "email_change") == 0)
    {
        db_token_type = "email_change_token_new";
    }
    else
    {
        db_token_type = "confirmation_token";
    }

    /* Hash the provided token */
    token_hash = orochi_auth_hash_token(token);

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        pfree(token_hash);
        result->error_code = "internal_error";
        result->error_message = "Failed to connect to database";
        return result;
    }

    /* Find and validate token */
    initStringInfo(&query);
    appendStringInfoString(&query,
        "SELECT user_id, relates_to FROM auth.one_time_tokens "
        "WHERE token_hash = $1 "
        "AND expires_at > NOW() "
        "AND used_at IS NULL");

    argvals[0] = CStringGetTextDatum(token_hash);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        pfree(query.data);
        pfree(token_hash);
        SPI_finish();
        result->error_code = "invalid_token";
        result->error_message = "Invalid or expired token";
        return result;
    }

    /* Extract user_id */
    {
        char *user_id_str = SPI_getvalue(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1);
        char *relates_to = SPI_getvalue(SPI_tuptable->vals[0],
                                         SPI_tuptable->tupdesc, 2);
        Datum uuid_datum;

        /* Verify email/phone matches if provided */
        if (email != NULL && relates_to != NULL && strcmp(email, relates_to) != 0)
        {
            pfree(query.data);
            pfree(token_hash);
            SPI_finish();
            result->error_code = "token_mismatch";
            result->error_message = "Token does not match email";
            return result;
        }

        if (phone != NULL && relates_to != NULL && strcmp(phone, relates_to) != 0)
        {
            pfree(query.data);
            pfree(token_hash);
            SPI_finish();
            result->error_code = "token_mismatch";
            result->error_message = "Token does not match phone";
            return result;
        }

        uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(user_id_str));
        memcpy(&user_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
    }

    pfree(query.data);

    /* Mark token as used */
    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE auth.one_time_tokens SET used_at = NOW(), updated_at = NOW() "
        "WHERE token_hash = %s AND used_at IS NULL",
        quote_literal_cstr(token_hash));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Update user confirmation status based on token type */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3],
                 user_id.data[4], user_id.data[5], user_id.data[6], user_id.data[7],
                 user_id.data[8], user_id.data[9], user_id.data[10], user_id.data[11],
                 user_id.data[12], user_id.data[13], user_id.data[14], user_id.data[15]);

        if (strcmp(token_type, "magiclink") == 0 || strcmp(token_type, "signup") == 0)
        {
            appendStringInfo(&query,
                "UPDATE auth.users SET email_confirmed_at = COALESCE(email_confirmed_at, NOW()), "
                "last_sign_in_at = NOW(), updated_at = NOW() WHERE id = '%s'",
                uuid_str);
        }
        else if (strcmp(token_type, "sms") == 0)
        {
            appendStringInfo(&query,
                "UPDATE auth.users SET phone_confirmed_at = COALESCE(phone_confirmed_at, NOW()), "
                "last_sign_in_at = NOW(), updated_at = NOW() WHERE id = '%s'",
                uuid_str);
        }
        else
        {
            appendStringInfo(&query,
                "UPDATE auth.users SET last_sign_in_at = NOW(), updated_at = NOW() "
                "WHERE id = '%s'",
                uuid_str);
        }
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    pfree(token_hash);

    /* Get user and create session */
    user = orochi_auth_get_user(user_id);
    if (user == NULL)
    {
        result->error_code = "user_not_found";
        result->error_message = "User not found";
        return result;
    }

    /* Create session */
    session = orochi_auth_create_session(user_id, NULL, NULL, OROCHI_AAL_1);

    result->user = user;
    result->session = session;
    result->refresh_token = pstrdup(session->refresh_token);
    result->access_token = orochi_auth_generate_jwt(user, session, orochi_auth_jwt_exp);
    result->expires_at = GetCurrentTimestamp() + (orochi_auth_jwt_exp * USECS_PER_SEC);
    result->success = true;

    elog(LOG, "OTP verified for user");

    return result;
}

/*
 * orochi_auth_send_recovery
 *    Send password recovery email
 */
OrochiMagicLink *
orochi_auth_send_recovery(const char *email)
{
    OrochiMagicLink *recovery_token;
    OrochiAuthUser *user;
    OrochiAuthConfig *config;
    StringInfoData query;
    int ret;
    char *token;
    char *token_hash;
    char *site_url;
    char *link_url;
    Jsonb *template_data;
    StringInfoData template_json;
    MemoryContext recovery_context;
    MemoryContext old_context;

    if (email == NULL || strlen(email) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("email is required for password recovery")));

    config = orochi_auth_get_config();
    site_url = config->site_url ? config->site_url : "http://localhost:3000";

    /* Find user */
    user = orochi_auth_get_user_by_email(email);

    if (user == NULL)
    {
        /* Return success to prevent email enumeration */
        elog(NOTICE, "Recovery requested for non-existent email");
        return NULL;
    }

    /* Check rate limiting */
    if (!rate_limit_check(user->id, OROCHI_TOKEN_RECOVERY,
                          config->rate_limit_email_sent))
    {
        orochi_auth_free_user(user);
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("too many recovery requests, please try again later")));
    }

    /* Create memory context */
    recovery_context = AllocSetContextCreate(TopMemoryContext,
                                              "RecoveryToken",
                                              ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(recovery_context);

    recovery_token = palloc0(sizeof(OrochiMagicLink));

    /* Generate token */
    token = orochi_auth_generate_token(OROCHI_AUTH_TOKEN_LENGTH);
    token_hash = orochi_auth_hash_token(token);

    recovery_token->token_type = OROCHI_TOKEN_RECOVERY;
    recovery_token->token_hash = pstrdup(token_hash);
    recovery_token->relates_to = pstrdup(email);
    memcpy(&recovery_token->user_id, &user->id, sizeof(pg_uuid_t));

    MemoryContextSwitchTo(old_context);

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        orochi_auth_free_user(user);
        MemoryContextDelete(recovery_context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));
    }

    /* Store recovery token on user record */
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
            "UPDATE auth.users SET recovery_token = %s, recovery_sent_at = NOW(), "
            "updated_at = NOW() WHERE id = '%s'",
            quote_literal_cstr(token_hash),
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Also store in one_time_tokens table */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user->id.data[0], user->id.data[1], user->id.data[2], user->id.data[3],
                 user->id.data[4], user->id.data[5], user->id.data[6], user->id.data[7],
                 user->id.data[8], user->id.data[9], user->id.data[10], user->id.data[11],
                 user->id.data[12], user->id.data[13], user->id.data[14], user->id.data[15]);

        /* Delete existing recovery token */
        appendStringInfo(&query,
            "DELETE FROM auth.one_time_tokens WHERE user_id = '%s' "
            "AND token_type = 'recovery_token'",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

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
            "INSERT INTO auth.one_time_tokens "
            "(user_id, token_type, token_hash, relates_to, created_at, expires_at) "
            "VALUES ('%s', 'recovery_token', %s, %s, NOW(), NOW() + INTERVAL '%d seconds') "
            "RETURNING id, created_at, expires_at",
            uuid_str,
            quote_literal_cstr(token_hash),
            quote_literal_cstr(email),
            OROCHI_AUTH_MAGIC_LINK_EXP);
    }

    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        recovery_token->created_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        recovery_token->expires_at = DatumGetTimestampTz(SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull));
    }

    pfree(query.data);

    /* Build recovery link URL */
    link_url = psprintf("%s/auth/v1/verify?token=%s&type=recovery",
                        site_url, token);

    /* Build template data */
    initStringInfo(&template_json);
    appendStringInfo(&template_json,
        "{\"recovery_link\": \"%s\", \"token\": \"%s\", \"email\": \"%s\", "
        "\"expires_in\": %d}",
        link_url, token, email, OROCHI_AUTH_MAGIC_LINK_EXP);

    template_data = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
                                    CStringGetDatum(template_json.data)));
    pfree(template_json.data);

    /* Queue email */
    queue_email_notification(email, "recovery", template_data);

    SPI_finish();

    pfree(token);
    pfree(token_hash);
    pfree(link_url);
    orochi_auth_free_user(user);

    elog(LOG, "Recovery email sent to: %s", email);

    return recovery_token;
}

/*
 * orochi_auth_verify_recovery
 *    Verify a recovery token and reset password
 */
bool
orochi_auth_verify_recovery(const char *token, const char *new_password)
{
    StringInfoData query;
    int ret;
    char *token_hash;
    char *encrypted_password;
    pg_uuid_t user_id;
    Oid argtypes[1] = { TEXTOID };
    Datum argvals[1];

    if (token == NULL || new_password == NULL)
        return false;

    if (strlen(new_password) < OROCHI_AUTH_MIN_PASSWORD_LENGTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("password too short (min %d characters)",
                        OROCHI_AUTH_MIN_PASSWORD_LENGTH)));

    token_hash = orochi_auth_hash_token(token);
    encrypted_password = orochi_auth_hash_password(new_password);

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        pfree(token_hash);
        pfree(encrypted_password);
        return false;
    }

    /* Find token */
    initStringInfo(&query);
    appendStringInfoString(&query,
        "SELECT user_id FROM auth.one_time_tokens "
        "WHERE token_hash = $1 AND token_type = 'recovery_token' "
        "AND expires_at > NOW() AND used_at IS NULL");

    argvals[0] = CStringGetTextDatum(token_hash);

    ret = SPI_execute_with_args(query.data, 1, argtypes, argvals, NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        pfree(query.data);
        pfree(token_hash);
        pfree(encrypted_password);
        SPI_finish();
        return false;
    }

    /* Extract user_id */
    {
        char *user_id_str = SPI_getvalue(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1);
        Datum uuid_datum = DirectFunctionCall1(uuid_in, CStringGetDatum(user_id_str));
        memcpy(&user_id, DatumGetUUIDP(uuid_datum), sizeof(pg_uuid_t));
    }

    pfree(query.data);

    /* Mark token as used */
    initStringInfo(&query);
    appendStringInfo(&query,
        "UPDATE auth.one_time_tokens SET used_at = NOW(), updated_at = NOW() "
        "WHERE token_hash = %s",
        quote_literal_cstr(token_hash));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Update user password */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3],
                 user_id.data[4], user_id.data[5], user_id.data[6], user_id.data[7],
                 user_id.data[8], user_id.data[9], user_id.data[10], user_id.data[11],
                 user_id.data[12], user_id.data[13], user_id.data[14], user_id.data[15]);

        appendStringInfo(&query,
            "UPDATE auth.users SET encrypted_password = %s, "
            "recovery_token = NULL, recovery_sent_at = NULL, updated_at = NOW() "
            "WHERE id = '%s'",
            quote_literal_cstr(encrypted_password),
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Revoke all existing sessions for security */
    initStringInfo(&query);
    {
        char uuid_str[37];
        snprintf(uuid_str, sizeof(uuid_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3],
                 user_id.data[4], user_id.data[5], user_id.data[6], user_id.data[7],
                 user_id.data[8], user_id.data[9], user_id.data[10], user_id.data[11],
                 user_id.data[12], user_id.data[13], user_id.data[14], user_id.data[15]);

        appendStringInfo(&query,
            "DELETE FROM auth.sessions WHERE user_id = '%s'",
            uuid_str);
    }
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    pfree(token_hash);
    pfree(encrypted_password);

    elog(LOG, "Password reset completed for user");

    return true;
}

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * rate_limit_check
 *    Check if user is within rate limits
 */
static bool
rate_limit_check(pg_uuid_t user_id, OrochiAuthTokenType token_type, int limit_per_hour)
{
    StringInfoData query;
    int ret;
    int recent_count = 0;
    char uuid_str[37];
    const char *token_type_str;

    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             user_id.data[0], user_id.data[1], user_id.data[2], user_id.data[3],
             user_id.data[4], user_id.data[5], user_id.data[6], user_id.data[7],
             user_id.data[8], user_id.data[9], user_id.data[10], user_id.data[11],
             user_id.data[12], user_id.data[13], user_id.data[14], user_id.data[15]);

    token_type_str = orochi_auth_token_type_name(token_type);

    if (SPI_connect() != SPI_OK_CONNECT)
        return true;  /* Allow on error */

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT COUNT(*) FROM auth.one_time_tokens "
        "WHERE user_id = '%s' "
        "AND created_at > NOW() - INTERVAL '1 hour'",
        uuid_str);

    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        recent_count = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                    SPI_tuptable->tupdesc, 1, &isnull));
    }

    pfree(query.data);
    SPI_finish();

    return recent_count < limit_per_hour;
}

/*
 * queue_email_notification
 *    Queue an email for delivery using pg_notify
 */
static void
queue_email_notification(const char *email, const char *template_name,
                          Jsonb *template_data)
{
    StringInfoData payload;
    StringInfoData template_str;

    initStringInfo(&template_str);
    (void) JsonbToCString(&template_str, &template_data->root, VARSIZE(template_data));

    initStringInfo(&payload);
    appendStringInfo(&payload,
        "{\"type\": \"%s\", \"email\": \"%s\", \"data\": %s}",
        template_name, email, template_str.data);

    /* Use pg_notify to send to email worker */
    {
        StringInfoData notify_query;

        if (SPI_connect() != SPI_OK_CONNECT)
        {
            elog(WARNING, "Failed to queue email notification");
            pfree(payload.data);
            pfree(template_str.data);
            return;
        }

        initStringInfo(&notify_query);
        appendStringInfo(&notify_query,
            "SELECT pg_notify('auth_email', %s)",
            quote_literal_cstr(payload.data));

        SPI_execute(notify_query.data, false, 0);
        pfree(notify_query.data);
        SPI_finish();
    }

    pfree(payload.data);
    pfree(template_str.data);
}

/*
 * queue_sms_notification
 *    Queue an SMS for delivery using pg_notify
 */
static void
queue_sms_notification(const char *phone, const char *message, const char *channel)
{
    StringInfoData payload;

    initStringInfo(&payload);
    appendStringInfo(&payload,
        "{\"phone\": \"%s\", \"message\": \"%s\", \"channel\": \"%s\"}",
        phone, message, channel);

    /* Use pg_notify to send to SMS worker */
    {
        StringInfoData notify_query;

        if (SPI_connect() != SPI_OK_CONNECT)
        {
            elog(WARNING, "Failed to queue SMS notification");
            pfree(payload.data);
            return;
        }

        initStringInfo(&notify_query);
        appendStringInfo(&notify_query,
            "SELECT pg_notify('auth_sms', %s)",
            quote_literal_cstr(payload.data));

        SPI_execute(notify_query.data, false, 0);
        pfree(notify_query.data);
        SPI_finish();
    }

    pfree(payload.data);
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_auth_send_magic_link_sql);
PG_FUNCTION_INFO_V1(orochi_auth_send_phone_otp_sql);
PG_FUNCTION_INFO_V1(orochi_auth_verify_otp_sql);
PG_FUNCTION_INFO_V1(orochi_auth_send_recovery_sql);

Datum
orochi_auth_send_magic_link_sql(PG_FUNCTION_ARGS)
{
    text *email_text = PG_GETARG_TEXT_PP(0);
    text *redirect_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);

    char *email = text_to_cstring(email_text);
    char *redirect_to = redirect_text ? text_to_cstring(redirect_text) : NULL;

    OrochiMagicLink *result = orochi_auth_send_magic_link(email, redirect_to);

    if (result == NULL)
        PG_RETURN_NULL();

    PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"message\": \"Magic link sent\"}")));
}

Datum
orochi_auth_send_phone_otp_sql(PG_FUNCTION_ARGS)
{
    text *phone_text = PG_GETARG_TEXT_PP(0);
    text *channel_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);

    char *phone = text_to_cstring(phone_text);
    char *channel = channel_text ? text_to_cstring(channel_text) : "sms";

    OrochiMagicLink *result = orochi_auth_send_phone_otp(phone, channel);

    if (result == NULL)
        PG_RETURN_NULL();

    PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"message\": \"OTP sent\"}")));
}

Datum
orochi_auth_verify_otp_sql(PG_FUNCTION_ARGS)
{
    text *token_text = PG_GETARG_TEXT_PP(0);
    text *type_text = PG_GETARG_TEXT_PP(1);
    text *email_text = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);
    text *phone_text = PG_ARGISNULL(3) ? NULL : PG_GETARG_TEXT_PP(3);

    char *token = text_to_cstring(token_text);
    char *type = text_to_cstring(type_text);
    char *email = email_text ? text_to_cstring(email_text) : NULL;
    char *phone = phone_text ? text_to_cstring(phone_text) : NULL;

    OrochiAuthResult *result;
    TupleDesc tupdesc;
    Datum values[4];
    bool nulls[4];
    HeapTuple tuple;

    result = orochi_auth_verify_otp(token, type, email, phone);

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context "
                        "that cannot accept type record")));

    memset(nulls, 0, sizeof(nulls));

    if (result->success)
    {
        values[0] = BoolGetDatum(true);
        values[1] = CStringGetTextDatum(result->access_token);
        values[2] = CStringGetTextDatum(result->refresh_token);
        values[3] = TimestampTzGetDatum(result->expires_at);
    }
    else
    {
        values[0] = BoolGetDatum(false);
        nulls[1] = true;
        nulls[2] = true;
        nulls[3] = true;
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
orochi_auth_send_recovery_sql(PG_FUNCTION_ARGS)
{
    text *email_text = PG_GETARG_TEXT_PP(0);
    char *email = text_to_cstring(email_text);

    OrochiMagicLink *result = orochi_auth_send_recovery(email);

    /* Always return success to prevent email enumeration */
    PG_RETURN_JSONB_P(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"message\": \"If the email exists, a recovery link has been sent\"}")));
}
