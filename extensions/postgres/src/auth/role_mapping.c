/*-------------------------------------------------------------------------
 *
 * role_mapping.c
 *    Orochi DB Database Roles Mapped to Application Users Implementation
 *
 * This module implements dynamic role management for high-scale deployments:
 *   - Role template system with JSON-based privilege definitions
 *   - Dynamic role creation at connection time
 *   - High-performance caching for 100K+ roles
 *   - Batch operations for efficient role synchronization
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
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <openssl/md5.h>
#include <string.h>

#include "branch_access.h"
#include "role_mapping.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Memory context for role mapping operations */
static MemoryContext RoleMappingContext = NULL;

/* Role cache */
static HTAB *RoleMappingCache = NULL;

/* Shared state */
typedef struct RoleMappingSharedState {
    LWLock *cache_lock;
    uint64 roles_created;
    uint64 roles_synced;
    uint64 cache_hits;
    uint64 cache_misses;
    uint64 batch_operations;
} RoleMappingSharedState;

static RoleMappingSharedState *role_mapping_shared = NULL;

/* Forward declarations */
static char *uuid_to_string(pg_uuid_t uuid);
static void generate_random_bytes(uint8 *buffer, int length);
static char *generate_hash_from_uuid(pg_uuid_t uuid);
static pg_uuid_t generate_uuid(void);

/* ============================================================
 * Initialization
 * ============================================================ */

/*
 * role_mapping_init - Initialize role mapping module
 */
void role_mapping_init(void)
{
    HASHCTL hash_ctl;

    if (RoleMappingContext == NULL) {
        RoleMappingContext =
            AllocSetContextCreate(TopMemoryContext, "RoleMappingContext", ALLOCSET_DEFAULT_SIZES);
    }

    /* Initialize role cache - sized for 100K+ roles */
    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = 256; /* cache_key size */
    hash_ctl.entrysize = sizeof(RoleCacheEntry);
    hash_ctl.hcxt = RoleMappingContext;

    RoleMappingCache =
        hash_create("RoleMappingCache", ROLE_CACHE_SIZE, &hash_ctl, HASH_ELEM | HASH_CONTEXT);

    elog(LOG, "Role mapping module initialized (cache size: %d)", ROLE_CACHE_SIZE);
}

/*
 * role_mapping_shmem_size - Calculate shared memory size needed
 */
Size role_mapping_shmem_size(void)
{
    return sizeof(RoleMappingSharedState);
}

/* ============================================================
 * Role Name Generation
 * ============================================================ */

/*
 * role_generate_name - Generate PostgreSQL role name for an identity
 */
char *role_generate_name(RoleIdentityType type, pg_uuid_t identity_id)
{
    char *hash;
    char *role_name;
    const char *prefix;

    hash = generate_hash_from_uuid(identity_id);

    switch (type) {
    case ROLE_IDENTITY_USER:
        prefix = ROLE_PREFIX_USER;
        break;
    case ROLE_IDENTITY_TEAM:
        prefix = ROLE_PREFIX_TEAM;
        break;
    case ROLE_IDENTITY_SERVICE:
        prefix = ROLE_PREFIX_SERVICE;
        break;
    default:
        prefix = "orochi_x_";
        break;
    }

    role_name = psprintf("%s%s", prefix, hash);
    pfree(hash);

    return role_name;
}

/*
 * role_generate_branch_name - Generate role name for branch-specific access
 */
char *role_generate_branch_name(pg_uuid_t branch_id)
{
    char *hash = generate_hash_from_uuid(branch_id);
    char *role_name = psprintf("%s%s", ROLE_PREFIX_BRANCH, hash);
    pfree(hash);
    return role_name;
}

/*
 * role_parse_name - Parse role name to extract type and hash
 */
bool role_parse_name(const char *role_name, RoleIdentityType *type, char *hash_out)
{
    if (role_name == NULL)
        return false;

    if (strncmp(role_name, ROLE_PREFIX_USER, strlen(ROLE_PREFIX_USER)) == 0) {
        *type = ROLE_IDENTITY_USER;
        if (hash_out)
            strncpy(hash_out, role_name + strlen(ROLE_PREFIX_USER), ROLE_HASH_LEN);
        return true;
    }

    if (strncmp(role_name, ROLE_PREFIX_TEAM, strlen(ROLE_PREFIX_TEAM)) == 0) {
        *type = ROLE_IDENTITY_TEAM;
        if (hash_out)
            strncpy(hash_out, role_name + strlen(ROLE_PREFIX_TEAM), ROLE_HASH_LEN);
        return true;
    }

    if (strncmp(role_name, ROLE_PREFIX_SERVICE, strlen(ROLE_PREFIX_SERVICE)) == 0) {
        *type = ROLE_IDENTITY_SERVICE;
        if (hash_out)
            strncpy(hash_out, role_name + strlen(ROLE_PREFIX_SERVICE), ROLE_HASH_LEN);
        return true;
    }

    return false;
}

