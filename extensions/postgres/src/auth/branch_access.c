/*-------------------------------------------------------------------------
 *
 * branch_access.c
 *    Orochi DB Branch-Level Access Control Implementation
 *
 * This module implements fine-grained access control for database branches:
 *   - Permission resolution with inheritance support
 *   - Protection rules enforcement
 *   - Cross-branch operation validation
 *   - Branch-shard isolation for copy-on-write semantics
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
#include "miscadmin.h"
#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include <string.h>

#include "branch_access.h"

/* ============================================================
 * Static Variables and Forward Declarations
 * ============================================================ */

/* Memory context for branch access operations */
static MemoryContext BranchAccessContext = NULL;

/* Permission cache using hash table */
static HTAB *BranchPermissionCache = NULL;

/* Shared state */
typedef struct BranchAccessSharedState {
  LWLock *cache_lock;
  uint64 cache_hits;
  uint64 cache_misses;
  uint64 permission_checks;
  uint64 access_denied;
} BranchAccessSharedState;

static BranchAccessSharedState *branch_access_shared = NULL;

/* Forward declarations */
static char *uuid_to_string(pg_uuid_t uuid);
static BranchPermissionLevel
resolve_inherited_permission(pg_uuid_t user_id, pg_uuid_t branch_id, int depth);
static bool check_ip_restriction(OrochiBranchPermission *perm,
                                 const char *ip_address);
static bool check_time_restriction(OrochiBranchPermission *perm,
                                   TimestampTz check_time);

/* ============================================================
 * Initialization
 * ============================================================ */

/*
 * branch_access_init - Initialize branch access control module
 */
void branch_access_init(void) {
  HASHCTL hash_ctl;

  if (BranchAccessContext == NULL) {
    BranchAccessContext = AllocSetContextCreate(
        TopMemoryContext, "BranchAccessContext", ALLOCSET_DEFAULT_SIZES);
  }

  /* Initialize permission cache */
  memset(&hash_ctl, 0, sizeof(hash_ctl));
  hash_ctl.keysize = sizeof(char) * 128;
  hash_ctl.entrysize = sizeof(BranchPermissionCacheEntry);
  hash_ctl.hcxt = BranchAccessContext;

  BranchPermissionCache =
      hash_create("BranchPermissionCache", BRANCH_PERM_CACHE_SIZE, &hash_ctl,
                  HASH_ELEM | HASH_CONTEXT);

  elog(LOG, "Branch access control module initialized");
}

/*
 * branch_access_shmem_size - Calculate shared memory size needed
 */
Size branch_access_shmem_size(void) { return sizeof(BranchAccessSharedState); }

/* ============================================================
 * Permission Resolution
 * ============================================================ */

/*
 * branch_get_permission - Get user's permission level for a branch
 */
BranchPermissionLevel branch_get_permission(pg_uuid_t user_id,
                                            pg_uuid_t branch_id) {
  return branch_get_effective_permission(user_id, branch_id, NULL);
}

/*
 * branch_get_effective_permission - Get permission with constraint checks
 */
