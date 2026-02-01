/*-------------------------------------------------------------------------
 *
 * columnar.c
 *    Orochi DB columnar storage engine implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/primnodes.h"
#include "postgres.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 180000
#include "common/hashfn.h"
/* PG18 removed some operator OID macros - define them using known OIDs */
#ifndef Int8EqualOperator
#define Int8EqualOperator 410
#endif
#ifndef Float8EqualOperator
#define Float8EqualOperator 670
#endif
#ifndef Int4GreaterOperator
#define Int4GreaterOperator 521
#endif
#ifndef Int8GreaterOperator
#define Int8GreaterOperator 413
#endif
#else
#include "utils/hashutils.h"
#endif
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "../core/catalog.h"
#include "../orochi.h"
#include "columnar.h"
#include "compression.h"

/* Forward declarations */
static void serialize_chunk_group(ColumnarWriteState *state);
static void flush_stripe(ColumnarWriteState *state);
static void load_chunk_group(ColumnarReadState *state, int chunk_group_index);
static bool apply_skip_list_filter(ColumnarReadState *state,
                                   ColumnarChunkGroup *chunk_group);

/* ============================================================
 * Compression Implementation
 * ============================================================ */

int64 columnar_compress_buffer(const char *input, int64 input_size,
                               char *output, int64 max_output_size,
                               OrochiCompressionType type, int level) {
  int64 compressed_size = 0;

  switch (type) {
  case OROCHI_COMPRESS_NONE:
    if (max_output_size < input_size)
      elog(ERROR, "Output buffer too small for uncompressed data");
    memcpy(output, input, input_size);
    compressed_size = input_size;
    break;

  case OROCHI_COMPRESS_LZ4:
    /* LZ4 compression */
    compressed_size = compress_lz4(input, input_size, output, max_output_size);
    break;

  case OROCHI_COMPRESS_ZSTD:
    /* ZSTD compression */
    compressed_size =
        compress_zstd(input, input_size, output, max_output_size, level);
    break;

  case OROCHI_COMPRESS_PGLZ:
    /* PostgreSQL native compression */
    compressed_size = compress_pglz(input, input_size, output, max_output_size);
    break;

  case OROCHI_COMPRESS_DELTA:
    /* Delta encoding for integers */
    compressed_size =
        compress_delta(input, input_size, output, max_output_size);
    break;

  case OROCHI_COMPRESS_GORILLA:
    /* Gorilla compression for floats */
    compressed_size =
        compress_gorilla(input, input_size, output, max_output_size);
    break;

  case OROCHI_COMPRESS_DICTIONARY:
    /* Dictionary encoding */
    compressed_size =
        compress_dictionary(input, input_size, output, max_output_size);
    break;

  case OROCHI_COMPRESS_RLE:
    /* Run-length encoding */
    compressed_size = compress_rle(input, input_size, output, max_output_size);
    break;

  default:
    elog(ERROR, "Unknown compression type: %d", type);
  }

  return compressed_size;
}