/* ============================================================
 * Template Management
 * ============================================================ */

/*
 * role_get_template - Retrieve role template by ID
 */
OrochiRoleTemplate *role_get_template(pg_uuid_t template_id)
{
    MemoryContext old_context;
    OrochiRoleTemplate *tmpl = NULL;
    StringInfoData query;
    int ret;

    old_context = MemoryContextSwitchTo(RoleMappingContext);

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT template_id, org_id, template_name, display_name, description, "
                     "base_privileges, schema_privileges, table_privileges, "
                     "function_privileges, "
                     "sequence_privileges, rls_policies, role_options, is_system, is_active, "
                     "created_at, updated_at "
                     "FROM platform.role_templates WHERE template_id = '%s'",
                     uuid_to_string(template_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        tmpl = palloc0(sizeof(OrochiRoleTemplate));

        /* Parse template_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 1, &isnull);
            if (!isnull)
                memcpy(&tmpl->template_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        /* Parse org_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 2, &isnull);
            if (!isnull) {
                tmpl->org_id = palloc(sizeof(pg_uuid_t));
                memcpy(tmpl->org_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }

        /* Parse template_name */
        {
            char *name = SPI_getvalue(tuple, tupdesc, 3);
            if (name)
                strncpy(tmpl->template_name, name, ROLE_TEMPLATE_MAX_LEN);
        }

        /* Parse display_name and description */
        {
            char *display_name = SPI_getvalue(tuple, tupdesc, 4);
            char *description = SPI_getvalue(tuple, tupdesc, 5);
            if (display_name)
                tmpl->display_name = pstrdup(display_name);
            if (description)
                tmpl->description = pstrdup(description);
        }

        /* Parse privilege JSON fields */
        {
            char *base_priv = SPI_getvalue(tuple, tupdesc, 6);
            char *schema_priv = SPI_getvalue(tuple, tupdesc, 7);
            char *table_priv = SPI_getvalue(tuple, tupdesc, 8);
            char *func_priv = SPI_getvalue(tuple, tupdesc, 9);
            char *seq_priv = SPI_getvalue(tuple, tupdesc, 10);
            char *rls = SPI_getvalue(tuple, tupdesc, 11);

            if (base_priv)
                tmpl->base_privileges = pstrdup(base_priv);
            if (schema_priv)
                tmpl->schema_privileges = pstrdup(schema_priv);
            if (table_priv)
                tmpl->table_privileges = pstrdup(table_priv);
            if (func_priv)
                tmpl->function_privileges = pstrdup(func_priv);
            if (seq_priv)
                tmpl->sequence_privileges = pstrdup(seq_priv);
            if (rls)
                tmpl->rls_policies = pstrdup(rls);
        }

        /* Parse role_options JSON - extract individual options */
        /* For now, use defaults */
        tmpl->can_login = true;
        tmpl->can_createdb = false;
        tmpl->can_createrole = false;
        tmpl->inherit = true;
        tmpl->can_replication = false;
        tmpl->bypass_rls = false;
        tmpl->connection_limit = 10;

        /* Parse flags */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 13, &isnull);
            tmpl->is_system = isnull ? false : DatumGetBool(d);
        }
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 14, &isnull);
            tmpl->is_active = isnull ? true : DatumGetBool(d);
        }
    }

    SPI_finish();
    pfree(query.data);

    MemoryContextSwitchTo(old_context);

    return tmpl;
}

/*
 * role_get_template_by_name - Retrieve template by name
 */
OrochiRoleTemplate *role_get_template_by_name(pg_uuid_t *org_id, const char *template_name)
{
    OrochiRoleTemplate *tmpl = NULL;
    StringInfoData query;
    int ret;

    if (template_name == NULL)
        return NULL;

    initStringInfo(&query);

    if (org_id != NULL) {
        appendStringInfo(&query,
                         "SELECT template_id FROM platform.role_templates "
                         "WHERE (org_id = '%s' OR org_id IS NULL) AND template_name = '%s' "
                         "ORDER BY org_id NULLS LAST LIMIT 1",
                         uuid_to_string(*org_id), template_name);
    } else {
        appendStringInfo(&query,
                         "SELECT template_id FROM platform.role_templates "
                         "WHERE org_id IS NULL AND template_name = '%s'",
                         template_name);
    }

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            pg_uuid_t template_id;
            memcpy(&template_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            SPI_finish();
            pfree(query.data);
            return role_get_template(template_id);
        }
    }

    SPI_finish();
    pfree(query.data);

    return NULL;
}

/* ============================================================
 * Role Mapping Management
 * ============================================================ */

/*
 * role_get_mapping - Retrieve role mapping by ID
 */
