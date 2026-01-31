-- =============================================================================
-- PgDog/Orochi Hash Compatibility Test Script
-- =============================================================================
-- This script verifies that the Go hash implementation in PgDog produces
-- identical hash values to Orochi DB's native hash_value() function.
--
-- Prerequisites:
--   1. PostgreSQL 16+ with Orochi extension installed
--   2. CREATE EXTENSION orochi; executed
--
-- Usage:
--   psql -f sharding_compatibility_test.sql
--
-- The output should be saved and compared against the Go test results.
-- =============================================================================

\echo '=================================='
\echo 'Orochi Hash Compatibility Tests'
\echo '=================================='
\echo ''

-- Verify extension is loaded
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'orochi') THEN
        RAISE EXCEPTION 'Orochi extension not installed. Run: CREATE EXTENSION orochi;';
    END IF;
END $$;

\echo 'Extension Version:'
SELECT orochi.orochi_version();
\echo ''

-- =============================================================================
-- Test 1: INT4 (int32) Hash Values
-- =============================================================================
\echo '=================================='
\echo 'Test 1: INT4 (int32) Hash Values'
\echo '=================================='

-- Create a table for test values
CREATE TEMP TABLE int4_hash_tests (
    test_name TEXT,
    test_value INT4,
    hash_value INT4,
    shard_4 INT4,
    shard_8 INT4,
    shard_16 INT4,
    shard_32 INT4
);

-- Insert test values and compute hashes
INSERT INTO int4_hash_tests
SELECT
    test_name,
    test_value,
    orochi.hash_value(test_value) as hash_value,
    orochi.get_shard_index(orochi.hash_value(test_value), 4) as shard_4,
    orochi.get_shard_index(orochi.hash_value(test_value), 8) as shard_8,
    orochi.get_shard_index(orochi.hash_value(test_value), 16) as shard_16,
    orochi.get_shard_index(orochi.hash_value(test_value), 32) as shard_32
FROM (VALUES
    ('zero', 0::INT4),
    ('one', 1::INT4),
    ('minus_one', -1::INT4),
    ('positive_100', 100::INT4),
    ('negative_100', -100::INT4),
    ('positive_large', 123456789::INT4),
    ('negative_large', -123456789::INT4),
    ('max_int32', 2147483647::INT4),
    ('min_int32', -2147483648::INT4),
    ('user_id_42', 42::INT4),
    ('user_id_1001', 1001::INT4),
    ('typical_pk_1', 12345::INT4),
    ('typical_pk_2', 67890::INT4)
) AS t(test_name, test_value);

\echo 'INT4 Hash Results:'
SELECT * FROM int4_hash_tests ORDER BY test_name;

-- Output in Go test format
\echo ''
\echo '// Go test data for INT4:'
SELECT format(
    'testCases = append(testCases, hashTestCase{name: "%s", value: int32(%s), expectedHash: %s, expectedShard4: %s})',
    test_name,
    test_value::TEXT,
    hash_value::TEXT,
    shard_4::TEXT
) FROM int4_hash_tests ORDER BY test_name;
\echo ''

-- =============================================================================
-- Test 2: INT8 (int64/bigint) Hash Values
-- =============================================================================
\echo '=================================='
\echo 'Test 2: INT8 (int64/bigint) Hash Values'
\echo '=================================='

CREATE TEMP TABLE int8_hash_tests (
    test_name TEXT,
    test_value INT8,
    hash_value INT4,
    shard_4 INT4,
    shard_8 INT4,
    shard_16 INT4,
    shard_32 INT4
);

INSERT INTO int8_hash_tests
SELECT
    test_name,
    test_value,
    orochi.hash_value(test_value) as hash_value,
    orochi.get_shard_index(orochi.hash_value(test_value), 4) as shard_4,
    orochi.get_shard_index(orochi.hash_value(test_value), 8) as shard_8,
    orochi.get_shard_index(orochi.hash_value(test_value), 16) as shard_16,
    orochi.get_shard_index(orochi.hash_value(test_value), 32) as shard_32
FROM (VALUES
    ('zero', 0::INT8),
    ('one', 1::INT8),
    ('minus_one', -1::INT8),
    ('bigint_positive', 1234567890123456789::INT8),
    ('bigint_negative', -1234567890123456789::INT8),
    ('max_int64', 9223372036854775807::INT8),
    ('min_int64', -9223372036854775808::INT8),
    ('js_max_safe', 9007199254740992::INT8),
    ('js_over_safe', 9007199254740993::INT8),
    ('typical_snowflake', 1234567890123456::INT8)
) AS t(test_name, test_value);

\echo 'INT8 Hash Results:'
SELECT * FROM int8_hash_tests ORDER BY test_name;

-- Output in Go test format
\echo ''
\echo '// Go test data for INT8:'
SELECT format(
    'testCases = append(testCases, hashTestCase{name: "%s", value: int64(%s), expectedHash: %s, expectedShard4: %s})',
    test_name,
    test_value::TEXT,
    hash_value::TEXT,
    shard_4::TEXT
) FROM int8_hash_tests ORDER BY test_name;
\echo ''

