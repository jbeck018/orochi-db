/*-------------------------------------------------------------------------
 *
 * vectorized_scan.c
 *    Orochi DB vectorized columnar scan implementation
 *
 * This module provides vectorized scanning of columnar storage:
 *   - Efficient batch reading from columnar chunks
 *   - Predicate pushdown with skip list integration
 *   - SIMD-accelerated filter application
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* PG18 removed some operator OID macros - define them using known OIDs */
#if PG_VERSION_NUM >= 180000
#ifndef Int4EqualOperator
#define Int4EqualOperator 96
#endif
#ifndef Int8EqualOperator
#define Int8EqualOperator 410
#endif
#ifndef Float8EqualOperator
#define Float8EqualOperator 670
#endif
#ifndef Int4LessOperator
#define Int4LessOperator 97
#endif
#ifndef Int8LessOperator
#define Int8LessOperator 412
#endif
#ifndef Int84LEOperator
#define Int84LEOperator 542
#endif
#ifndef Int4GreaterOperator
#define Int4GreaterOperator 521
#endif
#ifndef Int8GreaterOperator
#define Int8GreaterOperator 413
#endif
#ifndef Int84GEOperator
#define Int84GEOperator 544
#endif
#endif

#include <stdint.h> /* For uint8, int32, int64 types */

#include "../core/catalog.h"
#include "../storage/columnar.h"
#include "vectorized.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* Forward declarations */
static void load_batch_from_chunk_group(VectorizedScanState *state, VectorBatch *batch,
                                        ColumnarChunkGroup *chunk_group);
static VectorizedCompareOp pg_op_to_vectorized(Oid opno);
static bool can_pushdown_predicate(Node *node, int *column_index, VectorizedCompareOp *op,
                                   Datum *value, VectorizedDataType *data_type);

/* ============================================================
 * Vectorized Scan State Management
 * ============================================================ */

VectorizedScanState *vectorized_columnar_scan_begin(Relation relation, Snapshot snapshot,
                                                    Bitmapset *columns_needed)
{
    VectorizedScanState *state;
    TupleDesc tupdesc = RelationGetDescr(relation);
    MemoryContext old_context;
    int i, proj_idx;

    state = (VectorizedScanState *)palloc0(sizeof(VectorizedScanState));

    state->relation = relation;
    state->tupdesc = CreateTupleDescCopy(tupdesc);

    /* Start columnar read state */
    state->columnar_state = columnar_begin_read(relation, snapshot, columns_needed);

    /* Copy column projection */
    if (columns_needed != NULL && !bms_is_empty(columns_needed)) {
        state->columns_needed = bms_copy(columns_needed);
        state->projection_count = bms_num_members(columns_needed);
    } else {
        /* Need all columns */
        state->columns_needed = NULL;
        state->projection_count = tupdesc->natts;
        for (i = 0; i < tupdesc->natts; i++) {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            if (!attr->attisdropped) {
                state->columns_needed = bms_add_member(state->columns_needed, i);
            }
        }
    }

    /* Build projection map */
    state->projection_map = palloc(state->projection_count * sizeof(int32));
    proj_idx = 0;
    for (i = 0; i < tupdesc->natts; i++) {
        if (bms_is_member(i, state->columns_needed)) {
            state->projection_map[proj_idx++] = i;
        }
    }

    /* Create initial batch */
    state->current_batch = vector_batch_create(VECTORIZED_BATCH_SIZE, state->projection_count);

    /* Initialize columns in batch */
    for (i = 0; i < state->projection_count; i++) {
        int32 table_col = state->projection_map[i];
        Form_pg_attribute attr = TupleDescAttr(tupdesc, table_col);
        VectorizedDataType vtype = vectorized_type_from_oid(attr->atttypid);

        vector_batch_add_column(state->current_batch, i, vtype, attr->atttypid);
    }

    /* Initialize filter arrays */
    state->predicates = NIL;
    state->filters = NULL;
    state->filter_count = 0;

    /* Statistics */
    state->batches_read = 0;
    state->rows_scanned = 0;
    state->chunks_skipped = 0;

    return state;
}

