/*-------------------------------------------------------------------------
 *
 * tdigest.c
 *    T-Digest quantile estimation for Orochi DB
 *
 * This file implements the T-Digest algorithm for streaming quantile
 * estimation. T-Digest provides:
 *
 *   - Accurate percentile estimation with O(delta) memory
 *   - Excellent accuracy at distribution tails
 *   - Fully mergeable for parallel computation
 *
 * The implementation uses the improved "merging" variant described
 * in the paper "Computing Extremely Accurate Quantiles Using T-Digests".
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include <math.h>
#include <string.h>
#include <float.h>

#include "tdigest.h"

/* PostgreSQL function declarations */
PG_FUNCTION_INFO_V1(tdigest_create_sql);
PG_FUNCTION_INFO_V1(tdigest_add_sql);
PG_FUNCTION_INFO_V1(tdigest_quantile_sql);
PG_FUNCTION_INFO_V1(tdigest_percentile_sql);
PG_FUNCTION_INFO_V1(tdigest_merge_sql);
PG_FUNCTION_INFO_V1(tdigest_cdf_sql);
PG_FUNCTION_INFO_V1(tdigest_info_sql);
PG_FUNCTION_INFO_V1(tdigest_in);
PG_FUNCTION_INFO_V1(tdigest_out);
PG_FUNCTION_INFO_V1(tdigest_trans);
PG_FUNCTION_INFO_V1(tdigest_combine);
PG_FUNCTION_INFO_V1(tdigest_final_percentile);
PG_FUNCTION_INFO_V1(tdigest_final_agg);
PG_FUNCTION_INFO_V1(tdigest_serial);
PG_FUNCTION_INFO_V1(tdigest_deserial);

/* ============================================================
 * Comparison function for sorting
 * ============================================================ */

typedef struct
{
    double value;
    double weight;
} ValueWeight;

