/*-------------------------------------------------------------------------
 *
 * hyperloglog.h
 *    HyperLogLog cardinality estimation for Orochi DB
 *
 * HyperLogLog is a probabilistic data structure for estimating the
 * cardinality (count of distinct elements) of a multiset with a small
 * memory footprint. This implementation provides:
 *
 *   - Configurable precision (4-18 bits)
 *   - Bias correction for small cardinalities
 *   - Parallel-safe merge operations
 *   - Serialization for aggregate state transfer
 *
 * Error rate: ~1.04/sqrt(m) where m = 2^precision
 * Default precision 14 gives ~0.8% standard error with 16KB memory
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_HYPERLOGLOG_H
#define OROCHI_HYPERLOGLOG_H

#include "postgres.h"
#include "fmgr.h"

/* ============================================================
 * Constants
 * ============================================================ */

/* Default precision gives ~0.8% error rate with 16KB memory */
#define HLL_PRECISION_DEFAULT       14
#define HLL_PRECISION_MIN           4
#define HLL_PRECISION_MAX           18

/* Register count = 2^precision */
#define HLL_REGISTERS(precision)    (1 << (precision))

/* Alpha constants for bias correction (based on precision) */
#define HLL_ALPHA_INF               0.7213f / (1.0f + 1.079f / (float)(1 << precision))

/* Threshold for switching between linear counting and HLL */
#define HLL_LINEAR_COUNT_THRESHOLD  2.5f

/* MurmurHash3 constants */
#define MURMUR_C1                   0xcc9e2d51
#define MURMUR_C2                   0x1b873593
#define MURMUR_SEED                 42

/* Size calculation for memory allocation */
#define HLL_SIZE(precision)         (offsetof(HyperLogLog, registers) + HLL_REGISTERS(precision))

/* ============================================================
 * Data Structures
 * ============================================================ */

/*
 * HyperLogLog - Probabilistic cardinality estimator
 *
 * Uses registers to track the maximum number of leading zeros
 * seen for each hash bucket. The harmonic mean of 2^register
 * values estimates the total cardinality.
 */
typedef struct HyperLogLog
{
    int32       vl_len_;            /* PostgreSQL varlena header */
    uint8       precision;          /* Number of bits for buckets (p) */
    uint8       sparse;             /* Using sparse representation? */
    uint16      flags;              /* Reserved for future use */
    int64       total_count;        /* Total elements added */
    uint8       registers[FLEXIBLE_ARRAY_MEMBER]; /* Max leading zeros per bucket */
} HyperLogLog;

/* Datum conversion macros for PostgreSQL integration */
#define DatumGetHLL(x)              ((HyperLogLog *) PG_DETOAST_DATUM(x))
#define PG_GETARG_HLL_P(n)          DatumGetHLL(PG_GETARG_DATUM(n))
#define PG_RETURN_HLL_P(x)          PG_RETURN_POINTER(x)

/* ============================================================
 * Function Prototypes - Core Operations
 * ============================================================ */

/*
 * Create a new HyperLogLog with specified precision.
 * Precision determines accuracy vs memory tradeoff.
 * Higher precision = more accurate but more memory.
 *
 * Memory usage: 2^precision bytes
 * Error rate: 1.04 / sqrt(2^precision)
 *
 * Precision 14 (default): 16KB memory, ~0.81% error
 * Precision 12: 4KB memory, ~1.63% error
 * Precision 16: 64KB memory, ~0.41% error
 */
extern HyperLogLog *hll_create(uint8 precision);

/*
 * Add an element to the HyperLogLog.
 * The element is hashed using MurmurHash3 to distribute
 * uniformly across buckets.
 *
 * Parameters:
 *   hll - The HyperLogLog structure
 *   data - Pointer to element data
 *   len - Length of element data in bytes
 */
extern void hll_add(HyperLogLog *hll, const void *data, Size len);

/*
 * Add an element using a pre-computed hash value.
 * Useful when the hash is already available.
 */
extern void hll_add_hash(HyperLogLog *hll, uint64 hash);

/*
 * Estimate the cardinality (count of distinct elements).
 * Applies bias correction for small cardinalities and
 * uses linear counting when many registers are empty.
 *
 * Returns: Estimated distinct count
 */