void vectorized_scan_add_filter(VectorizedScanState *state, int32 column_index,
                                VectorizedCompareOp op, Datum value, VectorizedDataType data_type)
{
    VectorizedFilterState *filter;
    int32 batch_column = -1;
    int i;

    /* Find the column in the batch projection */
    for (i = 0; i < state->projection_count; i++) {
        if (state->projection_map[i] == column_index) {
            batch_column = i;
            break;
        }
    }

    if (batch_column < 0) {
        elog(WARNING, "Filter column %d not in projection, ignoring", column_index);
        return;
    }

    filter = palloc0(sizeof(VectorizedFilterState));
    filter->column_index = batch_column;
    filter->compare_op = op;
    filter->data_type = data_type;
    filter->const_value = value;
    filter->const_is_null = false;

    /* Add to filter array */
    if (state->filters == NULL) {
        state->filters = palloc(sizeof(VectorizedFilterState *));
    } else {
        state->filters =
            repalloc(state->filters, (state->filter_count + 1) * sizeof(VectorizedFilterState *));
    }

    state->filters[state->filter_count++] = filter;
}

void vectorized_columnar_scan_end(VectorizedScanState *state)
{
    int i;

    if (state == NULL)
        return;

    /* Log statistics */
    elog(DEBUG1, "Vectorized scan complete: batches=%ld rows=%ld chunks_skipped=%ld",
         state->batches_read, state->rows_scanned, state->chunks_skipped);

    /* Free filters */
    if (state->filters != NULL) {
        for (i = 0; i < state->filter_count; i++) {
            if (state->filters[i] != NULL)
                pfree(state->filters[i]);
        }
        pfree(state->filters);
    }

    /* Free batch */
    if (state->current_batch != NULL)
        vector_batch_free(state->current_batch);

    /* End columnar read */
    if (state->columnar_state != NULL)
        columnar_end_read(state->columnar_state);

    /* Free projection map */
    if (state->projection_map != NULL)
        pfree(state->projection_map);

    /* Free columns needed */
    if (state->columns_needed != NULL)
        bms_free(state->columns_needed);

    /* Free tuple descriptor */
    if (state->tupdesc != NULL)
        FreeTupleDesc(state->tupdesc);

    pfree(state);
}

/* ============================================================
 * Batch Loading from Columnar Storage
 * ============================================================ */

/*
 * Load data from a chunk group into the vector batch
 */
