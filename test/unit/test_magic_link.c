/*-------------------------------------------------------------------------
 *
 * test_magic_link.c
 *    Unit tests for Orochi DB Magic Link and OTP authentication
 *
 * Tests covered:
 *   - Magic link token generation
 *   - Magic link verification
 *   - OTP (One-Time Password) generation
 *   - OTP verification
 *   - Rate limiting
 *   - Token expiration
 *   - Email/phone validation
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

#define MAX_TOKENS          200
#define MAX_OTP_ATTEMPTS    5
#define UUID_LEN            36
#define TOKEN_LEN           64
#define OTP_LEN             6
#define OTP_BUF_LEN         16  /* Extra buffer space for OTP storage */
#define MAGIC_LINK_TIMEOUT  3600     /* 1 hour */
#define OTP_TIMEOUT         300      /* 5 minutes */
#define RATE_LIMIT_WINDOW   3600     /* 1 hour window */
#define MAX_REQUESTS_HOUR   30       /* Max 30 requests per hour */

/* Token type */
typedef enum MockTokenType
{
    MOCK_TOKEN_MAGIC_LINK = 0,
    MOCK_TOKEN_EMAIL_OTP,
    MOCK_TOKEN_PHONE_OTP,
    MOCK_TOKEN_EMAIL_CONFIRM,
    MOCK_TOKEN_PASSWORD_RESET,
    MOCK_TOKEN_EMAIL_CHANGE
} MockTokenType;

/* Token status */
typedef enum MockTokenStatus
{
    MOCK_TOKEN_PENDING = 0,
    MOCK_TOKEN_VERIFIED,
    MOCK_TOKEN_EXPIRED,
    MOCK_TOKEN_REVOKED
} MockTokenStatus;

/* Token record */
typedef struct MockAuthToken
{
    char                token_id[UUID_LEN + 1];
    char                token_value[TOKEN_LEN + 1];
    char                otp_code[OTP_BUF_LEN];
    MockTokenType       type;
    MockTokenStatus     status;
    char                user_id[UUID_LEN + 1];
    char                email[256];
    char                phone[32];
    char                redirect_url[512];
    int64               created_at;
    int64               expires_at;
    int64               verified_at;
    int                 verification_attempts;
    char                ip_address[46];
    char                user_agent[256];
    bool                in_use;
} MockAuthToken;

/* Rate limit record */
typedef struct MockRateLimit
{
    char                identifier[256];    /* Email or phone */
    MockTokenType       type;
    int                 request_count;
    int64               window_start;
    bool                in_use;
} MockRateLimit;

/* Global state */
static MockAuthToken mock_tokens[MAX_TOKENS];
static MockRateLimit mock_rate_limits[100];
static int64 mock_current_time = 1704067200;

/* ============================================================
 * Mock Utility Functions
 * ============================================================ */

static void mock_magic_init(void)
{
    memset(mock_tokens, 0, sizeof(mock_tokens));
    memset(mock_rate_limits, 0, sizeof(mock_rate_limits));
    mock_current_time = 1704067200;
}

