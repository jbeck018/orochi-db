/*-------------------------------------------------------------------------
 *
 * test_webauthn.c
 *    Unit tests for Orochi DB WebAuthn/FIDO2 passwordless authentication
 *
 * Tests covered:
 *   - Challenge generation and validation
 *   - Credential registration flow
 *   - Authentication assertion verification
 *   - Sign count validation (replay protection)
 *   - Device/credential management
 *   - Multi-credential scenarios
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

#define MAX_CREDENTIALS     50
#define MAX_CHALLENGES      100
#define UUID_LEN            36
#define CHALLENGE_LEN       32
#define CREDENTIAL_ID_LEN   64
#define PUBLIC_KEY_LEN      256
#define CHALLENGE_TIMEOUT   300  /* 5 minutes */

/* Credential types */
typedef enum MockCredentialType
{
    MOCK_CREDENTIAL_PASSKEY = 0,
    MOCK_CREDENTIAL_SECURITY_KEY,
    MOCK_CREDENTIAL_PLATFORM,
    MOCK_CREDENTIAL_CROSS_PLATFORM
} MockCredentialType;

/* Device types */
typedef enum MockDeviceType
{
    MOCK_DEVICE_WINDOWS_HELLO = 0,
    MOCK_DEVICE_APPLE_TOUCH_ID,
    MOCK_DEVICE_APPLE_FACE_ID,
    MOCK_DEVICE_ANDROID_BIOMETRIC,
    MOCK_DEVICE_YUBIKEY,
    MOCK_DEVICE_OTHER
} MockDeviceType;

/* Credential status */
typedef enum MockCredentialStatus
{
    MOCK_CRED_PENDING = 0,
    MOCK_CRED_ACTIVE,
    MOCK_CRED_SUSPENDED,
    MOCK_CRED_REVOKED
} MockCredentialStatus;

/* Challenge type */
typedef enum MockChallengeType
{
    MOCK_CHALLENGE_REGISTRATION = 0,
    MOCK_CHALLENGE_AUTHENTICATION
} MockChallengeType;

/* User verification requirement */
typedef enum MockUserVerification
{
    MOCK_UV_REQUIRED = 0,
    MOCK_UV_PREFERRED,
    MOCK_UV_DISCOURAGED
} MockUserVerification;

/* WebAuthn credential */
typedef struct MockWebAuthnCredential
{
    char                    credential_id[UUID_LEN + 1];
    char                    user_id[UUID_LEN + 1];
    unsigned char           webauthn_cred_id[CREDENTIAL_ID_LEN];
    int                     webauthn_cred_id_len;
    unsigned char           public_key[PUBLIC_KEY_LEN];
    int                     public_key_len;
    int                     algorithm;          /* COSE algorithm */
    MockCredentialType      cred_type;
    MockDeviceType          device_type;
    char                    device_name[128];
    char                    transports[128];    /* Comma-separated: usb,nfc,ble,internal */
    uint64                  sign_count;
    bool                    backup_eligible;
    bool                    backup_state;
    MockCredentialStatus    status;
    int64                   created_at;
    int64                   last_used_at;
    char                    last_used_ip[46];
    bool                    in_use;
} MockWebAuthnCredential;

/* WebAuthn challenge */
typedef struct MockWebAuthnChallenge
{
    char                    challenge_id[UUID_LEN + 1];
    unsigned char           challenge[CHALLENGE_LEN];
    MockChallengeType       type;
    char                    user_id[UUID_LEN + 1];
    char                    rp_id[256];
    char                    origin[256];
    MockUserVerification    user_verification;
    int64                   created_at;
    int64                   expires_at;
    bool                    consumed;
    bool                    in_use;
} MockWebAuthnChallenge;

/* Authenticator data (simplified) */
typedef struct MockAuthenticatorData
{
    unsigned char           rp_id_hash[32];
    unsigned char           flags;              /* Bit flags */
    uint32                  sign_count;
    bool                    user_present;
    bool                    user_verified;
    bool                    backup_eligible;
    bool                    backup_state;
} MockAuthenticatorData;

/* Global state */
static MockWebAuthnCredential mock_credentials[MAX_CREDENTIALS];
static MockWebAuthnChallenge mock_challenges[MAX_CHALLENGES];
static int64 mock_current_time = 1704067200;

/* ============================================================
 * Mock Utility Functions
 * ============================================================ */