static void load_batch_from_chunk_group(VectorizedScanState *state, VectorBatch *batch,
                                        ColumnarChunkGroup *chunk_group)
{
    int32 row_count;
    int i;

    if (chunk_group == NULL)
        return;

    row_count = (int32)Min(chunk_group->row_count, batch->capacity);
    batch->count = row_count;

    /* Load each projected column */
    for (i = 0; i < state->projection_count; i++) {
        int32 table_col = state->projection_map[i];
        VectorColumn *vcol = batch->columns[i];
        ColumnarColumnChunk *ccol = NULL;

        /* Find the column chunk */
        if (chunk_group->columns != NULL && table_col < chunk_group->column_count) {
            ccol = chunk_group->columns[table_col];
        }

        if (ccol == NULL) {
            /* No data for this column - set all to NULL */
            vcol->has_nulls = true;
            memset(vcol->nulls, 0xFF, ((row_count + 63) / 64) * sizeof(uint64));
            continue;
        }

        /* Copy data based on type */
        switch (vcol->data_type) {
        case VECTORIZED_TYPE_INT64:
        case VECTORIZED_TYPE_TIMESTAMP: {
            int64 *dest = (int64 *)vcol->data;

            /*
             * Read decompressed values from the columnar read state.
             * The columnar_state buffers are populated by load_chunk_group()
             * which reads compressed data from catalog storage and decompresses
             * using LZ4/ZSTD via columnar_decompress_buffer().
             */
            ColumnarReadState *rs = state->columnar_state;
            if (rs->column_buffers[table_col].is_loaded) {
                Datum *src = rs->column_buffers[table_col].values;
                bool *src_nulls = rs->column_buffers[table_col].nulls;

                for (int j = 0; j < row_count; j++) {
                    if (src_nulls[j]) {
                        vcol->nulls[j / 64] |= (1ULL << (j % 64));
                        vcol->has_nulls = true;
                    } else {
                        dest[j] = DatumGetInt64(src[j]);
                    }
                }
            }
        } break;

        case VECTORIZED_TYPE_INT32:
        case VECTORIZED_TYPE_DATE: {
            int32 *dest = (int32 *)vcol->data;
            ColumnarReadState *rs = state->columnar_state;

            if (rs->column_buffers[table_col].is_loaded) {
                Datum *src = rs->column_buffers[table_col].values;
                bool *src_nulls = rs->column_buffers[table_col].nulls;

                for (int j = 0; j < row_count; j++) {
                    if (src_nulls[j]) {
                        vcol->nulls[j / 64] |= (1ULL << (j % 64));
                        vcol->has_nulls = true;
                    } else {
                        dest[j] = DatumGetInt32(src[j]);
                    }
                }
            }
        } break;

        case VECTORIZED_TYPE_FLOAT64: {
            double *dest = (double *)vcol->data;
            ColumnarReadState *rs = state->columnar_state;

            if (rs->column_buffers[table_col].is_loaded) {
                Datum *src = rs->column_buffers[table_col].values;
                bool *src_nulls = rs->column_buffers[table_col].nulls;

                for (int j = 0; j < row_count; j++) {
                    if (src_nulls[j]) {
                        vcol->nulls[j / 64] |= (1ULL << (j % 64));
                        vcol->has_nulls = true;
                    } else {
                        dest[j] = DatumGetFloat8(src[j]);
                    }
                }
            }
        } break;

        case VECTORIZED_TYPE_FLOAT32: {
            float *dest = (float *)vcol->data;
            ColumnarReadState *rs = state->columnar_state;

            if (rs->column_buffers[table_col].is_loaded) {
                Datum *src = rs->column_buffers[table_col].values;
                bool *src_nulls = rs->column_buffers[table_col].nulls;

                for (int j = 0; j < row_count; j++) {
                    if (src_nulls[j]) {
                        vcol->nulls[j / 64] |= (1ULL << (j % 64));
                        vcol->has_nulls = true;
                    } else {
                        dest[j] = DatumGetFloat4(src[j]);
                    }
                }
            }
        } break;

        case VECTORIZED_TYPE_BOOL: {
            bool *dest = (bool *)vcol->data;
            ColumnarReadState *rs = state->columnar_state;

            if (rs->column_buffers[table_col].is_loaded) {
                Datum *src = rs->column_buffers[table_col].values;
                bool *src_nulls = rs->column_buffers[table_col].nulls;

                for (int j = 0; j < row_count; j++) {
                    if (src_nulls[j]) {
                        vcol->nulls[j / 64] |= (1ULL << (j % 64));
                        vcol->has_nulls = true;
                    } else {
                        dest[j] = DatumGetBool(src[j]);
                    }
                }
            }
        } break;

        default:
            /* For other types, store Datum values directly */
            {
                Datum *dest = (Datum *)vcol->data;
                ColumnarReadState *rs = state->columnar_state;

                if (rs->column_buffers[table_col].is_loaded) {
                    Datum *src = rs->column_buffers[table_col].values;
                    bool *src_nulls = rs->column_buffers[table_col].nulls;

                    for (int j = 0; j < row_count; j++) {
                        if (src_nulls[j]) {
                            vcol->nulls[j / 64] |= (1ULL << (j % 64));
                            vcol->has_nulls = true;
                        } else {
                            dest[j] = src[j];
                        }
                    }
                }
            }
            break;
        }

        /* Handle null bitmap from column chunk */
        if (ccol->has_nulls && ccol->null_buffer != NULL) {
            vcol->has_nulls = true;
            /* Copy null bitmap */
            int null_bytes = (row_count + 7) / 8;
            for (int j = 0; j < null_bytes; j++) {
                uint8 null_byte = ((uint8 *)ccol->null_buffer)[j];
                for (int k = 0; k < 8 && (j * 8 + k) < row_count; k++) {
                    if (null_byte & (1 << k)) {
                        int idx = j * 8 + k;
                        vcol->nulls[idx / 64] |= (1ULL << (idx % 64));
                    }
                }
            }
        }
    }

    /* Reset selection vector */
    batch->has_selection = false;
    batch->selected_count = 0;
}

