/*-------------------------------------------------------------------------
 *
 * compression.c
 *    Orochi DB compression algorithms implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "common/pg_lzcompress.h"

#include <string.h>

#ifdef HAVE_LZ4
#include <lz4.h>
#endif

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include "compression.h"

/* ============================================================
 * LZ4 Compression
 * ============================================================ */

int64
compress_lz4(const char *input, int64 input_size,
             char *output, int64 max_output_size)
{
#ifdef HAVE_LZ4
    int compressed_size;

    compressed_size = LZ4_compress_default(input, output,
                                           (int) input_size,
                                           (int) max_output_size);

    if (compressed_size <= 0)
        elog(ERROR, "LZ4 compression failed");

    return (int64) compressed_size;
#else
    elog(ERROR, "LZ4 support not compiled in");
    return 0;
#endif
}

int64
decompress_lz4(const char *input, int64 input_size,
               char *output, int64 expected_size)
{
#ifdef HAVE_LZ4
    int decompressed_size;

    decompressed_size = LZ4_decompress_safe(input, output,
                                            (int) input_size,
                                            (int) expected_size);

    if (decompressed_size < 0)
        elog(ERROR, "LZ4 decompression failed");

    return (int64) decompressed_size;
#else
    elog(ERROR, "LZ4 support not compiled in");
    return 0;
#endif
}

/* ============================================================
 * ZSTD Compression
 * ============================================================ */

int64
compress_zstd(const char *input, int64 input_size,
              char *output, int64 max_output_size, int level)
{
#ifdef HAVE_ZSTD
    size_t compressed_size;

    compressed_size = ZSTD_compress(output, (size_t) max_output_size,
                                    input, (size_t) input_size,
                                    level);

    if (ZSTD_isError(compressed_size))
        elog(ERROR, "ZSTD compression failed: %s",
             ZSTD_getErrorName(compressed_size));

    return (int64) compressed_size;
#else
    elog(ERROR, "ZSTD support not compiled in");
    return 0;
#endif
}

int64
decompress_zstd(const char *input, int64 input_size,
                char *output, int64 expected_size)
{
#ifdef HAVE_ZSTD
    size_t decompressed_size;

    decompressed_size = ZSTD_decompress(output, (size_t) expected_size,
                                        input, (size_t) input_size);

    if (ZSTD_isError(decompressed_size))
        elog(ERROR, "ZSTD decompression failed: %s",
             ZSTD_getErrorName(decompressed_size));

    return (int64) decompressed_size;
#else
    elog(ERROR, "ZSTD support not compiled in");
    return 0;
#endif
}

/* ============================================================
 * PostgreSQL Native Compression (PGLZ)
 * ============================================================ */

int64
compress_pglz(const char *input, int64 input_size,
              char *output, int64 max_output_size)
{
    int32 compressed_size;

    compressed_size = pglz_compress((const char *) input, (int32) input_size,
                                    output, PGLZ_strategy_default);

    if (compressed_size < 0)
    {
        /* Compression failed or didn't help, return uncompressed */
        memcpy(output, input, input_size);
        return input_size;
    }

    return (int64) compressed_size;
}

int64
decompress_pglz(const char *input, int64 input_size,
                char *output, int64 expected_size)
{
    int32 decompressed_size;

    decompressed_size = pglz_decompress((const char *) input, (int32) input_size,
                                        output, (int32) expected_size, true);

    if (decompressed_size < 0)
        elog(ERROR, "PGLZ decompression failed");

    return (int64) decompressed_size;
}

/* ============================================================
 * Delta Encoding (for integers/timestamps)
 * ============================================================ */

int64
compress_delta(const char *input, int64 input_size,
               char *output, int64 max_output_size)
{
    const int64 *values = (const int64 *) input;
    int64 *deltas = (int64 *) output;
    int64 count = input_size / sizeof(int64);
    int64 prev = 0;
    int64 i;

    if (max_output_size < input_size)
        return -1;

    for (i = 0; i < count; i++)
    {
        deltas[i] = values[i] - prev;
        prev = values[i];
    }

    /* TODO: Apply zigzag encoding and simple8b_rle for better compression */

    return input_size;
}