static int
compare_value_weight(const void *a, const void *b)
{
    double va = ((const ValueWeight *)a)->value;
    double vb = ((const ValueWeight *)b)->value;

    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static int
compare_centroids(const void *a, const void *b)
{
    double ma = ((const Centroid *)a)->mean;
    double mb = ((const Centroid *)b)->mean;

    if (ma < mb) return -1;
    if (ma > mb) return 1;
    return 0;
}

/* ============================================================
 * Scale Functions
 *
 * Scale functions control the maximum weight of centroids at
 * different quantiles. They ensure high accuracy at the tails
 * while allowing larger centroids near the median.
 * ============================================================ */

/*
 * K0: Classic scale function - arcsin approximation
 * k(q) = delta/2 * arcsin(2q - 1) / pi + delta/4
 */
static double
scale_k0(double q, double delta)
{
    return delta * (asin(2.0 * q - 1.0) + M_PI_2) / M_PI;
}

static double
scale_k0_inverse(double k, double delta)
{
    return (sin(k * M_PI / delta - M_PI_2) + 1.0) / 2.0;
}

/*
 * K1: Improved scale function (default)
 * k(q) = delta/2pi * arcsin(2q - 1)
 */
static double
scale_k1(double q, double delta)
{
    if (q <= 0.0) return 0.0;
    if (q >= 1.0) return delta;

    return delta * (asin(2.0 * q - 1.0) / M_PI + 0.5);
}

static double
scale_k1_inverse(double k, double delta)
{
    double p = k / delta - 0.5;
    return (sin(p * M_PI) + 1.0) / 2.0;
}

/*
 * K2: Aggressive tail accuracy
 * k(q) = delta * log(q / (1-q)) / 4
 */
static double
scale_k2(double q, double delta)
{
    if (q <= 1e-15) return 0.0;
    if (q >= 1.0 - 1e-15) return delta;

    return delta * (log(q / (1.0 - q)) / 4.0 + 0.5);
}

static double
scale_k2_inverse(double k, double delta)
{
    double x = (k / delta - 0.5) * 4.0;
    return exp(x) / (1.0 + exp(x));
}

/*
 * K3: Most aggressive tail accuracy
 * k(q) = delta * (q < 0.5 ? log(2q) : -log(2(1-q))) / 4
 */
static double
scale_k3(double q, double delta)
{
    if (q <= 1e-15) return 0.0;
    if (q >= 1.0 - 1e-15) return delta;

    if (q < 0.5)
        return delta * log(2.0 * q) / 4.0 + delta / 4.0;
    else
        return delta * (-log(2.0 * (1.0 - q))) / 4.0 + delta * 3.0 / 4.0;
}

static double
scale_k3_inverse(double k, double delta)
{
    if (k < delta / 4.0)
        return exp((k - delta / 4.0) * 4.0 / delta) / 2.0;
    else
        return 1.0 - exp((delta * 3.0 / 4.0 - k) * 4.0 / delta) / 2.0;
}

double
tdigest_scale_function(double q, double compression, int scale_type)
{
    switch (scale_type)
    {
        case TDIGEST_SCALE_K0:
            return scale_k0(q, compression);
        case TDIGEST_SCALE_K1:
            return scale_k1(q, compression);
        case TDIGEST_SCALE_K2:
            return scale_k2(q, compression);
        case TDIGEST_SCALE_K3:
            return scale_k3(q, compression);
        default:
            return scale_k1(q, compression);
    }
}

double
tdigest_scale_inverse(double k, double compression, int scale_type)
{
    switch (scale_type)
    {
        case TDIGEST_SCALE_K0:
            return scale_k0_inverse(k, compression);
        case TDIGEST_SCALE_K1:
            return scale_k1_inverse(k, compression);
        case TDIGEST_SCALE_K2:
            return scale_k2_inverse(k, compression);
        case TDIGEST_SCALE_K3:
            return scale_k3_inverse(k, compression);
        default:
            return scale_k1_inverse(k, compression);
    }
}

/* ============================================================
 * Core T-Digest Operations
 * ============================================================ */

/*
 * Create a new T-Digest with specified compression factor.
 */
TDigest *
tdigest_create(double compression)
{
    return tdigest_create_with_scale(compression, TDIGEST_SCALE_K1);
}

TDigest *
tdigest_create_with_scale(double compression, int scale_function)
{
    TDigest *td;
    int max_centroids;
    Size size;

    /* Validate compression */
    if (compression < TDIGEST_COMPRESSION_MIN)
        compression = TDIGEST_COMPRESSION_MIN;
    else if (compression > TDIGEST_COMPRESSION_MAX)
        compression = TDIGEST_COMPRESSION_MAX;

    max_centroids = TDIGEST_MAX_CENTROIDS(compression);
    size = TDIGEST_SIZE(max_centroids);

    td = (TDigest *) palloc0(size);
    SET_VARSIZE(td, size);

    td->compression = compression;
    td->num_centroids = 0;
    td->max_centroids = max_centroids;
    td->total_weight = 0;
    td->min_value = DBL_MAX;
    td->max_value = -DBL_MAX;
    td->buffer_count = 0;
    td->scale_function = (uint8) scale_function;
    td->is_sorted = 1;
    td->flags = 0;

    return td;
}

/*
 * Sort centroids by mean value.
 */
void
tdigest_sort_centroids(TDigest *td)
{
    if (!td->is_sorted && td->num_centroids > 1)
    {
        Centroid *centroids = TDIGEST_CENTROIDS(td);
        qsort(centroids, td->num_centroids, sizeof(Centroid), compare_centroids);
        td->is_sorted = 1;
    }
}

/*
 * Flush buffer and recompress centroids.
 */
void
tdigest_compress(TDigest *td)
{
    Centroid *centroids = TDIGEST_CENTROIDS(td);
    double *buffer_values = TDIGEST_BUFFER_VALUES(td);
    double *buffer_weights = TDIGEST_BUFFER_WEIGHTS(td);

    if (td->buffer_count == 0 && td->is_sorted)
        return;

    /* Combine existing centroids and buffered values */
    int total_count = td->num_centroids + td->buffer_count;
    if (total_count == 0)
        return;

    ValueWeight *all_values = palloc(total_count * sizeof(ValueWeight));
    int idx = 0;

    /* Add existing centroids */
    for (int i = 0; i < td->num_centroids; i++)
    {
        all_values[idx].value = centroids[i].mean;
        all_values[idx].weight = centroids[i].weight;
        idx++;
    }

    /* Add buffered values */
    for (int i = 0; i < td->buffer_count; i++)
    {
        all_values[idx].value = buffer_values[i];
        all_values[idx].weight = buffer_weights[i];
        idx++;
    }

    /* Sort by value */
    qsort(all_values, total_count, sizeof(ValueWeight), compare_value_weight);

    /* Calculate total weight */
    double total_weight = 0.0;
    for (int i = 0; i < total_count; i++)
        total_weight += all_values[i].weight;

    /* Merge values into new centroids using scale function */
    td->num_centroids = 0;
    td->buffer_count = 0;
    td->total_weight = 0;

    double cumulative_weight = 0.0;
    double centroid_mean = all_values[0].value;
    double centroid_weight = all_values[0].weight;
    double k_low = tdigest_scale_function(0.0, td->compression, td->scale_function);

    for (int i = 1; i < total_count; i++)
    {
        double proposed_weight = centroid_weight + all_values[i].weight;
        double q_low = cumulative_weight / total_weight;
        double q_high = (cumulative_weight + proposed_weight) / total_weight;

        double k_high = tdigest_scale_function(q_high, td->compression, td->scale_function);

        /* Check if adding this point would exceed the scale function limit */
        if (k_high - k_low <= 1.0 || centroid_weight == 0.0)
        {
            /* Add to current centroid */
            double new_mean = centroid_mean + (all_values[i].value - centroid_mean) *
                              all_values[i].weight / proposed_weight;
            centroid_mean = new_mean;
            centroid_weight = proposed_weight;
        }
        else
        {
            /* Save current centroid and start new one */
            if (td->num_centroids < td->max_centroids)
            {
                centroids[td->num_centroids].mean = centroid_mean;
                centroids[td->num_centroids].weight = centroid_weight;
                td->num_centroids++;
                td->total_weight += (int64) centroid_weight;
            }

            cumulative_weight += centroid_weight;
            k_low = tdigest_scale_function(cumulative_weight / total_weight,
                                            td->compression, td->scale_function);

            centroid_mean = all_values[i].value;
            centroid_weight = all_values[i].weight;
        }
    }

    /* Save last centroid */
    if (centroid_weight > 0.0 && td->num_centroids < td->max_centroids)
    {
        centroids[td->num_centroids].mean = centroid_mean;
        centroids[td->num_centroids].weight = centroid_weight;
        td->num_centroids++;
        td->total_weight += (int64) centroid_weight;
    }

    td->is_sorted = 1;

    pfree(all_values);
}

/*
 * Add a value with weight to the T-Digest.
 */
void
tdigest_add_weighted(TDigest *td, double value, double weight)
{
    double *buffer_values;
    double *buffer_weights;

    if (isnan(value) || isinf(value) || weight <= 0.0)
        return;

    /* Update min/max */
    if (value < td->min_value)
        td->min_value = value;
    if (value > td->max_value)
        td->max_value = value;

    /* Add to buffer */
    buffer_values = TDIGEST_BUFFER_VALUES(td);
    buffer_weights = TDIGEST_BUFFER_WEIGHTS(td);

    buffer_values[td->buffer_count] = value;
    buffer_weights[td->buffer_count] = weight;
    td->buffer_count++;

    /* Compress if buffer is full */
    if (td->buffer_count >= TDIGEST_BUFFER_SIZE)
        tdigest_compress(td);
}

/*
 * Add a value to the T-Digest (weight = 1).
 */
void
tdigest_add(TDigest *td, double value)
{
    tdigest_add_weighted(td, value, 1.0);
}

/*
 * Add a centroid directly (used during merge).
 */
void
tdigest_add_centroid(TDigest *td, double mean, double weight)
{
    tdigest_add_weighted(td, mean, weight);
}

/*
 * Get the value at a specified quantile.
 */
double
tdigest_quantile(TDigest *td, double q)
{
    Centroid *centroids;
    double total_weight;
    double cumulative_weight;
    double target_weight;

    /* Ensure centroids are ready */
    tdigest_compress(td);
    tdigest_sort_centroids(td);

    if (td->num_centroids == 0)
        return NAN;

    /* Handle boundary cases */
    if (q <= 0.0)
        return td->min_value;
    if (q >= 1.0)
        return td->max_value;

    centroids = TDIGEST_CENTROIDS(td);
    total_weight = (double) td->total_weight;

    if (total_weight == 0.0)
        return NAN;

    target_weight = q * total_weight;
    cumulative_weight = 0.0;

    /* Handle single centroid case */
    if (td->num_centroids == 1)
        return centroids[0].mean;

    /* Walk through centroids */
    for (int i = 0; i < td->num_centroids; i++)
    {
        double next_weight = cumulative_weight + centroids[i].weight;

        if (target_weight < next_weight)
        {
            /* Target falls within this centroid */
            double weight_in_centroid = target_weight - cumulative_weight;
            double fraction = weight_in_centroid / centroids[i].weight;

            /* Interpolate between this centroid and neighbors */
            double prev_mean = (i == 0) ? td->min_value : centroids[i - 1].mean;
            double next_mean = (i == td->num_centroids - 1) ? td->max_value : centroids[i + 1].mean;

            double delta_prev = centroids[i].mean - prev_mean;
            double delta_next = next_mean - centroids[i].mean;

            /* Piecewise linear interpolation */
            if (fraction < 0.5)
            {
                return centroids[i].mean - delta_prev * (0.5 - fraction);
            }
            else
            {
                return centroids[i].mean + delta_next * (fraction - 0.5);
            }
        }

        cumulative_weight = next_weight;
    }

    /* Should not reach here, but return max as fallback */
    return td->max_value;
}

/*
 * Get multiple quantiles efficiently.
 */
void
tdigest_quantiles(TDigest *td, double *quantiles, double *results, int count)
{
    /* Simple implementation - could be optimized */
    for (int i = 0; i < count; i++)
    {
        results[i] = tdigest_quantile(td, quantiles[i]);
    }
}

/*
 * Get the CDF value for x.
 */
double
tdigest_cdf(TDigest *td, double x)
{
    Centroid *centroids;
    double total_weight;
    double cumulative_weight;

    tdigest_compress(td);
    tdigest_sort_centroids(td);

    if (td->num_centroids == 0)
        return NAN;

    /* Handle boundary cases */
    if (x <= td->min_value)
        return 0.0;
    if (x >= td->max_value)
        return 1.0;

    centroids = TDIGEST_CENTROIDS(td);
    total_weight = (double) td->total_weight;

    if (total_weight == 0.0)
        return NAN;

    cumulative_weight = 0.0;

    for (int i = 0; i < td->num_centroids; i++)
    {
        if (x <= centroids[i].mean)
        {
            /* Interpolate within or before this centroid */
            double prev_mean = (i == 0) ? td->min_value : centroids[i - 1].mean;
            double prev_weight = (i == 0) ? 0.0 : cumulative_weight;

            double fraction = (x - prev_mean) / (centroids[i].mean - prev_mean);
            return (prev_weight + fraction * centroids[i].weight / 2.0) / total_weight;
        }

        cumulative_weight += centroids[i].weight;
    }

    return 1.0;
}

/*
 * Merge two T-Digests.
 */
TDigest *
tdigest_merge(TDigest *td1, TDigest *td2)
{
    TDigest *result;
    Centroid *centroids1;
    Centroid *centroids2;
    double compression;

    if (td1 == NULL && td2 == NULL)
        return NULL;

    if (td1 == NULL)
        return (TDigest *) PG_DETOAST_DATUM_COPY(PointerGetDatum(td2));
    if (td2 == NULL)
        return (TDigest *) PG_DETOAST_DATUM_COPY(PointerGetDatum(td1));

    /* Compress both before merging */
    tdigest_compress(td1);
    tdigest_compress(td2);

    compression = Max(td1->compression, td2->compression);
    result = tdigest_create(compression);

    result->min_value = Min(td1->min_value, td2->min_value);
    result->max_value = Max(td1->max_value, td2->max_value);

    /* Add all centroids from both digests */
    centroids1 = TDIGEST_CENTROIDS(td1);
    centroids2 = TDIGEST_CENTROIDS(td2);

    for (int i = 0; i < td1->num_centroids; i++)
        tdigest_add_centroid(result, centroids1[i].mean, centroids1[i].weight);

    for (int i = 0; i < td2->num_centroids; i++)
        tdigest_add_centroid(result, centroids2[i].mean, centroids2[i].weight);

    /* Compress the merged result */
    tdigest_compress(result);

    return result;
}

/*
 * Merge td2 into td1 in place.
 */
void
tdigest_merge_inplace(TDigest *td1, TDigest *td2)
{
    Centroid *centroids2;

    if (td1 == NULL || td2 == NULL)
        return;

    tdigest_compress(td2);

    td1->min_value = Min(td1->min_value, td2->min_value);
    td1->max_value = Max(td1->max_value, td2->max_value);

    centroids2 = TDIGEST_CENTROIDS(td2);

    for (int i = 0; i < td2->num_centroids; i++)
        tdigest_add_centroid(td1, centroids2[i].mean, centroids2[i].weight);

    tdigest_compress(td1);
}

/*
 * Reset T-Digest to empty state.
 */
void
tdigest_reset(TDigest *td)
{
    td->num_centroids = 0;
    td->total_weight = 0;
    td->min_value = DBL_MAX;
    td->max_value = -DBL_MAX;
    td->buffer_count = 0;
    td->is_sorted = 1;
}

/* ============================================================
 * Statistics Functions
 * ============================================================ */

int64
tdigest_count(TDigest *td)
{
    tdigest_compress(td);
    return td->total_weight;
}

double
tdigest_min(TDigest *td)
{
    return td->min_value == DBL_MAX ? NAN : td->min_value;
}

double
tdigest_max(TDigest *td)
{
    return td->max_value == -DBL_MAX ? NAN : td->max_value;
}

double
tdigest_mean(TDigest *td)
{
    Centroid *centroids;
    double sum = 0.0;

    tdigest_compress(td);

    if (td->total_weight == 0)
        return NAN;

    centroids = TDIGEST_CENTROIDS(td);

    for (int i = 0; i < td->num_centroids; i++)
        sum += centroids[i].mean * centroids[i].weight;

    return sum / (double) td->total_weight;
}

double
tdigest_trimmed_mean(TDigest *td, double lower_fraction, double upper_fraction)
{
    double q_low = lower_fraction;
    double q_high = 1.0 - upper_fraction;

    if (q_low >= q_high)
        return NAN;

    /* Simple implementation: average of interior quantiles */
    double v_low = tdigest_quantile(td, q_low);
    double v_high = tdigest_quantile(td, q_high);

    return (v_low + v_high) / 2.0;
}

/* ============================================================
 * Serialization
 * ============================================================ */

bytea *
tdigest_serialize(TDigest *td)
{
    StringInfoData buf;
    Centroid *centroids;

    tdigest_compress(td);

    initStringInfo(&buf);

    /* Write header */
    appendBinaryStringInfo(&buf, (char *)&td->compression, sizeof(double));
    appendBinaryStringInfo(&buf, (char *)&td->num_centroids, sizeof(int32));
    appendBinaryStringInfo(&buf, (char *)&td->total_weight, sizeof(int64));
    appendBinaryStringInfo(&buf, (char *)&td->min_value, sizeof(double));
    appendBinaryStringInfo(&buf, (char *)&td->max_value, sizeof(double));
    appendBinaryStringInfo(&buf, (char *)&td->scale_function, sizeof(uint8));

    /* Write centroids */
    centroids = TDIGEST_CENTROIDS(td);
    for (int i = 0; i < td->num_centroids; i++)
    {
        appendBinaryStringInfo(&buf, (char *)&centroids[i].mean, sizeof(double));
        appendBinaryStringInfo(&buf, (char *)&centroids[i].weight, sizeof(double));
    }

    /* Create bytea */
    bytea *result = (bytea *)palloc(VARHDRSZ + buf.len);
    SET_VARSIZE(result, VARHDRSZ + buf.len);
    memcpy(VARDATA(result), buf.data, buf.len);

    pfree(buf.data);

    return result;
}

TDigest *
tdigest_deserialize(bytea *data)
{
    char *ptr = VARDATA(data);
    TDigest *td;
    Centroid *centroids;
    double compression;
    int32 num_centroids;
    int64 total_weight;
    double min_value, max_value;
    uint8 scale_function;

    /* Read header */
    memcpy(&compression, ptr, sizeof(double));
    ptr += sizeof(double);
    memcpy(&num_centroids, ptr, sizeof(int32));
    ptr += sizeof(int32);
    memcpy(&total_weight, ptr, sizeof(int64));
    ptr += sizeof(int64);
    memcpy(&min_value, ptr, sizeof(double));
    ptr += sizeof(double);
    memcpy(&max_value, ptr, sizeof(double));
    ptr += sizeof(double);
    memcpy(&scale_function, ptr, sizeof(uint8));
    ptr += sizeof(uint8);

    /* Create T-Digest */
    td = tdigest_create_with_scale(compression, scale_function);
    td->total_weight = total_weight;
    td->min_value = min_value;
    td->max_value = max_value;
    td->num_centroids = num_centroids;
    td->is_sorted = 1;

    /* Read centroids */
    centroids = TDIGEST_CENTROIDS(td);
    for (int i = 0; i < num_centroids; i++)
    {
        memcpy(&centroids[i].mean, ptr, sizeof(double));
        ptr += sizeof(double);
        memcpy(&centroids[i].weight, ptr, sizeof(double));
        ptr += sizeof(double);
    }

    return td;
}

Size
tdigest_size(TDigest *td)
{
    return TDIGEST_SIZE(td->max_centroids);
}

/* ============================================================
 * PostgreSQL Aggregate Functions
 * ============================================================ */

/*
 * Aggregate state structure including percentile parameter.
 */
typedef struct TDigestAggState
{
    TDigest    *digest;
    double      percentile;
} TDigestAggState;

Datum
tdigest_trans(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    TDigest *state;
    double value;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("tdigest_trans called in non-aggregate context")));

    /* Get or create state */
    if (PG_ARGISNULL(0))
    {
        old_context = MemoryContextSwitchTo(agg_context);
        state = tdigest_create(TDIGEST_COMPRESSION_DEFAULT);
        MemoryContextSwitchTo(old_context);
    }
    else
    {
        state = PG_GETARG_TDIGEST_P(0);
    }

    /* Add the new value */
    if (!PG_ARGISNULL(1))
    {
        value = PG_GETARG_FLOAT8(1);
        tdigest_add(state, value);
    }

    PG_RETURN_POINTER(state);
}