OrochiRoleMapping *role_get_mapping(pg_uuid_t mapping_id)
{
    MemoryContext old_context;
    OrochiRoleMapping *mapping = NULL;
    StringInfoData query;
    int ret;

    old_context = MemoryContextSwitchTo(RoleMappingContext);

    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT mapping_id, user_id, team_id, service_account_id, "
                     "cluster_id, branch_id, template_id, custom_privileges, "
                     "pg_role_name, pg_role_created, status, last_sync_at, sync_error, "
                     "created_at, updated_at "
                     "FROM platform.user_role_mappings WHERE mapping_id = '%s'",
                     uuid_to_string(mapping_id));

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tuple = SPI_tuptable->vals[0];
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        bool isnull;

        mapping = palloc0(sizeof(OrochiRoleMapping));

        /* Parse mapping_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 1, &isnull);
            if (!isnull)
                memcpy(&mapping->mapping_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        /* Parse identity fields */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 2, &isnull);
            if (!isnull) {
                mapping->user_id = palloc(sizeof(pg_uuid_t));
                memcpy(mapping->user_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 3, &isnull);
            if (!isnull) {
                mapping->team_id = palloc(sizeof(pg_uuid_t));
                memcpy(mapping->team_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 4, &isnull);
            if (!isnull) {
                mapping->service_account_id = palloc(sizeof(pg_uuid_t));
                memcpy(mapping->service_account_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }

        /* Parse cluster_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 5, &isnull);
            if (!isnull)
                memcpy(&mapping->cluster_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
        }

        /* Parse branch_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 6, &isnull);
            if (!isnull) {
                mapping->branch_id = palloc(sizeof(pg_uuid_t));
                memcpy(mapping->branch_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }

        /* Parse template_id */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 7, &isnull);
            if (!isnull) {
                mapping->template_id = palloc(sizeof(pg_uuid_t));
                memcpy(mapping->template_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            }
        }

        /* Parse custom_privileges */
        {
            char *custom_priv = SPI_getvalue(tuple, tupdesc, 8);
            if (custom_priv)
                mapping->custom_privileges = pstrdup(custom_priv);
        }

        /* Parse pg_role_name */
        {
            char *role_name = SPI_getvalue(tuple, tupdesc, 9);
            if (role_name)
                strncpy(mapping->pg_role_name, role_name, ROLE_NAME_MAX_LEN);
        }

        /* Parse pg_role_created */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 10, &isnull);
            mapping->pg_role_created = isnull ? false : DatumGetBool(d);
        }

        /* Parse status */
        {
            char *status_str = SPI_getvalue(tuple, tupdesc, 11);
            if (status_str)
                mapping->status = role_parse_mapping_status(status_str);
        }

        /* Parse timestamps */
        {
            Datum d = SPI_getbinval(tuple, tupdesc, 12, &isnull);
            if (!isnull)
                mapping->last_sync_at = DatumGetTimestampTz(d);
        }

        /* Parse sync_error */
        {
            char *sync_error = SPI_getvalue(tuple, tupdesc, 13);
            if (sync_error)
                mapping->sync_error = pstrdup(sync_error);
        }
    }

    SPI_finish();
    pfree(query.data);

    MemoryContextSwitchTo(old_context);

    return mapping;
}

/*
 * role_get_mapping_for_user - Get mapping for user/cluster/branch combination
 */
OrochiRoleMapping *role_get_mapping_for_user(pg_uuid_t user_id, pg_uuid_t cluster_id,
                                             pg_uuid_t *branch_id)
{
    OrochiRoleMapping *mapping = NULL;
    RoleCacheEntry *cache_entry;
    StringInfoData cache_key;
    StringInfoData query;
    bool found;
    int ret;

    /* Build cache key */
    initStringInfo(&cache_key);
    appendStringInfo(&cache_key, "%s:%s:%s", uuid_to_string(user_id), uuid_to_string(cluster_id),
                     branch_id ? uuid_to_string(*branch_id) : "null");

    /* Check cache */
    if (RoleMappingCache != NULL) {
        cache_entry = hash_search(RoleMappingCache, cache_key.data, HASH_FIND, &found);
        if (found && cache_entry->is_valid) {
            int64 age_sec = (GetCurrentTimestamp() - cache_entry->cached_at) / 1000000;
            if (age_sec < ROLE_CACHE_TTL_SEC) {
                if (role_mapping_shared != NULL) {
                    LWLockAcquire(role_mapping_shared->cache_lock, LW_EXCLUSIVE);
                    role_mapping_shared->cache_hits++;
                    LWLockRelease(role_mapping_shared->cache_lock);
                }

                /* Return cached mapping (recreate from cache_entry) */
                mapping = palloc0(sizeof(OrochiRoleMapping));
                strncpy(mapping->pg_role_name, cache_entry->pg_role_name, ROLE_NAME_MAX_LEN);
                mapping->status = ROLE_MAPPING_ACTIVE;
                pfree(cache_key.data);
                return mapping;
            }
        }
    }

    if (role_mapping_shared != NULL) {
        LWLockAcquire(role_mapping_shared->cache_lock, LW_EXCLUSIVE);
        role_mapping_shared->cache_misses++;
        LWLockRelease(role_mapping_shared->cache_lock);
    }

    /* Query database */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT mapping_id FROM platform.user_role_mappings "
                     "WHERE user_id = '%s' AND cluster_id = '%s' ",
                     uuid_to_string(user_id), uuid_to_string(cluster_id));

    if (branch_id != NULL)
        appendStringInfo(&query, "AND branch_id = '%s' ", uuid_to_string(*branch_id));
    else
        appendStringInfoString(&query, "AND branch_id IS NULL ");

    appendStringInfoString(&query, "AND status != 'deleted' LIMIT 1");

    SPI_connect();
    ret = SPI_execute(query.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            pg_uuid_t mapping_id;
            memcpy(&mapping_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
            SPI_finish();
            pfree(query.data);
            mapping = role_get_mapping(mapping_id);

            /* Cache the result */
            if (mapping != NULL && RoleMappingCache != NULL) {
                cache_entry = hash_search(RoleMappingCache, cache_key.data, HASH_ENTER, &found);
                strncpy(cache_entry->cache_key, cache_key.data, 255);
                strncpy(cache_entry->pg_role_name, mapping->pg_role_name, ROLE_NAME_MAX_LEN);
                cache_entry->cached_at = GetCurrentTimestamp();
                cache_entry->is_valid = true;
            }

            pfree(cache_key.data);
            return mapping;
        }
    }

    SPI_finish();
    pfree(query.data);
    pfree(cache_key.data);

    return NULL;
}

