/*-------------------------------------------------------------------------
 *
 * utils.c
 *    Orochi DB utility functions
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "fmgr.h"
#include "postgres.h"
#include "utils/builtins.h"

#include "../orochi.h"
#include "../sharding/distribution.h"

/*
 * orochi_compression_type_name
 *    Convert compression type enum to string
 */
char *orochi_compression_type_name(OrochiCompressionType type)
{
    switch (type) {
    case OROCHI_COMPRESS_NONE:
        return "none";
    case OROCHI_COMPRESS_LZ4:
        return "lz4";
    case OROCHI_COMPRESS_ZSTD:
        return "zstd";
    case OROCHI_COMPRESS_PGLZ:
        return "pglz";
    case OROCHI_COMPRESS_DELTA:
        return "delta";
    case OROCHI_COMPRESS_GORILLA:
        return "gorilla";
    case OROCHI_COMPRESS_DICTIONARY:
        return "dictionary";
    case OROCHI_COMPRESS_RLE:
        return "rle";
    default:
        return "unknown";
    }
}

/*
 * orochi_storage_tier_name
 *    Convert storage tier enum to string
 */
char *orochi_storage_tier_name(OrochiStorageTier tier)
{
    switch (tier) {
    case OROCHI_TIER_HOT:
        return "hot";
    case OROCHI_TIER_WARM:
        return "warm";
    case OROCHI_TIER_COLD:
        return "cold";
    case OROCHI_TIER_FROZEN:
        return "frozen";
    default:
        return "unknown";
    }
}

/*
 * orochi_hash_value
 *    Calculate hash value for distribution key
 *    This is a wrapper that calls the implementation in distribution.c
 */
int32 orochi_hash_value(Datum value, Oid type_oid)
{
    return orochi_hash_datum(value, type_oid);
}