BranchPermissionLevel branch_get_effective_permission(pg_uuid_t user_id,
                                                      pg_uuid_t branch_id,
                                                      const char *ip_address) {
  MemoryContext old_context;
  BranchPermissionLevel permission = BRANCH_PERM_NONE;
  BranchPermissionCacheEntry *cache_entry;
  OrochiBranchInfo *branch;
  StringInfoData query;
  StringInfoData cache_key;
  bool found;
  int ret;

  old_context = MemoryContextSwitchTo(BranchAccessContext);

  /* Build cache key */
  initStringInfo(&cache_key);
  appendStringInfo(&cache_key, "%s:%s", uuid_to_string(user_id),
                   uuid_to_string(branch_id));

  /* Check cache first */
  if (BranchPermissionCache != NULL) {
    cache_entry =
        hash_search(BranchPermissionCache, cache_key.data, HASH_FIND, &found);

    if (found && cache_entry->is_valid) {
      TimestampTz now = GetCurrentTimestamp();
      int64 age_sec = (now - cache_entry->cached_at) / 1000000;

      if (age_sec < BRANCH_PERM_CACHE_TTL_SEC) {
        if (branch_access_shared != NULL) {
          LWLockAcquire(branch_access_shared->cache_lock, LW_EXCLUSIVE);
          branch_access_shared->cache_hits++;
          LWLockRelease(branch_access_shared->cache_lock);
        }

        pfree(cache_key.data);
        MemoryContextSwitchTo(old_context);
        return cache_entry->permission;
      }
    }
  }

  if (branch_access_shared != NULL) {
    LWLockAcquire(branch_access_shared->cache_lock, LW_EXCLUSIVE);
    branch_access_shared->cache_misses++;
    branch_access_shared->permission_checks++;
    LWLockRelease(branch_access_shared->cache_lock);
  }

  /* Get branch info */
  branch = branch_get_info(branch_id);
  if (branch == NULL) {
    pfree(cache_key.data);
    MemoryContextSwitchTo(old_context);
    return BRANCH_PERM_NONE;
  }

  /* Check if branch is locked */
  if (branch->protection_level == BRANCH_PROTECTION_LOCKED) {
    branch_free_info(branch);
    pfree(cache_key.data);
    MemoryContextSwitchTo(old_context);
    return BRANCH_PERM_NONE;
  }

  /* Check if user is branch owner */
  if (memcmp(&branch->owner_user_id, &user_id, sizeof(pg_uuid_t)) == 0) {
    permission = BRANCH_PERM_ADMIN;
    goto cache_and_return;
  }

  /* Check direct user permission */
  initStringInfo(&query);
  appendStringInfo(&query,
                   "SELECT permission_level FROM platform.branch_permissions "
                   "WHERE branch_id = '%s' AND user_id = '%s' "
                   "AND (valid_until IS NULL OR valid_until > NOW()) "
                   "AND revoked_at IS NULL "
                   "ORDER BY CASE permission_level "
                   "  WHEN 'admin' THEN 1 WHEN 'write' THEN 2 "
                   "  WHEN 'read' THEN 3 WHEN 'connect' THEN 4 END "
                   "LIMIT 1",
                   uuid_to_string(branch_id), uuid_to_string(user_id));

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    char *level_str =
        SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    permission = branch_parse_permission_level(level_str);
  }

  SPI_finish();
  pfree(query.data);

  if (permission != BRANCH_PERM_NONE)
    goto cache_and_return;

  /* Check team permissions */
  initStringInfo(&query);
  appendStringInfo(
      &query,
      "SELECT bp.permission_level FROM platform.branch_permissions bp "
      "JOIN platform.team_members tm ON bp.team_id = tm.team_id "
      "WHERE bp.branch_id = '%s' AND tm.user_id = '%s' "
      "AND (bp.valid_until IS NULL OR bp.valid_until > NOW()) "
      "AND bp.revoked_at IS NULL "
      "ORDER BY CASE bp.permission_level "
      "  WHEN 'admin' THEN 1 WHEN 'write' THEN 2 "
      "  WHEN 'read' THEN 3 WHEN 'connect' THEN 4 END "
      "LIMIT 1",
      uuid_to_string(branch_id), uuid_to_string(user_id));

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    char *level_str =
        SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    permission = branch_parse_permission_level(level_str);
  }

  SPI_finish();
  pfree(query.data);

  if (permission != BRANCH_PERM_NONE)
    goto cache_and_return;

  /* Check inherited permissions from parent branch */
  if (branch->inherit_permissions && branch->parent_branch_id != NULL) {
    permission =
        resolve_inherited_permission(user_id, *branch->parent_branch_id, 0);

    if (permission != BRANCH_PERM_NONE) {
      /* Downgrade write to read for read-only protected branches */
      if (branch->protection_level == BRANCH_PROTECTION_READ_ONLY &&
          (permission == BRANCH_PERM_ADMIN ||
           permission == BRANCH_PERM_WRITE)) {
        permission = BRANCH_PERM_READ;
      }
      goto cache_and_return;
    }
  }

  /* Check project-level permissions */
  initStringInfo(&query);
  appendStringInfo(
      &query,
      "SELECT pp.permission_level FROM platform.project_permissions pp "
      "JOIN platform.clusters c ON pp.project_id = c.project_id "
      "WHERE c.cluster_id = '%s' AND pp.user_id = '%s' "
      "AND (pp.valid_until IS NULL OR pp.valid_until > NOW()) "
      "AND pp.revoked_at IS NULL "
      "ORDER BY CASE pp.permission_level "
      "  WHEN 'admin' THEN 1 WHEN 'developer' THEN 2 "
      "  WHEN 'viewer' THEN 3 END "
      "LIMIT 1",
      uuid_to_string(branch->cluster_id), uuid_to_string(user_id));

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    char *level_str =
        SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    /* Map project permission to branch permission */
    if (strcmp(level_str, "admin") == 0)
      permission = BRANCH_PERM_ADMIN;
    else if (strcmp(level_str, "developer") == 0)
      permission = BRANCH_PERM_WRITE;
    else if (strcmp(level_str, "viewer") == 0)
      permission = BRANCH_PERM_READ;
  }

  SPI_finish();
  pfree(query.data);

