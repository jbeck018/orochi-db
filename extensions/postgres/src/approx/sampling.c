/*-------------------------------------------------------------------------
 *
 * sampling.c
 *    Sampling operators for approximate query processing in Orochi DB
 *
 * This file implements sampling methods for fast approximate query results:
 *
 *   - Bernoulli Sampling: Each row has independent probability p of selection
 *   - Reservoir Sampling: Fixed-size uniform random sample (Algorithm R)
 *   - System Sampling: Block-level sampling for I/O efficiency
 *
 * Sampling enables trading accuracy for speed on large datasets,
 * useful for exploratory queries and approximate aggregations.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */


/* postgres.h must be included first */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/execnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define SAMPLING_DEFAULT_PROBABILITY    0.1 /* 10% sample */
#define SAMPLING_DEFAULT_RESERVOIR_SIZE 1000
#define SAMPLING_MAX_RESERVOIR_SIZE     1000000

/* ============================================================
 * Random Number Generation
 *
 * Uses a thread-safe PRNG for sampling decisions.
 * Implements xoshiro256++ for fast, high-quality randomness.
 * ============================================================ */

typedef struct SamplingRNGState {
    uint64 s[4]; /* RNG state */
    bool initialized;
} SamplingRNGState;

/* Global RNG state (per-backend) */
static SamplingRNGState sampling_rng = { .initialized = false };

/*
 * Rotate left operation for xoshiro256++
 */
static inline uint64 rotl(const uint64 x, int k)
{
    return (x << k) | (x >> (64 - k));
}

/*
 * Initialize RNG with seed.
 */
static void sampling_rng_init(uint64 seed)
{
    /* Use splitmix64 to generate initial state from seed */
    uint64 z = seed;

    for (int i = 0; i < 4; i++) {
        z += 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        sampling_rng.s[i] = z ^ (z >> 31);
    }

    sampling_rng.initialized = true;
}

/*
 * Generate next random 64-bit value (xoshiro256++).
 */
static uint64 sampling_rng_next(void)
{
    if (!sampling_rng.initialized) {
        /* Initialize with combination of time and pointer address for uniqueness */
        uint64 seed = (uint64)time(NULL) ^ (uint64)(uintptr_t)&sampling_rng;
        sampling_rng_init(seed);
    }

    const uint64 result = rotl(sampling_rng.s[0] + sampling_rng.s[3], 23) + sampling_rng.s[0];
    const uint64 t = sampling_rng.s[1] << 17;

    sampling_rng.s[2] ^= sampling_rng.s[0];
    sampling_rng.s[3] ^= sampling_rng.s[1];
    sampling_rng.s[1] ^= sampling_rng.s[2];
    sampling_rng.s[0] ^= sampling_rng.s[3];

    sampling_rng.s[2] ^= t;
    sampling_rng.s[3] = rotl(sampling_rng.s[3], 45);

    return result;
}

/*
 * Generate uniform random double in [0, 1).
 */
static double sampling_random_double(void)
{
    /* Use top 53 bits for double precision */
    return (sampling_rng_next() >> 11) * (1.0 / (1ULL << 53));
}

/*
 * Generate random integer in [0, n).
 */
static int64 sampling_random_int(int64 n)
{
    if (n <= 0)
        return 0;

    /* Rejection sampling for unbiased results */
    uint64 threshold = (UINT64_MAX - n + 1) % n;
    uint64 r;

    do {
        r = sampling_rng_next();
    } while (r < threshold);

    return r % n;
}

/* ============================================================
 * Bernoulli Sampling
 *
 * Each row is independently selected with probability p.
 * Simple and unbiased, but sample size varies.
 * ============================================================ */

/*
 * BernoulliSampleState - State for Bernoulli sampling scan.
 */
typedef struct BernoulliSampleState {
    double probability;  /* Selection probability [0,1] */
    int64 rows_seen;     /* Total rows examined */
    int64 rows_selected; /* Rows selected so far */
    uint64 seed;         /* Random seed (optional) */
} BernoulliSampleState;

/*
 * Create Bernoulli sampling state.
 */
BernoulliSampleState *bernoulli_sample_create(double probability, uint64 seed)
{
    BernoulliSampleState *state;

    if (probability <= 0.0 || probability > 1.0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("sampling probability must be between 0 and 1")));

    state = (BernoulliSampleState *)palloc0(sizeof(BernoulliSampleState));
    state->probability = probability;
    state->rows_seen = 0;
    state->rows_selected = 0;
    state->seed = seed;

    if (seed != 0)
        sampling_rng_init(seed);

    return state;
}

