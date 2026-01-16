/*-------------------------------------------------------------------------
 *
 * raft_log.c
 *    Orochi DB Raft log management and persistence
 *
 * This module manages the Raft log including:
 *   - In-memory log storage with array-based entries
 *   - Append, get, truncate operations
 *   - WAL persistence for durability
 *   - Log compaction via snapshots
 *   - Recovery from WAL on restart
 *
 * The log is the central data structure in Raft, containing all commands
 * that need to be replicated across the cluster.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "storage/fd.h"
#include "miscadmin.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "raft.h"

/* ============================================================
 * WAL Record Types
 * ============================================================ */

#define RAFT_WAL_MAGIC          0x52414654  /* "RAFT" */
#define RAFT_WAL_VERSION        1

typedef enum RaftWALRecordType
{
    RAFT_WAL_ENTRY = 1,         /* Log entry record */
    RAFT_WAL_TRUNCATE,          /* Log truncation record */
    RAFT_WAL_COMMIT,            /* Commit index update */
    RAFT_WAL_SNAPSHOT           /* Snapshot marker */
} RaftWALRecordType;

/*
 * WAL file header
 */
typedef struct RaftWALHeader
{
    uint32              magic;              /* RAFT_WAL_MAGIC */
    uint32              version;            /* WAL format version */
    int32               node_id;            /* Node that wrote this WAL */
    uint64              first_index;        /* First log index in file */
    TimestampTz         created_at;         /* File creation time */
} RaftWALHeader;

/*
 * WAL record header
 */
typedef struct RaftWALRecord
{
    RaftWALRecordType   type;               /* Record type */
    uint32              size;               /* Record payload size */
    uint32              crc;                /* CRC32 of payload */
    uint64              index;              /* Log index (for entry records) */
} RaftWALRecord;

/* ============================================================
 * Private Function Declarations
 * ============================================================ */

static void raft_log_ensure_capacity(RaftLog *log, int32 needed);
static uint32 raft_log_compute_crc(const char *data, int32 size);
static bool raft_log_write_wal_header(RaftLog *log, int fd);
static bool raft_log_write_wal_record(RaftLog *log, int fd,
                                      RaftWALRecordType type,
                                      const char *data, int32 size,
                                      uint64 index);
static bool raft_log_write_full(int fd, const void *buf, size_t count);
static bool raft_log_read_full(int fd, void *buf, size_t count);
static bool raft_log_read_wal_record(int fd, RaftWALRecord *record,
                                     char **data);

/* ============================================================
 * Log Initialization and Cleanup
 * ============================================================ */

/*
 * raft_log_init - Initialize a new Raft log
 */
RaftLog *
raft_log_init(const char *wal_path)
{
    RaftLog *log;
    MemoryContext old_context;

    old_context = MemoryContextSwitchTo(TopMemoryContext);

    log = (RaftLog *) palloc0(sizeof(RaftLog));

    /* Initialize indices */
    log->first_index = 1;   /* Raft logs are 1-indexed */
    log->last_index = 0;    /* Empty log */
    log->commit_index = 0;
    log->last_applied = 0;

    /* Initialize entry storage */
    log->capacity = 1024;   /* Start with 1024 entries */
    log->entries = (RaftLogEntry *) palloc0(sizeof(RaftLogEntry) * log->capacity);

    /* WAL path */
    if (wal_path != NULL)
    {
        log->wal_path = pstrdup(wal_path);
        log->persistence_enabled = true;
    }
    else
    {
        log->wal_path = NULL;
        log->persistence_enabled = false;
    }

    /* Lock will be assigned from shared memory if needed */
    log->lock = NULL;

    MemoryContextSwitchTo(old_context);

    elog(DEBUG1, "Raft log initialized (capacity=%d, persistence=%s)",
         log->capacity, log->persistence_enabled ? "enabled" : "disabled");

    return log;
}

/*
 * raft_log_free - Free all log resources
 */
