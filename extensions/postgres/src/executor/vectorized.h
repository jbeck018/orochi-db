/*-------------------------------------------------------------------------
 *
 * vectorized.h
 *    Orochi DB vectorized query execution engine
 *
 * The vectorized execution engine provides:
 *   - SIMD-accelerated operations on column batches
 *   - Selection vector for predicate evaluation
 *   - Vectorized aggregations (SUM, COUNT, AVG, MIN, MAX)
 *   - Integration with columnar storage
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_VECTORIZED_H
#define OROCHI_VECTORIZED_H

#include "access/tupdesc.h"
#include "nodes/execnodes.h"
#include "postgres.h"

#include "../storage/columnar.h"

/* ============================================================
 * SIMD Configuration
 * ============================================================ */

/* AVX2 requires 32-byte alignment */
#define VECTORIZED_ALIGNMENT 32

/*
 * IMPORTANT: Do NOT use a simple macro for aligned allocation.
 * Aligned allocation requires storing the original pointer for proper freeing.
 * Always use the function vectorized_palloc_aligned() which handles this
 * correctly.
 */

/* Macro for portable aligned allocation using palloc */
#define VECTORIZED_PALLOC_ALIGNED(size) vectorized_palloc_aligned(size, VECTORIZED_ALIGNMENT)

/* Check if pointer is properly aligned */
#define VECTORIZED_IS_ALIGNED(ptr) (((uintptr_t)(ptr) & (VECTORIZED_ALIGNMENT - 1)) == 0)

/* Default batch size - matches columnar storage */
#define VECTORIZED_BATCH_SIZE COLUMNAR_VECTOR_BATCH_SIZE /* 1024 */

/* Maximum columns in a batch */
#define VECTORIZED_MAX_COLUMNS 256

/* SIMD lane counts */
#define VECTORIZED_INT64_LANES   4 /* AVX2: 256 bits / 64 bits */
#define VECTORIZED_INT32_LANES   8 /* AVX2: 256 bits / 32 bits */
#define VECTORIZED_FLOAT64_LANES 4 /* AVX2: 256 bits / 64 bits */
#define VECTORIZED_FLOAT32_LANES 8 /* AVX2: 256 bits / 32 bits */

/* ============================================================
 * Vectorized Operator Types
 * ============================================================ */

typedef enum VectorizedOperatorType {
    VECTORIZED_OP_SCAN,     /* Columnar table scan */
    VECTORIZED_OP_FILTER,   /* Predicate evaluation */
    VECTORIZED_OP_PROJECT,  /* Column projection */
    VECTORIZED_OP_AGG,      /* Aggregation */
    VECTORIZED_OP_JOIN,     /* Hash join */
    VECTORIZED_OP_SORT,     /* Sorting */
    VECTORIZED_OP_LIMIT,    /* Limit/offset */
    VECTORIZED_OP_DISTINCT, /* Distinct elimination */
    VECTORIZED_OP_GROUPBY   /* Group by */
} VectorizedOperatorType;

/* ============================================================
 * Comparison Operators for Filters
 * ============================================================ */

typedef enum VectorizedCompareOp {
    VECTORIZED_CMP_EQ,         /* Equal */
    VECTORIZED_CMP_NE,         /* Not equal */
    VECTORIZED_CMP_LT,         /* Less than */
    VECTORIZED_CMP_LE,         /* Less than or equal */
    VECTORIZED_CMP_GT,         /* Greater than */
    VECTORIZED_CMP_GE,         /* Greater than or equal */
    VECTORIZED_CMP_IS_NULL,    /* IS NULL */
    VECTORIZED_CMP_IS_NOT_NULL /* IS NOT NULL */
} VectorizedCompareOp;

/* ============================================================
 * Aggregation Types
 * ============================================================ */

typedef enum VectorizedAggType {
    VECTORIZED_AGG_COUNT, /* COUNT(*) or COUNT(col) */
    VECTORIZED_AGG_SUM,   /* SUM(col) */
    VECTORIZED_AGG_AVG,   /* AVG(col) */
    VECTORIZED_AGG_MIN,   /* MIN(col) */
    VECTORIZED_AGG_MAX    /* MAX(col) */
} VectorizedAggType;

