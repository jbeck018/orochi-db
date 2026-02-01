/*-------------------------------------------------------------------------
 *
 * jwt_claims.c
 *    JWT custom claims handling for Orochi DB
 *
 * This module provides:
 *   - Custom claims extraction and manipulation
 *   - Claims builder/factory functions
 *   - Claims validation helpers
 *   - Integration with auth.custom_claims_config and auth.user_claims tables
 *   - Pre-token generation hook support
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "jwt.h"

#include <string.h>
#include <time.h>

/* SQL function declarations */
PG_FUNCTION_INFO_V1(orochi_jwt_build_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_merge_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_add_claim);
PG_FUNCTION_INFO_V1(orochi_jwt_remove_claim);
PG_FUNCTION_INFO_V1(orochi_jwt_validate_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_get_custom_claims);
PG_FUNCTION_INFO_V1(orochi_jwt_extract_claim_path);

/* ============================================================
 * Constants
 * ============================================================ */

/* Standard claim names that should not be overwritten by custom claims */
static const char *reserved_claims[] = { "iss", "sub", "aud", "exp", "nbf", "iat", "jti", NULL };

/* Maximum custom claims per token */
#define MAX_CUSTOM_CLAIMS 64

/* Maximum claim name length */
#define MAX_CLAIM_NAME_LENGTH 128

/* Maximum claim value size (64KB) */
#define MAX_CLAIM_VALUE_SIZE (64 * 1024)

/* ============================================================
 * Custom Claims Configuration Structure
 * ============================================================ */

typedef enum ClaimSourceType {
    CLAIM_SOURCE_USER_METADATA = 0,
    CLAIM_SOURCE_APP_METADATA,
    CLAIM_SOURCE_SQL_FUNCTION,
    CLAIM_SOURCE_STATIC
} ClaimSourceType;

typedef struct CustomClaimConfig {
    char *claim_name;
    ClaimSourceType source_type;
    char *source_path;
    char *source_function;
    Jsonb *static_value;
    bool include_in_access_token;
    bool include_in_id_token;
    bool required;
    bool enabled;
} CustomClaimConfig;

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * Check if a claim name is reserved
 */
static bool is_reserved_claim(const char *claim_name)
{
    const char **reserved;

    for (reserved = reserved_claims; *reserved != NULL; reserved++) {
        if (strcmp(claim_name, *reserved) == 0)
            return true;
    }

    return false;
}

/*
 * Validate claim name format
 */
static bool validate_claim_name(const char *claim_name)
{
    size_t len;
    const char *p;

    if (claim_name == NULL || claim_name[0] == '\0')
        return false;

    len = strlen(claim_name);
    if (len > MAX_CLAIM_NAME_LENGTH)
        return false;

    /* Claim name should be alphanumeric with underscores */
    for (p = claim_name; *p != '\0'; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') &&
            *p != '_' && *p != '-' && *p != '.') {
            return false;
        }
    }

    return true;
}

/*
 * Extract value from JSONB using dot-notation path
 */
static Jsonb *extract_jsonb_path(Jsonb *jsonb, const char *path)
{
    char *path_copy;
    char *token;
    char *saveptr;
    JsonbValue *current;
    JsonbContainer *container;

    if (jsonb == NULL || path == NULL || path[0] == '\0')
        return NULL;

    path_copy = pstrdup(path);
    container = &jsonb->root;
    current = NULL;

    /* Split path by '.' and traverse */
    token = strtok_r(path_copy, ".", &saveptr);
    while (token != NULL) {
        current = getKeyJsonValueFromContainer(container, token, strlen(token), NULL);

        if (current == NULL) {
            pfree(path_copy);
            return NULL;
        }

        /* If this is the last token, return the value */
        token = strtok_r(NULL, ".", &saveptr);
        if (token == NULL)
            break;

        /* Otherwise, the value must be an object to continue traversing */
        if (current->type != jbvBinary) {
            pfree(path_copy);
            return NULL;
        }

        container = current->val.binary.data;
    }

    pfree(path_copy);

    if (current == NULL)
        return NULL;

    return JsonbValueToJsonb(current);
}

/*
 * Convert ClaimSourceType string to enum
 */
