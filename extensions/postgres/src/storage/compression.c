/*-------------------------------------------------------------------------
 *
 * compression.c
 *    Orochi DB compression algorithms implementation
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "common/pg_lzcompress.h"
#include "postgres.h"

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

int64 compress_lz4(const char *input, int64 input_size, char *output, int64 max_output_size)
{
#ifdef HAVE_LZ4
    int compressed_size;

    compressed_size = LZ4_compress_default(input, output, (int)input_size, (int)max_output_size);

    if (compressed_size <= 0)
        elog(ERROR, "LZ4 compression failed");

    return (int64)compressed_size;
#else
    elog(ERROR, "LZ4 support not compiled in");
    return 0;
#endif
}

int64 decompress_lz4(const char *input, int64 input_size, char *output, int64 expected_size)
{
#ifdef HAVE_LZ4
    int decompressed_size;

    decompressed_size = LZ4_decompress_safe(input, output, (int)input_size, (int)expected_size);

    if (decompressed_size < 0)
        elog(ERROR, "LZ4 decompression failed");

    return (int64)decompressed_size;
#else
    elog(ERROR, "LZ4 support not compiled in");
    return 0;
#endif
}

/* ============================================================
 * ZSTD Compression
 * ============================================================ */

int64 compress_zstd(const char *input, int64 input_size, char *output, int64 max_output_size,
                    int level)
{
#ifdef HAVE_ZSTD
    size_t compressed_size;

    compressed_size =
        ZSTD_compress(output, (size_t)max_output_size, input, (size_t)input_size, level);

    if (ZSTD_isError(compressed_size))
        elog(ERROR, "ZSTD compression failed: %s", ZSTD_getErrorName(compressed_size));

    return (int64)compressed_size;
#else
    elog(ERROR, "ZSTD support not compiled in");
    return 0;
#endif
}

int64 decompress_zstd(const char *input, int64 input_size, char *output, int64 expected_size)
{
#ifdef HAVE_ZSTD
    size_t decompressed_size;

    decompressed_size = ZSTD_decompress(output, (size_t)expected_size, input, (size_t)input_size);

    if (ZSTD_isError(decompressed_size))
        elog(ERROR, "ZSTD decompression failed: %s", ZSTD_getErrorName(decompressed_size));

    return (int64)decompressed_size;
#else
    elog(ERROR, "ZSTD support not compiled in");
    return 0;
#endif
}

/* ============================================================
 * PostgreSQL Native Compression (PGLZ)
 * ============================================================ */

int64 compress_pglz(const char *input, int64 input_size, char *output, int64 max_output_size)
{
    int32 compressed_size;

    compressed_size =
        pglz_compress((const char *)input, (int32)input_size, output, PGLZ_strategy_default);

    if (compressed_size < 0) {
        /* Compression failed or didn't help, return uncompressed */
        memcpy(output, input, input_size);
        return input_size;
    }

    return (int64)compressed_size;
}

int64 decompress_pglz(const char *input, int64 input_size, char *output, int64 expected_size)
{
    int32 decompressed_size;

    decompressed_size =
        pglz_decompress((const char *)input, (int32)input_size, output, (int32)expected_size, true);

    if (decompressed_size < 0)
        elog(ERROR, "PGLZ decompression failed");

    return (int64)decompressed_size;
}

/* ============================================================
 * Zigzag Encoding Helpers
 * Maps signed integers to unsigned integers for better compression:
 * 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, ...
 * ============================================================ */

static inline uint64 zigzag_encode(int64 n)
{
    return (uint64)((n << 1) ^ (n >> 63));
}

static inline int64 zigzag_decode(uint64 n)
{
    return (int64)((n >> 1) ^ -(int64)(n & 1));
}

/*
 * Variable-length integer encoding (varint)
 * Encodes small numbers in fewer bytes
 */
