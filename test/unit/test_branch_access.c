/*-------------------------------------------------------------------------
 *
 * test_branch_access.c
 *    Unit tests for Orochi DB branch-level access control
 *
 * Tests covered:
 *   - Branch permission levels (admin, write, read, connect)
 *   - Permission inheritance from parent branches
 *   - Team-based permissions
 *   - Branch protection rules
 *   - Cross-branch access validation
 *   - Permission resolution hierarchy
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Constants and Types
 * ============================================================ */

#define MAX_BRANCHES        50
#define MAX_PERMISSIONS     100
#define MAX_TEAMS           20
#define MAX_TEAM_MEMBERS    50
#define MAX_PROTECTION_RULES 50
#define UUID_LEN            36

/* Branch permission levels */
typedef enum MockBranchPermission
{
    MOCK_PERM_NONE = 0,
    MOCK_PERM_CONNECT,
    MOCK_PERM_READ,
    MOCK_PERM_WRITE,
    MOCK_PERM_ADMIN
} MockBranchPermission;

/* Branch status */
typedef enum MockBranchStatus
{
    MOCK_BRANCH_CREATING = 0,
    MOCK_BRANCH_READY,
    MOCK_BRANCH_SUSPENDED,
    MOCK_BRANCH_DELETING,
    MOCK_BRANCH_DELETED
} MockBranchStatus;

/* Protection level */
typedef enum MockProtectionLevel
{
    MOCK_PROTECT_NONE = 0,
    MOCK_PROTECT_REQUIRE_APPROVAL,
    MOCK_PROTECT_READ_ONLY,
    MOCK_PROTECT_LOCKED
} MockProtectionLevel;

/* Protection rule types */
typedef enum MockProtectionRuleType
{
    MOCK_RULE_REQUIRE_APPROVAL = 0,
    MOCK_RULE_RESTRICT_DDL,
    MOCK_RULE_RESTRICT_DELETE,
    MOCK_RULE_REQUIRE_MFA,
    MOCK_RULE_AUDIT_ALL,
    MOCK_RULE_TIME_WINDOW,
    MOCK_RULE_IP_ALLOWLIST
} MockProtectionRuleType;

/* Branch structure */
typedef struct MockBranch
{
    char                    branch_id[UUID_LEN + 1];
    char                    project_id[UUID_LEN + 1];
    char                    parent_branch_id[UUID_LEN + 1];
    char                    name[64];
    char                    owner_user_id[UUID_LEN + 1];
    MockBranchStatus        status;
    MockProtectionLevel     protection_level;
    bool                    inherit_permissions;
    int64                   created_at;
    bool                    in_use;
} MockBranch;

/* Branch permission grant */
typedef struct MockBranchPermGrant
{
    char                    permission_id[UUID_LEN + 1];
    char                    branch_id[UUID_LEN + 1];
    char                    user_id[UUID_LEN + 1];     /* Either user_id or team_id */
    char                    team_id[UUID_LEN + 1];
    MockBranchPermission    permission_level;
    int64                   valid_until;              /* 0 = no expiry */
    char                    granted_by[UUID_LEN + 1];
    int64                   granted_at;
    bool                    revoked;
    bool                    in_use;
} MockBranchPermGrant;

/* Team */
typedef struct MockTeam
{
    char                    team_id[UUID_LEN + 1];
    char                    project_id[UUID_LEN + 1];
    char                    name[64];
    bool                    in_use;
} MockTeam;

/* Team membership */
typedef struct MockTeamMember
{
    char                    team_id[UUID_LEN + 1];
    char                    user_id[UUID_LEN + 1];
    bool                    in_use;
} MockTeamMember;

/* Protection rule */
typedef struct MockProtectionRule
{
    char                    rule_id[UUID_LEN + 1];
    char                    branch_id[UUID_LEN + 1];
    MockProtectionRuleType  rule_type;
    char                    config[512];             /* JSON-like config */
    bool                    enabled;
    bool                    in_use;
} MockProtectionRule;