static ClaimSourceType source_type_from_string(const char *str)
{
    if (str == NULL)
        return CLAIM_SOURCE_STATIC;

    if (strcmp(str, "user_metadata") == 0)
        return CLAIM_SOURCE_USER_METADATA;
    if (strcmp(str, "app_metadata") == 0)
        return CLAIM_SOURCE_APP_METADATA;
    if (strcmp(str, "sql_function") == 0)
        return CLAIM_SOURCE_SQL_FUNCTION;
    if (strcmp(str, "static") == 0)
        return CLAIM_SOURCE_STATIC;

    return CLAIM_SOURCE_STATIC;
}

/* ============================================================
 * Claims Builder Functions
 * ============================================================ */

/*
 * Create a new claims builder (empty JSONB object)
 */
static Jsonb *create_empty_claims(void)
{
    return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));
}

/*
 * Add a string claim to JSONB
 */
static Jsonb *add_string_claim(Jsonb *claims, const char *name, const char *value)
{
    JsonbParseState *state = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    Jsonb *result;

    if (claims == NULL)
        claims = create_empty_claims();

    if (name == NULL || value == NULL)
        return claims;

    /* Start building new object */
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    /* Copy existing key-value pairs (except the one we're replacing) */
    it = JsonbIteratorInit(&claims->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            bool skip = (strcmp(key, name) == 0);
            JsonbValue key_val = v;

            r = JsonbIteratorNext(&it, &v, true);

            if (!skip) {
                pushJsonbValue(&state, WJB_KEY, &key_val);
                pushJsonbValue(&state, WJB_VALUE, &v);
            }

            pfree(key);
        }
    }

    /* Add new claim */
    v.type = jbvString;
    v.val.string.len = strlen(name);
    v.val.string.val = (char *)name;
    pushJsonbValue(&state, WJB_KEY, &v);

    v.val.string.len = strlen(value);
    v.val.string.val = (char *)value;
    pushJsonbValue(&state, WJB_VALUE, &v);

    result = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));

    return result;
}

/*
 * Add a numeric claim to JSONB
 */
static Jsonb *add_numeric_claim(Jsonb *claims, const char *name, int64 value)
{
    JsonbParseState *state = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    Jsonb *result;
    char value_str[32];

    if (claims == NULL)
        claims = create_empty_claims();

    if (name == NULL)
        return claims;

    /* Start building new object */
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    /* Copy existing key-value pairs (except the one we're replacing) */
    it = JsonbIteratorInit(&claims->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            bool skip = (strcmp(key, name) == 0);
            JsonbValue key_val = v;

            r = JsonbIteratorNext(&it, &v, true);

            if (!skip) {
                pushJsonbValue(&state, WJB_KEY, &key_val);
                pushJsonbValue(&state, WJB_VALUE, &v);
            }

            pfree(key);
        }
    }

    /* Add new claim */
    v.type = jbvString;
    v.val.string.len = strlen(name);
    v.val.string.val = (char *)name;
    pushJsonbValue(&state, WJB_KEY, &v);

    snprintf(value_str, sizeof(value_str), "%ld", (long)value);
    v.type = jbvNumeric;
    v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(value)));
    pushJsonbValue(&state, WJB_VALUE, &v);

    result = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));

    return result;
}

/*
 * Add a boolean claim to JSONB
 */
static Jsonb *add_bool_claim(Jsonb *claims, const char *name, bool value)
{
    JsonbParseState *state = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    Jsonb *result;

    if (claims == NULL)
        claims = create_empty_claims();

    if (name == NULL)
        return claims;

    /* Start building new object */
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    /* Copy existing key-value pairs (except the one we're replacing) */
    it = JsonbIteratorInit(&claims->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            bool skip = (strcmp(key, name) == 0);
            JsonbValue key_val = v;

            r = JsonbIteratorNext(&it, &v, true);

            if (!skip) {
                pushJsonbValue(&state, WJB_KEY, &key_val);
                pushJsonbValue(&state, WJB_VALUE, &v);
            }

            pfree(key);
        }
    }

    /* Add new claim */
    v.type = jbvString;
    v.val.string.len = strlen(name);
    v.val.string.val = (char *)name;
    pushJsonbValue(&state, WJB_KEY, &v);

    v.type = jbvBool;
    v.val.boolean = value;
    pushJsonbValue(&state, WJB_VALUE, &v);

    result = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));

    return result;
}