cache_and_return:
  /* Cache the result */
  if (BranchPermissionCache != NULL) {
    cache_entry =
        hash_search(BranchPermissionCache, cache_key.data, HASH_ENTER, &found);

    strncpy(cache_entry->cache_key, cache_key.data,
            sizeof(cache_entry->cache_key) - 1);
    cache_entry->permission = permission;
    cache_entry->cached_at = GetCurrentTimestamp();
    cache_entry->is_valid = true;
  }

  branch_free_info(branch);
  pfree(cache_key.data);
  MemoryContextSwitchTo(old_context);

  return permission;
}

/*
 * resolve_inherited_permission - Recursively resolve inherited permissions
 */
static BranchPermissionLevel resolve_inherited_permission(pg_uuid_t user_id,
                                                          pg_uuid_t branch_id,
                                                          int depth) {
  OrochiBranchInfo *branch;
  BranchPermissionLevel permission = BRANCH_PERM_NONE;
  StringInfoData query;
  int ret;

  /* Prevent infinite recursion */
  if (depth > 10) {
    elog(WARNING, "Branch inheritance depth exceeded maximum");
    return BRANCH_PERM_NONE;
  }

  /* Check direct permission on this branch */
  initStringInfo(&query);
  appendStringInfo(&query,
                   "SELECT permission_level FROM platform.branch_permissions "
                   "WHERE branch_id = '%s' AND user_id = '%s' "
                   "AND (valid_until IS NULL OR valid_until > NOW()) "
                   "AND revoked_at IS NULL "
                   "ORDER BY CASE permission_level "
                   "  WHEN 'admin' THEN 1 WHEN 'write' THEN 2 "
                   "  WHEN 'read' THEN 3 WHEN 'connect' THEN 4 END "
                   "LIMIT 1",
                   uuid_to_string(branch_id), uuid_to_string(user_id));

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    char *level_str =
        SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    permission = branch_parse_permission_level(level_str);
  }

  SPI_finish();
  pfree(query.data);

  if (permission != BRANCH_PERM_NONE)
    return permission;

  /* Check parent branch */
  branch = branch_get_info(branch_id);
  if (branch != NULL && branch->inherit_permissions &&
      branch->parent_branch_id != NULL) {
    permission = resolve_inherited_permission(
        user_id, *branch->parent_branch_id, depth + 1);
  }

  if (branch != NULL)
    branch_free_info(branch);

  return permission;
}

/*
 * branch_create_access_context - Create access context for a user/branch
 */
BranchAccessContext *branch_create_access_context(pg_uuid_t user_id,
                                                  pg_uuid_t branch_id) {
  MemoryContext old_context;
  BranchAccessContext *ctx;
  OrochiBranchInfo *branch;

  old_context = MemoryContextSwitchTo(BranchAccessContext);

  ctx = palloc0(sizeof(BranchAccessContext));
  memcpy(&ctx->user_id, &user_id, sizeof(pg_uuid_t));
  memcpy(&ctx->branch_id, &branch_id, sizeof(pg_uuid_t));
  ctx->check_time = GetCurrentTimestamp();

  ctx->effective_permission = branch_get_permission(user_id, branch_id);

  /* Check if user is owner */
  branch = branch_get_info(branch_id);
  if (branch != NULL) {
    ctx->is_owner =
        (memcmp(&branch->owner_user_id, &user_id, sizeof(pg_uuid_t)) == 0);
    branch_free_info(branch);
  }

  MemoryContextSwitchTo(old_context);

  return ctx;
}

/*
 * branch_check_permission - Check if user has required permission level
 */
bool branch_check_permission(pg_uuid_t user_id, pg_uuid_t branch_id,
                             BranchPermissionLevel required_level) {
  BranchPermissionLevel actual = branch_get_permission(user_id, branch_id);

  if (actual == BRANCH_PERM_NONE) {
    if (branch_access_shared != NULL) {
      LWLockAcquire(branch_access_shared->cache_lock, LW_EXCLUSIVE);
      branch_access_shared->access_denied++;
      LWLockRelease(branch_access_shared->cache_lock);
    }
    return false;
  }

  /* Higher permission level includes lower ones */
  return actual >= required_level;
}

/* ============================================================
 * Permission Management
 * ============================================================ */

/*
 * branch_grant_permission - Grant permission to a user
 */
