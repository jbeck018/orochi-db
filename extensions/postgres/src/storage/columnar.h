/*-------------------------------------------------------------------------
 *
 * columnar.h
 *    Orochi DB columnar storage engine
 *
 * The columnar storage engine provides:
 *   - Column-oriented data storage for analytics
 *   - Multiple compression algorithms per column
 *   - Vectorized read operations
 *   - Skip list support for predicate pushdown
 *   - Integration with PostgreSQL's Table Access Method (TAM)
 *
 * Data Organization:
 *   Table -> Stripes -> Chunk Groups -> Column Chunks
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_COLUMNAR_H
#define OROCHI_COLUMNAR_H

#include "postgres.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "nodes/execnodes.h"
#include "storage/bufpage.h"

#include "../orochi.h"

/* ============================================================
 * Configuration Constants
 * ============================================================ */

/* Default stripe configuration */
#define COLUMNAR_DEFAULT_STRIPE_ROW_LIMIT       150000
#define COLUMNAR_MIN_STRIPE_ROW_LIMIT           1000
#define COLUMNAR_MAX_STRIPE_ROW_LIMIT           10000000

/* Default chunk group configuration */
#define COLUMNAR_DEFAULT_CHUNK_GROUP_ROW_LIMIT  10000
#define COLUMNAR_MIN_CHUNK_GROUP_ROW_LIMIT      1000
#define COLUMNAR_MAX_CHUNK_GROUP_ROW_LIMIT      100000

/* Maximum decompressed stripe size (1GB) */
#define COLUMNAR_MAX_STRIPE_SIZE                (1024 * 1024 * 1024)

/* Vectorized execution batch size */
#define COLUMNAR_VECTOR_BATCH_SIZE              1024

/* Compression levels */
#define COLUMNAR_COMPRESSION_LEVEL_DEFAULT      3
#define COLUMNAR_COMPRESSION_LEVEL_MIN          1
#define COLUMNAR_COMPRESSION_LEVEL_MAX          19

/* ============================================================
 * Columnar Storage Structures
 * ============================================================ */

/*
 * ColumnarOptions - Per-table columnar storage options
 */
typedef struct ColumnarOptions
{
    int32               stripe_row_limit;       /* Max rows per stripe */
    int32               chunk_group_row_limit;  /* Max rows per chunk group */
    OrochiCompressionType compression_type;     /* Default compression */
    int32               compression_level;      /* Compression level */
    bool                enable_vectorization;   /* Enable vectorized ops */
} ColumnarOptions;

/*
 * ColumnarStripe - A stripe contains multiple chunk groups
 */
typedef struct ColumnarStripe
{
    int64               stripe_id;          /* Unique stripe ID */
    int64               first_row_number;   /* First row in stripe */
    int64               row_count;          /* Total rows in stripe */
    int32               chunk_group_count;  /* Number of chunk groups */
    int32               column_count;       /* Number of columns */
    int64               data_offset;        /* Offset to data in file */
    int64               data_size;          /* Compressed data size */
    int64               metadata_offset;    /* Offset to metadata */
    int64               metadata_size;      /* Metadata size */
    bool                is_flushed;         /* Has been written to disk */
} ColumnarStripe;

/*
 * ColumnarChunkGroup - A group of column chunks
 */
typedef struct ColumnarChunkGroup
{
    int32               chunk_group_index;  /* Index within stripe */
    int64               row_count;          /* Rows in this group */
    int64               deleted_rows;       /* Deleted row count */
    struct ColumnarColumnChunk **columns;   /* Column chunks */
    int32               column_count;       /* Number of columns */
    uint8              *deletion_mask;      /* Bit mask for deletions */
} ColumnarChunkGroup;

/*
 * ColumnarColumnChunk - Individual column data within chunk group
 */
typedef struct ColumnarColumnChunk
{
    int16               column_index;       /* Column position */
    Oid                 column_type;        /* PostgreSQL type OID */
    int64               value_count;        /* Number of values */
    int64               null_count;         /* Number of nulls */
    bool                has_nulls;          /* Contains null values */

    /* Compression info */
    OrochiCompressionType compression_type;
    int64               compressed_size;
    int64               decompressed_size;

    /* Data buffers */
    char               *value_buffer;       /* Compressed values */
    char               *null_buffer;        /* Null bitmap */

    /* Statistics for predicate pushdown */
    bool                has_min_max;        /* Min/max available */
    Datum               min_value;          /* Minimum value */
    Datum               max_value;          /* Maximum value */
    bool                min_is_null;
    bool                max_is_null;

    /* Bloom filter for equality predicates */
    void               *bloom_filter;
    int32               bloom_filter_size;
} ColumnarColumnChunk;

/*
 * ColumnarSkipList - Skip list entry for chunk elimination
 */
typedef struct ColumnarSkipListEntry
{
    int64               stripe_id;          /* Stripe identifier */
    int32               chunk_group_index;  /* Chunk group index */
    int16               column_index;       /* Column index */
    Datum               min_value;          /* Minimum value */
    Datum               max_value;          /* Maximum value */
    int64               row_count;          /* Row count */
    bool                has_nulls;          /* Contains nulls */
} ColumnarSkipListEntry;

