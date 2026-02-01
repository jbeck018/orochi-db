/*-------------------------------------------------------------------------
 *
 * json_columnar.h
 *    Orochi DB columnar storage for JSON/JSONB data
 *
 * This module provides:
 *   - Automatic extraction of common paths to columnar format
 *   - Hybrid row/columnar storage for nested data
 *   - Compression optimized for repeated values
 *   - Efficient scanning of extracted columns
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_JSON_COLUMNAR_H
#define OROCHI_JSON_COLUMNAR_H

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "postgres.h"
#include "utils/jsonb.h"

#include "../orochi.h"
#include "json_index.h"

/* ============================================================
 * Configuration Constants
 * ============================================================ */

/* Maximum paths to extract to columns */
#define JSON_COLUMNAR_MAX_EXTRACTED_PATHS 64

/* Minimum row count before considering columnar extraction */
#define JSON_COLUMNAR_MIN_ROWS_FOR_EXTRACTION 1000

/* Default compression threshold (% of repeated values) */
#define JSON_COLUMNAR_COMPRESSION_THRESHOLD 0.5

/* Chunk size for columnar JSON storage */
#define JSON_COLUMNAR_CHUNK_SIZE 10000

/* Maximum depth for path extraction */
#define JSON_COLUMNAR_MAX_DEPTH 8

/* ============================================================
 * Columnar JSON Storage Types
 * ============================================================ */

/*
 * JsonColumnarStorageType - Type of storage for extracted column
 */
typedef enum JsonColumnarStorageType {
    JSON_COLUMNAR_INLINE = 0, /* Small values stored inline */
    JSON_COLUMNAR_DICTIONARY, /* Dictionary encoded */
    JSON_COLUMNAR_DELTA,      /* Delta encoded (for numbers) */
    JSON_COLUMNAR_RLE,        /* Run-length encoded */
    JSON_COLUMNAR_RAW         /* Raw uncompressed */
} JsonColumnarStorageType;

/*
 * JsonColumnState - State of an extracted column
 */
typedef enum JsonColumnState {
    JSON_COLUMN_ACTIVE = 0, /* Column is being used */
    JSON_COLUMN_STALE,      /* Column needs refresh */
    JSON_COLUMN_DEPRECATED  /* Column scheduled for removal */
} JsonColumnState;

/* ============================================================
 * Extracted Column Metadata
 * ============================================================ */

/*
 * JsonExtractedColumn - Metadata for an extracted JSON column
 */
typedef struct JsonExtractedColumn {
    int32 column_id;                      /* Unique column ID */
    Oid table_oid;                        /* Parent table OID */
    int16 source_attnum;                  /* Source JSONB column */
    char *path;                           /* JSON path for extraction */
    Oid target_type;                      /* Target PostgreSQL type */
    JsonColumnarStorageType storage_type; /* Storage type used */
    JsonColumnState state;                /* Column state */

    /* Statistics */
    int64 row_count;          /* Number of rows */
    int64 null_count;         /* Null values */
    int64 distinct_count;     /* Distinct values */
    int64 storage_size;       /* Size in bytes */
    double compression_ratio; /* Compression achieved */

    /* Dictionary info (if dictionary encoded) */
    int32 dict_size;    /* Dictionary entries */
    char **dict_values; /* Dictionary values */

    /* Timestamps */
    TimestampTz created_at;     /* Creation time */
    TimestampTz last_refreshed; /* Last refresh time */
} JsonExtractedColumn;

/*
 * JsonColumnarConfig - Configuration for columnar JSON storage
 */
typedef struct JsonColumnarConfig {
    Oid table_oid;                /* Table OID */
    int16 source_attnum;          /* Source JSONB column */
    int32 max_columns;            /* Max extracted columns */
    int64 min_rows;               /* Min rows for extraction */
    double selectivity_threshold; /* Min selectivity */
    bool auto_extract;            /* Auto-extract paths */
    bool auto_refresh;            /* Auto-refresh columns */
    Interval *refresh_interval;   /* Refresh interval */
} JsonColumnarConfig;

/*
 * JsonColumnarChunk - A chunk of columnar JSON data
 */