bool branch_grant_permission(pg_uuid_t branch_id, pg_uuid_t grantee_user_id,
                             BranchPermissionLevel level, pg_uuid_t granted_by,
                             TimestampTz valid_until) {
  StringInfoData query;
  int ret;

  /* Verify granter has admin permission */
  if (!branch_check_permission(granted_by, branch_id, BRANCH_PERM_ADMIN)) {
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("insufficient privilege to grant branch permissions")));
    return false;
  }

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "INSERT INTO platform.branch_permissions "
      "(branch_id, user_id, permission_level, granted_by, granted_at%s) "
      "VALUES ('%s', '%s', '%s', '%s', NOW()%s) "
      "ON CONFLICT (branch_id, user_id) WHERE team_id IS NULL AND role_id IS "
      "NULL "
      "DO UPDATE SET permission_level = EXCLUDED.permission_level, "
      "granted_by = EXCLUDED.granted_by, granted_at = NOW(), "
      "revoked_at = NULL%s",
      valid_until != 0 ? ", valid_until" : "", uuid_to_string(branch_id),
      uuid_to_string(grantee_user_id), branch_permission_level_name(level),
      uuid_to_string(granted_by), valid_until != 0 ? ", to_timestamp($1)" : "",
      valid_until != 0 ? ", valid_until = EXCLUDED.valid_until" : "");

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  if (ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
    return false;

  /* Invalidate cache for this user/branch */
  branch_invalidate_permission_cache(branch_id);

  elog(LOG, "Granted %s permission on branch %s to user %s",
       branch_permission_level_name(level), uuid_to_string(branch_id),
       uuid_to_string(grantee_user_id));

  return true;
}

/*
 * branch_grant_team_permission - Grant permission to a team
 */
bool branch_grant_team_permission(pg_uuid_t branch_id, pg_uuid_t team_id,
                                  BranchPermissionLevel level,
                                  pg_uuid_t granted_by) {
  StringInfoData query;
  int ret;

  if (!branch_check_permission(granted_by, branch_id, BRANCH_PERM_ADMIN)) {
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("insufficient privilege to grant branch permissions")));
    return false;
  }

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "INSERT INTO platform.branch_permissions "
      "(branch_id, team_id, permission_level, granted_by, granted_at) "
      "VALUES ('%s', '%s', '%s', '%s', NOW()) "
      "ON CONFLICT (branch_id, team_id) WHERE user_id IS NULL AND role_id IS "
      "NULL "
      "DO UPDATE SET permission_level = EXCLUDED.permission_level, "
      "granted_by = EXCLUDED.granted_by, granted_at = NOW(), revoked_at = NULL",
      uuid_to_string(branch_id), uuid_to_string(team_id),
      branch_permission_level_name(level), uuid_to_string(granted_by));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  if (ret != SPI_OK_INSERT && ret != SPI_OK_UPDATE)
    return false;

  /* Invalidate cache */
  branch_invalidate_permission_cache(branch_id);

  return true;
}

/*
 * branch_revoke_permission - Revoke permission from a user
 */
bool branch_revoke_permission(pg_uuid_t branch_id, pg_uuid_t user_id,
                              pg_uuid_t revoked_by) {
  StringInfoData query;
  int ret;

  if (!branch_check_permission(revoked_by, branch_id, BRANCH_PERM_ADMIN)) {
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("insufficient privilege to revoke branch permissions")));
    return false;
  }

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "UPDATE platform.branch_permissions SET revoked_at = NOW() "
      "WHERE branch_id = '%s' AND user_id = '%s' AND revoked_at IS NULL",
      uuid_to_string(branch_id), uuid_to_string(user_id));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  if (ret != SPI_OK_UPDATE || SPI_processed == 0)
    return false;

  branch_invalidate_permission_cache(branch_id);

  elog(LOG, "Revoked permission on branch %s from user %s",
       uuid_to_string(branch_id), uuid_to_string(user_id));

  return true;
}

/*
 * branch_revoke_all_permissions - Revoke all permissions for a branch
 */
bool branch_revoke_all_permissions(pg_uuid_t branch_id, pg_uuid_t revoked_by) {
  StringInfoData query;
  int ret;

  if (!branch_check_permission(revoked_by, branch_id, BRANCH_PERM_ADMIN)) {
    ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("insufficient privilege to revoke branch permissions")));
    return false;
  }

  initStringInfo(&query);
  appendStringInfo(&query,
                   "UPDATE platform.branch_permissions SET revoked_at = NOW() "
                   "WHERE branch_id = '%s' AND revoked_at IS NULL",
                   uuid_to_string(branch_id));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  branch_invalidate_permission_cache(branch_id);

  return (ret == SPI_OK_UPDATE);
}