/*
 * role_create_mapping - Create new role mapping for a user
 */
pg_uuid_t role_create_mapping(pg_uuid_t user_id, pg_uuid_t cluster_id, pg_uuid_t *branch_id,
                              pg_uuid_t template_id, const char *custom_privileges)
{
    pg_uuid_t mapping_id;
    char *pg_role_name;
    StringInfoData query;
    int ret;

    /* Generate mapping ID */
    mapping_id = generate_uuid();

    /* Generate PostgreSQL role name */
    pg_role_name = role_generate_name(ROLE_IDENTITY_USER, user_id);

    /* Insert mapping */
    initStringInfo(&query);
    appendStringInfo(
        &query,
        "INSERT INTO platform.user_role_mappings "
        "(mapping_id, user_id, cluster_id, branch_id, template_id, "
        "custom_privileges, pg_role_name, pg_role_created, status) "
        "VALUES ('%s', '%s', '%s', %s, '%s', %s, '%s', FALSE, 'pending') "
        "ON CONFLICT (user_id, cluster_id, branch_id) DO UPDATE SET "
        "template_id = EXCLUDED.template_id, "
        "custom_privileges = EXCLUDED.custom_privileges, "
        "status = 'pending', "
        "updated_at = NOW()",
        uuid_to_string(mapping_id), uuid_to_string(user_id), uuid_to_string(cluster_id),
        branch_id ? psprintf("'%s'", uuid_to_string(*branch_id)) : "NULL",
        uuid_to_string(template_id),
        custom_privileges ? quote_literal_cstr(custom_privileges) : "NULL", pg_role_name);

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);
    SPI_finish();

    pfree(query.data);
    pfree(pg_role_name);

    if (ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to create role mapping")));
    }

    elog(LOG, "Created role mapping %s for user %s", uuid_to_string(mapping_id),
         uuid_to_string(user_id));

    return mapping_id;
}

/* ============================================================
 * Database Role Operations
 * ============================================================ */

/*
 * role_create_db_role - Create the PostgreSQL role for a mapping
 */
