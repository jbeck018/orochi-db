/*-------------------------------------------------------------------------
 *
 * compression.h
 *    Orochi DB compression algorithms interface
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef OROCHI_COMPRESSION_H
#define OROCHI_COMPRESSION_H

#include "../orochi.h"
#include "postgres.h"

/* LZ4 compression */
extern int64 compress_lz4(const char *input, int64 input_size, char *output,
                          int64 max_output_size);
extern int64 decompress_lz4(const char *input, int64 input_size, char *output,
                            int64 expected_size);

/* ZSTD compression */
extern int64 compress_zstd(const char *input, int64 input_size, char *output,
                           int64 max_output_size, int level);
extern int64 decompress_zstd(const char *input, int64 input_size, char *output,
                             int64 expected_size);

/* PostgreSQL native compression */
extern int64 compress_pglz(const char *input, int64 input_size, char *output,
                           int64 max_output_size);
extern int64 decompress_pglz(const char *input, int64 input_size, char *output,
                             int64 expected_size);

/* Delta encoding for integers */
extern int64 compress_delta(const char *input, int64 input_size, char *output,
                            int64 max_output_size);
extern int64 decompress_delta(const char *input, int64 input_size, char *output,
                              int64 expected_size);

/* Gorilla compression for floats */
extern int64 compress_gorilla(const char *input, int64 input_size, char *output,
                              int64 max_output_size);
extern int64 decompress_gorilla(const char *input, int64 input_size,
                                char *output, int64 expected_size);

/* Dictionary encoding */
extern int64 compress_dictionary(const char *input, int64 input_size,
                                 char *output, int64 max_output_size);
extern int64 decompress_dictionary(const char *input, int64 input_size,
                                   char *output, int64 expected_size);

/* Run-length encoding */
extern int64 compress_rle(const char *input, int64 input_size, char *output,
                          int64 max_output_size);
extern int64 decompress_rle(const char *input, int64 input_size, char *output,
                            int64 expected_size);

#endif /* OROCHI_COMPRESSION_H */