/* Global state */
static MockBranch mock_branches[MAX_BRANCHES];
static MockBranchPermGrant mock_permissions[MAX_PERMISSIONS];
static MockTeam mock_teams[MAX_TEAMS];
static MockTeamMember mock_team_members[MAX_TEAM_MEMBERS];
static MockProtectionRule mock_rules[MAX_PROTECTION_RULES];
static int64 mock_current_time = 1704067200;

/* ============================================================
 * Mock Utility Functions
 * ============================================================ */

static void mock_branch_access_init(void)
{
    memset(mock_branches, 0, sizeof(mock_branches));
    memset(mock_permissions, 0, sizeof(mock_permissions));
    memset(mock_teams, 0, sizeof(mock_teams));
    memset(mock_team_members, 0, sizeof(mock_team_members));
    memset(mock_rules, 0, sizeof(mock_rules));
    mock_current_time = 1704067200;
}

static void mock_branch_access_cleanup(void)
{
    memset(mock_branches, 0, sizeof(mock_branches));
    memset(mock_permissions, 0, sizeof(mock_permissions));
    memset(mock_teams, 0, sizeof(mock_teams));
    memset(mock_team_members, 0, sizeof(mock_team_members));
    memset(mock_rules, 0, sizeof(mock_rules));
}

static void mock_generate_uuid(char *buf)
{
    static int counter = 0;
    snprintf(buf, UUID_LEN + 1, "%08x-%04x-%04x-%04x-%012x",
             (unsigned int)(mock_current_time & 0xFFFFFFFF),
             (counter >> 16) & 0xFFFF,
             counter & 0xFFFF,
             (unsigned int)(rand() & 0xFFFF),
             (unsigned int)(rand() & 0xFFFFFFFF));
    counter++;
}

static const char *mock_permission_name(MockBranchPermission perm)
{
    switch (perm)
    {
        case MOCK_PERM_ADMIN:   return "admin";
        case MOCK_PERM_WRITE:   return "write";
        case MOCK_PERM_READ:    return "read";
        case MOCK_PERM_CONNECT: return "connect";
        default:               return "none";
    }
}

/* ============================================================
 * Mock Branch Functions
 * ============================================================ */

/* Create a branch */
static MockBranch *mock_create_branch(const char *project_id, const char *name,
                                       const char *owner_user_id,
                                       const char *parent_branch_id)
{
    MockBranch *branch = NULL;
    for (int i = 0; i < MAX_BRANCHES; i++)
    {
        if (!mock_branches[i].in_use)
        {
            branch = &mock_branches[i];
            break;
        }
    }
    if (!branch)
        return NULL;

    mock_generate_uuid(branch->branch_id);
    strncpy(branch->project_id, project_id, UUID_LEN);
    strncpy(branch->name, name, sizeof(branch->name) - 1);
    strncpy(branch->owner_user_id, owner_user_id, UUID_LEN);

    if (parent_branch_id)
        strncpy(branch->parent_branch_id, parent_branch_id, UUID_LEN);
    else
        branch->parent_branch_id[0] = '\0';

    branch->status = MOCK_BRANCH_READY;
    branch->protection_level = MOCK_PROTECT_NONE;
    branch->inherit_permissions = true;
    branch->created_at = mock_current_time;
    branch->in_use = true;

    return branch;
}

/* Find branch by ID */
static MockBranch *mock_find_branch(const char *branch_id)
{
    for (int i = 0; i < MAX_BRANCHES; i++)
    {
        if (mock_branches[i].in_use &&
            strcmp(mock_branches[i].branch_id, branch_id) == 0)
            return &mock_branches[i];
    }
    return NULL;
}

/* ============================================================
 * Mock Team Functions
 * ============================================================ */

/* Create a team */
static MockTeam *mock_create_team(const char *project_id, const char *name)
{
    MockTeam *team = NULL;
    for (int i = 0; i < MAX_TEAMS; i++)
    {
        if (!mock_teams[i].in_use)
        {
            team = &mock_teams[i];
            break;
        }
    }
    if (!team)
        return NULL;

    mock_generate_uuid(team->team_id);
    strncpy(team->project_id, project_id, UUID_LEN);
    strncpy(team->name, name, sizeof(team->name) - 1);
    team->in_use = true;

    return team;
}