Datum
tdigest_combine(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    TDigest *state1;
    TDigest *state2;
    TDigest *result;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("tdigest_combine called in non-aggregate context")));

    state1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_TDIGEST_P(0);
    state2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_TDIGEST_P(1);

    if (state1 == NULL && state2 == NULL)
        PG_RETURN_NULL();

    old_context = MemoryContextSwitchTo(agg_context);
    result = tdigest_merge(state1, state2);
    MemoryContextSwitchTo(old_context);

    PG_RETURN_POINTER(result);
}

Datum
tdigest_final_percentile(PG_FUNCTION_ARGS)
{
    TDigest *state;
    double percentile;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = PG_GETARG_TDIGEST_P(0);

    /* Get percentile from second argument or default to 0.5 (median) */
    percentile = PG_NARGS() > 1 && !PG_ARGISNULL(1) ?
                 PG_GETARG_FLOAT8(1) : 0.5;

    /* Convert percentile (0-100) to quantile (0-1) if needed */
    if (percentile > 1.0)
        percentile /= 100.0;

    PG_RETURN_FLOAT8(tdigest_quantile(state, percentile));
}

Datum
tdigest_final_agg(PG_FUNCTION_ARGS)
{
    TDigest *state;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = PG_GETARG_TDIGEST_P(0);
    tdigest_compress(state);

    PG_RETURN_TDIGEST_P(state);
}