static void mock_magic_cleanup(void)
{
    memset(mock_tokens, 0, sizeof(mock_tokens));
    memset(mock_rate_limits, 0, sizeof(mock_rate_limits));
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

static void mock_generate_secure_token(char *buf, int len)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    for (int i = 0; i < len - 1; i++)
    {
        buf[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buf[len - 1] = '\0';
}

static void mock_generate_otp(char *buf)
{
    /* Generate 6-digit OTP */
    int otp = rand() % 1000000;
    snprintf(buf, OTP_BUF_LEN, "%06d", otp);
}

/* Validate email format (simple check) */
static bool mock_validate_email(const char *email)
{
    if (!email || strlen(email) < 5)
        return false;

    const char *at = strchr(email, '@');
    if (!at || at == email)
        return false;

    const char *dot = strchr(at, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0')
        return false;

    return true;
}

/* Validate phone format (simple check - E.164) */
static bool mock_validate_phone(const char *phone)
{
    if (!phone || strlen(phone) < 8 || strlen(phone) > 15)
        return false;

    const char *p = phone;
    if (*p == '+') p++;

    while (*p)
    {
        if (*p < '0' || *p > '9')
            return false;
        p++;
    }

    return true;
}

/* ============================================================
 * Mock Rate Limiting
 * ============================================================ */

static MockRateLimit *mock_find_rate_limit(const char *identifier, MockTokenType type)
{
    for (int i = 0; i < 100; i++)
    {
        if (mock_rate_limits[i].in_use &&
            mock_rate_limits[i].type == type &&
            strcmp(mock_rate_limits[i].identifier, identifier) == 0)
            return &mock_rate_limits[i];
    }
    return NULL;
}

static bool mock_check_rate_limit(const char *identifier, MockTokenType type)
{
    MockRateLimit *limit = mock_find_rate_limit(identifier, type);

    if (limit)
    {
        /* Check if window has expired */
        if (mock_current_time > limit->window_start + RATE_LIMIT_WINDOW)
        {
            /* Reset the window */
            limit->request_count = 0;
            limit->window_start = mock_current_time;
        }

        if (limit->request_count >= MAX_REQUESTS_HOUR)
            return false;

        limit->request_count++;
        return true;
    }

    /* Create new rate limit entry */
    for (int i = 0; i < 100; i++)
    {
        if (!mock_rate_limits[i].in_use)
        {
            strncpy(mock_rate_limits[i].identifier, identifier,
                    sizeof(mock_rate_limits[i].identifier) - 1);
            mock_rate_limits[i].type = type;
            mock_rate_limits[i].request_count = 1;
            mock_rate_limits[i].window_start = mock_current_time;
            mock_rate_limits[i].in_use = true;
            return true;
        }
    }

    return false;  /* Rate limit table full */
}

/* ============================================================
 * Mock Magic Link Functions
 * ============================================================ */

/* Create a magic link token */
static MockAuthToken *mock_create_magic_link(const char *email, const char *redirect_url,
                                              const char *ip_address)
{
    if (!mock_validate_email(email))
        return NULL;

    /* Check rate limit */
    if (!mock_check_rate_limit(email, MOCK_TOKEN_MAGIC_LINK))
        return NULL;

    /* Find free slot */
    MockAuthToken *token = NULL;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!mock_tokens[i].in_use)
        {
            token = &mock_tokens[i];
            break;
        }
    }
    if (!token)
        return NULL;

    mock_generate_uuid(token->token_id);
    mock_generate_secure_token(token->token_value, TOKEN_LEN);
    token->type = MOCK_TOKEN_MAGIC_LINK;
    token->status = MOCK_TOKEN_PENDING;
    strncpy(token->email, email, sizeof(token->email) - 1);

    if (redirect_url)
        strncpy(token->redirect_url, redirect_url, sizeof(token->redirect_url) - 1);
    if (ip_address)
        strncpy(token->ip_address, ip_address, sizeof(token->ip_address) - 1);

    token->created_at = mock_current_time;
    token->expires_at = mock_current_time + MAGIC_LINK_TIMEOUT;
    token->verification_attempts = 0;
    token->in_use = true;

    return token;
}

/* Find token by value */
static MockAuthToken *mock_find_token_by_value(const char *token_value)
{
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_tokens[i].in_use &&
            strcmp(mock_tokens[i].token_value, token_value) == 0)
            return &mock_tokens[i];
    }
    return NULL;
}

/* Find token by ID */
static MockAuthToken *mock_find_token_by_id(const char *token_id)
{
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_tokens[i].in_use &&
            strcmp(mock_tokens[i].token_id, token_id) == 0)
            return &mock_tokens[i];
    }
    return NULL;
}

/* Verify magic link token */
typedef struct MockVerifyResult
{
    bool success;
    char error[256];
    char email[256];
    char redirect_url[512];
} MockVerifyResult;

