/*-------------------------------------------------------------------------
 *
 * tdigest.h
 *    T-Digest quantile estimation for Orochi DB
 *
 * T-Digest is a probabilistic data structure for accurate estimation
 * of percentiles and quantiles with bounded memory. It provides:
 *
 *   - Sub-linear memory usage with configurable accuracy
 *   - High accuracy at distribution tails (p < 0.01 or p > 0.99)
 *   - Fully mergeable for parallel aggregation
 *   - Support for weighted samples
 *
 * References:
 *   - Dunning & Ertl, "Computing Extremely Accurate Quantiles Using
 *     T-Digests", arXiv:1902.04023
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_TDIGEST_H
#define OROCHI_TDIGEST_H

#include "postgres.h"
#include "fmgr.h"

/* ============================================================
 * Constants
 * ============================================================ */

/* Default compression factor (delta) - higher = more centroids = more accuracy
 */
#define TDIGEST_COMPRESSION_DEFAULT 100.0
#define TDIGEST_COMPRESSION_MIN     10.0
#define TDIGEST_COMPRESSION_MAX     1000.0

/* Maximum centroids = ceil(compression * PI / 2) */
#define TDIGEST_MAX_CENTROIDS(delta) ((int)ceil((delta) * 1.5708))

/* Buffer size for unmerged points */
#define TDIGEST_BUFFER_SIZE 500

/* Scale function options */
#define TDIGEST_SCALE_K0 0 /* Classic scale function */
#define TDIGEST_SCALE_K1 1 /* Improved scale function (default) */
#define TDIGEST_SCALE_K2 2 /* Aggressive tail accuracy */
#define TDIGEST_SCALE_K3 3 /* Most aggressive tail accuracy */

/* ============================================================
 * Data Structures
 * ============================================================ */

/*
 * Centroid - A weighted point representing a cluster of values.
 *
 * In T-Digest, similar values are grouped into centroids. Each centroid
 * stores the mean of its values and the total weight (count).
 */
typedef struct Centroid {
    double mean;   /* Mean value of this centroid */
    double weight; /* Total weight (count) of values in this centroid */
} Centroid;

/*
 * TDigest - Streaming quantile estimator.
 *
 * The T-Digest maintains a sorted list of centroids that approximate
 * the distribution of input values. The compression parameter controls
 * the tradeoff between accuracy and memory usage.
 *
 * Memory usage: O(compression) centroids
 * Accuracy: ~1/compression relative error at median, much better at tails
 */
typedef struct TDigest {
    int32 vl_len_;        /* PostgreSQL varlena header */
    double compression;   /* Compression factor (delta) */
    int32 num_centroids;  /* Current number of centroids */
    int32 max_centroids;  /* Maximum centroids capacity */
    int64 total_weight;   /* Sum of all centroid weights */
    double min_value;     /* Minimum observed value */
    double max_value;     /* Maximum observed value */
    int32 buffer_count;   /* Unmerged values in buffer */
    uint8 scale_function; /* Which scale function to use */
    uint8 is_sorted;      /* Are centroids sorted? */
    uint16 flags;         /* Reserved */

    /* Variable-length arrays follow:
     * Centroid centroids[max_centroids];
     * double buffer_values[TDIGEST_BUFFER_SIZE];
     * double buffer_weights[TDIGEST_BUFFER_SIZE];
     */
} TDigest;

/* Size calculation macros */
#define TDIGEST_HEADER_SIZE      offsetof(TDigest, flags) + sizeof(uint16)
#define TDIGEST_CENTROIDS_OFFSET MAXALIGN(TDIGEST_HEADER_SIZE)
#define TDIGEST_CENTROIDS(td)    ((Centroid *)((char *)(td) + TDIGEST_CENTROIDS_OFFSET))
#define TDIGEST_BUFFER_VALUES(td) \
    ((double *)((char *)(td) + TDIGEST_CENTROIDS_OFFSET + (td)->max_centroids * sizeof(Centroid)))