/*
 * Decide whether to select a row using Bernoulli sampling.
 */
bool bernoulli_sample_row(BernoulliSampleState *state)
{
    state->rows_seen++;

    if (sampling_random_double() < state->probability) {
        state->rows_selected++;
        return true;
    }

    return false;
}

/*
 * Get sampling statistics.
 */
void bernoulli_sample_stats(BernoulliSampleState *state, int64 *rows_seen, int64 *rows_selected,
                            double *actual_rate)
{
    if (rows_seen)
        *rows_seen = state->rows_seen;
    if (rows_selected)
        *rows_selected = state->rows_selected;
    if (actual_rate && state->rows_seen > 0)
        *actual_rate = (double)state->rows_selected / (double)state->rows_seen;
}

/* ============================================================
 * Reservoir Sampling (Algorithm R)
 *
 * Select exactly k items uniformly at random from a stream
 * of unknown length. Memory usage is O(k).
 * ============================================================ */

/*
 * ReservoirSampleItem - A sampled item with metadata.
 */
typedef struct ReservoirSampleItem {
    int32 num_values;                    /* Number of values stored */
    Datum values[FLEXIBLE_ARRAY_MEMBER]; /* Column values */
                                         /* bool nulls[] follows */
} ReservoirSampleItem;

/*
 * ReservoirSampleState - State for reservoir sampling.
 */
typedef struct ReservoirSampleState {
    int32 reservoir_size;         /* Maximum items to keep (k) */
    int32 num_columns;            /* Number of columns per row */
    int64 rows_seen;              /* Total rows examined (n) */
    MemoryContext sample_context; /* Memory context for samples */

    /* Array of sampled items */
    Datum *values_array; /* Flattened values: [item][col] */
    bool *nulls_array;   /* Flattened nulls: [item][col] */
    int32 items_stored;  /* Current items in reservoir */

    /* Type information for columns */
    Oid *column_types;
    int16 *column_typlen;
    bool *column_typbyval;
} ReservoirSampleState;

/*
 * Create reservoir sampling state.
 */
ReservoirSampleState *reservoir_sample_create(int32 reservoir_size, int32 num_columns,
                                              Oid *column_types)
{
    ReservoirSampleState *state;
    MemoryContext sample_context;
    MemoryContext old_context;

    if (reservoir_size <= 0)
        reservoir_size = SAMPLING_DEFAULT_RESERVOIR_SIZE;
    if (reservoir_size > SAMPLING_MAX_RESERVOIR_SIZE)
        reservoir_size = SAMPLING_MAX_RESERVOIR_SIZE;

    /* Create dedicated memory context */
    sample_context =
        AllocSetContextCreate(CurrentMemoryContext, "ReservoirSampling", ALLOCSET_DEFAULT_SIZES);
    old_context = MemoryContextSwitchTo(sample_context);

    state = (ReservoirSampleState *)palloc0(sizeof(ReservoirSampleState));
    state->reservoir_size = reservoir_size;
    state->num_columns = num_columns;
    state->rows_seen = 0;
    state->items_stored = 0;
    state->sample_context = sample_context;

    /* Allocate arrays */
    state->values_array = palloc0(reservoir_size * num_columns * sizeof(Datum));
    state->nulls_array = palloc0(reservoir_size * num_columns * sizeof(bool));

    /* Store column type info */
    state->column_types = palloc(num_columns * sizeof(Oid));
    state->column_typlen = palloc(num_columns * sizeof(int16));
    state->column_typbyval = palloc(num_columns * sizeof(bool));

    for (int i = 0; i < num_columns; i++) {
        state->column_types[i] = column_types[i];
        get_typlenbyval(column_types[i], &state->column_typlen[i], &state->column_typbyval[i]);
    }

    MemoryContextSwitchTo(old_context);

    return state;
}

/*
 * Add a row to the reservoir sample.
 * Uses Algorithm R for uniform sampling.
 */