/*
 * Add a JSONB claim to JSONB
 */
static Jsonb *add_jsonb_claim(Jsonb *claims, const char *name, Jsonb *value)
{
    JsonbParseState *state = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    Jsonb *result;

    if (claims == NULL)
        claims = create_empty_claims();

    if (name == NULL || value == NULL)
        return claims;

    /* Start building new object */
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    /* Copy existing key-value pairs (except the one we're replacing) */
    it = JsonbIteratorInit(&claims->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            bool skip = (strcmp(key, name) == 0);
            JsonbValue key_val = v;

            r = JsonbIteratorNext(&it, &v, true);

            if (!skip) {
                pushJsonbValue(&state, WJB_KEY, &key_val);
                pushJsonbValue(&state, WJB_VALUE, &v);
            }

            pfree(key);
        }
    }

    /* Add new claim */
    v.type = jbvString;
    v.val.string.len = strlen(name);
    v.val.string.val = (char *)name;
    pushJsonbValue(&state, WJB_KEY, &v);

    v.type = jbvBinary;
    v.val.binary.data = &value->root;
    v.val.binary.len = VARSIZE(value) - VARHDRSZ;
    pushJsonbValue(&state, WJB_VALUE, &v);

    result = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));

    return result;
}

/*
 * Remove a claim from JSONB
 */
static Jsonb *remove_claim(Jsonb *claims, const char *name)
{
    JsonbParseState *state = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    Jsonb *result;

    if (claims == NULL || name == NULL)
        return claims;

    /* Start building new object */
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    /* Copy all key-value pairs except the one to remove */
    it = JsonbIteratorInit(&claims->root);
    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            bool skip = (strcmp(key, name) == 0);
            JsonbValue key_val = v;

            r = JsonbIteratorNext(&it, &v, true);

            if (!skip) {
                pushJsonbValue(&state, WJB_KEY, &key_val);
                pushJsonbValue(&state, WJB_VALUE, &v);
            }

            pfree(key);
        }
    }

    result = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));

    return result;
}

/*
 * Merge two JSONB objects (second overwrites first for duplicate keys)
 */
static Jsonb *merge_jsonb(Jsonb *base, Jsonb *overlay)
{
    Datum result;

    if (base == NULL)
        return overlay;
    if (overlay == NULL)
        return base;

    /* Use PostgreSQL's jsonb concatenation operator */
    result = DirectFunctionCall2(jsonb_concat, JsonbPGetDatum(base), JsonbPGetDatum(overlay));

    return DatumGetJsonbP(result);
}

/* ============================================================
 * Custom Claims Resolution
 * ============================================================ */

/*
 * Load custom claims configuration from database
 */
static List *load_custom_claims_config(void)
{
    List *configs = NIL;
    int ret;
    uint64 proc;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        return NIL;

    ret = SPI_execute("SELECT claim_name, source_type, source_path, source_function, "
                      "       static_value, include_in_access_token, include_in_id_token, "
                      "       required, enabled "
                      "FROM auth.custom_claims_config "
                      "WHERE enabled = TRUE",
                      true, 0);

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        return NIL;
    }

    proc = SPI_processed;

    for (uint64 i = 0; i < proc; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        CustomClaimConfig *config = palloc0(sizeof(CustomClaimConfig));
        bool isnull;
        Datum datum;

        /* claim_name */
        datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (!isnull)
            config->claim_name = TextDatumGetCString(datum);

        /* source_type */
        datum = SPI_getbinval(tuple, tupdesc, 2, &isnull);
        if (!isnull)
            config->source_type = source_type_from_string(TextDatumGetCString(datum));

        /* source_path */
        datum = SPI_getbinval(tuple, tupdesc, 3, &isnull);
        if (!isnull)
            config->source_path = TextDatumGetCString(datum);

        /* source_function */
        datum = SPI_getbinval(tuple, tupdesc, 4, &isnull);
        if (!isnull)
            config->source_function = TextDatumGetCString(datum);

        /* static_value */
        datum = SPI_getbinval(tuple, tupdesc, 5, &isnull);
        if (!isnull)
            config->static_value = DatumGetJsonbPCopy(datum);

        /* include_in_access_token */
        datum = SPI_getbinval(tuple, tupdesc, 6, &isnull);
        config->include_in_access_token = isnull ? true : DatumGetBool(datum);

        /* include_in_id_token */
        datum = SPI_getbinval(tuple, tupdesc, 7, &isnull);
        config->include_in_id_token = isnull ? true : DatumGetBool(datum);

        /* required */
        datum = SPI_getbinval(tuple, tupdesc, 8, &isnull);
        config->required = isnull ? false : DatumGetBool(datum);

        /* enabled */
        config->enabled = true;

        configs = lappend(configs, config);
    }

    SPI_finish();
    return configs;
}

