/*-------------------------------------------------------------------------
 *
 * auth_cache.c
 *    Orochi DB Authentication System - Shared Memory Cache Implementation
 *
 * This file implements:
 *   - Shared memory initialization for auth cache
 *   - Token caching with FIFO eviction
 *   - Session caching
 *   - Signing key cache management
 *   - Rate limiting in shared memory
 *   - Background worker for cleanup
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "auth.h"

/* ============================================================
 * Static Variables
 * ============================================================ */

/* LWLock tranche ID for auth locks */
static int OrochiAuthLWLockTrancheId = 0;

/* ============================================================
 * Shared Memory Size Calculation
 * ============================================================ */

/*
 * orochi_auth_shmem_size
 *    Calculate shared memory requirements for auth cache
 */
Size orochi_auth_shmem_size(void)
{
    Size size = 0;

    /* Main OrochiAuthCache structure */
    size = add_size(size, sizeof(OrochiAuthCache));

    /* Align to cache line for better performance */
    size = MAXALIGN(size);

    return size;
}

/* ============================================================
 * Shared Memory Initialization
 * ============================================================ */

/*
 * orochi_auth_shmem_init
 *    Initialize auth shared memory structures
 */
void orochi_auth_shmem_init(void)
{
    bool found;
    LWLock *locks;
    int i;

    /* Allocate shared memory */
    OrochiAuthSharedState = ShmemInitStruct("orochi_auth_cache", orochi_auth_shmem_size(), &found);

    if (!found) {
        /* First time initialization */
        memset(OrochiAuthSharedState, 0, sizeof(OrochiAuthCache));

        /* Get LWLock tranche */
        OrochiAuthLWLockTrancheId = LWLockNewTrancheId();
        LWLockRegisterTranche(OrochiAuthLWLockTrancheId, OROCHI_AUTH_LWLOCK_TRANCHE_NAME);

        /* Initialize locks from named LWLock tranche */
        locks = GetNamedLWLockTranche(OROCHI_AUTH_LWLOCK_TRANCHE_NAME);

        OrochiAuthSharedState->main_lock = &locks[0];
        OrochiAuthSharedState->token_cache_lock = &locks[1];
        OrochiAuthSharedState->session_cache_lock = &locks[2];
        OrochiAuthSharedState->rate_limit_lock = &locks[3];
        OrochiAuthSharedState->signing_key_lock = &locks[4];

        /* Initialize token cache (FIFO ring buffer) */
        OrochiAuthSharedState->token_cache_count = 0;
        OrochiAuthSharedState->token_cache_head = 0;
        OrochiAuthSharedState->token_cache_tail = 0;

        /* Initialize all token cache entries */
        for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++) {
            OrochiAuthSharedState->token_cache[i].token_hash[0] = '\0';
            OrochiAuthSharedState->token_cache[i].is_revoked = false;
            OrochiAuthSharedState->token_cache[i].expires_at = 0;
            OrochiAuthSharedState->token_cache[i].hash_slot = i;
        }

        /* Initialize session cache (FIFO ring buffer) */
        OrochiAuthSharedState->session_cache_count = 0;
        OrochiAuthSharedState->session_cache_head = 0;
        OrochiAuthSharedState->session_cache_tail = 0;

        /* Initialize all session cache entries */
        for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++) {
            OrochiAuthSharedState->session_cache[i].session_id[0] = '\0';
            OrochiAuthSharedState->session_cache[i].state = OROCHI_SESSION_EXPIRED;
            OrochiAuthSharedState->session_cache[i].hash_slot = i;
        }

        /* Initialize signing key cache */
        OrochiAuthSharedState->signing_key_count = 0;
        for (i = 0; i < OROCHI_AUTH_MAX_SIGNING_KEYS; i++) {
            OrochiAuthSharedState->signing_keys[i].key_id[0] = '\0';
            OrochiAuthSharedState->signing_keys[i].is_active = false;
            OrochiAuthSharedState->signing_keys[i].is_current = false;
        }

        /* Initialize rate limiting */
        OrochiAuthSharedState->rate_limit_count = 0;
        for (i = 0; i < OROCHI_AUTH_MAX_RATE_LIMITS; i++) {
            OrochiAuthSharedState->rate_limits[i].key[0] = '\0';
            OrochiAuthSharedState->rate_limits[i].count = 0;
            OrochiAuthSharedState->rate_limits[i].is_blocked = false;
        }

        /* Initialize atomic statistics counters */
        pg_atomic_init_u64(&OrochiAuthSharedState->total_auth_requests, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->cache_hits, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->cache_misses, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->failed_authentications, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->tokens_issued, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->tokens_revoked, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->sessions_created, 0);
        pg_atomic_init_u64(&OrochiAuthSharedState->sessions_expired, 0);

        /* Initialize timestamps */
        OrochiAuthSharedState->last_key_rotation = GetCurrentTimestamp();
        OrochiAuthSharedState->last_cleanup = GetCurrentTimestamp();
        OrochiAuthSharedState->startup_time = GetCurrentTimestamp();

        OrochiAuthSharedState->is_initialized = true;

        elog(LOG, "Orochi Auth shared memory initialized: %zu bytes", orochi_auth_shmem_size());
    } else {
        /* Shared memory already initialized (attach only) */
        elog(LOG, "Orochi Auth attached to existing shared memory");
    }
}

/* ============================================================
 * Token Cache Functions
 * ============================================================ */

/*
 * orochi_auth_cache_hash_slot
 *    Calculate hash slot for a token hash
 */
static uint32 orochi_auth_cache_hash_slot(const char *token_hash)
{
    uint32 hash = 0;
    int i;

    /* Simple hash function for first 8 bytes of hash */
    for (i = 0; i < 8 && token_hash[i]; i++) {
        hash = hash * 31 + (unsigned char)token_hash[i];
    }

    return hash % OROCHI_AUTH_MAX_CACHED_TOKENS;
}

/*
 * orochi_auth_cache_lookup
 *    Look up token in cache (must hold lock)
 */