static MockVerifyResult mock_verify_magic_link(const char *token_value)
{
    MockVerifyResult result = {0};

    MockAuthToken *token = mock_find_token_by_value(token_value);
    if (!token)
    {
        strcpy(result.error, "Token not found");
        return result;
    }

    if (token->type != MOCK_TOKEN_MAGIC_LINK)
    {
        strcpy(result.error, "Invalid token type");
        return result;
    }

    if (token->status == MOCK_TOKEN_VERIFIED)
    {
        strcpy(result.error, "Token already used");
        return result;
    }

    if (token->status == MOCK_TOKEN_REVOKED)
    {
        strcpy(result.error, "Token has been revoked");
        return result;
    }

    if (mock_current_time > token->expires_at)
    {
        token->status = MOCK_TOKEN_EXPIRED;
        strcpy(result.error, "Token has expired");
        return result;
    }

    /* Token is valid */
    token->status = MOCK_TOKEN_VERIFIED;
    token->verified_at = mock_current_time;

    result.success = true;
    strcpy(result.email, token->email);
    if (token->redirect_url[0])
        strcpy(result.redirect_url, token->redirect_url);

    return result;
}

/* ============================================================
 * Mock OTP Functions
 * ============================================================ */

/* Create email OTP */
static MockAuthToken *mock_create_email_otp(const char *email, const char *ip_address)
{
    if (!mock_validate_email(email))
        return NULL;

    if (!mock_check_rate_limit(email, MOCK_TOKEN_EMAIL_OTP))
        return NULL;

    MockAuthToken *token = NULL;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!mock_tokens[i].in_use)
        {
            token = &mock_tokens[i];
            break;
        }
    }
    if (!token)
        return NULL;

    mock_generate_uuid(token->token_id);
    mock_generate_otp(token->otp_code);
    token->token_value[0] = '\0';  /* OTP doesn't use token_value */
    token->type = MOCK_TOKEN_EMAIL_OTP;
    token->status = MOCK_TOKEN_PENDING;
    strncpy(token->email, email, sizeof(token->email) - 1);

    if (ip_address)
        strncpy(token->ip_address, ip_address, sizeof(token->ip_address) - 1);

    token->created_at = mock_current_time;
    token->expires_at = mock_current_time + OTP_TIMEOUT;
    token->verification_attempts = 0;
    token->in_use = true;

    return token;
}

/* Create phone OTP */
static MockAuthToken *mock_create_phone_otp(const char *phone, const char *ip_address)
{
    if (!mock_validate_phone(phone))
        return NULL;

    if (!mock_check_rate_limit(phone, MOCK_TOKEN_PHONE_OTP))
        return NULL;

    MockAuthToken *token = NULL;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!mock_tokens[i].in_use)
        {
            token = &mock_tokens[i];
            break;
        }
    }
    if (!token)
        return NULL;

    mock_generate_uuid(token->token_id);
    mock_generate_otp(token->otp_code);
    token->type = MOCK_TOKEN_PHONE_OTP;
    token->status = MOCK_TOKEN_PENDING;
    strncpy(token->phone, phone, sizeof(token->phone) - 1);

    if (ip_address)
        strncpy(token->ip_address, ip_address, sizeof(token->ip_address) - 1);

    token->created_at = mock_current_time;
    token->expires_at = mock_current_time + OTP_TIMEOUT;
    token->verification_attempts = 0;
    token->in_use = true;

    return token;
}

/* Verify OTP */
static MockVerifyResult mock_verify_otp(const char *token_id, const char *otp_code)
{
    MockVerifyResult result = {0};

    MockAuthToken *token = mock_find_token_by_id(token_id);
    if (!token)
    {
        strcpy(result.error, "Token not found");
        return result;
    }

    if (token->type != MOCK_TOKEN_EMAIL_OTP && token->type != MOCK_TOKEN_PHONE_OTP)
    {
        strcpy(result.error, "Invalid token type");
        return result;
    }

    if (token->status == MOCK_TOKEN_VERIFIED)
    {
        strcpy(result.error, "OTP already used");
        return result;
    }

    if (token->status == MOCK_TOKEN_REVOKED)
    {
        strcpy(result.error, "OTP has been revoked");
        return result;
    }

    if (mock_current_time > token->expires_at)
    {
        token->status = MOCK_TOKEN_EXPIRED;
        strcpy(result.error, "OTP has expired");
        return result;
    }

    /* Check attempt limit */
    token->verification_attempts++;
    if (token->verification_attempts > MAX_OTP_ATTEMPTS)
    {
        token->status = MOCK_TOKEN_REVOKED;
        strcpy(result.error, "Too many failed attempts");
        return result;
    }

    /* Verify OTP code */
    if (strcmp(token->otp_code, otp_code) != 0)
    {
        snprintf(result.error, sizeof(result.error),
                 "Invalid OTP code (%d attempts remaining)",
                 MAX_OTP_ATTEMPTS - token->verification_attempts);
        return result;
    }

    /* OTP is valid */
    token->status = MOCK_TOKEN_VERIFIED;
    token->verified_at = mock_current_time;

    result.success = true;
    if (token->email[0])
        strcpy(result.email, token->email);

    return result;
}