/*
 * Load user-specific claims from database
 */
static Jsonb *load_user_claims(const char *user_id)
{
    Jsonb *claims = create_empty_claims();
    int ret;
    uint64 proc;
    char *query;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        return claims;

    query = psprintf("SELECT claim_name, claim_value "
                     "FROM auth.user_claims "
                     "WHERE user_id = '%s'::uuid",
                     user_id);

    ret = SPI_execute(query, true, 0);
    pfree(query);

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        return claims;
    }

    proc = SPI_processed;

    for (uint64 i = 0; i < proc; i++) {
        HeapTuple tuple = SPI_tuptable->vals[i];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;
        Datum datum;
        char *claim_name;
        Jsonb *claim_value;

        datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (isnull)
            continue;
        claim_name = TextDatumGetCString(datum);

        datum = SPI_getbinval(tuple, tupdesc, 2, &isnull);
        if (isnull)
            continue;
        claim_value = DatumGetJsonbPCopy(datum);

        claims = add_jsonb_claim(claims, claim_name, claim_value);
    }

    SPI_finish();
    return claims;
}

/*
 * Execute SQL function to get claim value
 */
static Jsonb *execute_claim_function(const char *function_name, const char *user_id)
{
    int ret;
    char *query;
    Datum result;
    bool isnull;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        return NULL;

    /* Execute function with user_id parameter */
    query = psprintf("SELECT %s('%s'::uuid)::jsonb", function_name, user_id);
    ret = SPI_execute(query, true, 1);
    pfree(query);

    if (ret != SPI_OK_SELECT || SPI_processed == 0) {
        SPI_finish();
        return NULL;
    }

    result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

    if (isnull) {
        SPI_finish();
        return NULL;
    }

    result = SPI_datumTransfer(result, false, -1);
    SPI_finish();

    return DatumGetJsonbP(result);
}

/*
 * Resolve a single custom claim based on configuration
 */
static Jsonb *resolve_custom_claim(CustomClaimConfig *config, Jsonb *user_metadata,
                                   Jsonb *app_metadata, const char *user_id)
{
    Jsonb *value = NULL;

    switch (config->source_type) {
    case CLAIM_SOURCE_USER_METADATA:
        if (user_metadata != NULL && config->source_path != NULL)
            value = extract_jsonb_path(user_metadata, config->source_path);
        break;

    case CLAIM_SOURCE_APP_METADATA:
        if (app_metadata != NULL && config->source_path != NULL)
            value = extract_jsonb_path(app_metadata, config->source_path);
        break;

    case CLAIM_SOURCE_SQL_FUNCTION:
        if (config->source_function != NULL)
            value = execute_claim_function(config->source_function, user_id);
        break;

    case CLAIM_SOURCE_STATIC:
        value = config->static_value;
        break;
    }

    return value;
}

/*
 * Build custom claims for a user
 */
static Jsonb *build_custom_claims(const char *user_id, Jsonb *user_metadata, Jsonb *app_metadata,
                                  bool for_access_token)
{
    List *configs;
    ListCell *lc;
    Jsonb *custom_claims;
    Jsonb *user_claims;

    /* Start with empty claims */
    custom_claims = create_empty_claims();

    /* Load and process configured claims */
    configs = load_custom_claims_config();

    foreach (lc, configs) {
        CustomClaimConfig *config = (CustomClaimConfig *)lfirst(lc);
        Jsonb *value;

        /* Skip if not for this token type */
        if (for_access_token && !config->include_in_access_token)
            continue;
        if (!for_access_token && !config->include_in_id_token)
            continue;

        /* Resolve claim value */
        value = resolve_custom_claim(config, user_metadata, app_metadata, user_id);

        if (value != NULL) {
            custom_claims = add_jsonb_claim(custom_claims, config->claim_name, value);
        } else if (config->required) {
            ereport(WARNING, (errmsg("Required custom claim '%s' could not be resolved",
                                     config->claim_name)));
        }
    }

    /* Load user-specific claims (these override configured claims) */
    user_claims = load_user_claims(user_id);
    if (user_claims != NULL) {
        custom_claims = merge_jsonb(custom_claims, user_claims);
    }

    return custom_claims;
}