OrochiToken *orochi_auth_cache_lookup(const char *token_hash)
{
    uint32 slot;
    int probe_count;
    OrochiToken *entry;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return NULL;

    /* Calculate initial slot */
    slot = orochi_auth_cache_hash_slot(token_hash);

    /* Linear probing with limited probes */
    for (probe_count = 0; probe_count < 16; probe_count++) {
        entry = &OrochiAuthSharedState->token_cache[slot];

        /* Empty slot means not found */
        if (entry->token_hash[0] == '\0')
            return NULL;

        /* Check for match */
        if (strcmp(entry->token_hash, token_hash) == 0)
            return entry;

        /* Move to next slot */
        slot = (slot + 1) % OROCHI_AUTH_MAX_CACHED_TOKENS;
    }

    return NULL; /* Not found after probing */
}

/*
 * orochi_auth_cache_token
 *    Add token to cache
 */
void orochi_auth_cache_token(OrochiTokenValidation *validation, const char *token_hash)
{
    OrochiToken *entry;
    uint32 slot;
    int probe_count;
    TimestampTz now;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    /* Calculate initial slot */
    slot = orochi_auth_cache_hash_slot(token_hash);

    /* Find empty slot or matching slot (with linear probing) */
    for (probe_count = 0; probe_count < 16; probe_count++) {
        entry = &OrochiAuthSharedState->token_cache[slot];

        /* Empty slot or expired entry - use it */
        if (entry->token_hash[0] == '\0' || entry->expires_at < now ||
            strcmp(entry->token_hash, token_hash) == 0) {
            /* Fill in token data */
            strlcpy(entry->token_hash, token_hash, sizeof(entry->token_hash));
            entry->token_type = OROCHI_TOKEN_ACCESS; /* Default */
            strlcpy(entry->user_id, validation->user_id, sizeof(entry->user_id));
            strlcpy(entry->tenant_id, validation->tenant_id, sizeof(entry->tenant_id));
            strlcpy(entry->session_id, validation->session_id, sizeof(entry->session_id));
            entry->issued_at = now;
            entry->expires_at = validation->expires_at;
            entry->not_before = 0;
            entry->scopes = validation->scopes;
            entry->is_revoked = false;
            entry->jti[0] = '\0';
            entry->hash_slot = slot;
            entry->cached_at = now;

            /* Update count if new entry */
            if (OrochiAuthSharedState->token_cache_count < OROCHI_AUTH_MAX_CACHED_TOKENS)
                OrochiAuthSharedState->token_cache_count++;

            LWLockRelease(OrochiAuthSharedState->token_cache_lock);
            return;
        }

        /* Move to next slot */
        slot = (slot + 1) % OROCHI_AUTH_MAX_CACHED_TOKENS;
    }

    /* Cache is full and no expired entries found - use FIFO eviction */
    slot = OrochiAuthSharedState->token_cache_head;
    entry = &OrochiAuthSharedState->token_cache[slot];

    /* Fill in token data */
    strlcpy(entry->token_hash, token_hash, sizeof(entry->token_hash));
    entry->token_type = OROCHI_TOKEN_ACCESS;
    strlcpy(entry->user_id, validation->user_id, sizeof(entry->user_id));
    strlcpy(entry->tenant_id, validation->tenant_id, sizeof(entry->tenant_id));
    strlcpy(entry->session_id, validation->session_id, sizeof(entry->session_id));
    entry->issued_at = now;
    entry->expires_at = validation->expires_at;
    entry->not_before = 0;
    entry->scopes = validation->scopes;
    entry->is_revoked = false;
    entry->jti[0] = '\0';
    entry->hash_slot = slot;
    entry->cached_at = now;

    /* Move head forward (FIFO) */
    OrochiAuthSharedState->token_cache_head =
        (OrochiAuthSharedState->token_cache_head + 1) % OROCHI_AUTH_MAX_CACHED_TOKENS;

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);
}

/*
 * orochi_auth_cache_invalidate_token
 *    Invalidate a token in cache
 */
void orochi_auth_cache_invalidate_token(const char *token_hash)
{
    OrochiToken *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    entry = orochi_auth_cache_lookup(token_hash);
    if (entry != NULL) {
        entry->is_revoked = true;
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->tokens_revoked, 1);
    }

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);
}

/*
 * orochi_auth_cache_invalidate_user
 *    Invalidate all tokens for a user
 */
void orochi_auth_cache_invalidate_user(const char *user_id)
{
    int i;

    if (OrochiAuthSharedState == NULL || user_id == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++) {
        OrochiToken *entry = &OrochiAuthSharedState->token_cache[i];

        if (entry->token_hash[0] != '\0' && strcmp(entry->user_id, user_id) == 0) {
            entry->is_revoked = true;
            pg_atomic_fetch_add_u64(&OrochiAuthSharedState->tokens_revoked, 1);
        }
    }

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);
}

/*
 * orochi_auth_cleanup_expired_tokens
 *    Remove expired tokens from cache
 */
void orochi_auth_cleanup_expired_tokens(void)
{
    TimestampTz now;
    int i;
    int cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++) {
        OrochiToken *entry = &OrochiAuthSharedState->token_cache[i];

        if (entry->token_hash[0] != '\0' && entry->expires_at < now) {
            /* Clear the entry */
            entry->token_hash[0] = '\0';
            entry->user_id[0] = '\0';
            entry->tenant_id[0] = '\0';
            entry->session_id[0] = '\0';
            entry->is_revoked = false;
            cleaned++;
        }
    }

    /* Update count */
    OrochiAuthSharedState->token_cache_count -= cleaned;
    if (OrochiAuthSharedState->token_cache_count < 0)
        OrochiAuthSharedState->token_cache_count = 0;

    OrochiAuthSharedState->last_cleanup = now;

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);

    if (cleaned > 0)
        elog(LOG, "Orochi Auth: cleaned %d expired tokens", cleaned);
}

/* ============================================================
 * Session Cache Functions
 * ============================================================ */

/*
 * orochi_auth_session_hash_slot
 *    Calculate hash slot for a session ID
 */
static uint32 orochi_auth_session_hash_slot(const char *session_id)
{
    uint32 hash = 0;
    int i;

    for (i = 0; i < 8 && session_id[i]; i++) {
        hash = hash * 31 + (unsigned char)session_id[i];
    }

    return hash % OROCHI_AUTH_MAX_CACHED_SESSIONS;
}

/*
 * orochi_auth_session_cache_lookup
 *    Look up session in cache
 */