/* ============================================================
 * Branch Management
 * ============================================================ */

/*
 * branch_get_info - Retrieve branch information
 */
OrochiBranchInfo *branch_get_info(pg_uuid_t branch_id) {
  MemoryContext old_context;
  OrochiBranchInfo *info = NULL;
  StringInfoData query;
  int ret;

  old_context = MemoryContextSwitchTo(BranchAccessContext);

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "SELECT branch_id, cluster_id, parent_branch_id, name, slug, "
      "description, "
      "status, protection_level, owner_user_id, inherit_permissions, "
      "endpoint_id, created_at, updated_at "
      "FROM platform.branches WHERE branch_id = '%s'",
      uuid_to_string(branch_id));

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    bool isnull;

    info = palloc0(sizeof(OrochiBranchInfo));

    /* Parse branch_id */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 1, &isnull);
      if (!isnull)
        memcpy(&info->branch_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
    }

    /* Parse cluster_id */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 2, &isnull);
      if (!isnull)
        memcpy(&info->cluster_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
    }

    /* Parse parent_branch_id */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 3, &isnull);
      if (!isnull) {
        info->parent_branch_id = palloc(sizeof(pg_uuid_t));
        memcpy(info->parent_branch_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
      }
    }

    /* Parse name and slug */
    {
      char *name = SPI_getvalue(tuple, tupdesc, 4);
      char *slug = SPI_getvalue(tuple, tupdesc, 5);
      if (name)
        strncpy(info->name, name, BRANCH_NAME_MAX_LEN);
      if (slug)
        strncpy(info->slug, slug, BRANCH_SLUG_MAX_LEN);
    }

    /* Parse status */
    {
      char *status_str = SPI_getvalue(tuple, tupdesc, 7);
      if (status_str)
        info->status = branch_parse_status(status_str);
    }

    /* Parse protection_level */
    {
      char *prot_str = SPI_getvalue(tuple, tupdesc, 8);
      if (prot_str)
        info->protection_level = branch_parse_protection_level(prot_str);
    }

    /* Parse owner_user_id */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 9, &isnull);
      if (!isnull)
        memcpy(&info->owner_user_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
    }

    /* Parse inherit_permissions */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 10, &isnull);
      if (!isnull)
        info->inherit_permissions = DatumGetBool(d);
    }

    /* Parse timestamps */
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 12, &isnull);
      if (!isnull)
        info->created_at = DatumGetTimestampTz(d);
    }
    {
      Datum d = SPI_getbinval(tuple, tupdesc, 13, &isnull);
      if (!isnull)
        info->updated_at = DatumGetTimestampTz(d);
    }
  }

  SPI_finish();
  pfree(query.data);

  MemoryContextSwitchTo(old_context);

  return info;
}

/*
 * branch_get_by_slug - Get branch by cluster and slug
 */
OrochiBranchInfo *branch_get_by_slug(pg_uuid_t cluster_id, const char *slug) {
  MemoryContext old_context;
  OrochiBranchInfo *info = NULL;
  StringInfoData query;
  int ret;

  if (slug == NULL)
    return NULL;

  old_context = MemoryContextSwitchTo(BranchAccessContext);

  initStringInfo(&query);
  appendStringInfo(&query,
                   "SELECT branch_id FROM platform.branches "
                   "WHERE cluster_id = '%s' AND slug = '%s'",
                   uuid_to_string(cluster_id), slug);

  SPI_connect();
  ret = SPI_execute(query.data, true, 1);

  if (ret == SPI_OK_SELECT && SPI_processed > 0) {
    bool isnull;
    Datum d =
        SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull) {
      pg_uuid_t branch_id;
      memcpy(&branch_id, DatumGetUUIDP(d), sizeof(pg_uuid_t));
      SPI_finish();
      pfree(query.data);
      info = branch_get_info(branch_id);
      MemoryContextSwitchTo(old_context);
      return info;
    }
  }

  SPI_finish();
  pfree(query.data);

  MemoryContextSwitchTo(old_context);

  return NULL;
}

/*
 * branch_update_status - Update branch status
 */
bool branch_update_status(pg_uuid_t branch_id, BranchStatus new_status) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "UPDATE platform.branches SET status = '%s', updated_at = NOW() "
      "WHERE branch_id = '%s'",
      branch_status_name(new_status), uuid_to_string(branch_id));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  return (ret == SPI_OK_UPDATE && SPI_processed > 0);
}

/*
 * branch_update_protection - Update branch protection level
 */