/*
 * Read next batch of rows from columnar storage
 * Returns NULL when no more data is available
 */
VectorBatch *vectorized_columnar_scan(VectorizedScanState *state)
{
    ColumnarReadState *columnar = state->columnar_state;
    VectorBatch *batch = state->current_batch;
    int rows_loaded = 0;

    if (state == NULL || columnar == NULL)
        return NULL;

    /* Reset the batch for new data */
    vector_batch_reset(batch);

    /* Try to fill the batch with up to VECTORIZED_BATCH_SIZE rows */
    while (rows_loaded < VECTORIZED_BATCH_SIZE) {
        /* Check if we need to load a new chunk group */
        if (columnar->current_chunk_group == NULL ||
            columnar->current_row_in_chunk >= columnar->current_chunk_group->row_count) {
            /* Move to next chunk group */
            columnar->current_chunk_group_index++;

            /* Check if we need a new stripe */
            if (columnar->current_stripe == NULL ||
                columnar->current_chunk_group_index >=
                    columnar->current_stripe->chunk_group_count) {
                columnar->current_stripe_index++;

                if (columnar->current_stripe_index >= list_length(columnar->stripe_list)) {
                    /* No more data */
                    break;
                }

                columnar->current_stripe =
                    list_nth(columnar->stripe_list, columnar->current_stripe_index);
                columnar->current_chunk_group_index = 0;
                columnar->stripes_scanned++;

                /* Reset stripe context for new stripe */
                MemoryContextReset(columnar->stripe_context);
            }

            /*
             * Check skip list to see if we can skip this chunk group.
             * This implements predicate pushdown at the chunk level.
             */
            if (columnar->skip_list != NULL && state->predicates != NIL) {
                int64 stripe_id = columnar->current_stripe->stripe_id;
                int32 cg_idx = columnar->current_chunk_group_index;
                bool can_skip = true;

                /* Check each skip list entry for this chunk group */
                for (int i = 0; i < columnar->skip_list->entry_count; i++) {
                    ColumnarSkipListEntry *entry = &columnar->skip_list->entries[i];

                    if (entry->stripe_id == stripe_id && entry->chunk_group_index == cg_idx) {
                        if (!columnar_can_skip_chunk(entry, state->predicates)) {
                            can_skip = false;
                            break;
                        }
                    }
                }

                if (can_skip) {
                    state->chunks_skipped++;
                    continue; /* Skip to next chunk group */
                }
            }

            /* Load the chunk group data */
            /* This triggers decompression and populates column_buffers */
            {
                MemoryContext old_context;
                List *column_chunks;
                ListCell *lc;

                old_context = MemoryContextSwitchTo(columnar->stripe_context);

                /* Get column chunk metadata from catalog */
                column_chunks =
                    orochi_catalog_get_stripe_columns(columnar->current_stripe->stripe_id);

                /* Allocate chunk group structure */
                columnar->current_chunk_group = palloc0(sizeof(ColumnarChunkGroup));
                columnar->current_chunk_group->chunk_group_index =
                    columnar->current_chunk_group_index;
                columnar->current_chunk_group->column_count = state->tupdesc->natts;
                columnar->current_chunk_group->row_count = columnar->current_stripe->row_count;
                columnar->current_chunk_group->columns =
                    palloc0(state->tupdesc->natts * sizeof(ColumnarColumnChunk *));

                /* Process column chunks */
                foreach (lc, column_chunks) {
                    OrochiColumnChunk *cc = (OrochiColumnChunk *)lfirst(lc);

                    if (cc->column_index < state->tupdesc->natts) {
                        ColumnarColumnChunk *chunk = palloc0(sizeof(ColumnarColumnChunk));
                        chunk->column_index = cc->column_index;
                        chunk->column_type = cc->data_type;
                        chunk->value_count = cc->value_count;
                        chunk->has_nulls = cc->has_nulls;
                        chunk->compressed_size = cc->compressed_size;
                        chunk->decompressed_size = cc->uncompressed_size;
                        chunk->compression_type = cc->compression_type;

                        /* Copy min/max for predicate evaluation */
                        chunk->has_min_max = true;
                        chunk->min_value = cc->min_value;
                        chunk->max_value = cc->max_value;

                        columnar->current_chunk_group->columns[cc->column_index] = chunk;
                    }
                }

                /* Populate column read buffers with decompressed data */
                for (int col_idx = 0; col_idx < state->tupdesc->natts; col_idx++) {
                    ColumnarColumnChunk *chunk;
                    Form_pg_attribute attr;
                    char *compressed_data;
                    int64 compressed_size;

                    if (!bms_is_member(col_idx, state->columns_needed))
                        continue;

                    struct ColumnReadBuffer *col_buf = &columnar->column_buffers[col_idx];
                    int64 row_count = columnar->current_chunk_group->row_count;

                    /* Allocate value and null arrays */
                    col_buf->values = palloc0(row_count * sizeof(Datum));
                    col_buf->nulls = palloc0(row_count * sizeof(bool));
                    col_buf->row_count = row_count;
                    col_buf->is_loaded = true;

                    /* Get the column chunk metadata and attribute info */
                    chunk = columnar->current_chunk_group->columns[col_idx];
                    attr = TupleDescAttr(state->tupdesc, col_idx);

                    if (chunk == NULL) {
                        /* No chunk metadata, mark all as null */
                        memset(col_buf->nulls, true, row_count * sizeof(bool));
                        continue;
                    }

                    /* Read compressed data from catalog storage */
                    compressed_data = orochi_catalog_read_chunk_data(
                        columnar->current_stripe->stripe_id, columnar->current_chunk_group_index,
                        col_idx, &compressed_size);

                    if (compressed_data != NULL && compressed_size > 0) {
                        char *decompressed_buffer;
                        int64 decompressed_size = chunk->decompressed_size;
                        OrochiCompressionType compression_type = chunk->compression_type;

                        if (decompressed_size <= 0)
                            decompressed_size = compressed_size;

                        decompressed_buffer = palloc(decompressed_size);

                        /* Decompress the data */
                        if (compression_type != OROCHI_COMPRESS_NONE) {
                            int64 actual_size = columnar_decompress_buffer(
                                compressed_data, compressed_size, decompressed_buffer,
                                decompressed_size, compression_type);

                            if (actual_size > 0 && actual_size != decompressed_size)
                                decompressed_size = actual_size;
                        } else {
                            memcpy(decompressed_buffer, compressed_data, compressed_size);
                            decompressed_size = compressed_size;
                        }

                        /* Decode values based on attribute type */
                        if (attr->attbyval && attr->attlen > 0) {
                            /* Fixed-width pass-by-value type */
                            for (int64 j = 0; j < row_count && j * attr->attlen < decompressed_size;
                                 j++) {
                                memcpy(&col_buf->values[j],
                                       decompressed_buffer + (j * attr->attlen), attr->attlen);
                                col_buf->nulls[j] = false;
                            }
                        } else {
                            /* Variable-length type with size prefix */
                            int64 offset = 0;
                            int64 j = 0;

                            while (offset < decompressed_size && j < row_count) {
                                Size value_size;

                                if (offset + (int64)sizeof(Size) > decompressed_size)
                                    break;

                                memcpy(&value_size, decompressed_buffer + offset, sizeof(Size));
                                offset += sizeof(Size);

                                if (value_size == 0) {
                                    col_buf->nulls[j] = true;
                                    col_buf->values[j] = (Datum)0;
                                } else {
                                    if (offset + (int64)value_size > decompressed_size)
                                        break;

                                    void *value_ptr = palloc(value_size);
                                    memcpy(value_ptr, decompressed_buffer + offset, value_size);
                                    col_buf->values[j] = PointerGetDatum(value_ptr);
                                    col_buf->nulls[j] = false;
                                    offset += value_size;
                                }
                                j++;
                            }

                            /* Mark remaining as null */
                            while (j < row_count) {
                                col_buf->nulls[j] = true;
                                col_buf->values[j] = (Datum)0;
                                j++;
                            }
                        }

                        pfree(decompressed_buffer);
                        pfree(compressed_data);
                    } else {
                        /* No data available, initialize based on metadata */
                        if (chunk->has_nulls) {
                            memset(col_buf->nulls, true, row_count * sizeof(bool));
                        } else {
                            memset(col_buf->nulls, false, row_count * sizeof(bool));
                            memset(col_buf->values, 0, row_count * sizeof(Datum));
                        }
                    }
                }

                list_free_deep(column_chunks);
                MemoryContextSwitchTo(old_context);
            }

            columnar->current_row_in_chunk = 0;
        }

        /* Calculate how many rows to load from current chunk group */
        int64 rows_available =
            columnar->current_chunk_group->row_count - columnar->current_row_in_chunk;
        int32 rows_to_load = (int32)Min(rows_available, VECTORIZED_BATCH_SIZE - rows_loaded);

        /* If we're at the start of a chunk group, load all available data */
        if (columnar->current_row_in_chunk == 0 && rows_to_load > 0) {
            load_batch_from_chunk_group(state, batch, columnar->current_chunk_group);
            rows_loaded = batch->count;
        }

        /* Advance position */
        columnar->current_row_in_chunk += rows_to_load;
        columnar->total_rows_read += rows_to_load;
    }

    if (rows_loaded == 0)
        return NULL;

    batch->count = rows_loaded;
    batch->rows_processed += rows_loaded;
    state->rows_scanned += rows_loaded;
    state->batches_read++;

    return batch;
}

