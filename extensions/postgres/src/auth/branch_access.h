/*-------------------------------------------------------------------------
 *
 * branch_access.h
 *    Orochi DB Branch-Level Access Control
 *
 * This module implements fine-grained access control for database branches:
 *   - Branch permission levels (admin, write, read, connect)
 *   - Permission inheritance from parent branches
 *   - Protection rules (require approval, restrict DDL, etc.)
 *   - Cross-branch operation validation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_BRANCH_ACCESS_H
#define OROCHI_BRANCH_ACCESS_H

#include "postgres.h"
#include "utils/inet.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define BRANCH_NAME_MAX_LEN 63
#define BRANCH_SLUG_MAX_LEN 63
#define BRANCH_DESCRIPTION_MAX_LEN 1024

/* Cache configuration */
#define BRANCH_PERM_CACHE_SIZE 10000
#define BRANCH_PERM_CACHE_TTL_SEC 60

/* ============================================================
 * Enumerations
 * ============================================================ */

/*
 * Branch permission level
 */
typedef enum BranchPermissionLevel {
  BRANCH_PERM_NONE = 0, /* No access */
  BRANCH_PERM_CONNECT,  /* Can connect but no table access */
  BRANCH_PERM_READ,     /* Read only: SELECT */
  BRANCH_PERM_WRITE,    /* Read + Write: SELECT, INSERT, UPDATE, DELETE */
  BRANCH_PERM_ADMIN     /* Full control: DDL, DML, grant permissions */
} BranchPermissionLevel;

/*
 * Branch status
 */
typedef enum BranchStatus {
  BRANCH_STATUS_CREATING = 0,
  BRANCH_STATUS_READY,
  BRANCH_STATUS_SUSPENDED,
  BRANCH_STATUS_DELETING,
  BRANCH_STATUS_DELETED,
  BRANCH_STATUS_FAILED
} BranchStatus;

/*
 * Branch protection level
 */
typedef enum BranchProtectionLevel {
  BRANCH_PROTECTION_NONE = 0,         /* No protection */
  BRANCH_PROTECTION_REQUIRE_APPROVAL, /* Require approval for writes */
  BRANCH_PROTECTION_READ_ONLY,        /* No writes allowed */
  BRANCH_PROTECTION_LOCKED            /* No access */
} BranchProtectionLevel;

/*
 * Protection rule type
 */
typedef enum BranchProtectionRuleType {
  BRANCH_RULE_REQUIRE_APPROVAL = 0,
  BRANCH_RULE_RESTRICT_DDL,
  BRANCH_RULE_RESTRICT_DELETE,
  BRANCH_RULE_REQUIRE_MFA,
  BRANCH_RULE_AUDIT_ALL_QUERIES,
  BRANCH_RULE_TIME_WINDOW,
  BRANCH_RULE_IP_ALLOWLIST
} BranchProtectionRuleType;

/*
 * Cross-branch operation type
 */
typedef enum CrossBranchOperation {
  CROSS_BRANCH_COPY = 0,
  CROSS_BRANCH_COMPARE,
  CROSS_BRANCH_MERGE
} CrossBranchOperation;

/*
 * Copy-on-write state for branch shards
 */
typedef enum BranchCowState {
  BRANCH_COW_SHARED = 0, /* Sharing pages with parent */
  BRANCH_COW_DIVERGING,  /* Some pages copied */
  BRANCH_COW_INDEPENDENT /* Fully independent */
} BranchCowState;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiBranchInfo - Branch metadata
 */
typedef struct OrochiBranchInfo {
  pg_uuid_t branch_id;
  pg_uuid_t cluster_id;
  pg_uuid_t *parent_branch_id; /* NULL if root */

  /* Identification */
  char name[BRANCH_NAME_MAX_LEN + 1];
  char slug[BRANCH_SLUG_MAX_LEN + 1];
  char *description;

  /* State */
  BranchStatus status;
  BranchProtectionLevel protection_level;

  /* Copy-on-Write state */
  uint64 parent_lsn; /* LSN at branch creation */
  TimestampTz parent_timestamp;
  TimestampTz diverged_at; /* When first write occurred */

  /* Access control */
  pg_uuid_t owner_user_id;
  bool inherit_permissions;

  /* Compute endpoint */
  pg_uuid_t *endpoint_id;
  char *connection_string; /* Encrypted */

  /* Lifecycle */
  Interval *auto_delete_after;
  TimestampTz created_at;
  TimestampTz updated_at;
  TimestampTz deleted_at;
} OrochiBranchInfo;