bool branch_update_protection(pg_uuid_t branch_id,
                              BranchProtectionLevel level) {
  StringInfoData query;
  int ret;

  initStringInfo(&query);
  appendStringInfo(&query,
                   "UPDATE platform.branches SET protection_level = '%s', "
                   "updated_at = NOW() "
                   "WHERE branch_id = '%s'",
                   branch_protection_level_name(level),
                   uuid_to_string(branch_id));

  SPI_connect();
  ret = SPI_execute(query.data, false, 0);
  SPI_finish();

  pfree(query.data);

  /* Invalidate permission cache since protection affects effective permissions
   */
  branch_invalidate_permission_cache(branch_id);

  return (ret == SPI_OK_UPDATE && SPI_processed > 0);
}

/* ============================================================
 * Cross-Branch Operations
 * ============================================================ */

/*
 * branch_validate_cross_branch_access - Validate cross-branch operation
 */
bool branch_validate_cross_branch_access(pg_uuid_t user_id,
                                         pg_uuid_t source_branch_id,
                                         pg_uuid_t target_branch_id,
                                         CrossBranchOperation operation) {
  BranchPermissionLevel source_perm;
  BranchPermissionLevel target_perm;

  source_perm = branch_get_permission(user_id, source_branch_id);
  target_perm = branch_get_permission(user_id, target_branch_id);

  if (source_perm == BRANCH_PERM_NONE || target_perm == BRANCH_PERM_NONE)
    return false;

  switch (operation) {
  case CROSS_BRANCH_COPY:
    /* Need read on source, write on target */
    return (source_perm >= BRANCH_PERM_READ &&
            target_perm >= BRANCH_PERM_WRITE);

  case CROSS_BRANCH_COMPARE:
    /* Need read on both */
    return (source_perm >= BRANCH_PERM_READ && target_perm >= BRANCH_PERM_READ);

  case CROSS_BRANCH_MERGE:
    /* Need write on both */
    return (source_perm >= BRANCH_PERM_WRITE &&
            target_perm >= BRANCH_PERM_WRITE);

  default:
    return false;
  }
}

/* ============================================================
 * Caching
 * ============================================================ */

/*
 * branch_invalidate_permission_cache - Invalidate cache entries for a branch
 */
void branch_invalidate_permission_cache(pg_uuid_t branch_id) {
  HASH_SEQ_STATUS status;
  BranchPermissionCacheEntry *entry;
  char branch_str[64];

  if (BranchPermissionCache == NULL)
    return;

  snprintf(branch_str, sizeof(branch_str), ":%s", uuid_to_string(branch_id));

  hash_seq_init(&status, BranchPermissionCache);
  while ((entry = hash_seq_search(&status)) != NULL) {
    if (strstr(entry->cache_key, branch_str) != NULL) {
      entry->is_valid = false;
    }
  }
}

/*
 * branch_invalidate_user_cache - Invalidate cache entries for a user
 */
void branch_invalidate_user_cache(pg_uuid_t user_id) {
  HASH_SEQ_STATUS status;
  BranchPermissionCacheEntry *entry;
  char user_str[64];

  if (BranchPermissionCache == NULL)
    return;

  snprintf(user_str, sizeof(user_str), "%s:", uuid_to_string(user_id));

  hash_seq_init(&status, BranchPermissionCache);
  while ((entry = hash_seq_search(&status)) != NULL) {
    if (strncmp(entry->cache_key, user_str, strlen(user_str)) == 0) {
      entry->is_valid = false;
    }
  }
}

/*
 * branch_clear_permission_cache - Clear entire permission cache
 */