/* ============================================================
 * Column Data Types for Vectorized Operations
 * ============================================================ */

typedef enum VectorizedDataType {
    VECTORIZED_TYPE_INT32,
    VECTORIZED_TYPE_INT64,
    VECTORIZED_TYPE_FLOAT32,
    VECTORIZED_TYPE_FLOAT64,
    VECTORIZED_TYPE_BOOL,
    VECTORIZED_TYPE_STRING,    /* Variable-length strings */
    VECTORIZED_TYPE_TIMESTAMP, /* Stored as int64 microseconds */
    VECTORIZED_TYPE_DATE,      /* Stored as int32 days */
    VECTORIZED_TYPE_UNKNOWN
} VectorizedDataType;

/* ============================================================
 * Vector Batch Structure
 * ============================================================ */

/*
 * VectorColumn - A single column in a batch
 */
typedef struct VectorColumn {
    VectorizedDataType data_type; /* Column data type */
    int32 type_len;               /* Type length (-1 for variable) */
    bool type_byval;              /* Pass by value? */
    Oid type_oid;                 /* PostgreSQL type OID */

    /* Data buffer (aligned for SIMD) */
    void *data;     /* Column data array */
    uint64 *nulls;  /* Null bitmap (1 bit per row) */
    bool has_nulls; /* Fast check for any nulls */

    /* For variable-length data */
    int32 *offsets;      /* Offsets into data buffer */
    int64 data_capacity; /* Capacity of data buffer */
} VectorColumn;

/*
 * VectorBatch - A batch of columnar data for vectorized processing
 *
 * The selection vector indicates which rows are "active" after filtering.
 * Operations only process rows where selection[i] is set.
 */
typedef struct VectorBatch {
    int32 capacity;     /* Maximum rows in batch */
    int32 count;        /* Current number of rows */
    int32 column_count; /* Number of columns */

    /* Column arrays (aligned for SIMD) */
    VectorColumn **columns; /* Array of column pointers */

    /* Selection vector - indices of active rows after filtering */
    uint32 *selection;    /* Selected row indices */
    int32 selected_count; /* Number of selected rows */
    bool has_selection;   /* Is selection vector active? */

    /* Memory management */
    MemoryContext batch_context; /* Memory context for batch data */

    /* Statistics */
    int64 rows_processed; /* Total rows processed */
    int64 rows_filtered;  /* Rows eliminated by filters */
} VectorBatch;

/* ============================================================
 * Vectorized Filter State
 * ============================================================ */

typedef struct VectorizedFilterState {
    int32 column_index;             /* Column to filter */
    VectorizedCompareOp compare_op; /* Comparison operator */
    VectorizedDataType data_type;   /* Data type of column */

    /* Constant value for comparison */
    Datum const_value;
    bool const_is_null;

    /* For BETWEEN operations */
    Datum const_value2;
} VectorizedFilterState;

/* ============================================================
 * Vectorized Aggregation State
 * ============================================================ */

typedef struct VectorizedAggState {
    VectorizedAggType agg_type;   /* Aggregation type */
    int32 column_index;           /* Column to aggregate (-1 for COUNT(*)) */
    VectorizedDataType data_type; /* Data type of column */

    /* Running aggregation state */
    int64 count;        /* Row count */
    int64 sum_int64;    /* Sum for integers */
    double sum_float64; /* Sum for floats */
    int64 min_int64;    /* Minimum integer */
    int64 max_int64;    /* Maximum integer */
    double min_float64; /* Minimum float */
    double max_float64; /* Maximum float */
    bool has_value;     /* Has seen any non-null value */
} VectorizedAggState;

/* ============================================================
 * Vectorized Scan State (for columnar tables)
 * ============================================================ */

typedef struct VectorizedScanState {
    /* Source columnar read state */
    ColumnarReadState *columnar_state;
    Relation relation;
    TupleDesc tupdesc;

    /* Column projection */
    Bitmapset *columns_needed;
    int32 projection_count;
    int32 *projection_map; /* Maps batch columns to table columns */

    /* Current batch */
    VectorBatch *current_batch;

    /* Predicate pushdown */
    List *predicates;
    VectorizedFilterState **filters;
    int32 filter_count;

    /* Statistics */
    int64 batches_read;
    int64 rows_scanned;
    int64 chunks_skipped;
} VectorizedScanState;