void reservoir_sample_add(ReservoirSampleState *state, Datum *values, bool *nulls)
{
    MemoryContext old_context;
    int slot;

    state->rows_seen++;

    if (state->items_stored < state->reservoir_size) {
        /* Reservoir not full yet - always add */
        slot = state->items_stored;
        state->items_stored++;
    } else {
        /* Reservoir full - randomly replace with probability k/n */
        int64 j = sampling_random_int(state->rows_seen);

        if (j >= state->reservoir_size)
            return; /* Don't select this item */

        slot = (int)j;
    }

    /* Store the row in the selected slot */
    old_context = MemoryContextSwitchTo(state->sample_context);

    for (int i = 0; i < state->num_columns; i++) {
        int idx = slot * state->num_columns + i;

        /* Free old value if it's a pass-by-reference type */
        if (!state->nulls_array[idx] && !state->column_typbyval[i] &&
            state->values_array[idx] != 0) {
            pfree(DatumGetPointer(state->values_array[idx]));
        }

        state->nulls_array[idx] = nulls[i];

        if (!nulls[i]) {
            /* Copy the datum */
            state->values_array[idx] =
                datumCopy(values[i], state->column_typbyval[i], state->column_typlen[i]);
        } else {
            state->values_array[idx] = (Datum)0;
        }
    }

    MemoryContextSwitchTo(old_context);
}

/*
 * Get the number of items currently in the reservoir.
 */
int32 reservoir_sample_count(ReservoirSampleState *state)
{
    return state->items_stored;
}

/*
 * Get a sample row by index.
 */
bool reservoir_sample_get(ReservoirSampleState *state, int32 index, Datum *values, bool *nulls)
{
    if (index < 0 || index >= state->items_stored)
        return false;

    for (int i = 0; i < state->num_columns; i++) {
        int idx = index * state->num_columns + i;
        values[i] = state->values_array[idx];
        nulls[i] = state->nulls_array[idx];
    }

    return true;
}

/*
 * Free reservoir sampling state.
 */
void reservoir_sample_free(ReservoirSampleState *state)
{
    MemoryContextDelete(state->sample_context);
}

/* ============================================================
 * System Sampling (Block-level)
 *
 * Sample entire blocks/pages for I/O efficiency.
 * Less accurate than row-level sampling but faster.
 * ============================================================ */

typedef struct SystemSampleState {
    double probability; /* Block selection probability */
    int64 blocks_seen;
    int64 blocks_selected;
    int64 rows_returned;
} SystemSampleState;

/*
 * Create system sampling state.
 */
SystemSampleState *system_sample_create(double probability)
{
    SystemSampleState *state;

    state = (SystemSampleState *)palloc0(sizeof(SystemSampleState));
    state->probability = probability;
    state->blocks_seen = 0;
    state->blocks_selected = 0;
    state->rows_returned = 0;

    return state;
}

/*
 * Decide whether to sample a block.
 */
bool system_sample_block(SystemSampleState *state)
{
    state->blocks_seen++;

    if (sampling_random_double() < state->probability) {
        state->blocks_selected++;
        return true;
    }

    return false;
}

/*
 * Record that a row was returned from a sampled block.
 */
void system_sample_row_returned(SystemSampleState *state)
{
    state->rows_returned++;
}

/* ============================================================
 * PostgreSQL SQL Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(sampling_bernoulli);
PG_FUNCTION_INFO_V1(sampling_set_seed);
PG_FUNCTION_INFO_V1(sampling_random);

/*
 * SQL function: Test if a row should be selected using Bernoulli sampling.
 * sampling_bernoulli(probability float8) -> boolean
 */
Datum sampling_bernoulli(PG_FUNCTION_ARGS)
{
    double probability = PG_GETARG_FLOAT8(0);

    if (probability <= 0.0)
        PG_RETURN_BOOL(false);
    if (probability >= 1.0)
        PG_RETURN_BOOL(true);

    PG_RETURN_BOOL(sampling_random_double() < probability);
}

/*
 * SQL function: Set random seed for reproducible sampling.
 * sampling_set_seed(seed bigint) -> void
 */
Datum sampling_set_seed(PG_FUNCTION_ARGS)
{
    int64 seed = PG_GETARG_INT64(0);

    sampling_rng_init((uint64)seed);

    PG_RETURN_VOID();
}

/*
 * SQL function: Generate random double for sampling.
 * sampling_random() -> float8
 */
Datum sampling_random(PG_FUNCTION_ARGS)
{
    PG_RETURN_FLOAT8(sampling_random_double());
}

/* ============================================================
 * Aggregate Functions for Reservoir Sampling
 * ============================================================ */