int64 columnar_decompress_buffer(const char *input, int64 input_size,
                                 char *output, int64 expected_size,
                                 OrochiCompressionType type) {
  int64 decompressed_size = 0;

  switch (type) {
  case OROCHI_COMPRESS_NONE:
    memcpy(output, input, input_size);
    decompressed_size = input_size;
    break;

  case OROCHI_COMPRESS_LZ4:
    decompressed_size =
        decompress_lz4(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_ZSTD:
    decompressed_size =
        decompress_zstd(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_PGLZ:
    decompressed_size =
        decompress_pglz(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_DELTA:
    decompressed_size =
        decompress_delta(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_GORILLA:
    decompressed_size =
        decompress_gorilla(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_DICTIONARY:
    decompressed_size =
        decompress_dictionary(input, input_size, output, expected_size);
    break;

  case OROCHI_COMPRESS_RLE:
    decompressed_size =
        decompress_rle(input, input_size, output, expected_size);
    break;

  default:
    elog(ERROR, "Unknown compression type: %d", type);
  }

  if (decompressed_size != expected_size)
    elog(WARNING, "Decompressed size mismatch: got %ld, expected %ld",
         decompressed_size, expected_size);

  return decompressed_size;
}

OrochiCompressionType columnar_select_compression(Oid type_oid,
                                                  Datum *sample_values,
                                                  int sample_count) {
  /*
   * Select optimal compression algorithm based on data type:
   * - Integers with steady delta: Delta encoding
   * - Floats: Gorilla compression
   * - Low-cardinality strings: Dictionary
   * - Boolean: RLE
   * - Default: ZSTD
   */
  switch (type_oid) {
  case INT2OID:
  case INT4OID:
  case INT8OID:
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
  case DATEOID:
    return OROCHI_COMPRESS_DELTA;

  case FLOAT4OID:
  case FLOAT8OID:
    return OROCHI_COMPRESS_GORILLA;

  case BOOLOID:
    return OROCHI_COMPRESS_RLE;

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
    /*
     * Analyze cardinality for dictionary encoding.
     * If the ratio of unique values to total values is below a threshold,
     * dictionary encoding is more efficient.
     */
    if (sample_values != NULL && sample_count > 0) {
      HTAB *seen_values;
      HASHCTL hash_ctl;
      int unique_count = 0;
      int i;
      double cardinality_ratio;

      /* Create a hash table to count unique values */
      memset(&hash_ctl, 0, sizeof(hash_ctl));
      hash_ctl.keysize = sizeof(uint32);
      hash_ctl.entrysize = sizeof(uint32);
      hash_ctl.hcxt = CurrentMemoryContext;

      seen_values = hash_create("CardinalityCheck", sample_count, &hash_ctl,
                                HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

      for (i = 0; i < sample_count; i++) {
        uint32 hash_val;
        bool found;

        /* Compute hash of the text value */
        hash_val = DatumGetUInt32(
            hash_any((unsigned char *)DatumGetPointer(sample_values[i]),
                     VARSIZE_ANY_EXHDR(DatumGetPointer(sample_values[i]))));

        hash_search(seen_values, &hash_val, HASH_ENTER, &found);
        if (!found)
          unique_count++;
      }

      hash_destroy(seen_values);

      /* Calculate cardinality ratio */
      cardinality_ratio = (double)unique_count / (double)sample_count;

      /*
       * Use dictionary encoding if cardinality is low (< 10% unique values)
       * or if there are very few unique values (< 256 for byte-sized index)
       */
      if (cardinality_ratio < 0.1 || unique_count < 256)
        return OROCHI_COMPRESS_DICTIONARY;
    }
    return OROCHI_COMPRESS_ZSTD;

  default:
    return OROCHI_COMPRESS_ZSTD;
  }
}

/* ============================================================
 * Write Operations
 * ============================================================ */

ColumnarWriteState *columnar_begin_write(Relation relation,
                                         ColumnarOptions *options) {
  ColumnarWriteState *state;
  TupleDesc tupdesc = RelationGetDescr(relation);
  int i;
  MemoryContext old_context;

  /* Create write state in dedicated memory context */
  state = (ColumnarWriteState *)palloc0(sizeof(ColumnarWriteState));

  state->write_context = AllocSetContextCreate(
      CurrentMemoryContext, "ColumnarWriteContext", ALLOCSET_DEFAULT_SIZES);
  state->stripe_context = AllocSetContextCreate(
      state->write_context, "ColumnarStripeContext", ALLOCSET_DEFAULT_SIZES);

  old_context = MemoryContextSwitchTo(state->write_context);

  state->relation = relation;
  state->tupdesc = CreateTupleDescCopy(tupdesc);

  /* Set options */
  if (options)
    memcpy(&state->options, options, sizeof(ColumnarOptions));
  else {
    /* Default options */
    state->options.stripe_row_limit = orochi_stripe_row_limit;
    state->options.chunk_group_row_limit =
        COLUMNAR_DEFAULT_CHUNK_GROUP_ROW_LIMIT;
    state->options.compression_type = OROCHI_COMPRESS_ZSTD;
    state->options.compression_level = orochi_compression_level;
    state->options.enable_vectorization = orochi_enable_vectorized_execution;
  }

  /* Allocate per-column write buffers */
  state->column_buffers =
      palloc0(tupdesc->natts * sizeof(*state->column_buffers));

  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    if (attr->attisdropped)
      continue;

    state->column_buffers[i].capacity = state->options.chunk_group_row_limit;
    state->column_buffers[i].values =
        palloc(sizeof(Datum) * state->column_buffers[i].capacity);
    state->column_buffers[i].nulls =
        palloc(sizeof(bool) * state->column_buffers[i].capacity);
    state->column_buffers[i].count = 0;
    state->column_buffers[i].has_nulls = false;
  }

  /* Initialize stripe */
  state->current_stripe = palloc0(sizeof(ColumnarStripe));
  state->current_stripe->column_count = tupdesc->natts;
  state->stripe_row_count = 0;

  /* Initialize chunk group */
  state->current_chunk_group = palloc0(sizeof(ColumnarChunkGroup));
  state->current_chunk_group->column_count = tupdesc->natts;
  state->chunk_group_row_count = 0;

  MemoryContextSwitchTo(old_context);

  return state;
}

void columnar_write_row(ColumnarWriteState *state, Datum *values, bool *nulls) {
  TupleDesc tupdesc = state->tupdesc;
  int i;

  /* Buffer values for each column */
  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
    struct ColumnWriteBuffer *buf = &state->column_buffers[i];

    if (attr->attisdropped)
      continue;

    if (buf->count >= buf->capacity) {
      /* Expand buffer */
      buf->capacity *= 2;
      buf->values = repalloc(buf->values, sizeof(Datum) * buf->capacity);
      buf->nulls = repalloc(buf->nulls, sizeof(bool) * buf->capacity);
    }

    buf->nulls[buf->count] = nulls[i];

    if (!nulls[i]) {
      /* Copy datum (deep copy for pass-by-reference types) */
      if (attr->attbyval)
        buf->values[buf->count] = values[i];
      else {
        Size datum_size = datumGetSize(values[i], attr->attbyval, attr->attlen);
        buf->values[buf->count] =
            datumCopy(values[i], attr->attbyval, attr->attlen);
      }

      /* Update min/max statistics */
      if (buf->count == 0) {
        /* First non-null value: initialize min/max */
        buf->min_value = buf->values[buf->count];
        buf->max_value = buf->values[buf->count];
      } else {
        /*
         * Compare current value with min/max and update.
         * Use the appropriate comparison based on type.
         */
        Oid cmp_proc;
        TypeCacheEntry *typentry;
        Datum curr_value = buf->values[buf->count];
        int cmp_result;

        typentry = lookup_type_cache(
            attr->atttypid, TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO);

        if (OidIsValid(typentry->cmp_proc)) {
          /* Compare with min value */
          cmp_result = DatumGetInt32(
              FunctionCall2Coll(&typentry->cmp_proc_finfo, attr->attcollation,
                                curr_value, buf->min_value));
          if (cmp_result < 0)
            buf->min_value = curr_value;

          /* Compare with max value */
          cmp_result = DatumGetInt32(
              FunctionCall2Coll(&typentry->cmp_proc_finfo, attr->attcollation,
                                curr_value, buf->max_value));
          if (cmp_result > 0)
            buf->max_value = curr_value;
        }
      }
    } else {
      buf->has_nulls = true;
    }

    buf->count++;
  }

  state->chunk_group_row_count++;
  state->stripe_row_count++;
  state->total_rows_written++;

  /* Check if chunk group is full */
  if (state->chunk_group_row_count >= state->options.chunk_group_row_limit) {
    serialize_chunk_group(state);
  }

  /* Check if stripe is full */
  if (state->stripe_row_count >= state->options.stripe_row_limit) {
    flush_stripe(state);
  }
}

void columnar_write_rows(ColumnarWriteState *state, TupleTableSlot **slots,
                         int count) {
  int i;

  for (i = 0; i < count; i++) {
    TupleTableSlot *slot = slots[i];
    bool should_free;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, false, &should_free);
    Datum *values;
    bool *nulls;
    int natts = state->tupdesc->natts;

    values = palloc(sizeof(Datum) * natts);
    nulls = palloc(sizeof(bool) * natts);

    heap_deform_tuple(tuple, state->tupdesc, values, nulls);
    columnar_write_row(state, values, nulls);

    pfree(values);
    pfree(nulls);

    if (should_free)
      heap_freetuple(tuple);
  }
}

static void serialize_chunk_group(ColumnarWriteState *state) {
  TupleDesc tupdesc = state->tupdesc;
  ColumnarChunkGroup *cg = state->current_chunk_group;
  int i;
  MemoryContext old_context;

  old_context = MemoryContextSwitchTo(state->stripe_context);

  /* Allocate column chunks */
  cg->columns = palloc0(tupdesc->natts * sizeof(ColumnarColumnChunk *));
  cg->row_count = state->chunk_group_row_count;

  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
    struct ColumnWriteBuffer *buf = &state->column_buffers[i];
    ColumnarColumnChunk *chunk;
    OrochiCompressionType compression;
    int64 value_buffer_size;
    char *raw_buffer;
    char *compressed_buffer;
    int64 compressed_size;

    if (attr->attisdropped || buf->count == 0)
      continue;

    chunk = palloc0(sizeof(ColumnarColumnChunk));
    chunk->column_index = i;
    chunk->column_type = attr->atttypid;
    chunk->value_count = buf->count;
    chunk->has_nulls = buf->has_nulls;

    /* Count nulls */
    chunk->null_count = 0;
    for (int j = 0; j < buf->count; j++) {
      if (buf->nulls[j])
        chunk->null_count++;
    }

    /* Select compression based on type */
    compression =
        columnar_select_compression(attr->atttypid, buf->values, buf->count);
    chunk->compression_type = compression;

    /* Serialize values to buffer */
    if (attr->attbyval && attr->attlen > 0) {
      value_buffer_size = buf->count * attr->attlen;
      raw_buffer = palloc(value_buffer_size);

      for (int j = 0; j < buf->count; j++) {
        if (!buf->nulls[j])
          memcpy(raw_buffer + (j * attr->attlen), &buf->values[j],
                 attr->attlen);
        else
          memset(raw_buffer + (j * attr->attlen), 0, attr->attlen);
      }
    } else {
      /* Variable-length types - serialize with length prefix */
      StringInfoData si;
      initStringInfo(&si);

      for (int j = 0; j < buf->count; j++) {
        if (!buf->nulls[j]) {
          Size datum_size =
              datumGetSize(buf->values[j], attr->attbyval, attr->attlen);
          appendBinaryStringInfo(&si, (char *)&datum_size, sizeof(Size));
          appendBinaryStringInfo(&si, DatumGetPointer(buf->values[j]),
                                 datum_size);
        } else {
          Size zero = 0;
          appendBinaryStringInfo(&si, (char *)&zero, sizeof(Size));
        }
      }

      raw_buffer = si.data;
      value_buffer_size = si.len;
    }

    /* Compress the buffer */
    chunk->decompressed_size = value_buffer_size;
    compressed_buffer = palloc(value_buffer_size); /* Worst case */

    compressed_size = columnar_compress_buffer(
        raw_buffer, value_buffer_size, compressed_buffer, value_buffer_size,
        compression, state->options.compression_level);

    if (compressed_size < value_buffer_size) {
      chunk->compressed_size = compressed_size;
      chunk->value_buffer = compressed_buffer;
      pfree(raw_buffer);
    } else {
      /* Compression didn't help, store uncompressed */
      chunk->compression_type = OROCHI_COMPRESS_NONE;
      chunk->compressed_size = value_buffer_size;
      chunk->value_buffer = raw_buffer;
      pfree(compressed_buffer);
    }

    /* Store null bitmap */
    if (chunk->has_nulls) {
      int null_bytes = (buf->count + 7) / 8;
      chunk->null_buffer = palloc0(null_bytes);

      for (int j = 0; j < buf->count; j++) {
        if (buf->nulls[j])
          chunk->null_buffer[j / 8] |= (1 << (j % 8));
      }
    }

    /* Store min/max */
    chunk->has_min_max = !buf->has_nulls || buf->count > chunk->null_count;
    if (chunk->has_min_max) {
      chunk->min_value = buf->min_value;
      chunk->max_value = buf->max_value;
    }

    cg->columns[i] = chunk;

    /*
     * Also store compression info in ColumnWriteBuffer for flush_stripe
     * to access when writing to catalog storage.
     */
    buf->attr_type = attr->atttypid;
    buf->row_count = chunk->value_count;
    buf->null_count = chunk->null_count;
    buf->compressed_data = chunk->value_buffer;
    buf->compressed_size = chunk->compressed_size;
    buf->uncompressed_size = chunk->decompressed_size;
    buf->compression_type = chunk->compression_type;

    /* Reset count for next chunk group (but keep compression data for flush) */
    buf->count = 0;
    buf->has_nulls = false;
  }

  /* Increment chunk group index */
  state->current_stripe->chunk_group_count++;
  state->chunk_group_row_count = 0;

  MemoryContextSwitchTo(old_context);
}

static void flush_stripe(ColumnarWriteState *state) {
  ColumnarStripe *stripe = state->current_stripe;
  int64 stripe_id;
  MemoryContext old_context;

  /* Serialize any remaining data */
  if (state->chunk_group_row_count > 0)
    serialize_chunk_group(state);

  /* Get next stripe ID from catalog */
  stripe_id = orochi_catalog_create_stripe(
      RelationGetRelid(state->relation),
      state->current_stripe->first_row_number, state->stripe_row_count,
      state->current_stripe->column_count);
  stripe->stripe_id = stripe_id;
  stripe->row_count = state->stripe_row_count;

  /*
   * Write stripe data to storage.
   * Store each column's compressed data separately in the catalog.
   */
  {
    int cg_idx;
    int64 total_data_size = 0;

    /* Store data for each chunk group */
    for (cg_idx = 0; cg_idx < stripe->chunk_group_count; cg_idx++) {
      int col_idx;

      /* Store each column separately */
      for (col_idx = 0; col_idx < state->tupdesc->natts; col_idx++) {
        struct ColumnWriteBuffer *col_buf = &state->column_buffers[col_idx];
        OrochiColumnChunk chunk_meta;

        /* Build chunk metadata */
        memset(&chunk_meta, 0, sizeof(OrochiColumnChunk));
        chunk_meta.stripe_id = stripe_id;
        chunk_meta.chunk_group_index = cg_idx;
        chunk_meta.column_index = col_idx;
        chunk_meta.data_type = col_buf->attr_type;
        chunk_meta.value_count = col_buf->row_count;
        chunk_meta.null_count = col_buf->null_count;
        chunk_meta.has_nulls = (col_buf->null_count > 0);
        chunk_meta.compressed_size = col_buf->compressed_size;
        chunk_meta.decompressed_size = col_buf->uncompressed_size;
        chunk_meta.compression = col_buf->compression_type;
        chunk_meta.min_value = col_buf->min_value;
        chunk_meta.max_value = col_buf->max_value;

        /* Store chunk with compressed data */
        orochi_catalog_create_column_chunk_with_data(
            stripe_id, cg_idx, col_idx, &chunk_meta, col_buf->compressed_data,
            col_buf->compressed_size);

        /* Update min/max statistics for skip list */
        orochi_catalog_update_column_stats(
            stripe_id, col_idx, col_buf->min_value, col_buf->max_value,
            col_buf->attr_type);

        total_data_size += col_buf->compressed_size;
      }
    }

    stripe->data_size = total_data_size;

    elog(DEBUG1, "Wrote stripe %ld: %d chunk groups, %ld bytes", stripe_id,
         stripe->chunk_group_count, total_data_size);
  }

  /* Update catalog with final sizes */
  orochi_catalog_update_stripe_status(stripe_id, true, stripe->data_size,
                                      stripe->metadata_size);

  /* Reset for next stripe */
  old_context = MemoryContextSwitchTo(state->write_context);
  MemoryContextReset(state->stripe_context);

  state->current_stripe = palloc0(sizeof(ColumnarStripe));
  state->current_stripe->first_row_number = state->total_rows_written;
  state->current_stripe->column_count = state->tupdesc->natts;

  state->current_chunk_group = palloc0(sizeof(ColumnarChunkGroup));
  state->current_chunk_group->column_count = state->tupdesc->natts;

  state->stripe_row_count = 0;
  state->stripes_created++;

  MemoryContextSwitchTo(old_context);
}

void columnar_flush_writes(ColumnarWriteState *state) {
  if (state->stripe_row_count > 0)
    flush_stripe(state);
}

void columnar_end_write(ColumnarWriteState *state) {
  /* Flush any remaining data */
  columnar_flush_writes(state);

  /* Free memory contexts */
  MemoryContextDelete(state->write_context);
}

/* ============================================================
 * Read Operations
 * ============================================================ */

ColumnarReadState *columnar_begin_read(Relation relation, Snapshot snapshot,
                                       Bitmapset *columns_needed) {
  ColumnarReadState *state;
  TupleDesc tupdesc = RelationGetDescr(relation);
  int i;
  MemoryContext old_context;

  state = (ColumnarReadState *)palloc0(sizeof(ColumnarReadState));

  state->read_context = AllocSetContextCreate(
      CurrentMemoryContext, "ColumnarReadContext", ALLOCSET_DEFAULT_SIZES);
  state->stripe_context = AllocSetContextCreate(
      state->read_context, "ColumnarStripeReadContext", ALLOCSET_DEFAULT_SIZES);

  old_context = MemoryContextSwitchTo(state->read_context);

  state->relation = relation;
  state->tupdesc = CreateTupleDescCopy(tupdesc);
  state->snapshot = snapshot;
  state->columns_needed = bms_copy(columns_needed);

  /* If no columns specified, need all */
  if (bms_is_empty(state->columns_needed)) {
    for (i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (!attr->attisdropped)
        state->columns_needed = bms_add_member(state->columns_needed, i);
    }
  }

  /* Allocate column read buffers */
  state->column_buffers =
      palloc0(tupdesc->natts * sizeof(*state->column_buffers));

  /* Load stripe list from catalog */
  state->stripe_list =
      orochi_catalog_get_table_stripes(RelationGetRelid(relation));
  state->current_stripe_index = -1;

  /* Load skip list for predicate pushdown */
  state->skip_list = columnar_build_skip_list(relation);

  /* Vectorization settings */
  state->enable_vectorization = orochi_enable_vectorized_execution;
  state->vector_batch_size = COLUMNAR_VECTOR_BATCH_SIZE;

  MemoryContextSwitchTo(old_context);

  return state;
}

static void load_chunk_group(ColumnarReadState *state, int chunk_group_index) {
  ColumnarStripe *stripe = state->current_stripe;
  TupleDesc tupdesc = state->tupdesc;
  MemoryContext old_context;
  List *column_chunks;
  ListCell *lc;
  int col_idx;

  if (stripe == NULL)
    return;

  old_context = MemoryContextSwitchTo(state->stripe_context);

  /* Get column chunk metadata from catalog */
  column_chunks = orochi_catalog_get_stripe_columns(stripe->stripe_id);

  /* Allocate chunk group structure */
  state->current_chunk_group = palloc0(sizeof(ColumnarChunkGroup));
  state->current_chunk_group->chunk_group_index = chunk_group_index;
  state->current_chunk_group->column_count = tupdesc->natts;
  state->current_chunk_group->row_count = stripe->row_count;

  /* Process each column in projection */
  for (col_idx = 0; col_idx < tupdesc->natts; col_idx++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, col_idx);
    OrochiColumnChunk *chunk_meta = NULL;
    struct ColumnReadBuffer *col_buf = &state->column_buffers[col_idx];

    /* Skip if not in projection */
    if (!bms_is_member(col_idx, state->columns_needed)) {
      col_buf->is_loaded = false;
      continue;
    }

    /* Find the metadata for this column */
    foreach (lc, column_chunks) {
      OrochiColumnChunk *cc = (OrochiColumnChunk *)lfirst(lc);
      if (cc->column_index == col_idx) {
        chunk_meta = cc;
        break;
      }
    }

    if (chunk_meta == NULL) {
      /* No data for this column, initialize with nulls */
      col_buf->values = palloc0(stripe->row_count * sizeof(Datum));
      col_buf->nulls = palloc(stripe->row_count * sizeof(bool));
      memset(col_buf->nulls, true, stripe->row_count * sizeof(bool));
      col_buf->row_count = stripe->row_count;
      col_buf->is_loaded = true;
      continue;
    }

    /* Allocate buffers for decompressed data */
    col_buf->values = palloc0(chunk_meta->value_count * sizeof(Datum));
    col_buf->nulls = palloc0(chunk_meta->value_count * sizeof(bool));
    col_buf->row_count = chunk_meta->value_count;

    /*
     * Read compressed data from catalog storage and decompress.
     * Use the catalog function which reads from orochi_column_chunks table.
     */
    {
      char *compressed_data = NULL;
      int64 compressed_size = 0;
      int64 decompressed_size = chunk_meta->decompressed_size;
      char *decompressed_buffer = NULL;

      /* Read the compressed chunk data from catalog storage */
      compressed_data = orochi_catalog_read_chunk_data(
          stripe->stripe_id, chunk_group_index, col_idx, &compressed_size);

      /* Decompress the data if we have it */
      if (compressed_data != NULL && compressed_size > 0) {
        OrochiCompressionType compression_type = chunk_meta->compression;

        /* Allocate decompression buffer */
        if (decompressed_size <= 0)
          decompressed_size = compressed_size;
        decompressed_buffer = palloc(decompressed_size);

        if (compression_type != OROCHI_COMPRESS_NONE) {
          /* Decompress the column data using appropriate algorithm */
          int64 actual_size = columnar_decompress_buffer(
              compressed_data, compressed_size, decompressed_buffer,
              decompressed_size, compression_type);

          if (actual_size > 0 && actual_size != decompressed_size) {
            elog(DEBUG1, "Column %d decompression: expected %ld, got %ld",
                 col_idx, decompressed_size, actual_size);
            decompressed_size = actual_size;
          }
        } else {
          /* No compression, just copy the raw data */
          memcpy(decompressed_buffer, compressed_data, compressed_size);
          decompressed_size = compressed_size;
        }

        /*
         * Decode the decompressed data into Datum values.
         * Format depends on whether type is pass-by-value or pass-by-reference.
         */
        if (attr->attbyval && attr->attlen > 0) {
          /* Fixed-width pass-by-value type (int, float, etc.) */
          int64 j;
          for (j = 0; j < chunk_meta->value_count &&
                      j * attr->attlen < decompressed_size;
               j++) {
            memcpy(&col_buf->values[j],
                   decompressed_buffer + (j * attr->attlen), attr->attlen);
            col_buf->nulls[j] = false;
          }
          /* Mark remaining as null if we ran out of data */
          while (j < chunk_meta->value_count) {
            col_buf->nulls[j] = true;
            col_buf->values[j] = (Datum)0;
            j++;
          }
        } else {
          /* Variable-length type - values stored with size prefix */
          int64 offset = 0;
          int64 j = 0;

          while (offset < decompressed_size && j < chunk_meta->value_count) {
            Size value_size;

            /* Read value size */
            if (offset + (int64)sizeof(Size) > decompressed_size)
              break;

            memcpy(&value_size, decompressed_buffer + offset, sizeof(Size));
            offset += sizeof(Size);

            if (value_size == 0) {
              /* Null value */
              col_buf->nulls[j] = true;
              col_buf->values[j] = (Datum)0;
            } else {
              /* Read actual value */
              if (offset + (int64)value_size > decompressed_size)
                break;

              /* Copy value with proper alignment */
              void *value_ptr = palloc(value_size);
              memcpy(value_ptr, decompressed_buffer + offset, value_size);
              col_buf->values[j] = PointerGetDatum(value_ptr);
              col_buf->nulls[j] = false;

              offset += value_size;
            }
            j++;
          }

          /* Mark remaining as nulls if we ran out of data */
          while (j < chunk_meta->value_count) {
            col_buf->nulls[j] = true;
            col_buf->values[j] = (Datum)0;
            j++;
          }
        }

        pfree(decompressed_buffer);
        pfree(compressed_data);
      } else {
        /*
         * No compressed data available in catalog storage.
         * Initialize with defaults based on null metadata.
         */
        if (chunk_meta->has_nulls) {
          memset(col_buf->nulls, true, chunk_meta->value_count * sizeof(bool));
        } else {
          memset(col_buf->nulls, false, chunk_meta->value_count * sizeof(bool));
          memset(col_buf->values, 0, chunk_meta->value_count * sizeof(Datum));
        }
      }
    }

    col_buf->is_loaded = true;
  }

  /* Set row position in chunk */
  state->current_row_in_chunk = 0;

  /* Free column chunks list */
  list_free_deep(column_chunks);

  MemoryContextSwitchTo(old_context);
}

bool columnar_read_next_row(ColumnarReadState *state, TupleTableSlot *slot) {
  TupleDesc tupdesc = state->tupdesc;
  Datum *values;
  bool *nulls;
  int i;

  ExecClearTuple(slot);

  /* Check if we need to load next chunk group */
  if (state->current_chunk_group == NULL ||
      state->current_row_in_chunk >= state->current_chunk_group->row_count) {
    /* Try next chunk group */
    state->current_chunk_group_index++;

    if (state->current_stripe == NULL ||
        state->current_chunk_group_index >=
            state->current_stripe->chunk_group_count) {
      /* Need next stripe */
      state->current_stripe_index++;

      if (state->current_stripe_index >= list_length(state->stripe_list)) {
        /* No more data */
        return false;
      }

      state->current_stripe =
          list_nth(state->stripe_list, state->current_stripe_index);
      state->current_chunk_group_index = 0;
      state->stripes_scanned++;

      MemoryContextReset(state->stripe_context);
    }

    /* Load chunk group */
    load_chunk_group(state, state->current_chunk_group_index);
    state->current_row_in_chunk = 0;
  }

  /* Read values from column buffers */
  values = palloc(tupdesc->natts * sizeof(Datum));
  nulls = palloc(tupdesc->natts * sizeof(bool));

  for (i = 0; i < tupdesc->natts; i++) {
    struct ColumnReadBuffer *buf = &state->column_buffers[i];

    if (bms_is_member(i, state->columns_needed) && buf->is_loaded) {
      values[i] = buf->values[state->current_row_in_chunk];
      nulls[i] = buf->nulls[state->current_row_in_chunk];
    } else {
      values[i] = (Datum)0;
      nulls[i] = true;
    }
  }

  /* Store in slot */
  slot->tts_values = values;
  slot->tts_isnull = nulls;
  ExecStoreVirtualTuple(slot);

  state->current_row_in_chunk++;
  state->total_rows_read++;

  return true;
}

int columnar_read_next_vector(ColumnarReadState *state, TupleTableSlot *slot,
                              int max_rows) {
  int rows_read = 0;

  /* Vectorized read - batch multiple rows */
  while (rows_read < max_rows) {
    if (!columnar_read_next_row(state, slot))
      break;
    rows_read++;
  }

  return rows_read;
}

bool columnar_seek_to_row(ColumnarReadState *state, int64 row_number) {
  ListCell *lc;
  int stripe_index = 0;

  /* Find stripe containing row */
  foreach (lc, state->stripe_list) {
    ColumnarStripe *stripe = (ColumnarStripe *)lfirst(lc);

    if (row_number >= stripe->first_row_number &&
        row_number < stripe->first_row_number + stripe->row_count) {
      state->current_stripe_index = stripe_index;
      state->current_stripe = stripe;

      /* Calculate chunk group and row within */
      int64 row_in_stripe = row_number - stripe->first_row_number;

      /*
       * Calculate the chunk group index based on row position.
       * Each chunk group contains chunk_group_row_limit rows.
       */
      int32 chunk_group_row_limit = COLUMNAR_DEFAULT_CHUNK_GROUP_ROW_LIMIT;
      int32 chunk_group_index = (int32)(row_in_stripe / chunk_group_row_limit);
      int64 row_in_chunk = row_in_stripe % chunk_group_row_limit;

      /* Load the correct chunk group */
      state->current_chunk_group_index = chunk_group_index;
      load_chunk_group(state, chunk_group_index);
      state->current_row_in_chunk = row_in_chunk;

      return true;
    }
    stripe_index++;
  }

  return false;
}

void columnar_end_read(ColumnarReadState *state) {
  MemoryContextDelete(state->read_context);
}

/* ============================================================
 * Skip List Operations
 * ============================================================ */

ColumnarSkipList *columnar_build_skip_list(Relation relation) {
  ColumnarSkipList *skip_list;
  List *stripes;
  ListCell *stripe_lc;
  int total_entries = 0;
  int entry_idx = 0;

  skip_list = palloc0(sizeof(ColumnarSkipList));

  /* Get all stripes for this table */
  stripes = orochi_catalog_get_table_stripes(RelationGetRelid(relation));

  if (stripes == NIL)
    return skip_list;

  /* First pass: count total entries needed */
  foreach (stripe_lc, stripes) {
    ColumnarStripe *stripe = (ColumnarStripe *)lfirst(stripe_lc);
    List *columns = orochi_catalog_get_stripe_columns(stripe->stripe_id);
    total_entries += list_length(columns);
    list_free_deep(columns);
  }

  if (total_entries == 0)
    return skip_list;

  /* Allocate entries array */
  skip_list->entries = palloc0(total_entries * sizeof(ColumnarSkipListEntry));
  skip_list->entry_count = total_entries;

  /* Second pass: populate entries */
  foreach (stripe_lc, stripes) {
    ColumnarStripe *stripe = (ColumnarStripe *)lfirst(stripe_lc);
    List *columns = orochi_catalog_get_stripe_columns(stripe->stripe_id);
    ListCell *col_lc;

    foreach (col_lc, columns) {
      OrochiColumnChunk *chunk = (OrochiColumnChunk *)lfirst(col_lc);
      ColumnarSkipListEntry *entry = &skip_list->entries[entry_idx++];

      entry->stripe_id = stripe->stripe_id;
      entry->chunk_group_index = 0; /* Default to first chunk group */
      entry->column_index = chunk->column_index;
      entry->min_value = chunk->min_value;
      entry->max_value = chunk->max_value;
      entry->row_count = chunk->value_count;
      entry->has_nulls = chunk->has_nulls;
    }

    list_free_deep(columns);
  }

  list_free_deep(stripes);

  return skip_list;
}

/*
 * columnar_can_skip_chunk
 *
 * Evaluate predicates against min/max statistics to determine if chunk can be
 * skipped. Returns true if the chunk definitely contains no matching rows.
 */
bool columnar_can_skip_chunk(ColumnarSkipListEntry *entry, List *predicates) {
  ListCell *lc;

  if (entry == NULL || predicates == NIL)
    return false;

  /* Evaluate each predicate against min/max */
  foreach (lc, predicates) {
    Node *pred = (Node *)lfirst(lc);

    if (IsA(pred, OpExpr)) {
      OpExpr *op = (OpExpr *)pred;
      Oid opno = op->opno;
      Node *left;
      Node *right;
      Var *var = NULL;
      Const *constval = NULL;

      if (list_length(op->args) != 2)
        continue;

      left = (Node *)linitial(op->args);
      right = (Node *)lsecond(op->args);

      /* Identify var and const in the expression */
      if (IsA(left, Var) && IsA(right, Const)) {
        var = (Var *)left;
        constval = (Const *)right;
      } else if (IsA(left, Const) && IsA(right, Var)) {
        constval = (Const *)left;
        var = (Var *)right;
      } else
        continue;

      /* Skip if wrong column */
      if (var->varattno - 1 != entry->column_index)
        continue;

      /* Handle null constant */
      if (constval->constisnull) {
        /* x = NULL never matches any row */
        return true;
      }

      /* Evaluate based on operator type */
      /* This is a simplified version - full implementation would use
       * the operator's comparison function */
      switch (opno) {
      case Int4EqualOperator:
      case Int8EqualOperator:
      case Float8EqualOperator:
        /* For equality, skip if value outside min/max range */
        {
          Datum val = constval->constvalue;
          /* Simple numeric comparison */
          if (DatumGetInt64(val) < DatumGetInt64(entry->min_value) ||
              DatumGetInt64(val) > DatumGetInt64(entry->max_value))
            return true;
        }
        break;

      case Int4LessOperator:
      case Int8LessOperator:
        /* WHERE col < val: skip if min >= val */
        if (DatumGetInt64(entry->min_value) >=
            DatumGetInt64(constval->constvalue))
          return true;
        break;

      case Int4GreaterOperator:
      case Int8GreaterOperator:
        /* WHERE col > val: skip if max <= val */
        if (DatumGetInt64(entry->max_value) <=
            DatumGetInt64(constval->constvalue))
          return true;
        break;

      default:
        /* Unknown operator, don't skip */
        break;
      }
    }
  }

  return false;
}

/*
 * columnar_get_matching_chunks
 *
 * Filter skip list entries using predicates and return matching chunks.
 */
List *columnar_get_matching_chunks(ColumnarSkipList *skip_list,
                                   List *predicates) {
  List *result = NIL;
  int i;

  if (skip_list == NULL || skip_list->entry_count == 0)
    return NIL;

  for (i = 0; i < skip_list->entry_count; i++) {
    ColumnarSkipListEntry *entry = &skip_list->entries[i];

    /* Include chunk if it can't be skipped */
    if (!columnar_can_skip_chunk(entry, predicates)) {
      /* Copy entry to result */
      ColumnarSkipListEntry *match = palloc(sizeof(ColumnarSkipListEntry));
      memcpy(match, entry, sizeof(ColumnarSkipListEntry));
      result = lappend(result, match);
    }
  }

  return result;
}

/* ============================================================
 * Table Access Method Callbacks
 * ============================================================ */

/* Scan descriptor for columnar tables */
typedef struct ColumnarScanDescData {
  TableScanDescData base;
  ColumnarReadState *read_state;
} ColumnarScanDescData;

typedef ColumnarScanDescData *ColumnarScanDesc;

TableScanDesc columnar_beginscan(Relation relation, Snapshot snapshot,
                                 int nkeys, struct ScanKeyData *keys,
                                 ParallelTableScanDesc parallel_scan,
                                 uint32 flags) {
  ColumnarScanDesc scan;

  scan = (ColumnarScanDesc)palloc0(sizeof(ColumnarScanDescData));
  scan->base.rs_rd = relation;
  scan->base.rs_snapshot = snapshot;
  scan->base.rs_nkeys = nkeys;
  scan->base.rs_flags = flags;
  scan->base.rs_parallel = parallel_scan;

  scan->read_state = columnar_begin_read(relation, snapshot, NULL);

  return (TableScanDesc)scan;
}

void columnar_endscan(TableScanDesc sscan) {
  ColumnarScanDesc scan = (ColumnarScanDesc)sscan;

  if (scan->read_state)
    columnar_end_read(scan->read_state);

  pfree(scan);
}

void columnar_rescan(TableScanDesc sscan, struct ScanKeyData *key,
                     bool set_params, bool allow_strat, bool allow_sync,
                     bool allow_pagemode) {
  ColumnarScanDesc scan = (ColumnarScanDesc)sscan;

  /* Reset read state to beginning */
  if (scan->read_state) {
    scan->read_state->current_stripe_index = -1;
    scan->read_state->current_chunk_group_index = -1;
    scan->read_state->current_row_in_chunk = 0;
  }
}

bool columnar_getnextslot(TableScanDesc sscan, ScanDirection direction,
                          TupleTableSlot *slot) {
  ColumnarScanDesc scan = (ColumnarScanDesc)sscan;

  /* Only forward scan supported for now */
  if (direction != ForwardScanDirection)
    elog(ERROR, "Columnar storage only supports forward scans");

  return columnar_read_next_row(scan->read_state, slot);
}

void columnar_tuple_insert(Relation relation, TupleTableSlot *slot,
                           CommandId cid, int options,
                           struct BulkInsertStateData *bistate) {
  ColumnarWriteState *write_state;
  Datum *values;
  bool *nulls;
  TupleDesc tupdesc = RelationGetDescr(relation);

  write_state = columnar_begin_write(relation, NULL);

  values = palloc(tupdesc->natts * sizeof(Datum));
  nulls = palloc(tupdesc->natts * sizeof(bool));

  slot_getallattrs(slot);
  memcpy(values, slot->tts_values, tupdesc->natts * sizeof(Datum));
  memcpy(nulls, slot->tts_isnull, tupdesc->natts * sizeof(bool));

  columnar_write_row(write_state, values, nulls);
  columnar_end_write(write_state);

  pfree(values);
  pfree(nulls);
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

ItemPointerData columnar_row_to_itemptr(int64 row_number) {
  ItemPointerData tid;
  BlockNumber block = row_number / MaxHeapTuplesPerPage;
  OffsetNumber offset = (row_number % MaxHeapTuplesPerPage) + 1;

  ItemPointerSet(&tid, block, offset);
  return tid;
}

int64 columnar_itemptr_to_row(ItemPointer tid) {
  return (int64)ItemPointerGetBlockNumber(tid) * MaxHeapTuplesPerPage +
         ItemPointerGetOffsetNumber(tid) - 1;
}

const char *columnar_compression_name(OrochiCompressionType type) {
  switch (type) {
  case OROCHI_COMPRESS_NONE:
    return "none";
  case OROCHI_COMPRESS_LZ4:
    return "lz4";
  case OROCHI_COMPRESS_ZSTD:
    return "zstd";
  case OROCHI_COMPRESS_PGLZ:
    return "pglz";
  case OROCHI_COMPRESS_DELTA:
    return "delta";
  case OROCHI_COMPRESS_GORILLA:
    return "gorilla";
  case OROCHI_COMPRESS_DICTIONARY:
    return "dictionary";
  case OROCHI_COMPRESS_RLE:
    return "rle";
  default:
    return "unknown";
  }
}

OrochiCompressionType columnar_parse_compression(const char *name) {
  if (pg_strcasecmp(name, "none") == 0)
    return OROCHI_COMPRESS_NONE;
  if (pg_strcasecmp(name, "lz4") == 0)
    return OROCHI_COMPRESS_LZ4;
  if (pg_strcasecmp(name, "zstd") == 0)
    return OROCHI_COMPRESS_ZSTD;
  if (pg_strcasecmp(name, "pglz") == 0)
    return OROCHI_COMPRESS_PGLZ;
  if (pg_strcasecmp(name, "delta") == 0)
    return OROCHI_COMPRESS_DELTA;
  if (pg_strcasecmp(name, "gorilla") == 0)
    return OROCHI_COMPRESS_GORILLA;
  if (pg_strcasecmp(name, "dictionary") == 0)
    return OROCHI_COMPRESS_DICTIONARY;
  if (pg_strcasecmp(name, "rle") == 0)
    return OROCHI_COMPRESS_RLE;

  elog(ERROR, "Unknown compression type: %s", name);
  return OROCHI_COMPRESS_NONE; /* Keep compiler happy */
}
