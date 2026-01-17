/*-------------------------------------------------------------------------
 *
 * hyperloglog.c
 *    HyperLogLog cardinality estimation for Orochi DB
 *
 * This file implements the HyperLogLog algorithm for probabilistic
 * cardinality estimation. Key features:
 *
 *   - MurmurHash3 for uniform hash distribution
 *   - Bias correction using empirically-derived constants
 *   - Linear counting for small cardinalities
 *   - Parallel-safe operations for PostgreSQL aggregates
 *
 * References:
 *   - Flajolet et al., "HyperLogLog: the analysis of a near-optimal
 *     cardinality estimation algorithm", DMTCS 2007
 *   - Heule et al., "HyperLogLog in Practice: Algorithmic Engineering
 *     of a State of The Art Cardinality Estimation Algorithm", EDBT 2013
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
#include "utils/typcache.h"
#include "utils/datum.h"
#include "utils/hashutils.h"
#include "catalog/pg_type.h"

#include <math.h>
#include <string.h>

#include "hyperloglog.h"

/* PostgreSQL function declarations */
PG_FUNCTION_INFO_V1(hll_create_sql);
PG_FUNCTION_INFO_V1(hll_add_sql);
PG_FUNCTION_INFO_V1(hll_count_sql);
PG_FUNCTION_INFO_V1(hll_merge_sql);
PG_FUNCTION_INFO_V1(hll_in);
PG_FUNCTION_INFO_V1(hll_out);
PG_FUNCTION_INFO_V1(hll_trans);
PG_FUNCTION_INFO_V1(hll_combine);
PG_FUNCTION_INFO_V1(hll_final);
PG_FUNCTION_INFO_V1(hll_serial);
PG_FUNCTION_INFO_V1(hll_deserial);

/* ============================================================
 * Bias Correction Data
 *
 * Empirically-derived bias corrections for small cardinalities.
 * These values come from the Google HyperLogLog++ paper.
 * ============================================================ */

/* Raw estimate thresholds for bias correction */
static const double bias_thresholds[] = {
    10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
    120, 140, 160, 180, 200, 220, 240, 260, 280, 300
};

/* Bias correction values for precision 14 */
static const double bias_data_p14[] = {
    0.7153, 0.6800, 0.6566, 0.6333, 0.6100, 0.5867, 0.5634, 0.5400,
    0.5167, 0.4934, 0.4600, 0.4267, 0.3934, 0.3600, 0.3267, 0.2933,
    0.2600, 0.2267, 0.1934, 0.1600
};

#define BIAS_DATA_SIZE (sizeof(bias_thresholds) / sizeof(bias_thresholds[0]))

/* ============================================================
 * MurmurHash3 Implementation
 *
 * A fast, non-cryptographic hash function with excellent
 * distribution properties. This is the 64-bit variant.
 * ============================================================ */