/* ============================================================
 * Batch Creation and Management
 * ============================================================ */

/*
 * Create a new vector batch with specified capacity
 */
extern VectorBatch *vector_batch_create(int32 capacity, int32 column_count);

/*
 * Free a vector batch and all associated memory
 */
extern void vector_batch_free(VectorBatch *batch);

/*
 * Reset batch for reuse (keeps allocated memory)
 */
extern void vector_batch_reset(VectorBatch *batch);

/*
 * Add a column to the batch
 */
extern VectorColumn *vector_batch_add_column(VectorBatch *batch, int32 column_index,
                                             VectorizedDataType data_type, Oid type_oid);

/*
 * Allocate aligned memory for vectorized operations
 */
extern void *vectorized_palloc_aligned(Size size, Size alignment);

/*
 * Free aligned memory allocated by vectorized_palloc_aligned
 */
extern void vectorized_pfree_aligned(void *aligned_ptr);

/*
 * Convert PostgreSQL type to vectorized data type
 */
extern VectorizedDataType vectorized_type_from_oid(Oid type_oid);

/* ============================================================
 * Vectorized Filter Operations (SIMD-accelerated)
 * ============================================================ */

/*
 * Filter int64 column using SIMD
 * Updates selection vector with matching rows
 */
extern void vectorized_filter_int64(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                    int64 value);

/*
 * Filter float64 column using SIMD
 */
extern void vectorized_filter_float64(VectorBatch *batch, int32 column_index,
                                      VectorizedCompareOp op, double value);

/*
 * Filter int32 column using SIMD
 */
extern void vectorized_filter_int32(VectorBatch *batch, int32 column_index, VectorizedCompareOp op,
                                    int32 value);

/*
 * Filter float32 column using SIMD
 */
extern void vectorized_filter_float32(VectorBatch *batch, int32 column_index,
                                      VectorizedCompareOp op, float value);

/*
 * Apply generic filter with Datum comparison
 */
extern void vectorized_filter_generic(VectorBatch *batch, VectorizedFilterState *filter);

/*
 * Combine two selection vectors with AND
 */
extern void vectorized_selection_and(VectorBatch *batch, uint32 *other_selection,
                                     int32 other_count);

/*
 * Combine two selection vectors with OR
 */
extern void vectorized_selection_or(VectorBatch *batch, uint32 *other_selection, int32 other_count);

/* ============================================================
 * Vectorized Aggregation Operations (SIMD-accelerated)
 * ============================================================ */

/*
 * Sum int64 column using SIMD
 */
extern int64 vectorized_sum_int64(VectorBatch *batch, int32 column_index);

/*
 * Sum float64 column using SIMD
 */
extern double vectorized_sum_float64(VectorBatch *batch, int32 column_index);

/*
 * Sum int32 column using SIMD (returns int64 to prevent overflow)
 */
extern int64 vectorized_sum_int32(VectorBatch *batch, int32 column_index);

/*
 * Sum float32 column using SIMD (returns double for precision)
 */
extern double vectorized_sum_float32(VectorBatch *batch, int32 column_index);

/*
 * Count rows with selection vector
 */
extern int64 vectorized_count(VectorBatch *batch);

/*
 * Count non-null values in column
 */
extern int64 vectorized_count_column(VectorBatch *batch, int32 column_index);

/*
 * Find minimum int64 value
 */
extern int64 vectorized_min_int64(VectorBatch *batch, int32 column_index, bool *found);

/*
 * Find maximum int64 value
 */
extern int64 vectorized_max_int64(VectorBatch *batch, int32 column_index, bool *found);

/*
 * Find minimum float64 value
 */
extern double vectorized_min_float64(VectorBatch *batch, int32 column_index, bool *found);

/*
 * Find maximum float64 value
 */
extern double vectorized_max_float64(VectorBatch *batch, int32 column_index, bool *found);

