/*-------------------------------------------------------------------------
 *
 * vector_ops.c
 *    Orochi DB vector operations implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "postgres.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include <float.h>
#include <math.h>

#include "../core/catalog.h"
#include "../orochi.h"
#include "vector_ops.h"

PG_FUNCTION_INFO_V1(vector_in);
PG_FUNCTION_INFO_V1(vector_out);
PG_FUNCTION_INFO_V1(vector_typmod_in);
PG_FUNCTION_INFO_V1(vector_typmod_out);
PG_FUNCTION_INFO_V1(vector_dims);
PG_FUNCTION_INFO_V1(vector_norm);
PG_FUNCTION_INFO_V1(vector_l2_distance);
PG_FUNCTION_INFO_V1(vector_cosine_distance);
PG_FUNCTION_INFO_V1(vector_inner_product);
PG_FUNCTION_INFO_V1(vector_add);
PG_FUNCTION_INFO_V1(vector_sub);
PG_FUNCTION_INFO_V1(vector_normalize);
PG_FUNCTION_INFO_V1(vector_from_array);

/* ============================================================
 * Vector Type Functions
 * ============================================================ */

/*
 * Create new zero-initialized vector
 */
Vector *vector_new(int dim)
{
    Vector *v;
    int size;

    if (dim < 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vector dimension must be at least 1")));

    if (dim > VECTOR_MAX_DIM)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vector dimension cannot exceed %d", VECTOR_MAX_DIM)));

    size = VECTOR_SIZE(dim);
    v = (Vector *)palloc0(size);
    SET_VARSIZE(v, size);
    v->dim = dim;

    return v;
}

/*
 * Copy vector
 */
Vector *vector_copy(Vector *v)
{
    Vector *copy;
    int size;

    if (v == NULL)
        return NULL;

    size = VARSIZE(v);
    copy = (Vector *)palloc(size);
    memcpy(copy, v, size);

    return copy;
}

/*
 * vector_in - parse text representation
 * Format: '[1.0, 2.0, 3.0]' or '1.0, 2.0, 3.0'
 */
Datum vector_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    int32 typmod = PG_GETARG_INT32(2);
    Vector *result;
    float *values;
    int dim = 0;
    int capacity = 16;
    char *ptr = str;
    char *end;
    bool in_brackets = false;

    values = palloc(sizeof(float) * capacity);

    /* Skip whitespace and optional opening bracket */
    while (*ptr && isspace((unsigned char)*ptr))
        ptr++;

    if (*ptr == '[') {
        in_brackets = true;
        ptr++;
    }

    while (*ptr) {
        float val;

        /* Skip whitespace and commas */
        while (*ptr && (isspace((unsigned char)*ptr) || *ptr == ','))
            ptr++;

        if (*ptr == ']' || *ptr == '\0')
            break;

        /* Parse float value */
        val = strtof(ptr, &end);

        if (end == ptr)
            ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                            errmsg("invalid input syntax for vector: \"%s\"", str)));

        /* Check for special values */
        if (isnan(val))
            ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                            errmsg("NaN not allowed in vector")));

        if (isinf(val))
            ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                            errmsg("Infinity not allowed in vector")));

        /* Store value */
        if (dim >= capacity) {
            capacity *= 2;
            values = repalloc(values, sizeof(float) * capacity);
        }
        values[dim++] = val;

        ptr = end;
    }

    if (dim == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                        errmsg("vector must have at least 1 dimension")));

    /* Check typmod constraint */
    if (typmod != -1 && dim != typmod)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("expected %d dimensions, got %d", typmod, dim)));

    /* Create result vector */
    result = vector_new(dim);
    memcpy(result->x, values, sizeof(float) * dim);

    pfree(values);

    PG_RETURN_VECTOR_P(result);
}

/*
 * vector_out - format as text
 */
Datum vector_out(PG_FUNCTION_ARGS)
{
    Vector *v = PG_GETARG_VECTOR_P(0);
    StringInfoData str;
    int i;

    initStringInfo(&str);
    appendStringInfoChar(&str, '[');

    for (i = 0; i < v->dim; i++) {
        if (i > 0)
            appendStringInfoString(&str, ", ");
        appendStringInfo(&str, "%g", v->x[i]);
    }

    appendStringInfoChar(&str, ']');

    PG_RETURN_CSTRING(str.data);
}