extern int64 hll_count(HyperLogLog *hll);

/*
 * Merge two HyperLogLogs into a new structure.
 * The result estimates the union cardinality.
 * Both HLLs must have the same precision.
 *
 * This operation is commutative and associative,
 * making it suitable for parallel aggregation.
 */
extern HyperLogLog *hll_merge(HyperLogLog *hll1, HyperLogLog *hll2);

/*
 * Merge hll2 into hll1 in place.
 * More memory efficient than hll_merge when
 * the original hll1 is no longer needed.
 */
extern void hll_merge_inplace(HyperLogLog *hll1, HyperLogLog *hll2);

/*
 * Reset HyperLogLog to empty state.
 * Clears all registers but preserves precision setting.
 */
extern void hll_reset(HyperLogLog *hll);

/* ============================================================
 * Function Prototypes - Serialization
 * ============================================================ */

/*
 * Serialize HyperLogLog to bytea for storage/transfer.
 * The serialized format includes:
 *   - 4 bytes: precision and flags
 *   - 8 bytes: total count
 *   - N bytes: compressed registers
 */
extern bytea *hll_serialize(HyperLogLog *hll);

/*
 * Deserialize bytea back to HyperLogLog.
 */
extern HyperLogLog *hll_deserialize(bytea *data);

/*
 * Get the memory size of an HLL structure.
 */
extern Size hll_size(HyperLogLog *hll);

/* ============================================================
 * Function Prototypes - Hashing
 * ============================================================ */

/*
 * Compute MurmurHash3 for arbitrary data.
 * Returns a 64-bit hash value.
 */
extern uint64 hll_murmurhash3_64(const void *key, Size len, uint32 seed);

/*
 * Compute hash for PostgreSQL Datum.
 * Handles various types (int, text, etc.) appropriately.
 */
extern uint64 hll_hash_datum(Datum value, Oid type_oid, bool isnull);

/* ============================================================
 * Function Prototypes - PostgreSQL Aggregate Support
 * ============================================================ */

/*
 * Aggregate transition function.
 * Called for each row to add element to HLL state.
 */
extern Datum hll_trans(PG_FUNCTION_ARGS);

/*
 * Aggregate combine function for parallel execution.
 * Merges two partial HLL states.
 */
extern Datum hll_combine(PG_FUNCTION_ARGS);

/*
 * Aggregate final function.
 * Returns the cardinality estimate.
 */
extern Datum hll_final(PG_FUNCTION_ARGS);

/*
 * Aggregate serial function.
 * Serializes HLL state for transfer between processes.
 */
extern Datum hll_serial(PG_FUNCTION_ARGS);

/*
 * Aggregate deserial function.
 * Deserializes HLL state from transferred bytes.
 */
extern Datum hll_deserial(PG_FUNCTION_ARGS);

/* ============================================================
 * Function Prototypes - SQL Interface
 * ============================================================ */

/*
 * Create HLL from precision specification.
 * SQL: hll_create(precision int) -> hll
 */
extern Datum hll_create_sql(PG_FUNCTION_ARGS);

/*
 * Add element to HLL (any type).
 * SQL: hll_add(hll, anyelement) -> hll
 */
extern Datum hll_add_sql(PG_FUNCTION_ARGS);

/*
 * Get cardinality estimate.
 * SQL: hll_count(hll) -> bigint
 */
extern Datum hll_count_sql(PG_FUNCTION_ARGS);

/*
 * Merge two HLLs.
 * SQL: hll_merge(hll, hll) -> hll
 */
extern Datum hll_merge_sql(PG_FUNCTION_ARGS);

/*
 * HLL input function (from text).
 */
extern Datum hll_in(PG_FUNCTION_ARGS);

/*
 * HLL output function (to text).
 */
extern Datum hll_out(PG_FUNCTION_ARGS);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Get theoretical error rate for a precision.
 */
extern float hll_error_rate(uint8 precision);

/*
 * Count number of leading zeros in a 64-bit value.
 * Uses compiler intrinsic when available.
 */
extern uint8 hll_leading_zeros(uint64 value);

/*
 * Get alpha constant for bias correction.
 */
extern float hll_alpha(uint8 precision);

#endif /* OROCHI_HYPERLOGLOG_H */