/* ============================================================
 * Predicate Pushdown
 * ============================================================ */

/*
 * Convert PostgreSQL operator OID to vectorized comparison operator
 */
static VectorizedCompareOp pg_op_to_vectorized(Oid opno)
{
    switch (opno) {
    /* Integer equality */
    case Int4EqualOperator:
    case Int8EqualOperator:
    case Float8EqualOperator:
        return VECTORIZED_CMP_EQ;

    /* Integer less than */
    case Int4LessOperator:
    case Int8LessOperator:
        return VECTORIZED_CMP_LT;

    /* Integer less than or equal */
    case Int84LEOperator:
        return VECTORIZED_CMP_LE;

    /* Integer greater than */
    case Int4GreaterOperator:
    case Int8GreaterOperator:
        return VECTORIZED_CMP_GT;

    /* Integer greater than or equal */
    case Int84GEOperator:
        return VECTORIZED_CMP_GE;

    default:
        return VECTORIZED_CMP_EQ; /* Default fallback */
    }
}

/*
 * Check if a predicate can be pushed down to vectorized execution
 * Returns true if pushdown is possible, filling in the output parameters
 */
static bool can_pushdown_predicate(Node *node, int *column_index, VectorizedCompareOp *op,
                                   Datum *value, VectorizedDataType *data_type)
{
    if (!IsA(node, OpExpr))
        return false;

    OpExpr *opexpr = (OpExpr *)node;

    if (list_length(opexpr->args) != 2)
        return false;

    Node *left = (Node *)linitial(opexpr->args);
    Node *right = (Node *)lsecond(opexpr->args);

    Var *var = NULL;
    Const *constval = NULL;
    bool reversed = false;

    /* Identify variable and constant */
    if (IsA(left, Var) && IsA(right, Const)) {
        var = (Var *)left;
        constval = (Const *)right;
    } else if (IsA(left, Const) && IsA(right, Var)) {
        constval = (Const *)left;
        var = (Var *)right;
        reversed = true;
    } else {
        return false;
    }

    /* Don't push down if constant is null */
    if (constval->constisnull)
        return false;

    /* Get column index (varattno is 1-based) */
    *column_index = var->varattno - 1;

    /* Determine data type */
    *data_type = vectorized_type_from_oid(var->vartype);
    if (*data_type == VECTORIZED_TYPE_UNKNOWN)
        return false;

    /* Get operator */
    *op = pg_op_to_vectorized(opexpr->opno);

    /* Reverse comparison if needed */
    if (reversed) {
        switch (*op) {
        case VECTORIZED_CMP_LT:
            *op = VECTORIZED_CMP_GT;
            break;
        case VECTORIZED_CMP_LE:
            *op = VECTORIZED_CMP_GE;
            break;
        case VECTORIZED_CMP_GT:
            *op = VECTORIZED_CMP_LT;
            break;
        case VECTORIZED_CMP_GE:
            *op = VECTORIZED_CMP_LE;
            break;
        default:
            break;
        }
    }

    *value = constval->constvalue;

    return true;
}

