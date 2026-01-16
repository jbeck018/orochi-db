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

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "storage/latch.h"
#include "utils/timestamp.h"
#include "utils/memutils.h"
#include "pgstat.h"

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
Size
orochi_auth_shmem_size(void)
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
void
orochi_auth_shmem_init(void)
{
    bool        found;
    LWLock     *locks;
    int         i;

    /* Allocate shared memory */
    OrochiAuthSharedState = ShmemInitStruct("orochi_auth_cache",
                                             orochi_auth_shmem_size(),
                                             &found);

    if (!found)
    {
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
        for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++)
        {
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
        for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++)
        {
            OrochiAuthSharedState->session_cache[i].session_id[0] = '\0';
            OrochiAuthSharedState->session_cache[i].state = OROCHI_SESSION_EXPIRED;
            OrochiAuthSharedState->session_cache[i].hash_slot = i;
        }

        /* Initialize signing key cache */
        OrochiAuthSharedState->signing_key_count = 0;
        for (i = 0; i < OROCHI_AUTH_MAX_SIGNING_KEYS; i++)
        {
            OrochiAuthSharedState->signing_keys[i].key_id[0] = '\0';
            OrochiAuthSharedState->signing_keys[i].is_active = false;
            OrochiAuthSharedState->signing_keys[i].is_current = false;
        }

        /* Initialize rate limiting */
        OrochiAuthSharedState->rate_limit_count = 0;
        for (i = 0; i < OROCHI_AUTH_MAX_RATE_LIMITS; i++)
        {
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

        elog(LOG, "Orochi Auth shared memory initialized: %zu bytes",
             orochi_auth_shmem_size());
    }
    else
    {
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
static uint32
orochi_auth_cache_hash_slot(const char *token_hash)
{
    uint32 hash = 0;
    int    i;

    /* Simple hash function for first 8 bytes of hash */
    for (i = 0; i < 8 && token_hash[i]; i++)
    {
        hash = hash * 31 + (unsigned char) token_hash[i];
    }

    return hash % OROCHI_AUTH_MAX_CACHED_TOKENS;
}

/*
 * orochi_auth_cache_lookup
 *    Look up token in cache (must hold lock)
 */
OrochiToken *
orochi_auth_cache_lookup(const char *token_hash)
{
    uint32       slot;
    int          probe_count;
    OrochiToken *entry;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return NULL;

    /* Calculate initial slot */
    slot = orochi_auth_cache_hash_slot(token_hash);

    /* Linear probing with limited probes */
    for (probe_count = 0; probe_count < 16; probe_count++)
    {
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

    return NULL;  /* Not found after probing */
}

/*
 * orochi_auth_cache_token
 *    Add token to cache
 */
void
orochi_auth_cache_token(OrochiTokenValidation *validation, const char *token_hash)
{
    OrochiToken *entry;
    uint32       slot;
    int          probe_count;
    TimestampTz  now;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    /* Calculate initial slot */
    slot = orochi_auth_cache_hash_slot(token_hash);

    /* Find empty slot or matching slot (with linear probing) */
    for (probe_count = 0; probe_count < 16; probe_count++)
    {
        entry = &OrochiAuthSharedState->token_cache[slot];

        /* Empty slot or expired entry - use it */
        if (entry->token_hash[0] == '\0' ||
            entry->expires_at < now ||
            strcmp(entry->token_hash, token_hash) == 0)
        {
            /* Fill in token data */
            strlcpy(entry->token_hash, token_hash, sizeof(entry->token_hash));
            entry->token_type = OROCHI_TOKEN_ACCESS;  /* Default */
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
void
orochi_auth_cache_invalidate_token(const char *token_hash)
{
    OrochiToken *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    entry = orochi_auth_cache_lookup(token_hash);
    if (entry != NULL)
    {
        entry->is_revoked = true;
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->tokens_revoked, 1);
    }

    LWLockRelease(OrochiAuthSharedState->token_cache_lock);
}

/*
 * orochi_auth_cache_invalidate_user
 *    Invalidate all tokens for a user
 */
void
orochi_auth_cache_invalidate_user(const char *user_id)
{
    int i;

    if (OrochiAuthSharedState == NULL || user_id == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++)
    {
        OrochiToken *entry = &OrochiAuthSharedState->token_cache[i];

        if (entry->token_hash[0] != '\0' &&
            strcmp(entry->user_id, user_id) == 0)
        {
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
void
orochi_auth_cleanup_expired_tokens(void)
{
    TimestampTz now;
    int         i;
    int         cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->token_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_TOKENS; i++)
    {
        OrochiToken *entry = &OrochiAuthSharedState->token_cache[i];

        if (entry->token_hash[0] != '\0' && entry->expires_at < now)
        {
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
static uint32
orochi_auth_session_hash_slot(const char *session_id)
{
    uint32 hash = 0;
    int    i;

    for (i = 0; i < 8 && session_id[i]; i++)
    {
        hash = hash * 31 + (unsigned char) session_id[i];
    }

    return hash % OROCHI_AUTH_MAX_CACHED_SESSIONS;
}

/*
 * orochi_auth_session_cache_lookup
 *    Look up session in cache
 */
OrochiSession *
orochi_auth_session_cache_lookup(const char *session_id)
{
    uint32         slot;
    int            probe_count;
    OrochiSession *entry;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return NULL;

    if (session_id == NULL || session_id[0] == '\0')
        return NULL;

    slot = orochi_auth_session_hash_slot(session_id);

    /* Linear probing */
    for (probe_count = 0; probe_count < 16; probe_count++)
    {
        entry = &OrochiAuthSharedState->session_cache[slot];

        if (entry->session_id[0] == '\0')
            return NULL;

        if (strcmp(entry->session_id, session_id) == 0 &&
            entry->state != OROCHI_SESSION_EXPIRED)
        {
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
void
orochi_auth_cache_session(OrochiSession *session)
{
    OrochiSession *entry;
    uint32         slot;
    int            probe_count;
    TimestampTz    now;

    if (OrochiAuthSharedState == NULL || !OrochiAuthSharedState->is_initialized)
        return;

    if (session == NULL || session->session_id[0] == '\0')
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    slot = orochi_auth_session_hash_slot(session->session_id);

    /* Find empty or matching slot */
    for (probe_count = 0; probe_count < 16; probe_count++)
    {
        entry = &OrochiAuthSharedState->session_cache[slot];

        if (entry->session_id[0] == '\0' ||
            entry->state == OROCHI_SESSION_EXPIRED ||
            strcmp(entry->session_id, session->session_id) == 0)
        {
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
void
orochi_auth_cache_invalidate_session(const char *session_id)
{
    OrochiSession *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    entry = orochi_auth_session_cache_lookup(session_id);
    if (entry != NULL)
    {
        entry->state = OROCHI_SESSION_REVOKED;
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->sessions_expired, 1);
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_cleanup_expired_sessions
 *    Remove expired sessions from cache
 */
void
orochi_auth_cleanup_expired_sessions(void)
{
    TimestampTz now;
    int         i;
    int         cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++)
    {
        OrochiSession *entry = &OrochiAuthSharedState->session_cache[i];

        if (entry->session_id[0] != '\0' &&
            (entry->expires_at < now || entry->state == OROCHI_SESSION_REVOKED))
        {
            entry->state = OROCHI_SESSION_EXPIRED;
            entry->session_id[0] = '\0';
            cleaned++;
        }
    }

    OrochiAuthSharedState->session_cache_count -= cleaned;
    if (OrochiAuthSharedState->session_cache_count < 0)
        OrochiAuthSharedState->session_cache_count = 0;

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);

    if (cleaned > 0)
    {
        pg_atomic_fetch_add_u64(&OrochiAuthSharedState->sessions_expired, cleaned);
        elog(LOG, "Orochi Auth: cleaned %d expired sessions", cleaned);
    }
}

/*
 * orochi_auth_update_session_states
 *    Update session states (mark idle sessions)
 */
void
orochi_auth_update_session_states(void)
{
    TimestampTz now;
    TimestampTz idle_threshold;
    int         i;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();
    /* Sessions idle for more than 30 minutes are marked IDLE */
    idle_threshold = now - (30 * 60 * USECS_PER_SEC);

    LWLockAcquire(OrochiAuthSharedState->session_cache_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_CACHED_SESSIONS; i++)
    {
        OrochiSession *entry = &OrochiAuthSharedState->session_cache[i];

        if (entry->session_id[0] != '\0' &&
            entry->state == OROCHI_SESSION_ACTIVE &&
            entry->last_activity < idle_threshold)
        {
            entry->state = OROCHI_SESSION_IDLE;
        }
    }

    LWLockRelease(OrochiAuthSharedState->session_cache_lock);
}

/*
 * orochi_auth_enforce_session_limits
 *    Enforce per-user session limits (revoke oldest sessions)
 */
void
orochi_auth_enforce_session_limits(void)
{
    /* TODO: Implement per-user session counting and limit enforcement */
}

/* ============================================================
 * Signing Key Cache Functions
 * ============================================================ */

/*
 * orochi_auth_get_signing_key
 *    Get current signing key for tenant
 */
OrochiSigningKey *
orochi_auth_get_signing_key(const char *tenant_id)
{
    int i;
    OrochiSigningKey *key = NULL;

    if (OrochiAuthSharedState == NULL)
        return NULL;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++)
    {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && entry->is_current)
        {
            /* Match tenant_id or use global key (empty tenant_id) */
            if (tenant_id == NULL ||
                entry->tenant_id[0] == '\0' ||
                strcmp(entry->tenant_id, tenant_id) == 0)
            {
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
OrochiSigningKey *
orochi_auth_get_signing_key_by_id(const char *key_id)
{
    int i;
    OrochiSigningKey *key = NULL;

    if (OrochiAuthSharedState == NULL || key_id == NULL)
        return NULL;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++)
    {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && strcmp(entry->key_id, key_id) == 0)
        {
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
void
orochi_auth_cache_signing_key(OrochiSigningKey *key)
{
    int i;
    OrochiSigningKey *entry = NULL;

    if (OrochiAuthSharedState == NULL || key == NULL)
        return;

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_EXCLUSIVE);

    /* Look for existing entry with same key_id */
    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++)
    {
        if (strcmp(OrochiAuthSharedState->signing_keys[i].key_id, key->key_id) == 0)
        {
            entry = &OrochiAuthSharedState->signing_keys[i];
            break;
        }
    }

    /* If not found, use next available slot */
    if (entry == NULL && OrochiAuthSharedState->signing_key_count < OROCHI_AUTH_MAX_SIGNING_KEYS)
    {
        entry = &OrochiAuthSharedState->signing_keys[OrochiAuthSharedState->signing_key_count];
        OrochiAuthSharedState->signing_key_count++;
    }

    if (entry != NULL)
    {
        /* Copy key data */
        memcpy(entry, key, sizeof(OrochiSigningKey));
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
}

/*
 * orochi_auth_check_key_rotation
 *    Check and rotate signing keys if needed
 */
void
orochi_auth_check_key_rotation(void)
{
    TimestampTz now;
    TimestampTz rotation_threshold;
    int         i;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();
    /* Keys older than rotation_days should be rotated */
    rotation_threshold = now - (orochi_auth_key_rotation_days * 24 * 60 * 60 * USECS_PER_SEC);

    LWLockAcquire(OrochiAuthSharedState->signing_key_lock, LW_SHARED);

    for (i = 0; i < OrochiAuthSharedState->signing_key_count; i++)
    {
        OrochiSigningKey *entry = &OrochiAuthSharedState->signing_keys[i];

        if (entry->is_active && entry->is_current &&
            entry->created_at < rotation_threshold)
        {
            elog(LOG, "Orochi Auth: signing key %s needs rotation", entry->key_id);
            /* TODO: Trigger key rotation via pg_notify or database update */
        }
    }

    LWLockRelease(OrochiAuthSharedState->signing_key_lock);
}

/*
 * orochi_auth_refresh_signing_key_cache
 *    Refresh signing key cache from database
 */
void
orochi_auth_refresh_signing_key_cache(void)
{
    /* TODO: Query database for signing keys and update cache */
    if (OrochiAuthSharedState != NULL)
    {
        OrochiAuthSharedState->last_key_rotation = GetCurrentTimestamp();
    }
}

/* ============================================================
 * Rate Limiting Functions
 * ============================================================ */

/*
 * orochi_auth_rate_limit_hash
 *    Calculate hash for rate limit key
 */
static uint32
orochi_auth_rate_limit_hash(const char *key, const char *action)
{
    uint32 hash = 0;
    const char *p;

    for (p = key; *p; p++)
        hash = hash * 31 + (unsigned char) *p;

    hash = hash * 17;

    for (p = action; *p; p++)
        hash = hash * 31 + (unsigned char) *p;

    return hash % OROCHI_AUTH_MAX_RATE_LIMITS;
}

/*
 * orochi_auth_check_rate_limit
 *    Check if request is allowed under rate limit
 */
bool
orochi_auth_check_rate_limit(const char *key, const char *action, int limit)
{
    uint32           slot;
    int              probe_count;
    OrochiRateLimit *entry;
    TimestampTz      now;
    TimestampTz      window_end;
    bool             allowed = true;

    if (!orochi_auth_rate_limit_enabled || OrochiAuthSharedState == NULL)
        return true;

    now = GetCurrentTimestamp();
    slot = orochi_auth_rate_limit_hash(key, action);

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    /* Find existing entry or empty slot */
    for (probe_count = 0; probe_count < 8; probe_count++)
    {
        entry = &OrochiAuthSharedState->rate_limits[slot];

        /* Empty slot */
        if (entry->key[0] == '\0')
        {
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
        if (strncmp(entry->key, key, strlen(key)) == 0)
        {
            window_end = entry->window_start + (entry->window_seconds * USECS_PER_SEC);

            /* Check if blocked */
            if (entry->is_blocked && entry->blocked_until > now)
            {
                allowed = false;
            }
            else if (now > window_end)
            {
                /* Window expired, reset */
                entry->count = 1;
                entry->window_start = now;
                entry->is_blocked = false;
                allowed = true;
            }
            else if (entry->count >= entry->limit)
            {
                /* Limit exceeded */
                entry->is_blocked = true;
                entry->blocked_until = now + (entry->window_seconds * USECS_PER_SEC);
                allowed = false;

                pg_atomic_fetch_add_u64(&OrochiAuthSharedState->failed_authentications, 1);
            }
            else
            {
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
    return true;  /* Allow if no slot available */
}

/*
 * orochi_auth_record_rate_limit
 *    Record a rate limit hit (increment counter)
 */
void
orochi_auth_record_rate_limit(const char *key, const char *action)
{
    /* Already handled in orochi_auth_check_rate_limit */
}

/*
 * orochi_auth_clear_rate_limit
 *    Clear rate limit for a key
 */
void
orochi_auth_clear_rate_limit(const char *key, const char *action)
{
    uint32           slot;
    int              probe_count;
    OrochiRateLimit *entry;

    if (OrochiAuthSharedState == NULL)
        return;

    slot = orochi_auth_rate_limit_hash(key, action);

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    for (probe_count = 0; probe_count < 8; probe_count++)
    {
        entry = &OrochiAuthSharedState->rate_limits[slot];

        if (entry->key[0] != '\0' && strncmp(entry->key, key, strlen(key)) == 0)
        {
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
void
orochi_auth_cleanup_rate_limits(void)
{
    TimestampTz now;
    int         i;
    int         cleaned = 0;

    if (OrochiAuthSharedState == NULL)
        return;

    now = GetCurrentTimestamp();

    LWLockAcquire(OrochiAuthSharedState->rate_limit_lock, LW_EXCLUSIVE);

    for (i = 0; i < OROCHI_AUTH_MAX_RATE_LIMITS; i++)
    {
        OrochiRateLimit *entry = &OrochiAuthSharedState->rate_limits[i];

        if (entry->key[0] != '\0')
        {
            TimestampTz window_end = entry->window_start +
                                     (entry->window_seconds * USECS_PER_SEC * 2);

            /* Remove entries that have been idle for 2x the window */
            if (now > window_end)
            {
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
 *    Log an authentication event
 */
void
orochi_auth_audit_log(const char *tenant_id,
                       const char *user_id,
                       const char *session_id,
                       OrochiAuditEvent event_type,
                       const char *ip_address,
                       const char *user_agent,
                       const char *resource_type,
                       const char *resource_id)
{
    if (!orochi_auth_audit_enabled)
        return;

    /*
     * TODO: Insert audit log entry into database table.
     * For now, just log to PostgreSQL log.
     */
    elog(LOG, "Orochi Auth Audit: event=%s tenant=%s user=%s session=%s ip=%s",
         orochi_auth_audit_event_name(event_type),
         tenant_id ? tenant_id : "(none)",
         user_id ? user_id : "(none)",
         session_id ? session_id : "(none)",
         ip_address ? ip_address : "(none)");
}

/*
 * orochi_auth_ensure_audit_partitions
 *    Ensure audit log partitions exist for current period
 */
void
orochi_auth_ensure_audit_partitions(void)
{
    /* TODO: Create monthly audit log partitions */
}

/*
 * orochi_auth_archive_audit_partitions
 *    Archive old audit log partitions
 */
void
orochi_auth_archive_audit_partitions(void)
{
    /* TODO: Archive partitions older than retention period */
}

/* ============================================================
 * Background Worker Functions
 * ============================================================ */

/*
 * orochi_auth_token_cleanup_worker_main
 *    Background worker for token cleanup
 */
void
orochi_auth_token_cleanup_worker_main(Datum main_arg)
{
    /* Set up signal handlers */
    pqsignal(SIGTERM, die);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    /* Unblock signals */
    BackgroundWorkerUnblockSignals();

    /* Connect to database */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth token cleanup worker started");

    while (!ShutdownRequestPending)
    {
        int rc;

        /* Wait for latch or timeout */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       OROCHI_AUTH_TOKEN_CLEANUP_SEC * 1000L,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending)
        {
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
void
orochi_auth_session_worker_main(Datum main_arg)
{
    pqsignal(SIGTERM, die);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth session worker started");

    while (!ShutdownRequestPending)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       60000L,  /* 1 minute */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending)
        {
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
void
orochi_auth_key_rotation_worker_main(Datum main_arg)
{
    pqsignal(SIGTERM, die);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi Auth key rotation worker started");

    while (!ShutdownRequestPending)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       86400000L,  /* 24 hours */
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);

        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        /* Check and rotate signing keys */
        orochi_auth_check_key_rotation();

        /* Refresh signing key cache */
        orochi_auth_refresh_signing_key_cache();

        CHECK_FOR_INTERRUPTS();
    }

    proc_exit(0);
}

/*
 * orochi_auth_process_revocation_queue
 *    Process pending token revocations
 */
void
orochi_auth_process_revocation_queue(void)
{
    /*
     * TODO: Check pg_notify queue for token revocation messages
     * and invalidate corresponding tokens in cache.
     */
}