void
raft_log_free(RaftLog *log)
{
    uint64 i;
    uint64 entry_count;

    if (log == NULL)
        return;

    /* Free entry command data - check for empty log to avoid underflow */
    if (log->last_index >= log->first_index)
    {
        entry_count = log->last_index - log->first_index + 1;
        for (i = 0; i < entry_count; i++)
        {
            if (log->entries[i].command_data != NULL)
            {
                pfree(log->entries[i].command_data);
                log->entries[i].command_data = NULL;
            }
        }
    }

    /* Free entries array */
    if (log->entries != NULL)
        pfree(log->entries);

    /* Free WAL path */
    if (log->wal_path != NULL)
        pfree(log->wal_path);

    pfree(log);
}

/* ============================================================
 * Log Entry Operations
 * ============================================================ */

/*
 * raft_log_append - Append entries to the log
 */
bool
raft_log_append(RaftLog *log, RaftLogEntry *entries, int32 count)
{
    int32 i;
    uint64 next_index;

    if (count <= 0)
        return true;

    /* Ensure we have space */
    raft_log_ensure_capacity(log, (log->last_index - log->first_index + 1) + count);

    next_index = log->last_index + 1;

    for (i = 0; i < count; i++)
    {
        uint64 array_index = next_index - log->first_index;
        RaftLogEntry *dest = &log->entries[array_index];
        RaftLogEntry *src = &entries[i];

        /* Copy entry */
        dest->index = next_index;
        dest->term = src->term;
        dest->type = src->type;
        dest->created_at = src->created_at ? src->created_at : GetCurrentTimestamp();

        /* Copy command data */
        if (src->command_size > 0 && src->command_data != NULL)
        {
            dest->command_size = src->command_size;
            dest->command_data = palloc(src->command_size);
            memcpy(dest->command_data, src->command_data, src->command_size);
        }
        else
        {
            dest->command_size = 0;
            dest->command_data = NULL;
        }

        next_index++;
    }

    log->last_index = next_index - 1;

    /* Persist to WAL if enabled */
    if (log->persistence_enabled)
    {
        raft_log_persist(log);
    }

    elog(DEBUG1, "Raft log: appended %d entries (last_index=%lu)",
         count, log->last_index);

    return true;
}

/*
 * raft_log_get - Get entry at specified index
 */
RaftLogEntry *
raft_log_get(RaftLog *log, uint64 index)
{
    uint64 array_index;

    /* Check bounds */
    if (index < log->first_index || index > log->last_index)
        return NULL;

    array_index = index - log->first_index;
    return &log->entries[array_index];
}

/*
 * raft_log_get_range - Get entries in range [start_index, end_index]
 */
RaftLogEntry *
raft_log_get_range(RaftLog *log, uint64 start_index, uint64 end_index,
                   int32 *count)
{
    RaftLogEntry *result;
    uint64 i;
    int32 result_count;

    *count = 0;

    /* Validate range */
    if (start_index > end_index ||
        start_index < log->first_index ||
        end_index > log->last_index)
    {
        return NULL;
    }

    result_count = (int32)(end_index - start_index + 1);
    result = (RaftLogEntry *) palloc(sizeof(RaftLogEntry) * result_count);

    for (i = start_index; i <= end_index; i++)
    {
        RaftLogEntry *src = raft_log_get(log, i);
        RaftLogEntry *dest = &result[i - start_index];

        if (src == NULL)
        {
            pfree(result);
            return NULL;
        }

        /* Shallow copy - caller must not free command_data */
        *dest = *src;
    }

    *count = result_count;
    return result;
}

/*
 * raft_log_truncate - Truncate log after given index
 *
 * Removes all entries with index > after_index
 */
void
raft_log_truncate(RaftLog *log, uint64 after_index)
{
    uint64 i;

    /* Nothing to truncate */
    if (after_index >= log->last_index)
        return;

    /* Validate */
    if (after_index < log->first_index - 1)
    {
        elog(WARNING, "Cannot truncate before first_index");
        return;
    }

    /* Free command data for entries being removed */
    for (i = after_index + 1; i <= log->last_index; i++)
    {
        uint64 array_index = i - log->first_index;

        if (log->entries[array_index].command_data != NULL)
        {
            pfree(log->entries[array_index].command_data);
            log->entries[array_index].command_data = NULL;
        }
    }

    log->last_index = after_index;

    /* Update commit_index if necessary */
    if (log->commit_index > log->last_index)
        log->commit_index = log->last_index;

    /* Update last_applied if necessary */
    if (log->last_applied > log->last_index)
        log->last_applied = log->last_index;

    /* Persist truncation to WAL */
    if (log->persistence_enabled)
    {
        raft_log_persist(log);
    }

    elog(DEBUG1, "Raft log: truncated after index %lu (new last_index=%lu)",
         after_index, log->last_index);
}

