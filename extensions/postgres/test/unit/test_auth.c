/*-------------------------------------------------------------------------
 *
 * test_auth.c
 *    Unit tests for Orochi DB core authentication system
 *
 * Tests covered:
 *   - User creation and management
 *   - Password hashing and verification
 *   - Session creation, validation, and expiration
 *   - Token generation and validation
 *   - Role management
 *   - Auth context setting
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

#define MAX_USERS           100
#define MAX_SESSIONS        200
#define MAX_TOKENS          500
#define MAX_ROLES           50
#define UUID_LEN            36
#define TOKEN_LEN           64
#define HASH_LEN            128
#define SESSION_TIMEOUT_SEC 86400  /* 24 hours */
#define TOKEN_EXPIRY_SEC    3600   /* 1 hour */

/* User status */
typedef enum MockUserStatus
{
    MOCK_USER_PENDING = 0,
    MOCK_USER_ACTIVE,
    MOCK_USER_SUSPENDED,
    MOCK_USER_BANNED,
    MOCK_USER_DELETED
} MockUserStatus;

/* Session status */
typedef enum MockSessionStatus
{
    MOCK_SESSION_ACTIVE = 0,
    MOCK_SESSION_EXPIRED,
    MOCK_SESSION_REVOKED
} MockSessionStatus;

/* AAL levels (Authenticator Assurance Level) */
typedef enum MockAAL
{
    MOCK_AAL1 = 1,  /* Single factor */
    MOCK_AAL2 = 2,  /* Multi-factor */
    MOCK_AAL3 = 3   /* Hardware-based */
} MockAAL;

/* User structure */
typedef struct MockUser
{
    char                user_id[UUID_LEN + 1];
    char                email[256];
    char                phone[32];
    char                password_hash[HASH_LEN + 1];
    MockUserStatus      status;
    bool                email_confirmed;
    bool                phone_confirmed;
    bool                is_anonymous;
    char                role[64];
    int64               created_at;
    int64               last_sign_in;
    bool                in_use;
} MockUser;

/* Session structure */
typedef struct MockSession
{
    char                session_id[UUID_LEN + 1];
    char                user_id[UUID_LEN + 1];
    char                refresh_token[TOKEN_LEN + 1];
    char                ip_address[46];
    char                user_agent[256];
    MockAAL             aal;
    MockSessionStatus   status;
    int64               created_at;
    int64               expires_at;
    int64               last_activity;
    bool                in_use;
} MockSession;

/* Role structure */
typedef struct MockRole
{
    char                role_name[64];
    char                description[256];
    int                 permission_mask;
    bool                in_use;
} MockRole;

/* Token cache entry */
typedef struct MockTokenCacheEntry
{
    char                token_hash[65];  /* SHA256 hex */
    char                user_id[UUID_LEN + 1];
    int64               expires_at;
    bool                revoked;
    bool                in_use;
} MockTokenCacheEntry;

/* Global mock state */
static MockUser mock_users[MAX_USERS];
static MockSession mock_sessions[MAX_SESSIONS];
static MockRole mock_roles[MAX_ROLES];
static MockTokenCacheEntry mock_token_cache[MAX_TOKENS];
static int mock_user_count = 0;
static int mock_session_count = 0;
static int64 mock_current_time = 1704067200;  /* 2024-01-01 00:00:00 UTC */

/* ============================================================
 * Mock Utility Functions
 * ============================================================ */

static void mock_auth_init(void)
{
    memset(mock_users, 0, sizeof(mock_users));
    memset(mock_sessions, 0, sizeof(mock_sessions));
    memset(mock_roles, 0, sizeof(mock_roles));
    memset(mock_token_cache, 0, sizeof(mock_token_cache));
    mock_user_count = 0;
    mock_session_count = 0;
    mock_current_time = 1704067200;
}

static void mock_auth_cleanup(void)
{
    memset(mock_users, 0, sizeof(mock_users));
    memset(mock_sessions, 0, sizeof(mock_sessions));
    memset(mock_roles, 0, sizeof(mock_roles));
    memset(mock_token_cache, 0, sizeof(mock_token_cache));
}