/* ============================================================
 * Mock Token Management
 * ============================================================ */

/* Revoke all pending tokens for email */
static int mock_revoke_tokens_for_email(const char *email, MockTokenType type)
{
    int count = 0;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_tokens[i].in_use &&
            mock_tokens[i].type == type &&
            mock_tokens[i].status == MOCK_TOKEN_PENDING &&
            strcmp(mock_tokens[i].email, email) == 0)
        {
            mock_tokens[i].status = MOCK_TOKEN_REVOKED;
            count++;
        }
    }
    return count;
}

/* Count pending tokens for identifier */
static int mock_count_pending_tokens(const char *identifier, MockTokenType type)
{
    int count = 0;
    const char *field;

    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!mock_tokens[i].in_use || mock_tokens[i].type != type)
            continue;

        if (mock_tokens[i].status != MOCK_TOKEN_PENDING)
            continue;

        field = (type == MOCK_TOKEN_PHONE_OTP) ?
                mock_tokens[i].phone : mock_tokens[i].email;

        if (strcmp(field, identifier) == 0)
            count++;
    }
    return count;
}

/* Cleanup expired tokens */
static int mock_cleanup_expired_tokens(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (mock_tokens[i].in_use &&
            mock_tokens[i].status == MOCK_TOKEN_PENDING &&
            mock_current_time > mock_tokens[i].expires_at)
        {
            mock_tokens[i].status = MOCK_TOKEN_EXPIRED;
            count++;
        }
    }
    return count;
}

/* ============================================================
 * Test Cases: Email Validation
 * ============================================================ */

static void test_email_validation_valid(void)
{
    TEST_BEGIN("email_validation_valid")
        TEST_ASSERT(mock_validate_email("user@example.com"));
        TEST_ASSERT(mock_validate_email("test.user@subdomain.example.co.uk"));
        TEST_ASSERT(mock_validate_email("a@b.c"));
    TEST_END();
}

static void test_email_validation_invalid(void)
{
    TEST_BEGIN("email_validation_invalid")
        TEST_ASSERT_FALSE(mock_validate_email(NULL));
        TEST_ASSERT_FALSE(mock_validate_email(""));
        TEST_ASSERT_FALSE(mock_validate_email("invalid"));
        TEST_ASSERT_FALSE(mock_validate_email("@example.com"));
        TEST_ASSERT_FALSE(mock_validate_email("user@"));
        TEST_ASSERT_FALSE(mock_validate_email("user@.com"));
        TEST_ASSERT_FALSE(mock_validate_email("user@com."));
    TEST_END();
}

/* ============================================================
 * Test Cases: Phone Validation
 * ============================================================ */

static void test_phone_validation_valid(void)
{
    TEST_BEGIN("phone_validation_valid")
        TEST_ASSERT(mock_validate_phone("+14155551234"));
        TEST_ASSERT(mock_validate_phone("14155551234"));
        TEST_ASSERT(mock_validate_phone("+442071234567"));
        TEST_ASSERT(mock_validate_phone("12345678"));
    TEST_END();
}