/*
 * raft_log_term_at - Get term of entry at index
 */
uint64
raft_log_term_at(RaftLog *log, uint64 index)
{
    RaftLogEntry *entry;

    /* Index 0 has term 0 by convention */
    if (index == 0)
        return 0;

    entry = raft_log_get(log, index);
    if (entry == NULL)
        return 0;

    return entry->term;
}

/*
 * raft_log_ensure_capacity - Grow log array if needed
 */
static void
raft_log_ensure_capacity(RaftLog *log, int32 needed)
{
    if (needed <= log->capacity)
        return;

    /* Double capacity until sufficient */
    while (log->capacity < needed)
        log->capacity *= 2;

    /* Cap at maximum */
    if (log->capacity > RAFT_MAX_LOG_ENTRIES)
        log->capacity = RAFT_MAX_LOG_ENTRIES;

    log->entries = (RaftLogEntry *) repalloc(log->entries,
                                              sizeof(RaftLogEntry) * log->capacity);

    elog(DEBUG1, "Raft log: grew capacity to %d entries", log->capacity);
}

/* ============================================================
 * Log Compaction
 * ============================================================ */

/*
 * raft_log_compact - Compact log up to given index
 *
 * This is called after a snapshot is taken. Entries up to and including
 * up_to_index can be discarded since they're in the snapshot.
 */
void
raft_log_compact(RaftLog *log, uint64 up_to_index)
{
    uint64 entries_to_remove;
    uint64 remaining_entries;
    uint64 i;

    /* Validate */
    if (up_to_index < log->first_index)
    {
        elog(DEBUG1, "Raft log: nothing to compact (up_to=%lu, first=%lu)",
             up_to_index, log->first_index);
        return;
    }

    if (up_to_index > log->last_index)
        up_to_index = log->last_index;

    /* Cannot compact uncommitted entries */
    if (up_to_index > log->commit_index)
        up_to_index = log->commit_index;

    /* Cannot compact unapplied entries */
    if (up_to_index > log->last_applied)
        up_to_index = log->last_applied;

    entries_to_remove = up_to_index - log->first_index + 1;
    remaining_entries = log->last_index - up_to_index;

    /* Free command data for compacted entries */
    for (i = 0; i < entries_to_remove; i++)
    {
        if (log->entries[i].command_data != NULL)
        {
            pfree(log->entries[i].command_data);
            log->entries[i].command_data = NULL;
        }
    }

    /* Shift remaining entries to start of array */
    if (remaining_entries > 0)
    {
        memmove(log->entries,
                &log->entries[entries_to_remove],
                sizeof(RaftLogEntry) * remaining_entries);
    }

    /* Clear the freed slots */
    memset(&log->entries[remaining_entries], 0,
           sizeof(RaftLogEntry) * entries_to_remove);

    /* Update first_index */
    log->first_index = up_to_index + 1;

    elog(LOG, "Raft log: compacted %lu entries (new first_index=%lu)",
         entries_to_remove, log->first_index);
}

/* ============================================================
 * WAL Persistence
 * ============================================================ */

/*
 * raft_log_persist - Write log entries to WAL
 */
