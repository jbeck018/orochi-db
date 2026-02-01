/*-------------------------------------------------------------------------
 *
 * role_mapping.h
 *    Orochi DB Database Roles Mapped to Application Users
 *
 * This module implements dynamic role management for mapping application
 * users to PostgreSQL database roles:
 *   - Role template system for consistent permission patterns
 *   - Dynamic role creation and privilege assignment
 *   - High-performance role caching for 100K+ roles
 *   - Role lifecycle management (create, sync, cleanup)
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_ROLE_MAPPING_H
#define OROCHI_ROLE_MAPPING_H

#include "branch_access.h"
#include "postgres.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* ============================================================
 * Constants
 * ============================================================ */

/* Role naming convention */
#define ROLE_PREFIX_USER "orochi_u_"
#define ROLE_PREFIX_TEAM "orochi_t_"
#define ROLE_PREFIX_SERVICE "orochi_s_"
#define ROLE_PREFIX_BRANCH "orochi_b_"
#define ROLE_HASH_LEN 8

/* Role limits */
#define ROLE_NAME_MAX_LEN 63
#define ROLE_TEMPLATE_MAX_LEN 63
#define ROLE_PRIVILEGE_MAX_LEN 4096

/* Cache configuration - optimized for 100K+ roles */
#define ROLE_CACHE_SIZE 100000
#define ROLE_CACHE_TTL_SEC 300
#define ROLE_BATCH_SIZE 1000

/* ============================================================
 * Enumerations
 * ============================================================ */

/*
 * Identity type for role mapping
 */
typedef enum RoleIdentityType {
  ROLE_IDENTITY_USER = 0,
  ROLE_IDENTITY_TEAM,
  ROLE_IDENTITY_SERVICE
} RoleIdentityType;

/*
 * Role mapping status
 */
typedef enum RoleMappingStatus {
  ROLE_MAPPING_PENDING = 0,
  ROLE_MAPPING_ACTIVE,
  ROLE_MAPPING_SYNCING,
  ROLE_MAPPING_ERROR,
  ROLE_MAPPING_SUSPENDED,
  ROLE_MAPPING_DELETED
} RoleMappingStatus;

/*
 * Role sync operation
 */
typedef enum RoleSyncOperation {
  ROLE_SYNC_CREATE = 0,
  ROLE_SYNC_UPDATE_PRIVILEGES,
  ROLE_SYNC_SUSPEND,
  ROLE_SYNC_DELETE
} RoleSyncOperation;

/* ============================================================
 * Core Data Structures
 * ============================================================ */

/*
 * OrochiRoleTemplate - Template for role permissions
 */
typedef struct OrochiRoleTemplate {
  pg_uuid_t template_id;
  pg_uuid_t *org_id; /* NULL for system templates */

  /* Template identification */
  char template_name[ROLE_TEMPLATE_MAX_LEN + 1];
  char *display_name;
  char *description;

  /* Base privileges (applied to all schemas) */
  char *base_privileges; /* JSON */

  /* Schema-specific privileges */
  char *schema_privileges; /* JSON */

  /* Table-level privileges (patterns) */
  char *table_privileges; /* JSON */

  /* Function/procedure privileges */
  char *function_privileges; /* JSON */

  /* Sequence privileges */
  char *sequence_privileges; /* JSON */

  /* Row-level security policies */
  char *rls_policies; /* JSON */

  /* PostgreSQL role options */
  bool can_login;
  bool can_createdb;
  bool can_createrole;
  bool inherit;
  bool can_replication;
  bool bypass_rls;
  int connection_limit;

  /* Status */
  bool is_system;
  bool is_active;

  TimestampTz created_at;
  TimestampTz updated_at;
} OrochiRoleTemplate;

/*
 * OrochiRoleMapping - Mapping between identity and database role
 */
typedef struct OrochiRoleMapping {
  pg_uuid_t mapping_id;

  /* Source identity (one of these is set) */
  pg_uuid_t *user_id;
  pg_uuid_t *team_id;
  pg_uuid_t *service_account_id;

  /* Target */
  pg_uuid_t cluster_id;
  pg_uuid_t *branch_id; /* NULL for cluster-wide */

  /* Role configuration */
  pg_uuid_t *template_id;
  char *custom_privileges; /* JSON - override template */

  /* Generated PostgreSQL role */
  char pg_role_name[ROLE_NAME_MAX_LEN + 1];
  bool pg_role_created;

  /* Status */
  RoleMappingStatus status;
  TimestampTz last_sync_at;
  char *sync_error;

  TimestampTz created_at;
  TimestampTz updated_at;
} OrochiRoleMapping;

/*
 * RoleCacheEntry - Cached role lookup for high performance
 */
typedef struct RoleCacheEntry {
  char cache_key[256]; /* identity:cluster:branch */
  char pg_role_name[ROLE_NAME_MAX_LEN + 1];
  char *permissions; /* JSON */
  BranchPermissionLevel branch_permission;
  TimestampTz cached_at;
  bool is_valid;
} RoleCacheEntry;

/*
 * RoleSyncJob - Batch role synchronization job
 */
typedef struct RoleSyncJob {
  pg_uuid_t job_id;
  pg_uuid_t cluster_id;
  RoleSyncOperation operation;
  pg_uuid_t *mapping_ids;
  int mapping_count;
  TimestampTz created_at;
  TimestampTz started_at;
  TimestampTz completed_at;
  int success_count;
  int failure_count;
  char *error_log;
} RoleSyncJob;

/*
 * RoleStatistics - Statistics for monitoring
 */