OrochiSession *orochi_auth_session_cache_lookup(const char *session_id)
{
    uint32 slot;
    int probe_count;
    OrochiSession *entry;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return NULL;

    if (session_id == NULL || session_id[0] == '\0')
        return NULL;

    slot = orochi_auth_session_hash_slot(session_id);

    /* Linear probing */
    for (probe_count = 0; probe_count < 16; probe_count++) {
        entry = &OrochiAuthSharedState->session_cache[slot];

        if (entry->session_id[0] == '\0')
            return NULL;

        if (strcmp(entry->session_id, session_id) == 0 && entry->state != OROCHI_SESSION_EXPIRED) {
            return entry;
        }

        slot = (slot + 1) % OROCHI_AUTH_MAX_CACHED_SESSIONS;
    }

    return NULL;
}

/*
 * orochi_auth_cache_session
 *    Add session to cache
 */
void orochi_auth_cache_session(OrochiSession *session)
{
    OrochiSession *entry;
    uint32 slot;
    int probe_count;
    TimestampTz now;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return;

    if (session == NULL || session->session_id[0] == '\0')
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    slot = orochi_auth_session_hash_slot(session->session_id);

    /* Find empty or matching slot */
    for (probe_count = 0; probe_count < 16; probe_count++) {
        entry = &OrochiAuthSharedState->session_cache[slot];

        if (entry->session_id[0] == '\0' || entry->state == OROCHI_SESSION_EXPIRED ||
            strcmp(entry->session_id, session->session_id) == 0) {
            /* Copy session data */
            memcpy(entry, session, sizeof(OrochiSession));
            entry->hash_slot = slot;

            if (OrochiAuthSharedState->session_cache_count < OROCHI_AUTH_MAX_CACHED_SESSIONS)
                OrochiAuthSharedState->session_cache_count++;

            LWLockRelease(OrochiAuthSharedState->session_cache_lock);
            return;
        }

        slot = (slot + 1) % OROCHI_AUTH_MAX_CACHED_SESSIONS;
    }

    /* FIFO eviction */
    slot = OrochiAuthSharedState->session_cache_head;
    entry = &OrochiAuthSharedState->session_cache[slot];

    memcpy(entry, session, sizeof(OrochiSession));
    entry->hash_slot = slot;

    OrochiAuthSharedState->session_cache_head =
        (OrochiAuthSharedState->session_cache_head + 1) % OROCHI_AUTH_MAX_CACHED_SESSIONS;

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_cache_invalidate_session
 *    Invalidate a session in cache
 */
void orochi_auth_cache_invalidate_session(const char *session_id)
{
    OrochiSession *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    entry = orochi_auth_session_cache_lookup(session_id);
    if (entry != NULL) {
        entry->state = OROCHI_SESSION_REVOKED;
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->sessions_expired, 1);
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_cleanup_expired_sessions
 *    Remove expired sessions from cache
 */
void orochi_auth_cleanup_expired_sessions(void)
{
    TimestampTz now;
    int i;
    int cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++) {
        OrochiSession *entry = &OrochiAuthSharedState->session_cache[i];

        if (entry->session_id[0] != '\0' &&
            (entry->expires_at < now || entry->state == OROCHI_SESSION_REVOKED)) {
            entry->state = OROCHI_SESSION_EXPIRED;
            entry->session_id[0] = '\0';
            cleaned++;
        }
    }

    OrochiAuthSharedState->session_cache_count -= cleaned;
    if (OrochiAuthSharedState->session_cache_count < 0)
        OrochiAuthSharedState->session_cache_count = 0;

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);

    if (cleaned > 0) {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->sessions_expired, cleaned);
        elog(LOG, "Orochi Auth: cleaned %d expired sessions", cleaned);
    }
}

/*
 * orochi_auth_update_session_states
 *    Update session states (mark idle sessions)
 */