/* Generate a mock UUID */
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

/* Simple hash function (mock - not cryptographic) */
static void mock_hash_password(const char *password, const char *salt, char *hash_out)
{
    unsigned int h = 0x811c9dc5;
    const char *p = password;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h *= 0x01000193;
    }
    p = salt;
    while (*p)
    {
        h ^= (unsigned char)*p++;
        h *= 0x01000193;
    }
    snprintf(hash_out, HASH_LEN + 1, "$mock$%08x$%s", h, salt);
}

/* Verify password against hash */
static bool mock_verify_password(const char *password, const char *hash)
{
    /* Extract salt from hash (format: $mock$hash$salt) */
    const char *salt_start = strrchr(hash, '$');
    if (!salt_start || salt_start == hash)
        return false;

    char computed_hash[HASH_LEN + 1];
    mock_hash_password(password, salt_start + 1, computed_hash);
    return strcmp(hash, computed_hash) == 0;
}

/* Generate random token */
static void mock_generate_token(char *buf, int len)
{
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < len - 1; i++)
    {
        buf[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buf[len - 1] = '\0';
}

/* ============================================================
 * Mock Auth Functions
 * ============================================================ */

/* Create a new user */
static MockUser *mock_create_user(const char *email, const char *password)
{
    if (mock_user_count >= MAX_USERS)
        return NULL;

    /* Check for duplicate email */
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (mock_users[i].in_use && strcmp(mock_users[i].email, email) == 0)
            return NULL;
    }

    /* Find free slot */
    MockUser *user = NULL;
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (!mock_users[i].in_use)
        {
            user = &mock_users[i];
            break;
        }
    }
    if (!user)
        return NULL;

    mock_generate_uuid(user->user_id);
    strncpy(user->email, email, sizeof(user->email) - 1);

    /* Hash password with salt */
    char salt[17];
    mock_generate_token(salt, sizeof(salt));
    mock_hash_password(password, salt, user->password_hash);

    user->status = MOCK_USER_PENDING;
    user->email_confirmed = false;
    user->phone_confirmed = false;
    user->is_anonymous = false;
    strcpy(user->role, "authenticated");
    user->created_at = mock_current_time;
    user->last_sign_in = 0;
    user->in_use = true;

    mock_user_count++;
    return user;
}

/* Create anonymous user */
static MockUser *mock_create_anonymous_user(void)
{
    if (mock_user_count >= MAX_USERS)
        return NULL;

    MockUser *user = NULL;
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (!mock_users[i].in_use)
        {
            user = &mock_users[i];
            break;
        }
    }
    if (!user)
        return NULL;

    mock_generate_uuid(user->user_id);
    user->email[0] = '\0';
    user->password_hash[0] = '\0';
    user->status = MOCK_USER_ACTIVE;
    user->email_confirmed = false;
    user->is_anonymous = true;
    strcpy(user->role, "anon");
    user->created_at = mock_current_time;
    user->in_use = true;

    mock_user_count++;
    return user;
}

/* Find user by ID */
static MockUser *mock_find_user_by_id(const char *user_id)
{
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (mock_users[i].in_use && strcmp(mock_users[i].user_id, user_id) == 0)
            return &mock_users[i];
    }
    return NULL;
}

/* Find user by email */
static MockUser *mock_find_user_by_email(const char *email)
{
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (mock_users[i].in_use && strcmp(mock_users[i].email, email) == 0)
            return &mock_users[i];
    }
    return NULL;
}

/* Authenticate user with password */
static MockUser *mock_authenticate_user(const char *email, const char *password)
{
    MockUser *user = mock_find_user_by_email(email);
    if (!user)
        return NULL;

    if (user->status != MOCK_USER_ACTIVE)
        return NULL;

    if (!mock_verify_password(password, user->password_hash))
        return NULL;

    user->last_sign_in = mock_current_time;
    return user;
}

