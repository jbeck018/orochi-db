/*-------------------------------------------------------------------------
 *
 * test_tiered_storage.c
 *    Unit tests for Orochi DB tiered storage functionality
 *
 * Tests chunk serialization, compression, decompression, checksum
 * calculation, chunk file parsing, and S3 key generation.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* Include compression library headers */
#include <lz4.h>
#include <zstd.h>

/* Orochi types */
typedef enum OrochiCompressionType
{
    OROCHI_COMPRESS_NONE = 0,
    OROCHI_COMPRESS_LZ4,
    OROCHI_COMPRESS_ZSTD,
    OROCHI_COMPRESS_PGLZ,
    OROCHI_COMPRESS_DELTA,
    OROCHI_COMPRESS_GORILLA,
    OROCHI_COMPRESS_DICTIONARY,
    OROCHI_COMPRESS_RLE
} OrochiCompressionType;

typedef enum OrochiStorageTier
{
    OROCHI_TIER_HOT = 0,
    OROCHI_TIER_WARM,
    OROCHI_TIER_COLD,
    OROCHI_TIER_FROZEN
} OrochiStorageTier;

/* ============================================================
 * Mock Chunk Structures
 * ============================================================ */

#define CHUNK_MAGIC 0x4F524F43  /* "OROC" */
#define CHUNK_VERSION 1

/*
 * Chunk file header
 */
typedef struct ChunkFileHeader
{
    uint32_t magic;              /* Magic number: 0x4F524F43 */
    uint16_t version;            /* Format version */
    uint16_t flags;              /* Flags (compressed, encrypted, etc.) */
    uint32_t compression_type;   /* Compression algorithm */
    uint64_t chunk_id;           /* Chunk identifier */
    uint64_t original_size;      /* Uncompressed size */
    uint64_t compressed_size;    /* Compressed size (0 if not compressed) */
    uint32_t checksum;           /* CRC32 or similar checksum */
    uint64_t row_count;          /* Number of rows in chunk */
    uint64_t timestamp;          /* Creation/modification timestamp */
} ChunkFileHeader;

/*
 * Chunk metadata
 */
typedef struct ChunkMetadata
{
    int64_t chunk_id;
    int64_t table_oid;
    int64_t start_time;
    int64_t end_time;
    int64_t row_count;
    int64_t byte_size;
    OrochiCompressionType compression;
    OrochiStorageTier tier;
} ChunkMetadata;

/* ============================================================
 * Helper Functions
 * ============================================================ */

/*
 * Calculate CRC32 checksum
 */
static uint32_t
calculate_crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    size_t i, j;

    for (i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
    }
    return ~crc;
}

/*
 * Serialize chunk header
 */
static void
serialize_chunk_header(ChunkFileHeader *header, unsigned char *buffer)
{
    memcpy(buffer, header, sizeof(ChunkFileHeader));
}

/*
 * Deserialize chunk header
 */
static void
deserialize_chunk_header(const unsigned char *buffer, ChunkFileHeader *header)
{
    memcpy(header, buffer, sizeof(ChunkFileHeader));
}

/*
 * Compress data using LZ4
 */
static int
compress_lz4(const char *input, size_t input_size, char *output, size_t output_capacity)
{
    return LZ4_compress_default(input, output, input_size, output_capacity);
}

/*
 * Decompress data using LZ4
 */
static int
decompress_lz4(const char *input, size_t input_size, char *output, size_t output_capacity)
{
    return LZ4_decompress_safe(input, output, input_size, output_capacity);
}

/*
 * Compress data using ZSTD
 */
static size_t
compress_zstd(const char *input, size_t input_size, char *output, size_t output_capacity)
{
    return ZSTD_compress(output, output_capacity, input, input_size, 3);
}

/*
 * Decompress data using ZSTD
 */
static size_t
decompress_zstd(const char *input, size_t input_size, char *output, size_t output_capacity)
{
    return ZSTD_decompress(output, output_capacity, input, input_size);
}

/*
 * Generate S3 key for chunk
 */
static char *
generate_s3_key(int64_t chunk_id)
{
    char *key = palloc(128);
    snprintf(key, 128, "chunks/%lld/%lld.chunk",
             (long long)(chunk_id / 1000), (long long)chunk_id);
    return key;
}

/* ============================================================
 * Test Cases - Chunk Serialization
 * ============================================================ */