void orochi_auth_update_session_states(void)
{
    TimestampTz now;
    TimestampTz idle_threshold;
    int i;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();
    /* Sessions idle for more than 30 minutes are marked IDLE */
    idle_threshold = now - (30 * 60 * USECS_PER_SEC);

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++) {
        OrochiSession *entry = &OrochiAuthSharedState->session_cache[i];

        if (entry->session_id[0] != '\0' && entry->state == OROCHI_SESSION_ACTIVE &&
            entry->last_activity < idle_threshold) {
            entry->state = OROCHI_SESSION_IDLE;
        }
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_enforce_session_limits
 *    Enforce per-user session limits (revoke oldest sessions)
 */
void orochi_auth_enforce_session_limits(void)
{
    int i;
    int max_per_user = orochi_auth_max_sessions_per_user;

    if (OrochiAuthSharedState == NULL || max_per_user <= 0)
        return;

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    /*
     * For each active user, count sessions and revoke oldest ones
     * if the count exceeds the per-user limit.
     *
     * Simple O(n^2) approach is fine given the bounded cache size
     * (OROCHI_AUTH_MAX_CACHED_SESSIONS is typically small).
     */
    for (i = 0; i < OrochiAuthSharedState->session_cache_count; i++) {
        OrochiSession *session = &OrochiAuthSharedState->session_cache[i];
        int count;
        int j;
        TimestampTz oldest_time;
        int oldest_idx;

        if (session->state != OROCHI_SESSION_ACTIVE)
            continue;

        /* Count active sessions for this user */
        count = 0;
        for (j = 0; j < OrochiAuthSharedState->session_cache_count; j++) {
            OrochiSession *other = &OrochiAuthSharedState->session_cache[j];
            if (other->state == OROCHI_SESSION_ACTIVE &&
                strcmp(other->user_id, session->user_id) == 0)
                count++;
        }

        /* If within limit, nothing to do for this user */
        if (count <= max_per_user)
            continue;

        /* Revoke oldest sessions until within limit */
        while (count > max_per_user) {
            oldest_time = DT_NOEND;
            oldest_idx = -1;

            for (j = 0; j < OrochiAuthSharedState->session_cache_count; j++) {
                OrochiSession *other = &OrochiAuthSharedState->session_cache[j];
                if (other->state == OROCHI_SESSION_ACTIVE &&
                    strcmp(other->user_id, session->user_id) == 0 &&
                    other->last_activity < oldest_time) {
                    oldest_time = other->last_activity;
                    oldest_idx = j;
                }
            }

            if (oldest_idx >= 0) {
                OrochiAuthSharedState->session_cache[oldest_idx].state = OROCHI_SESSION_REVOKED;
                elog(LOG, "Orochi Auth: revoked oldest session %s for user %s (limit %d exceeded)",
                     OrochiAuthSharedState->session_cache[oldest_idx].session_id, session->user_id,
                     max_per_user);
            }
            count--;
        }
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/* ============================================================
 * Signing Key Cache Functions
 * ============================================================ */

/*
 * orochi_auth_get_signing_key
 *    Get current signing key for tenant
 */
OrochiSigningKey *orochi_auth_get_signing_key(const char *tenant_id)
{
    int i;
    OrochiSigningKey *key = NULL;

    if (OrochiAuthSharedState == NULL)
        return NULL;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++) {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && entry->is_current) {
            /* Match tenant_id or use global key (empty tenant_id) */
            if (tenant_id == NULL || entry->tenant_id[0] == '\0' ||
                strcmp(entry->tenant_id, tenant_id) == 0) {
                key = entry;
                break;
            }
        }
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
    return key;
}

/*
 * orochi_auth_get_signing_key_by_id
 *    Get signing key by key ID (kid)
 */
OrochiSigningKey *orochi_auth_get_signing_key_by_id(const char *key_id)
{
    int i;
    OrochiSigningKey *key = NULL;

    if (OrochiAuthSharedState == NULL || key_id == NULL)
        return NULL;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++) {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && strcmp(entry->key_id, key_id) == 0) {
            key = entry;
            break;
        }
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
    return key;
}

/*
 * orochi_auth_cache_signing_key
 *    Add signing key to cache
 */
void orochi_auth_cache_signing_key(OrochiSigningKey *key)
{
    int i;
    OrochiSigningKey *entry = NULL;

    if (OrochiAuthSharedState == NULL || key == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_EXCLUSIVE);

    /* Look for existing entry with same key_id */
    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++) {
        if (strcmp(OrochiAuthSharedState->signing_keys[i].key_id, key->key_id) == 0) {
            entry = &OrochiAuthSharedState->signing_keys[i];
            break;
        }
    }

    /* If not found, use next available slot */
    if (entry == NULL && OrochiAuthSharedState->signing_key_count < OROCHI_AUTH_MAX_SIGNING_KEYS) {
        entry = &OrochiAuthSharedState->signing_keys[OrochiAuthSharedState->signing_key_count];
        OrochiAuthSharedState->signing_key_count++;
    }

    if (entry != NULL) {
        /* Copy key data */
        memcpy(entry, key, sizeof(OrochiSigningKey));
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
}

/*
 * orochi_auth_check_key_rotation
 *    Check and rotate signing keys if needed
 */
void orochi_auth_check_key_rotation(void)
{
    TimestampTz now;
    TimestampTz rotation_threshold;
    int i;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();
    /* Keys older than rotation_days should be rotated */
    rotation_threshold = now - (orochi_auth_key_rotation_days * 24 * 60 * 60 * USECS_PER_SEC);

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++) {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && entry->is_current && entry->created_at < rotation_threshold) {
            elog(LOG, "Orochi Auth: signing key %s needs rotation (age exceeds %d days)",
                 entry->key_id, orochi_auth_key_rotation_days);

            /* Mark the old key as no longer current */
            entry->is_current = false;
            entry->rotated_at = now;

            /* Notify listeners that key rotation is needed */
            {
                StringInfoData notify_payload;

                LWLockRelease(OrochiAuthSharedState->signing_key_lock);

                initStringInfo(&notify_payload);
                appendStringInfo(&notify_payload, "{\"event\":\"key_rotation\",\"key_id\":\"%s\"}",
                                 entry->key_id);

                if (SPI_connect() == SPI_OK_CONNECT) {
                    StringInfoData notify_query;
                    initStringInfo(&notify_query);
                    appendStringInfo(&notify_query, "SELECT pg_notify('orochi_auth_events', %s)",
                                     quote_literal_cstr(notify_payload.data));
                    SPI_execute(notify_query.data, false, 0);
                    pfree(notify_query.data);
                    SPI_finish();
                }

                pfree(notify_payload.data);

                /* Re-acquire lock and continue scan */
                LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);
            }
        }
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
}

/*
 * orochi_auth_refresh_signing_key_cache
 *    Refresh signing key cache from database
 */
void orochi_auth_refresh_signing_key_cache(void)
{
    int ret;
    int i;

    if (OrochiAuthSharedState == NULL)
        return;

    /* Query the auth.signing_keys table for active keys */
    if (SPI_connect() != SPI_OK_CONNECT) {
        elog(WARNING, "Orochi Auth: failed to connect to SPI for key refresh");
        return;
    }

    ret = SPI_execute("SELECT key_id, tenant_id, algorithm, public_key, private_key_encrypted, "
                      "       created_at, expires_at, rotated_at, is_current, is_active, version "
                      "FROM auth.signing_keys "
                      "WHERE is_active = true "
                      "ORDER BY created_at DESC",
                      true, OROCHI_AUTH_MAX_SIGNING_KEYS);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;

        LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_EXCLUSIVE);

        OrochiAuthSharedState->signing_key_count = 0;

        for (i = 0; i < (int)SPI_processed && i < OROCHI_AUTH_MAX_SIGNING_KEYS; i++) {
            HeapTuple tuple = SPI_tuptable->vals[i];
            OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];
            bool isnull;
            char *val;

            memset(entry, 0, sizeof(OrochiSigningKey));

            val = SPI_getvalue(tuple, tupdesc, 1);
            if (val)
                strlcpy(entry->key_id, val, sizeof(entry->key_id));

            val = SPI_getvalue(tuple, tupdesc, 2);
            if (val)
                strlcpy(entry->tenant_id, val, sizeof(entry->tenant_id));

            entry->algorithm = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));

            val = SPI_getvalue(tuple, tupdesc, 4);
            if (val)
                strlcpy(entry->public_key, val, sizeof(entry->public_key));

            val = SPI_getvalue(tuple, tupdesc, 5);
            if (val)
                strlcpy(entry->private_key_encrypted, val, sizeof(entry->private_key_encrypted));

            entry->created_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 6, &isnull));
            if (isnull)
                entry->created_at = GetCurrentTimestamp();

            entry->expires_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 7, &isnull));

            entry->rotated_at = DatumGetTimestampTz(SPI_getbinval(tuple, tupdesc, 8, &isnull));

            entry->is_current = DatumGetBool(SPI_getbinval(tuple, tupdesc, 9, &isnull));
            entry->is_active = DatumGetBool(SPI_getbinval(tuple, tupdesc, 10, &isnull));
            entry->version = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 11, &isnull));

            OrochiAuthSharedState->signing_key_count++;
        }

        OrochiAuthSharedState->last_key_rotation = GetCurrentTimestamp();

        LWLockRelease(OrochiAuthSharedState->signing_key_lock);

        elog(LOG, "Orochi Auth: refreshed %d signing keys from database",
             OrochiAuthSharedState->signing_key_count);
    }

    SPI_finish();
}