/* Add member to team */
static bool mock_add_team_member(const char *team_id, const char *user_id)
{
    MockTeamMember *member = NULL;
    for (int i = 0; i < MAX_TEAM_MEMBERS; i++)
    {
        if (!mock_team_members[i].in_use)
        {
            member = &mock_team_members[i];
            break;
        }
    }
    if (!member)
        return false;

    strncpy(member->team_id, team_id, UUID_LEN);
    strncpy(member->user_id, user_id, UUID_LEN);
    member->in_use = true;

    return true;
}

/* Check if user is member of team */
static bool mock_is_team_member(const char *team_id, const char *user_id)
{
    for (int i = 0; i < MAX_TEAM_MEMBERS; i++)
    {
        if (mock_team_members[i].in_use &&
            strcmp(mock_team_members[i].team_id, team_id) == 0 &&
            strcmp(mock_team_members[i].user_id, user_id) == 0)
            return true;
    }
    return false;
}

/* ============================================================
 * Mock Permission Functions
 * ============================================================ */

/* Grant permission to user on branch */
static MockBranchPermGrant *mock_grant_user_permission(
    const char *branch_id,
    const char *user_id,
    MockBranchPermission permission,
    const char *granted_by,
    int64 valid_until)
{
    MockBranchPermGrant *grant = NULL;
    for (int i = 0; i < MAX_PERMISSIONS; i++)
    {
        if (!mock_permissions[i].in_use)
        {
            grant = &mock_permissions[i];
            break;
        }
    }
    if (!grant)
        return NULL;

    mock_generate_uuid(grant->permission_id);
    strncpy(grant->branch_id, branch_id, UUID_LEN);
    strncpy(grant->user_id, user_id, UUID_LEN);
    grant->team_id[0] = '\0';
    grant->permission_level = permission;
    grant->valid_until = valid_until;
    strncpy(grant->granted_by, granted_by, UUID_LEN);
    grant->granted_at = mock_current_time;
    grant->revoked = false;
    grant->in_use = true;

    return grant;
}

/* Grant permission to team on branch */
static MockBranchPermGrant *mock_grant_team_permission(
    const char *branch_id,
    const char *team_id,
    MockBranchPermission permission,
    const char *granted_by)
{
    MockBranchPermGrant *grant = NULL;
    for (int i = 0; i < MAX_PERMISSIONS; i++)
    {
        if (!mock_permissions[i].in_use)
        {
            grant = &mock_permissions[i];
            break;
        }
    }
    if (!grant)
        return NULL;

    mock_generate_uuid(grant->permission_id);
    strncpy(grant->branch_id, branch_id, UUID_LEN);
    grant->user_id[0] = '\0';
    strncpy(grant->team_id, team_id, UUID_LEN);
    grant->permission_level = permission;
    grant->valid_until = 0;
    strncpy(grant->granted_by, granted_by, UUID_LEN);
    grant->granted_at = mock_current_time;
    grant->revoked = false;
    grant->in_use = true;

    return grant;
}

/* Revoke permission */
static bool mock_revoke_permission(const char *permission_id)
{
    for (int i = 0; i < MAX_PERMISSIONS; i++)
    {
        if (mock_permissions[i].in_use &&
            strcmp(mock_permissions[i].permission_id, permission_id) == 0)
        {
            mock_permissions[i].revoked = true;
            return true;
        }
    }
    return false;
}