static void test_phone_validation_invalid(void)
{
    TEST_BEGIN("phone_validation_invalid")
        TEST_ASSERT_FALSE(mock_validate_phone(NULL));
        TEST_ASSERT_FALSE(mock_validate_phone(""));
        TEST_ASSERT_FALSE(mock_validate_phone("123"));           /* Too short */
        TEST_ASSERT_FALSE(mock_validate_phone("12345678901234567")); /* Too long */
        TEST_ASSERT_FALSE(mock_validate_phone("abc12345678"));   /* Letters */
        TEST_ASSERT_FALSE(mock_validate_phone("123-456-7890")); /* Dashes */
    TEST_END();
}

/* ============================================================
 * Test Cases: Magic Link Generation
 * ============================================================ */

static void test_magic_link_creation(void)
{
    TEST_BEGIN("magic_link_creation")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link(
            "user@example.com", "https://app.com/dashboard", "192.168.1.1");

        TEST_ASSERT_NOT_NULL(token);
        TEST_ASSERT_EQ(MOCK_TOKEN_MAGIC_LINK, token->type);
        TEST_ASSERT_EQ(MOCK_TOKEN_PENDING, token->status);
        TEST_ASSERT_STR_EQ("user@example.com", token->email);
        TEST_ASSERT_STR_EQ("https://app.com/dashboard", token->redirect_url);
        TEST_ASSERT_GT(strlen(token->token_value), 0);
        TEST_ASSERT_GT(token->expires_at, mock_current_time);

        mock_magic_cleanup();
    TEST_END();
}

static void test_magic_link_invalid_email(void)
{
    TEST_BEGIN("magic_link_invalid_email")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link("invalid-email", NULL, NULL);
        TEST_ASSERT_NULL(token);

        mock_magic_cleanup();
    TEST_END();
}

static void test_magic_link_unique_tokens(void)
{
    TEST_BEGIN("magic_link_unique_tokens")
        mock_magic_init();

        MockAuthToken *t1 = mock_create_magic_link("user1@example.com", NULL, NULL);
        MockAuthToken *t2 = mock_create_magic_link("user2@example.com", NULL, NULL);

        TEST_ASSERT_NOT_NULL(t1);
        TEST_ASSERT_NOT_NULL(t2);
        TEST_ASSERT_NEQ(0, strcmp(t1->token_value, t2->token_value));
        TEST_ASSERT_NEQ(0, strcmp(t1->token_id, t2->token_id));

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Magic Link Verification
 * ============================================================ */

static void test_magic_link_verify_success(void)
{
    TEST_BEGIN("magic_link_verify_success")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link(
            "verify@example.com", "https://app.com/callback", NULL);

        MockVerifyResult result = mock_verify_magic_link(token->token_value);

        TEST_ASSERT(result.success);
        TEST_ASSERT_STR_EQ("verify@example.com", result.email);
        TEST_ASSERT_STR_EQ("https://app.com/callback", result.redirect_url);
        TEST_ASSERT_EQ(MOCK_TOKEN_VERIFIED, token->status);

        mock_magic_cleanup();
    TEST_END();
}

static void test_magic_link_verify_not_found(void)
{
    TEST_BEGIN("magic_link_verify_not_found")
        mock_magic_init();

        MockVerifyResult result = mock_verify_magic_link("nonexistent-token");

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "not found"));

        mock_magic_cleanup();
    TEST_END();
}

static void test_magic_link_verify_expired(void)
{
    TEST_BEGIN("magic_link_verify_expired")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link("expire@example.com", NULL, NULL);

        /* Advance time past expiration */
        mock_current_time += MAGIC_LINK_TIMEOUT + 1;

        MockVerifyResult result = mock_verify_magic_link(token->token_value);

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "expired"));
        TEST_ASSERT_EQ(MOCK_TOKEN_EXPIRED, token->status);

        mock_magic_cleanup();
    TEST_END();
}