/* ============================================================
 * Rate Limiting Functions
 * ============================================================ */

/*
 * orochi_auth_rate_limit_hash
 *    Calculate hash for rate limit key
 */
static uint32 orochi_auth_rate_limit_hash(const char *key, const char *action)
{
    uint32 hash = 0;
    const char *p;

    for (p = key; *p; p++)
        hash = hash * 31 + (unsigned char)*p;

    hash = hash * 17;

    for (p = action; *p; p++)
        hash = hash * 31 + (unsigned char)*p;

    return hash % OROCHI_AUTH_MAX_RATE_LIMITS;
}

/*
 * orochi_auth_check_rate_limit
 *    Check if request is allowed under rate limit
 */
bool orochi_auth_check_rate_limit(const char *key, const char *action, int limit)
{
    uint32 slot;
    int probe_count;
    OrochiRateLimit *entry;
    TimestampTz now;
    TimestampTz window_end;
    bool allowed = true;

    if (!orochi_auth_rate_limit_enabled || OrochiAuthSharedState == NULL)
        return true;

    now = GetCurrentTimestamp();
    slot = orochi_auth_rate_limit_hash(key, action);

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    /* Find existing entry or empty slot */
    for (probe_count = 0; probe_count < 8; probe_count++) {
        entry = &OrochiAuthSharedState->rate_limits[slot];

        /* Empty slot */
        if (entry->key[0] == '\0') {
            /* Create new entry */
            snprintf(entry->key, sizeof(entry->key), "%s:%s", key, action);
            entry->count = 1;
            entry->limit = limit;
            entry->window_start = now;
            entry->window_seconds = orochi_auth_rate_limit_window;
            entry->is_blocked = false;
            entry->blocked_until = 0;

            if (OrochiAuthSharedState->rate_limit_count < OROCHI_AUTH_MAX_RATE_LIMITS)
                OrochiAuthSharedState->rate_limit_count++;

            LWLockRelease(OrochiAuthSharedState->rate_limit_lock);
            return true;
        }

        /* Found matching entry */
        if (strncmp(entry->key, key, strlen(key)) == 0) {
            window_end = entry->window_start + (entry->window_seconds * USECS_PER_SEC);

            /* Check if blocked */
            if (entry->is_blocked && entry->blocked_until > now) {
                allowed = false;
            } else if (now > window_end) {
                /* Window expired, reset */
                entry->count = 1;
                entry->window_start = now;
                entry->is_blocked = false;
                allowed = true;
            } else if (entry->count >= entry->limit) {
                /* Limit exceeded */
                entry->is_blocked = true;
                entry->blocked_until = now + (entry->window_seconds * USECS_PER_SEC);
                allowed = false;

                pg_atomic_fetch_add_u64(&OrochiAuthSharedState->failed_authentications, 1);
            } else {
                /* Increment count */
                entry->count++;
                allowed = true;
            }

            LWLockRelease(OrochiAuthSharedState->rate_limit_lock);
            return allowed;
        }

        slot = (slot + 1) % OROCHI_AUTH_MAX_RATE_LIMITS;
    }

    LWLockRelease(OrochiAuthSharedState->rate_limit_lock);
    return true; /* Allow if no slot available */
}

/*
 * orochi_auth_record_rate_limit
 *    Record a rate limit hit (increment counter)
 */
void orochi_auth_record_rate_limit(const char *key, const char *action)
{
    /* Already handled in orochi_auth_check_rate_limit */
}

/*
 * orochi_auth_clear_rate_limit
 *    Clear rate limit for a key
 */
void orochi_auth_clear_rate_limit(const char *key, const char *action)
{
    uint32 slot;
    int probe_count;
    OrochiRateLimit *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    slot = orochi_auth_rate_limit_hash(key, action);

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    for (probe_count = 0; probe_count < 8; probe_count++) {
        entry = &OrochiAuthSharedState->rate_limits[slot];

        if (entry->key[0] != '\0' && strncmp(entry->key, key, strlen(key)) == 0) {
            entry->key[0] = '\0';
            entry->count = 0;
            entry->is_blocked = false;
            OrochiAuthSharedState->rate_limit_count--;
            break;
        }

        slot = (slot + 1) % OROCHI_AUTH_MAX_RATE_LIMITS;
    }

    LWLockRelease(OrochiAuthSharedState->rate_limit_lock);
}

/*
 * orochi_auth_cleanup_rate_limits
 *    Cleanup expired rate limits
 */
void orochi_auth_cleanup_rate_limits(void)
{
    TimestampTz now;
    int i;
    int cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_RATE_LIMITS; i++) {
        OrochiRateLimit *entry = &OrochiAuthSharedState->rate_limits[i];

        if (entry->key[0] != '\0') {
            TimestampTz window_end =
                entry->window_start + (entry->window_seconds * USECS_PER_SEC * 2);

            /* Remove entries that have been idle for 2x the window */
            if (now > window_end) {
                entry->key[0] = '\0';
                entry->count = 0;
                entry->is_blocked = false;
                cleaned++;
            }
        }
    }

    OrochiAuthSharedState->rate_limit_count -= cleaned;
    if (OrochiAuthSharedState->rate_limit_count < 0)
        OrochiAuthSharedState->rate_limit_count = 0;

    LWLockRelease(OrochiAuthSharedState->rate_limit_lock);
}

/* ============================================================
 * Audit Logging Functions
 * ============================================================ */

/*
 * orochi_auth_audit_log
 *    Log an authentication event to the audit log table
 */
