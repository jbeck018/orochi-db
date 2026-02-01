/*-------------------------------------------------------------------------
 *
 * ddl_stream.h
 *    Orochi DB Stream DDL definitions
 *
 * This module provides change tracking streams similar to Snowflake Streams:
 *   - CREATE STREAM for incremental change tracking
 *   - Automatic offset management
 *   - Support for INSERT, UPDATE, DELETE tracking
 *
 * Example:
 *   CREATE STREAM my_stream ON TABLE source_table;
 *   -- Tracks changes for incremental processing
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#ifndef OROCHI_DDL_STREAM_H
#define OROCHI_DDL_STREAM_H

#include "access/xlogdefs.h"
#include "fmgr.h"
#include "postgres.h"
#include "utils/timestamp.h"

/* ============================================================
 * Stream Types
 * ============================================================ */

typedef enum StreamType {
  STREAM_TYPE_STANDARD = 0,    /* Tracks all DML changes */
  STREAM_TYPE_APPEND_ONLY = 1, /* Only tracks INSERTs */
  STREAM_TYPE_INSERT_ONLY = 2  /* Alias for append-only */
} StreamType;

/* ============================================================
 * Stream Modes
 * ============================================================ */

typedef enum StreamMode {
  STREAM_MODE_DEFAULT = 0,    /* Standard CDC behavior */
  STREAM_MODE_KEYS_ONLY = 1,  /* Only capture key columns */
  STREAM_MODE_FULL_BEFORE = 2 /* Full before/after images */
} StreamMode;

/* ============================================================
 * Stream States
 * ============================================================ */

typedef enum StreamState {
  STREAM_STATE_ACTIVE = 0,  /* Stream is active */
  STREAM_STATE_STALE = 1,   /* Stream data may be incomplete */
  STREAM_STATE_PAUSED = 2,  /* Stream is paused */
  STREAM_STATE_DISABLED = 3 /* Stream is disabled */
} StreamState;

/* ============================================================
 * Stream Metadata Action Types (for reading stream)
 * ============================================================ */

typedef enum StreamAction {
  STREAM_ACTION_INSERT = 0, /* Row was inserted */
  STREAM_ACTION_UPDATE = 1, /* Row was updated */
  STREAM_ACTION_DELETE = 2  /* Row was deleted */
} StreamAction;

/* ============================================================
 * Stream Offset Types
 * ============================================================ */

typedef struct StreamOffset {
  XLogRecPtr lsn;        /* WAL LSN position */
  TransactionId xid;     /* Transaction ID */
  TimestampTz timestamp; /* Offset timestamp */
  int64 sequence;        /* Sequence number within xid */
} StreamOffset;

/* ============================================================
 * Stream Column Definition
 * ============================================================ */

typedef struct StreamColumn {
  char *column_name;  /* Column name */
  Oid column_type;    /* PostgreSQL type OID */
  bool is_key;        /* Part of primary/unique key */
  bool track_changes; /* Track changes for this column */
} StreamColumn;

/* ============================================================
 * Stream Change Record
 * ============================================================ */

typedef struct StreamChangeRecord {
  int64 change_id;         /* Unique change identifier */
  StreamAction action;     /* INSERT/UPDATE/DELETE */
  TransactionId xid;       /* Transaction ID */
  TimestampTz change_time; /* When change occurred */

  /* Row data */
  int num_columns;   /* Number of columns */
  Datum *old_values; /* Before image (UPDATE/DELETE) */
  bool *old_nulls;   /* Null flags for before */
  Datum *new_values; /* After image (INSERT/UPDATE) */
  bool *new_nulls;   /* Null flags for after */

  /* Metadata */
  bool is_update; /* True if update metadata row */
  int64 row_id;   /* Internal row identifier */
} StreamChangeRecord;

/* ============================================================
 * Main Stream Structure
 * ============================================================ */

typedef struct Stream {
  int64 stream_id;     /* Unique identifier */
  char *stream_name;   /* Stream name */
  char *stream_schema; /* Schema containing stream */

  /* Source table */
  Oid source_table_oid; /* Source table OID */
  char *source_schema;  /* Source table schema */
  char *source_table;   /* Source table name */

  /* Stream configuration */
  StreamType stream_type; /* Type of stream */
  StreamMode stream_mode; /* Capture mode */
  bool show_initial_rows; /* Include existing rows */

  /* Column tracking */
  int num_columns;       /* Number of tracked columns */
  StreamColumn *columns; /* Column definitions */

  /* Offset tracking */
  StreamOffset initial_offset; /* Initial offset at creation */
  StreamOffset current_offset; /* Current consumer offset */
  StreamOffset latest_offset;  /* Latest available offset */

  /* State */
  StreamState state;     /* Current state */
  bool has_pending_data; /* Has unconsumed changes */

  /* Staleness tracking */
  Interval *data_retention; /* Source table retention */
  TimestampTz stale_after;  /* When stream becomes stale */

  /* Timestamps */
  TimestampTz created_at;       /* Creation time */
  TimestampTz last_consumed_at; /* Last consumption time */

  /* Statistics */
  int64 total_changes;   /* Total changes captured */
  int64 pending_changes; /* Pending changes */
  int64 bytes_captured;  /* Total bytes captured */

  /* Memory context */
  MemoryContext stream_context; /* Memory context */
} Stream;