/*
 * Apply predicates to a batch using vectorized filtering
 */
void vectorized_apply_predicate(VectorizedScanState *state, VectorBatch *batch)
{
    int i;

    if (state == NULL || batch == NULL)
        return;

    /* Apply each filter */
    for (i = 0; i < state->filter_count; i++) {
        VectorizedFilterState *filter = state->filters[i];

        if (filter == NULL)
            continue;

        vectorized_filter_generic(batch, filter);

        /* Early exit if nothing selected */
        if (batch->has_selection && batch->selected_count == 0)
            break;
    }

    /* Also try to apply any qual list predicates */
    if (state->predicates != NIL) {
        ListCell *lc;

        foreach (lc, state->predicates) {
            Node *pred = (Node *)lfirst(lc);
            int column_index;
            VectorizedCompareOp op;
            Datum value;
            VectorizedDataType data_type;

            if (can_pushdown_predicate(pred, &column_index, &op, &value, &data_type)) {
                /* Find column in projection */
                int batch_col = -1;
                for (int j = 0; j < state->projection_count; j++) {
                    if (state->projection_map[j] == column_index) {
                        batch_col = j;
                        break;
                    }
                }

                if (batch_col >= 0) {
                    switch (data_type) {
                    case VECTORIZED_TYPE_INT64:
                    case VECTORIZED_TYPE_TIMESTAMP:
                        vectorized_filter_int64(batch, batch_col, op, DatumGetInt64(value));
                        break;

                    case VECTORIZED_TYPE_INT32:
                    case VECTORIZED_TYPE_DATE:
                        vectorized_filter_int32(batch, batch_col, op, DatumGetInt32(value));
                        break;

                    case VECTORIZED_TYPE_FLOAT64:
                        vectorized_filter_float64(batch, batch_col, op, DatumGetFloat8(value));
                        break;

                    case VECTORIZED_TYPE_FLOAT32:
                        vectorized_filter_float32(batch, batch_col, op, DatumGetFloat4(value));
                        break;

                    default:
                        break;
                    }
                }
            }

            /* Early exit if nothing selected */
            if (batch->has_selection && batch->selected_count == 0)
                break;
        }
    }
}