/* ============================================================
 * SQL Functions
 * ============================================================ */

/*
 * orochi_jwt_build_claims
 *    Build standard JWT claims for a user
 *
 * Usage: SELECT orochi_jwt_build_claims(user_id, email, role, session_id,
 * exp_seconds)
 */
Datum orochi_jwt_build_claims(PG_FUNCTION_ARGS)
{
    text *user_id_text;
    text *email_text;
    text *role_text;
    text *session_id_text;
    int32 exp_seconds;
    Jsonb *claims;
    int64 now_unix;
    char *user_id;
    char *issuer = "orochi";

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    user_id_text = PG_GETARG_TEXT_PP(0);
    user_id = text_to_cstring(user_id_text);

    now_unix = orochi_timestamptz_to_unix(GetCurrentTimestamp());

    /* Get optional parameters */
    exp_seconds = PG_ARGISNULL(4) ? 3600 : PG_GETARG_INT32(4);

    /* Build base claims */
    claims = create_empty_claims();

    /* Standard claims */
    claims = add_string_claim(claims, "aud", "authenticated");
    claims = add_numeric_claim(claims, "exp", now_unix + exp_seconds);
    claims = add_numeric_claim(claims, "iat", now_unix);
    claims = add_string_claim(claims, "iss", issuer);
    claims = add_string_claim(claims, "sub", user_id);

    /* Optional claims */
    if (!PG_ARGISNULL(1)) {
        email_text = PG_GETARG_TEXT_PP(1);
        claims = add_string_claim(claims, "email", text_to_cstring(email_text));
    }

    if (!PG_ARGISNULL(2)) {
        role_text = PG_GETARG_TEXT_PP(2);
        claims = add_string_claim(claims, "role", text_to_cstring(role_text));
    } else {
        claims = add_string_claim(claims, "role", "authenticated");
    }

    if (!PG_ARGISNULL(3)) {
        session_id_text = PG_GETARG_TEXT_PP(3);
        claims = add_string_claim(claims, "session_id", text_to_cstring(session_id_text));
    }

    /* Default AAL */
    claims = add_string_claim(claims, "aal", "aal1");

    pfree(user_id);

    PG_RETURN_JSONB_P(claims);
}

/*
 * orochi_jwt_merge_claims
 *    Merge two claim sets (second overwrites first)
 *
 * Usage: SELECT orochi_jwt_merge_claims(base_claims, overlay_claims)
 */
Datum orochi_jwt_merge_claims(PG_FUNCTION_ARGS)
{
    Jsonb *base;
    Jsonb *overlay;
    Jsonb *result;

    if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
        PG_RETURN_NULL();

    if (PG_ARGISNULL(0))
        PG_RETURN_JSONB_P(PG_GETARG_JSONB_P(1));

    if (PG_ARGISNULL(1))
        PG_RETURN_JSONB_P(PG_GETARG_JSONB_P(0));

    base = PG_GETARG_JSONB_P(0);
    overlay = PG_GETARG_JSONB_P(1);

    result = merge_jsonb(base, overlay);

    PG_RETURN_JSONB_P(result);
}

/*
 * orochi_jwt_add_claim
 *    Add or update a claim in a claims object
 *
 * Usage: SELECT orochi_jwt_add_claim(claims, name, value)
 */