typedef struct RoleStatistics {
  pg_uuid_t cluster_id;
  TimestampTz stat_time;
  int total_roles;
  int active_roles;
  int pending_roles;
  int error_roles;
  double avg_create_time_ms;
  double avg_sync_time_ms;
  int64 pg_roles_table_size;
} RoleStatistics;

/* ============================================================
 * Function Declarations
 * ============================================================ */

/* Initialization */
extern void role_mapping_init(void);
extern Size role_mapping_shmem_size(void);

/* Role name generation */
extern char *role_generate_name(RoleIdentityType type, pg_uuid_t identity_id);
extern char *role_generate_branch_name(pg_uuid_t branch_id);
extern bool role_parse_name(const char *role_name, RoleIdentityType *type,
                            char *hash_out);

/* Template management */
extern OrochiRoleTemplate *role_get_template(pg_uuid_t template_id);
extern OrochiRoleTemplate *role_get_template_by_name(pg_uuid_t *org_id,
                                                     const char *template_name);

extern List *role_get_system_templates(void);
extern List *role_get_org_templates(pg_uuid_t org_id);

extern pg_uuid_t role_create_template(pg_uuid_t *org_id,
                                      const char *template_name,
                                      const char *display_name,
                                      const char *base_privileges);

extern bool role_update_template(pg_uuid_t template_id,
                                 const char *base_privileges,
                                 const char *schema_privileges,
                                 const char *table_privileges);

extern bool role_delete_template(pg_uuid_t template_id);

/* Role mapping management */
extern OrochiRoleMapping *role_get_mapping(pg_uuid_t mapping_id);
extern OrochiRoleMapping *role_get_mapping_for_user(pg_uuid_t user_id,
                                                    pg_uuid_t cluster_id,
                                                    pg_uuid_t *branch_id);

extern OrochiRoleMapping *role_get_mapping_for_team(pg_uuid_t team_id,
                                                    pg_uuid_t cluster_id);

extern pg_uuid_t role_create_mapping(pg_uuid_t user_id, pg_uuid_t cluster_id,
                                     pg_uuid_t *branch_id,
                                     pg_uuid_t template_id,
                                     const char *custom_privileges);

extern pg_uuid_t role_create_team_mapping(pg_uuid_t team_id,
                                          pg_uuid_t cluster_id,
                                          pg_uuid_t template_id);

extern pg_uuid_t role_create_service_mapping(pg_uuid_t service_account_id,
                                             pg_uuid_t cluster_id,
                                             pg_uuid_t template_id);

extern bool role_update_mapping(pg_uuid_t mapping_id, pg_uuid_t *template_id,
                                const char *custom_privileges);

extern bool role_delete_mapping(pg_uuid_t mapping_id);

/* Database role operations */
extern bool role_create_db_role(pg_uuid_t mapping_id);
extern bool role_apply_privileges(pg_uuid_t mapping_id);
extern bool role_revoke_privileges(pg_uuid_t mapping_id);
extern bool role_drop_db_role(pg_uuid_t mapping_id);

extern bool role_sync_to_cluster(pg_uuid_t mapping_id);
extern bool role_sync_all_cluster_roles(pg_uuid_t cluster_id);

/* Batch operations for performance */
extern int role_batch_create(pg_uuid_t cluster_id, pg_uuid_t *user_ids,
                             int count);
extern int role_batch_sync(pg_uuid_t cluster_id, pg_uuid_t *mapping_ids,
                           int count);
extern int role_batch_delete(pg_uuid_t cluster_id, pg_uuid_t *mapping_ids,
                             int count);

/* Role lookup (high-performance) */
extern char *role_lookup_for_connection(pg_uuid_t user_id, pg_uuid_t cluster_id,
                                        pg_uuid_t *branch_id);

extern BranchPermissionLevel
role_get_permission_for_branch(pg_uuid_t user_id, pg_uuid_t branch_id);

/* User lifecycle */
extern int role_cleanup_user_roles(pg_uuid_t user_id);
extern int role_suspend_user_roles(pg_uuid_t user_id);
extern int role_reactivate_user_roles(pg_uuid_t user_id);

/* Statistics */
extern RoleStatistics *role_get_statistics(pg_uuid_t cluster_id);
extern void role_update_statistics(pg_uuid_t cluster_id);

/* Caching */
extern void role_cache_mapping(const char *cache_key,
                               OrochiRoleMapping *mapping);
extern OrochiRoleMapping *role_get_cached_mapping(const char *cache_key);
extern void role_invalidate_cache(pg_uuid_t cluster_id);
extern void role_invalidate_user_cache(pg_uuid_t user_id);
extern void role_clear_cache(void);

/* Utility functions */
extern const char *role_identity_type_name(RoleIdentityType type);
extern const char *role_mapping_status_name(RoleMappingStatus status);
extern const char *role_sync_operation_name(RoleSyncOperation op);

extern RoleIdentityType role_parse_identity_type(const char *str);
extern RoleMappingStatus role_parse_mapping_status(const char *str);

/* Cleanup */
extern void role_free_template(OrochiRoleTemplate *tmpl);
extern void role_free_mapping(OrochiRoleMapping *mapping);
extern void role_free_statistics(RoleStatistics *stats);

/* SQL-callable functions */
extern Datum role_create_mapping_sql(PG_FUNCTION_ARGS);
extern Datum role_sync_mapping_sql(PG_FUNCTION_ARGS);
extern Datum role_lookup_sql(PG_FUNCTION_ARGS);
extern Datum role_batch_create_sql(PG_FUNCTION_ARGS);
extern Datum role_get_statistics_sql(PG_FUNCTION_ARGS);

#endif /* OROCHI_ROLE_MAPPING_H */