static void test_magic_link_verify_reuse_prevented(void)
{
    TEST_BEGIN("magic_link_verify_reuse_prevented")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link("reuse@example.com", NULL, NULL);

        /* First verification */
        MockVerifyResult result1 = mock_verify_magic_link(token->token_value);
        TEST_ASSERT(result1.success);

        /* Second verification should fail */
        MockVerifyResult result2 = mock_verify_magic_link(token->token_value);
        TEST_ASSERT_FALSE(result2.success);
        TEST_ASSERT_NOT_NULL(strstr(result2.error, "already used"));

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: OTP Generation
 * ============================================================ */

static void test_otp_email_creation(void)
{
    TEST_BEGIN("otp_email_creation")
        mock_magic_init();

        MockAuthToken *token = mock_create_email_otp("otp@example.com", "10.0.0.1");

        TEST_ASSERT_NOT_NULL(token);
        TEST_ASSERT_EQ(MOCK_TOKEN_EMAIL_OTP, token->type);
        TEST_ASSERT_EQ(6, strlen(token->otp_code));
        TEST_ASSERT_STR_EQ("otp@example.com", token->email);

        /* OTP should be numeric */
        for (int i = 0; i < 6; i++)
            TEST_ASSERT(token->otp_code[i] >= '0' && token->otp_code[i] <= '9');

        mock_magic_cleanup();
    TEST_END();
}

static void test_otp_phone_creation(void)
{
    TEST_BEGIN("otp_phone_creation")
        mock_magic_init();

        MockAuthToken *token = mock_create_phone_otp("+14155551234", NULL);

        TEST_ASSERT_NOT_NULL(token);
        TEST_ASSERT_EQ(MOCK_TOKEN_PHONE_OTP, token->type);
        TEST_ASSERT_EQ(6, strlen(token->otp_code));
        TEST_ASSERT_STR_EQ("+14155551234", token->phone);

        mock_magic_cleanup();
    TEST_END();
}

static void test_otp_unique_codes(void)
{
    TEST_BEGIN("otp_unique_codes")
        mock_magic_init();

        /* Generate multiple OTPs - at least some should be different */
        char codes[10][OTP_BUF_LEN];
        for (int i = 0; i < 10; i++)
        {
            char email[64];
            snprintf(email, sizeof(email), "user%d@example.com", i);
            MockAuthToken *token = mock_create_email_otp(email, NULL);
            TEST_ASSERT_NOT_NULL(token);
            strcpy(codes[i], token->otp_code);
        }

        /* Check for at least some uniqueness */
        int unique = 0;
        for (int i = 1; i < 10; i++)
        {
            if (strcmp(codes[i], codes[0]) != 0)
                unique++;
        }
        TEST_ASSERT_GT(unique, 0);

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: OTP Verification
 * ============================================================ */

static void test_otp_verify_success(void)
{
    TEST_BEGIN("otp_verify_success")
        mock_magic_init();

        MockAuthToken *token = mock_create_email_otp("verify@example.com", NULL);

        MockVerifyResult result = mock_verify_otp(token->token_id, token->otp_code);

        TEST_ASSERT(result.success);
        TEST_ASSERT_STR_EQ("verify@example.com", result.email);
        TEST_ASSERT_EQ(MOCK_TOKEN_VERIFIED, token->status);

        mock_magic_cleanup();
    TEST_END();
}

static void test_otp_verify_wrong_code(void)
{
    TEST_BEGIN("otp_verify_wrong_code")
        mock_magic_init();

        MockAuthToken *token = mock_create_email_otp("wrong@example.com", NULL);

        MockVerifyResult result = mock_verify_otp(token->token_id, "000000");

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "Invalid"));
        TEST_ASSERT_EQ(MOCK_TOKEN_PENDING, token->status);
        TEST_ASSERT_EQ(1, token->verification_attempts);

        mock_magic_cleanup();
    TEST_END();
}

static void test_otp_verify_max_attempts(void)
{
    TEST_BEGIN("otp_verify_max_attempts")
        mock_magic_init();

        MockAuthToken *token = mock_create_email_otp("attempts@example.com", NULL);

        /* Try wrong code multiple times */
        for (int i = 0; i < MAX_OTP_ATTEMPTS; i++)
        {
            MockVerifyResult result = mock_verify_otp(token->token_id, "999999");
            TEST_ASSERT_FALSE(result.success);
        }

        /* One more attempt should show rate limit error */
        MockVerifyResult result = mock_verify_otp(token->token_id, "999999");
        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "Too many"));
        TEST_ASSERT_EQ(MOCK_TOKEN_REVOKED, token->status);

        /* Even correct code should fail now */
        MockVerifyResult result2 = mock_verify_otp(token->token_id, token->otp_code);
        TEST_ASSERT_FALSE(result2.success);

        mock_magic_cleanup();
    TEST_END();
}

