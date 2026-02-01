/*-------------------------------------------------------------------------
 *
 * vector_ops.h
 *    Orochi DB vector operations for AI/ML workloads
 *
 * Provides:
 *   - Vector data type
 *   - Distance functions (L2, cosine, inner product)
 *   - Vector indexing (IVFFlat, HNSW)
 *   - Batch vector operations
 *   - SIMD-optimized operations
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_VECTOR_OPS_H
#define OROCHI_VECTOR_OPS_H

#include "../orochi.h"
#include "fmgr.h"
#include "postgres.h"

/* ============================================================
 * Vector Type Constants
 * ============================================================ */

#define VECTOR_MAX_DIM   16000 /* Maximum dimensions */
#define VECTOR_SIZE(dim) (offsetof(Vector, x) + sizeof(float) * (dim))

/* Index types */
#define VECTOR_INDEX_FLAT    "flat"
#define VECTOR_INDEX_IVFFLAT "ivfflat"
#define VECTOR_INDEX_HNSW    "hnsw"

/* Distance functions */
#define VECTOR_DIST_L2     "l2"
#define VECTOR_DIST_COSINE "cosine"
#define VECTOR_DIST_INNER  "inner_product"

/* ============================================================
 * Vector Data Type
 * ============================================================ */

/*
 * Vector - variable-length float array
 */
typedef struct Vector {
    int32 vl_len_;                  /* varlena header */
    int16 dim;                      /* number of dimensions */
    int16 unused;                   /* padding */
    float x[FLEXIBLE_ARRAY_MEMBER]; /* vector elements */
} Vector;

#define DatumGetVector(x)     ((Vector *)PG_DETOAST_DATUM(x))
#define PG_GETARG_VECTOR_P(n) DatumGetVector(PG_GETARG_DATUM(n))
#define PG_RETURN_VECTOR_P(x) PG_RETURN_POINTER(x)

/* ============================================================
 * Vector Index Structures
 * ============================================================ */

/*
 * IVF (Inverted File) index parameters
 */
typedef struct IVFIndexOptions {
    int32 lists;  /* Number of clusters */
    int32 probes; /* Clusters to search */
} IVFIndexOptions;

/*
 * HNSW (Hierarchical Navigable Small World) parameters
 */
typedef struct HNSWIndexOptions {
    int32 m;               /* Max connections per layer */
    int32 ef_construction; /* Size of dynamic candidate list */
    int32 ef_search;       /* Search-time ef */
} HNSWIndexOptions;

/*
 * Vector index scan state
 */
typedef struct VectorScanState {
    Oid index_oid;
    Vector *query_vector;
    int k; /* Number of results */
    char *distance_type;
    float distance_threshold;
} VectorScanState;

/* ============================================================
 * Vector Type Functions (SQL-callable)
 * ============================================================ */

/* Input/output */
extern Datum vector_in(PG_FUNCTION_ARGS);
extern Datum vector_out(PG_FUNCTION_ARGS);
extern Datum vector_recv(PG_FUNCTION_ARGS);
extern Datum vector_send(PG_FUNCTION_ARGS);
extern Datum vector_typmod_in(PG_FUNCTION_ARGS);
extern Datum vector_typmod_out(PG_FUNCTION_ARGS);

/* Construction */
extern Datum vector_from_array(PG_FUNCTION_ARGS);
extern Datum array_to_vector(PG_FUNCTION_ARGS);
extern Datum vector_to_array(PG_FUNCTION_ARGS);

/* Properties */
extern Datum vector_dims(PG_FUNCTION_ARGS);
extern Datum vector_norm(PG_FUNCTION_ARGS);

/* ============================================================
 * Distance Functions (SQL-callable)
 * ============================================================ */

/* L2 (Euclidean) distance */
extern Datum vector_l2_distance(PG_FUNCTION_ARGS);
extern Datum vector_l2_squared_distance(PG_FUNCTION_ARGS);

/* Cosine distance */
extern Datum vector_cosine_distance(PG_FUNCTION_ARGS);
extern Datum vector_cosine_similarity(PG_FUNCTION_ARGS);

/* Inner product */
extern Datum vector_inner_product(PG_FUNCTION_ARGS);
extern Datum vector_negative_inner_product(PG_FUNCTION_ARGS);

/* ============================================================
 * Vector Operations (SQL-callable)
 * ============================================================ */

/* Arithmetic */
extern Datum vector_add(PG_FUNCTION_ARGS);
extern Datum vector_sub(PG_FUNCTION_ARGS);
extern Datum vector_mul(PG_FUNCTION_ARGS);

/* Scalar operations */
extern Datum vector_scalar_mul(PG_FUNCTION_ARGS);

/* Comparison */
extern Datum vector_eq(PG_FUNCTION_ARGS);
extern Datum vector_ne(PG_FUNCTION_ARGS);
extern Datum vector_lt(PG_FUNCTION_ARGS);
extern Datum vector_le(PG_FUNCTION_ARGS);
extern Datum vector_gt(PG_FUNCTION_ARGS);
extern Datum vector_ge(PG_FUNCTION_ARGS);
extern Datum vector_cmp(PG_FUNCTION_ARGS);