#define TDIGEST_BUFFER_WEIGHTS(td)                                                                 \
    ((double *)((char *)(td) + TDIGEST_CENTROIDS_OFFSET + (td)->max_centroids * sizeof(Centroid) + \
                TDIGEST_BUFFER_SIZE * sizeof(double)))

#define TDIGEST_SIZE(max_centroids)                                  \
    (TDIGEST_CENTROIDS_OFFSET + (max_centroids) * sizeof(Centroid) + \
     TDIGEST_BUFFER_SIZE * sizeof(double) * 2)

/* Datum conversion macros */
#define DatumGetTDigest(x)     ((TDigest *)PG_DETOAST_DATUM(x))
#define PG_GETARG_TDIGEST_P(n) DatumGetTDigest(PG_GETARG_DATUM(n))
#define PG_RETURN_TDIGEST_P(x) PG_RETURN_POINTER(x)

/* ============================================================
 * Function Prototypes - Core Operations
 * ============================================================ */

/*
 * Create a new T-Digest with specified compression factor.
 *
 * Higher compression = more centroids = better accuracy but more memory.
 * Recommended values:
 *   - 100: Good default, ~1% relative error at median
 *   - 200: High accuracy, ~0.5% relative error
 *   - 50:  Low memory, ~2% relative error
 *
 * Memory usage: approximately compression * 16 bytes
 */
extern TDigest *tdigest_create(double compression);

/*
 * Create T-Digest with explicit scale function selection.
 */
extern TDigest *tdigest_create_with_scale(double compression, int scale_function);

/*
 * Add a value to the T-Digest.
 *
 * Values are buffered and periodically merged into centroids
 * for efficiency. The merge maintains the T-Digest invariants
 * ensuring accurate quantile estimation.
 */
extern void tdigest_add(TDigest *td, double value);

/*
 * Add a weighted value to the T-Digest.
 *
 * Useful when values have different importances or when
 * pre-aggregated data is being merged.
 */
extern void tdigest_add_weighted(TDigest *td, double value, double weight);

/*
 * Get the value at a specified quantile (0.0 to 1.0).
 *
 * Examples:
 *   quantile(0.5)  -> median
 *   quantile(0.95) -> 95th percentile
 *   quantile(0.99) -> 99th percentile
 *
 * T-Digest provides especially high accuracy at extreme quantiles
 * (close to 0 or 1) which is often most important in practice.
 */
extern double tdigest_quantile(TDigest *td, double q);

/*
 * Get multiple quantiles efficiently.
 *
 * More efficient than calling tdigest_quantile multiple times
 * as it only sorts centroids once.
 */
extern void tdigest_quantiles(TDigest *td, double *quantiles, double *results, int count);

/*
 * Get the CDF value (cumulative distribution function) for a value.
 *
 * Returns the fraction of values less than or equal to x.
 * This is the inverse of the quantile function.
 */
extern double tdigest_cdf(TDigest *td, double x);

/*
 * Merge two T-Digests into a new structure.
 *
 * The result estimates the distribution of the union.
 * This operation is commutative and associative,
 * making it suitable for parallel aggregation.
 */
extern TDigest *tdigest_merge(TDigest *td1, TDigest *td2);

/*
 * Merge td2 into td1 in place.
 */
extern void tdigest_merge_inplace(TDigest *td1, TDigest *td2);

/*
 * Reset T-Digest to empty state.
 */
extern void tdigest_reset(TDigest *td);

/*
 * Flush buffered values into centroids.
 * Called automatically when buffer is full.
 */
extern void tdigest_compress(TDigest *td);

/* ============================================================
 * Function Prototypes - Statistics
 * ============================================================ */

/*
 * Get total count of values added.
 */
extern int64 tdigest_count(TDigest *td);

/*
 * Get minimum observed value.
 */
extern double tdigest_min(TDigest *td);

/*
 * Get maximum observed value.
 */
extern double tdigest_max(TDigest *td);

/*
 * Get mean of all values.
 */
extern double tdigest_mean(TDigest *td);