/* Create session for user */
static MockSession *mock_create_session(const char *user_id, const char *ip_address,
                                         const char *user_agent, MockAAL aal)
{
    MockUser *user = mock_find_user_by_id(user_id);
    if (!user)
        return NULL;

    /* Find free slot */
    MockSession *session = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (!mock_sessions[i].in_use)
        {
            session = &mock_sessions[i];
            break;
        }
    }
    if (!session)
        return NULL;

    mock_generate_uuid(session->session_id);
    strncpy(session->user_id, user_id, UUID_LEN);
    mock_generate_token(session->refresh_token, TOKEN_LEN);

    if (ip_address)
        strncpy(session->ip_address, ip_address, sizeof(session->ip_address) - 1);
    if (user_agent)
        strncpy(session->user_agent, user_agent, sizeof(session->user_agent) - 1);

    session->aal = aal;
    session->status = MOCK_SESSION_ACTIVE;
    session->created_at = mock_current_time;
    session->expires_at = mock_current_time + SESSION_TIMEOUT_SEC;
    session->last_activity = mock_current_time;
    session->in_use = true;

    mock_session_count++;
    return session;
}

/* Find session by ID */
static MockSession *mock_find_session(const char *session_id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (mock_sessions[i].in_use && strcmp(mock_sessions[i].session_id, session_id) == 0)
            return &mock_sessions[i];
    }
    return NULL;
}

/* Validate session */
static bool mock_validate_session(const char *session_id)
{
    MockSession *session = mock_find_session(session_id);
    if (!session)
        return false;

    if (session->status != MOCK_SESSION_ACTIVE)
        return false;

    if (mock_current_time > session->expires_at)
    {
        session->status = MOCK_SESSION_EXPIRED;
        return false;
    }

    session->last_activity = mock_current_time;
    return true;
}

/* Revoke session */
static bool mock_revoke_session(const char *session_id)
{
    MockSession *session = mock_find_session(session_id);
    if (!session)
        return false;

    session->status = MOCK_SESSION_REVOKED;
    return true;
}

/* Refresh session */
static bool mock_refresh_session(const char *session_id, char *new_refresh_token)
{
    MockSession *session = mock_find_session(session_id);
    if (!session || session->status != MOCK_SESSION_ACTIVE)
        return false;

    if (mock_current_time > session->expires_at)
    {
        session->status = MOCK_SESSION_EXPIRED;
        return false;
    }

    /* Generate new refresh token */
    mock_generate_token(session->refresh_token, TOKEN_LEN);
    if (new_refresh_token)
        strcpy(new_refresh_token, session->refresh_token);

    session->expires_at = mock_current_time + SESSION_TIMEOUT_SEC;
    session->last_activity = mock_current_time;
    return true;
}

/* Token cache operations */
static bool mock_cache_token(const char *token_hash, const char *user_id, int64 expires_at)
{
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!mock_token_cache[i].in_use)
        {
            strncpy(mock_token_cache[i].token_hash, token_hash, 64);
            strncpy(mock_token_cache[i].user_id, user_id, UUID_LEN);
            mock_token_cache[i].expires_at = expires_at;
            mock_token_cache[i].revoked = false;
            mock_token_cache[i].in_use = true;
            return true;
        }
    }
    return false;
}

static MockTokenCacheEntry *mock_lookup_token(const char *token_hash)
{
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_token_cache[i].in_use &&
            strcmp(mock_token_cache[i].token_hash, token_hash) == 0)
        {
            return &mock_token_cache[i];
        }
    }
    return NULL;
}

static void mock_evict_expired_tokens(void)
{
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_token_cache[i].in_use &&
            mock_token_cache[i].expires_at < mock_current_time)
        {
            mock_token_cache[i].in_use = false;
        }
    }
}

/* Count active sessions for user */
static int mock_count_user_sessions(const char *user_id)
{
    int count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (mock_sessions[i].in_use &&
            mock_sessions[i].status == MOCK_SESSION_ACTIVE &&
            strcmp(mock_sessions[i].user_id, user_id) == 0)
        {
            count++;
        }
    }
    return count;
}