void
raft_log_persist(RaftLog *log)
{
    StringInfoData path;
    int fd;
    uint64 i;
    struct stat statbuf;
    bool new_file = false;

    if (!log->persistence_enabled || log->wal_path == NULL)
        return;

    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_wal_%lu.log",
                     log->wal_path, log->first_index);

    /* Check if file exists */
    if (stat(path.data, &statbuf) != 0)
        new_file = true;

    /* Open or create WAL file */
    fd = open(path.data, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        elog(WARNING, "Failed to open Raft WAL file: %s", path.data);
        pfree(path.data);
        return;
    }

    /* Write header for new files */
    if (new_file)
    {
        if (!raft_log_write_wal_header(log, fd))
        {
            elog(WARNING, "Failed to write Raft WAL header: %m");
            close(fd);
            pfree(path.data);
            return;
        }
    }

    /* Write all entries */
    for (i = log->first_index; i <= log->last_index; i++)
    {
        RaftLogEntry *entry = raft_log_get(log, i);
        StringInfoData entry_buf;

        if (entry == NULL)
            continue;

        /* Serialize entry */
        initStringInfo(&entry_buf);
        appendBinaryStringInfo(&entry_buf, (char *) &entry->term, sizeof(uint64));
        appendBinaryStringInfo(&entry_buf, (char *) &entry->type, sizeof(int32));
        appendBinaryStringInfo(&entry_buf, (char *) &entry->command_size, sizeof(int32));
        appendBinaryStringInfo(&entry_buf, (char *) &entry->created_at, sizeof(TimestampTz));

        if (entry->command_size > 0 && entry->command_data != NULL)
            appendBinaryStringInfo(&entry_buf, entry->command_data, entry->command_size);

        /* Write record */
        if (!raft_log_write_wal_record(log, fd, RAFT_WAL_ENTRY,
                                       entry_buf.data, entry_buf.len, entry->index))
        {
            elog(WARNING, "Failed to write Raft WAL entry at index %lu: %m", entry->index);
            pfree(entry_buf.data);
            close(fd);
            pfree(path.data);
            return;
        }

        pfree(entry_buf.data);
    }

    /* Write commit index marker */
    if (!raft_log_write_wal_record(log, fd, RAFT_WAL_COMMIT,
                                   (char *) &log->commit_index,
                                   sizeof(uint64), log->commit_index))
    {
        elog(WARNING, "Failed to write Raft WAL commit marker: %m");
        close(fd);
        pfree(path.data);
        return;
    }

    /* Sync to disk */
    if (fsync(fd) != 0)
    {
        elog(WARNING, "Failed to fsync Raft WAL file: %m");
        close(fd);
        pfree(path.data);
        return;
    }
    close(fd);

    pfree(path.data);

    elog(DEBUG1, "Raft log: persisted entries %lu-%lu to WAL",
         log->first_index, log->last_index);
}

/*
 * raft_log_recover - Recover log from WAL on startup
 */
void
raft_log_recover(RaftLog *log)
{
    StringInfoData path;
    int fd;
    RaftWALHeader header;
    RaftWALRecord record;
    char *data;
    ssize_t bytes_read;

    if (!log->persistence_enabled || log->wal_path == NULL)
        return;

    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_wal_%lu.log",
                     log->wal_path, log->first_index);

    fd = open(path.data, O_RDONLY);
    if (fd < 0)
    {
        elog(DEBUG1, "No Raft WAL file to recover: %s", path.data);
        pfree(path.data);
        return;
    }

    /* Read and verify header */
    bytes_read = read(fd, &header, sizeof(RaftWALHeader));
    if (bytes_read != sizeof(RaftWALHeader))
    {
        elog(WARNING, "Invalid Raft WAL header");
        close(fd);
        pfree(path.data);
        return;
    }

    if (header.magic != RAFT_WAL_MAGIC)
    {
        elog(WARNING, "Invalid Raft WAL magic number");
        close(fd);
        pfree(path.data);
        return;
    }

    if (header.version != RAFT_WAL_VERSION)
    {
        elog(WARNING, "Unsupported Raft WAL version: %u", header.version);
        close(fd);
        pfree(path.data);
        return;
    }

    log->first_index = header.first_index;

    /* Read records */
    while (raft_log_read_wal_record(fd, &record, &data))
    {
        switch (record.type)
        {
            case RAFT_WAL_ENTRY:
            {
                RaftLogEntry entry;
                const char *ptr = data;

                memset(&entry, 0, sizeof(RaftLogEntry));
                entry.index = record.index;

                memcpy(&entry.term, ptr, sizeof(uint64));
                ptr += sizeof(uint64);
                memcpy(&entry.type, ptr, sizeof(int32));
                ptr += sizeof(int32);
                memcpy(&entry.command_size, ptr, sizeof(int32));
                ptr += sizeof(int32);
                memcpy(&entry.created_at, ptr, sizeof(TimestampTz));
                ptr += sizeof(TimestampTz);

                if (entry.command_size > 0)
                {
                    entry.command_data = palloc(entry.command_size);
                    memcpy(entry.command_data, ptr, entry.command_size);
                }

                /* Append entry to in-memory log */
                if (entry.index > log->last_index)
                {
                    raft_log_ensure_capacity(log, entry.index - log->first_index + 1);

                    uint64 array_index = entry.index - log->first_index;
                    log->entries[array_index] = entry;
                    log->last_index = entry.index;
                }

                break;
            }

            case RAFT_WAL_COMMIT:
            {
                uint64 commit_index;
                memcpy(&commit_index, data, sizeof(uint64));

                if (commit_index > log->commit_index &&
                    commit_index <= log->last_index)
                {
                    log->commit_index = commit_index;
                }
                break;
            }

            case RAFT_WAL_TRUNCATE:
            {
                uint64 after_index;
                memcpy(&after_index, data, sizeof(uint64));
                raft_log_truncate(log, after_index);
                break;
            }

            case RAFT_WAL_SNAPSHOT:
                /* Snapshot markers are handled separately */
                break;
        }

        if (data != NULL)
            pfree(data);
    }

    close(fd);
    pfree(path.data);

    elog(LOG, "Raft log: recovered %lu entries from WAL (index %lu-%lu)",
         log->last_index - log->first_index + 1,
         log->first_index, log->last_index);
}