/* ============================================================
 * Helper Functions for Direct Batch Loading
 * ============================================================ */

/*
 * Load int64 column data from a raw buffer (post-decompression)
 * Uses SIMD for faster data movement when available
 */
#ifdef __AVX2__
static void load_int64_column_avx2(int64 *dest, const int64 *src, int32 count,
                                   const uint8 *null_bitmap, uint64 *dest_nulls)
{
    int32 i;

    /* Process 4 int64 values at a time */
    for (i = 0; i + VECTORIZED_INT64_LANES <= count; i += VECTORIZED_INT64_LANES) {
        __m256i data = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dest + i), data);
    }

    /* Handle remaining */
    for (; i < count; i++) {
        dest[i] = src[i];
    }

    /* Handle null bitmap if present */
    if (null_bitmap != NULL && dest_nulls != NULL) {
        int null_bytes = (count + 7) / 8;
        for (int j = 0; j < null_bytes; j++) {
            uint8 byte = null_bitmap[j];
            for (int k = 0; k < 8 && (j * 8 + k) < count; k++) {
                if (byte & (1 << k)) {
                    int idx = j * 8 + k;
                    dest_nulls[idx / 64] |= (1ULL << (idx % 64));
                }
            }
        }
    }
}

/*
 * Load float64 column data from a raw buffer
 */