/*
 * Get trimmed mean (excluding extreme percentiles).
 */
extern double tdigest_trimmed_mean(TDigest *td, double lower_fraction, double upper_fraction);

/* ============================================================
 * Function Prototypes - Serialization
 * ============================================================ */

/*
 * Serialize T-Digest to bytea for storage/transfer.
 */
extern bytea *tdigest_serialize(TDigest *td);

/*
 * Deserialize bytea back to T-Digest.
 */
extern TDigest *tdigest_deserialize(bytea *data);

/*
 * Get the memory size of a T-Digest structure.
 */
extern Size tdigest_size(TDigest *td);

/* ============================================================
 * Function Prototypes - PostgreSQL Aggregate Support
 * ============================================================ */

/*
 * Aggregate transition function.
 */
extern Datum tdigest_trans(PG_FUNCTION_ARGS);

/*
 * Aggregate combine function for parallel execution.
 */
extern Datum tdigest_combine(PG_FUNCTION_ARGS);

/*
 * Aggregate final function returning percentile value.
 */
extern Datum tdigest_final_percentile(PG_FUNCTION_ARGS);

/*
 * Aggregate final function returning the T-Digest itself.
 */
extern Datum tdigest_final_agg(PG_FUNCTION_ARGS);

/*
 * Aggregate serial function.
 */
extern Datum tdigest_serial(PG_FUNCTION_ARGS);

/*
 * Aggregate deserial function.
 */
extern Datum tdigest_deserial(PG_FUNCTION_ARGS);

/* ============================================================
 * Function Prototypes - SQL Interface
 * ============================================================ */

/*
 * Create T-Digest from compression specification.
 * SQL: tdigest_create(compression float8) -> tdigest
 */
extern Datum tdigest_create_sql(PG_FUNCTION_ARGS);

/*
 * Add value to T-Digest.
 * SQL: tdigest_add(tdigest, value float8) -> tdigest
 */
extern Datum tdigest_add_sql(PG_FUNCTION_ARGS);

/*
 * Get quantile value.
 * SQL: tdigest_quantile(tdigest, q float8) -> float8
 */
extern Datum tdigest_quantile_sql(PG_FUNCTION_ARGS);

/*
 * Get percentile value (quantile * 100).
 * SQL: tdigest_percentile(tdigest, p float8) -> float8
 */
extern Datum tdigest_percentile_sql(PG_FUNCTION_ARGS);

/*
 * Merge two T-Digests.
 * SQL: tdigest_merge(tdigest, tdigest) -> tdigest
 */
extern Datum tdigest_merge_sql(PG_FUNCTION_ARGS);

/*
 * Get CDF value.
 * SQL: tdigest_cdf(tdigest, x float8) -> float8
 */
extern Datum tdigest_cdf_sql(PG_FUNCTION_ARGS);

/*
 * Get summary statistics.
 * SQL: tdigest_info(tdigest) -> text
 */
extern Datum tdigest_info_sql(PG_FUNCTION_ARGS);

/*
 * T-Digest input function (from text).
 */
extern Datum tdigest_in(PG_FUNCTION_ARGS);

/*
 * T-Digest output function (to text).
 */
extern Datum tdigest_out(PG_FUNCTION_ARGS);

/* ============================================================
 * Internal Functions
 * ============================================================ */

/*
 * Scale function k(q) that determines centroid size limits.
 * Different scale functions provide different accuracy tradeoffs.
 */
extern double tdigest_scale_function(double q, double compression, int scale_type);

/*
 * Inverse scale function k^(-1)(k).
 */
extern double tdigest_scale_inverse(double k, double compression, int scale_type);

/*
 * Sort centroids by mean value.
 */
extern void tdigest_sort_centroids(TDigest *td);

/*
 * Binary search for centroid containing quantile q.
 */
extern int tdigest_find_centroid(TDigest *td, double q);

/*
 * Add a centroid directly (used during merge).
 */
extern void tdigest_add_centroid(TDigest *td, double mean, double weight);

#endif /* OROCHI_TDIGEST_H */