typedef struct JsonColumnarChunk {
    int64 chunk_id;  /* Chunk ID */
    int32 column_id; /* Extracted column ID */
    int64 first_row; /* First row in chunk */
    int64 row_count; /* Rows in chunk */

    /* Data storage */
    char *data;              /* Compressed data */
    int64 data_size;         /* Compressed size */
    int64 uncompressed_size; /* Original size */

    /* Null bitmap */
    uint8 *null_bitmap;     /* Null indicator bits */
    int32 null_bitmap_size; /* Bitmap size */

    /* Dictionary reference */
    int32 *dict_indices; /* Dictionary indices (if encoded) */

    /* Statistics */
    Datum min_value; /* Minimum value */
    Datum max_value; /* Maximum value */
    bool has_nulls;  /* Contains nulls */
} JsonColumnarChunk;

/*
 * JsonColumnarWriteState - State for writing columnar JSON
 */
typedef struct JsonColumnarWriteState {
    Oid table_oid;
    int16 source_attnum;
    List *extracted_columns; /* List of JsonExtractedColumn */

    /* Current chunk buffers (one per extracted column) */
    struct ColumnBuffer {
        int32 column_id;
        Datum *values;
        bool *nulls;
        int64 count;
        int64 capacity;
    } *column_buffers;
    int32 num_columns;

    /* Memory context */
    MemoryContext write_context;

    /* Statistics */
    int64 rows_written;
    int64 chunks_created;
} JsonColumnarWriteState;

/*
 * JsonColumnarReadState - State for reading columnar JSON
 */
typedef struct JsonColumnarReadState {
    Oid table_oid;
    int16 source_attnum;
    List *columns_to_read; /* Column IDs to read */

    /* Current position */
    int64 current_row;
    int32 current_chunk_index;
    JsonColumnarChunk *current_chunk;

    /* Decompressed data cache */
    struct DecompressedColumn {
        int32 column_id;
        Datum *values;
        bool *nulls;
        int64 row_count;
        bool is_loaded;
    } *decompressed;
    int32 num_decompressed;

    /* Memory context */
    MemoryContext read_context;

    /* Statistics */
    int64 rows_read;
    int64 chunks_read;
} JsonColumnarReadState;

/* ============================================================
 * Column Extraction Operations
 * ============================================================ */

/*
 * Extract common paths to columns
 */
extern List *json_columnar_extract_columns(Oid table_oid, int16 source_attnum, const char **paths,
                                           int path_count);

/*
 * Auto-detect and extract high-value paths
 */
extern List *json_columnar_auto_extract(Oid table_oid, int16 source_attnum, int max_columns);

/*
 * Drop an extracted column
 */
extern void json_columnar_drop_column(int32 column_id);

/*
 * Refresh an extracted column (update from source)
 */
extern void json_columnar_refresh_column(int32 column_id);

/*
 * Refresh all extracted columns for a table
 */
extern void json_columnar_refresh_all(Oid table_oid, int16 source_attnum);

/*
 * Get extracted columns for a table
 */
extern List *json_columnar_get_columns(Oid table_oid, int16 source_attnum);

/*
 * Get extracted column by ID
 */
extern JsonExtractedColumn *json_columnar_get_column(int32 column_id);

/*
 * Free extracted column metadata
 */
extern void json_columnar_free_column(JsonExtractedColumn *col);

/* ============================================================
 * Columnar Write Operations
 * ============================================================ */

/*
 * Begin columnar write for JSON extraction
 */
extern JsonColumnarWriteState *json_columnar_begin_write(Oid table_oid, int16 source_attnum);

/*
 * Write a JSON value to extracted columns
 */
extern void json_columnar_write_value(JsonColumnarWriteState *state, Jsonb *value, int64 row_id);

/*
 * Flush pending writes
 */
extern void json_columnar_flush_writes(JsonColumnarWriteState *state);

/*
 * End columnar write
 */
extern void json_columnar_end_write(JsonColumnarWriteState *state);

/* ============================================================
 * Columnar Read Operations
 * ============================================================ */

/*
 * Begin columnar read for JSON
 */