void branch_clear_permission_cache(void) {
  HASH_SEQ_STATUS status;
  BranchPermissionCacheEntry *entry;

  if (BranchPermissionCache == NULL)
    return;

  hash_seq_init(&status, BranchPermissionCache);
  while ((entry = hash_seq_search(&status)) != NULL) {
    entry->is_valid = false;
  }
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * branch_permission_level_name - Get string name for permission level
 */
const char *branch_permission_level_name(BranchPermissionLevel level) {
  switch (level) {
  case BRANCH_PERM_NONE:
    return "none";
  case BRANCH_PERM_CONNECT:
    return "connect";
  case BRANCH_PERM_READ:
    return "read";
  case BRANCH_PERM_WRITE:
    return "write";
  case BRANCH_PERM_ADMIN:
    return "admin";
  default:
    return "unknown";
  }
}

/*
 * branch_status_name - Get string name for branch status
 */
const char *branch_status_name(BranchStatus status) {
  switch (status) {
  case BRANCH_STATUS_CREATING:
    return "creating";
  case BRANCH_STATUS_READY:
    return "ready";
  case BRANCH_STATUS_SUSPENDED:
    return "suspended";
  case BRANCH_STATUS_DELETING:
    return "deleting";
  case BRANCH_STATUS_DELETED:
    return "deleted";
  case BRANCH_STATUS_FAILED:
    return "failed";
  default:
    return "unknown";
  }
}

/*
 * branch_protection_level_name - Get string name for protection level
 */
const char *branch_protection_level_name(BranchProtectionLevel level) {
  switch (level) {
  case BRANCH_PROTECTION_NONE:
    return "none";
  case BRANCH_PROTECTION_REQUIRE_APPROVAL:
    return "require_approval";
  case BRANCH_PROTECTION_READ_ONLY:
    return "read_only";
  case BRANCH_PROTECTION_LOCKED:
    return "locked";
  default:
    return "unknown";
  }
}

/*
 * branch_protection_rule_type_name - Get string name for rule type
 */
const char *branch_protection_rule_type_name(BranchProtectionRuleType type) {
  switch (type) {
  case BRANCH_RULE_REQUIRE_APPROVAL:
    return "require_approval";
  case BRANCH_RULE_RESTRICT_DDL:
    return "restrict_ddl";
  case BRANCH_RULE_RESTRICT_DELETE:
    return "restrict_delete";
  case BRANCH_RULE_REQUIRE_MFA:
    return "require_mfa";
  case BRANCH_RULE_AUDIT_ALL_QUERIES:
    return "audit_all_queries";
  case BRANCH_RULE_TIME_WINDOW:
    return "time_window";
  case BRANCH_RULE_IP_ALLOWLIST:
    return "ip_allowlist";
  default:
    return "unknown";
  }
}

/*
 * branch_cow_state_name - Get string name for CoW state
 */
const char *branch_cow_state_name(BranchCowState state) {
  switch (state) {
  case BRANCH_COW_SHARED:
    return "shared";
  case BRANCH_COW_DIVERGING:
    return "diverging";
  case BRANCH_COW_INDEPENDENT:
    return "independent";
  default:
    return "unknown";
  }
}

/*
 * branch_parse_permission_level - Parse permission level from string
 */
BranchPermissionLevel branch_parse_permission_level(const char *str) {
  if (str == NULL)
    return BRANCH_PERM_NONE;

  if (strcmp(str, "admin") == 0)
    return BRANCH_PERM_ADMIN;
  if (strcmp(str, "write") == 0)
    return BRANCH_PERM_WRITE;
  if (strcmp(str, "read") == 0)
    return BRANCH_PERM_READ;
  if (strcmp(str, "connect") == 0)
    return BRANCH_PERM_CONNECT;

  return BRANCH_PERM_NONE;
}

/*
 * branch_parse_status - Parse branch status from string
 */
BranchStatus branch_parse_status(const char *str) {
  if (str == NULL)
    return BRANCH_STATUS_CREATING;

  if (strcmp(str, "ready") == 0)
    return BRANCH_STATUS_READY;
  if (strcmp(str, "suspended") == 0)
    return BRANCH_STATUS_SUSPENDED;
  if (strcmp(str, "deleting") == 0)
    return BRANCH_STATUS_DELETING;
  if (strcmp(str, "deleted") == 0)
    return BRANCH_STATUS_DELETED;
  if (strcmp(str, "failed") == 0)
    return BRANCH_STATUS_FAILED;

  return BRANCH_STATUS_CREATING;
}

/*
 * branch_parse_protection_level - Parse protection level from string
 */
BranchProtectionLevel branch_parse_protection_level(const char *str) {
  if (str == NULL)
    return BRANCH_PROTECTION_NONE;

  if (strcmp(str, "require_approval") == 0)
    return BRANCH_PROTECTION_REQUIRE_APPROVAL;
  if (strcmp(str, "read_only") == 0)
    return BRANCH_PROTECTION_READ_ONLY;
  if (strcmp(str, "locked") == 0)
    return BRANCH_PROTECTION_LOCKED;

  return BRANCH_PROTECTION_NONE;
}

/*
 * branch_free_info - Free branch info structure
 */
void branch_free_info(OrochiBranchInfo *info) {
  if (info == NULL)
    return;

  if (info->parent_branch_id)
    pfree(info->parent_branch_id);
  if (info->description)
    pfree(info->description);
  if (info->endpoint_id)
    pfree(info->endpoint_id);
  if (info->connection_string)
    pfree(info->connection_string);
  if (info->auto_delete_after)
    pfree(info->auto_delete_after);

  pfree(info);
}

/*
 * branch_free_permission - Free permission structure
 */
void branch_free_permission(OrochiBranchPermission *perm) {
  if (perm == NULL)
    return;

  if (perm->user_id)
    pfree(perm->user_id);
  if (perm->team_id)
    pfree(perm->team_id);
  if (perm->role_id)
    pfree(perm->role_id);
  if (perm->ip_restrictions)
    pfree(perm->ip_restrictions);
  if (perm->time_restrictions)
    pfree(perm->time_restrictions);

  pfree(perm);
}

/*
 * branch_free_access_context - Free access context structure
 */
void branch_free_access_context(BranchAccessContext *ctx) {
  if (ctx == NULL)
    return;

  if (ctx->ip_address)
    pfree(ctx->ip_address);

  pfree(ctx);
}

/* ============================================================
 * Static Helper Functions
 * ============================================================ */

static char *uuid_to_string(pg_uuid_t uuid) {
  char *str = palloc(37);

  snprintf(
      str, 37,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3], uuid.data[4],
      uuid.data[5], uuid.data[6], uuid.data[7], uuid.data[8], uuid.data[9],
      uuid.data[10], uuid.data[11], uuid.data[12], uuid.data[13], uuid.data[14],
      uuid.data[15]);

  return str;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(branch_check_access_sql);
PG_FUNCTION_INFO_V1(branch_grant_access_sql);
PG_FUNCTION_INFO_V1(branch_revoke_access_sql);
PG_FUNCTION_INFO_V1(branch_validate_operation_sql);

/*
 * branch_check_access_sql - Check if user has access to branch
 */
Datum branch_check_access_sql(PG_FUNCTION_ARGS) {
  pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
  pg_uuid_t *branch_id = PG_GETARG_UUID_P(1);
  text *required_level_text = PG_GETARG_TEXT_PP(2);

  BranchPermissionLevel required;
  bool has_access;

  required =
      branch_parse_permission_level(text_to_cstring(required_level_text));
  has_access = branch_check_permission(*user_id, *branch_id, required);

  PG_RETURN_BOOL(has_access);
}

/*
 * branch_grant_access_sql - Grant access to a branch
 */
Datum branch_grant_access_sql(PG_FUNCTION_ARGS) {
  pg_uuid_t *branch_id = PG_GETARG_UUID_P(0);
  pg_uuid_t *grantee_id = PG_GETARG_UUID_P(1);
  text *level_text = PG_GETARG_TEXT_PP(2);
  pg_uuid_t *granted_by = PG_GETARG_UUID_P(3);

  BranchPermissionLevel level;
  bool success;

  level = branch_parse_permission_level(text_to_cstring(level_text));
  success =
      branch_grant_permission(*branch_id, *grantee_id, level, *granted_by, 0);

  PG_RETURN_BOOL(success);
}

/*
 * branch_revoke_access_sql - Revoke access from a branch
 */
Datum branch_revoke_access_sql(PG_FUNCTION_ARGS) {
  pg_uuid_t *branch_id = PG_GETARG_UUID_P(0);
  pg_uuid_t *user_id = PG_GETARG_UUID_P(1);
  pg_uuid_t *revoked_by = PG_GETARG_UUID_P(2);

  bool success = branch_revoke_permission(*branch_id, *user_id, *revoked_by);

  PG_RETURN_BOOL(success);
}

/*
 * branch_validate_operation_sql - Validate cross-branch operation
 */
Datum branch_validate_operation_sql(PG_FUNCTION_ARGS) {
  pg_uuid_t *user_id = PG_GETARG_UUID_P(0);
  pg_uuid_t *source_branch_id = PG_GETARG_UUID_P(1);
  pg_uuid_t *target_branch_id = PG_GETARG_UUID_P(2);
  text *operation_text = PG_GETARG_TEXT_PP(3);

  CrossBranchOperation op;
  bool valid;
  char *op_str = text_to_cstring(operation_text);

  if (strcmp(op_str, "copy") == 0)
    op = CROSS_BRANCH_COPY;
  else if (strcmp(op_str, "compare") == 0)
    op = CROSS_BRANCH_COMPARE;
  else if (strcmp(op_str, "merge") == 0)
    op = CROSS_BRANCH_MERGE;
  else {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid operation: %s", op_str)));
  }

  valid = branch_validate_cross_branch_access(*user_id, *source_branch_id,
                                              *target_branch_id, op);

  PG_RETURN_BOOL(valid);
}