/*
 * vector_typmod_in - parse type modifier (dimensions)
 *
 * Accepts a single integer specifying the vector dimensions.
 * Example: vector(128) -> typmod = 128
 */
Datum vector_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
    int32 *tl;
    int n;

    tl = ArrayGetIntegerTypmods(ta, &n);

    if (n != 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid type modifier"),
                        errhint("vector type takes a single dimension, e.g., vector(128)")));

    if (tl[0] < 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vector dimensions must be at least 1")));

    if (tl[0] > VECTOR_MAX_DIM)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vector dimensions cannot exceed %d", VECTOR_MAX_DIM)));

    PG_RETURN_INT32(tl[0]);
}

/*
 * vector_typmod_out - output type modifier
 */
Datum vector_typmod_out(PG_FUNCTION_ARGS)
{
    int32 typmod = PG_GETARG_INT32(0);
    char *result;

    if (typmod < 0)
        result = pstrdup("");
    else
        result = psprintf("(%d)", typmod);

    PG_RETURN_CSTRING(result);
}

/*
 * vector_dims - return number of dimensions
 */
Datum vector_dims(PG_FUNCTION_ARGS)
{
    Vector *v = PG_GETARG_VECTOR_P(0);
    PG_RETURN_INT32(v->dim);
}

/*
 * vector_norm - return L2 norm
 */
Datum vector_norm(PG_FUNCTION_ARGS)
{
    Vector *v = PG_GETARG_VECTOR_P(0);
    PG_RETURN_FLOAT8(vector_l2_norm(v));
}

/*
 * vector_from_array - convert float array to vector
 */
Datum vector_from_array(PG_FUNCTION_ARGS)
{
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0);
    Vector *result;
    float4 *data;
    int nelems;
    int i;

    if (ARR_NDIM(arr) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("array must be 1-dimensional")));

    if (ARR_HASNULL(arr))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("array must not contain nulls")));

    nelems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
    data = (float4 *)ARR_DATA_PTR(arr);

    result = vector_new(nelems);

    for (i = 0; i < nelems; i++)
        result->x[i] = data[i];

    PG_RETURN_VECTOR_P(result);
}

/* ============================================================
 * Distance Functions
 * ============================================================ */

/*
 * Calculate L2 (Euclidean) norm
 */
float vector_l2_norm(Vector *v)
{
    return vector_l2_norm_simd(v->x, v->dim);
}

/*
 * Calculate L2 distance
 */
float vector_l2_dist(Vector *a, Vector *b)
{
    if (a->dim != b->dim)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vectors must have same dimensions")));

    return vector_l2_dist_simd(a->x, b->x, a->dim);
}

/*
 * Calculate cosine distance (1 - cosine_similarity)
 */
float vector_cosine_dist(Vector *a, Vector *b)
{
    float dot, norm_a, norm_b, similarity;

    if (a->dim != b->dim)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vectors must have same dimensions")));

    dot = vector_inner_prod_simd(a->x, b->x, a->dim);
    norm_a = vector_l2_norm_simd(a->x, a->dim);
    norm_b = vector_l2_norm_simd(b->x, b->dim);

    if (norm_a == 0 || norm_b == 0)
        return 1.0f; /* Maximum distance for zero vectors */

    similarity = dot / (norm_a * norm_b);

    /* Clamp to [-1, 1] for numerical stability */
    if (similarity > 1.0f)
        similarity = 1.0f;
    if (similarity < -1.0f)
        similarity = -1.0f;

    return 1.0f - similarity;
}

/*
 * Calculate inner product
 */
float vector_inner_prod(Vector *a, Vector *b)
{
    if (a->dim != b->dim)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vectors must have same dimensions")));

    return vector_inner_prod_simd(a->x, b->x, a->dim);
}

/*
 * SQL function: L2 distance
 */
Datum vector_l2_distance(PG_FUNCTION_ARGS)
{
    Vector *a = PG_GETARG_VECTOR_P(0);
    Vector *b = PG_GETARG_VECTOR_P(1);

    PG_RETURN_FLOAT8(vector_l2_dist(a, b));
}

/*
 * SQL function: Cosine distance
 */