static inline int encode_varint(uint64 value, uint8 *output)
{
    int bytes = 0;

    while (value >= 0x80) {
        output[bytes++] = (uint8)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    output[bytes++] = (uint8)value;

    return bytes;
}

static inline int decode_varint(const uint8 *input, uint64 *value)
{
    uint64 result = 0;
    int shift = 0;
    int bytes = 0;

    do {
        result |= (uint64)(input[bytes] & 0x7F) << shift;
        shift += 7;
    } while (input[bytes++] & 0x80);

    *value = result;
    return bytes;
}

/* ============================================================
 * Delta Encoding (for integers/timestamps)
 * Uses zigzag encoding + varint for efficient storage
 * ============================================================ */

int64 compress_delta(const char *input, int64 input_size, char *output, int64 max_output_size)
{
    const int64 *values = (const int64 *)input;
    uint8 *out = (uint8 *)output;
    int64 count = input_size / sizeof(int64);
    int64 prev = 0;
    int64 i;
    int64 out_idx = 0;

    /* Reserve space for count header */
    if (max_output_size < (int64)sizeof(int64))
        return -1;

    /* Write count as header */
    memcpy(out, &count, sizeof(int64));
    out_idx = sizeof(int64);

    for (i = 0; i < count; i++) {
        int64 delta = values[i] - prev;
        uint64 zigzag = zigzag_encode(delta);
        int bytes_needed;

        /* Estimate max bytes needed (10 bytes max for uint64 varint) */
        if (out_idx + 10 > max_output_size) {
            /* Fall back to uncompressed */
            memcpy(output, input, input_size);
            return input_size;
        }

        bytes_needed = encode_varint(zigzag, out + out_idx);
        out_idx += bytes_needed;
        prev = values[i];
    }

    /* Only use compressed if smaller */
    if (out_idx >= input_size) {
        memcpy(output, input, input_size);
        return input_size;
    }

    return out_idx;
}

int64 decompress_delta(const char *input, int64 input_size, char *output, int64 expected_size)
{
    const uint8 *in = (const uint8 *)input;
    int64 *values = (int64 *)output;
    int64 count = expected_size / sizeof(int64);
    int64 prev = 0;
    int64 i;
    int64 in_idx = 0;

    /* Check if this is varint-encoded (has count header) */
    if (input_size >= (int64)sizeof(int64)) {
        int64 stored_count;
        memcpy(&stored_count, in, sizeof(int64));

        if (stored_count == count) {
            /* Varint-encoded delta */
            in_idx = sizeof(int64);

            for (i = 0; i < count && in_idx < input_size; i++) {
                uint64 zigzag;
                int bytes_read = decode_varint(in + in_idx, &zigzag);
                int64 delta = zigzag_decode(zigzag);

                values[i] = prev + delta;
                prev = values[i];
                in_idx += bytes_read;
            }

            return expected_size;
        }
    }

    /* Fall back to simple delta (uncompressed int64 deltas) */
    {
        const int64 *deltas = (const int64 *)input;
        count = input_size / sizeof(int64);
        prev = 0;

        for (i = 0; i < count; i++) {
            values[i] = prev + deltas[i];
            prev = values[i];
        }
    }

    return expected_size;
}

/* ============================================================
 * Gorilla Compression (for floats)
 * Based on Facebook's Gorilla paper
 * Uses XOR encoding with bit-packing for repeated leading/trailing zeros
 * ============================================================ */

/*
 * Count leading zeros in a 64-bit integer
 */
static inline int count_leading_zeros(uint64 x)
{
    int n = 0;
    if (x == 0)
        return 64;

#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(x);
#else
    if ((x & 0xFFFFFFFF00000000ULL) == 0) {
        n += 32;
        x <<= 32;
    }
    if ((x & 0xFFFF000000000000ULL) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x & 0xFF00000000000000ULL) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x & 0xF000000000000000ULL) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x & 0xC000000000000000ULL) == 0) {
        n += 2;
        x <<= 2;
    }
    if ((x & 0x8000000000000000ULL) == 0) {
        n += 1;
    }
    return n;
#endif
}

/*
 * Count trailing zeros in a 64-bit integer
 */
static inline int count_trailing_zeros(uint64 x)
{
    int n = 0;
    if (x == 0)
        return 64;

#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    if ((x & 0x00000000FFFFFFFFULL) == 0) {
        n += 32;
        x >>= 32;
    }
    if ((x & 0x000000000000FFFFULL) == 0) {
        n += 16;
        x >>= 16;
    }
    if ((x & 0x00000000000000FFULL) == 0) {
        n += 8;
        x >>= 8;
    }
    if ((x & 0x000000000000000FULL) == 0) {
        n += 4;
        x >>= 4;
    }
    if ((x & 0x0000000000000003ULL) == 0) {
        n += 2;
        x >>= 2;
    }
    if ((x & 0x0000000000000001ULL) == 0) {
        n += 1;
    }
    return n;