static void load_float64_column_avx2(double *dest, const double *src, int32 count,
                                     const uint8 *null_bitmap, uint64 *dest_nulls)
{
    int32 i;

    /* Process 4 double values at a time */
    for (i = 0; i + VECTORIZED_FLOAT64_LANES <= count; i += VECTORIZED_FLOAT64_LANES) {
        __m256d data = _mm256_loadu_pd(src + i);
        _mm256_storeu_pd(dest + i, data);
    }

    /* Handle remaining */
    for (; i < count; i++) {
        dest[i] = src[i];
    }

    /* Handle null bitmap */
    if (null_bitmap != NULL && dest_nulls != NULL) {
        int null_bytes = (count + 7) / 8;
        for (int j = 0; j < null_bytes; j++) {
            uint8 byte = null_bitmap[j];
            for (int k = 0; k < 8 && (j * 8 + k) < count; k++) {
                if (byte & (1 << k)) {
                    int idx = j * 8 + k;
                    dest_nulls[idx / 64] |= (1ULL << (idx % 64));
                }
            }
        }
    }
}

/*
 * Load int32 column data from a raw buffer
 */
static void load_int32_column_avx2(int32 *dest, const int32 *src, int32 count,
                                   const uint8 *null_bitmap, uint64 *dest_nulls)
{
    int32 i;

    /* Process 8 int32 values at a time */
    for (i = 0; i + VECTORIZED_INT32_LANES <= count; i += VECTORIZED_INT32_LANES) {
        __m256i data = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dest + i), data);
    }

    /* Handle remaining */
    for (; i < count; i++) {
        dest[i] = src[i];
    }

    /* Handle null bitmap */
    if (null_bitmap != NULL && dest_nulls != NULL) {
        int null_bytes = (count + 7) / 8;
        for (int j = 0; j < null_bytes; j++) {
            uint8 byte = null_bitmap[j];
            for (int k = 0; k < 8 && (j * 8 + k) < count; k++) {
                if (byte & (1 << k)) {
                    int idx = j * 8 + k;
                    dest_nulls[idx / 64] |= (1ULL << (idx % 64));
                }
            }
        }
    }
}
#endif /* __AVX2__ */

/*
 * Scalar fallback for loading column data
 */
static void load_column_scalar(void *dest, const void *src, int32 count, int32 elem_size,
                               const uint8 *null_bitmap, uint64 *dest_nulls)
{
    memcpy(dest, src, count * elem_size);

    if (null_bitmap != NULL && dest_nulls != NULL) {
        int null_bytes = (count + 7) / 8;
        for (int j = 0; j < null_bytes; j++) {
            uint8 byte = null_bitmap[j];
            for (int k = 0; k < 8 && (j * 8 + k) < count; k++) {
                if (byte & (1 << k)) {
                    int idx = j * 8 + k;
                    dest_nulls[idx / 64] |= (1ULL << (idx % 64));
                }
            }
        }
    }
}

/*
 * Load raw decompressed data into a vector column
 * Dispatches to SIMD implementation when available
 */
void vectorized_load_column_data(VectorColumn *column, const void *src_data, int32 row_count,
                                 const uint8 *null_bitmap)
{
    if (column == NULL || src_data == NULL || row_count <= 0)
        return;

#ifdef __AVX2__
    switch (column->data_type) {
    case VECTORIZED_TYPE_INT64:
    case VECTORIZED_TYPE_TIMESTAMP:
        load_int64_column_avx2((int64 *)column->data, (const int64 *)src_data, row_count,
                               null_bitmap, column->nulls);
        break;

    case VECTORIZED_TYPE_FLOAT64:
        load_float64_column_avx2((double *)column->data, (const double *)src_data, row_count,
                                 null_bitmap, column->nulls);
        break;

    case VECTORIZED_TYPE_INT32:
    case VECTORIZED_TYPE_DATE:
        load_int32_column_avx2((int32 *)column->data, (const int32 *)src_data, row_count,
                               null_bitmap, column->nulls);
        break;

    default:
        load_column_scalar(column->data, src_data, row_count,
                           column->type_len > 0 ? column->type_len : sizeof(Datum), null_bitmap,
                           column->nulls);
        break;
    }
#else
    load_column_scalar(column->data, src_data, row_count,
                       column->type_len > 0 ? column->type_len : sizeof(Datum), null_bitmap,
                       column->nulls);
#endif

    if (null_bitmap != NULL)
        column->has_nulls = true;
}