bool role_create_db_role(pg_uuid_t mapping_id)
{
    OrochiRoleMapping *mapping;
    OrochiRoleTemplate *tmpl = NULL;
    StringInfoData sql;
    int ret;

    mapping = role_get_mapping(mapping_id);
    if (mapping == NULL) {
        elog(WARNING, "Mapping %s not found", uuid_to_string(mapping_id));
        return false;
    }

    if (mapping->pg_role_created) {
        role_free_mapping(mapping);
        return true; /* Already created */
    }

    /* Get template for role options */
    if (mapping->template_id != NULL)
        tmpl = role_get_template(*mapping->template_id);

    /* Build CREATE ROLE statement */
    initStringInfo(&sql);
    appendStringInfo(&sql, "CREATE ROLE %s", quote_identifier(mapping->pg_role_name));

    if (tmpl != NULL) {
        if (tmpl->can_login)
            appendStringInfoString(&sql, " LOGIN");
        else
            appendStringInfoString(&sql, " NOLOGIN");

        if (tmpl->can_createdb)
            appendStringInfoString(&sql, " CREATEDB");

        if (tmpl->can_createrole)
            appendStringInfoString(&sql, " CREATEROLE");

        if (tmpl->inherit)
            appendStringInfoString(&sql, " INHERIT");
        else
            appendStringInfoString(&sql, " NOINHERIT");

        if (tmpl->connection_limit > 0)
            appendStringInfo(&sql, " CONNECTION LIMIT %d", tmpl->connection_limit);
    } else {
        /* Default options */
        appendStringInfoString(&sql, " LOGIN INHERIT CONNECTION LIMIT 10");
    }

    /* Execute CREATE ROLE */
    SPI_connect();
    PG_TRY();
    {
        ret = SPI_execute(sql.data, false, 0);
    }
    PG_CATCH();
    {
        /* Role might already exist, which is okay */
        FlushErrorState();
        ret = SPI_OK_UTILITY;
    }
    PG_END_TRY();
    SPI_finish();

    pfree(sql.data);

    /* Update mapping status */
    initStringInfo(&sql);
    appendStringInfo(&sql,
                     "UPDATE platform.user_role_mappings SET "
                     "pg_role_created = TRUE, status = 'active', last_sync_at = NOW() "
                     "WHERE mapping_id = '%s'",
                     uuid_to_string(mapping_id));

    SPI_connect();
    SPI_execute(sql.data, false, 0);
    SPI_finish();

    pfree(sql.data);

    if (role_mapping_shared != NULL) {
        LWLockAcquire(role_mapping_shared->cache_lock, LW_EXCLUSIVE);
        role_mapping_shared->roles_created++;
        LWLockRelease(role_mapping_shared->cache_lock);
    }

    if (tmpl != NULL)
        role_free_template(tmpl);
    role_free_mapping(mapping);

    elog(LOG, "Created database role for mapping %s", uuid_to_string(mapping_id));

    return true;
}

/*
 * role_apply_privileges - Apply template privileges to a role
 */
bool role_apply_privileges(pg_uuid_t mapping_id)
{
    OrochiRoleMapping *mapping;
    OrochiRoleTemplate *tmpl = NULL;
    StringInfoData sql;
    int ret;

    mapping = role_get_mapping(mapping_id);
    if (mapping == NULL)
        return false;

    if (mapping->template_id != NULL)
        tmpl = role_get_template(*mapping->template_id);

    if (tmpl == NULL) {
        role_free_mapping(mapping);
        return true; /* No template, nothing to apply */
    }

    /* Apply base privileges to public schema */
    if (tmpl->base_privileges != NULL) {
        /* Parse JSON and apply - simplified for now */
        /* In production, parse JSON array and apply each privilege */

        /* Example: GRANT USAGE ON SCHEMA public TO role */
        initStringInfo(&sql);
        appendStringInfo(&sql, "GRANT USAGE ON SCHEMA public TO %s",
                         quote_identifier(mapping->pg_role_name));

        SPI_connect();
        ret = SPI_execute(sql.data, false, 0);
        SPI_finish();

        pfree(sql.data);
    }

    /* Apply table privileges */
    if (tmpl->table_privileges != NULL) {
        /* Grant SELECT on all tables by default */
        initStringInfo(&sql);
        appendStringInfo(&sql, "GRANT SELECT ON ALL TABLES IN SCHEMA public TO %s",
                         quote_identifier(mapping->pg_role_name));

        SPI_connect();
        SPI_execute(sql.data, false, 0);
        SPI_finish();

        pfree(sql.data);

        /* Set default privileges for future tables */
        initStringInfo(&sql);
        appendStringInfo(&sql,
                         "ALTER DEFAULT PRIVILEGES IN SCHEMA public "
                         "GRANT SELECT ON TABLES TO %s",
                         quote_identifier(mapping->pg_role_name));

        SPI_connect();
        SPI_execute(sql.data, false, 0);
        SPI_finish();

        pfree(sql.data);
    }

    /* Apply sequence privileges */
    if (tmpl->sequence_privileges != NULL) {
        initStringInfo(&sql);
        appendStringInfo(&sql, "GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO %s",
                         quote_identifier(mapping->pg_role_name));

        SPI_connect();
        SPI_execute(sql.data, false, 0);
        SPI_finish();

        pfree(sql.data);
    }

    /* Apply function privileges */
    if (tmpl->function_privileges != NULL) {
        initStringInfo(&sql);
        appendStringInfo(&sql, "GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO %s",
                         quote_identifier(mapping->pg_role_name));

        SPI_connect();
        SPI_execute(sql.data, false, 0);
        SPI_finish();

        pfree(sql.data);
    }

    role_free_template(tmpl);
    role_free_mapping(mapping);

    return true;
}

/*
 * role_sync_to_cluster - Sync role to cluster (create + apply privileges)
 */