/*
 * ColumnarSkipList - Complete skip list for a table
 */
typedef struct ColumnarSkipList
{
    int32               entry_count;        /* Number of entries */
    ColumnarSkipListEntry *entries;         /* Skip list entries */
} ColumnarSkipList;

/* ============================================================
 * Write State Management
 * ============================================================ */

/*
 * ColumnarWriteState - State for writing columnar data
 */
typedef struct ColumnarWriteState
{
    Relation            relation;           /* Target relation */
    TupleDesc           tupdesc;            /* Tuple descriptor */
    ColumnarOptions     options;            /* Storage options */

    /* Current stripe being written */
    ColumnarStripe     *current_stripe;
    int64               stripe_row_count;   /* Rows in current stripe */

    /* Current chunk group */
    ColumnarChunkGroup *current_chunk_group;
    int64               chunk_group_row_count;

    /* Per-column write buffers */
    struct ColumnWriteBuffer {
        Datum          *values;             /* Value buffer */
        bool           *nulls;              /* Null bitmap */
        int64           count;              /* Values buffered */
        int64           capacity;           /* Buffer capacity */
        Datum           min_value;          /* Running min */
        Datum           max_value;          /* Running max */
        bool            has_nulls;
        /* Compression state */
        Oid             attr_type;          /* Column data type OID */
        int64           row_count;          /* Number of rows in buffer */
        int64           null_count;         /* Number of null values */
        char           *compressed_data;    /* Compressed column data */
        int64           compressed_size;    /* Size of compressed data */
        int64           uncompressed_size;  /* Size of uncompressed data */
        OrochiCompressionType compression_type; /* Compression algorithm */
    } *column_buffers;

    /* Memory context for writes */
    MemoryContext       write_context;
    MemoryContext       stripe_context;

    /* Statistics */
    int64               total_rows_written;
    int64               total_bytes_written;
    int64               stripes_created;
} ColumnarWriteState;

/* ============================================================
 * Read State Management
 * ============================================================ */

/*
 * ColumnarReadState - State for reading columnar data
 */
typedef struct ColumnarReadState
{
    Relation            relation;           /* Source relation */
    TupleDesc           tupdesc;            /* Tuple descriptor */
    Snapshot            snapshot;           /* Transaction snapshot */

    /* Stripe navigation */
    List               *stripe_list;        /* List of ColumnarStripe */
    int32               current_stripe_index;
    ColumnarStripe     *current_stripe;

    /* Chunk group navigation */
    int32               current_chunk_group_index;
    ColumnarChunkGroup *current_chunk_group;
    int64               current_row_in_chunk;

    /* Column projection */
    Bitmapset          *columns_needed;     /* Required columns */

    /* Decompressed column data cache */
    struct ColumnReadBuffer {
        Datum          *values;
        bool           *nulls;
        int64           row_count;
        bool            is_loaded;
    } *column_buffers;

    /* Vectorization support */
    bool                enable_vectorization;
    int32               vector_batch_size;
    int64               vector_rows_pending;

    /* Skip list for predicate pushdown */
    ColumnarSkipList   *skip_list;
    List               *qual_list;          /* Filter predicates */

    /* Memory context */
    MemoryContext       read_context;
    MemoryContext       stripe_context;

    /* Statistics */
    int64               total_rows_read;
    int64               stripes_scanned;
    int64               chunks_skipped;
} ColumnarReadState;

/* ============================================================
 * Compression Interface
 * ============================================================ */

/*
 * CompressionBuffer - Buffer for compression operations
 */
typedef struct CompressionBuffer
{
    char               *data;
    int64               size;
    int64               capacity;
    OrochiCompressionType type;
} CompressionBuffer;

/*
 * Compress a buffer using specified algorithm
 */
extern int64 columnar_compress_buffer(const char *input, int64 input_size,
                                      char *output, int64 max_output_size,
                                      OrochiCompressionType type, int level);

/*
 * Decompress a buffer
 */
extern int64 columnar_decompress_buffer(const char *input, int64 input_size,
                                        char *output, int64 expected_size,
                                        OrochiCompressionType type);

/*
 * Select optimal compression for column type
 */
extern OrochiCompressionType columnar_select_compression(Oid type_oid,
                                                         Datum *sample_values,
                                                         int sample_count);

/* ============================================================
 * Write Operations
 * ============================================================ */

/*
 * Begin columnar write operation
 */
extern ColumnarWriteState *columnar_begin_write(Relation relation,
                                                ColumnarOptions *options);

/*
 * Write a single row
 */
extern void columnar_write_row(ColumnarWriteState *state,
                               Datum *values, bool *nulls);

/*
 * Write multiple rows (batch)
 */
extern void columnar_write_rows(ColumnarWriteState *state,
                                TupleTableSlot **slots, int count);

/*
 * Flush pending writes to disk
 */
extern void columnar_flush_writes(ColumnarWriteState *state);

/*
 * End write operation and cleanup
 */
extern void columnar_end_write(ColumnarWriteState *state);

/*
 * Truncate and combine stripes (maintenance)
 */