extern JsonColumnarReadState *json_columnar_begin_read(Oid table_oid, int16 source_attnum,
                                                       List *column_ids);

/*
 * Read value from extracted column
 */
extern bool json_columnar_read_value(JsonColumnarReadState *state, int32 column_id, int64 row_id,
                                     Datum *value, bool *isnull);

/*
 * Read batch of values
 */
extern int json_columnar_read_batch(JsonColumnarReadState *state, int32 column_id, int64 start_row,
                                    int max_rows, Datum *values, bool *nulls);

/*
 * End columnar read
 */
extern void json_columnar_end_read(JsonColumnarReadState *state);

/* ============================================================
 * Compression Operations
 * ============================================================ */

/*
 * Determine optimal storage type for a column
 */
extern JsonColumnarStorageType json_columnar_select_storage(JsonExtractedColumn *col,
                                                            Datum *sample_values, int sample_count);

/*
 * Compress column data
 */
extern char *json_columnar_compress(Datum *values, bool *nulls, int count,
                                    JsonColumnarStorageType storage_type, int64 *compressed_size);

/*
 * Decompress column data
 */
extern void json_columnar_decompress(const char *compressed, int64 compressed_size,
                                     JsonColumnarStorageType storage_type, int count, Datum *values,
                                     bool *nulls);

/*
 * Build dictionary for column values
 */
extern void json_columnar_build_dictionary(Datum *values, int count, char ***dict_values,
                                           int *dict_size, int32 **dict_indices);

/* ============================================================
 * Hybrid Row/Columnar Operations
 * ============================================================ */

/*
 * Configuration for hybrid storage
 */
typedef struct JsonHybridConfig {
    Oid table_oid;
    int16 source_attnum;
    List *columnar_paths;  /* Paths stored columnar */
    bool keep_original;    /* Keep original JSONB */
    double size_threshold; /* Max size for inline */
} JsonHybridConfig;

/*
 * Enable hybrid storage for a table
 */
extern void json_columnar_enable_hybrid(Oid table_oid, int16 source_attnum,
                                        const char **columnar_paths, int path_count);

/*
 * Disable hybrid storage
 */
extern void json_columnar_disable_hybrid(Oid table_oid, int16 source_attnum);

/*
 * Check if hybrid storage is enabled
 */
extern bool json_columnar_is_hybrid(Oid table_oid, int16 source_attnum);

/*
 * Get hybrid configuration
 */
extern JsonHybridConfig *json_columnar_get_hybrid_config(Oid table_oid, int16 source_attnum);

/* ============================================================
 * Statistics and Maintenance
 * ============================================================ */

/*
 * Get storage statistics for extracted columns
 */
typedef struct JsonColumnarStats {
    Oid table_oid;
    int16 source_attnum;
    int32 num_columns;
    int64 total_rows;
    int64 columnar_size;    /* Size of columnar data */
    int64 original_size;    /* Size of original JSONB */
    double space_savings;   /* % space saved */
    double avg_compression; /* Average compression ratio */
} JsonColumnarStats;

extern JsonColumnarStats *json_columnar_get_stats(Oid table_oid, int16 source_attnum);

/*
 * Analyze extracted columns and recommend changes
 */
extern List *json_columnar_analyze(Oid table_oid, int16 source_attnum);

/*
 * Compact columnar storage (merge small chunks)
 */
extern void json_columnar_compact(Oid table_oid, int16 source_attnum);

/*
 * Vacuum columnar storage (remove stale data)
 */
extern void json_columnar_vacuum(Oid table_oid, int16 source_attnum);

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

/* Extract columns from JSON */
extern Datum orochi_extract_columns(PG_FUNCTION_ARGS);

/* Get extracted column info */
extern Datum orochi_json_columnar_info(PG_FUNCTION_ARGS);

/* Refresh extracted columns */
extern Datum orochi_json_columnar_refresh(PG_FUNCTION_ARGS);

/* Get columnar statistics */
extern Datum orochi_json_columnar_stats(PG_FUNCTION_ARGS);

/* Enable hybrid storage */
extern Datum orochi_json_enable_hybrid(PG_FUNCTION_ARGS);

#endif /* OROCHI_JSON_COLUMNAR_H */