void orochi_auth_audit_log(const char *tenant_id, const char *user_id, const char *session_id,
                           OrochiAuditEvent event_type, const char *ip_address,
                           const char *user_agent, const char *resource_type,
                           const char *resource_id)
{
    int ret;
    StringInfoData query;
    Oid argtypes[8];
    Datum values[8];
    char nulls[8];
    int i;
    bool connected = false;

    if (!orochi_auth_audit_enabled)
        return;

    /* Always log to PostgreSQL log for debugging */
    elog(LOG, "Orochi Auth Audit: event=%s tenant=%s user=%s session=%s ip=%s",
         orochi_auth_audit_event_name(event_type), tenant_id ? tenant_id : "(none)",
         user_id ? user_id : "(none)", session_id ? session_id : "(none)",
         ip_address ? ip_address : "(none)");

    /* Attempt to insert into audit log table via SPI */
    PG_TRY();
    {
        ret = SPI_connect();
        if (ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi Auth: SPI_connect failed: %d", ret);
            PG_RE_THROW();
        }
        connected = true;

        /* Build the INSERT query */
        initStringInfo(&query);
        appendStringInfoString(&query, "INSERT INTO orochi.orochi_auth_audit_log "
                                       "(tenant_id, user_id, session_id, event_type, event_name, "
                                       " ip_address, user_agent, resource_type, resource_id) "
                                       "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)");

        /* Set up parameter types */
        argtypes[0] = TEXTOID; /* tenant_id */
        argtypes[1] = TEXTOID; /* user_id */
        argtypes[2] = TEXTOID; /* session_id */
        argtypes[3] = INT4OID; /* event_type */
        argtypes[4] = TEXTOID; /* event_name */
        argtypes[5] = TEXTOID; /* ip_address */
        argtypes[6] = TEXTOID; /* user_agent */
        argtypes[7] = TEXTOID; /* resource_type */

        /* Initialize nulls array */
        for (i = 0; i < 8; i++)
            nulls[i] = ' ';

        /* Set parameter values */
        if (tenant_id != NULL && tenant_id[0] != '\0')
            values[0] = CStringGetTextDatum(tenant_id);
        else
            nulls[0] = 'n';

        if (user_id != NULL && user_id[0] != '\0')
            values[1] = CStringGetTextDatum(user_id);
        else
            nulls[1] = 'n';

        if (session_id != NULL && session_id[0] != '\0')
            values[2] = CStringGetTextDatum(session_id);
        else
            nulls[2] = 'n';

        values[3] = Int32GetDatum((int32)event_type);
        values[4] = CStringGetTextDatum(orochi_auth_audit_event_name(event_type));

        if (ip_address != NULL && ip_address[0] != '\0')
            values[5] = CStringGetTextDatum(ip_address);
        else
            nulls[5] = 'n';

        if (user_agent != NULL && user_agent[0] != '\0')
            values[6] = CStringGetTextDatum(user_agent);
        else
            nulls[6] = 'n';

        if (resource_type != NULL && resource_type[0] != '\0')
            values[7] = CStringGetTextDatum(resource_type);
        else
            nulls[7] = 'n';

        /* Execute with 9 parameters (resource_id added inline) */
        {
            Oid all_argtypes[9];
            Datum all_values[9];
            char all_nulls[9];

            memcpy(all_argtypes, argtypes, sizeof(argtypes));
            all_argtypes[8] = TEXTOID; /* resource_id */

            memcpy(all_values, values, sizeof(values));
            memcpy(all_nulls, nulls, sizeof(nulls));
            all_nulls[8] = ' '; /* Initialize 9th parameter as non-NULL */

            if (resource_id != NULL && resource_id[0] != '\0')
                all_values[8] = CStringGetTextDatum(resource_id);
            else
                all_nulls[8] = 'n';

            ret =
                SPI_execute_with_args(query.data, 9, all_argtypes, all_values, all_nulls, false, 0);
        }

        if (ret != SPI_OK_INSERT) {
            elog(WARNING, "Orochi Auth: audit log insert failed: %d", ret);
        }

        pfree(query.data);
        SPI_finish();
        connected = false;
    }
    PG_CATCH();
    {
        /* Clean up on error */
        if (connected)
            SPI_finish();

        /* Don't re-throw - audit logging should not break authentication */
        elog(WARNING, "Orochi Auth: audit logging failed, continuing without audit");
        FlushErrorState();
    }
    PG_END_TRY();
}

/*
 * orochi_auth_ensure_audit_partitions
 *    Ensure audit log partitions exist for current and next month
 */
void orochi_auth_ensure_audit_partitions(void)
{
    int ret;
    StringInfoData query;
    bool connected = false;
    TimestampTz now;
    struct pg_tm tm;
    fsec_t fsec;
    int current_year, current_month;
    int next_year, next_month;

    if (!orochi_auth_audit_enabled)
        return;

    /* Get current timestamp and extract year/month */
    now = GetCurrentTimestamp();
    if (timestamp2tm(now, NULL, &tm, &fsec, NULL, NULL) != 0) {
        elog(WARNING, "Orochi Auth: failed to decode current timestamp");
        return;
    }

    current_year = tm.tm_year;
    current_month = tm.tm_mon;

    /* Calculate next month */
    next_month = current_month + 1;
    next_year = current_year;
    if (next_month > 12) {
        next_month = 1;
        next_year++;
    }

    PG_TRY();
    {
        ret = SPI_connect();
        if (ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi Auth: SPI_connect failed for partition creation: %d", ret);
            PG_RE_THROW();
        }
        connected = true;

        initStringInfo(&query);

        /*
         * Create partition for current month if it doesn't exist.
         * Use DO block with exception handling to ignore "already exists" errors.
         */
        appendStringInfo(&query,
                         "DO $$ "
                         "BEGIN "
                         "  CREATE TABLE IF NOT EXISTS orochi.orochi_auth_audit_log_y%04dm%02d "
                         "  PARTITION OF orochi.orochi_auth_audit_log "
                         "  FOR VALUES FROM ('%04d-%02d-01') TO ('%04d-%02d-01'); "
                         "EXCEPTION WHEN duplicate_table THEN "
                         "  NULL; " /* Partition already exists, ignore */
                         "END $$",
                         current_year, current_month, current_year, current_month,
                         (current_month == 12 ? current_year + 1 : current_year),
                         (current_month == 12 ? 1 : current_month + 1));

        ret = SPI_execute(query.data, false, 0);
        if (ret != SPI_OK_UTILITY) {
            elog(LOG, "Orochi Auth: partition creation for %04d-%02d returned %d", current_year,
                 current_month, ret);
        }

        /* Reset query buffer for next partition */
        resetStringInfo(&query);

        /*
         * Create partition for next month if it doesn't exist.
         */
        {
            int after_next_month = next_month + 1;
            int after_next_year = next_year;
            if (after_next_month > 12) {
                after_next_month = 1;
                after_next_year++;
            }

            appendStringInfo(&query,
                             "DO $$ "
                             "BEGIN "
                             "  CREATE TABLE IF NOT EXISTS "
                             "orochi.orochi_auth_audit_log_y%04dm%02d "
                             "  PARTITION OF orochi.orochi_auth_audit_log "
                             "  FOR VALUES FROM ('%04d-%02d-01') TO ('%04d-%02d-01'); "
                             "EXCEPTION WHEN duplicate_table THEN "
                             "  NULL; " /* Partition already exists, ignore */
                             "END $$",
                             next_year, next_month, next_year, next_month, after_next_year,
                             after_next_month);
        }

        ret = SPI_execute(query.data, false, 0);
        if (ret != SPI_OK_UTILITY) {
            elog(LOG, "Orochi Auth: partition creation for %04d-%02d returned %d", next_year,
                 next_month, ret);
        }

        pfree(query.data);
        SPI_finish();
        connected = false;

        elog(DEBUG1, "Orochi Auth: ensured audit log partitions for %04d-%02d and %04d-%02d",
             current_year, current_month, next_year, next_month);
    }
    PG_CATCH();
    {
        if (connected)
            SPI_finish();

        /* Log but don't fail - partition management should not break auth */
        elog(WARNING, "Orochi Auth: failed to ensure audit partitions");
        FlushErrorState();
    }
    PG_END_TRY();
}