bool role_sync_to_cluster(pg_uuid_t mapping_id)
{
    bool success;

    /* Create the role first */
    success = role_create_db_role(mapping_id);
    if (!success)
        return false;

    /* Apply privileges */
    success = role_apply_privileges(mapping_id);

    if (role_mapping_shared != NULL) {
        LWLockAcquire(role_mapping_shared->cache_lock, LW_EXCLUSIVE);
        role_mapping_shared->roles_synced++;
        LWLockRelease(role_mapping_shared->cache_lock);
    }

    return success;
}

/* ============================================================
 * Batch Operations
 * ============================================================ */

/*
 * role_batch_create - Create roles for multiple users in batch
 */
int role_batch_create(pg_uuid_t cluster_id, pg_uuid_t *user_ids, int count)
{
    int success_count = 0;
    int batch_size;
    StringInfoData batch_sql;
    int i;

    if (count <= 0 || user_ids == NULL)
        return 0;

    if (role_mapping_shared != NULL) {
        LWLockAcquire(role_mapping_shared->cache_lock, LW_EXCLUSIVE);
        role_mapping_shared->batch_operations++;
        LWLockRelease(role_mapping_shared->cache_lock);
    }

    /* Process in batches for efficiency */
    for (i = 0; i < count; i += ROLE_BATCH_SIZE) {
        int j;
        batch_size = (i + ROLE_BATCH_SIZE > count) ? (count - i) : ROLE_BATCH_SIZE;

        initStringInfo(&batch_sql);

        for (j = 0; j < batch_size; j++) {
            char *role_name = role_generate_name(ROLE_IDENTITY_USER, user_ids[i + j]);

            appendStringInfo(&batch_sql,
                             "DO $$ BEGIN "
                             "CREATE ROLE %s LOGIN CONNECTION LIMIT 10; "
                             "EXCEPTION WHEN duplicate_object THEN NULL; END $$; ",
                             quote_identifier(role_name));

            pfree(role_name);
        }

        SPI_connect();
        SPI_execute(batch_sql.data, false, 0);
        SPI_finish();

        pfree(batch_sql.data);

        success_count += batch_size;
    }

    elog(LOG, "Batch created %d roles for cluster %s", success_count, uuid_to_string(cluster_id));

    return success_count;
}

/* ============================================================
 * High-Performance Lookup
 * ============================================================ */

/*
 * role_lookup_for_connection - Fast role lookup for connection time
 */
char *role_lookup_for_connection(pg_uuid_t user_id, pg_uuid_t cluster_id, pg_uuid_t *branch_id)
{
    OrochiRoleMapping *mapping;
    char *role_name;

    /* Try to get existing mapping */
    mapping = role_get_mapping_for_user(user_id, cluster_id, branch_id);

    if (mapping != NULL && mapping->status == ROLE_MAPPING_ACTIVE) {
        role_name = pstrdup(mapping->pg_role_name);
        role_free_mapping(mapping);
        return role_name;
    }

    if (mapping != NULL)
        role_free_mapping(mapping);

    /* Generate role name (role may need to be created lazily) */
    return role_generate_name(ROLE_IDENTITY_USER, user_id);
}

/* ============================================================
 * User Lifecycle
 * ============================================================ */

/*
 * role_cleanup_user_roles - Cleanup all roles for a deleted user
 */
int role_cleanup_user_roles(pg_uuid_t user_id)
{
    StringInfoData query;
    int ret;
    int count = 0;

    /* Mark all mappings as deleted */
    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE platform.user_role_mappings SET "
                     "status = 'deleted', updated_at = NOW() "
                     "WHERE user_id = '%s' AND status != 'deleted'",
                     uuid_to_string(user_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_UPDATE)
        count = SPI_processed;

    SPI_finish();
    pfree(query.data);

    /* Invalidate cache */
    role_invalidate_user_cache(user_id);

    elog(LOG, "Cleaned up %d role mappings for user %s", count, uuid_to_string(user_id));

    return count;
}

/*
 * role_suspend_user_roles - Suspend all roles for a user
 */
int role_suspend_user_roles(pg_uuid_t user_id)
{
    StringInfoData query;
    int ret;
    int count = 0;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE platform.user_role_mappings SET "
                     "status = 'suspended', updated_at = NOW() "
                     "WHERE user_id = '%s' AND status = 'active'",
                     uuid_to_string(user_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_UPDATE)
        count = SPI_processed;

    SPI_finish();
    pfree(query.data);

    role_invalidate_user_cache(user_id);

    return count;
}

/*
 * role_reactivate_user_roles - Reactivate suspended roles for a user
 */
int role_reactivate_user_roles(pg_uuid_t user_id)
{
    StringInfoData query;
    int ret;
    int count = 0;

    initStringInfo(&query);
    appendStringInfo(&query,
                     "UPDATE platform.user_role_mappings SET "
                     "status = 'active', updated_at = NOW() "
                     "WHERE user_id = '%s' AND status = 'suspended'",
                     uuid_to_string(user_id));

    SPI_connect();
    ret = SPI_execute(query.data, false, 0);

    if (ret == SPI_OK_UPDATE)
        count = SPI_processed;

    SPI_finish();
    pfree(query.data);

    role_invalidate_user_cache(user_id);

    return count;
}