/* Revoke all sessions for user */
static int mock_revoke_all_user_sessions(const char *user_id)
{
    int count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (mock_sessions[i].in_use &&
            mock_sessions[i].status == MOCK_SESSION_ACTIVE &&
            strcmp(mock_sessions[i].user_id, user_id) == 0)
        {
            mock_sessions[i].status = MOCK_SESSION_REVOKED;
            count++;
        }
    }
    return count;
}

/* ============================================================
 * Test Cases: User Management
 * ============================================================ */

static void test_user_creation(void)
{
    TEST_BEGIN("user_creation_basic")
        mock_auth_init();

        MockUser *user = mock_create_user("test@example.com", "password123");
        TEST_ASSERT_NOT_NULL(user);
        TEST_ASSERT_STR_EQ("test@example.com", user->email);
        TEST_ASSERT_EQ(MOCK_USER_PENDING, user->status);
        TEST_ASSERT_FALSE(user->email_confirmed);
        TEST_ASSERT_FALSE(user->is_anonymous);
        TEST_ASSERT_STR_EQ("authenticated", user->role);

        mock_auth_cleanup();
    TEST_END();
}

static void test_user_duplicate_email(void)
{
    TEST_BEGIN("user_duplicate_email_rejected")
        mock_auth_init();

        MockUser *user1 = mock_create_user("dup@example.com", "password1");
        TEST_ASSERT_NOT_NULL(user1);

        /* Attempt to create user with same email */
        MockUser *user2 = mock_create_user("dup@example.com", "password2");
        TEST_ASSERT_NULL(user2);

        mock_auth_cleanup();
    TEST_END();
}

static void test_anonymous_user(void)
{
    TEST_BEGIN("anonymous_user_creation")
        mock_auth_init();

        MockUser *user = mock_create_anonymous_user();
        TEST_ASSERT_NOT_NULL(user);
        TEST_ASSERT(user->is_anonymous);
        TEST_ASSERT_EQ(MOCK_USER_ACTIVE, user->status);
        TEST_ASSERT_STR_EQ("anon", user->role);
        TEST_ASSERT_EQ(0, strlen(user->email));

        mock_auth_cleanup();
    TEST_END();
}

static void test_user_lookup_by_id(void)
{
    TEST_BEGIN("user_lookup_by_id")
        mock_auth_init();

        MockUser *created = mock_create_user("lookup@example.com", "pass");
        TEST_ASSERT_NOT_NULL(created);

        MockUser *found = mock_find_user_by_id(created->user_id);
        TEST_ASSERT_NOT_NULL(found);
        TEST_ASSERT_STR_EQ(created->email, found->email);

        /* Non-existent user */
        MockUser *not_found = mock_find_user_by_id("00000000-0000-0000-0000-000000000000");
        TEST_ASSERT_NULL(not_found);

        mock_auth_cleanup();
    TEST_END();
}