/*
 * raft_log_write_full - Write full buffer with retry on partial writes
 *
 * Returns true on success, false on error (errno is set).
 */
static bool
raft_log_write_full(int fd, const void *buf, size_t count)
{
    const char *ptr = (const char *) buf;
    size_t remaining = count;

    while (remaining > 0)
    {
        ssize_t written = write(fd, ptr, remaining);

        if (written < 0)
        {
            if (errno == EINTR)
                continue;  /* Retry on interrupt */
            return false;  /* Real error */
        }

        ptr += written;
        remaining -= written;
    }

    return true;
}

/*
 * raft_log_read_full - Read full buffer with retry on partial reads
 *
 * Returns true on success, false on error or EOF before completing read.
 */
static bool
raft_log_read_full(int fd, void *buf, size_t count)
{
    char *ptr = (char *) buf;
    size_t remaining = count;

    while (remaining > 0)
    {
        ssize_t bytes_read = read(fd, ptr, remaining);

        if (bytes_read < 0)
        {
            if (errno == EINTR)
                continue;  /* Retry on interrupt */
            return false;  /* Real error */
        }

        if (bytes_read == 0)
            return false;  /* EOF before completing read */

        ptr += bytes_read;
        remaining -= bytes_read;
    }

    return true;
}

/*
 * raft_log_write_wal_header - Write WAL file header
 *
 * Returns true on success, false on error.
 */
static bool
raft_log_write_wal_header(RaftLog *log, int fd)
{
    RaftWALHeader header;

    header.magic = RAFT_WAL_MAGIC;
    header.version = RAFT_WAL_VERSION;
    header.node_id = 0;  /* Will be set by caller if needed */
    header.first_index = log->first_index;
    header.created_at = GetCurrentTimestamp();

    return raft_log_write_full(fd, &header, sizeof(RaftWALHeader));
}

/*
 * raft_log_write_wal_record - Write a record to WAL
 *
 * Returns true on success, false on error.
 */
static bool
raft_log_write_wal_record(RaftLog *log, int fd, RaftWALRecordType type,
                          const char *data, int32 size, uint64 index)
{
    RaftWALRecord record;

    record.type = type;
    record.size = size;
    record.crc = raft_log_compute_crc(data, size);
    record.index = index;

    if (!raft_log_write_full(fd, &record, sizeof(RaftWALRecord)))
        return false;

    if (size > 0 && data != NULL)
    {
        if (!raft_log_write_full(fd, data, size))
            return false;
    }

    return true;
}

/*
 * raft_log_read_wal_record - Read a record from WAL
 *
 * Returns true on success, false on error or EOF.
 * On error (not EOF), logs a warning with details.
 */