static void test_otp_verify_expired(void)
{
    TEST_BEGIN("otp_verify_expired")
        mock_magic_init();

        MockAuthToken *token = mock_create_email_otp("expire@example.com", NULL);

        /* Advance time past expiration */
        mock_current_time += OTP_TIMEOUT + 1;

        MockVerifyResult result = mock_verify_otp(token->token_id, token->otp_code);

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "expired"));

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Rate Limiting
 * ============================================================ */

static void test_rate_limit_magic_link(void)
{
    TEST_BEGIN("rate_limit_magic_link")
        mock_magic_init();

        /* Should allow up to MAX_REQUESTS_HOUR */
        for (int i = 0; i < MAX_REQUESTS_HOUR; i++)
        {
            MockAuthToken *token = mock_create_magic_link("ratelimit@example.com", NULL, NULL);
            TEST_ASSERT_NOT_NULL(token);
        }

        /* Next request should fail */
        MockAuthToken *token = mock_create_magic_link("ratelimit@example.com", NULL, NULL);
        TEST_ASSERT_NULL(token);

        mock_magic_cleanup();
    TEST_END();
}

static void test_rate_limit_reset(void)
{
    TEST_BEGIN("rate_limit_reset")
        mock_magic_init();

        /* Use up rate limit */
        for (int i = 0; i < MAX_REQUESTS_HOUR; i++)
        {
            mock_create_magic_link("reset@example.com", NULL, NULL);
        }

        /* Should be rate limited */
        TEST_ASSERT_NULL(mock_create_magic_link("reset@example.com", NULL, NULL));

        /* Advance time past window */
        mock_current_time += RATE_LIMIT_WINDOW + 1;

        /* Should work again */
        MockAuthToken *token = mock_create_magic_link("reset@example.com", NULL, NULL);
        TEST_ASSERT_NOT_NULL(token);

        mock_magic_cleanup();
    TEST_END();
}