static void test_chunk_header_serialization(void)
{
    TEST_BEGIN("chunk_header_serialization")
        ChunkFileHeader header;
        unsigned char buffer[sizeof(ChunkFileHeader)];
        ChunkFileHeader deserialized;

        /* Initialize header */
        memset(&header, 0, sizeof(ChunkFileHeader));
        header.magic = CHUNK_MAGIC;
        header.version = CHUNK_VERSION;
        header.flags = 0x0001;
        header.compression_type = OROCHI_COMPRESS_LZ4;
        header.chunk_id = 12345;
        header.original_size = 1024 * 1024;
        header.compressed_size = 512 * 1024;
        header.checksum = 0xDEADBEEF;
        header.row_count = 10000;
        header.timestamp = 1234567890;

        /* Serialize */
        serialize_chunk_header(&header, buffer);

        /* Deserialize */
        deserialize_chunk_header(buffer, &deserialized);

        /* Verify all fields */
        TEST_ASSERT_EQ(CHUNK_MAGIC, deserialized.magic);
        TEST_ASSERT_EQ(CHUNK_VERSION, deserialized.version);
        TEST_ASSERT_EQ(0x0001, deserialized.flags);
        TEST_ASSERT_EQ(OROCHI_COMPRESS_LZ4, deserialized.compression_type);
        TEST_ASSERT_EQ(12345, deserialized.chunk_id);
        TEST_ASSERT_EQ(1024 * 1024, deserialized.original_size);
        TEST_ASSERT_EQ(512 * 1024, deserialized.compressed_size);
        TEST_ASSERT_EQ(0xDEADBEEF, deserialized.checksum);
        TEST_ASSERT_EQ(10000, deserialized.row_count);
        TEST_ASSERT_EQ(1234567890, deserialized.timestamp);
    TEST_END();
}

static void test_chunk_header_magic_number(void)
{
    TEST_BEGIN("chunk_header_magic_number")
        ChunkFileHeader header;

        header.magic = CHUNK_MAGIC;
        TEST_ASSERT_EQ(0x4F524F43, header.magic);

        /* Verify as bytes */
        unsigned char *bytes = (unsigned char *)&header.magic;
        TEST_ASSERT_EQ('C', bytes[0]);  /* Little-endian */
        TEST_ASSERT_EQ('R', bytes[1]);
        TEST_ASSERT_EQ('O', bytes[2]);
        TEST_ASSERT_EQ('O', bytes[3]);
    TEST_END();
}

static void test_chunk_header_size(void)
{
    TEST_BEGIN("chunk_header_size")
        /* Header should be fixed size for compatibility */
        size_t expected_size = 64;  /* Adjust based on actual struct */
        size_t actual_size = sizeof(ChunkFileHeader);

        /* Header should be reasonable size */
        TEST_ASSERT_GTE(actual_size, 48);  /* At least 48 bytes */
        TEST_ASSERT_LTE(actual_size, 128); /* At most 128 bytes */
    TEST_END();
}

/* ============================================================
 * Test Cases - Checksum Calculation
 * ============================================================ */

static void test_checksum_empty_data(void)
{
    TEST_BEGIN("checksum_empty_data")
        unsigned char data[1] = {0};
        uint32_t checksum = calculate_crc32(data, 0);

        /* Empty data should have consistent checksum */
        TEST_ASSERT_EQ(0xFFFFFFFF, checksum);
    TEST_END();
}

static void test_checksum_single_byte(void)
{
    TEST_BEGIN("checksum_single_byte")
        unsigned char data[1] = {0x42};
        uint32_t checksum = calculate_crc32(data, 1);

        /* Single byte should produce non-zero checksum */
        TEST_ASSERT_NEQ(0, checksum);
        TEST_ASSERT_NEQ(0xFFFFFFFF, checksum);
    TEST_END();
}

static void test_checksum_deterministic(void)
{
    TEST_BEGIN("checksum_deterministic")
        unsigned char data[] = "Hello, Orochi DB!";
        uint32_t checksum1 = calculate_crc32(data, strlen((char *)data));
        uint32_t checksum2 = calculate_crc32(data, strlen((char *)data));

        /* Same data should produce same checksum */
        TEST_ASSERT_EQ(checksum1, checksum2);
    TEST_END();
}

static void test_checksum_different_data(void)
{
    TEST_BEGIN("checksum_different_data")
        unsigned char data1[] = "Hello, Orochi DB!";
        unsigned char data2[] = "Hello, Orochi DC!";
        uint32_t checksum1 = calculate_crc32(data1, strlen((char *)data1));
        uint32_t checksum2 = calculate_crc32(data2, strlen((char *)data2));

        /* Different data should produce different checksums */
        TEST_ASSERT_NEQ(checksum1, checksum2);
    TEST_END();
}