Datum
tdigest_serial(PG_FUNCTION_ARGS)
{
    TDigest *state;
    bytea *result;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = PG_GETARG_TDIGEST_P(0);
    result = tdigest_serialize(state);

    PG_RETURN_BYTEA_P(result);
}

Datum
tdigest_deserial(PG_FUNCTION_ARGS)
{
    bytea *data;
    TDigest *result;
    MemoryContext agg_context;
    MemoryContext old_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("tdigest_deserial called in non-aggregate context")));

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    data = PG_GETARG_BYTEA_PP(0);

    old_context = MemoryContextSwitchTo(agg_context);
    result = tdigest_deserialize(data);
    MemoryContextSwitchTo(old_context);

    PG_RETURN_POINTER(result);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

Datum
tdigest_create_sql(PG_FUNCTION_ARGS)
{
    double compression = PG_GETARG_FLOAT8(0);

    if (compression < TDIGEST_COMPRESSION_MIN || compression > TDIGEST_COMPRESSION_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("T-Digest compression must be between %.0f and %.0f",
                        TDIGEST_COMPRESSION_MIN, TDIGEST_COMPRESSION_MAX)));

    PG_RETURN_TDIGEST_P(tdigest_create(compression));
}

Datum
tdigest_add_sql(PG_FUNCTION_ARGS)
{
    TDigest *td;
    double value;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("T-Digest cannot be NULL")));

    td = (TDigest *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0));

    if (!PG_ARGISNULL(1))
    {
        value = PG_GETARG_FLOAT8(1);
        tdigest_add(td, value);
    }

    PG_RETURN_TDIGEST_P(td);
}