/* ============================================================
 * Caching
 * ============================================================ */

/*
 * role_invalidate_cache - Invalidate cache for a cluster
 */
void role_invalidate_cache(pg_uuid_t cluster_id)
{
    HASH_SEQ_STATUS status;
    RoleCacheEntry *entry;
    char cluster_str[64];

    if (RoleMappingCache == NULL)
        return;

    snprintf(cluster_str, sizeof(cluster_str), ":%s:", uuid_to_string(cluster_id));

    hash_seq_init(&status, RoleMappingCache);
    while ((entry = hash_seq_search(&status)) != NULL) {
        if (strstr(entry->cache_key, cluster_str) != NULL)
            entry->is_valid = false;
    }
}

/*
 * role_invalidate_user_cache - Invalidate cache for a user
 */
void role_invalidate_user_cache(pg_uuid_t user_id)
{
    HASH_SEQ_STATUS status;
    RoleCacheEntry *entry;
    char user_str[64];

    if (RoleMappingCache == NULL)
        return;

    snprintf(user_str, sizeof(user_str), "%s:", uuid_to_string(user_id));

    hash_seq_init(&status, RoleMappingCache);
    while ((entry = hash_seq_search(&status)) != NULL) {
        if (strncmp(entry->cache_key, user_str, strlen(user_str)) == 0)
            entry->is_valid = false;
    }
}

/*
 * role_clear_cache - Clear entire cache
 */
void role_clear_cache(void)
{
    HASH_SEQ_STATUS status;
    RoleCacheEntry *entry;

    if (RoleMappingCache == NULL)
        return;

    hash_seq_init(&status, RoleMappingCache);
    while ((entry = hash_seq_search(&status)) != NULL) {
        entry->is_valid = false;
    }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

const char *role_identity_type_name(RoleIdentityType type)
{
    switch (type) {
    case ROLE_IDENTITY_USER:
        return "user";
    case ROLE_IDENTITY_TEAM:
        return "team";
    case ROLE_IDENTITY_SERVICE:
        return "service";
    default:
        return "unknown";
    }
}

const char *role_mapping_status_name(RoleMappingStatus status)
{
    switch (status) {
    case ROLE_MAPPING_PENDING:
        return "pending";
    case ROLE_MAPPING_ACTIVE:
        return "active";
    case ROLE_MAPPING_SYNCING:
        return "syncing";
    case ROLE_MAPPING_ERROR:
        return "error";
    case ROLE_MAPPING_SUSPENDED:
        return "suspended";
    case ROLE_MAPPING_DELETED:
        return "deleted";
    default:
        return "unknown";
    }
}

const char *role_sync_operation_name(RoleSyncOperation op)
{
    switch (op) {
    case ROLE_SYNC_CREATE:
        return "create";
    case ROLE_SYNC_UPDATE_PRIVILEGES:
        return "update_privileges";
    case ROLE_SYNC_SUSPEND:
        return "suspend";
    case ROLE_SYNC_DELETE:
        return "delete";
    default:
        return "unknown";
    }
}

RoleIdentityType role_parse_identity_type(const char *str)
{
    if (str == NULL)
        return ROLE_IDENTITY_USER;

    if (strcmp(str, "team") == 0)
        return ROLE_IDENTITY_TEAM;
    if (strcmp(str, "service") == 0)
        return ROLE_IDENTITY_SERVICE;

    return ROLE_IDENTITY_USER;
}

RoleMappingStatus role_parse_mapping_status(const char *str)
{
    if (str == NULL)
        return ROLE_MAPPING_PENDING;

    if (strcmp(str, "active") == 0)
        return ROLE_MAPPING_ACTIVE;
    if (strcmp(str, "syncing") == 0)
        return ROLE_MAPPING_SYNCING;
    if (strcmp(str, "error") == 0)
        return ROLE_MAPPING_ERROR;
    if (strcmp(str, "suspended") == 0)
        return ROLE_MAPPING_SUSPENDED;
    if (strcmp(str, "deleted") == 0)
        return ROLE_MAPPING_DELETED;

    return ROLE_MAPPING_PENDING;
}

/* ============================================================
 * Cleanup Functions
 * ============================================================ */

void role_free_template(OrochiRoleTemplate *tmpl)
{
    if (tmpl == NULL)
        return;

    if (tmpl->org_id)
        pfree(tmpl->org_id);
    if (tmpl->display_name)
        pfree(tmpl->display_name);
    if (tmpl->description)
        pfree(tmpl->description);
    if (tmpl->base_privileges)
        pfree(tmpl->base_privileges);
    if (tmpl->schema_privileges)
        pfree(tmpl->schema_privileges);
    if (tmpl->table_privileges)
        pfree(tmpl->table_privileges);
    if (tmpl->function_privileges)
        pfree(tmpl->function_privileges);
    if (tmpl->sequence_privileges)
        pfree(tmpl->sequence_privileges);
    if (tmpl->rls_policies)
        pfree(tmpl->rls_policies);

    pfree(tmpl);
}

void role_free_mapping(OrochiRoleMapping *mapping)
{
    if (mapping == NULL)
        return;

    if (mapping->user_id)
        pfree(mapping->user_id);
    if (mapping->team_id)
        pfree(mapping->team_id);
    if (mapping->service_account_id)
        pfree(mapping->service_account_id);
    if (mapping->branch_id)
        pfree(mapping->branch_id);
    if (mapping->template_id)
        pfree(mapping->template_id);
    if (mapping->custom_privileges)
        pfree(mapping->custom_privileges);
    if (mapping->sync_error)
        pfree(mapping->sync_error);

    pfree(mapping);
}

void role_free_statistics(RoleStatistics *stats)
{
    if (stats != NULL)
        pfree(stats);
}

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

static char *uuid_to_string(pg_uuid_t uuid)
{
    char *str = palloc(37);

    snprintf(str, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3], uuid.data[4], uuid.data[5],
             uuid.data[6], uuid.data[7], uuid.data[8], uuid.data[9], uuid.data[10], uuid.data[11],
             uuid.data[12], uuid.data[13], uuid.data[14], uuid.data[15]);

    return str;
}