/*
 * Update running aggregation state with batch
 */
extern void vectorized_agg_update(VectorizedAggState *state, VectorBatch *batch);

/*
 * Finalize aggregation and return result
 */
extern Datum vectorized_agg_finalize(VectorizedAggState *state, bool *is_null);

/* ============================================================
 * Vectorized Scan Operations
 * ============================================================ */

/*
 * Begin vectorized columnar scan
 */
extern VectorizedScanState *vectorized_columnar_scan_begin(Relation relation, Snapshot snapshot,
                                                           Bitmapset *columns_needed);

/*
 * Read next batch from columnar storage
 */
extern VectorBatch *vectorized_columnar_scan(VectorizedScanState *state);

/*
 * Apply predicates to batch (predicate pushdown)
 */
extern void vectorized_apply_predicate(VectorizedScanState *state, VectorBatch *batch);

/*
 * Add filter predicate to scan
 */
extern void vectorized_scan_add_filter(VectorizedScanState *state, int32 column_index,
                                       VectorizedCompareOp op, Datum value,
                                       VectorizedDataType data_type);

/*
 * End vectorized scan
 */
extern void vectorized_columnar_scan_end(VectorizedScanState *state);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Materialize batch to tuple table slots
 */
extern int vectorized_batch_to_slots(VectorBatch *batch, TupleTableSlot **slots, TupleDesc tupdesc,
                                     int max_slots);

/*
 * Get string representation of data type
 */
extern const char *vectorized_type_name(VectorizedDataType type);

/*
 * Get string representation of comparison operator
 */
extern const char *vectorized_compare_op_name(VectorizedCompareOp op);

/*
 * Get string representation of aggregation type
 */
extern const char *vectorized_agg_type_name(VectorizedAggType type);

/*
 * Check if CPU supports AVX2
 */
extern bool vectorized_cpu_supports_avx2(void);

/*
 * Print batch statistics for debugging
 */
extern void vectorized_batch_debug_print(VectorBatch *batch);

/*
 * Load raw decompressed data into a vector column
 * Dispatches to SIMD implementation when available
 */
extern void vectorized_load_column_data(VectorColumn *column, const void *src_data, int32 row_count,
                                        const uint8 *null_bitmap);

/* ============================================================
 * JIT Integration
 *
 * These functions provide JIT-accelerated paths for vectorized
 * execution when JIT compilation is enabled.
 * ============================================================ */

/* Forward declarations for JIT types */
struct JitCompiledFilter;
struct JitCompiledAgg;
struct JitContext;

/*
 * Execute filter with optional JIT acceleration
 * Falls back to interpreted execution if JIT unavailable or disabled
 */
extern void vectorized_filter_with_jit(VectorBatch *batch, VectorizedFilterState *filter,
                                       struct JitCompiledFilter *jit_filter);

/*
 * Execute aggregation with optional JIT acceleration
 * Falls back to interpreted execution if JIT unavailable or disabled
 */
extern void vectorized_aggregate_with_jit(VectorizedAggState *state, VectorBatch *batch,
                                          struct JitCompiledAgg *jit_agg);

/*
 * Check if JIT is beneficial for this batch size and expression cost
 */
extern bool vectorized_should_use_jit(VectorBatch *batch, int32 expr_cost);

/*
 * Get the global JIT context (creates if not exists)
 */
extern struct JitContext *vectorized_get_jit_context(void);

/*
 * Compile a filter for JIT execution
 * Returns NULL if JIT is disabled or compilation fails
 */
extern struct JitCompiledFilter *vectorized_compile_filter_jit(VectorizedFilterState *filter);

/*
 * Compile an aggregation for JIT execution
 * Returns NULL if JIT is disabled or compilation fails
 */
extern struct JitCompiledAgg *vectorized_compile_agg_jit(VectorizedAggState *agg);

/*
 * Free JIT-compiled filter
 */
extern void vectorized_free_filter_jit(struct JitCompiledFilter *jit_filter);

/*
 * Free JIT-compiled aggregation
 */
extern void vectorized_free_agg_jit(struct JitCompiledAgg *jit_agg);

#endif /* OROCHI_VECTORIZED_H */