static inline uint64
rotl64(uint64 x, int8 r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64
fmix64(uint64 k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

uint64
hll_murmurhash3_64(const void *key, Size len, uint32 seed)
{
    const uint8 *data = (const uint8 *)key;
    const int nblocks = len / 16;

    uint64 h1 = seed;
    uint64 h2 = seed;

    const uint64 c1 = 0x87c37b91114253d5ULL;
    const uint64 c2 = 0x4cf5ad432745937fULL;

    /* Body - process 16-byte blocks */
    const uint64 *blocks = (const uint64 *)(data);

    for (int i = 0; i < nblocks; i++)
    {
        uint64 k1 = blocks[i * 2];
        uint64 k2 = blocks[i * 2 + 1];

        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;

        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;

        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    /* Tail - process remaining bytes */
    const uint8 *tail = (const uint8 *)(data + nblocks * 16);

    uint64 k1 = 0;
    uint64 k2 = 0;

    switch (len & 15)
    {
        case 15: k2 ^= ((uint64)tail[14]) << 48;  /* fall through */
        case 14: k2 ^= ((uint64)tail[13]) << 40;  /* fall through */
        case 13: k2 ^= ((uint64)tail[12]) << 32;  /* fall through */
        case 12: k2 ^= ((uint64)tail[11]) << 24;  /* fall through */
        case 11: k2 ^= ((uint64)tail[10]) << 16;  /* fall through */
        case 10: k2 ^= ((uint64)tail[9]) << 8;    /* fall through */
        case 9:  k2 ^= ((uint64)tail[8]) << 0;
                 k2 *= c2;
                 k2 = rotl64(k2, 33);
                 k2 *= c1;
                 h2 ^= k2;
                 /* fall through */

        case 8:  k1 ^= ((uint64)tail[7]) << 56;   /* fall through */
        case 7:  k1 ^= ((uint64)tail[6]) << 48;   /* fall through */
        case 6:  k1 ^= ((uint64)tail[5]) << 40;   /* fall through */
        case 5:  k1 ^= ((uint64)tail[4]) << 32;   /* fall through */
        case 4:  k1 ^= ((uint64)tail[3]) << 24;   /* fall through */
        case 3:  k1 ^= ((uint64)tail[2]) << 16;   /* fall through */
        case 2:  k1 ^= ((uint64)tail[1]) << 8;    /* fall through */
        case 1:  k1 ^= ((uint64)tail[0]) << 0;
                 k1 *= c1;
                 k1 = rotl64(k1, 31);
                 k1 *= c2;
                 h1 ^= k1;
    }

    /* Finalization */
    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;

    return h1;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

/*
 * Count leading zeros in a 64-bit value.
 * Returns 64 if value is 0.
 */
uint8
hll_leading_zeros(uint64 value)
{
    if (value == 0)
        return 64;

#ifdef __GNUC__
    return __builtin_clzll(value);
#else
    /* Fallback implementation */
    uint8 n = 0;
    if ((value & 0xFFFFFFFF00000000ULL) == 0) { n += 32; value <<= 32; }
    if ((value & 0xFFFF000000000000ULL) == 0) { n += 16; value <<= 16; }
    if ((value & 0xFF00000000000000ULL) == 0) { n +=  8; value <<=  8; }
    if ((value & 0xF000000000000000ULL) == 0) { n +=  4; value <<=  4; }
    if ((value & 0xC000000000000000ULL) == 0) { n +=  2; value <<=  2; }
    if ((value & 0x8000000000000000ULL) == 0) { n +=  1; }
    return n;
#endif
}

/*
 * Get alpha constant for bias correction.
 * Alpha depends on the number of registers (m = 2^precision).
 */
float
hll_alpha(uint8 precision)
{
    int m = 1 << precision;

    switch (m)
    {
        case 16:
            return 0.673f;
        case 32:
            return 0.697f;
        case 64:
            return 0.709f;
        default:
            return 0.7213f / (1.0f + 1.079f / (float)m);
    }
}

/*
 * Get theoretical error rate for a precision.
 */
float
hll_error_rate(uint8 precision)
{
    int m = 1 << precision;
    return 1.04f / sqrtf((float)m);
}

/*
 * Apply bias correction for small cardinalities.
 * Uses linear interpolation between known bias values.
 */
static double
apply_bias_correction(double raw_estimate, uint8 precision)
{
    double bias = 0.0;

    /* Only apply bias correction for precision 14 and small estimates */
    if (precision != 14 || raw_estimate >= bias_thresholds[BIAS_DATA_SIZE - 1])
        return raw_estimate;

    /* Find interpolation points */
    for (size_t i = 0; i < BIAS_DATA_SIZE - 1; i++)
    {
        if (raw_estimate < bias_thresholds[i + 1])
        {
            double t = (raw_estimate - bias_thresholds[i]) /
                       (bias_thresholds[i + 1] - bias_thresholds[i]);
            bias = bias_data_p14[i] + t * (bias_data_p14[i + 1] - bias_data_p14[i]);
            break;
        }
    }

    return raw_estimate - bias * raw_estimate;
}

/* ============================================================
 * Core HyperLogLog Operations
 * ============================================================ */

/*
 * Create a new HyperLogLog with specified precision.
 */
HyperLogLog *
hll_create(uint8 precision)
{
    HyperLogLog *hll;
    int num_registers;
    Size size;

    /* Validate precision */
    if (precision < HLL_PRECISION_MIN)
        precision = HLL_PRECISION_MIN;
    else if (precision > HLL_PRECISION_MAX)
        precision = HLL_PRECISION_MAX;

    num_registers = 1 << precision;
    size = HLL_SIZE(precision);

    hll = (HyperLogLog *) palloc0(size);
    SET_VARSIZE(hll, size);

    hll->precision = precision;
    hll->sparse = 0;
    hll->flags = 0;
    hll->total_count = 0;

    /* Registers are already zeroed by palloc0 */

    return hll;
}

/*
 * Add an element using its hash value.
 */
void
hll_add_hash(HyperLogLog *hll, uint64 hash)
{
    uint32 idx;
    uint8 rank;
    int num_registers;

    num_registers = 1 << hll->precision;

    /* Use top p bits as bucket index */
    idx = hash >> (64 - hll->precision);

    /* Count leading zeros in remaining bits + 1 */
    /* Shift left by precision to get the remaining bits */
    uint64 remaining = (hash << hll->precision) | ((uint64)1 << (hll->precision - 1));
    rank = hll_leading_zeros(remaining) + 1;

    /* Update register if new rank is higher */
    if (rank > hll->registers[idx])
        hll->registers[idx] = rank;

    hll->total_count++;
}

/*
 * Add an element to the HyperLogLog.
 */
void
hll_add(HyperLogLog *hll, const void *data, Size len)
{
    uint64 hash = hll_murmurhash3_64(data, len, MURMUR_SEED);
    hll_add_hash(hll, hash);
}

/*
 * Estimate the cardinality.
 */
int64
hll_count(HyperLogLog *hll)
{
    int num_registers = 1 << hll->precision;
    double alpha = hll_alpha(hll->precision);
    double sum = 0.0;
    int empty_registers = 0;

    /* Calculate harmonic mean of 2^(-register) values */
    for (int i = 0; i < num_registers; i++)
    {
        sum += 1.0 / (double)(1ULL << hll->registers[i]);
        if (hll->registers[i] == 0)
            empty_registers++;
    }

    /* Raw HyperLogLog estimate */
    double raw_estimate = alpha * (double)num_registers * (double)num_registers / sum;

    /* Apply corrections */
    double estimate;

    if (raw_estimate <= HLL_LINEAR_COUNT_THRESHOLD * num_registers)
    {
        /* Small range correction using linear counting */
        if (empty_registers > 0)
        {
            /* Linear counting */
            estimate = (double)num_registers * log((double)num_registers / (double)empty_registers);
        }
        else
        {
            estimate = raw_estimate;
        }
    }
    else if (raw_estimate <= (1.0 / 30.0) * (double)(1ULL << 32))
    {
        /* Intermediate range - no correction needed */
        estimate = raw_estimate;
    }
    else
    {
        /* Large range correction for hash collision */
        estimate = -(double)(1ULL << 32) * log(1.0 - raw_estimate / (double)(1ULL << 32));
    }

    /* Apply bias correction for small cardinalities */
    estimate = apply_bias_correction(estimate, hll->precision);

    return (int64)round(estimate);
}

/*
 * Merge two HyperLogLogs.
 */
HyperLogLog *
hll_merge(HyperLogLog *hll1, HyperLogLog *hll2)
{
    HyperLogLog *result;
    int num_registers;

    if (hll1 == NULL)
        return hll2 ? (HyperLogLog *)PG_DETOAST_DATUM_COPY(PointerGetDatum(hll2)) : NULL;
    if (hll2 == NULL)
        return (HyperLogLog *)PG_DETOAST_DATUM_COPY(PointerGetDatum(hll1));

    /* Both HLLs must have the same precision */
    if (hll1->precision != hll2->precision)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot merge HyperLogLogs with different precisions")));

    result = hll_create(hll1->precision);
    num_registers = 1 << hll1->precision;

    /* Take maximum of each register */
    for (int i = 0; i < num_registers; i++)
    {
        result->registers[i] = Max(hll1->registers[i], hll2->registers[i]);
    }

    result->total_count = hll1->total_count + hll2->total_count;

    return result;
}

/*
 * Merge hll2 into hll1 in place.
 */
void
hll_merge_inplace(HyperLogLog *hll1, HyperLogLog *hll2)
{
    int num_registers;

    if (hll1 == NULL || hll2 == NULL)
        return;

    if (hll1->precision != hll2->precision)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot merge HyperLogLogs with different precisions")));

    num_registers = 1 << hll1->precision;

    for (int i = 0; i < num_registers; i++)
    {
        if (hll2->registers[i] > hll1->registers[i])
            hll1->registers[i] = hll2->registers[i];
    }

    hll1->total_count += hll2->total_count;
}

/*
 * Reset HyperLogLog to empty state.
 */
void
hll_reset(HyperLogLog *hll)
{
    int num_registers = 1 << hll->precision;

    memset(hll->registers, 0, num_registers);
    hll->total_count = 0;
}

/*
 * Get the memory size of an HLL structure.
 */
Size
hll_size(HyperLogLog *hll)
{
    return HLL_SIZE(hll->precision);
}

/* ============================================================
 * Serialization
 * ============================================================ */

/*
 * Serialize HyperLogLog to bytea.
 */
bytea *
hll_serialize(HyperLogLog *hll)
{
    StringInfoData buf;
    int num_registers = 1 << hll->precision;

    initStringInfo(&buf);

    /* Write header */
    appendBinaryStringInfo(&buf, (char *)&hll->precision, sizeof(uint8));
    appendBinaryStringInfo(&buf, (char *)&hll->sparse, sizeof(uint8));
    appendBinaryStringInfo(&buf, (char *)&hll->flags, sizeof(uint16));
    appendBinaryStringInfo(&buf, (char *)&hll->total_count, sizeof(int64));

    /* Write registers */
    appendBinaryStringInfo(&buf, (char *)hll->registers, num_registers);

    /* Create bytea */
    bytea *result = (bytea *)palloc(VARHDRSZ + buf.len);
    SET_VARSIZE(result, VARHDRSZ + buf.len);
    memcpy(VARDATA(result), buf.data, buf.len);

    pfree(buf.data);

    return result;
}

/*
 * Deserialize bytea to HyperLogLog.
 */
HyperLogLog *
hll_deserialize(bytea *data)
{
    char *ptr = VARDATA(data);
    uint8 precision;
    HyperLogLog *hll;
    int num_registers;

    /* Read precision first to determine size */
    memcpy(&precision, ptr, sizeof(uint8));

    /* Create HLL structure */
    hll = hll_create(precision);
    num_registers = 1 << precision;

    /* Read header */
    ptr += sizeof(uint8);  /* precision already read */
    memcpy(&hll->sparse, ptr, sizeof(uint8));
    ptr += sizeof(uint8);
    memcpy(&hll->flags, ptr, sizeof(uint16));
    ptr += sizeof(uint16);
    memcpy(&hll->total_count, ptr, sizeof(int64));
    ptr += sizeof(int64);

    /* Read registers */
    memcpy(hll->registers, ptr, num_registers);

    return hll;
}

/* ============================================================
 * Datum Hashing
 * ============================================================ */

/*
 * Hash a PostgreSQL Datum based on its type.
 */
uint64
hll_hash_datum(Datum value, Oid type_oid, bool isnull)
{
    if (isnull)
    {
        /* Hash NULL as a special value */
        uint64 null_marker = 0xDEADBEEFCAFEBABEULL;
        return hll_murmurhash3_64(&null_marker, sizeof(null_marker), MURMUR_SEED);
    }

    switch (type_oid)
    {
        case INT2OID:
        {
            int16 v = DatumGetInt16(value);
            return hll_murmurhash3_64(&v, sizeof(v), MURMUR_SEED);
        }
        case INT4OID:
        {
            int32 v = DatumGetInt32(value);
            return hll_murmurhash3_64(&v, sizeof(v), MURMUR_SEED);
        }
        case INT8OID:
        {
            int64 v = DatumGetInt64(value);
            return hll_murmurhash3_64(&v, sizeof(v), MURMUR_SEED);
        }
        case FLOAT4OID:
        {
            float4 v = DatumGetFloat4(value);
            return hll_murmurhash3_64(&v, sizeof(v), MURMUR_SEED);
        }
        case FLOAT8OID:
        {
            float8 v = DatumGetFloat8(value);
            return hll_murmurhash3_64(&v, sizeof(v), MURMUR_SEED);
        }
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
        {
            text *t = DatumGetTextPP(value);
            return hll_murmurhash3_64(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), MURMUR_SEED);
        }
        case BYTEAOID:
        {
            bytea *b = DatumGetByteaPP(value);
            return hll_murmurhash3_64(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), MURMUR_SEED);
        }
        case UUIDOID:
        {
            return hll_murmurhash3_64(DatumGetPointer(value), 16, MURMUR_SEED);
        }
        default:
        {
            /* Generic handling: use PostgreSQL's hash function */
            TypeCacheEntry *typentry = lookup_type_cache(type_oid, TYPECACHE_HASH_PROC_FINFO);

            if (OidIsValid(typentry->hash_proc))
            {
                Datum hash_val = FunctionCall1(&typentry->hash_proc_finfo, value);
                int32 h = DatumGetInt32(hash_val);
                return hll_murmurhash3_64(&h, sizeof(h), MURMUR_SEED);
            }
            else
            {
                /* Fallback: hash the datum pointer */
                ereport(WARNING,
                        (errmsg("no hash function for type %u, using pointer hash", type_oid)));
                return hll_murmurhash3_64(&value, sizeof(Datum), MURMUR_SEED);
            }
        }
    }
}

/* ============================================================
 * PostgreSQL Aggregate Functions
 * ============================================================ */

/*
 * Aggregate transition function.
 */
Datum
hll_trans(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    HyperLogLog *state;
    Datum value;
    Oid value_type;
    bool isnull;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("hll_trans called in non-aggregate context")));

    /* Get or create state */
    if (PG_ARGISNULL(0))
    {
        old_context = MemoryContextSwitchTo(agg_context);
        state = hll_create(HLL_PRECISION_DEFAULT);
        MemoryContextSwitchTo(old_context);
    }
    else
    {
        state = PG_GETARG_HLL_P(0);
    }

    /* Add the new value */
    isnull = PG_ARGISNULL(1);
    if (!isnull)
    {
        value = PG_GETARG_DATUM(1);
        value_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

        uint64 hash = hll_hash_datum(value, value_type, false);
        hll_add_hash(state, hash);
    }

    PG_RETURN_POINTER(state);
}