static void generate_random_bytes(uint8 *buffer, int length)
{
    /* Use PostgreSQL's random bytes generation */
    for (int i = 0; i < length; i++)
        buffer[i] = (uint8)(random() & 0xFF);
}

static char *generate_hash_from_uuid(pg_uuid_t uuid)
{
    uint8 md5_hash[MD5_DIGEST_LENGTH];
    char *hash_str;
    MD5_CTX ctx;

    MD5_Init(&ctx);
    MD5_Update(&ctx, uuid.data, sizeof(uuid.data));
    MD5_Final(md5_hash, &ctx);

    /* Take first 8 characters (4 bytes) */
    hash_str = palloc(ROLE_HASH_LEN + 1);
    for (int i = 0; i < ROLE_HASH_LEN / 2; i++)
        snprintf(hash_str + i * 2, 3, "%02x", md5_hash[i]);
    hash_str[ROLE_HASH_LEN] = '\0';

    return hash_str;
}

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

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(role_create_mapping_sql);
PG_FUNCTION_INFO_V1(role_sync_mapping_sql);
PG_FUNCTION_INFO_V1(role_lookup_sql);
PG_FUNCTION_INFO_V1(role_batch_create_sql);

Datum role_create_mapping_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
    pg_uuid_t *cluster_id = PG_GETARG_UUID_P(1);
    pg_uuid_t *branch_id = PG_ARGISNULL(2) ? NULL : PG_GETARG_UUID_P(2);
    pg_uuid_t *template_id = PG_GETARG_UUID_P(3);
    text *custom_priv = PG_ARGISNULL(4) ? NULL : PG_GETARG_TEXT_PP(4);

    pg_uuid_t mapping_id;

    mapping_id = role_create_mapping(*user_id, *cluster_id, branch_id, *template_id,
                                     custom_priv ? text_to_cstring(custom_priv) : NULL);

    PG_RETURN_UUID_P(&mapping_id);
}

Datum role_sync_mapping_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *mapping_id = PG_GETARG_UUID_P(0);

    bool success = role_sync_to_cluster(*mapping_id);

    PG_RETURN_BOOL(success);
}

Datum role_lookup_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
    pg_uuid_t *cluster_id = PG_GETARG_UUID_P(1);
    pg_uuid_t *branch_id = PG_ARGISNULL(2) ? NULL : PG_GETARG_UUID_P(2);

    char *role_name = role_lookup_for_connection(*user_id, *cluster_id, branch_id);

    PG_RETURN_TEXT_P(cstring_to_text(role_name));
}

Datum role_batch_create_sql(PG_FUNCTION_ARGS)
{
    pg_uuid_t *cluster_id = PG_GETARG_UUID_P(0);
    ArrayType *user_ids_array = PG_GETARG_ARRAYTYPE_P(1);
    pg_uuid_t *user_ids;
    int count;
    int success_count;
    Datum *datums;
    bool *nulls;
    int i;

    deconstruct_array(user_ids_array, UUIDOID, 16, false, 'c', &datums, &nulls, &count);

    user_ids = palloc(sizeof(pg_uuid_t) * count);
    for (i = 0; i < count; i++) {
        if (!nulls[i])
            memcpy(&user_ids[i], DatumGetUUIDP(datums[i]), sizeof(pg_uuid_t));
    }

    success_count = role_batch_create(*cluster_id, user_ids, count);

    pfree(user_ids);

    PG_RETURN_INT32(success_count);
}