#endif
}

/*
 * Gorilla header structure:
 * - 8 bytes: count of values
 * - Followed by bit-packed XOR deltas
 *
 * For each XOR result:
 * - If XOR == 0: write single 0 bit
 * - If leading zeros same as previous: write 10 + meaningful bits
 * - Otherwise: write 11 + 5 bits leading + 6 bits length + meaningful bits
 */

int64 compress_gorilla(const char *input, int64 input_size, char *output, int64 max_output_size)
{
    const double *values = (const double *)input;
    int64 count = input_size / sizeof(double);
    uint8 *out = (uint8 *)output;
    uint64 prev_bits = 0;
    int prev_leading = 0;
    int prev_trailing = 0;
    int64 i;
    int64 out_idx;
    int bit_pos = 0; /* Current bit position in output byte */
    uint8 current_byte = 0;

    if (max_output_size < (int64)(sizeof(int64) + count))
        return -1;

    /* Write count header */
    memcpy(out, &count, sizeof(int64));
    out_idx = sizeof(int64);

/* Helper macro to write bits */
#define WRITE_BITS(val, nbits)                                                                     \
    do {                                                                                           \
        int _bits_remaining = (nbits);                                                             \
        uint64 _value = (val);                                                                     \
        while (_bits_remaining > 0) {                                                              \
            int bits_in_byte = 8 - bit_pos;                                                        \
            int bits_to_write = (_bits_remaining < bits_in_byte) ? _bits_remaining : bits_in_byte; \
            int shift = _bits_remaining - bits_to_write;                                           \
            current_byte |= ((_value >> shift) & ((1 << bits_to_write) - 1))                       \
                         << (bits_in_byte - bits_to_write);                                        \
            _bits_remaining -= bits_to_write;                                                      \
            bit_pos += bits_to_write;                                                              \
            if (bit_pos == 8) {                                                                    \
                out[out_idx++] = current_byte;                                                     \
                current_byte = 0;                                                                  \
                bit_pos = 0;                                                                       \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    for (i = 0; i < count; i++) {
        uint64 curr_bits;
        uint64 xor_result;

        memcpy(&curr_bits, &values[i], sizeof(uint64));
        xor_result = curr_bits ^ prev_bits;

        if (xor_result == 0) {
            /* Same as previous: write single 0 bit */
            WRITE_BITS(0, 1);
        } else {
            int leading = count_leading_zeros(xor_result);
            int trailing = count_trailing_zeros(xor_result);
            int meaningful_bits = 64 - leading - trailing;

            /* Clamp leading to 5 bits (max 31) */
            if (leading > 31)
                leading = 31;

            if (i > 0 && leading >= prev_leading && trailing >= prev_trailing) {
                /* Can reuse previous leading/trailing: write 10 + meaningful bits */
                int prev_meaningful = 64 - prev_leading - prev_trailing;
                uint64 significant =
                    (xor_result >> prev_trailing) & ((1ULL << prev_meaningful) - 1);

                WRITE_BITS(2, 2); /* 10 */
                WRITE_BITS(significant, prev_meaningful);
            } else {
                /* New leading/trailing: write 11 + 5 bits leading + 6 bits length +
                 * bits */
                uint64 significant = (xor_result >> trailing) & ((1ULL << meaningful_bits) - 1);

                WRITE_BITS(3, 2); /* 11 */
                WRITE_BITS(leading, 5);
                WRITE_BITS(meaningful_bits, 6);
                WRITE_BITS(significant, meaningful_bits);

                prev_leading = leading;
                prev_trailing = trailing;
            }
        }

        prev_bits = curr_bits;

        /* Check for overflow */
        if (out_idx >= max_output_size - 16) {
            /* Fall back to simple XOR storage */
            uint64 *simple_out = (uint64 *)output;
            int64 simple_idx = 0;

            prev_bits = 0;
            for (i = 0; i < count; i++) {
                memcpy(&curr_bits, &values[i], sizeof(uint64));
                simple_out[simple_idx++] = curr_bits ^ prev_bits;
                prev_bits = curr_bits;
            }
            return simple_idx * sizeof(uint64);
        }
    }

#undef WRITE_BITS

    /* Flush remaining bits */
    if (bit_pos > 0) {
        out[out_idx++] = current_byte;
    }

    return out_idx;
}

int64 decompress_gorilla(const char *input, int64 input_size, char *output, int64 expected_size)
{
    const uint8 *in = (const uint8 *)input;
    double *values = (double *)output;
    int64 count = expected_size / sizeof(double);
    uint64 prev_bits = 0;
    int prev_leading = 0;
    int prev_meaningful = 64;
    int64 i;
    int64 in_idx = 0;
    int bit_pos = 0;

    /* Check for header */
    if (input_size >= (int64)sizeof(int64)) {
        int64 stored_count;
        memcpy(&stored_count, in, sizeof(int64));

        if (stored_count == count) {
            /* Bit-packed format */
            in_idx = sizeof(int64);

/* Helper macro to read bits */
#define READ_BITS(nbits, result)                                                            \
    do {                                                                                    \
        int _bits_needed = (nbits);                                                         \
        uint64 _result = 0;                                                                 \
        while (_bits_needed > 0) {                                                          \
            int bits_in_byte = 8 - bit_pos;                                                 \
            int bits_to_read = (_bits_needed < bits_in_byte) ? _bits_needed : bits_in_byte; \
            uint8 mask = ((1 << bits_to_read) - 1);                                         \
            int shift = bits_in_byte - bits_to_read;                                        \
            _result = (_result << bits_to_read) | ((in[in_idx] >> shift) & mask);           \
            _bits_needed -= bits_to_read;                                                   \
            bit_pos += bits_to_read;                                                        \
            if (bit_pos == 8) {                                                             \
                in_idx++;                                                                   \
                bit_pos = 0;                                                                \
            }                                                                               \
        }                                                                                   \
        result = _result;                                                                   \
    } while (0)

            for (i = 0; i < count && in_idx < input_size; i++) {
                uint64 first_bit;
                READ_BITS(1, first_bit);

                if (first_bit == 0) {
                    /* Same as previous */
                    memcpy(&values[i], &prev_bits, sizeof(double));
                } else {
                    uint64 second_bit;
                    READ_BITS(1, second_bit);

                    if (second_bit == 0) {
                        /* Reuse previous leading/trailing */
                        uint64 significant;
                        READ_BITS(prev_meaningful, significant);
                        uint64 xor_val = significant << (64 - prev_leading - prev_meaningful);
                        uint64 curr_bits = prev_bits ^ xor_val;
                        memcpy(&values[i], &curr_bits, sizeof(double));
                        prev_bits = curr_bits;
                    } else {
                        /* New leading/trailing */
                        uint64 leading, meaningful, significant;
                        READ_BITS(5, leading);
                        READ_BITS(6, meaningful);
                        if (meaningful == 0)
                            meaningful = 64;
                        READ_BITS((int)meaningful, significant);

                        int trailing = 64 - (int)leading - (int)meaningful;
                        if (trailing < 0)
                            trailing = 0;

                        uint64 xor_val = significant << trailing;
                        uint64 curr_bits = prev_bits ^ xor_val;
                        memcpy(&values[i], &curr_bits, sizeof(double));

                        prev_bits = curr_bits;
                        prev_leading = (int)leading;
                        prev_meaningful = (int)meaningful;
                    }
                }
            }

#undef READ_BITS

            return expected_size;
        }
    }

    /* Fall back to simple XOR format */
    {
        const uint64 *xor_in = (const uint64 *)input;
        count = input_size / sizeof(uint64);
        prev_bits = 0;

        for (i = 0; i < count; i++) {
            uint64 curr_bits = xor_in[i] ^ prev_bits;
            memcpy(&values[i], &curr_bits, sizeof(double));
            prev_bits = curr_bits;
        }
    }

    return expected_size;
}

/* ============================================================
 * Dictionary Encoding
 * For low-cardinality string columns (< 256 unique values)
 *
 * Format:
 * - 4 bytes: number of unique strings (dict_size)
 * - 4 bytes: number of values (value_count)
 * - Dictionary: dict_size null-terminated strings
 * - Encoded values: value_count uint8 indices (or uint16 if dict_size > 256)
 * ============================================================ */

#define DICT_MAX_ENTRIES     65535
#define DICT_SMALL_THRESHOLD 256

typedef struct DictEntry {
    char *str;
    int len;
} DictEntry;

/*
 * Simple hash for string dictionary
 */
static inline uint32 dict_hash(const char *str, int len)
{
    uint32 hash = 5381;
    int i;

    for (i = 0; i < len; i++)
        hash = ((hash << 5) + hash) + (uint8)str[i];

    return hash;
}

int64 compress_dictionary(const char *input, int64 input_size, char *output, int64 max_output_size)
{
    /* Parse input as array of null-terminated strings */
    const char *ptr = input;
    const char *end = input + input_size;
    DictEntry *dict = NULL;
    uint32 *hash_table = NULL;
    int *indices = NULL;
    int dict_size = 0;
    int value_count = 0;
    int64 out_idx = 0;
    int i;
    int dict_capacity = 256;
    int hash_size = 1024;

    /* Allocate working memory */
    dict = (DictEntry *)palloc0(dict_capacity * sizeof(DictEntry));
    hash_table = (uint32 *)palloc0(hash_size * sizeof(uint32));
    indices = (int *)palloc0((input_size / 2 + 1) * sizeof(int));

    /* Initialize hash table to -1 (empty) */
    for (i = 0; i < hash_size; i++)
        hash_table[i] = (uint32)-1;

    /* First pass: build dictionary and collect indices */
    while (ptr < end) {
        int str_len = strlen(ptr);
        uint32 h = dict_hash(ptr, str_len) % hash_size;
        int found_idx = -1;

        /* Linear probe hash table */
        while (hash_table[h] != (uint32)-1) {
            int idx = hash_table[h];
            if (dict[idx].len == str_len && memcmp(dict[idx].str, ptr, str_len) == 0) {
                found_idx = idx;
                break;
            }
            h = (h + 1) % hash_size;
        }

        if (found_idx < 0) {
            /* New string, add to dictionary */
            if (dict_size >= dict_capacity) {
                /* Expand dictionary */
                dict_capacity *= 2;
                dict = (DictEntry *)repalloc(dict, dict_capacity * sizeof(DictEntry));
            }

            if (dict_size >= DICT_MAX_ENTRIES) {
                /* Too many unique values, fall back to copy */
                pfree(dict);
                pfree(hash_table);
                pfree(indices);

                if (max_output_size < input_size)
                    return -1;
                memcpy(output, input, input_size);
                return input_size;
            }

            dict[dict_size].str = (char *)ptr;
            dict[dict_size].len = str_len;
            hash_table[h] = dict_size;
            found_idx = dict_size;
            dict_size++;
        }

        indices[value_count++] = found_idx;
        ptr += str_len + 1; /* Skip null terminator */
    }

    /* Calculate output size */
    {
        int64 dict_bytes = 0;
        int index_bytes = (dict_size <= DICT_SMALL_THRESHOLD) ? 1 : 2;
        int64 estimated_size;

        for (i = 0; i < dict_size; i++)
            dict_bytes += dict[i].len + 1; /* Include null terminators */

        estimated_size = 8 + dict_bytes + (int64)value_count * index_bytes;

        if (estimated_size >= input_size || estimated_size > max_output_size) {
            /* Not worth it, fall back to copy */
            pfree(dict);
            pfree(hash_table);
            pfree(indices);

            if (max_output_size < input_size)
                return -1;
            memcpy(output, input, input_size);
            return input_size;
        }
    }

    /* Write header */
    {
        uint32 ds = (uint32)dict_size;
        uint32 vc = (uint32)value_count;
        memcpy(output + out_idx, &ds, sizeof(uint32));
        out_idx += sizeof(uint32);
        memcpy(output + out_idx, &vc, sizeof(uint32));
        out_idx += sizeof(uint32);
    }

    /* Write dictionary */
    for (i = 0; i < dict_size; i++) {
        memcpy(output + out_idx, dict[i].str, dict[i].len + 1);
        out_idx += dict[i].len + 1;
    }

    /* Write indices */
    if (dict_size <= DICT_SMALL_THRESHOLD) {
        /* Use uint8 indices */
        for (i = 0; i < value_count; i++) {
            output[out_idx++] = (uint8)indices[i];
        }
    } else {
        /* Use uint16 indices */
        for (i = 0; i < value_count; i++) {
            uint16 idx = (uint16)indices[i];
            memcpy(output + out_idx, &idx, sizeof(uint16));
            out_idx += sizeof(uint16);
        }
    }

    pfree(dict);
    pfree(hash_table);
    pfree(indices);

    return out_idx;
}

int64 decompress_dictionary(const char *input, int64 input_size, char *output, int64 expected_size)
{
    uint32 dict_size, value_count;
    const char *dict_start;
    const char *indices_start;
    char **dict = NULL;
    int *dict_lens = NULL;
    int64 out_idx = 0;
    int64 in_idx = 0;
    int i;

    /* Check minimum size for header */
    if (input_size < 8) {
        /* Not dictionary encoded, just copy */
        memcpy(output, input, input_size);
        return input_size;
    }

    /* Read header */
    memcpy(&dict_size, input, sizeof(uint32));
    memcpy(&value_count, input + sizeof(uint32), sizeof(uint32));
    in_idx = 8;

    /* Sanity check */
    if (dict_size == 0 || dict_size > DICT_MAX_ENTRIES || value_count == 0) {
        /* Not dictionary encoded, just copy */
        memcpy(output, input, input_size);
        return input_size;
    }

    /* Allocate dictionary pointers */
    dict = (char **)palloc0(dict_size * sizeof(char *));
    dict_lens = (int *)palloc0(dict_size * sizeof(int));

    /* Read dictionary strings */
    dict_start = input + in_idx;
    for (i = 0; i < (int)dict_size && in_idx < input_size; i++) {
        dict[i] = (char *)(input + in_idx);
        dict_lens[i] = strlen(dict[i]);
        in_idx += dict_lens[i] + 1;
    }

    indices_start = input + in_idx;

    /* Decode values */
    if (dict_size <= DICT_SMALL_THRESHOLD) {
        /* uint8 indices */
        for (i = 0; i < (int)value_count && in_idx < input_size; i++) {
            uint8 idx = (uint8)indices_start[i];
            if (idx < dict_size) {
                memcpy(output + out_idx, dict[idx], dict_lens[idx] + 1);
                out_idx += dict_lens[idx] + 1;
            }
        }
    } else {
        /* uint16 indices */
        for (i = 0; i < (int)value_count && in_idx + i * 2 + 1 < input_size; i++) {
            uint16 idx;
            memcpy(&idx, indices_start + i * 2, sizeof(uint16));
            if (idx < dict_size) {
                memcpy(output + out_idx, dict[idx], dict_lens[idx] + 1);
                out_idx += dict_lens[idx] + 1;
            }
        }
    }

    pfree(dict);
    pfree(dict_lens);

    return out_idx > 0 ? out_idx : expected_size;
}

/* ============================================================
 * Run-Length Encoding
 * ============================================================ */

int64 compress_rle(const char *input, int64 input_size, char *output, int64 max_output_size)
{
    const uint8 *in = (const uint8 *)input;
    uint8 *out = (uint8 *)output;
    int64 out_idx = 0;
    int64 i = 0;

    while (i < input_size && out_idx + 2 <= max_output_size) {
        uint8 val = in[i];
        int64 run_length = 1;

        /* Count consecutive identical values */
        while (i + run_length < input_size && in[i + run_length] == val && run_length < 255) {
            run_length++;
        }

        /* Store value and count */
        out[out_idx++] = val;
        out[out_idx++] = (uint8)run_length;

        i += run_length;
    }

    return out_idx;
}

int64 decompress_rle(const char *input, int64 input_size, char *output, int64 expected_size)
{
    const uint8 *in = (const uint8 *)input;
    uint8 *out = (uint8 *)output;
    int64 out_idx = 0;
    int64 i = 0;

    while (i + 1 < input_size && out_idx < expected_size) {
        uint8 val = in[i];
        uint8 count = in[i + 1];
        int64 j;

        for (j = 0; j < count && out_idx < expected_size; j++)
            out[out_idx++] = val;

        i += 2;
    }

    return out_idx;
}