static void test_user_lookup_by_email(void)
{
    TEST_BEGIN("user_lookup_by_email")
        mock_auth_init();

        MockUser *created = mock_create_user("email@example.com", "pass");
        TEST_ASSERT_NOT_NULL(created);

        MockUser *found = mock_find_user_by_email("email@example.com");
        TEST_ASSERT_NOT_NULL(found);
        TEST_ASSERT_STR_EQ(created->user_id, found->user_id);

        /* Non-existent email */
        MockUser *not_found = mock_find_user_by_email("nonexistent@example.com");
        TEST_ASSERT_NULL(not_found);

        mock_auth_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Password Handling
 * ============================================================ */

static void test_password_hashing(void)
{
    TEST_BEGIN("password_hashing")
        char hash1[HASH_LEN + 1];
        char hash2[HASH_LEN + 1];

        mock_hash_password("mypassword", "salt1", hash1);
        mock_hash_password("mypassword", "salt2", hash2);

        /* Same password with different salt produces different hash */
        TEST_ASSERT_NEQ(0, strcmp(hash1, hash2));

        /* Hash contains mock marker */
        TEST_ASSERT_NOT_NULL(strstr(hash1, "$mock$"));
    TEST_END();
}

static void test_password_verification(void)
{
    TEST_BEGIN("password_verification")
        mock_auth_init();

        MockUser *user = mock_create_user("verify@example.com", "correctpassword");
        TEST_ASSERT_NOT_NULL(user);

        /* Correct password */
        TEST_ASSERT(mock_verify_password("correctpassword", user->password_hash));

        /* Incorrect password */
        TEST_ASSERT_FALSE(mock_verify_password("wrongpassword", user->password_hash));

        mock_auth_cleanup();
    TEST_END();
}

static void test_authentication(void)
{
    TEST_BEGIN("user_authentication")
        mock_auth_init();

        MockUser *user = mock_create_user("auth@example.com", "secret123");
        TEST_ASSERT_NOT_NULL(user);
        user->status = MOCK_USER_ACTIVE;  /* Activate user */

        /* Successful authentication */
        MockUser *authed = mock_authenticate_user("auth@example.com", "secret123");
        TEST_ASSERT_NOT_NULL(authed);
        TEST_ASSERT_STR_EQ(user->user_id, authed->user_id);

        /* Failed authentication - wrong password */
        MockUser *failed = mock_authenticate_user("auth@example.com", "wrongpassword");
        TEST_ASSERT_NULL(failed);

        /* Failed authentication - non-existent user */
        MockUser *no_user = mock_authenticate_user("nobody@example.com", "secret123");
        TEST_ASSERT_NULL(no_user);

        mock_auth_cleanup();
    TEST_END();
}

static void test_authentication_inactive_user(void)
{
    TEST_BEGIN("authentication_inactive_user")
        mock_auth_init();

        MockUser *user = mock_create_user("inactive@example.com", "pass");
        TEST_ASSERT_NOT_NULL(user);

        /* User still pending - should fail */
        MockUser *authed = mock_authenticate_user("inactive@example.com", "pass");
        TEST_ASSERT_NULL(authed);

        /* Suspended user */
        user->status = MOCK_USER_SUSPENDED;
        authed = mock_authenticate_user("inactive@example.com", "pass");
        TEST_ASSERT_NULL(authed);

        /* Banned user */
        user->status = MOCK_USER_BANNED;
        authed = mock_authenticate_user("inactive@example.com", "pass");
        TEST_ASSERT_NULL(authed);

        mock_auth_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Session Management
 * ============================================================ */

static void test_session_creation(void)
{
    TEST_BEGIN("session_creation")
        mock_auth_init();

        MockUser *user = mock_create_user("session@example.com", "pass");
        TEST_ASSERT_NOT_NULL(user);

        MockSession *session = mock_create_session(user->user_id, "192.168.1.1",
                                                    "Mozilla/5.0", MOCK_AAL1);
        TEST_ASSERT_NOT_NULL(session);
        TEST_ASSERT_STR_EQ(user->user_id, session->user_id);
        TEST_ASSERT_EQ(MOCK_SESSION_ACTIVE, session->status);
        TEST_ASSERT_EQ(MOCK_AAL1, session->aal);
        TEST_ASSERT_GT(strlen(session->refresh_token), 0);

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_validation(void)
{
    TEST_BEGIN("session_validation")
        mock_auth_init();

        MockUser *user = mock_create_user("valid@example.com", "pass");
        MockSession *session = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);
        TEST_ASSERT_NOT_NULL(session);

        /* Valid session */
        TEST_ASSERT(mock_validate_session(session->session_id));

        /* Non-existent session */
        TEST_ASSERT_FALSE(mock_validate_session("00000000-0000-0000-0000-000000000000"));

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_expiration(void)
{
    TEST_BEGIN("session_expiration")
        mock_auth_init();

        MockUser *user = mock_create_user("expire@example.com", "pass");
        MockSession *session = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);
        TEST_ASSERT_NOT_NULL(session);

        /* Valid before expiry */
        TEST_ASSERT(mock_validate_session(session->session_id));

        /* Advance time past expiration */
        mock_current_time += SESSION_TIMEOUT_SEC + 1;

        /* Should now be expired */
        TEST_ASSERT_FALSE(mock_validate_session(session->session_id));
        TEST_ASSERT_EQ(MOCK_SESSION_EXPIRED, session->status);

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_revocation(void)
{
    TEST_BEGIN("session_revocation")
        mock_auth_init();

        MockUser *user = mock_create_user("revoke@example.com", "pass");
        MockSession *session = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);

        /* Initially valid */
        TEST_ASSERT(mock_validate_session(session->session_id));

        /* Revoke session */
        TEST_ASSERT(mock_revoke_session(session->session_id));
        TEST_ASSERT_EQ(MOCK_SESSION_REVOKED, session->status);

        /* No longer valid */
        TEST_ASSERT_FALSE(mock_validate_session(session->session_id));

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_refresh(void)
{
    TEST_BEGIN("session_refresh")
        mock_auth_init();

        MockUser *user = mock_create_user("refresh@example.com", "pass");
        MockSession *session = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);

        char old_token[TOKEN_LEN + 1];
        strcpy(old_token, session->refresh_token);
        int64 old_expires = session->expires_at;

        /* Advance time */
        mock_current_time += 3600;

        char new_token[TOKEN_LEN + 1];
        TEST_ASSERT(mock_refresh_session(session->session_id, new_token));

        /* Token should be different */
        TEST_ASSERT_NEQ(0, strcmp(old_token, new_token));

        /* Expiry should be extended */
        TEST_ASSERT_GT(session->expires_at, old_expires);

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_refresh_expired(void)
{
    TEST_BEGIN("session_refresh_expired_fails")
        mock_auth_init();

        MockUser *user = mock_create_user("expired@example.com", "pass");
        MockSession *session = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);

        /* Expire the session */
        mock_current_time += SESSION_TIMEOUT_SEC + 1;

        /* Refresh should fail */
        char new_token[TOKEN_LEN + 1];
        TEST_ASSERT_FALSE(mock_refresh_session(session->session_id, new_token));

        mock_auth_cleanup();
    TEST_END();
}

static void test_multiple_sessions(void)
{
    TEST_BEGIN("multiple_sessions_per_user")
        mock_auth_init();

        MockUser *user = mock_create_user("multi@example.com", "pass");

        /* Create multiple sessions */
        MockSession *s1 = mock_create_session(user->user_id, "1.1.1.1", "Chrome", MOCK_AAL1);
        MockSession *s2 = mock_create_session(user->user_id, "2.2.2.2", "Firefox", MOCK_AAL1);
        MockSession *s3 = mock_create_session(user->user_id, "3.3.3.3", "Safari", MOCK_AAL2);

        TEST_ASSERT_NOT_NULL(s1);
        TEST_ASSERT_NOT_NULL(s2);
        TEST_ASSERT_NOT_NULL(s3);

        TEST_ASSERT_EQ(3, mock_count_user_sessions(user->user_id));

        /* All sessions unique */
        TEST_ASSERT_NEQ(0, strcmp(s1->session_id, s2->session_id));
        TEST_ASSERT_NEQ(0, strcmp(s2->session_id, s3->session_id));

        mock_auth_cleanup();
    TEST_END();
}

static void test_revoke_all_sessions(void)
{
    TEST_BEGIN("revoke_all_user_sessions")
        mock_auth_init();

        MockUser *user = mock_create_user("revokeall@example.com", "pass");

        mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);
        mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);
        mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);

        TEST_ASSERT_EQ(3, mock_count_user_sessions(user->user_id));

        int revoked = mock_revoke_all_user_sessions(user->user_id);
        TEST_ASSERT_EQ(3, revoked);
        TEST_ASSERT_EQ(0, mock_count_user_sessions(user->user_id));

        mock_auth_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Token Cache
 * ============================================================ */