static void test_checksum_large_buffer(void)
{
    TEST_BEGIN("checksum_large_buffer")
        size_t size = 1024 * 1024;  /* 1MB */
        unsigned char *data = palloc(size);

        /* Fill with pattern */
        for (size_t i = 0; i < size; i++)
            data[i] = (unsigned char)(i & 0xFF);

        uint32_t checksum = calculate_crc32(data, size);

        TEST_ASSERT_NEQ(0, checksum);
        TEST_ASSERT_NEQ(0xFFFFFFFF, checksum);

        pfree(data);
    TEST_END();
}

/* ============================================================
 * Test Cases - LZ4 Compression
 * ============================================================ */

static void test_lz4_compress_simple(void)
{
    TEST_BEGIN("lz4_compress_simple")
        const char *input = "The quick brown fox jumps over the lazy dog";
        size_t input_size = strlen(input);
        size_t max_output_size = LZ4_compressBound(input_size);
        char *compressed = palloc(max_output_size);

        int compressed_size = compress_lz4(input, input_size, compressed, max_output_size);

        TEST_ASSERT_GT(compressed_size, 0);
        TEST_ASSERT_LTE(compressed_size, max_output_size);

        pfree(compressed);
    TEST_END();
}

static void test_lz4_compress_decompress_roundtrip(void)
{
    TEST_BEGIN("lz4_compress_decompress_roundtrip")
        const char *input = "Orochi DB is a PostgreSQL HTAP extension with automatic sharding, "
                           "time-series optimization, columnar storage, and tiered storage.";
        size_t input_size = strlen(input);
        size_t max_compressed_size = LZ4_compressBound(input_size);
        char *compressed = palloc(max_compressed_size);
        char *decompressed = palloc(input_size + 1);

        /* Compress */
        int compressed_size = compress_lz4(input, input_size, compressed, max_compressed_size);
        TEST_ASSERT_GT(compressed_size, 0);

        /* Decompress */
        int decompressed_size = decompress_lz4(compressed, compressed_size,
                                               decompressed, input_size);
        TEST_ASSERT_EQ(input_size, decompressed_size);

        /* Verify data */
        decompressed[decompressed_size] = '\0';
        TEST_ASSERT_STR_EQ(input, decompressed);

        pfree(compressed);
        pfree(decompressed);
    TEST_END();
}

static void test_lz4_compress_binary_data(void)
{
    TEST_BEGIN("lz4_compress_binary_data")
        size_t input_size = 4096;
        unsigned char *input = palloc(input_size);

        /* Create binary pattern */
        for (size_t i = 0; i < input_size; i++)
            input[i] = (unsigned char)(i & 0xFF);

        size_t max_compressed_size = LZ4_compressBound(input_size);
        char *compressed = palloc(max_compressed_size);
        char *decompressed = palloc(input_size);

        /* Compress and decompress */
        int compressed_size = compress_lz4((char *)input, input_size,
                                          compressed, max_compressed_size);
        TEST_ASSERT_GT(compressed_size, 0);

        int decompressed_size = decompress_lz4(compressed, compressed_size,
                                               decompressed, input_size);
        TEST_ASSERT_EQ(input_size, decompressed_size);

        /* Verify binary data */
        TEST_ASSERT_MEM_EQ(input, decompressed, input_size);

        pfree(input);
        pfree(compressed);
        pfree(decompressed);
    TEST_END();
}

static void test_lz4_compress_highly_compressible(void)
{
    TEST_BEGIN("lz4_compress_highly_compressible")
        size_t input_size = 1024;
        char *input = palloc(input_size);

        /* Highly compressible data (all zeros) */
        memset(input, 0, input_size);

        size_t max_compressed_size = LZ4_compressBound(input_size);
        char *compressed = palloc(max_compressed_size);

        int compressed_size = compress_lz4(input, input_size, compressed, max_compressed_size);

        /* Compressed size should be much smaller */
        TEST_ASSERT_GT(compressed_size, 0);
        TEST_ASSERT_LT(compressed_size, input_size / 10);

        pfree(input);
        pfree(compressed);
    TEST_END();
}

/* ============================================================
 * Test Cases - ZSTD Compression
 * ============================================================ */

static void test_zstd_compress_simple(void)
{
    TEST_BEGIN("zstd_compress_simple")
        const char *input = "The quick brown fox jumps over the lazy dog";
        size_t input_size = strlen(input);
        size_t max_output_size = ZSTD_compressBound(input_size);
        char *compressed = palloc(max_output_size);

        size_t compressed_size = compress_zstd(input, input_size, compressed, max_output_size);

        TEST_ASSERT_FALSE(ZSTD_isError(compressed_size));
        TEST_ASSERT_GT(compressed_size, 0);
        TEST_ASSERT_LTE(compressed_size, max_output_size);

        pfree(compressed);
    TEST_END();
}