Datum orochi_jwt_add_claim(PG_FUNCTION_ARGS)
{
    Jsonb *claims;
    text *name_text;
    Jsonb *value;
    char *name;
    Jsonb *result;

    if (PG_ARGISNULL(1) || PG_ARGISNULL(2)) {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_JSONB_P(PG_GETARG_JSONB_P(0));
    }

    claims = PG_ARGISNULL(0) ? create_empty_claims() : PG_GETARG_JSONB_P(0);
    name_text = PG_GETARG_TEXT_PP(1);
    value = PG_GETARG_JSONB_P(2);

    name = text_to_cstring(name_text);

    /* Validate claim name */
    if (!validate_claim_name(name)) {
        pfree(name);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid claim name: %s", text_to_cstring(name_text))));
    }

    /* Warn if overwriting reserved claim */
    if (is_reserved_claim(name)) {
        ereport(WARNING, (errmsg("Overwriting reserved claim: %s", name)));
    }

    result = add_jsonb_claim(claims, name, value);

    pfree(name);

    PG_RETURN_JSONB_P(result);
}

/*
 * orochi_jwt_remove_claim
 *    Remove a claim from a claims object
 *
 * Usage: SELECT orochi_jwt_remove_claim(claims, name)
 */
Datum orochi_jwt_remove_claim(PG_FUNCTION_ARGS)
{
    Jsonb *claims;
    text *name_text;
    char *name;
    Jsonb *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_JSONB_P(PG_GETARG_JSONB_P(0));
    }

    claims = PG_GETARG_JSONB_P(0);
    name_text = PG_GETARG_TEXT_PP(1);
    name = text_to_cstring(name_text);

    /* Warn if removing reserved claim */
    if (is_reserved_claim(name)) {
        ereport(WARNING, (errmsg("Removing reserved claim: %s", name)));
    }

    result = remove_claim(claims, name);

    pfree(name);

    PG_RETURN_JSONB_P(result);
}

/*
 * orochi_jwt_validate_claims
 *    Validate a claims object for JWT creation
 *
 * Usage: SELECT orochi_jwt_validate_claims(claims)
 * Returns: true if valid, raises error if invalid
 */
Datum orochi_jwt_validate_claims(PG_FUNCTION_ARGS)
{
    Jsonb *claims;
    JsonbValue *sub_val, *exp_val;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("Claims cannot be NULL")));

    claims = PG_GETARG_JSONB_P(0);

    /* Check for required 'sub' claim */
    sub_val = getKeyJsonValueFromContainer(&claims->root, "sub", 3, NULL);
    if (sub_val == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Claims must contain 'sub' (subject) claim")));

    /* Check for 'exp' claim */
    exp_val = getKeyJsonValueFromContainer(&claims->root, "exp", 3, NULL);
    if (exp_val == NULL)
        ereport(WARNING, (errmsg("Claims do not contain 'exp' (expiration) claim")));

    PG_RETURN_BOOL(true);
}

/*
 * orochi_jwt_get_custom_claims
 *    Get resolved custom claims for a user
 *
 * Usage: SELECT orochi_jwt_get_custom_claims(user_id, user_metadata,
 * app_metadata, for_access_token)
 */
Datum orochi_jwt_get_custom_claims(PG_FUNCTION_ARGS)
{
    text *user_id_text;
    Jsonb *user_metadata;
    Jsonb *app_metadata;
    bool for_access_token;
    char *user_id;
    Jsonb *custom_claims;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    user_id_text = PG_GETARG_TEXT_PP(0);
    user_id = text_to_cstring(user_id_text);

    user_metadata = PG_ARGISNULL(1) ? NULL : PG_GETARG_JSONB_P(1);
    app_metadata = PG_ARGISNULL(2) ? NULL : PG_GETARG_JSONB_P(2);
    for_access_token = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);

    custom_claims = build_custom_claims(user_id, user_metadata, app_metadata, for_access_token);

    pfree(user_id);

    PG_RETURN_JSONB_P(custom_claims);
}

/*
 * orochi_jwt_extract_claim_path
 *    Extract a value from JSONB using dot-notation path
 *
 * Usage: SELECT orochi_jwt_extract_claim_path(jsonb_data, 'path.to.value')
 */
Datum orochi_jwt_extract_claim_path(PG_FUNCTION_ARGS)
{
    Jsonb *jsonb;
    text *path_text;
    char *path;
    Jsonb *result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    jsonb = PG_GETARG_JSONB_P(0);
    path_text = PG_GETARG_TEXT_PP(1);
    path = text_to_cstring(path_text);

    result = extract_jsonb_path(jsonb, path);

    pfree(path);

    if (result == NULL)
        PG_RETURN_NULL();

    PG_RETURN_JSONB_P(result);
}