/*
 * orochi_auth_archive_audit_partitions
 *    Archive audit log partitions older than retention period
 *
 * This function detaches and optionally drops partitions that are older
 * than the configured retention period (orochi_auth_audit_retention_days).
 */
void orochi_auth_archive_audit_partitions(void)
{
    int ret;
    StringInfoData query;
    bool connected = false;
    TimestampTz now;
    TimestampTz retention_cutoff;
    struct pg_tm tm;
    fsec_t fsec;
    int cutoff_year, cutoff_month;
    int i;

    if (!orochi_auth_audit_enabled)
        return;

    /* Calculate the retention cutoff date */
    now = GetCurrentTimestamp();
    retention_cutoff = now - ((int64)orochi_auth_audit_retention_days * USECS_PER_DAY);

    if (timestamp2tm(retention_cutoff, NULL, &tm, &fsec, NULL, NULL) != 0) {
        elog(WARNING, "Orochi Auth: failed to decode retention cutoff timestamp");
        return;
    }

    cutoff_year = tm.tm_year;
    cutoff_month = tm.tm_mon;

    elog(LOG,
         "Orochi Auth: archiving audit partitions older than %04d-%02d "
         "(retention: %d days)",
         cutoff_year, cutoff_month, orochi_auth_audit_retention_days);

    PG_TRY();
    {
        ret = SPI_connect();
        if (ret != SPI_OK_CONNECT) {
            elog(WARNING, "Orochi Auth: SPI_connect failed for partition archival: %d", ret);
            PG_RE_THROW();
        }
        connected = true;

        initStringInfo(&query);

        /*
         * Query pg_inherits to find all partitions of the audit log table
         * that are older than the retention cutoff.
         */
        appendStringInfoString(&query,
                               "SELECT c.relname, c.oid "
                               "FROM pg_inherits i "
                               "JOIN pg_class c ON i.inhrelid = c.oid "
                               "JOIN pg_class p ON i.inhparent = p.oid "
                               "JOIN pg_namespace n ON p.relnamespace = n.oid "
                               "WHERE n.nspname = 'orochi' "
                               "  AND p.relname = 'orochi_auth_audit_log' "
                               "  AND c.relname ~ '^orochi_auth_audit_log_y[0-9]{4}m[0-9]{2}$' "
                               "ORDER BY c.relname");

        ret = SPI_execute(query.data, true, 0);
        if (ret != SPI_OK_SELECT) {
            elog(WARNING, "Orochi Auth: failed to query audit partitions: %d", ret);
            pfree(query.data);
            SPI_finish();
            return;
        }

        /* Process each partition found */
        for (i = 0; i < SPI_processed; i++) {
            char *partition_name;
            int part_year, part_month;
            bool isnull;

            partition_name = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            if (partition_name == NULL)
                continue;

            /* Parse year and month from partition name:
             * orochi_auth_audit_log_yYYYYmMM */
            if (sscanf(partition_name, "orochi_auth_audit_log_y%4dm%2d", &part_year, &part_month) !=
                2) {
                elog(DEBUG1, "Orochi Auth: skipping non-matching partition: %s", partition_name);
                continue;
            }

            /* Check if partition is older than retention cutoff */
            if (part_year < cutoff_year ||
                (part_year == cutoff_year && part_month < cutoff_month)) {
                StringInfoData detach_query;

                elog(LOG, "Orochi Auth: archiving old partition %s (cutoff: %04d-%02d)",
                     partition_name, cutoff_year, cutoff_month);

                initStringInfo(&detach_query);

                /*
                 * Detach the partition from the parent table.
                 * The partition remains as a standalone table that can be
                 * exported to cold storage (S3, etc.) before being dropped.
                 */
                appendStringInfo(&detach_query,
                                 "ALTER TABLE orochi.orochi_auth_audit_log "
                                 "DETACH PARTITION orochi.%s",
                                 partition_name);

                ret = SPI_execute(detach_query.data, false, 0);
                if (ret == SPI_OK_UTILITY) {
                    elog(LOG, "Orochi Auth: detached partition %s", partition_name);

                    /*
                     * Optionally drop the detached partition after archival.
                     * In production, you might want to:
                     * 1. Export to S3/cold storage first
                     * 2. Verify the export succeeded
                     * 3. Then drop the table
                     *
                     * For now, we drop immediately. A more sophisticated
                     * implementation could use a separate background process
                     * for the export step.
                     */
                    resetStringInfo(&detach_query);
                    appendStringInfo(&detach_query, "DROP TABLE IF EXISTS orochi.%s",
                                     partition_name);

                    ret = SPI_execute(detach_query.data, false, 0);
                    if (ret == SPI_OK_UTILITY) {
                        elog(LOG, "Orochi Auth: dropped archived partition %s", partition_name);
                    } else {
                        elog(WARNING, "Orochi Auth: failed to drop partition %s: %d",
                             partition_name, ret);
                    }
                } else {
                    elog(WARNING, "Orochi Auth: failed to detach partition %s: %d", partition_name,
                         ret);
                }

                pfree(detach_query.data);
            }
        }

        pfree(query.data);
        SPI_finish();
        connected = false;
    }
    PG_CATCH();
    {
        if (connected)
            SPI_finish();

        /* Log but don't fail */
        elog(WARNING, "Orochi Auth: failed to archive audit partitions");
        FlushErrorState();
    }
    PG_END_TRY();
}