/* Get user's direct permission on branch */
static MockBranchPermission mock_get_direct_user_permission(
    const char *branch_id, const char *user_id)
{
    MockBranchPermission best = MOCK_PERM_NONE;

    for (int i = 0; i < MAX_PERMISSIONS; i++)
    {
        MockBranchPermGrant *grant = &mock_permissions[i];

        if (!grant->in_use || grant->revoked)
            continue;

        if (strcmp(grant->branch_id, branch_id) != 0)
            continue;

        if (grant->user_id[0] == '\0' || strcmp(grant->user_id, user_id) != 0)
            continue;

        /* Check expiration */
        if (grant->valid_until > 0 && mock_current_time > grant->valid_until)
            continue;

        if (grant->permission_level > best)
            best = grant->permission_level;
    }

    return best;
}

/* Get user's permission through team membership */
static MockBranchPermission mock_get_team_permission(
    const char *branch_id, const char *user_id)
{
    MockBranchPermission best = MOCK_PERM_NONE;

    for (int i = 0; i < MAX_PERMISSIONS; i++)
    {
        MockBranchPermGrant *grant = &mock_permissions[i];

        if (!grant->in_use || grant->revoked)
            continue;

        if (strcmp(grant->branch_id, branch_id) != 0)
            continue;

        if (grant->team_id[0] == '\0')
            continue;

        /* Check if user is member of this team */
        if (!mock_is_team_member(grant->team_id, user_id))
            continue;

        if (grant->permission_level > best)
            best = grant->permission_level;
    }

    return best;
}

/* Resolve effective permission for user on branch (recursive) */
static MockBranchPermission mock_get_branch_permission(
    const char *user_id, const char *branch_id)
{
    MockBranch *branch = mock_find_branch(branch_id);
    if (!branch)
        return MOCK_PERM_NONE;

    /* Check if branch is locked */
    if (branch->protection_level == MOCK_PROTECT_LOCKED)
        return MOCK_PERM_NONE;

    /* Branch owner has admin */
    if (strcmp(branch->owner_user_id, user_id) == 0)
        return MOCK_PERM_ADMIN;

    /* Check direct user permission */
    MockBranchPermission perm = mock_get_direct_user_permission(branch_id, user_id);
    if (perm != MOCK_PERM_NONE)
        return perm;

    /* Check team permissions */
    perm = mock_get_team_permission(branch_id, user_id);
    if (perm != MOCK_PERM_NONE)
        return perm;

    /* Check inherited permissions from parent */
    if (branch->inherit_permissions && branch->parent_branch_id[0] != '\0')
    {
        perm = mock_get_branch_permission(user_id, branch->parent_branch_id);
        if (perm != MOCK_PERM_NONE)
        {
            /* Downgrade write to read on read-only branches */
            if (branch->protection_level == MOCK_PROTECT_READ_ONLY &&
                (perm == MOCK_PERM_ADMIN || perm == MOCK_PERM_WRITE))
                return MOCK_PERM_READ;

            return perm;
        }
    }

    return MOCK_PERM_NONE;
}

/* ============================================================
 * Mock Protection Rules
 * ============================================================ */

/* Add protection rule to branch */
static MockProtectionRule *mock_add_protection_rule(
    const char *branch_id,
    MockProtectionRuleType rule_type,
    const char *config)
{
    MockProtectionRule *rule = NULL;
    for (int i = 0; i < MAX_PROTECTION_RULES; i++)
    {
        if (!mock_rules[i].in_use)
        {
            rule = &mock_rules[i];
            break;
        }
    }
    if (!rule)
        return NULL;

    mock_generate_uuid(rule->rule_id);
    strncpy(rule->branch_id, branch_id, UUID_LEN);
    rule->rule_type = rule_type;
    if (config)
        strncpy(rule->config, config, sizeof(rule->config) - 1);
    rule->enabled = true;
    rule->in_use = true;

    return rule;
}

/* Check if branch has specific rule enabled */
static bool mock_has_protection_rule(const char *branch_id, MockProtectionRuleType rule_type)
{
    for (int i = 0; i < MAX_PROTECTION_RULES; i++)
    {
        if (mock_rules[i].in_use &&
            mock_rules[i].enabled &&
            strcmp(mock_rules[i].branch_id, branch_id) == 0 &&
            mock_rules[i].rule_type == rule_type)
            return true;
    }
    return false;
}