/*
 * OrochiBranchPermission - Permission grant for a branch
 */
typedef struct OrochiBranchPermission {
  pg_uuid_t permission_id;
  pg_uuid_t branch_id;

  /* Grantee (one of these is set) */
  pg_uuid_t *user_id;
  pg_uuid_t *team_id;
  pg_uuid_t *role_id;

  /* Permission */
  BranchPermissionLevel permission_level;

  /* Constraints */
  TimestampTz valid_until;
  inet **ip_restrictions; /* Array of allowed IPs */
  int ip_restriction_count;
  char *time_restrictions; /* JSON config */

  /* Metadata */
  pg_uuid_t granted_by;
  TimestampTz granted_at;
  TimestampTz revoked_at;
} OrochiBranchPermission;

/*
 * OrochiBranchProtectionRule - Protection rule for a branch
 */
typedef struct OrochiBranchProtectionRule {
  pg_uuid_t rule_id;
  pg_uuid_t branch_id;
  BranchProtectionRuleType rule_type;
  char *rule_config; /* JSON configuration */
  bool enabled;
  TimestampTz created_at;
} OrochiBranchProtectionRule;

/*
 * OrochiBranchShardMapping - Branch to shard mapping for isolation
 */
typedef struct OrochiBranchShardMapping {
  pg_uuid_t mapping_id;
  pg_uuid_t branch_id;
  int64 shard_id;
  Oid table_oid;
  BranchCowState cow_state;
  int64 base_shard_id; /* Original shard for CoW */
  int32 node_id;
  char *storage_path;
  TimestampTz created_at;
} OrochiBranchShardMapping;

/*
 * BranchPermissionCacheEntry - Cached permission lookup
 */
typedef struct BranchPermissionCacheEntry {
  char cache_key[128]; /* user_id:branch_id */
  BranchPermissionLevel permission;
  TimestampTz cached_at;
  bool is_valid;
} BranchPermissionCacheEntry;

/*
 * BranchAccessContext - Context for branch access checks
 */
typedef struct BranchAccessContext {
  pg_uuid_t user_id;
  pg_uuid_t branch_id;
  BranchPermissionLevel effective_permission;
  bool is_owner;
  bool permission_inherited;
  char *ip_address;
  TimestampTz check_time;
} BranchAccessContext;

/* ============================================================
 * Function Declarations
 * ============================================================ */

/* Initialization */
extern void branch_access_init(void);
extern Size branch_access_shmem_size(void);

/* Permission resolution */
extern BranchPermissionLevel branch_get_permission(pg_uuid_t user_id,
                                                   pg_uuid_t branch_id);

extern BranchPermissionLevel
branch_get_effective_permission(pg_uuid_t user_id, pg_uuid_t branch_id,
                                const char *ip_address);

extern BranchAccessContext *branch_create_access_context(pg_uuid_t user_id,
                                                         pg_uuid_t branch_id);

extern bool branch_check_permission(pg_uuid_t user_id, pg_uuid_t branch_id,
                                    BranchPermissionLevel required_level);

/* Permission management */
extern bool branch_grant_permission(pg_uuid_t branch_id,
                                    pg_uuid_t grantee_user_id,
                                    BranchPermissionLevel level,
                                    pg_uuid_t granted_by,
                                    TimestampTz valid_until);

extern bool branch_grant_team_permission(pg_uuid_t branch_id, pg_uuid_t team_id,
                                         BranchPermissionLevel level,
                                         pg_uuid_t granted_by);

extern bool branch_revoke_permission(pg_uuid_t branch_id, pg_uuid_t user_id,
                                     pg_uuid_t revoked_by);