Datum
tdigest_quantile_sql(PG_FUNCTION_ARGS)
{
    TDigest *td;
    double q;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    td = PG_GETARG_TDIGEST_P(0);
    q = PG_GETARG_FLOAT8(1);

    PG_RETURN_FLOAT8(tdigest_quantile(td, q));
}

Datum
tdigest_percentile_sql(PG_FUNCTION_ARGS)
{
    TDigest *td;
    double p;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    td = PG_GETARG_TDIGEST_P(0);
    p = PG_GETARG_FLOAT8(1);

    /* Convert percentile (0-100) to quantile (0-1) */
    if (p > 1.0)
        p /= 100.0;

    PG_RETURN_FLOAT8(tdigest_quantile(td, p));
}

Datum
tdigest_merge_sql(PG_FUNCTION_ARGS)
{
    TDigest *td1;
    TDigest *td2;

    if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
        PG_RETURN_NULL();

    td1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_TDIGEST_P(0);
    td2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_TDIGEST_P(1);

    PG_RETURN_TDIGEST_P(tdigest_merge(td1, td2));
}

Datum
tdigest_cdf_sql(PG_FUNCTION_ARGS)
{
    TDigest *td;
    double x;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    td = PG_GETARG_TDIGEST_P(0);
    x = PG_GETARG_FLOAT8(1);

    PG_RETURN_FLOAT8(tdigest_cdf(td, x));
}