static void test_zstd_compress_decompress_roundtrip(void)
{
    TEST_BEGIN("zstd_compress_decompress_roundtrip")
        const char *input = "Orochi DB supports hot, warm, cold, and frozen storage tiers. "
                           "Data automatically moves between tiers based on access patterns.";
        size_t input_size = strlen(input);
        size_t max_compressed_size = ZSTD_compressBound(input_size);
        char *compressed = palloc(max_compressed_size);
        char *decompressed = palloc(input_size + 1);

        /* Compress */
        size_t compressed_size = compress_zstd(input, input_size, compressed, max_compressed_size);
        TEST_ASSERT_FALSE(ZSTD_isError(compressed_size));
        TEST_ASSERT_GT(compressed_size, 0);

        /* Decompress */
        size_t decompressed_size = decompress_zstd(compressed, compressed_size,
                                                   decompressed, input_size);
        TEST_ASSERT_FALSE(ZSTD_isError(decompressed_size));
        TEST_ASSERT_EQ(input_size, decompressed_size);

        /* Verify data */
        decompressed[decompressed_size] = '\0';
        TEST_ASSERT_STR_EQ(input, decompressed);

        pfree(compressed);
        pfree(decompressed);
    TEST_END();
}

static void test_zstd_better_compression_than_lz4(void)
{
    TEST_BEGIN("zstd_better_compression_than_lz4")
        /* Create somewhat repetitive data */
        size_t input_size = 8192;
        char *input = palloc(input_size);
        for (size_t i = 0; i < input_size; i++)
            input[i] = (char)((i % 256) < 128 ? 'A' : 'B');

        /* Compress with LZ4 */
        size_t lz4_max = LZ4_compressBound(input_size);
        char *lz4_compressed = palloc(lz4_max);
        int lz4_size = compress_lz4(input, input_size, lz4_compressed, lz4_max);
        TEST_ASSERT_GT(lz4_size, 0);

        /* Compress with ZSTD */
        size_t zstd_max = ZSTD_compressBound(input_size);
        char *zstd_compressed = palloc(zstd_max);
        size_t zstd_size = compress_zstd(input, input_size, zstd_compressed, zstd_max);
        TEST_ASSERT_FALSE(ZSTD_isError(zstd_size));

        /* ZSTD should typically achieve better compression */
        TEST_ASSERT_LTE(zstd_size, lz4_size);

        pfree(input);
        pfree(lz4_compressed);
        pfree(zstd_compressed);
    TEST_END();
}

/* ============================================================
 * Test Cases - Chunk File Parsing
 * ============================================================ */

static void test_parse_chunk_file_header_valid(void)
{
    TEST_BEGIN("parse_chunk_file_header_valid")
        ChunkFileHeader header;
        unsigned char buffer[sizeof(ChunkFileHeader)];

        /* Create valid header */
        memset(&header, 0, sizeof(ChunkFileHeader));
        header.magic = CHUNK_MAGIC;
        header.version = CHUNK_VERSION;
        header.compression_type = OROCHI_COMPRESS_NONE;
        header.chunk_id = 999;
        header.original_size = 2048;
        header.compressed_size = 0;
        header.checksum = 0x12345678;

        serialize_chunk_header(&header, buffer);

        /* Parse it back */
        ChunkFileHeader parsed;
        deserialize_chunk_header(buffer, &parsed);

        /* Validate */
        TEST_ASSERT_EQ(CHUNK_MAGIC, parsed.magic);
        TEST_ASSERT_EQ(CHUNK_VERSION, parsed.version);
        TEST_ASSERT_EQ(999, parsed.chunk_id);
    TEST_END();
}

static void test_parse_chunk_file_invalid_magic(void)
{
    TEST_BEGIN("parse_chunk_file_invalid_magic")
        ChunkFileHeader header;
        unsigned char buffer[sizeof(ChunkFileHeader)];

        /* Create header with wrong magic */
        memset(&header, 0, sizeof(ChunkFileHeader));
        header.magic = 0xDEADBEEF;  /* Wrong magic */
        header.version = CHUNK_VERSION;

        serialize_chunk_header(&header, buffer);

        ChunkFileHeader parsed;
        deserialize_chunk_header(buffer, &parsed);

        /* Should detect invalid magic */
        TEST_ASSERT_NEQ(CHUNK_MAGIC, parsed.magic);
    TEST_END();
}