/*
 * Internal state type for reservoir sampling aggregate.
 */
typedef struct ReservoirAggState {
    int32 reservoir_size;
    int32 num_items;
    int64 total_count;
    MemoryContext context;
    double *values; /* Sampled numeric values */
} ReservoirAggState;

PG_FUNCTION_INFO_V1(reservoir_sample_trans);
PG_FUNCTION_INFO_V1(reservoir_sample_final);
PG_FUNCTION_INFO_V1(reservoir_sample_combine);

/*
 * Aggregate transition function for reservoir sampling.
 */
Datum reservoir_sample_trans(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    ReservoirAggState *state;
    double value;
    int32 reservoir_size;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("reservoir_sample_trans called in non-aggregate context")));

    /* Get reservoir size from third argument, default to 1000 */
    reservoir_size =
        PG_NARGS() > 2 && !PG_ARGISNULL(2) ? PG_GETARG_INT32(2) : SAMPLING_DEFAULT_RESERVOIR_SIZE;

    /* Get or create state */
    if (PG_ARGISNULL(0)) {
        old_context = MemoryContextSwitchTo(agg_context);

        state = (ReservoirAggState *)palloc0(sizeof(ReservoirAggState));
        state->reservoir_size = reservoir_size;
        state->num_items = 0;
        state->total_count = 0;
        state->context = agg_context;
        state->values = palloc(reservoir_size * sizeof(double));

        MemoryContextSwitchTo(old_context);
    } else {
        state = (ReservoirAggState *)PG_GETARG_POINTER(0);
    }

    /* Skip NULL values */
    if (PG_ARGISNULL(1))
        PG_RETURN_POINTER(state);

    value = PG_GETARG_FLOAT8(1);
    state->total_count++;

    if (state->num_items < state->reservoir_size) {
        /* Reservoir not full - always add */
        state->values[state->num_items] = value;
        state->num_items++;
    } else {
        /* Reservoir full - randomly replace */
        int64 j = sampling_random_int(state->total_count);

        if (j < state->reservoir_size)
            state->values[j] = value;
    }

    PG_RETURN_POINTER(state);
}

/*
 * Aggregate final function - returns array of sampled values.
 */
Datum reservoir_sample_final(PG_FUNCTION_ARGS)
{
    ReservoirAggState *state;
    ArrayType *result;
    Datum *elems;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = (ReservoirAggState *)PG_GETARG_POINTER(0);

    if (state->num_items == 0)
        PG_RETURN_NULL();

    /* Convert to array */
    elems = palloc(state->num_items * sizeof(Datum));

    for (int i = 0; i < state->num_items; i++)
        elems[i] = Float8GetDatum(state->values[i]);

    result =
        construct_array(elems, state->num_items, FLOAT8OID, sizeof(float8), FLOAT8PASSBYVAL, 'd');

    pfree(elems);

    PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Aggregate combine function for parallel execution.
 */
Datum reservoir_sample_combine(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    ReservoirAggState *state1;
    ReservoirAggState *state2;
    ReservoirAggState *result;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("reservoir_sample_combine called in non-aggregate context")));

    state1 = PG_ARGISNULL(0) ? NULL : (ReservoirAggState *)PG_GETARG_POINTER(0);
    state2 = PG_ARGISNULL(1) ? NULL : (ReservoirAggState *)PG_GETARG_POINTER(1);

    if (state1 == NULL && state2 == NULL)
        PG_RETURN_NULL();

    if (state1 == NULL)
        PG_RETURN_POINTER(state2);
    if (state2 == NULL)
        PG_RETURN_POINTER(state1);

    /* Merge by re-sampling from both reservoirs */
    old_context = MemoryContextSwitchTo(agg_context);

    result = (ReservoirAggState *)palloc0(sizeof(ReservoirAggState));
    result->reservoir_size = state1->reservoir_size;
    result->num_items = 0;
    result->total_count = state1->total_count + state2->total_count;
    result->context = agg_context;
    result->values = palloc(result->reservoir_size * sizeof(double));

    /* Add items from both states using weighted probability */
    double p1 = (double)state1->total_count / (double)result->total_count;

    for (int i = 0; i < result->reservoir_size && i < state1->num_items + state2->num_items; i++) {
        if (sampling_random_double() < p1 && state1->num_items > 0) {
            int idx = sampling_random_int(state1->num_items);
            result->values[result->num_items++] = state1->values[idx];
        } else if (state2->num_items > 0) {
            int idx = sampling_random_int(state2->num_items);
            result->values[result->num_items++] = state2->values[idx];
        }
    }

    MemoryContextSwitchTo(old_context);

    PG_RETURN_POINTER(result);
}