Datum
tdigest_info_sql(PG_FUNCTION_ARGS)
{
    TDigest *td;
    char *result;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    td = PG_GETARG_TDIGEST_P(0);
    tdigest_compress(td);

    result = psprintf("TDigest(compression=%.0f, centroids=%d, count=%ld, min=%.4g, max=%.4g, p50=%.4g)",
                      td->compression,
                      td->num_centroids,
                      td->total_weight,
                      tdigest_min(td),
                      tdigest_max(td),
                      tdigest_quantile(td, 0.5));

    PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
tdigest_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    TDigest *td;
    double compression;

    /* Parse format: "TDigest(compression=N)" */
    if (sscanf(str, "TDigest(compression=%lf", &compression) == 1)
    {
        td = tdigest_create(compression);
    }
    else
    {
        td = tdigest_create(TDIGEST_COMPRESSION_DEFAULT);
    }

    PG_RETURN_TDIGEST_P(td);
}

Datum
tdigest_out(PG_FUNCTION_ARGS)
{
    TDigest *td = PG_GETARG_TDIGEST_P(0);
    char *result;

    tdigest_compress(td);

    result = psprintf("TDigest(compression=%.0f, count=%ld)",
                      td->compression,
                      td->total_weight);

    PG_RETURN_CSTRING(result);
}
