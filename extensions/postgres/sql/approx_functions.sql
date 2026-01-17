-- approx_functions.sql
-- Approximate Query Processing Functions for Orochi DB
--
-- This file defines:
--   - HyperLogLog for approximate count distinct
--   - T-Digest for approximate percentiles/quantiles
--   - Sampling operators for approximate aggregations
--
-- All algorithms are designed for parallel execution.
--
-- Copyright (c) 2024, Orochi DB Contributors

-- ============================================================
-- HyperLogLog Type
-- ============================================================

-- Create the HyperLogLog data type
CREATE TYPE hll;

CREATE FUNCTION hll_in(cstring)
RETURNS hll
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hll_out(hll)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE hll (
    INPUT = hll_in,
    OUTPUT = hll_out,
    STORAGE = extended
);

-- ============================================================
-- HyperLogLog Functions
-- ============================================================

-- Create a new HyperLogLog with specified precision
-- Higher precision = more accuracy but more memory
-- Default precision 14 gives ~0.81% error with 16KB memory
CREATE FUNCTION hll_create(precision integer DEFAULT 14)
RETURNS hll
AS 'MODULE_PATHNAME', 'hll_create_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Add an element to a HyperLogLog
CREATE FUNCTION hll_add(state hll, value anyelement)
RETURNS hll
AS 'MODULE_PATHNAME', 'hll_add_sql'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Get the estimated cardinality from a HyperLogLog
CREATE FUNCTION hll_count(state hll)
RETURNS bigint
AS 'MODULE_PATHNAME', 'hll_count_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Merge two HyperLogLogs (for combining partial results)
CREATE FUNCTION hll_merge(state1 hll, state2 hll)
RETURNS hll
AS 'MODULE_PATHNAME', 'hll_merge_sql'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- ============================================================
-- HyperLogLog Aggregate Support Functions
-- ============================================================