/* ============================================================
 * Table Function for Sampling
 * ============================================================ */

/*
 * Internal state for sample_rows SRF
 */
typedef struct SampleRowsState {
    Relation rel;                       /* Relation being sampled */
    TableScanDesc scan;                 /* Table scan descriptor */
    TupleDesc tupdesc;                  /* Tuple descriptor */
    BernoulliSampleState *sample_state; /* Bernoulli sampling state */
    TupleTableSlot *slot;               /* Slot for current tuple */
    bool scan_done;                     /* Scan complete flag */
} SampleRowsState;

PG_FUNCTION_INFO_V1(sample_rows);

/*
 * Table function that returns a sample of input rows.
 * sample_rows(relation regclass, probability float8, seed bigint DEFAULT NULL)
 *
 * Uses Bernoulli sampling to return a random sample of rows from the table.
 * Each row has an independent probability of being selected.
 */
Datum sample_rows(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    SampleRowsState *sample_state;

    /* On first call, set up sample state and open table scan */
    if (SRF_IS_FIRSTCALL()) {
        MemoryContext old_context;
        Oid relid;
        Relation rel;
        TupleDesc tupdesc;
        double probability;
        uint64 seed;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Get arguments */
        if (PG_ARGISNULL(0))
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("relation cannot be NULL")));

        relid = PG_GETARG_OID(0);
        probability =
            PG_NARGS() > 1 && !PG_ARGISNULL(1) ? PG_GETARG_FLOAT8(1) : SAMPLING_DEFAULT_PROBABILITY;
        seed = PG_NARGS() > 2 && !PG_ARGISNULL(2) ? PG_GETARG_INT64(2) : 0;

        /* Validate probability */
        if (probability <= 0.0 || probability > 1.0)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("sampling probability must be between 0 and 1")));

        /* Open relation for reading */
        rel = table_open(relid, AccessShareLock);
        tupdesc = RelationGetDescr(rel);

        /* Set up return type to match table structure */
        funcctx->tuple_desc = BlessTupleDesc(CreateTupleDescCopy(tupdesc));

        /* Allocate and initialize sample state */
        sample_state = (SampleRowsState *)palloc0(sizeof(SampleRowsState));
        sample_state->rel = rel;
        sample_state->tupdesc = tupdesc;
        sample_state->sample_state = bernoulli_sample_create(probability, seed);
        sample_state->scan_done = false;

        /* Begin table scan */
        sample_state->scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);

        /* Create tuple slot for reading */
        sample_state->slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);

        funcctx->user_fctx = sample_state;

        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    sample_state = (SampleRowsState *)funcctx->user_fctx;

    /* Scan for next sampled row */
    while (!sample_state->scan_done) {
        /* Get next tuple from table */
        if (!table_scan_getnextslot(sample_state->scan, ForwardScanDirection, sample_state->slot)) {
            /* No more tuples */
            sample_state->scan_done = true;
            break;
        }

        /* Apply Bernoulli sampling decision */
        if (bernoulli_sample_row(sample_state->sample_state)) {
            /* This row was selected - return it */
            HeapTuple tuple;
            Datum result;

            /* Extract tuple from slot */
            tuple = ExecCopySlotHeapTuple(sample_state->slot);

            /* Build result tuple */
            result = HeapTupleGetDatum(tuple);

            SRF_RETURN_NEXT(funcctx, result);
        }

        /* Row not selected, continue scanning */
    }

    /* Cleanup when done */
    if (sample_state->scan != NULL) {
        table_endscan(sample_state->scan);
        sample_state->scan = NULL;
    }

    if (sample_state->slot != NULL) {
        ExecDropSingleTupleTableSlot(sample_state->slot);
        sample_state->slot = NULL;
    }

    if (sample_state->rel != NULL) {
        table_close(sample_state->rel, AccessShareLock);
        sample_state->rel = NULL;
    }

    SRF_RETURN_DONE(funcctx);
}

/* ============================================================
 * Additional Sampling Table Function: Reservoir Sample
 * ============================================================ */

/*
 * Internal state for reservoir_sample_rows SRF
 */
