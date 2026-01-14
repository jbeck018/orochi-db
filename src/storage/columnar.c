/*-------------------------------------------------------------------------
 *
 * columnar.c
 *    Orochi DB columnar storage engine implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

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

int64
columnar_compress_buffer(const char *input, int64 input_size,
                         char *output, int64 max_output_size,
                         OrochiCompressionType type, int level)
{
    int64 compressed_size = 0;

    switch (type)
    {
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
            compressed_size = compress_zstd(input, input_size, output, max_output_size, level);
            break;

        case OROCHI_COMPRESS_PGLZ:
            /* PostgreSQL native compression */
            compressed_size = compress_pglz(input, input_size, output, max_output_size);
            break;

        case OROCHI_COMPRESS_DELTA:
            /* Delta encoding for integers */
            compressed_size = compress_delta(input, input_size, output, max_output_size);
            break;

        case OROCHI_COMPRESS_GORILLA:
            /* Gorilla compression for floats */
            compressed_size = compress_gorilla(input, input_size, output, max_output_size);
            break;

        case OROCHI_COMPRESS_DICTIONARY:
            /* Dictionary encoding */
            compressed_size = compress_dictionary(input, input_size, output, max_output_size);
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

int64
columnar_decompress_buffer(const char *input, int64 input_size,
                           char *output, int64 expected_size,
                           OrochiCompressionType type)
{
    int64 decompressed_size = 0;

    switch (type)
    {
        case OROCHI_COMPRESS_NONE:
            memcpy(output, input, input_size);
            decompressed_size = input_size;
            break;

        case OROCHI_COMPRESS_LZ4:
            decompressed_size = decompress_lz4(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_ZSTD:
            decompressed_size = decompress_zstd(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_PGLZ:
            decompressed_size = decompress_pglz(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_DELTA:
            decompressed_size = decompress_delta(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_GORILLA:
            decompressed_size = decompress_gorilla(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_DICTIONARY:
            decompressed_size = decompress_dictionary(input, input_size, output, expected_size);
            break;

        case OROCHI_COMPRESS_RLE:
            decompressed_size = decompress_rle(input, input_size, output, expected_size);
            break;

        default:
            elog(ERROR, "Unknown compression type: %d", type);
    }

    if (decompressed_size != expected_size)
        elog(WARNING, "Decompressed size mismatch: got %ld, expected %ld",
             decompressed_size, expected_size);

    return decompressed_size;
}

OrochiCompressionType
columnar_select_compression(Oid type_oid, Datum *sample_values, int sample_count)
{
    /*
     * Select optimal compression algorithm based on data type:
     * - Integers with steady delta: Delta encoding
     * - Floats: Gorilla compression
     * - Low-cardinality strings: Dictionary
     * - Boolean: RLE
     * - Default: ZSTD
     */
    switch (type_oid)
    {
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
            /* TODO: Analyze cardinality for dictionary encoding */
            return OROCHI_COMPRESS_ZSTD;

        default:
            return OROCHI_COMPRESS_ZSTD;
    }
}

/* ============================================================
 * Write Operations
 * ============================================================ */

ColumnarWriteState *
columnar_begin_write(Relation relation, ColumnarOptions *options)
{
    ColumnarWriteState *state;
    TupleDesc tupdesc = RelationGetDescr(relation);
    int i;
    MemoryContext old_context;

    /* Create write state in dedicated memory context */
    state = (ColumnarWriteState *) palloc0(sizeof(ColumnarWriteState));

    state->write_context = AllocSetContextCreate(CurrentMemoryContext,
                                                 "ColumnarWriteContext",
                                                 ALLOCSET_DEFAULT_SIZES);
    state->stripe_context = AllocSetContextCreate(state->write_context,
                                                  "ColumnarStripeContext",
                                                  ALLOCSET_DEFAULT_SIZES);

    old_context = MemoryContextSwitchTo(state->write_context);

    state->relation = relation;
    state->tupdesc = CreateTupleDescCopy(tupdesc);

    /* Set options */
    if (options)
        memcpy(&state->options, options, sizeof(ColumnarOptions));
    else
    {
        /* Default options */
        state->options.stripe_row_limit = orochi_stripe_row_limit;
        state->options.chunk_group_row_limit = COLUMNAR_DEFAULT_CHUNK_GROUP_ROW_LIMIT;
        state->options.compression_type = OROCHI_COMPRESS_ZSTD;
        state->options.compression_level = orochi_compression_level;
        state->options.enable_vectorization = orochi_enable_vectorized_execution;
    }

    /* Allocate per-column write buffers */
    state->column_buffers = palloc0(tupdesc->natts * sizeof(*state->column_buffers));

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (attr->attisdropped)
            continue;

        state->column_buffers[i].capacity = state->options.chunk_group_row_limit;
        state->column_buffers[i].values = palloc(sizeof(Datum) *
                                                  state->column_buffers[i].capacity);
        state->column_buffers[i].nulls = palloc(sizeof(bool) *
                                                 state->column_buffers[i].capacity);
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

void
columnar_write_row(ColumnarWriteState *state, Datum *values, bool *nulls)
{
    TupleDesc tupdesc = state->tupdesc;
    int i;

    /* Buffer values for each column */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        struct ColumnWriteBuffer *buf = &state->column_buffers[i];

        if (attr->attisdropped)
            continue;

        if (buf->count >= buf->capacity)
        {
            /* Expand buffer */
            buf->capacity *= 2;
            buf->values = repalloc(buf->values, sizeof(Datum) * buf->capacity);
            buf->nulls = repalloc(buf->nulls, sizeof(bool) * buf->capacity);
        }

        buf->nulls[buf->count] = nulls[i];

        if (!nulls[i])
        {
            /* Copy datum (deep copy for pass-by-reference types) */
            if (attr->attbyval)
                buf->values[buf->count] = values[i];
            else
            {
                Size datum_size = datumGetSize(values[i], attr->attbyval, attr->attlen);
                buf->values[buf->count] = datumCopy(values[i], attr->attbyval, attr->attlen);
            }

            /* Update min/max statistics */
            if (buf->count == 0 || !buf->has_nulls)
            {
                buf->min_value = buf->values[buf->count];
                buf->max_value = buf->values[buf->count];
            }
            /* TODO: Compare and update min/max */
        }
        else
        {
            buf->has_nulls = true;
        }

        buf->count++;
    }

    state->chunk_group_row_count++;
    state->stripe_row_count++;
    state->total_rows_written++;

    /* Check if chunk group is full */
    if (state->chunk_group_row_count >= state->options.chunk_group_row_limit)
    {
        serialize_chunk_group(state);
    }

    /* Check if stripe is full */
    if (state->stripe_row_count >= state->options.stripe_row_limit)
    {
        flush_stripe(state);
    }
}

void
columnar_write_rows(ColumnarWriteState *state, TupleTableSlot **slots, int count)
{
    int i;

    for (i = 0; i < count; i++)
    {
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

static void
serialize_chunk_group(ColumnarWriteState *state)
{
    TupleDesc tupdesc = state->tupdesc;
    ColumnarChunkGroup *cg = state->current_chunk_group;
    int i;
    MemoryContext old_context;

    old_context = MemoryContextSwitchTo(state->stripe_context);

    /* Allocate column chunks */
    cg->columns = palloc0(tupdesc->natts * sizeof(ColumnarColumnChunk *));
    cg->row_count = state->chunk_group_row_count;

    for (i = 0; i < tupdesc->natts; i++)
    {
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
        for (int j = 0; j < buf->count; j++)
        {
            if (buf->nulls[j])
                chunk->null_count++;
        }

        /* Select compression based on type */
        compression = columnar_select_compression(attr->atttypid, buf->values, buf->count);
        chunk->compression_type = compression;

        /* Serialize values to buffer */
        if (attr->attbyval && attr->attlen > 0)
        {
            value_buffer_size = buf->count * attr->attlen;
            raw_buffer = palloc(value_buffer_size);

            for (int j = 0; j < buf->count; j++)
            {
                if (!buf->nulls[j])
                    memcpy(raw_buffer + (j * attr->attlen), &buf->values[j], attr->attlen);
                else
                    memset(raw_buffer + (j * attr->attlen), 0, attr->attlen);
            }
        }
        else
        {
            /* Variable-length types - serialize with length prefix */
            StringInfoData si;
            initStringInfo(&si);

            for (int j = 0; j < buf->count; j++)
            {
                if (!buf->nulls[j])
                {
                    Size datum_size = datumGetSize(buf->values[j], attr->attbyval, attr->attlen);
                    appendBinaryStringInfo(&si, (char *)&datum_size, sizeof(Size));
                    appendBinaryStringInfo(&si, DatumGetPointer(buf->values[j]), datum_size);
                }
                else
                {
                    Size zero = 0;
                    appendBinaryStringInfo(&si, (char *)&zero, sizeof(Size));
                }
            }

            raw_buffer = si.data;
            value_buffer_size = si.len;
        }

        /* Compress the buffer */
        chunk->decompressed_size = value_buffer_size;
        compressed_buffer = palloc(value_buffer_size);  /* Worst case */

        compressed_size = columnar_compress_buffer(raw_buffer, value_buffer_size,
                                                   compressed_buffer, value_buffer_size,
                                                   compression,
                                                   state->options.compression_level);

        if (compressed_size < value_buffer_size)
        {
            chunk->compressed_size = compressed_size;
            chunk->value_buffer = compressed_buffer;
            pfree(raw_buffer);
        }
        else
        {
            /* Compression didn't help, store uncompressed */
            chunk->compression_type = OROCHI_COMPRESS_NONE;
            chunk->compressed_size = value_buffer_size;
            chunk->value_buffer = raw_buffer;
            pfree(compressed_buffer);
        }

        /* Store null bitmap */
        if (chunk->has_nulls)
        {
            int null_bytes = (buf->count + 7) / 8;
            chunk->null_buffer = palloc0(null_bytes);

            for (int j = 0; j < buf->count; j++)
            {
                if (buf->nulls[j])
                    chunk->null_buffer[j / 8] |= (1 << (j % 8));
            }
        }

        /* Store min/max */
        chunk->has_min_max = !buf->has_nulls || buf->count > chunk->null_count;
        if (chunk->has_min_max)
        {
            chunk->min_value = buf->min_value;
            chunk->max_value = buf->max_value;
        }

        cg->columns[i] = chunk;

        /* Reset buffer for next chunk group */
        buf->count = 0;
        buf->has_nulls = false;
    }

    /* Increment chunk group index */
    state->current_stripe->chunk_group_count++;
    state->chunk_group_row_count = 0;

    MemoryContextSwitchTo(old_context);
}

static void
flush_stripe(ColumnarWriteState *state)
{
    ColumnarStripe *stripe = state->current_stripe;
    int64 stripe_id;
    MemoryContext old_context;

    /* Serialize any remaining data */
    if (state->chunk_group_row_count > 0)
        serialize_chunk_group(state);

    /* Get next stripe ID from catalog */
    stripe_id = orochi_catalog_create_stripe(RelationGetRelid(state->relation),
                                             state->current_stripe->first_row_number,
                                             state->stripe_row_count,
                                             state->current_stripe->column_count);
    stripe->stripe_id = stripe_id;
    stripe->row_count = state->stripe_row_count;

    /* Write stripe data to storage */
    /* TODO: Implement actual storage write */

    /* Update catalog with final sizes */
    orochi_catalog_update_stripe_status(stripe_id, true,
                                        stripe->data_size,
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

void
columnar_flush_writes(ColumnarWriteState *state)
{
    if (state->stripe_row_count > 0)
        flush_stripe(state);
}

void
columnar_end_write(ColumnarWriteState *state)
{
    /* Flush any remaining data */
    columnar_flush_writes(state);

    /* Free memory contexts */
    MemoryContextDelete(state->write_context);
}

/* ============================================================
 * Read Operations
 * ============================================================ */

ColumnarReadState *
columnar_begin_read(Relation relation, Snapshot snapshot, Bitmapset *columns_needed)
{
    ColumnarReadState *state;
    TupleDesc tupdesc = RelationGetDescr(relation);
    int i;
    MemoryContext old_context;

    state = (ColumnarReadState *) palloc0(sizeof(ColumnarReadState));

    state->read_context = AllocSetContextCreate(CurrentMemoryContext,
                                                "ColumnarReadContext",
                                                ALLOCSET_DEFAULT_SIZES);
    state->stripe_context = AllocSetContextCreate(state->read_context,
                                                  "ColumnarStripeReadContext",
                                                  ALLOCSET_DEFAULT_SIZES);

    old_context = MemoryContextSwitchTo(state->read_context);

    state->relation = relation;
    state->tupdesc = CreateTupleDescCopy(tupdesc);
    state->snapshot = snapshot;
    state->columns_needed = bms_copy(columns_needed);

    /* If no columns specified, need all */
    if (bms_is_empty(state->columns_needed))
    {
        for (i = 0; i < tupdesc->natts; i++)
        {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            if (!attr->attisdropped)
                state->columns_needed = bms_add_member(state->columns_needed, i);
        }
    }

    /* Allocate column read buffers */
    state->column_buffers = palloc0(tupdesc->natts * sizeof(*state->column_buffers));

    /* Load stripe list from catalog */
    state->stripe_list = orochi_catalog_get_table_stripes(RelationGetRelid(relation));
    state->current_stripe_index = -1;

    /* Load skip list for predicate pushdown */
    state->skip_list = columnar_build_skip_list(relation);

    /* Vectorization settings */
    state->enable_vectorization = orochi_enable_vectorized_execution;
    state->vector_batch_size = COLUMNAR_VECTOR_BATCH_SIZE;

    MemoryContextSwitchTo(old_context);

    return state;
}

static void
load_chunk_group(ColumnarReadState *state, int chunk_group_index)
{
    /* TODO: Load chunk group data from storage */
    /* This would:
     * 1. Read compressed column data from storage
     * 2. Decompress into column_buffers
     * 3. Set up for iteration
     */
}

bool
columnar_read_next_row(ColumnarReadState *state, TupleTableSlot *slot)
{
    TupleDesc tupdesc = state->tupdesc;
    Datum *values;
    bool *nulls;
    int i;

    ExecClearTuple(slot);

    /* Check if we need to load next chunk group */
    if (state->current_chunk_group == NULL ||
        state->current_row_in_chunk >= state->current_chunk_group->row_count)
    {
        /* Try next chunk group */
        state->current_chunk_group_index++;

        if (state->current_stripe == NULL ||
            state->current_chunk_group_index >= state->current_stripe->chunk_group_count)
        {
            /* Need next stripe */
            state->current_stripe_index++;

            if (state->current_stripe_index >= list_length(state->stripe_list))
            {
                /* No more data */
                return false;
            }

            state->current_stripe = list_nth(state->stripe_list, state->current_stripe_index);
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

    for (i = 0; i < tupdesc->natts; i++)
    {
        struct ColumnReadBuffer *buf = &state->column_buffers[i];

        if (bms_is_member(i, state->columns_needed) && buf->is_loaded)
        {
            values[i] = buf->values[state->current_row_in_chunk];
            nulls[i] = buf->nulls[state->current_row_in_chunk];
        }
        else
        {
            values[i] = (Datum) 0;
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

int
columnar_read_next_vector(ColumnarReadState *state, TupleTableSlot *slot, int max_rows)
{
    int rows_read = 0;

    /* Vectorized read - batch multiple rows */
    while (rows_read < max_rows)
    {
        if (!columnar_read_next_row(state, slot))
            break;
        rows_read++;
    }

    return rows_read;
}

bool
columnar_seek_to_row(ColumnarReadState *state, int64 row_number)
{
    ListCell *lc;
    int stripe_index = 0;

    /* Find stripe containing row */
    foreach(lc, state->stripe_list)
    {
        ColumnarStripe *stripe = (ColumnarStripe *) lfirst(lc);

        if (row_number >= stripe->first_row_number &&
            row_number < stripe->first_row_number + stripe->row_count)
        {
            state->current_stripe_index = stripe_index;
            state->current_stripe = stripe;

            /* Calculate chunk group and row within */
            int64 row_in_stripe = row_number - stripe->first_row_number;
            /* TODO: Find correct chunk group */

            return true;
        }
        stripe_index++;
    }

    return false;
}

void
columnar_end_read(ColumnarReadState *state)
{
    MemoryContextDelete(state->read_context);
}

/* ============================================================
 * Skip List Operations
 * ============================================================ */

ColumnarSkipList *
columnar_build_skip_list(Relation relation)
{
    ColumnarSkipList *skip_list;

    skip_list = palloc0(sizeof(ColumnarSkipList));

    /* TODO: Load skip list from catalog */

    return skip_list;
}

bool
columnar_can_skip_chunk(ColumnarSkipListEntry *entry, List *predicates)
{
    /* TODO: Implement predicate evaluation against min/max */
    return false;
}

/* ============================================================
 * Table Access Method Callbacks
 * ============================================================ */

/* Scan descriptor for columnar tables */
typedef struct ColumnarScanDescData
{
    TableScanDescData   base;
    ColumnarReadState  *read_state;
} ColumnarScanDescData;

typedef ColumnarScanDescData *ColumnarScanDesc;

TableScanDesc
columnar_beginscan(Relation relation, Snapshot snapshot,
                   int nkeys, struct ScanKeyData *keys,
                   ParallelTableScanDesc parallel_scan,
                   uint32 flags)
{
    ColumnarScanDesc scan;

    scan = (ColumnarScanDesc) palloc0(sizeof(ColumnarScanDescData));
    scan->base.rs_rd = relation;
    scan->base.rs_snapshot = snapshot;
    scan->base.rs_nkeys = nkeys;
    scan->base.rs_flags = flags;
    scan->base.rs_parallel = parallel_scan;

    scan->read_state = columnar_begin_read(relation, snapshot, NULL);

    return (TableScanDesc) scan;
}

void
columnar_endscan(TableScanDesc sscan)
{
    ColumnarScanDesc scan = (ColumnarScanDesc) sscan;

    if (scan->read_state)
        columnar_end_read(scan->read_state);

    pfree(scan);
}

void
columnar_rescan(TableScanDesc sscan, struct ScanKeyData *key,
                bool set_params, bool allow_strat, bool allow_sync,
                bool allow_pagemode)
{
    ColumnarScanDesc scan = (ColumnarScanDesc) sscan;

    /* Reset read state to beginning */
    if (scan->read_state)
    {
        scan->read_state->current_stripe_index = -1;
        scan->read_state->current_chunk_group_index = -1;
        scan->read_state->current_row_in_chunk = 0;
    }
}

bool
columnar_getnextslot(TableScanDesc sscan, ScanDirection direction,
                     TupleTableSlot *slot)
{
    ColumnarScanDesc scan = (ColumnarScanDesc) sscan;

    /* Only forward scan supported for now */
    if (direction != ForwardScanDirection)
        elog(ERROR, "Columnar storage only supports forward scans");

    return columnar_read_next_row(scan->read_state, slot);
}

void
columnar_tuple_insert(Relation relation, TupleTableSlot *slot,
                      CommandId cid, int options,
                      struct BulkInsertStateData *bistate)
{
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

ItemPointerData
columnar_row_to_itemptr(int64 row_number)
{
    ItemPointerData tid;
    BlockNumber block = row_number / MaxHeapTuplesPerPage;
    OffsetNumber offset = (row_number % MaxHeapTuplesPerPage) + 1;

    ItemPointerSet(&tid, block, offset);
    return tid;
}

int64
columnar_itemptr_to_row(ItemPointer tid)
{
    return (int64) ItemPointerGetBlockNumber(tid) * MaxHeapTuplesPerPage +
           ItemPointerGetOffsetNumber(tid) - 1;
}

const char *
columnar_compression_name(OrochiCompressionType type)
{
    switch (type)
    {
        case OROCHI_COMPRESS_NONE:       return "none";
        case OROCHI_COMPRESS_LZ4:        return "lz4";
        case OROCHI_COMPRESS_ZSTD:       return "zstd";
        case OROCHI_COMPRESS_PGLZ:       return "pglz";
        case OROCHI_COMPRESS_DELTA:      return "delta";
        case OROCHI_COMPRESS_GORILLA:    return "gorilla";
        case OROCHI_COMPRESS_DICTIONARY: return "dictionary";
        case OROCHI_COMPRESS_RLE:        return "rle";
        default:                         return "unknown";
    }
}

OrochiCompressionType
columnar_parse_compression(const char *name)
{
    if (pg_strcasecmp(name, "none") == 0)       return OROCHI_COMPRESS_NONE;
    if (pg_strcasecmp(name, "lz4") == 0)        return OROCHI_COMPRESS_LZ4;
    if (pg_strcasecmp(name, "zstd") == 0)       return OROCHI_COMPRESS_ZSTD;
    if (pg_strcasecmp(name, "pglz") == 0)       return OROCHI_COMPRESS_PGLZ;
    if (pg_strcasecmp(name, "delta") == 0)      return OROCHI_COMPRESS_DELTA;
    if (pg_strcasecmp(name, "gorilla") == 0)    return OROCHI_COMPRESS_GORILLA;
    if (pg_strcasecmp(name, "dictionary") == 0) return OROCHI_COMPRESS_DICTIONARY;
    if (pg_strcasecmp(name, "rle") == 0)        return OROCHI_COMPRESS_RLE;

    elog(ERROR, "Unknown compression type: %s", name);
    return OROCHI_COMPRESS_NONE;  /* Keep compiler happy */
}