-- Transition function
CREATE FUNCTION hll_trans(state internal, value anyelement)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Combine function (for parallel aggregation)
CREATE FUNCTION hll_combine(state1 internal, state2 internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Final function
CREATE FUNCTION hll_final(state internal)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Serialization for parallel workers
CREATE FUNCTION hll_serial(state internal)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Deserialization for parallel workers
CREATE FUNCTION hll_deserial(data bytea, state internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- ============================================================
-- approx_count_distinct Aggregate
--
-- Usage:
--   SELECT approx_count_distinct(column_name) FROM table_name;
--
-- Returns an estimated count of distinct values using HyperLogLog.
-- Much faster than COUNT(DISTINCT) for large tables.
-- Error rate: ~0.81% (can be adjusted via underlying precision)
--
-- Parallel-safe: Uses combinefn for parallel query execution
-- ============================================================

CREATE AGGREGATE approx_count_distinct(anyelement) (
    SFUNC = hll_trans,
    STYPE = internal,
    FINALFUNC = hll_final,
    COMBINEFUNC = hll_combine,
    SERIALFUNC = hll_serial,
    DESERIALFUNC = hll_deserial,
    PARALLEL = SAFE
);

-- ============================================================
-- T-Digest Type
-- ============================================================

-- Create the T-Digest data type
CREATE TYPE tdigest;

CREATE FUNCTION tdigest_in(cstring)
RETURNS tdigest
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tdigest_out(tdigest)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE tdigest (
    INPUT = tdigest_in,
    OUTPUT = tdigest_out,
    STORAGE = extended
);

-- ============================================================
-- T-Digest Functions
-- ============================================================

-- Create a new T-Digest with specified compression
-- Higher compression = more accuracy but more memory
-- Default compression 100 gives good balance
CREATE FUNCTION tdigest_create(compression float8 DEFAULT 100.0)
RETURNS tdigest
AS 'MODULE_PATHNAME', 'tdigest_create_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Add a value to a T-Digest
CREATE FUNCTION tdigest_add(state tdigest, value float8)
RETURNS tdigest
AS 'MODULE_PATHNAME', 'tdigest_add_sql'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Get a quantile (0-1) from a T-Digest
-- Example: tdigest_quantile(td, 0.5) returns median
CREATE FUNCTION tdigest_quantile(state tdigest, q float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'tdigest_quantile_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Get a percentile (0-100) from a T-Digest
-- Example: tdigest_percentile(td, 95) returns 95th percentile
CREATE FUNCTION tdigest_percentile(state tdigest, p float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'tdigest_percentile_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Get the CDF value (fraction of values <= x)
CREATE FUNCTION tdigest_cdf(state tdigest, x float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'tdigest_cdf_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Merge two T-Digests
CREATE FUNCTION tdigest_merge(state1 tdigest, state2 tdigest)
RETURNS tdigest
AS 'MODULE_PATHNAME', 'tdigest_merge_sql'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Get summary information about a T-Digest
CREATE FUNCTION tdigest_info(state tdigest)
RETURNS text
AS 'MODULE_PATHNAME', 'tdigest_info_sql'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ============================================================
-- T-Digest Aggregate Support Functions
-- ============================================================

-- Transition function
CREATE FUNCTION tdigest_trans(state internal, value float8)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Combine function (for parallel aggregation)
CREATE FUNCTION tdigest_combine(state1 internal, state2 internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Final function returning percentile value
CREATE FUNCTION tdigest_final_percentile(state internal, p float8)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Final function returning the T-Digest itself
CREATE FUNCTION tdigest_final_agg(state internal)
RETURNS tdigest
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Serialization
CREATE FUNCTION tdigest_serial(state internal)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- Deserialization
CREATE FUNCTION tdigest_deserial(data bytea, state internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- ============================================================
-- approx_percentile Aggregate
--
-- Usage:
--   SELECT approx_percentile(column_name, 0.95) FROM table_name;
--   SELECT approx_percentile(column_name, 50) FROM table_name;  -- 50th percentile
--
-- Returns an estimated percentile value using T-Digest.
-- Much faster than exact percentile for large tables.
-- Error: typically < 1% relative error
--
-- Parallel-safe: Uses combinefn for parallel query execution
-- ============================================================

-- Create a wrapper final function that handles the percentile parameter
CREATE FUNCTION approx_percentile_final(state internal, p float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'tdigest_final_percentile'
LANGUAGE C PARALLEL SAFE;

-- Aggregate that takes percentile as second argument
CREATE AGGREGATE approx_percentile(float8, float8) (
    SFUNC = tdigest_trans,
    STYPE = internal,
    FINALFUNC = approx_percentile_final,
    FINALFUNC_EXTRA,
    COMBINEFUNC = tdigest_combine,
    SERIALFUNC = tdigest_serial,
    DESERIALFUNC = tdigest_deserial,
    PARALLEL = SAFE
);

-- ============================================================
-- approx_median Aggregate
--
-- Usage:
--   SELECT approx_median(column_name) FROM table_name;
--
-- Convenience aggregate for the 50th percentile (median)
-- ============================================================

CREATE FUNCTION approx_median_final(state internal)
RETURNS float8 AS $$
    SELECT tdigest_final_percentile(state, 0.5);
$$ LANGUAGE SQL PARALLEL SAFE;

CREATE AGGREGATE approx_median(float8) (
    SFUNC = tdigest_trans,
    STYPE = internal,
    FINALFUNC = approx_median_final,
    COMBINEFUNC = tdigest_combine,
    SERIALFUNC = tdigest_serial,
    DESERIALFUNC = tdigest_deserial,
    PARALLEL = SAFE
);

-- ============================================================
-- tdigest_agg Aggregate
--
-- Returns the T-Digest itself for further operations
--
-- Usage:
--   WITH td AS (SELECT tdigest_agg(value) as digest FROM data)
--   SELECT tdigest_percentile(digest, 0.99) FROM td;
-- ============================================================

CREATE AGGREGATE tdigest_agg(float8) (
    SFUNC = tdigest_trans,
    STYPE = internal,
    FINALFUNC = tdigest_final_agg,
    COMBINEFUNC = tdigest_combine,
    SERIALFUNC = tdigest_serial,
    DESERIALFUNC = tdigest_deserial,
    PARALLEL = SAFE
);

-- ============================================================
-- Sampling Functions
-- ============================================================

-- Set random seed for reproducible sampling
CREATE FUNCTION sampling_set_seed(seed bigint)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE STRICT;

-- Generate a random number (0-1) for sampling
CREATE FUNCTION sampling_random()
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

-- Bernoulli sampling function
-- Returns true with the specified probability
CREATE FUNCTION sampling_bernoulli(probability float8)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- ============================================================
-- Reservoir Sampling Aggregate Support Functions
-- ============================================================

CREATE FUNCTION reservoir_sample_trans(state internal, value float8, reservoir_size integer DEFAULT 1000)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION reservoir_sample_final(state internal)
RETURNS float8[]
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION reservoir_sample_combine(state1 internal, state2 internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

-- ============================================================
-- reservoir_sample Aggregate
--
-- Usage:
--   SELECT reservoir_sample(column_name, 1000) FROM table_name;
--
-- Returns an array of uniformly sampled values.
-- Useful for computing statistics on a sample.
-- ============================================================

CREATE AGGREGATE reservoir_sample(float8, integer) (
    SFUNC = reservoir_sample_trans,
    STYPE = internal,
    FINALFUNC = reservoir_sample_final,
    COMBINEFUNC = reservoir_sample_combine,
    PARALLEL = SAFE
);

-- Default reservoir size version
CREATE AGGREGATE reservoir_sample(float8) (
    SFUNC = reservoir_sample_trans,
    STYPE = internal,
    FINALFUNC = reservoir_sample_final,
    COMBINEFUNC = reservoir_sample_combine,
    PARALLEL = SAFE
);

-- ============================================================
-- Convenience Functions
-- ============================================================

-- Quick count distinct estimate on a table
CREATE FUNCTION quick_count_distinct(
    table_name regclass,
    column_name text
) RETURNS bigint AS $$
DECLARE
    result bigint;
BEGIN
    EXECUTE format('SELECT approx_count_distinct(%I) FROM %s', column_name, table_name)
    INTO result;
    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- Quick percentile estimate on a table
CREATE FUNCTION quick_percentile(
    table_name regclass,
    column_name text,
    percentile float8
) RETURNS float8 AS $$
DECLARE
    result float8;
BEGIN
    EXECUTE format('SELECT approx_percentile(%I, %L) FROM %s',
                   column_name, percentile, table_name)
    INTO result;
    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- Quick median estimate on a table
CREATE FUNCTION quick_median(
    table_name regclass,
    column_name text
) RETURNS float8 AS $$
BEGIN
    RETURN quick_percentile(table_name, column_name, 0.5);
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Sample Scan Operator
--
-- Create a sampled view of a table using Bernoulli sampling.
-- Much faster than scanning the full table.
-- ============================================================

CREATE FUNCTION create_sample_view(
    source_table regclass,
    sample_rate float8,
    view_name text DEFAULT NULL
) RETURNS text AS $$
DECLARE
    actual_view_name text;
BEGIN
    -- Generate view name if not provided
    actual_view_name := COALESCE(view_name,
        source_table::text || '_sample_' || (sample_rate * 100)::integer);

    -- Create the view with sampling condition
    EXECUTE format(
        'CREATE OR REPLACE VIEW %I AS SELECT * FROM %s WHERE sampling_bernoulli(%L)',
        actual_view_name, source_table, sample_rate
    );

    RETURN actual_view_name;
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Approximate Statistics View
--
-- Compute approximate statistics for all numeric columns
-- ============================================================

CREATE FUNCTION approx_table_stats(
    table_name regclass
) RETURNS TABLE (
    column_name text,
    approx_distinct bigint,
    approx_median float8,
    approx_p95 float8,
    approx_p99 float8
) AS $$
DECLARE
    col record;
BEGIN
    FOR col IN
        SELECT a.attname, a.atttypid
        FROM pg_attribute a
        WHERE a.attrelid = table_name
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND a.atttypid IN (21, 23, 20, 700, 701)  -- smallint, int, bigint, float4, float8
    LOOP
        RETURN QUERY EXECUTE format(
            'SELECT %L::text,
                    approx_count_distinct(%I),
                    approx_percentile(%I::float8, 0.5),
                    approx_percentile(%I::float8, 0.95),
                    approx_percentile(%I::float8, 0.99)
             FROM %s',
            col.attname, col.attname, col.attname, col.attname, col.attname, table_name
        );
    END LOOP;
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Documentation Comments
-- ============================================================

COMMENT ON FUNCTION approx_count_distinct(anyelement) IS
'Estimate count of distinct values using HyperLogLog algorithm.
Returns approximate count with ~0.81% standard error.
Much faster than COUNT(DISTINCT) for large tables.
Parallel-safe for use with parallel query execution.

Example:
  SELECT approx_count_distinct(user_id) FROM events;';

COMMENT ON AGGREGATE approx_percentile(float8, float8) IS
'Estimate percentile using T-Digest algorithm.
Second argument is percentile (0-1 or 0-100).
Returns approximate value with typically < 1% relative error.
Parallel-safe for use with parallel query execution.

Examples:
  SELECT approx_percentile(response_time, 0.95) FROM requests;
  SELECT approx_percentile(price, 50) FROM products;  -- median';

COMMENT ON AGGREGATE approx_median(float8) IS
'Estimate median (50th percentile) using T-Digest algorithm.
Equivalent to approx_percentile(column, 0.5).
Parallel-safe for use with parallel query execution.

Example:
  SELECT approx_median(salary) FROM employees;';

COMMENT ON AGGREGATE reservoir_sample(float8) IS
'Uniform random sample using reservoir sampling (Algorithm R).
Returns array of sampled values.
Useful for computing statistics on a sample.
Parallel-safe for use with parallel query execution.

Example:
  SELECT reservoir_sample(price, 1000) FROM products;';

COMMENT ON FUNCTION sampling_bernoulli(float8) IS
'Bernoulli sampling: returns true with specified probability.
Use in WHERE clause to sample rows.

Example:
  SELECT * FROM large_table WHERE sampling_bernoulli(0.01);  -- 1% sample';

COMMENT ON TYPE hll IS
'HyperLogLog data type for cardinality estimation.
Stores a probabilistic sketch of distinct values.
Memory usage: 2^precision bytes (default 16KB).
Use hll_create(), hll_add(), hll_count(), hll_merge().';

COMMENT ON TYPE tdigest IS
'T-Digest data type for quantile estimation.
Stores centroids approximating the distribution.
Memory usage: O(compression) centroids.
Use tdigest_create(), tdigest_add(), tdigest_quantile(), tdigest_merge().';