int64
decompress_delta(const char *input, int64 input_size,
                 char *output, int64 expected_size)
{
    const int64 *deltas = (const int64 *) input;
    int64 *values = (int64 *) output;
    int64 count = input_size / sizeof(int64);
    int64 prev = 0;
    int64 i;

    for (i = 0; i < count; i++)
    {
        values[i] = prev + deltas[i];
        prev = values[i];
    }

    return expected_size;
}

/* ============================================================
 * Gorilla Compression (for floats)
 * Based on Facebook's Gorilla paper
 * ============================================================ */

int64
compress_gorilla(const char *input, int64 input_size,
                 char *output, int64 max_output_size)
{
    const double *values = (const double *) input;
    int64 count = input_size / sizeof(double);
    uint64 *out = (uint64 *) output;
    uint64 prev_bits = 0;
    int64 out_idx = 0;
    int64 i;

    /* Simple XOR-based compression */
    for (i = 0; i < count; i++)
    {
        uint64 curr_bits;
        uint64 xor_result;

        memcpy(&curr_bits, &values[i], sizeof(uint64));
        xor_result = curr_bits ^ prev_bits;

        /* Store XOR result */
        out[out_idx++] = xor_result;
        prev_bits = curr_bits;
    }

    /* TODO: Implement proper Gorilla bit-packing for better compression */

    return out_idx * sizeof(uint64);
}

int64
decompress_gorilla(const char *input, int64 input_size,
                   char *output, int64 expected_size)
{
    const uint64 *in = (const uint64 *) input;
    double *values = (double *) output;
    int64 count = expected_size / sizeof(double);
    uint64 prev_bits = 0;
    int64 i;

    for (i = 0; i < count; i++)
    {
        uint64 curr_bits = in[i] ^ prev_bits;
        memcpy(&values[i], &curr_bits, sizeof(double));
        prev_bits = curr_bits;
    }

    return expected_size;
}

/* ============================================================
 * Dictionary Encoding
 * ============================================================ */

int64
compress_dictionary(const char *input, int64 input_size,
                    char *output, int64 max_output_size)
{
    /* TODO: Implement dictionary encoding for low-cardinality strings */
    /* For now, just copy */
    if (max_output_size < input_size)
        return -1;

    memcpy(output, input, input_size);
    return input_size;
}

int64
decompress_dictionary(const char *input, int64 input_size,
                      char *output, int64 expected_size)
{
    memcpy(output, input, input_size);
    return expected_size;
}

/* ============================================================
 * Run-Length Encoding
 * ============================================================ */

int64
compress_rle(const char *input, int64 input_size,
             char *output, int64 max_output_size)
{
    const uint8 *in = (const uint8 *) input;
    uint8 *out = (uint8 *) output;
    int64 out_idx = 0;
    int64 i = 0;

    while (i < input_size && out_idx + 2 <= max_output_size)
    {
        uint8 val = in[i];
        int64 run_length = 1;

        /* Count consecutive identical values */
        while (i + run_length < input_size &&
               in[i + run_length] == val &&
               run_length < 255)
        {
            run_length++;
        }

        /* Store value and count */
        out[out_idx++] = val;
        out[out_idx++] = (uint8) run_length;

        i += run_length;
    }

    return out_idx;
}

int64
decompress_rle(const char *input, int64 input_size,
               char *output, int64 expected_size)
{
    const uint8 *in = (const uint8 *) input;
    uint8 *out = (uint8 *) output;
    int64 out_idx = 0;
    int64 i = 0;

    while (i + 1 < input_size && out_idx < expected_size)
    {
        uint8 val = in[i];
        uint8 count = in[i + 1];
        int64 j;

        for (j = 0; j < count && out_idx < expected_size; j++)
            out[out_idx++] = val;

        i += 2;
    }

    return out_idx;
}