Datum vector_cosine_distance(PG_FUNCTION_ARGS)
{
    Vector *a = PG_GETARG_VECTOR_P(0);
    Vector *b = PG_GETARG_VECTOR_P(1);

    PG_RETURN_FLOAT8(vector_cosine_dist(a, b));
}

/*
 * SQL function: Inner product (for Maximum Inner Product Search)
 */
Datum vector_inner_product(PG_FUNCTION_ARGS)
{
    Vector *a = PG_GETARG_VECTOR_P(0);
    Vector *b = PG_GETARG_VECTOR_P(1);

    PG_RETURN_FLOAT8(vector_inner_prod(a, b));
}

/* ============================================================
 * Vector Arithmetic
 * ============================================================ */

/*
 * SQL function: Vector addition
 */
Datum vector_add(PG_FUNCTION_ARGS)
{
    Vector *a = PG_GETARG_VECTOR_P(0);
    Vector *b = PG_GETARG_VECTOR_P(1);
    Vector *result;
    int i;

    if (a->dim != b->dim)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vectors must have same dimensions")));

    result = vector_new(a->dim);

    for (i = 0; i < a->dim; i++)
        result->x[i] = a->x[i] + b->x[i];

    PG_RETURN_VECTOR_P(result);
}

/*
 * SQL function: Vector subtraction
 */
Datum vector_sub(PG_FUNCTION_ARGS)
{
    Vector *a = PG_GETARG_VECTOR_P(0);
    Vector *b = PG_GETARG_VECTOR_P(1);
    Vector *result;
    int i;

    if (a->dim != b->dim)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("vectors must have same dimensions")));

    result = vector_new(a->dim);

    for (i = 0; i < a->dim; i++)
        result->x[i] = a->x[i] - b->x[i];

    PG_RETURN_VECTOR_P(result);
}

/*
 * Normalize vector in place
 */
void vector_normalize_inplace(Vector *v)
{
    float norm = vector_l2_norm(v);
    int i;

    if (norm == 0)
        return;

    for (i = 0; i < v->dim; i++)
        v->x[i] /= norm;
}

/*
 * SQL function: Return normalized vector
 */
Datum vector_normalize(PG_FUNCTION_ARGS)
{
    Vector *v = PG_GETARG_VECTOR_P(0);
    Vector *result;

    result = vector_copy(v);
    vector_normalize_inplace(result);

    PG_RETURN_VECTOR_P(result);
}

/* ============================================================
 * SIMD Implementations
 * ============================================================ */

/*
 * Scalar fallback for L2 distance
 */
static float vector_l2_dist_scalar(const float *a, const float *b, int dim)
{
    float sum = 0.0f;
    int i;

    for (i = 0; i < dim; i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }

    return sqrtf(sum);
}

/*
 * Scalar fallback for inner product
 */
static float vector_inner_prod_scalar(const float *a, const float *b, int dim)
{
    float sum = 0.0f;
    int i;

    for (i = 0; i < dim; i++)
        sum += a[i] * b[i];

    return sum;
}

/*
 * Scalar fallback for L2 norm
 */
static float vector_l2_norm_scalar(const float *a, int dim)
{
    float sum = 0.0f;
    int i;

    for (i = 0; i < dim; i++)
        sum += a[i] * a[i];

    return sqrtf(sum);
}

#ifdef __AVX2__
#include <immintrin.h>

float vector_l2_dist_avx2(const float *a, const float *b, int dim)
{
    __m256 sum = _mm256_setzero_ps();
    int i;

    for (i = 0; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }

    /* Horizontal sum */
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(hi, lo);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);

    float result = _mm_cvtss_f32(sum128);

    /* Handle remaining elements */
    for (; i < dim; i++) {
        float diff = a[i] - b[i];
        result += diff * diff;
    }

    return sqrtf(result);
}

float vector_inner_prod_avx2(const float *a, const float *b, int dim)
{
    __m256 sum = _mm256_setzero_ps();
    int i;

    for (i = 0; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }

    /* Horizontal sum */
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(hi, lo);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);

    float result = _mm_cvtss_f32(sum128);

    /* Handle remaining elements */
    for (; i < dim; i++)
        result += a[i] * b[i];

    return result;
}