static void mock_webauthn_init(void)
{
    memset(mock_credentials, 0, sizeof(mock_credentials));
    memset(mock_challenges, 0, sizeof(mock_challenges));
    mock_current_time = 1704067200;
}

static void mock_webauthn_cleanup(void)
{
    memset(mock_credentials, 0, sizeof(mock_credentials));
    memset(mock_challenges, 0, sizeof(mock_challenges));
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

static void mock_generate_random_bytes(unsigned char *buf, int len)
{
    for (int i = 0; i < len; i++)
        buf[i] = rand() & 0xFF;
}

/* Simple hash for RP ID */
static void mock_hash_rp_id(const char *rp_id, unsigned char *hash)
{
    unsigned int h = 0x811c9dc5;
    while (*rp_id)
    {
        h ^= (unsigned char)*rp_id++;
        h *= 0x01000193;
    }

    for (int i = 0; i < 32; i++)
    {
        h = h * 1103515245 + 12345;
        hash[i] = (h >> 16) & 0xFF;
    }
}

/* ============================================================
 * Mock WebAuthn Functions
 * ============================================================ */

/* Create a challenge for registration or authentication */
static MockWebAuthnChallenge *mock_create_challenge(MockChallengeType type,
                                                     const char *user_id,
                                                     const char *rp_id,
                                                     const char *origin)
{
    MockWebAuthnChallenge *challenge = NULL;

    for (int i = 0; i < MAX_CHALLENGES; i++)
    {
        if (!mock_challenges[i].in_use)
        {
            challenge = &mock_challenges[i];
            break;
        }
    }
    if (!challenge)
        return NULL;

    mock_generate_uuid(challenge->challenge_id);
    mock_generate_random_bytes(challenge->challenge, CHALLENGE_LEN);
    challenge->type = type;

    if (user_id)
        strncpy(challenge->user_id, user_id, UUID_LEN);
    strncpy(challenge->rp_id, rp_id, sizeof(challenge->rp_id) - 1);
    strncpy(challenge->origin, origin, sizeof(challenge->origin) - 1);

    challenge->user_verification = MOCK_UV_PREFERRED;
    challenge->created_at = mock_current_time;
    challenge->expires_at = mock_current_time + CHALLENGE_TIMEOUT;
    challenge->consumed = false;
    challenge->in_use = true;

    return challenge;
}

/* Find challenge by ID */
static MockWebAuthnChallenge *mock_find_challenge(const char *challenge_id)
{
    for (int i = 0; i < MAX_CHALLENGES; i++)
    {
        if (mock_challenges[i].in_use &&
            strcmp(mock_challenges[i].challenge_id, challenge_id) == 0)
            return &mock_challenges[i];
    }
    return NULL;
}

/* Validate challenge */
static bool mock_validate_challenge(const char *challenge_id, MockChallengeType expected_type)
{
    MockWebAuthnChallenge *challenge = mock_find_challenge(challenge_id);
    if (!challenge)
        return false;

    if (challenge->consumed)
        return false;

    if (challenge->type != expected_type)
        return false;

    if (mock_current_time > challenge->expires_at)
        return false;

    return true;
}

/* Consume challenge */
static void mock_consume_challenge(const char *challenge_id)
{
    MockWebAuthnChallenge *challenge = mock_find_challenge(challenge_id);
    if (challenge)
        challenge->consumed = true;
}

/* Register a new credential */
static MockWebAuthnCredential *mock_register_credential(
    const char *challenge_id,
    const char *user_id,
    const unsigned char *cred_id, int cred_id_len,
    const unsigned char *public_key, int public_key_len,
    int algorithm,
    uint32 sign_count,
    const char *device_name,
    MockDeviceType device_type)
{
    /* Validate challenge */
    if (!mock_validate_challenge(challenge_id, MOCK_CHALLENGE_REGISTRATION))
        return NULL;

    MockWebAuthnChallenge *challenge = mock_find_challenge(challenge_id);

    /* Check user matches */
    if (strcmp(challenge->user_id, user_id) != 0)
        return NULL;

    /* Check for duplicate credential ID */
    for (int i = 0; i < MAX_CREDENTIALS; i++)
    {
        if (mock_credentials[i].in_use &&
            mock_credentials[i].webauthn_cred_id_len == cred_id_len &&
            memcmp(mock_credentials[i].webauthn_cred_id, cred_id, cred_id_len) == 0)
            return NULL;  /* Already registered */
    }

    /* Find free slot */
    MockWebAuthnCredential *cred = NULL;
    for (int i = 0; i < MAX_CREDENTIALS; i++)
    {
        if (!mock_credentials[i].in_use)
        {
            cred = &mock_credentials[i];
            break;
        }
    }
    if (!cred)
        return NULL;

    mock_generate_uuid(cred->credential_id);
    strncpy(cred->user_id, user_id, UUID_LEN);

    memcpy(cred->webauthn_cred_id, cred_id, cred_id_len);
    cred->webauthn_cred_id_len = cred_id_len;
    memcpy(cred->public_key, public_key, public_key_len);
    cred->public_key_len = public_key_len;

    cred->algorithm = algorithm;
    cred->sign_count = sign_count;
    cred->device_type = device_type;
    if (device_name)
        strncpy(cred->device_name, device_name, sizeof(cred->device_name) - 1);
    cred->cred_type = MOCK_CREDENTIAL_PASSKEY;
    cred->status = MOCK_CRED_ACTIVE;
    cred->created_at = mock_current_time;
    cred->backup_eligible = true;
    cred->backup_state = false;
    cred->in_use = true;

    /* Consume the challenge */
    mock_consume_challenge(challenge_id);

    return cred;
}

/* Find credential by WebAuthn credential ID */
static MockWebAuthnCredential *mock_find_credential_by_webauthn_id(
    const unsigned char *cred_id, int cred_id_len)
{
    for (int i = 0; i < MAX_CREDENTIALS; i++)
    {
        if (mock_credentials[i].in_use &&
            mock_credentials[i].webauthn_cred_id_len == cred_id_len &&
            memcmp(mock_credentials[i].webauthn_cred_id, cred_id, cred_id_len) == 0)
            return &mock_credentials[i];
    }
    return NULL;
}

/* Find credential by internal ID */
static MockWebAuthnCredential *mock_find_credential_by_id(const char *credential_id)
{
    for (int i = 0; i < MAX_CREDENTIALS; i++)
    {
        if (mock_credentials[i].in_use &&
            strcmp(mock_credentials[i].credential_id, credential_id) == 0)
            return &mock_credentials[i];
    }
    return NULL;
}

/* Get user's credentials */
static int mock_get_user_credentials(const char *user_id,
                                      MockWebAuthnCredential **results,
                                      int max_results)
{
    int count = 0;
    for (int i = 0; i < MAX_CREDENTIALS && count < max_results; i++)
    {
        if (mock_credentials[i].in_use &&
            mock_credentials[i].status == MOCK_CRED_ACTIVE &&
            strcmp(mock_credentials[i].user_id, user_id) == 0)
        {
            results[count++] = &mock_credentials[i];
        }
    }
    return count;
}

/* Verify authentication assertion */
typedef struct MockVerifyResult
{
    bool success;
    char error[256];
    MockWebAuthnCredential *credential;
} MockVerifyResult;

static MockVerifyResult mock_verify_authentication(
    const char *challenge_id,
    const unsigned char *cred_id, int cred_id_len,
    MockAuthenticatorData *auth_data,
    const unsigned char *signature, int sig_len,
    const char *client_data_json)
{
    MockVerifyResult result = {0};

    /* Validate challenge */
    if (!mock_validate_challenge(challenge_id, MOCK_CHALLENGE_AUTHENTICATION))
    {
        strcpy(result.error, "Invalid or expired challenge");
        return result;
    }

    MockWebAuthnChallenge *challenge = mock_find_challenge(challenge_id);

    /* Find credential */
    MockWebAuthnCredential *cred = mock_find_credential_by_webauthn_id(cred_id, cred_id_len);
    if (!cred)
    {
        strcpy(result.error, "Credential not found");
        return result;
    }

    if (cred->status != MOCK_CRED_ACTIVE)
    {
        strcpy(result.error, "Credential not active");
        return result;
    }

    /* Verify user matches */
    if (strcmp(cred->user_id, challenge->user_id) != 0)
    {
        strcpy(result.error, "Credential does not belong to user");
        return result;
    }

    /* Verify RP ID hash */
    unsigned char expected_rp_hash[32];
    mock_hash_rp_id(challenge->rp_id, expected_rp_hash);
    if (memcmp(auth_data->rp_id_hash, expected_rp_hash, 32) != 0)
    {
        strcpy(result.error, "RP ID hash mismatch");
        return result;
    }

    /* Check user presence */
    if (!auth_data->user_present)
    {
        strcpy(result.error, "User not present");
        return result;
    }

    /* Check sign count (replay protection) */
    if (auth_data->sign_count <= cred->sign_count && auth_data->sign_count != 0)
    {
        /* Possible cloned authenticator */
        cred->status = MOCK_CRED_SUSPENDED;
        strcpy(result.error, "Possible credential clone detected");
        return result;
    }

    /* Signature verification would happen here (mocked as always valid) */

    /* Update credential */
    cred->sign_count = auth_data->sign_count;
    cred->last_used_at = mock_current_time;

    /* Consume challenge */
    mock_consume_challenge(challenge_id);

    result.success = true;
    result.credential = cred;
    return result;
}

/* Revoke credential */
static bool mock_revoke_credential(const char *user_id, const char *credential_id,
                                    const char *reason)
{
    MockWebAuthnCredential *cred = mock_find_credential_by_id(credential_id);
    if (!cred)
        return false;

    if (strcmp(cred->user_id, user_id) != 0)
        return false;

    /* Check user has at least one other active credential */
    MockWebAuthnCredential *user_creds[MAX_CREDENTIALS];
    int cred_count = mock_get_user_credentials(user_id, user_creds, MAX_CREDENTIALS);
    if (cred_count <= 1)
        return false;  /* Can't revoke last credential */

    cred->status = MOCK_CRED_REVOKED;
    return true;
}

/* Count active credentials for user */
static int mock_count_user_credentials(const char *user_id)
{
    int count = 0;
    for (int i = 0; i < MAX_CREDENTIALS; i++)
    {
        if (mock_credentials[i].in_use &&
            mock_credentials[i].status == MOCK_CRED_ACTIVE &&
            strcmp(mock_credentials[i].user_id, user_id) == 0)
            count++;
    }
    return count;
}

/* ============================================================
 * Test Cases: Challenge Generation
 * ============================================================ */

static void test_challenge_creation(void)
{
    TEST_BEGIN("challenge_creation_registration")
        mock_webauthn_init();

        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-123", "example.com", "https://example.com");

        TEST_ASSERT_NOT_NULL(challenge);
        TEST_ASSERT_EQ(MOCK_CHALLENGE_REGISTRATION, challenge->type);
        TEST_ASSERT_STR_EQ("user-123", challenge->user_id);
        TEST_ASSERT_STR_EQ("example.com", challenge->rp_id);
        TEST_ASSERT_FALSE(challenge->consumed);
        TEST_ASSERT_GT(challenge->expires_at, mock_current_time);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_challenge_random_bytes(void)
{
    TEST_BEGIN("challenge_has_random_bytes")
        mock_webauthn_init();

        MockWebAuthnChallenge *c1 = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");
        MockWebAuthnChallenge *c2 = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");

        TEST_ASSERT_NOT_NULL(c1);
        TEST_ASSERT_NOT_NULL(c2);

        /* Challenges should be different */
        TEST_ASSERT_NEQ(0, memcmp(c1->challenge, c2->challenge, CHALLENGE_LEN));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_challenge_expiration(void)
{
    TEST_BEGIN("challenge_expiration")
        mock_webauthn_init();

        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        /* Valid before timeout */
        TEST_ASSERT(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_REGISTRATION));

        /* Expire the challenge */
        mock_current_time += CHALLENGE_TIMEOUT + 1;

        TEST_ASSERT_FALSE(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_REGISTRATION));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_challenge_consumed(void)
{
    TEST_BEGIN("challenge_consumed_once")
        mock_webauthn_init();

        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");

        TEST_ASSERT(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_AUTHENTICATION));

        mock_consume_challenge(challenge->challenge_id);

        /* Should fail after consumption */
        TEST_ASSERT_FALSE(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_AUTHENTICATION));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_challenge_type_mismatch(void)
{
    TEST_BEGIN("challenge_type_mismatch")
        mock_webauthn_init();

        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        /* Should fail with wrong type */
        TEST_ASSERT_FALSE(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_AUTHENTICATION));

        /* Should succeed with correct type */
        TEST_ASSERT(mock_validate_challenge(challenge->challenge_id, MOCK_CHALLENGE_REGISTRATION));

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Credential Registration
 * ============================================================ */

static void test_credential_registration(void)
{
    TEST_BEGIN("credential_registration_basic")
        mock_webauthn_init();

        /* Create registration challenge */
        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-123", "example.com", "https://example.com");

        /* Mock credential data */
        unsigned char cred_id[32];
        unsigned char public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred = mock_register_credential(
            challenge->challenge_id, "user-123",
            cred_id, sizeof(cred_id),
            public_key, sizeof(public_key),
            -7,  /* ES256 */
            1,   /* Initial sign count */
            "My Passkey",
            MOCK_DEVICE_APPLE_TOUCH_ID);

        TEST_ASSERT_NOT_NULL(cred);
        TEST_ASSERT_STR_EQ("user-123", cred->user_id);
        TEST_ASSERT_EQ(MOCK_CRED_ACTIVE, cred->status);
        TEST_ASSERT_EQ(-7, cred->algorithm);
        TEST_ASSERT_EQ(1, cred->sign_count);
        TEST_ASSERT_STR_EQ("My Passkey", cred->device_name);

        /* Challenge should be consumed */
        TEST_ASSERT(challenge->consumed);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_credential_registration_user_mismatch(void)
{
    TEST_BEGIN("credential_registration_user_mismatch")
        mock_webauthn_init();

        MockWebAuthnChallenge *challenge = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-123", "example.com", "https://example.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        /* Try to register with different user */
        MockWebAuthnCredential *cred = mock_register_credential(
            challenge->challenge_id, "different-user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);

        TEST_ASSERT_NULL(cred);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_credential_duplicate_rejected(void)
{
    TEST_BEGIN("credential_duplicate_rejected")
        mock_webauthn_init();

        /* First registration */
        MockWebAuthnChallenge *c1 = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-1", "rp.com", "https://rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred1 = mock_register_credential(
            c1->challenge_id, "user-1",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);
        TEST_ASSERT_NOT_NULL(cred1);

        /* Second registration with same credential ID */
        MockWebAuthnChallenge *c2 = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-2", "rp.com", "https://rp.com");

        MockWebAuthnCredential *cred2 = mock_register_credential(
            c2->challenge_id, "user-2",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);
        TEST_ASSERT_NULL(cred2);

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Authentication
 * ============================================================ */

static void test_authentication_success(void)
{
    TEST_BEGIN("authentication_success")
        mock_webauthn_init();

        /* Register a credential */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user-123", "example.com", "https://example.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred = mock_register_credential(
            reg->challenge_id, "user-123",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 1, NULL, MOCK_DEVICE_OTHER);
        TEST_ASSERT_NOT_NULL(cred);

        /* Create authentication challenge */
        MockWebAuthnChallenge *auth = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user-123", "example.com", "https://example.com");

        /* Mock authenticator data */
        MockAuthenticatorData auth_data;
        mock_hash_rp_id("example.com", auth_data.rp_id_hash);
        auth_data.sign_count = 2;  /* Incremented */
        auth_data.user_present = true;
        auth_data.user_verified = true;

        MockVerifyResult result = mock_verify_authentication(
            auth->challenge_id,
            cred_id, sizeof(cred_id),
            &auth_data,
            (unsigned char *)"sig", 3,
            "{}");

        TEST_ASSERT(result.success);
        TEST_ASSERT_NOT_NULL(result.credential);
        TEST_ASSERT_EQ(2, result.credential->sign_count);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_authentication_wrong_rp_id(void)
{
    TEST_BEGIN("authentication_wrong_rp_id")
        mock_webauthn_init();

        /* Register credential */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "correct-rp.com", "https://correct-rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        mock_register_credential(reg->challenge_id, "user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);

        /* Authenticate with wrong RP */
        MockWebAuthnChallenge *auth = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "correct-rp.com", "https://correct-rp.com");

        MockAuthenticatorData auth_data;
        mock_hash_rp_id("wrong-rp.com", auth_data.rp_id_hash);  /* Wrong RP */
        auth_data.sign_count = 1;
        auth_data.user_present = true;

        MockVerifyResult result = mock_verify_authentication(
            auth->challenge_id, cred_id, sizeof(cred_id),
            &auth_data, (unsigned char *)"sig", 3, "{}");

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "RP ID"));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_authentication_user_not_present(void)
{
    TEST_BEGIN("authentication_user_not_present")
        mock_webauthn_init();

        /* Register credential */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        mock_register_credential(reg->challenge_id, "user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);

        /* Authenticate without user presence */
        MockWebAuthnChallenge *auth = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");

        MockAuthenticatorData auth_data;
        mock_hash_rp_id("rp.com", auth_data.rp_id_hash);
        auth_data.sign_count = 1;
        auth_data.user_present = false;  /* No user presence */

        MockVerifyResult result = mock_verify_authentication(
            auth->challenge_id, cred_id, sizeof(cred_id),
            &auth_data, (unsigned char *)"sig", 3, "{}");

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "User not present"));

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Sign Count (Replay Protection)
 * ============================================================ */

static void test_sign_count_increments(void)
{
    TEST_BEGIN("sign_count_increments")
        mock_webauthn_init();

        /* Register credential with initial sign count */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred = mock_register_credential(
            reg->challenge_id, "user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 5, NULL, MOCK_DEVICE_OTHER);  /* Initial count: 5 */

        TEST_ASSERT_EQ(5, cred->sign_count);

        /* Authenticate with higher sign count */
        MockWebAuthnChallenge *auth = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");

        MockAuthenticatorData auth_data;
        mock_hash_rp_id("rp.com", auth_data.rp_id_hash);
        auth_data.sign_count = 10;  /* Higher than 5 */
        auth_data.user_present = true;

        MockVerifyResult result = mock_verify_authentication(
            auth->challenge_id, cred_id, sizeof(cred_id),
            &auth_data, (unsigned char *)"sig", 3, "{}");

        TEST_ASSERT(result.success);
        TEST_ASSERT_EQ(10, cred->sign_count);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_sign_count_replay_detection(void)
{
    TEST_BEGIN("sign_count_replay_detection")
        mock_webauthn_init();

        /* Register credential */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred = mock_register_credential(
            reg->challenge_id, "user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 100, NULL, MOCK_DEVICE_OTHER);  /* High initial count */

        /* Authenticate with lower sign count (replay attack) */
        MockWebAuthnChallenge *auth = mock_create_challenge(
            MOCK_CHALLENGE_AUTHENTICATION, "user", "rp.com", "https://rp.com");

        MockAuthenticatorData auth_data;
        mock_hash_rp_id("rp.com", auth_data.rp_id_hash);
        auth_data.sign_count = 50;  /* Lower than 100 */
        auth_data.user_present = true;

        MockVerifyResult result = mock_verify_authentication(
            auth->challenge_id, cred_id, sizeof(cred_id),
            &auth_data, (unsigned char *)"sig", 3, "{}");

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_NOT_NULL(strstr(result.error, "clone"));

        /* Credential should be suspended */
        TEST_ASSERT_EQ(MOCK_CRED_SUSPENDED, cred->status);

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Credential Management
 * ============================================================ */

static void test_get_user_credentials(void)
{
    TEST_BEGIN("get_user_credentials")
        mock_webauthn_init();

        /* Register multiple credentials for same user */
        for (int i = 0; i < 3; i++)
        {
            MockWebAuthnChallenge *reg = mock_create_challenge(
                MOCK_CHALLENGE_REGISTRATION, "user-multi", "rp.com", "https://rp.com");

            unsigned char cred_id[32], public_key[65];
            mock_generate_random_bytes(cred_id, sizeof(cred_id));
            mock_generate_random_bytes(public_key, sizeof(public_key));

            mock_register_credential(reg->challenge_id, "user-multi",
                cred_id, sizeof(cred_id), public_key, sizeof(public_key),
                -7, 0, NULL, MOCK_DEVICE_OTHER);
        }

        MockWebAuthnCredential *creds[10];
        int count = mock_get_user_credentials("user-multi", creds, 10);
        TEST_ASSERT_EQ(3, count);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_revoke_credential(void)
{
    TEST_BEGIN("revoke_credential")
        mock_webauthn_init();

        /* Register two credentials */
        for (int i = 0; i < 2; i++)
        {
            MockWebAuthnChallenge *reg = mock_create_challenge(
                MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

            unsigned char cred_id[32], public_key[65];
            mock_generate_random_bytes(cred_id, sizeof(cred_id));
            mock_generate_random_bytes(public_key, sizeof(public_key));

            mock_register_credential(reg->challenge_id, "user",
                cred_id, sizeof(cred_id), public_key, sizeof(public_key),
                -7, 0, NULL, MOCK_DEVICE_OTHER);
        }

        TEST_ASSERT_EQ(2, mock_count_user_credentials("user"));

        MockWebAuthnCredential *creds[2];
        mock_get_user_credentials("user", creds, 2);

        /* Revoke first credential */
        TEST_ASSERT(mock_revoke_credential("user", creds[0]->credential_id, "Test revocation"));
        TEST_ASSERT_EQ(MOCK_CRED_REVOKED, creds[0]->status);
        TEST_ASSERT_EQ(1, mock_count_user_credentials("user"));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_cannot_revoke_last_credential(void)
{
    TEST_BEGIN("cannot_revoke_last_credential")
        mock_webauthn_init();

        /* Register only one credential */
        MockWebAuthnChallenge *reg = mock_create_challenge(
            MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

        unsigned char cred_id[32], public_key[65];
        mock_generate_random_bytes(cred_id, sizeof(cred_id));
        mock_generate_random_bytes(public_key, sizeof(public_key));

        MockWebAuthnCredential *cred = mock_register_credential(
            reg->challenge_id, "user",
            cred_id, sizeof(cred_id), public_key, sizeof(public_key),
            -7, 0, NULL, MOCK_DEVICE_OTHER);

        /* Should not be able to revoke last credential */
        TEST_ASSERT_FALSE(mock_revoke_credential("user", cred->credential_id, "Try revoke"));
        TEST_ASSERT_EQ(MOCK_CRED_ACTIVE, cred->status);

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Cases: Edge Cases
 * ============================================================ */

static void test_credential_not_found(void)
{
    TEST_BEGIN("credential_not_found")
        mock_webauthn_init();

        unsigned char fake_cred_id[32];
        mock_generate_random_bytes(fake_cred_id, sizeof(fake_cred_id));

        MockWebAuthnCredential *cred = mock_find_credential_by_webauthn_id(
            fake_cred_id, sizeof(fake_cred_id));
        TEST_ASSERT_NULL(cred);

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_challenge_not_found(void)
{
    TEST_BEGIN("challenge_not_found")
        mock_webauthn_init();

        TEST_ASSERT_FALSE(mock_validate_challenge("fake-challenge-id", MOCK_CHALLENGE_REGISTRATION));

        mock_webauthn_cleanup();
    TEST_END();
}

static void test_different_device_types(void)
{
    TEST_BEGIN("different_device_types")
        mock_webauthn_init();

        MockDeviceType types[] = {
            MOCK_DEVICE_WINDOWS_HELLO,
            MOCK_DEVICE_APPLE_TOUCH_ID,
            MOCK_DEVICE_YUBIKEY,
            MOCK_DEVICE_ANDROID_BIOMETRIC
        };

        for (int i = 0; i < 4; i++)
        {
            MockWebAuthnChallenge *reg = mock_create_challenge(
                MOCK_CHALLENGE_REGISTRATION, "user", "rp.com", "https://rp.com");

            unsigned char cred_id[32], public_key[65];
            mock_generate_random_bytes(cred_id, sizeof(cred_id));
            mock_generate_random_bytes(public_key, sizeof(public_key));

            MockWebAuthnCredential *cred = mock_register_credential(
                reg->challenge_id, "user",
                cred_id, sizeof(cred_id), public_key, sizeof(public_key),
                -7, 0, NULL, types[i]);

            TEST_ASSERT_NOT_NULL(cred);
            TEST_ASSERT_EQ(types[i], cred->device_type);
        }

        mock_webauthn_cleanup();
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_webauthn(void)
{
    /* Challenge tests */
    test_challenge_creation();
    test_challenge_random_bytes();
    test_challenge_expiration();
    test_challenge_consumed();
    test_challenge_type_mismatch();

    /* Registration tests */
    test_credential_registration();
    test_credential_registration_user_mismatch();
    test_credential_duplicate_rejected();

    /* Authentication tests */
    test_authentication_success();
    test_authentication_wrong_rp_id();
    test_authentication_user_not_present();

    /* Sign count tests */
    test_sign_count_increments();
    test_sign_count_replay_detection();

    /* Credential management tests */
    test_get_user_credentials();
    test_revoke_credential();
    test_cannot_revoke_last_credential();

    /* Edge cases */
    test_credential_not_found();
    test_challenge_not_found();
    test_different_device_types();
}
