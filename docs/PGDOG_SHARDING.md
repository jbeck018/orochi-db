# PgDog Sharding with Orochi DB

This document describes how to configure PgDog for sharded query routing with Orochi DB distributed tables.

## Overview

Orochi DB uses hash-based sharding to distribute data across multiple PostgreSQL nodes. PgDog can be configured to route queries to the correct shard based on distribution column values, enabling efficient single-shard queries without requiring scatter-gather operations.

## Hash Algorithm

Orochi DB uses a CRC32-based hash algorithm for consistent data distribution.

### Algorithm Details

```
Algorithm: CRC32 IEEE 802.3
Polynomial: 0xEDB88320
Initial Value: 0xFFFFFFFF
Final XOR: 0xFFFFFFFF
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| OROCHI_HASH_SEED | 0x9747b28c | Reserved for future use |
| OROCHI_HASH_MODULUS | INT32_MAX | Maximum hash value |

### Type-Specific Hashing

Different data types are serialized before hashing:

| PostgreSQL Type | Go Type | Serialization |
|-----------------|---------|---------------|
| INT2 (smallint) | int16 | 2 bytes, little-endian |
| INT4 (integer) | int32 | 4 bytes, little-endian |
| INT8 (bigint) | int64 | 8 bytes, little-endian |
| TEXT/VARCHAR | string | UTF-8 bytes (no length prefix) |
| UUID | uuid.UUID | 16 bytes, raw binary |

### Shard Index Calculation

```go
shardIndex = uint32(hashValue) % uint32(shardCount)
```

The hash value is treated as unsigned for the modulo operation to ensure consistent positive shard indices.

## Go Package Usage

The `github.com/orochi-db/pgdog-router/pkg/hash` package provides hash functions compatible with Orochi DB.

### Installation

```bash
go get github.com/orochi-db/pgdog-router/pkg/hash
```

### Basic Usage

```go
import "github.com/orochi-db/pgdog-router/pkg/hash"

// Hash individual values
hash32 := hash.HashInt32(12345)           // Hash a 32-bit integer
hash64 := hash.HashInt64(1234567890123)   // Hash a 64-bit integer
hashText := hash.HashText("user@example.com") // Hash text
hashUUID := hash.HashUUID(myUUID)         // Hash a UUID

// Get shard index for a hash value
shardIndex := hash.GetShardIndex(hash32, 8) // 8 shards
```

### Using the Router

```go
import "github.com/orochi-db/pgdog-router/pkg/hash"

// Create a router with 8 shards
router := hash.NewRouter(8)

// Route values to shards
shard := router.RouteInt32(customerID)
shard := router.RouteInt64(orderID)
shard := router.RouteText(tenantID)
shard := router.RouteUUID(eventID)

// Generic routing
shard := router.Route(anyValue) // Returns -1 for unsupported types
```

## PgDog Configuration

### pgdog.toml Configuration

Configure sharding in PgDog's main configuration file:

```toml
[general]
host = "0.0.0.0"
port = 6432

# Sharding configuration
[sharding]
enabled = true
algorithm = "orochi_crc32"  # Use Orochi-compatible CRC32
default_shard_count = 8

# Define shards
[[shards]]
id = 0
host = "shard0.db.example.com"
port = 5432

[[shards]]
id = 1
host = "shard1.db.example.com"
port = 5432

# ... more shards

# Define distributed tables
[[distributed_tables]]
schema = "public"
table = "customers"
distribution_column = "customer_id"
distribution_type = "int4"
shard_count = 8
colocated_with = []

[[distributed_tables]]
schema = "public"
table = "orders"
distribution_column = "customer_id"
distribution_type = "int4"
shard_count = 8
colocated_with = ["public.customers"]  # Co-located for efficient JOINs

# Reference tables (replicated to all shards)
[[reference_tables]]
schema = "public"
table = "regions"
```

### Distribution Column Types

Supported distribution column types:

| Type | PgDog Config | Notes |
|------|--------------|-------|
| integer | int4 | Most common |
| bigint | int8 | For large IDs |
| text | text | String keys |
| varchar | text | Same as text |
| uuid | uuid | For UUID primary keys |
| smallint | int2 | Rarely used |

## Query Routing

### Single-Shard Queries

Queries with equality conditions on the distribution column route to a single shard:

```sql
-- Routes to single shard based on customer_id hash
SELECT * FROM customers WHERE customer_id = 42;
SELECT * FROM orders WHERE customer_id = 42;

-- UPDATE/DELETE also route to single shard
UPDATE customers SET name = 'New Name' WHERE customer_id = 42;
DELETE FROM orders WHERE customer_id = 42;
```

### Multi-Shard Queries (Scatter-Gather)

Queries without distribution column filters scatter to all shards:

```sql
-- Scatters to all shards, aggregates results
SELECT COUNT(*) FROM customers;
SELECT customer_id, SUM(total) FROM orders GROUP BY customer_id;
```

### Co-located JOINs

Tables co-located on the same distribution column can be joined efficiently:

```sql
-- Executed on each shard locally (no data movement)
SELECT c.name, o.total
FROM customers c
JOIN orders o ON c.customer_id = o.customer_id
WHERE c.customer_id = 42;
```

### Reference Table JOINs

Reference tables are replicated to all shards for efficient joins:

```sql
-- Reference table join (works on any shard)
SELECT c.name, r.region_name
FROM customers c
JOIN regions r ON c.region_id = r.region_id
WHERE c.customer_id = 42;
```

### Cross-Shard JOINs

JOINs between non-co-located tables require careful handling:

```sql
-- May require distributed execution
SELECT c.name, p.product_name
FROM customers c
JOIN products p ON c.preferred_product_id = p.product_id;
```

## Verifying Hash Compatibility

### SQL Verification

Run the SQL test script to generate hash values from Orochi DB:

```bash
psql -f tests/pgdog-integration/sharding_compatibility_test.sql > orochi_hashes.txt
```

### Go Verification

Compare with Go implementation:

```go
// Example verification code
value := int32(42)
goHash := hash.HashInt32(value)
goShard := hash.GetShardIndex(goHash, 8)