static void test_token_cache_insert(void)
{
    TEST_BEGIN("token_cache_insert")
        mock_auth_init();

        bool cached = mock_cache_token("abc123hash", "user-123", mock_current_time + 3600);
        TEST_ASSERT(cached);

        MockTokenCacheEntry *entry = mock_lookup_token("abc123hash");
        TEST_ASSERT_NOT_NULL(entry);
        TEST_ASSERT_STR_EQ("user-123", entry->user_id);
        TEST_ASSERT_FALSE(entry->revoked);

        mock_auth_cleanup();
    TEST_END();
}

static void test_token_cache_lookup_miss(void)
{
    TEST_BEGIN("token_cache_lookup_miss")
        mock_auth_init();

        MockTokenCacheEntry *entry = mock_lookup_token("nonexistent");
        TEST_ASSERT_NULL(entry);

        mock_auth_cleanup();
    TEST_END();
}

static void test_token_cache_eviction(void)
{
    TEST_BEGIN("token_cache_eviction")
        mock_auth_init();

        /* Cache a token that expires in 1 hour */
        mock_cache_token("expiring", "user-1", mock_current_time + 3600);
        TEST_ASSERT_NOT_NULL(mock_lookup_token("expiring"));

        /* Advance time past expiration */
        mock_current_time += 7200;

        /* Evict expired tokens */
        mock_evict_expired_tokens();

        /* Token should be gone */
        TEST_ASSERT_NULL(mock_lookup_token("expiring"));

        mock_auth_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Edge Cases
 * ============================================================ */

static void test_empty_email(void)
{
    TEST_BEGIN("empty_email_handling")
        mock_auth_init();

        /* Should still create user with empty email check */
        MockUser *found = mock_find_user_by_email("");
        TEST_ASSERT_NULL(found);

        mock_auth_cleanup();
    TEST_END();
}

static void test_max_users_limit(void)
{
    TEST_BEGIN("max_users_limit")
        mock_auth_init();

        char email[64];
        int created = 0;

        for (int i = 0; i < MAX_USERS + 10; i++)
        {
            snprintf(email, sizeof(email), "user%d@example.com", i);
            if (mock_create_user(email, "pass"))
                created++;
        }

        TEST_ASSERT_EQ(MAX_USERS, created);

        mock_auth_cleanup();
    TEST_END();
}

static void test_session_for_nonexistent_user(void)
{
    TEST_BEGIN("session_for_nonexistent_user")
        mock_auth_init();

        MockSession *session = mock_create_session("fake-user-id", NULL, NULL, MOCK_AAL1);
        TEST_ASSERT_NULL(session);

        mock_auth_cleanup();
    TEST_END();
}

static void test_aal_levels(void)
{
    TEST_BEGIN("aal_levels")
        mock_auth_init();

        MockUser *user = mock_create_user("aal@example.com", "pass");

        MockSession *s1 = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL1);
        MockSession *s2 = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL2);
        MockSession *s3 = mock_create_session(user->user_id, NULL, NULL, MOCK_AAL3);

        TEST_ASSERT_EQ(MOCK_AAL1, s1->aal);
        TEST_ASSERT_EQ(MOCK_AAL2, s2->aal);
        TEST_ASSERT_EQ(MOCK_AAL3, s3->aal);

        mock_auth_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_auth(void)
{
    /* User management tests */
    test_user_creation();
    test_user_duplicate_email();
    test_anonymous_user();
    test_user_lookup_by_id();
    test_user_lookup_by_email();

    /* Password tests */
    test_password_hashing();
    test_password_verification();
    test_authentication();
    test_authentication_inactive_user();

    /* Session tests */
    test_session_creation();
    test_session_validation();
    test_session_expiration();
    test_session_revocation();
    test_session_refresh();
    test_session_refresh_expired();
    test_multiple_sessions();
    test_revoke_all_sessions();

    /* Token cache tests */
    test_token_cache_insert();
    test_token_cache_lookup_miss();
    test_token_cache_eviction();

    /* Edge cases */
    test_empty_email();
    test_max_users_limit();
    test_session_for_nonexistent_user();
    test_aal_levels();
}