static bool
raft_log_read_wal_record(int fd, RaftWALRecord *record, char **data)
{
    uint32 computed_crc;

    *data = NULL;

    /* Read record header */
    if (!raft_log_read_full(fd, record, sizeof(RaftWALRecord)))
    {
        /* Check if this is a read error vs EOF */
        if (errno != 0 && errno != EINTR)
            elog(WARNING, "Failed to read Raft WAL record header: %m");
        return false;
    }

    /* Read payload */
    if (record->size > 0)
    {
        *data = palloc(record->size);

        if (!raft_log_read_full(fd, *data, record->size))
        {
            if (errno != 0 && errno != EINTR)
                elog(WARNING, "Failed to read Raft WAL record payload at index %lu: %m",
                     record->index);
            else
                elog(WARNING, "Truncated Raft WAL record at index %lu", record->index);
            pfree(*data);
            *data = NULL;
            return false;
        }

        /* Verify CRC */
        computed_crc = raft_log_compute_crc(*data, record->size);
        if (computed_crc != record->crc)
        {
            elog(WARNING, "Raft WAL CRC mismatch at index %lu", record->index);
            pfree(*data);
            *data = NULL;
            return false;
        }
    }

    return true;
}

/*
 * raft_log_compute_crc - Compute CRC32 checksum
 *
 * Simple CRC32 implementation for WAL integrity checking.
 */