extern bool branch_revoke_all_permissions(pg_uuid_t branch_id,
                                          pg_uuid_t revoked_by);

/* Branch management */
extern OrochiBranchInfo *branch_get_info(pg_uuid_t branch_id);
extern OrochiBranchInfo *branch_get_by_slug(pg_uuid_t cluster_id,
                                            const char *slug);

extern pg_uuid_t branch_create(pg_uuid_t cluster_id,
                               pg_uuid_t *parent_branch_id, const char *name,
                               const char *slug, pg_uuid_t owner_user_id,
                               bool inherit_permissions);

extern bool branch_delete(pg_uuid_t branch_id, pg_uuid_t deleted_by);

extern bool branch_update_status(pg_uuid_t branch_id, BranchStatus new_status);

extern bool branch_update_protection(pg_uuid_t branch_id,
                                     BranchProtectionLevel level);

/* Protection rules */
extern bool branch_add_protection_rule(pg_uuid_t branch_id,
                                       BranchProtectionRuleType rule_type,
                                       const char *rule_config);

extern bool branch_remove_protection_rule(pg_uuid_t branch_id,
                                          BranchProtectionRuleType rule_type);

extern bool branch_check_protection_rule(pg_uuid_t branch_id,
                                         BranchProtectionRuleType rule_type,
                                         const char *context);

extern List *branch_get_protection_rules(pg_uuid_t branch_id);

/* Cross-branch operations */
extern bool branch_validate_cross_branch_access(pg_uuid_t user_id,
                                                pg_uuid_t source_branch_id,
                                                pg_uuid_t target_branch_id,
                                                CrossBranchOperation operation);

/* Inheritance */
extern bool branch_get_inherit_permissions(pg_uuid_t branch_id);
extern bool branch_set_inherit_permissions(pg_uuid_t branch_id, bool inherit);

extern pg_uuid_t *branch_get_parent(pg_uuid_t branch_id);
extern List *branch_get_children(pg_uuid_t branch_id);
extern List *branch_get_ancestors(pg_uuid_t branch_id);

/* Shard isolation */
extern OrochiBranchShardMapping *
branch_get_shard_mapping(pg_uuid_t branch_id, Oid table_oid, int32 hash_value);

extern bool branch_register_shard_mapping(pg_uuid_t branch_id, int64 shard_id,
                                          Oid table_oid, int64 base_shard_id);

extern bool branch_update_cow_state(pg_uuid_t branch_id, int64 shard_id,
                                    BranchCowState new_state);

/* Caching */
extern void branch_invalidate_permission_cache(pg_uuid_t branch_id);
extern void branch_invalidate_user_cache(pg_uuid_t user_id);
extern void branch_clear_permission_cache(void);

/* Utility functions */
extern const char *branch_permission_level_name(BranchPermissionLevel level);
extern const char *branch_status_name(BranchStatus status);
extern const char *branch_protection_level_name(BranchProtectionLevel level);
extern const char *
branch_protection_rule_type_name(BranchProtectionRuleType type);
extern const char *branch_cow_state_name(BranchCowState state);

extern BranchPermissionLevel branch_parse_permission_level(const char *str);
extern BranchStatus branch_parse_status(const char *str);
extern BranchProtectionLevel branch_parse_protection_level(const char *str);

/* Cleanup */
extern void branch_free_info(OrochiBranchInfo *info);
extern void branch_free_permission(OrochiBranchPermission *perm);
extern void branch_free_access_context(BranchAccessContext *ctx);

/* SQL-callable functions */
extern Datum branch_check_access_sql(PG_FUNCTION_ARGS);
extern Datum branch_grant_access_sql(PG_FUNCTION_ARGS);
extern Datum branch_revoke_access_sql(PG_FUNCTION_ARGS);
extern Datum branch_list_permissions_sql(PG_FUNCTION_ARGS);
extern Datum branch_validate_operation_sql(PG_FUNCTION_ARGS);

#endif /* OROCHI_BRANCH_ACCESS_H */