typedef struct ReservoirRowsState {
    ReservoirSampleState *reservoir; /* Reservoir sampling state */
    int32 current_index;             /* Current output index */
    TupleDesc tupdesc;               /* Tuple descriptor */
    bool scan_complete;              /* True when input scan done */
} ReservoirRowsState;

PG_FUNCTION_INFO_V1(reservoir_sample_rows);

/*
 * Table function that returns a fixed-size reservoir sample of rows.
 * reservoir_sample_rows(relation regclass, sample_size int, seed bigint DEFAULT
 * NULL)
 *
 * Uses Algorithm R to return exactly sample_size random rows (or fewer if table
 * is smaller).
 */
Datum reservoir_sample_rows(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    ReservoirRowsState *sample_state;

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext old_context;
        Oid relid;
        Relation rel;
        TupleDesc tupdesc;
        TableScanDesc scan;
        TupleTableSlot *slot;
        int32 sample_size;
        uint64 seed;
        Oid *column_types;
        int natts;
        int i;

        funcctx = SRF_FIRSTCALL_INIT();
        old_context = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Get arguments */
        if (PG_ARGISNULL(0))
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("relation cannot be NULL")));

        relid = PG_GETARG_OID(0);
        sample_size = PG_NARGS() > 1 && !PG_ARGISNULL(1) ? PG_GETARG_INT32(1)
                                                         : SAMPLING_DEFAULT_RESERVOIR_SIZE;
        seed = PG_NARGS() > 2 && !PG_ARGISNULL(2) ? PG_GETARG_INT64(2) : 0;

        if (seed != 0)
            sampling_rng_init(seed);

        /* Open relation */
        rel = table_open(relid, AccessShareLock);
        tupdesc = RelationGetDescr(rel);
        natts = tupdesc->natts;

        /* Set up return type */
        funcctx->tuple_desc = BlessTupleDesc(CreateTupleDescCopy(tupdesc));

        /* Build column type array for reservoir */
        column_types = palloc(natts * sizeof(Oid));
        for (i = 0; i < natts; i++) {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
            column_types[i] = attr->atttypid;
        }

        /* Allocate sample state */
        sample_state = (ReservoirRowsState *)palloc0(sizeof(ReservoirRowsState));
        sample_state->tupdesc = CreateTupleDescCopy(tupdesc);
        sample_state->current_index = 0;
        sample_state->scan_complete = false;

        /* Create reservoir */
        sample_state->reservoir = reservoir_sample_create(sample_size, natts, column_types);

        /* Scan entire table and fill reservoir */
        scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
        slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);

        while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
            Datum *values;
            bool *nulls;

            values = palloc(natts * sizeof(Datum));
            nulls = palloc(natts * sizeof(bool));

            slot_getallattrs(slot);
            memcpy(values, slot->tts_values, natts * sizeof(Datum));
            memcpy(nulls, slot->tts_isnull, natts * sizeof(bool));

            reservoir_sample_add(sample_state->reservoir, values, nulls);

            pfree(values);
            pfree(nulls);
        }

        ExecDropSingleTupleTableSlot(slot);
        table_endscan(scan);
        table_close(rel, AccessShareLock);

        sample_state->scan_complete = true;

        pfree(column_types);

        funcctx->user_fctx = sample_state;
        funcctx->max_calls = reservoir_sample_count(sample_state->reservoir);

        MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    sample_state = (ReservoirRowsState *)funcctx->user_fctx;

    if (sample_state->current_index < reservoir_sample_count(sample_state->reservoir)) {
        int natts = sample_state->tupdesc->natts;
        Datum *values;
        bool *nulls;
        HeapTuple tuple;
        Datum result;

        values = palloc(natts * sizeof(Datum));
        nulls = palloc(natts * sizeof(bool));

        if (reservoir_sample_get(sample_state->reservoir, sample_state->current_index, values,
                                 nulls)) {
            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
            result = HeapTupleGetDatum(tuple);

            sample_state->current_index++;

            pfree(values);
            pfree(nulls);

            SRF_RETURN_NEXT(funcctx, result);
        }

        pfree(values);
        pfree(nulls);
    }

    /* Cleanup reservoir */
    if (sample_state->reservoir != NULL) {
        reservoir_sample_free(sample_state->reservoir);
        sample_state->reservoir = NULL;
    }

    SRF_RETURN_DONE(funcctx);
}