/* ============================================================
 * Mock Cross-Branch Access
 * ============================================================ */

/* Validate cross-branch operation */
static bool mock_validate_cross_branch_access(
    const char *user_id,
    const char *source_branch_id,
    const char *target_branch_id,
    const char *operation)  /* "copy", "compare", "merge" */
{
    MockBranchPermission source_perm = mock_get_branch_permission(user_id, source_branch_id);
    MockBranchPermission target_perm = mock_get_branch_permission(user_id, target_branch_id);

    if (source_perm == MOCK_PERM_NONE || target_perm == MOCK_PERM_NONE)
        return false;

    if (strcmp(operation, "copy") == 0)
    {
        /* Need read on source, write on target */
        return source_perm >= MOCK_PERM_READ && target_perm >= MOCK_PERM_WRITE;
    }
    else if (strcmp(operation, "compare") == 0)
    {
        /* Need read on both */
        return source_perm >= MOCK_PERM_READ && target_perm >= MOCK_PERM_READ;
    }
    else if (strcmp(operation, "merge") == 0)
    {
        /* Need write on both */
        return source_perm >= MOCK_PERM_WRITE && target_perm >= MOCK_PERM_WRITE;
    }

    return false;
}

/* ============================================================
 * Test Cases: Basic Branch Permissions
 * ============================================================ */