/* Normalization */
extern Datum vector_normalize(PG_FUNCTION_ARGS);

/* ============================================================
 * Aggregate Functions (SQL-callable)
 * ============================================================ */

extern Datum vector_avg_accum(PG_FUNCTION_ARGS);
extern Datum vector_avg_final(PG_FUNCTION_ARGS);
extern Datum vector_sum_accum(PG_FUNCTION_ARGS);

/* ============================================================
 * Index Support Functions
 * ============================================================ */

/* Operator classes */
extern Datum vector_l2_ops_distance(PG_FUNCTION_ARGS);
extern Datum vector_cosine_ops_distance(PG_FUNCTION_ARGS);
extern Datum vector_ip_ops_distance(PG_FUNCTION_ARGS);

/* Index AM handlers */
extern Datum ivfflat_handler(PG_FUNCTION_ARGS);
extern Datum hnsw_handler(PG_FUNCTION_ARGS);

/* ============================================================
 * Internal Vector Functions
 * ============================================================ */

/*
 * Create new vector
 */
extern Vector *vector_new(int dim);

/*
 * Copy vector
 */
extern Vector *vector_copy(Vector *v);

/*
 * Calculate L2 distance (internal)
 */
extern float vector_l2_dist(Vector *a, Vector *b);

/*
 * Calculate cosine distance (internal)
 */
extern float vector_cosine_dist(Vector *a, Vector *b);

/*
 * Calculate inner product (internal)
 */
extern float vector_inner_prod(Vector *a, Vector *b);

/*
 * Calculate L2 norm (internal)
 */
extern float vector_l2_norm(Vector *v);

/*
 * Normalize vector in place
 */
extern void vector_normalize_inplace(Vector *v);

/* ============================================================
 * SIMD-Optimized Operations
 * ============================================================ */

#ifdef __AVX2__
extern float vector_l2_dist_avx2(const float *a, const float *b, int dim);
extern float vector_inner_prod_avx2(const float *a, const float *b, int dim);
extern float vector_l2_norm_avx2(const float *a, int dim);
#endif

#ifdef __AVX512F__
extern float vector_l2_dist_avx512(const float *a, const float *b, int dim);
extern float vector_inner_prod_avx512(const float *a, const float *b, int dim);
extern float vector_l2_norm_avx512(const float *a, int dim);
#endif

/* Dispatch to best available implementation */
extern float vector_l2_dist_simd(const float *a, const float *b, int dim);
extern float vector_inner_prod_simd(const float *a, const float *b, int dim);
extern float vector_l2_norm_simd(const float *a, int dim);

/* ============================================================
 * Batch Operations
 * ============================================================ */

/*
 * Compute distances from query to multiple vectors
 */
extern void vector_batch_l2_distances(Vector *query, Vector **vectors, int count, float *distances);

/*
 * Find k nearest neighbors
 */
extern void vector_knn(Vector *query, Vector **vectors, int count, int k, int *indices,
                       float *distances, const char *distance_type);

/* ============================================================
 * Quantization (for compression)
 * ============================================================ */

/*
 * Product quantization parameters
 */
typedef struct PQParams {
    int dim;          /* Original dimensions */
    int m;            /* Number of subquantizers */
    int ksub;         /* Codes per subquantizer (typically 256) */
    int dsub;         /* Dimensions per subquantizer */
    float *centroids; /* Codebook [m][ksub][dsub] */
} PQParams;

/*
 * Initialize product quantization
 */
extern PQParams *pq_init(int dim, int m);

/*
 * Train PQ codebook
 */
extern void pq_train(PQParams *pq, Vector **vectors, int count);

/*
 * Encode vector to PQ codes
 */
extern uint8 *pq_encode(PQParams *pq, Vector *v);

/*
 * Compute distance using PQ codes
 */
extern float pq_distance(PQParams *pq, Vector *query, uint8 *codes);

/* ============================================================
 * Index Creation Functions
 * ============================================================ */

/*
 * Create vector index
 */
extern void orochi_create_vector_index(Oid table_oid, const char *column_name, int dimensions,
                                       const char *index_type, const char *distance_type);

/*
 * Create IVFFlat index
 */
extern void orochi_create_ivfflat_index(Oid table_oid, const char *column_name, int dimensions,
                                        int lists, const char *distance_type);

/*
 * Create HNSW index
 */
extern void orochi_create_hnsw_index(Oid table_oid, const char *column_name, int dimensions, int m,
                                     int ef_construction, const char *distance_type);

/* ============================================================
 * Vector Search Functions
 * ============================================================ */

/*
 * Nearest neighbor search
 */
extern Datum vector_nearest_neighbors(PG_FUNCTION_ARGS);

/*
 * Range search
 */
extern Datum vector_range_search(PG_FUNCTION_ARGS);

#endif /* OROCHI_VECTOR_OPS_H */