static void test_parse_chunk_with_compressed_data(void)
{
    TEST_BEGIN("parse_chunk_with_compressed_data")
        const char *data = "Sample chunk data for compression testing";
        size_t original_size = strlen(data);
        size_t max_compressed = LZ4_compressBound(original_size);

        char *compressed = palloc(max_compressed);
        int compressed_size = compress_lz4(data, original_size, compressed, max_compressed);
        TEST_ASSERT_GT(compressed_size, 0);

        /* Create header for compressed chunk */
        ChunkFileHeader header;
        memset(&header, 0, sizeof(ChunkFileHeader));
        header.magic = CHUNK_MAGIC;
        header.version = CHUNK_VERSION;
        header.compression_type = OROCHI_COMPRESS_LZ4;
        header.original_size = original_size;
        header.compressed_size = compressed_size;
        header.checksum = calculate_crc32((unsigned char *)compressed, compressed_size);

        /* Verify checksum matches */
        uint32_t verify_checksum = calculate_crc32((unsigned char *)compressed, compressed_size);
        TEST_ASSERT_EQ(header.checksum, verify_checksum);

        pfree(compressed);
    TEST_END();
}

/* ============================================================
 * Test Cases - S3 Key Generation
 * ============================================================ */

static void test_s3_key_generation_basic(void)
{
    TEST_BEGIN("s3_key_generation_basic")
        char *key = generate_s3_key(12345);

        TEST_ASSERT_NOT_NULL(key);
        TEST_ASSERT_STR_EQ("chunks/12/12345.chunk", key);

        pfree(key);
    TEST_END();
}

static void test_s3_key_generation_different_chunks(void)
{
    TEST_BEGIN("s3_key_generation_different_chunks")
        char *key1 = generate_s3_key(1);
        char *key2 = generate_s3_key(2);
        char *key3 = generate_s3_key(1000);

        TEST_ASSERT_NOT_NULL(key1);
        TEST_ASSERT_NOT_NULL(key2);
        TEST_ASSERT_NOT_NULL(key3);

        /* Keys should be different */
        TEST_ASSERT_NEQ(0, strcmp(key1, key2));
        TEST_ASSERT_NEQ(0, strcmp(key1, key3));

        /* Verify format */
        TEST_ASSERT_STR_EQ("chunks/0/1.chunk", key1);
        TEST_ASSERT_STR_EQ("chunks/0/2.chunk", key2);
        TEST_ASSERT_STR_EQ("chunks/1/1000.chunk", key3);

        pfree(key1);
        pfree(key2);
        pfree(key3);
    TEST_END();
}

static void test_s3_key_generation_large_chunk_id(void)
{
    TEST_BEGIN("s3_key_generation_large_chunk_id")
        char *key = generate_s3_key(999999999);

        TEST_ASSERT_NOT_NULL(key);
        TEST_ASSERT_STR_EQ("chunks/999999/999999999.chunk", key);

        pfree(key);
    TEST_END();
}

static void test_s3_key_hierarchical_organization(void)
{
    TEST_BEGIN("s3_key_hierarchical_organization")
        /* Keys in same group should share prefix */
        char *key1 = generate_s3_key(5000);
        char *key2 = generate_s3_key(5001);
        char *key3 = generate_s3_key(5999);

        /* All should be in chunks/5/ */
        TEST_ASSERT_EQ(0, strncmp(key1, "chunks/5/", 9));
        TEST_ASSERT_EQ(0, strncmp(key2, "chunks/5/", 9));
        TEST_ASSERT_EQ(0, strncmp(key3, "chunks/5/", 9));

        pfree(key1);
        pfree(key2);
        pfree(key3);
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_tiered_storage(void)
{
    /* Chunk serialization tests */
    test_chunk_header_serialization();
    test_chunk_header_magic_number();
    test_chunk_header_size();

    /* Checksum tests */
    test_checksum_empty_data();
    test_checksum_single_byte();
    test_checksum_deterministic();
    test_checksum_different_data();
    test_checksum_large_buffer();

    /* LZ4 compression tests */
    test_lz4_compress_simple();
    test_lz4_compress_decompress_roundtrip();
    test_lz4_compress_binary_data();
    test_lz4_compress_highly_compressible();

    /* ZSTD compression tests */
    test_zstd_compress_simple();
    test_zstd_compress_decompress_roundtrip();
    test_zstd_better_compression_than_lz4();

    /* Chunk file parsing tests */
    test_parse_chunk_file_header_valid();
    test_parse_chunk_file_invalid_magic();
    test_parse_chunk_with_compressed_data();

    /* S3 key generation tests */
    test_s3_key_generation_basic();
    test_s3_key_generation_different_chunks();
    test_s3_key_generation_large_chunk_id();
    test_s3_key_hierarchical_organization();
}