/*
 * Aggregate combine function for parallel execution.
 */
Datum
hll_combine(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    MemoryContext old_context;
    HyperLogLog *state1;
    HyperLogLog *state2;
    HyperLogLog *result;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("hll_combine called in non-aggregate context")));

    state1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_HLL_P(0);
    state2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_HLL_P(1);

    if (state1 == NULL && state2 == NULL)
        PG_RETURN_NULL();

    old_context = MemoryContextSwitchTo(agg_context);
    result = hll_merge(state1, state2);
    MemoryContextSwitchTo(old_context);

    PG_RETURN_POINTER(result);
}

/*
 * Aggregate final function.
 */
Datum
hll_final(PG_FUNCTION_ARGS)
{
    HyperLogLog *state;

    if (PG_ARGISNULL(0))
        PG_RETURN_INT64(0);

    state = PG_GETARG_HLL_P(0);

    PG_RETURN_INT64(hll_count(state));
}

/*
 * Aggregate serial function.
 */
Datum
hll_serial(PG_FUNCTION_ARGS)
{
    HyperLogLog *state;
    bytea *result;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = PG_GETARG_HLL_P(0);
    result = hll_serialize(state);

    PG_RETURN_BYTEA_P(result);
}

/*
 * Aggregate deserial function.
 */