static void test_rate_limit_per_identifier(void)
{
    TEST_BEGIN("rate_limit_per_identifier")
        mock_magic_init();

        /* Rate limit one email */
        for (int i = 0; i < MAX_REQUESTS_HOUR; i++)
        {
            mock_create_magic_link("user1@example.com", NULL, NULL);
        }

        /* Different email should still work */
        MockAuthToken *token = mock_create_magic_link("user2@example.com", NULL, NULL);
        TEST_ASSERT_NOT_NULL(token);

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Token Management
 * ============================================================ */

static void test_revoke_pending_tokens(void)
{
    TEST_BEGIN("revoke_pending_tokens")
        mock_magic_init();

        /* Create multiple tokens for same email */
        mock_create_magic_link("revoke@example.com", NULL, NULL);
        mock_create_magic_link("revoke@example.com", NULL, NULL);
        mock_create_magic_link("revoke@example.com", NULL, NULL);

        int count = mock_count_pending_tokens("revoke@example.com", MOCK_TOKEN_MAGIC_LINK);
        TEST_ASSERT_EQ(3, count);

        /* Revoke all */
        int revoked = mock_revoke_tokens_for_email("revoke@example.com", MOCK_TOKEN_MAGIC_LINK);
        TEST_ASSERT_EQ(3, revoked);

        count = mock_count_pending_tokens("revoke@example.com", MOCK_TOKEN_MAGIC_LINK);
        TEST_ASSERT_EQ(0, count);

        mock_magic_cleanup();
    TEST_END();
}

static void test_cleanup_expired_tokens(void)
{
    TEST_BEGIN("cleanup_expired_tokens")
        mock_magic_init();

        /* Create some tokens */
        mock_create_magic_link("cleanup1@example.com", NULL, NULL);
        mock_create_magic_link("cleanup2@example.com", NULL, NULL);
        mock_create_email_otp("cleanup3@example.com", NULL);

        /* Advance time to expire some */
        mock_current_time += OTP_TIMEOUT + 1;  /* OTP expired */

        int expired = mock_cleanup_expired_tokens();
        TEST_ASSERT_EQ(1, expired);  /* Only OTP should be expired */

        /* Advance more time */
        mock_current_time += MAGIC_LINK_TIMEOUT;

        expired = mock_cleanup_expired_tokens();
        TEST_ASSERT_EQ(2, expired);  /* Magic links now expired */

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Edge Cases
 * ============================================================ */

static void test_otp_leading_zeros(void)
{
    TEST_BEGIN("otp_leading_zeros")
        mock_magic_init();

        /* Create OTPs until we get one with leading zeros */
        bool found_leading_zero = false;
        for (int i = 0; i < 100 && !found_leading_zero; i++)
        {
            char email[64];
            snprintf(email, sizeof(email), "user%d@example.com", i);
            MockAuthToken *token = mock_create_email_otp(email, NULL);
            if (token && token->otp_code[0] == '0')
            {
                found_leading_zero = true;
                /* Verify it still works */
                MockVerifyResult result = mock_verify_otp(token->token_id, token->otp_code);
                TEST_ASSERT(result.success);
            }
        }

        /* We may or may not find one - that's OK */
        mock_magic_cleanup();
    TEST_END();
}

static void test_empty_redirect_url(void)
{
    TEST_BEGIN("empty_redirect_url")
        mock_magic_init();

        MockAuthToken *token = mock_create_magic_link("noredirect@example.com", NULL, NULL);
        TEST_ASSERT_NOT_NULL(token);
        TEST_ASSERT_EQ(0, strlen(token->redirect_url));

        MockVerifyResult result = mock_verify_magic_link(token->token_value);
        TEST_ASSERT(result.success);
        TEST_ASSERT_EQ(0, strlen(result.redirect_url));

        mock_magic_cleanup();
    TEST_END();
}

static void test_token_lookup_methods(void)
{
    TEST_BEGIN("token_lookup_methods")
        mock_magic_init();

        MockAuthToken *created = mock_create_magic_link("lookup@example.com", NULL, NULL);

        /* Lookup by value */
        MockAuthToken *by_value = mock_find_token_by_value(created->token_value);
        TEST_ASSERT_NOT_NULL(by_value);
        TEST_ASSERT_STR_EQ(created->token_id, by_value->token_id);

        /* Lookup by ID */
        MockAuthToken *by_id = mock_find_token_by_id(created->token_id);
        TEST_ASSERT_NOT_NULL(by_id);
        TEST_ASSERT_STR_EQ(created->token_value, by_id->token_value);

        mock_magic_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_magic_link(void)
{
    /* Validation tests */
    test_email_validation_valid();
    test_email_validation_invalid();
    test_phone_validation_valid();
    test_phone_validation_invalid();

    /* Magic link generation tests */
    test_magic_link_creation();
    test_magic_link_invalid_email();
    test_magic_link_unique_tokens();

    /* Magic link verification tests */
    test_magic_link_verify_success();
    test_magic_link_verify_not_found();
    test_magic_link_verify_expired();
    test_magic_link_verify_reuse_prevented();

    /* OTP generation tests */
    test_otp_email_creation();
    test_otp_phone_creation();
    test_otp_unique_codes();

    /* OTP verification tests */
    test_otp_verify_success();
    test_otp_verify_wrong_code();
    test_otp_verify_max_attempts();
    test_otp_verify_expired();

    /* Rate limiting tests */
    test_rate_limit_magic_link();
    test_rate_limit_reset();
    test_rate_limit_per_identifier();

    /* Token management tests */
    test_revoke_pending_tokens();
    test_cleanup_expired_tokens();

    /* Edge cases */
    test_otp_leading_zeros();
    test_empty_redirect_url();
    test_token_lookup_methods();
}