-- =============================================================================
-- Test 3: TEXT/VARCHAR Hash Values
-- =============================================================================
\echo '=================================='
\echo 'Test 3: TEXT/VARCHAR Hash Values'
\echo '=================================='

CREATE TEMP TABLE text_hash_tests (
    test_name TEXT,
    test_value TEXT,
    hash_value INT4,
    shard_4 INT4,
    shard_8 INT4,
    shard_16 INT4,
    shard_32 INT4
);

INSERT INTO text_hash_tests
SELECT
    test_name,
    test_value,
    orochi.hash_value(test_value) as hash_value,
    orochi.get_shard_index(orochi.hash_value(test_value), 4) as shard_4,
    orochi.get_shard_index(orochi.hash_value(test_value), 8) as shard_8,
    orochi.get_shard_index(orochi.hash_value(test_value), 16) as shard_16,
    orochi.get_shard_index(orochi.hash_value(test_value), 32) as shard_32
FROM (VALUES
    ('empty', ''::TEXT),
    ('single_a', 'a'::TEXT),
    ('hello', 'hello'::TEXT),
    ('hello_world', 'hello world'::TEXT),
    ('test', 'test'::TEXT),
    ('email', 'user@example.com'::TEXT),
    ('uuid_string', '550e8400-e29b-41d4-a716-446655440000'::TEXT),
    ('special_chars', '!@#$%^&*()'::TEXT),
    ('whitespace', '  spaces  '::TEXT),
    ('long_string', 'this is a much longer string that might be used as a distribution key'::TEXT),
    ('numeric_string', '123456789'::TEXT),
    ('mixed_case', 'HelloWorld'::TEXT),
    ('tenant_id', 'tenant_abc123'::TEXT),
    ('partition_key', 'pk_2024_01'::TEXT)
) AS t(test_name, test_value);

\echo 'TEXT Hash Results:'
SELECT * FROM text_hash_tests ORDER BY test_name;

-- Output in Go test format (with proper escaping)
\echo ''
\echo '// Go test data for TEXT:'
SELECT format(
    'testCases = append(testCases, hashTestCase{name: "%s", value: "%s", expectedHash: %s, expectedShard4: %s})',
    test_name,
    replace(test_value, '"', '\"'),
    hash_value::TEXT,
    shard_4::TEXT
) FROM text_hash_tests ORDER BY test_name;
\echo ''

-- =============================================================================
-- Test 4: UUID Hash Values
-- =============================================================================
\echo '=================================='
\echo 'Test 4: UUID Hash Values'
\echo '=================================='

CREATE TEMP TABLE uuid_hash_tests (
    test_name TEXT,
    test_value UUID,
    hash_value INT4,
    shard_4 INT4,
    shard_8 INT4,
    shard_16 INT4,
    shard_32 INT4
);

INSERT INTO uuid_hash_tests
SELECT
    test_name,
    test_value,
    orochi.hash_value(test_value) as hash_value,
    orochi.get_shard_index(orochi.hash_value(test_value), 4) as shard_4,
    orochi.get_shard_index(orochi.hash_value(test_value), 8) as shard_8,
    orochi.get_shard_index(orochi.hash_value(test_value), 16) as shard_16,
    orochi.get_shard_index(orochi.hash_value(test_value), 32) as shard_32
FROM (VALUES
    ('nil_uuid', '00000000-0000-0000-0000-000000000000'::UUID),
    ('uuid_v4_1', '550e8400-e29b-41d4-a716-446655440000'::UUID),
    ('uuid_v4_2', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::UUID),
    ('uuid_v4_3', '6ba7b810-9dad-11d1-80b4-00c04fd430c8'::UUID),
    ('max_uuid', 'ffffffff-ffff-ffff-ffff-ffffffffffff'::UUID),
    ('random_1', 'f47ac10b-58cc-4372-a567-0e02b2c3d479'::UUID),
    ('random_2', '7c9e6679-7425-40de-944b-e07fc1f90ae7'::UUID)
) AS t(test_name, test_value);

\echo 'UUID Hash Results:'
SELECT * FROM uuid_hash_tests ORDER BY test_name;

-- Output in Go test format
\echo ''
\echo '// Go test data for UUID:'
SELECT format(
    'testCases = append(testCases, hashTestCase{name: "%s", value: uuid.MustParse("%s"), expectedHash: %s, expectedShard4: %s})',
    test_name,
    test_value::TEXT,
    hash_value::TEXT,
    shard_4::TEXT
) FROM uuid_hash_tests ORDER BY test_name;
\echo ''

-- =============================================================================
-- Test 5: INT2 (int16/smallint) Hash Values
-- =============================================================================
\echo '=================================='
\echo 'Test 5: INT2 (int16/smallint) Hash Values'
\echo '=================================='

CREATE TEMP TABLE int2_hash_tests (
    test_name TEXT,
    test_value INT2,
    hash_value INT4,
    shard_4 INT4,
    shard_8 INT4,
    shard_16 INT4,
    shard_32 INT4
);