// Compare with Orochi: SELECT orochi.hash_value(42::int4), orochi.get_shard_index(orochi.hash_value(42::int4), 8)
```

### Integration Tests

Run the integration tests:

```bash
# Set environment variables
export PGDOG_DSN="postgres://postgres:password@localhost:6432/testdb?sslmode=disable"
export SHARD_COUNT=8

# Run tests
go test -v -tags=integration ./tests/pgdog-integration/...
```

## Migration Guide

### From pgcat

1. **Export table metadata**: Note distribution columns and shard counts
2. **Convert sharding function**: Replace pgcat's hash with Orochi CRC32
3. **Re-hash existing data**: May need to redistribute if hash algorithms differ
4. **Update routing configuration**: Configure PgDog with Orochi-compatible settings

### From Citus

1. **Map distribution columns**: Citus uses hash32 by default
2. **Verify co-location groups**: Ensure same tables are co-located
3. **Test hash compatibility**: Compare hash values for sample data
4. **Consider hybrid approach**: Use PgDog for routing, Citus for execution

### From Custom Sharding

1. **Document existing algorithm**: Understand current hash/routing logic
2. **Map to Orochi CRC32**: Implement conversion or re-sharding
3. **Test thoroughly**: Verify all queries route correctly
4. **Plan migration window**: May need coordinated data migration

## Performance Considerations

### Hash Function Performance

The CRC32 hash is highly optimized:

| Operation | Latency |
|-----------|---------|
| HashInt32 | ~15 ns |
| HashInt64 | ~20 ns |
| HashText (short) | ~30 ns |
| HashUUID | ~25 ns |
| GetShardIndex | ~5 ns |

### Routing Overhead

- Single-shard queries: Minimal overhead (~50 microseconds)
- Multi-shard queries: Proportional to shard count
- Reference table joins: Same as single-shard

### Best Practices

1. **Choose good distribution columns**: High cardinality, frequently filtered
2. **Co-locate related tables**: Enable efficient local joins
3. **Use reference tables wisely**: Small, frequently joined lookup tables
4. **Monitor shard balance**: Ensure even data distribution

## Troubleshooting

### Hash Mismatch

If queries route to wrong shards:

1. Verify data type matches (int4 vs int8)
2. Check byte ordering (little-endian)
3. Compare hash values directly
4. Ensure same shard count everywhere

### Uneven Distribution

If some shards have more data:

1. Check distribution column cardinality
2. Analyze actual hash distribution
3. Consider different distribution column
4. Use `orochi.get_rebalance_plan()` for recommendations

### Connection Issues

If routing fails:

1. Verify shard connectivity
2. Check PgDog logs for errors
3. Test direct shard connections
4. Verify authentication settings

## API Reference

### SQL Functions

```sql
-- Compute hash value
SELECT orochi.hash_value(42::int4);         -- Returns integer
SELECT orochi.hash_value('test'::text);     -- Returns integer
SELECT orochi.hash_value(uuid_val::uuid);   -- Returns integer

-- Get shard index
SELECT orochi.get_shard_index(hash_val, shard_count);

-- Get shard for value in table
SELECT orochi.get_shard_for_value('public.customers'::regclass, 42);
```

### Go Functions

```go
// Hash functions
hash.OrochiHash(data []byte) uint32
hash.HashInt16(value int16) int32
hash.HashInt32(value int32) int32
hash.HashInt64(value int64) int32
hash.HashText(value string) int32
hash.HashUUID(value uuid.UUID) int32
hash.HashBytes(data []byte) int32

// Shard routing
hash.GetShardIndex(hashValue int32, shardCount int32) int32
hash.GetShardIndexForInt32(value int32, shardCount int32) int32
hash.GetShardIndexForInt64(value int64, shardCount int32) int32
hash.GetShardIndexForText(value string, shardCount int32) int32
hash.GetShardIndexForUUID(value uuid.UUID, shardCount int32) int32

// Generic interface
hash.HashValue(value interface{}) (int32, bool)

// Router type
router := hash.NewRouter(shardCount int32)
router.ShardCount() int32
router.RouteInt32(value int32) int32
router.RouteInt64(value int64) int32
router.RouteText(value string) int32
router.RouteUUID(value uuid.UUID) int32
router.Route(value interface{}) int32
```

## Related Documentation

- [PgDog Implementation Strategy](./PGDOG_IMPLEMENTATION_STRATEGY.md)
- [PgDog Testing Strategy](./testing/PGDOG_TESTING_STRATEGY.md)
- [Orochi DB Extension Documentation](../extensions/postgres/docs/)