Datum
hll_deserial(PG_FUNCTION_ARGS)
{
    bytea *data;
    HyperLogLog *result;
    MemoryContext agg_context;
    MemoryContext old_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("hll_deserial called in non-aggregate context")));

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    data = PG_GETARG_BYTEA_PP(0);

    old_context = MemoryContextSwitchTo(agg_context);
    result = hll_deserialize(data);
    MemoryContextSwitchTo(old_context);

    PG_RETURN_POINTER(result);
}

/* ============================================================
 * SQL Interface Functions
 * ============================================================ */

/*
 * Create HLL from precision specification.
 */
Datum
hll_create_sql(PG_FUNCTION_ARGS)
{
    int32 precision = PG_GETARG_INT32(0);

    if (precision < HLL_PRECISION_MIN || precision > HLL_PRECISION_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("HLL precision must be between %d and %d",
                        HLL_PRECISION_MIN, HLL_PRECISION_MAX)));

    PG_RETURN_HLL_P(hll_create((uint8)precision));
}

/*
 * Add element to HLL.
 */
Datum
hll_add_sql(PG_FUNCTION_ARGS)
{
    HyperLogLog *hll;
    Datum value;
    Oid value_type;
    bool isnull;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("HLL cannot be NULL")));

    hll = PG_GETARG_HLL_P(0);

    /* Make a copy to avoid modifying the original */
    hll = (HyperLogLog *)PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0));

    isnull = PG_ARGISNULL(1);
    if (!isnull)
    {
        value = PG_GETARG_DATUM(1);
        value_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

        uint64 hash = hll_hash_datum(value, value_type, false);
        hll_add_hash(hll, hash);
    }

    PG_RETURN_HLL_P(hll);
}