extern void columnar_truncate_and_combine(Relation relation);

/* ============================================================
 * Read Operations
 * ============================================================ */

/*
 * Begin columnar read operation
 */
extern ColumnarReadState *columnar_begin_read(Relation relation,
                                              Snapshot snapshot,
                                              Bitmapset *columns_needed);

/*
 * Read next row
 */
extern bool columnar_read_next_row(ColumnarReadState *state,
                                   TupleTableSlot *slot);

/*
 * Read next vector of rows
 */
extern int columnar_read_next_vector(ColumnarReadState *state,
                                     TupleTableSlot *slot,
                                     int max_rows);

/*
 * Seek to specific row number
 */
extern bool columnar_seek_to_row(ColumnarReadState *state, int64 row_number);

/*
 * End read operation
 */
extern void columnar_end_read(ColumnarReadState *state);

/* ============================================================
 * Skip List Operations
 * ============================================================ */

/*
 * Build skip list for table
 */
extern ColumnarSkipList *columnar_build_skip_list(Relation relation);

/*
 * Check if chunk can be skipped based on predicate
 */
extern bool columnar_can_skip_chunk(ColumnarSkipListEntry *entry,
                                    List *predicates);

/*
 * Get chunks matching predicate
 */
extern List *columnar_get_matching_chunks(ColumnarSkipList *skip_list,
                                          List *predicates);

/* ============================================================
 * Table Access Method Interface
 * ============================================================ */

/*
 * TAM handler function
 */
extern const TableAmRoutine *columnar_tableam_handler(void);

/*
 * Scan functions
 */
extern TableScanDesc columnar_beginscan(Relation relation, Snapshot snapshot,
                                        int nkeys, struct ScanKeyData *keys,
                                        ParallelTableScanDesc parallel_scan,
                                        uint32 flags);
extern void columnar_endscan(TableScanDesc scan);
extern void columnar_rescan(TableScanDesc scan, struct ScanKeyData *key,
                           bool set_params, bool allow_strat, bool allow_sync,
                           bool allow_pagemode);
extern bool columnar_getnextslot(TableScanDesc scan, ScanDirection direction,
                                 TupleTableSlot *slot);

/*
 * Tuple operations
 */
extern void columnar_tuple_insert(Relation relation, TupleTableSlot *slot,
                                  CommandId cid, int options,
                                  struct BulkInsertStateData *bistate);
extern void columnar_tuple_insert_speculative(Relation relation,
                                              TupleTableSlot *slot,
                                              CommandId cid, int options,
                                              struct BulkInsertStateData *bistate,
                                              uint32 specToken);
extern void columnar_tuple_complete_speculative(Relation relation,
                                                TupleTableSlot *slot,
                                                uint32 specToken,
                                                bool succeeded);
extern TM_Result columnar_tuple_delete(Relation relation, ItemPointer tid,
                                       CommandId cid, Snapshot snapshot,
                                       Snapshot crosscheck, bool wait,
                                       TM_FailureData *tmfd, bool changingPart);
extern TM_Result columnar_tuple_update(Relation relation, ItemPointer otid,
                                       TupleTableSlot *slot, CommandId cid,
                                       Snapshot snapshot, Snapshot crosscheck,
                                       bool wait, TM_FailureData *tmfd,
                                       LockTupleMode *lockmode,
                                       TU_UpdateIndexes *update_indexes);
extern TM_Result columnar_tuple_lock(Relation relation, ItemPointer tid,
                                     Snapshot snapshot, TupleTableSlot *slot,
                                     CommandId cid, LockTupleMode mode,
                                     LockWaitPolicy wait_policy, uint8 flags,
                                     TM_FailureData *tmfd);

/*
 * Index support
 */
extern bool columnar_fetch_row_version(Relation relation, ItemPointer tid,
                                       Snapshot snapshot, TupleTableSlot *slot);
extern void columnar_get_latest_tid(TableScanDesc scan, ItemPointer tid);

/*
 * Relation size
 */
extern uint64 columnar_relation_size(Relation rel, ForkNumber forkNumber);
extern bool columnar_relation_needs_toast_table(Relation rel);

/*
 * Planner support
 */
extern void columnar_estimate_rel_size(Relation rel, int32 *attr_widths,
                                       BlockNumber *pages, double *tuples,
                                       double *allvisfrac);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Convert between row numbers and ItemPointers
 */
extern ItemPointerData columnar_row_to_itemptr(int64 row_number);
extern int64 columnar_itemptr_to_row(ItemPointer tid);

/*
 * Get columnar options for table
 */
extern ColumnarOptions *columnar_get_options(Oid relid);

/*
 * Set columnar options for table
 */
extern void columnar_set_options(Oid relid, ColumnarOptions *options);

/*
 * Check if table uses columnar storage
 */
extern bool is_columnar_table(Oid relid);

/*
 * Get compression name from type
 */
extern const char *columnar_compression_name(OrochiCompressionType type);

/*
 * Parse compression name to type
 */
extern OrochiCompressionType columnar_parse_compression(const char *name);

#endif /* OROCHI_COLUMNAR_H */