static void test_branch_owner_has_admin(void)
{
    TEST_BEGIN("branch_owner_has_admin")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner-user", NULL);

        MockBranchPermission perm = mock_get_branch_permission("owner-user", branch->branch_id);
        TEST_ASSERT_EQ(MOCK_PERM_ADMIN, perm);

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_no_permission_by_default(void)
{
    TEST_BEGIN("no_permission_by_default")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        MockBranchPermission perm = mock_get_branch_permission("other-user", branch->branch_id);
        TEST_ASSERT_EQ(MOCK_PERM_NONE, perm);

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_grant_user_permission(void)
{
    TEST_BEGIN("grant_user_permission")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        mock_grant_user_permission(branch->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);

        MockBranchPermission perm = mock_get_branch_permission("user-1", branch->branch_id);
        TEST_ASSERT_EQ(MOCK_PERM_READ, perm);

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_permission_levels(void)
{
    TEST_BEGIN("permission_levels")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        /* Grant different permission levels to different users */
        mock_grant_user_permission(branch->branch_id, "connect-user", MOCK_PERM_CONNECT, "owner", 0);
        mock_grant_user_permission(branch->branch_id, "read-user", MOCK_PERM_READ, "owner", 0);
        mock_grant_user_permission(branch->branch_id, "write-user", MOCK_PERM_WRITE, "owner", 0);
        mock_grant_user_permission(branch->branch_id, "admin-user", MOCK_PERM_ADMIN, "owner", 0);

        TEST_ASSERT_EQ(MOCK_PERM_CONNECT, mock_get_branch_permission("connect-user", branch->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_READ, mock_get_branch_permission("read-user", branch->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("write-user", branch->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_ADMIN, mock_get_branch_permission("admin-user", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_highest_permission_wins(void)
{
    TEST_BEGIN("highest_permission_wins")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        /* Grant multiple permissions - highest should win */
        mock_grant_user_permission(branch->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);
        mock_grant_user_permission(branch->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        MockBranchPermission perm = mock_get_branch_permission("user-1", branch->branch_id);
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, perm);

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Permission Expiration
 * ============================================================ */

static void test_permission_expiration(void)
{
    TEST_BEGIN("permission_expiration")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        /* Grant permission that expires in 1 hour */
        mock_grant_user_permission(branch->branch_id, "user-1", MOCK_PERM_WRITE,
                                    "owner", mock_current_time + 3600);

        /* Permission should be valid now */
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-1", branch->branch_id));

        /* Advance time past expiration */
        mock_current_time += 7200;

        /* Permission should be expired */
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("user-1", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_permission_revocation(void)
{
    TEST_BEGIN("permission_revocation")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        MockBranchPermGrant *grant = mock_grant_user_permission(
            branch->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        /* Permission should be valid */
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-1", branch->branch_id));

        /* Revoke permission */
        mock_revoke_permission(grant->permission_id);

        /* Permission should be gone */
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("user-1", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Team Permissions
 * ============================================================ */

static void test_team_permission(void)
{
    TEST_BEGIN("team_permission")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);
        MockTeam *team = mock_create_team("project-1", "developers");

        mock_add_team_member(team->team_id, "user-1");
        mock_add_team_member(team->team_id, "user-2");

        /* Grant write to team */
        mock_grant_team_permission(branch->branch_id, team->team_id, MOCK_PERM_WRITE, "owner");

        /* Team members should have permission */
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-1", branch->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-2", branch->branch_id));

        /* Non-member should not */
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("user-3", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_direct_overrides_team(void)
{
    TEST_BEGIN("direct_permission_overrides_team")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);
        MockTeam *team = mock_create_team("project-1", "viewers");

        mock_add_team_member(team->team_id, "user-1");

        /* Grant read to team */
        mock_grant_team_permission(branch->branch_id, team->team_id, MOCK_PERM_READ, "owner");

        /* Grant write directly to user */
        mock_grant_user_permission(branch->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        /* Direct permission (write) should take precedence */
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-1", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Permission Inheritance
 * ============================================================ */

static void test_permission_inheritance_basic(void)
{
    TEST_BEGIN("permission_inheritance_basic")
        mock_branch_access_init();

        /* Create parent branch */
        MockBranch *main = mock_create_branch("project-1", "main", "owner", NULL);
        mock_grant_user_permission(main->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        /* Create child branch */
        MockBranch *feature = mock_create_branch("project-1", "feature", "owner", main->branch_id);

        /* User should have inherited permission on child */
        TEST_ASSERT_EQ(MOCK_PERM_WRITE, mock_get_branch_permission("user-1", feature->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_inheritance_disabled(void)
{
    TEST_BEGIN("inheritance_disabled")
        mock_branch_access_init();

        MockBranch *main = mock_create_branch("project-1", "main", "owner", NULL);
        mock_grant_user_permission(main->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        MockBranch *feature = mock_create_branch("project-1", "feature", "owner", main->branch_id);
        feature->inherit_permissions = false;  /* Disable inheritance */

        /* User should NOT have permission (inheritance disabled) */
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("user-1", feature->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_inheritance_multiple_levels(void)
{
    TEST_BEGIN("inheritance_multiple_levels")
        mock_branch_access_init();

        /* main -> develop -> feature */
        MockBranch *main = mock_create_branch("project-1", "main", "owner", NULL);
        MockBranch *develop = mock_create_branch("project-1", "develop", "owner", main->branch_id);
        MockBranch *feature = mock_create_branch("project-1", "feature", "owner", develop->branch_id);

        mock_grant_user_permission(main->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);

        /* Permission should propagate through all levels */
        TEST_ASSERT_EQ(MOCK_PERM_READ, mock_get_branch_permission("user-1", develop->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_READ, mock_get_branch_permission("user-1", feature->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_child_permission_overrides_parent(void)
{
    TEST_BEGIN("child_permission_overrides_parent")
        mock_branch_access_init();

        MockBranch *main = mock_create_branch("project-1", "main", "owner", NULL);
        MockBranch *feature = mock_create_branch("project-1", "feature", "owner", main->branch_id);

        /* Grant read on parent */
        mock_grant_user_permission(main->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);

        /* Grant admin on child */
        mock_grant_user_permission(feature->branch_id, "user-1", MOCK_PERM_ADMIN, "owner", 0);

        /* Child's direct permission should be used */
        TEST_ASSERT_EQ(MOCK_PERM_ADMIN, mock_get_branch_permission("user-1", feature->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Branch Protection
 * ============================================================ */

static void test_locked_branch_no_access(void)
{
    TEST_BEGIN("locked_branch_no_access")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);
        branch->protection_level = MOCK_PROTECT_LOCKED;

        /* Even owner should not have access to locked branch */
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("owner", branch->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_read_only_downgrades_write(void)
{
    TEST_BEGIN("read_only_downgrades_write")
        mock_branch_access_init();

        MockBranch *main = mock_create_branch("project-1", "main", "owner", NULL);
        mock_grant_user_permission(main->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        MockBranch *staging = mock_create_branch("project-1", "staging", "owner", main->branch_id);
        staging->protection_level = MOCK_PROTECT_READ_ONLY;

        /* Write permission should be downgraded to read */
        TEST_ASSERT_EQ(MOCK_PERM_READ, mock_get_branch_permission("user-1", staging->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_protection_rules(void)
{
    TEST_BEGIN("protection_rules")
        mock_branch_access_init();

        MockBranch *branch = mock_create_branch("project-1", "main", "owner", NULL);

        mock_add_protection_rule(branch->branch_id, MOCK_RULE_REQUIRE_APPROVAL,
                                  "{\"approvers\":[\"user-1\"]}");
        mock_add_protection_rule(branch->branch_id, MOCK_RULE_AUDIT_ALL, NULL);

        TEST_ASSERT(mock_has_protection_rule(branch->branch_id, MOCK_RULE_REQUIRE_APPROVAL));
        TEST_ASSERT(mock_has_protection_rule(branch->branch_id, MOCK_RULE_AUDIT_ALL));
        TEST_ASSERT_FALSE(mock_has_protection_rule(branch->branch_id, MOCK_RULE_RESTRICT_DDL));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Cross-Branch Access
 * ============================================================ */

static void test_cross_branch_copy(void)
{
    TEST_BEGIN("cross_branch_copy")
        mock_branch_access_init();

        MockBranch *source = mock_create_branch("project-1", "source", "owner", NULL);
        MockBranch *target = mock_create_branch("project-1", "target", "owner", NULL);

        mock_grant_user_permission(source->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);
        mock_grant_user_permission(target->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        /* Should be able to copy (read source, write target) */
        TEST_ASSERT(mock_validate_cross_branch_access(
            "user-1", source->branch_id, target->branch_id, "copy"));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_cross_branch_copy_insufficient_permissions(void)
{
    TEST_BEGIN("cross_branch_copy_insufficient")
        mock_branch_access_init();

        MockBranch *source = mock_create_branch("project-1", "source", "owner", NULL);
        MockBranch *target = mock_create_branch("project-1", "target", "owner", NULL);

        mock_grant_user_permission(source->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);
        mock_grant_user_permission(target->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);

        /* Should fail - no write on target */
        TEST_ASSERT_FALSE(mock_validate_cross_branch_access(
            "user-1", source->branch_id, target->branch_id, "copy"));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_cross_branch_compare(void)
{
    TEST_BEGIN("cross_branch_compare")
        mock_branch_access_init();

        MockBranch *b1 = mock_create_branch("project-1", "branch1", "owner", NULL);
        MockBranch *b2 = mock_create_branch("project-1", "branch2", "owner", NULL);

        mock_grant_user_permission(b1->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);
        mock_grant_user_permission(b2->branch_id, "user-1", MOCK_PERM_READ, "owner", 0);

        /* Should be able to compare (read both) */
        TEST_ASSERT(mock_validate_cross_branch_access(
            "user-1", b1->branch_id, b2->branch_id, "compare"));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_cross_branch_merge(void)
{
    TEST_BEGIN("cross_branch_merge")
        mock_branch_access_init();

        MockBranch *source = mock_create_branch("project-1", "feature", "owner", NULL);
        MockBranch *target = mock_create_branch("project-1", "main", "owner", NULL);

        mock_grant_user_permission(source->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);
        mock_grant_user_permission(target->branch_id, "user-1", MOCK_PERM_WRITE, "owner", 0);

        /* Should be able to merge (write both) */
        TEST_ASSERT(mock_validate_cross_branch_access(
            "user-1", source->branch_id, target->branch_id, "merge"));

        /* User with only read on target should fail */
        mock_grant_user_permission(source->branch_id, "user-2", MOCK_PERM_WRITE, "owner", 0);
        mock_grant_user_permission(target->branch_id, "user-2", MOCK_PERM_READ, "owner", 0);

        TEST_ASSERT_FALSE(mock_validate_cross_branch_access(
            "user-2", source->branch_id, target->branch_id, "merge"));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Edge Cases
 * ============================================================ */

static void test_branch_not_found(void)
{
    TEST_BEGIN("branch_not_found")
        mock_branch_access_init();

        MockBranchPermission perm = mock_get_branch_permission("user-1", "nonexistent-branch");
        TEST_ASSERT_EQ(MOCK_PERM_NONE, perm);

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_permission_name_helper(void)
{
    TEST_BEGIN("permission_name_helper")
        TEST_ASSERT_STR_EQ("admin", mock_permission_name(MOCK_PERM_ADMIN));
        TEST_ASSERT_STR_EQ("write", mock_permission_name(MOCK_PERM_WRITE));
        TEST_ASSERT_STR_EQ("read", mock_permission_name(MOCK_PERM_READ));
        TEST_ASSERT_STR_EQ("connect", mock_permission_name(MOCK_PERM_CONNECT));
        TEST_ASSERT_STR_EQ("none", mock_permission_name(MOCK_PERM_NONE));
    TEST_END();
}

static void test_multiple_branches_isolation(void)
{
    TEST_BEGIN("multiple_branches_isolation")
        mock_branch_access_init();

        MockBranch *b1 = mock_create_branch("project-1", "branch1", "owner", NULL);
        MockBranch *b2 = mock_create_branch("project-1", "branch2", "owner", NULL);

        mock_grant_user_permission(b1->branch_id, "user-1", MOCK_PERM_ADMIN, "owner", 0);

        /* User should have admin on b1 only */
        TEST_ASSERT_EQ(MOCK_PERM_ADMIN, mock_get_branch_permission("user-1", b1->branch_id));
        TEST_ASSERT_EQ(MOCK_PERM_NONE, mock_get_branch_permission("user-1", b2->branch_id));

        mock_branch_access_cleanup();
    TEST_END();
}

static void test_team_member_check(void)
{
    TEST_BEGIN("team_member_check")
        mock_branch_access_init();

        MockTeam *team = mock_create_team("project-1", "team");
        mock_add_team_member(team->team_id, "member-1");
        mock_add_team_member(team->team_id, "member-2");

        TEST_ASSERT(mock_is_team_member(team->team_id, "member-1"));
        TEST_ASSERT(mock_is_team_member(team->team_id, "member-2"));
        TEST_ASSERT_FALSE(mock_is_team_member(team->team_id, "non-member"));
        TEST_ASSERT_FALSE(mock_is_team_member("nonexistent-team", "member-1"));

        mock_branch_access_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_branch_access(void)
{
    /* Basic permission tests */
    test_branch_owner_has_admin();
    test_no_permission_by_default();
    test_grant_user_permission();
    test_permission_levels();
    test_highest_permission_wins();

    /* Expiration/revocation tests */
    test_permission_expiration();
    test_permission_revocation();

    /* Team permission tests */
    test_team_permission();
    test_direct_overrides_team();

    /* Inheritance tests */
    test_permission_inheritance_basic();
    test_inheritance_disabled();
    test_inheritance_multiple_levels();
    test_child_permission_overrides_parent();

    /* Protection tests */
    test_locked_branch_no_access();
    test_read_only_downgrades_write();
    test_protection_rules();

    /* Cross-branch tests */
    test_cross_branch_copy();
    test_cross_branch_copy_insufficient_permissions();
    test_cross_branch_compare();
    test_cross_branch_merge();

    /* Edge cases */
    test_branch_not_found();
    test_permission_name_helper();
    test_multiple_branches_isolation();
    test_team_member_check();
}