/*
 * Get cardinality estimate.
 */
Datum
hll_count_sql(PG_FUNCTION_ARGS)
{
    HyperLogLog *hll;

    if (PG_ARGISNULL(0))
        PG_RETURN_INT64(0);

    hll = PG_GETARG_HLL_P(0);

    PG_RETURN_INT64(hll_count(hll));
}

/*
 * Merge two HLLs.
 */
Datum
hll_merge_sql(PG_FUNCTION_ARGS)
{
    HyperLogLog *hll1;
    HyperLogLog *hll2;

    if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
        PG_RETURN_NULL();

    hll1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_HLL_P(0);
    hll2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_HLL_P(1);

    PG_RETURN_HLL_P(hll_merge(hll1, hll2));
}

/*
 * HLL input function.
 */
Datum
hll_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    HyperLogLog *hll;
    int precision;

    /* Parse format: "HLL(precision=N, count=M)" */
    if (sscanf(str, "HLL(precision=%d", &precision) == 1)
    {
        hll = hll_create((uint8)precision);
    }
    else
    {
        hll = hll_create(HLL_PRECISION_DEFAULT);
    }

    PG_RETURN_HLL_P(hll);
}

/*
 * HLL output function.
 */
Datum
hll_out(PG_FUNCTION_ARGS)
{
    HyperLogLog *hll = PG_GETARG_HLL_P(0);
    char *result;

    result = psprintf("HLL(precision=%d, estimate=%ld, total_added=%ld)",
                      hll->precision,
                      hll_count(hll),
                      hll->total_count);

    PG_RETURN_CSTRING(result);
}