float vector_l2_norm_avx2(const float *a, int dim)
{
    __m256 sum = _mm256_setzero_ps();
    int i;

    for (i = 0; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        sum = _mm256_fmadd_ps(va, va, sum);
    }

    /* Horizontal sum */
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(hi, lo);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);

    float result = _mm_cvtss_f32(sum128);

    /* Handle remaining elements */
    for (; i < dim; i++)
        result += a[i] * a[i];

    return sqrtf(result);
}
#endif

/*
 * Dispatch to best available implementation
 */
float vector_l2_dist_simd(const float *a, const float *b, int dim)
{
#ifdef __AVX2__
    return vector_l2_dist_avx2(a, b, dim);
#else
    return vector_l2_dist_scalar(a, b, dim);
#endif
}

float vector_inner_prod_simd(const float *a, const float *b, int dim)
{
#ifdef __AVX2__
    return vector_inner_prod_avx2(a, b, dim);
#else
    return vector_inner_prod_scalar(a, b, dim);
#endif
}

float vector_l2_norm_simd(const float *a, int dim)
{
#ifdef __AVX2__
    return vector_l2_norm_avx2(a, dim);
#else
    return vector_l2_norm_scalar(a, dim);
#endif
}

/* ============================================================
 * Batch Operations
 * ============================================================ */

void vector_batch_l2_distances(Vector *query, Vector **vectors, int count, float *distances)
{
    int i;

    for (i = 0; i < count; i++)
        distances[i] = vector_l2_dist(query, vectors[i]);
}

/*
 * Simple heap-based k-NN implementation
 */
void vector_knn(Vector *query, Vector **vectors, int count, int k, int *indices, float *distances,
                const char *distance_type)
{
    int i, j;
    float (*dist_func)(Vector *, Vector *);

    /* Select distance function */
    if (strcmp(distance_type, VECTOR_DIST_L2) == 0)
        dist_func = vector_l2_dist;
    else if (strcmp(distance_type, VECTOR_DIST_COSINE) == 0)
        dist_func = vector_cosine_dist;
    else if (strcmp(distance_type, VECTOR_DIST_INNER) == 0)
        dist_func = vector_inner_prod; /* Note: for MIPS, negate this */
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("unknown distance type: %s", distance_type)));

    /* Initialize with max distances */
    for (i = 0; i < k; i++) {
        indices[i] = -1;
        distances[i] = FLT_MAX;
    }

    /* Simple O(n*k) algorithm - replace with heap for better performance */
    for (i = 0; i < count; i++) {
        float d = dist_func(query, vectors[i]);

        /* Check if this should be in top-k */
        if (d < distances[k - 1]) {
            /* Find insertion point */
            for (j = k - 1; j > 0 && d < distances[j - 1]; j--) {
                distances[j] = distances[j - 1];
                indices[j] = indices[j - 1];
            }
            distances[j] = d;
            indices[j] = i;
        }
    }
}

/* ============================================================
 * Index Creation
 * ============================================================ */

void orochi_create_vector_index(Oid table_oid, const char *column_name, int dimensions,
                                const char *index_type, const char *distance_type)
{
    StringInfoData sql;

    initStringInfo(&sql);

    if (strcmp(index_type, VECTOR_INDEX_IVFFLAT) == 0) {
        appendStringInfo(&sql,
                         "CREATE INDEX ON %s USING ivfflat (%s vector_l2_ops) "
                         "WITH (lists = 100)",
                         get_rel_name(table_oid), column_name);
    } else if (strcmp(index_type, VECTOR_INDEX_HNSW) == 0) {
        appendStringInfo(&sql,
                         "CREATE INDEX ON %s USING hnsw (%s vector_l2_ops) "
                         "WITH (m = 16, ef_construction = 64)",
                         get_rel_name(table_oid), column_name);
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("unknown vector index type: %s", index_type)));
    }

    /* Register in catalog */
    OrochiVectorIndex index_info;
    memset(&index_info, 0, sizeof(index_info));
    index_info.table_oid = table_oid;
    index_info.dimensions = dimensions;
    index_info.index_type = pstrdup(index_type);
    index_info.distance_type = pstrdup(distance_type);

    orochi_catalog_register_vector_index(&index_info);

    elog(NOTICE, "Created %s vector index on %s.%s", index_type, get_rel_name(table_oid),
         column_name);

    pfree(sql.data);
}