INSERT INTO int2_hash_tests
SELECT
    test_name,
    test_value,
    orochi.hash_value(test_value) as hash_value,
    orochi.get_shard_index(orochi.hash_value(test_value), 4) as shard_4,
    orochi.get_shard_index(orochi.hash_value(test_value), 8) as shard_8,
    orochi.get_shard_index(orochi.hash_value(test_value), 16) as shard_16,
    orochi.get_shard_index(orochi.hash_value(test_value), 32) as shard_32
FROM (VALUES
    ('zero', 0::INT2),
    ('one', 1::INT2),
    ('minus_one', -1::INT2),
    ('positive', 12345::INT2),
    ('negative', -12345::INT2),
    ('max_int16', 32767::INT2),
    ('min_int16', -32768::INT2)
) AS t(test_name, test_value);

\echo 'INT2 Hash Results:'
SELECT * FROM int2_hash_tests ORDER BY test_name;

-- Output in Go test format
\echo ''
\echo '// Go test data for INT2:'
SELECT format(
    'testCases = append(testCases, hashTestCase{name: "%s", value: int16(%s), expectedHash: %s, expectedShard4: %s})',
    test_name,
    test_value::TEXT,
    hash_value::TEXT,
    shard_4::TEXT
) FROM int2_hash_tests ORDER BY test_name;
\echo ''

-- =============================================================================
-- Test 6: Distributed Table Simulation
-- =============================================================================
\echo '=================================='
\echo 'Test 6: Distributed Table Simulation'
\echo '=================================='

-- Simulate a distributed table with 8 shards
\echo 'Simulating orders table with 8 shards:'

CREATE TEMP TABLE simulated_orders (
    order_id BIGINT PRIMARY KEY,
    customer_id INT4,
    total NUMERIC(10,2),
    created_at TIMESTAMP DEFAULT now()
);

-- Insert sample data
INSERT INTO simulated_orders (order_id, customer_id, total)
SELECT
    i,
    (i % 1000) + 1,
    (random() * 1000)::NUMERIC(10,2)
FROM generate_series(1, 100) AS i;

-- Show shard distribution based on customer_id
\echo 'Shard distribution for customer_id (8 shards):'
SELECT
    orochi.get_shard_index(orochi.hash_value(customer_id), 8) as shard,
    count(*) as row_count,
    array_agg(DISTINCT customer_id ORDER BY customer_id) as customer_ids
FROM simulated_orders
GROUP BY orochi.get_shard_index(orochi.hash_value(customer_id), 8)
ORDER BY shard;

-- =============================================================================
-- Test 7: Cross-Shard Query Simulation
-- =============================================================================
\echo ''
\echo '=================================='
\echo 'Test 7: Cross-Shard Query Simulation'
\echo '=================================='

-- Simulate query routing decisions
\echo 'Query routing examples:'

SELECT
    'Single shard (customer_id=42)' as query_type,
    orochi.hash_value(42::INT4) as hash_value,
    orochi.get_shard_index(orochi.hash_value(42::INT4), 8) as target_shard;

SELECT
    'Single shard (customer_id=100)' as query_type,
    orochi.hash_value(100::INT4) as hash_value,
    orochi.get_shard_index(orochi.hash_value(100::INT4), 8) as target_shard;

SELECT
    'Single shard (email=user@example.com)' as query_type,
    orochi.hash_value('user@example.com'::TEXT) as hash_value,
    orochi.get_shard_index(orochi.hash_value('user@example.com'::TEXT), 8) as target_shard;

-- =============================================================================
-- Test 8: Verify Shard Index Boundaries
-- =============================================================================
\echo ''
\echo '=================================='
\echo 'Test 8: Shard Index Boundaries'
\echo '=================================='

\echo 'Verifying shard indices are within valid range [0, shard_count-1]:'

DO $$
DECLARE
    invalid_count INT := 0;
BEGIN
    -- Test with 1000 random integers
    SELECT COUNT(*) INTO invalid_count
    FROM generate_series(1, 1000) AS i
    WHERE orochi.get_shard_index(orochi.hash_value(i::INT4), 8) < 0
       OR orochi.get_shard_index(orochi.hash_value(i::INT4), 8) >= 8;

    IF invalid_count > 0 THEN
        RAISE EXCEPTION 'Found % invalid shard indices!', invalid_count;
    ELSE
        RAISE NOTICE 'All 1000 test values produced valid shard indices (0-7)';
    END IF;
END $$;

-- =============================================================================
-- Summary
-- =============================================================================
\echo ''
\echo '=================================='
\echo 'Test Summary'
\echo '=================================='

SELECT 'INT4 tests' as test_type, count(*) as count FROM int4_hash_tests
UNION ALL
SELECT 'INT8 tests', count(*) FROM int8_hash_tests
UNION ALL
SELECT 'TEXT tests', count(*) FROM text_hash_tests
UNION ALL
SELECT 'UUID tests', count(*) FROM uuid_hash_tests
UNION ALL
SELECT 'INT2 tests', count(*) FROM int2_hash_tests
ORDER BY test_type;

\echo ''
\echo 'Tests completed. Compare output with Go implementation results.'
\echo ''