/* ============================================================
 * Function Prototypes - Stream Management
 * ============================================================ */

/* Stream lifecycle */
extern int64 orochi_create_stream(const char *name, Oid source_table,
                                  StreamType type, StreamMode mode,
                                  bool show_initial_rows);
extern bool orochi_drop_stream(int64 stream_id, bool if_exists);
extern bool orochi_alter_stream(int64 stream_id, const char *alterations);

/* Stream operations */
extern Stream *orochi_get_stream(int64 stream_id);
extern Stream *orochi_get_stream_by_name(const char *schema, const char *name);
extern List *orochi_list_streams(void);
extern List *orochi_list_streams_on_table(Oid table_oid);

/* Stream consumption */
extern List *orochi_stream_get_changes(int64 stream_id, int limit);
extern bool orochi_stream_advance_offset(int64 stream_id, StreamOffset *offset);
extern bool orochi_stream_reset_offset(int64 stream_id);
extern StreamOffset *orochi_stream_get_offset(int64 stream_id);

/* Stream state management */
extern bool orochi_stream_pause(int64 stream_id);
extern bool orochi_stream_resume(int64 stream_id);
extern bool orochi_stream_refresh(int64 stream_id);
extern StreamState orochi_stream_get_state(int64 stream_id);

/* Check stream data availability */
extern bool orochi_stream_has_data(int64 stream_id);
extern int64 orochi_stream_pending_count(int64 stream_id);
extern bool orochi_stream_is_stale(int64 stream_id);

/* ============================================================
 * Function Prototypes - DDL Parsing
 * ============================================================ */

/* Parse CREATE STREAM statement */
extern Stream *ddl_parse_create_stream(const char *sql);

/* Parse stream options */
extern void ddl_parse_stream_options(Stream *stream, const char *options);

/* Validate stream definition */
extern bool ddl_validate_stream(Stream *stream, char **error_msg);

/* ============================================================
 * Function Prototypes - Catalog Operations
 * ============================================================ */

/* Store stream in catalog */
extern void ddl_catalog_store_stream(Stream *stream);
extern void ddl_catalog_update_stream(Stream *stream);
extern void ddl_catalog_delete_stream(int64 stream_id);

/* Load stream from catalog */
extern Stream *ddl_catalog_load_stream(int64 stream_id);
extern Stream *ddl_catalog_load_stream_by_name(const char *schema,
                                               const char *name);

/* Update stream offset */
extern void ddl_catalog_update_stream_offset(int64 stream_id,
                                             StreamOffset *offset);

/* ============================================================
 * Function Prototypes - Change Capture
 * ============================================================ */

/* Initialize change capture for a stream */
extern void stream_capture_init(Stream *stream);

/* Capture changes from WAL */
extern void stream_capture_wal_changes(Stream *stream, XLogRecPtr start,
                                       XLogRecPtr end);

/* Capture changes from trigger */
extern void stream_capture_trigger_change(Stream *stream, Oid table_oid,
                                          StreamAction action,
                                          HeapTuple old_tuple,
                                          HeapTuple new_tuple);

/* Materialize stream view */
extern void stream_materialize_view(Stream *stream);

/* ============================================================
 * SQL Function Declarations
 * ============================================================ */

extern Datum orochi_create_stream_sql(PG_FUNCTION_ARGS);
extern Datum orochi_drop_stream_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stream_status_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stream_get_changes_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stream_advance_sql(PG_FUNCTION_ARGS);
extern Datum orochi_stream_has_data_sql(PG_FUNCTION_ARGS);
extern Datum orochi_list_streams_sql(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

extern const char *stream_type_name(StreamType type);
extern StreamType stream_parse_type(const char *name);
extern const char *stream_mode_name(StreamMode mode);
extern StreamMode stream_parse_mode(const char *name);
extern const char *stream_state_name(StreamState state);
extern StreamState stream_parse_state(const char *name);
extern const char *stream_action_name(StreamAction action);

/* Stream offset comparison */
extern int stream_offset_compare(StreamOffset *a, StreamOffset *b);
extern bool stream_offset_is_valid(StreamOffset *offset);
extern StreamOffset *stream_offset_copy(StreamOffset *offset);

/* Free stream structures */
extern void stream_free(Stream *stream);
extern void stream_change_record_free(StreamChangeRecord *record);

/* ============================================================
 * Constants
 * ============================================================ */

#define STREAM_MAX_NAME_LENGTH 128
#define STREAM_DEFAULT_BATCH_SIZE 10000
#define STREAM_MAX_PENDING_CHANGES 1000000
#define STREAM_DEFAULT_STALENESS_DAYS 14

/* Stream metadata columns */
#define STREAM_METADATA_ACTION "METADATA$ACTION"
#define STREAM_METADATA_ISUPDATE "METADATA$ISUPDATE"
#define STREAM_METADATA_ROW_ID "METADATA$ROW_ID"

/* Catalog table names */
#define OROCHI_STREAMS_TABLE "orochi_streams"
#define OROCHI_STREAM_COLUMNS_TABLE "orochi_stream_columns"
#define OROCHI_STREAM_OFFSETS_TABLE "orochi_stream_offsets"

#endif /* OROCHI_DDL_STREAM_H */