static uint32
raft_log_compute_crc(const char *data, int32 size)
{
    static const uint32 crc_table[256] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd706b3,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };

    uint32 crc = 0xFFFFFFFF;
    int32 i;

    for (i = 0; i < size; i++)
    {
        crc = crc_table[(crc ^ (unsigned char)data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ============================================================
 * Snapshot Support
 * ============================================================ */

/*
 * RaftSnapshot - Snapshot of state machine at a point in time
 */
typedef struct RaftSnapshot
{
    uint64              last_included_index;
    uint64              last_included_term;
    char               *data;
    int32               data_size;
    TimestampTz         created_at;
} RaftSnapshot;

/*
 * raft_log_create_snapshot - Create a snapshot of the state machine
 *
 * This is called by the application when it has persisted its state
 * up to last_included_index. After this, we can compact the log.
 */
void
raft_log_create_snapshot(RaftLog *log, uint64 last_included_index,
                         const char *snapshot_data, int32 snapshot_size)
{
    StringInfoData path;
    int fd;

    if (!log->persistence_enabled || log->wal_path == NULL)
        return;

    /* Write snapshot to file */
    initStringInfo(&path);
    appendStringInfo(&path, "%s/raft_snapshot_%lu.snap",
                     log->wal_path, last_included_index);

    fd = open(path.data, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        elog(WARNING, "Failed to create snapshot file: %s", path.data);
        pfree(path.data);
        return;
    }

    /* Write snapshot header */
    RaftSnapshot snap;
    snap.last_included_index = last_included_index;
    snap.last_included_term = raft_log_term_at(log, last_included_index);
    snap.data_size = snapshot_size;
    snap.created_at = GetCurrentTimestamp();

    if (!raft_log_write_full(fd, &snap.last_included_index, sizeof(uint64)) ||
        !raft_log_write_full(fd, &snap.last_included_term, sizeof(uint64)) ||
        !raft_log_write_full(fd, &snap.data_size, sizeof(int32)) ||
        !raft_log_write_full(fd, &snap.created_at, sizeof(TimestampTz)))
    {
        elog(WARNING, "Failed to write Raft snapshot header: %m");
        close(fd);
        pfree(path.data);
        return;
    }

    /* Write snapshot data */
    if (snapshot_size > 0 && snapshot_data != NULL)
    {
        if (!raft_log_write_full(fd, snapshot_data, snapshot_size))
        {
            elog(WARNING, "Failed to write Raft snapshot data: %m");
            close(fd);
            pfree(path.data);
            return;
        }
    }

    if (fsync(fd) != 0)
    {
        elog(WARNING, "Failed to fsync Raft snapshot file: %m");
        close(fd);
        pfree(path.data);
        return;
    }
    close(fd);

    pfree(path.data);

    /* Compact the log */
    raft_log_compact(log, last_included_index);

    elog(LOG, "Raft log: created snapshot at index %lu, compacted log",
         last_included_index);
}

/*
 * raft_log_load_snapshot - Load the most recent snapshot
 *
 * Returns snapshot data that caller must free, or NULL if no snapshot.
 */
char *
raft_log_load_snapshot(RaftLog *log, uint64 *last_included_index,
                       uint64 *last_included_term, int32 *data_size)
{
    StringInfoData pattern;
    DIR *dir;
    struct dirent *entry;
    uint64 max_index = 0;
    char *best_file = NULL;
    char *snapshot_data = NULL;

    if (!log->persistence_enabled || log->wal_path == NULL)
        return NULL;

    /* Find the most recent snapshot file */
    dir = opendir(log->wal_path);
    if (dir == NULL)
        return NULL;

    while ((entry = readdir(dir)) != NULL)
    {
        uint64 index;

        if (sscanf(entry->d_name, "raft_snapshot_%lu.snap", &index) == 1)
        {
            if (index > max_index)
            {
                max_index = index;
                if (best_file != NULL)
                    pfree(best_file);
                best_file = pstrdup(entry->d_name);
            }
        }
    }
    closedir(dir);

    if (best_file == NULL)
        return NULL;

    /* Load the snapshot */
    initStringInfo(&pattern);
    appendStringInfo(&pattern, "%s/%s", log->wal_path, best_file);
    pfree(best_file);

    int fd = open(pattern.data, O_RDONLY);
    if (fd < 0)
    {
        pfree(pattern.data);
        return NULL;
    }

    /* Read header */
    uint64 snap_index, snap_term;
    int32 snap_size;
    TimestampTz snap_time;

    if (!raft_log_read_full(fd, &snap_index, sizeof(uint64)) ||
        !raft_log_read_full(fd, &snap_term, sizeof(uint64)) ||
        !raft_log_read_full(fd, &snap_size, sizeof(int32)) ||
        !raft_log_read_full(fd, &snap_time, sizeof(TimestampTz)))
    {
        elog(WARNING, "Failed to read Raft snapshot header: %m");
        close(fd);
        pfree(pattern.data);
        return NULL;
    }

    /* Validate snapshot size before allocation */
    if (snap_size < 0 || snap_size > 1024 * 1024 * 1024)  /* 1GB max */
    {
        elog(WARNING, "Invalid Raft snapshot size: %d", snap_size);
        close(fd);
        pfree(pattern.data);
        return NULL;
    }

    /* Read data */
    if (snap_size > 0)
    {
        snapshot_data = palloc(snap_size);
        if (!raft_log_read_full(fd, snapshot_data, snap_size))
        {
            elog(WARNING, "Failed to read Raft snapshot data: %m");
            pfree(snapshot_data);
            close(fd);
            pfree(pattern.data);
            return NULL;
        }
    }

    close(fd);
    pfree(pattern.data);

    *last_included_index = snap_index;
    *last_included_term = snap_term;
    *data_size = snap_size;

    elog(LOG, "Raft log: loaded snapshot at index %lu (term %lu, %d bytes)",
         snap_index, snap_term, snap_size);

    return snapshot_data;
}

/* ============================================================
 * Log Statistics and Debugging
 * ============================================================ */

/*
 * raft_log_get_stats - Get log statistics
 */
void
raft_log_get_stats(RaftLog *log, uint64 *first_index, uint64 *last_index,
                   uint64 *commit_index, uint64 *last_applied,
                   int32 *entry_count, int32 *capacity)
{
    if (first_index)
        *first_index = log->first_index;
    if (last_index)
        *last_index = log->last_index;
    if (commit_index)
        *commit_index = log->commit_index;
    if (last_applied)
        *last_applied = log->last_applied;
    if (entry_count)
        *entry_count = (int32)(log->last_index >= log->first_index ?
                               log->last_index - log->first_index + 1 : 0);
    if (capacity)
        *capacity = log->capacity;
}

/*
 * raft_log_dump - Dump log contents for debugging
 */
void
raft_log_dump(RaftLog *log, StringInfo buf)
{
    uint64 i;

    appendStringInfo(buf, "Raft Log: first=%lu, last=%lu, commit=%lu, applied=%lu\n",
                     log->first_index, log->last_index,
                     log->commit_index, log->last_applied);

    for (i = log->first_index; i <= log->last_index; i++)
    {
        RaftLogEntry *e = raft_log_get(log, i);
        if (e != NULL)
        {
            appendStringInfo(buf, "  [%lu] term=%lu type=%d size=%d\n",
                             e->index, e->term, e->type, e->command_size);
        }
    }
}