/* ============================================================
 * Background Worker Functions
 * ============================================================ */

/*
 * orochi_auth_token_cleanup_worker_main
 *    Background worker for token cleanup
 */
void orochi_auth_token_cleanup_worker_main(Datum main_arg)
{
    /* Set up signal handlers */
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    /* Unblock signals */
    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth token cleanup worker started");

    while (!ShutdownRequestPending) {
        int rc;

        /* Wait for latch or timeout */
        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       OROCHI_AUTH_TOKEN_CLEANUP_SEC * 1000L, PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        /* Perform cleanup tasks */
        orochi_auth_cleanup_expired_tokens();
        orochi_auth_cleanup_expired_sessions();
        orochi_auth_cleanup_rate_limits();
        orochi_auth_process_revocation_queue();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * orochi_auth_session_worker_main
 *    Background worker for session management
 */
void orochi_auth_session_worker_main(Datum main_arg)
{
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth session worker started");

    while (!ShutdownRequestPending) {
        int rc;

        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       60000L, /* 1 minute */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        /* Update session states */
        orochi_auth_update_session_states();

        /* Enforce session limits */
        orochi_auth_enforce_session_limits();

        /* Ensure audit partitions exist */
        orochi_auth_ensure_audit_partitions();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * orochi_auth_key_rotation_worker_main
 *    Background worker for key rotation
 */
void orochi_auth_key_rotation_worker_main(Datum main_arg)
{
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth key rotation worker started");

    while (!ShutdownRequestPending) {
        int rc;

        rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       86400000L, /* 24 hours */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        /* Check and rotate signing keys */
        orochi_auth_check_key_rotation();

        /* Refresh signing key cache */
        orochi_auth_refresh_signing_key_cache();

        /* Archive old audit log partitions (runs daily) */
        orochi_auth_archive_audit_partitions();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * orochi_auth_process_revocation_queue
 *    Process pending token revocations
 *
 * This function checks for recently revoked tokens in the database
 * and invalidates them in the shared memory cache. It handles:
 *   - Revoked refresh tokens (auth.refresh_tokens)
 *   - Revoked API keys (orochi.orochi_api_keys)
 *   - Invalidated sessions (auth.sessions)
 */
void orochi_auth_process_revocation_queue(void)
{
    static TimestampTz last_revocation_check = 0;
    TimestampTz now;
    TimestampTz check_window;
    StringInfoData query;
    int ret;
    int i;

    now = GetCurrentTimestamp();

    /* Initialize last check time on first call */
    if (last_revocation_check == 0)
        last_revocation_check = now - (60 * USECS_PER_SEC); /* 60 seconds ago */

    /* Don't check more than once per second */
    if (now - last_revocation_check < USECS_PER_SEC)
        return;

    /* Query window: check for items revoked since last check, plus buffer */
    check_window = last_revocation_check - (5 * USECS_PER_SEC); /* 5 sec buffer */

    if (SPI_connect() != SPI_OK_CONNECT) {
        elog(DEBUG1, "Failed to connect to SPI for revocation processing");
        return;
    }

    initStringInfo(&query);

    /*
     * Check for revoked sessions and invalidate their session IDs
     */
    appendStringInfo(&query,
                     "SELECT id::text FROM auth.sessions "
                     "WHERE not_after IS NOT NULL AND not_after > '%s'::timestamptz "
                     "AND not_after <= '%s'::timestamptz",
                     timestamptz_to_str(check_window), timestamptz_to_str(now));

    ret = SPI_execute(query.data, true, 100);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        for (i = 0; i < (int)SPI_processed; i++) {
            char *session_id = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            if (session_id != NULL) {
                orochi_auth_cache_invalidate_session(session_id);
                elog(DEBUG1, "Invalidated revoked session: %s", session_id);
                pfree(session_id);
            }
        }
    }

    /*
     * Check for revoked API keys
     */
    resetStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT key_hash FROM orochi.orochi_api_keys "
                     "WHERE is_revoked = TRUE "
                     "AND revoked_at > '%s'::timestamptz "
                     "AND revoked_at <= '%s'::timestamptz",
                     timestamptz_to_str(check_window), timestamptz_to_str(now));

    ret = SPI_execute(query.data, true, 100);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        for (i = 0; i < (int)SPI_processed; i++) {
            char *key_hash = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            if (key_hash != NULL) {
                orochi_auth_cache_invalidate_token(key_hash);
                elog(DEBUG1, "Invalidated revoked API key");
                pfree(key_hash);
            }
        }
    }

    /*
     * Check for revoked refresh tokens - invalidate by user_id
     * since refresh tokens are tied to users
     */
    resetStringInfo(&query);
    appendStringInfo(&query,
                     "SELECT DISTINCT user_id::text FROM auth.refresh_tokens "
                     "WHERE revoked = TRUE "
                     "AND updated_at > '%s'::timestamptz "
                     "AND updated_at <= '%s'::timestamptz",
                     timestamptz_to_str(check_window), timestamptz_to_str(now));

    ret = SPI_execute(query.data, true, 100);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        for (i = 0; i < (int)SPI_processed; i++) {
            char *user_id = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            if (user_id != NULL) {
                /* Don't invalidate all tokens for user on token rotation,
                 * but do track it for debugging */
                elog(DEBUG1, "Refresh token revoked for user: %s", user_id);
                pfree(user_id);
            }
        }
    }

    SPI_finish();
    pfree(query.data);

    /* Update last check timestamp */
    last_revocation_check = now;
}
