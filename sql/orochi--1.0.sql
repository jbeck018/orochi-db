-- orochi--1.0.sql
-- Orochi DB SQL interface - User-facing functions and types
-- This file is loaded when CREATE EXTENSION orochi is executed.
-- Copyright (c) 2024, Orochi DB Contributors

-- Note: Schema 'orochi' is automatically created by the extension framework
-- due to 'schema = orochi' in orochi.control

-- ============================================================
-- Vector Data Type
-- ============================================================

CREATE TYPE vector;

CREATE FUNCTION vector_in(cstring, oid, integer)
RETURNS vector
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_out(vector)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_typmod_in(cstring[])
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_typmod_out(integer)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE vector (
    INPUT = vector_in,
    OUTPUT = vector_out,
    TYPMOD_IN = vector_typmod_in,
    TYPMOD_OUT = vector_typmod_out,
    STORAGE = extended
);

-- ============================================================
-- Vector Functions
-- ============================================================

CREATE FUNCTION vector_dims(vector) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_norm(vector) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION l2_distance(vector, vector) RETURNS double precision
    AS 'MODULE_PATHNAME', 'vector_l2_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cosine_distance(vector, vector) RETURNS double precision
    AS 'MODULE_PATHNAME', 'vector_cosine_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION inner_product(vector, vector) RETURNS double precision
    AS 'MODULE_PATHNAME', 'vector_inner_product' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_add(vector, vector) RETURNS vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_sub(vector, vector) RETURNS vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_normalize(vector) RETURNS vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Vector operators
CREATE OPERATOR + (
    LEFTARG = vector,
    RIGHTARG = vector,
    FUNCTION = vector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = vector,
    RIGHTARG = vector,
    FUNCTION = vector_sub
);

CREATE OPERATOR <-> (
    LEFTARG = vector,
    RIGHTARG = vector,
    FUNCTION = l2_distance,
    COMMUTATOR = <->
);

CREATE OPERATOR <=> (
    LEFTARG = vector,
    RIGHTARG = vector,
    FUNCTION = cosine_distance,
    COMMUTATOR = <=>
);

-- ============================================================
-- Distributed Table Functions
-- ============================================================

CREATE FUNCTION create_distributed_table(
    table_name regclass,
    distribution_column text,
    shard_count integer DEFAULT NULL,
    colocate_with regclass DEFAULT NULL
) RETURNS void AS $$
DECLARE
    v_shard_count integer;
BEGIN
    -- Use default if not specified
    v_shard_count := COALESCE(shard_count, current_setting('orochi.default_shard_count')::integer);

    -- Register table as distributed
    PERFORM orochi._create_distributed_table(table_name, distribution_column, 0, v_shard_count);

    RAISE NOTICE 'Created distributed table % with % shards on column %',
        table_name, v_shard_count, distribution_column;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_distributed_table(
    table_oid oid,
    distribution_column text,
    strategy integer,
    shard_count integer
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_create_distributed_table_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION create_reference_table(table_name regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._create_reference_table(table_name);
    RAISE NOTICE 'Created reference table %', table_name;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_reference_table(table_oid oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_create_reference_table_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION undistribute_table(table_name regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._undistribute_table(table_name);
    RAISE NOTICE 'Undistributed table %', table_name;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._undistribute_table(table_oid oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_undistribute_table_sql'
    LANGUAGE C STRICT;

-- ============================================================
-- Shard Routing Helper Functions
-- These are used by triggers to route data to correct shards
-- ============================================================

-- Compute a stable hash value for routing (works with any type)
CREATE FUNCTION hash_value(anyelement)
RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_hash_value_sql'
    LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Get the shard index (0-based) for a hash value
CREATE FUNCTION get_shard_index(hash_value integer, shard_count integer)
RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_get_shard_index_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Get the shard index for a value in a specific distributed table
CREATE FUNCTION get_shard_for_value(table_name regclass, value anyelement)
RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_get_shard_for_value_sql'
    LANGUAGE C STABLE PARALLEL SAFE;

-- ============================================================
-- Hypertable Functions
-- ============================================================

CREATE FUNCTION create_hypertable(
    relation regclass,
    time_column_name text,
    chunk_time_interval interval DEFAULT INTERVAL '1 day',
    if_not_exists boolean DEFAULT FALSE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._create_hypertable(relation, time_column_name, chunk_time_interval, if_not_exists);
    RAISE NOTICE 'Created hypertable % with time column % and chunk interval %',
        relation, time_column_name, chunk_time_interval;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_hypertable(
    table_oid oid,
    time_column text,
    chunk_interval interval,
    if_not_exists boolean
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_create_hypertable_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION add_dimension(
    hypertable regclass,
    column_name text,
    number_partitions integer DEFAULT NULL,
    chunk_time_interval interval DEFAULT NULL
) RETURNS void AS $$
BEGIN
    PERFORM orochi._add_dimension(hypertable, column_name,
                                  COALESCE(number_partitions, 4),
                                  chunk_time_interval);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._add_dimension(
    hypertable_oid oid,
    column_name text,
    num_partitions integer,
    interval_val interval
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_add_dimension_sql'
    LANGUAGE C;

-- ============================================================
-- Time Bucketing Functions
-- ============================================================

CREATE FUNCTION time_bucket(
    bucket_width interval,
    ts timestamp
) RETURNS timestamp
    AS 'MODULE_PATHNAME', 'orochi_time_bucket_timestamp'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION time_bucket(
    bucket_width interval,
    ts timestamptz
) RETURNS timestamptz
    AS 'MODULE_PATHNAME', 'orochi_time_bucket_timestamptz'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION time_bucket(
    bucket_width interval,
    ts timestamptz,
    origin timestamptz
) RETURNS timestamptz
    AS 'MODULE_PATHNAME', 'orochi_time_bucket_timestamptz_origin'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION time_bucket(
    bucket_width bigint,
    ts bigint
) RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_time_bucket_int_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ============================================================
-- Chunk Management Functions
-- ============================================================

CREATE FUNCTION compress_chunk(chunk regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._compress_chunk(chunk);
    RAISE NOTICE 'Compressed chunk %', chunk;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._compress_chunk(chunk_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_compress_chunk_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION decompress_chunk(chunk regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._decompress_chunk(chunk);
    RAISE NOTICE 'Decompressed chunk %', chunk;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._decompress_chunk(chunk_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_decompress_chunk_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION drop_chunks(
    relation regclass,
    older_than interval DEFAULT NULL,
    newer_than interval DEFAULT NULL
) RETURNS integer AS $$
DECLARE
    dropped integer;
BEGIN
    SELECT orochi._drop_chunks(relation, older_than, newer_than) INTO dropped;
    RAISE NOTICE 'Dropped % chunks from %', dropped, relation;
    RETURN dropped;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_chunks(
    hypertable_oid oid,
    older_than interval,
    newer_than interval
) RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_drop_chunks_sql'
    LANGUAGE C;

-- ============================================================
-- Compression Policy Functions
-- ============================================================

CREATE FUNCTION add_compression_policy(
    hypertable regclass,
    compress_after interval
) RETURNS integer AS $$
DECLARE
    policy_id integer;
BEGIN
    SELECT orochi._add_compression_policy(hypertable, compress_after) INTO policy_id;
    RAISE NOTICE 'Added compression policy % for %', policy_id, hypertable;
    RETURN policy_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._add_compression_policy(
    hypertable_oid oid,
    compress_after interval
) RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_add_compression_policy_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION remove_compression_policy(hypertable regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._remove_compression_policy(hypertable);
    RAISE NOTICE 'Removed compression policy from %', hypertable;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._remove_compression_policy(hypertable_oid oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_remove_compression_policy_sql'
    LANGUAGE C STRICT;

-- ============================================================
-- Retention Policy Functions
-- ============================================================

CREATE FUNCTION add_retention_policy(
    relation regclass,
    drop_after interval
) RETURNS integer AS $$
DECLARE
    policy_id integer;
BEGIN
    SELECT orochi._add_retention_policy(relation, drop_after) INTO policy_id;
    RAISE NOTICE 'Added retention policy % for %', policy_id, relation;
    RETURN policy_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._add_retention_policy(
    hypertable_oid oid,
    drop_after interval
) RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_add_retention_policy_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION remove_retention_policy(relation regclass)
RETURNS void AS $$
BEGIN
    PERFORM orochi._remove_retention_policy(relation);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._remove_retention_policy(hypertable_oid oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_remove_retention_policy_sql'
    LANGUAGE C STRICT;

-- ============================================================
-- Tiered Storage Functions
-- ============================================================

CREATE FUNCTION set_tiering_policy(
    relation regclass,
    hot_after interval DEFAULT INTERVAL '1 day',
    warm_after interval DEFAULT INTERVAL '7 days',
    cold_after interval DEFAULT INTERVAL '30 days'
) RETURNS void AS $$
BEGIN
    PERFORM orochi._set_tiering_policy(relation, hot_after, warm_after, cold_after);
    RAISE NOTICE 'Set tiering policy for %', relation;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._set_tiering_policy(
    table_oid oid,
    hot_after interval,
    warm_after interval,
    cold_after interval
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_set_tiering_policy_sql'
    LANGUAGE C;

CREATE FUNCTION move_chunk_to_tier(
    chunk regclass,
    tier text
) RETURNS void AS $$
BEGIN
    PERFORM orochi._move_chunk_to_tier(chunk, tier);
    RAISE NOTICE 'Moved chunk % to % tier', chunk, tier;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._move_chunk_to_tier(chunk_id bigint, tier text)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_move_chunk_to_tier_sql'
    LANGUAGE C STRICT;

-- ============================================================
-- Node Management Functions
-- ============================================================

CREATE FUNCTION add_node(
    hostname text,
    port integer DEFAULT 5432
) RETURNS integer AS $$
DECLARE
    node_id integer;
BEGIN
    SELECT orochi._add_node(hostname, port) INTO node_id;
    RAISE NOTICE 'Added node %:% with ID %', hostname, port, node_id;
    RETURN node_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._add_node(hostname text, port integer)
RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_add_node_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION remove_node(node_id integer)
RETURNS void AS $$
BEGIN
    PERFORM orochi._remove_node(node_id);
    RAISE NOTICE 'Removed node %', node_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._remove_node(node_id integer)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_remove_node_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION drain_node(node_id integer)
RETURNS void AS $$
BEGIN
    PERFORM orochi._drain_node(node_id);
    RAISE NOTICE 'Drained node %', node_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drain_node(node_id integer)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drain_node_sql'
    LANGUAGE C STRICT;

CREATE FUNCTION rebalance_shards(
    relation regclass DEFAULT NULL,
    strategy text DEFAULT 'by_shard_count'
) RETURNS void AS $$
BEGIN
    PERFORM orochi._rebalance_shards(relation, strategy);
    RAISE NOTICE 'Rebalanced shards';
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._rebalance_shards(table_oid oid, strategy text)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_rebalance_shards_sql'
    LANGUAGE C;

-- ============================================================
-- Catalog Tables
-- ============================================================

-- Main table registry
CREATE TABLE IF NOT EXISTS orochi.orochi_tables (
    table_oid OID PRIMARY KEY,
    schema_name TEXT NOT NULL,
    table_name TEXT NOT NULL,
    storage_type INTEGER NOT NULL DEFAULT 0,
    shard_strategy INTEGER NOT NULL DEFAULT 0,
    shard_count INTEGER NOT NULL DEFAULT 0,
    distribution_column TEXT,
    is_distributed BOOLEAN NOT NULL DEFAULT FALSE,
    is_timeseries BOOLEAN NOT NULL DEFAULT FALSE,
    time_column TEXT,
    chunk_interval INTERVAL,
    compression INTEGER NOT NULL DEFAULT 0,
    compression_level INTEGER NOT NULL DEFAULT 3,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Shard metadata
CREATE TABLE IF NOT EXISTS orochi.orochi_shards (
    shard_id BIGSERIAL PRIMARY KEY,
    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,
    shard_index INTEGER NOT NULL,
    hash_min INTEGER NOT NULL,
    hash_max INTEGER NOT NULL,
    node_id INTEGER,
    row_count BIGINT NOT NULL DEFAULT 0,
    size_bytes BIGINT NOT NULL DEFAULT 0,
    storage_tier INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_accessed TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(table_oid, shard_index)
);

-- Cluster nodes
CREATE TABLE IF NOT EXISTS orochi.orochi_nodes (
    node_id SERIAL PRIMARY KEY,
    hostname TEXT NOT NULL,
    port INTEGER NOT NULL DEFAULT 5432,
    role INTEGER NOT NULL DEFAULT 1,
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    shard_count BIGINT NOT NULL DEFAULT 0,
    total_size BIGINT NOT NULL DEFAULT 0,
    cpu_usage DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    memory_usage DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    last_heartbeat TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(hostname, port)
);

-- Time-series chunks
CREATE TABLE IF NOT EXISTS orochi.orochi_chunks (
    chunk_id BIGSERIAL PRIMARY KEY,
    hypertable_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,
    dimension_id INTEGER NOT NULL DEFAULT 0,
    chunk_table_oid OID,
    range_start TIMESTAMPTZ NOT NULL,
    range_end TIMESTAMPTZ NOT NULL,
    row_count BIGINT NOT NULL DEFAULT 0,
    size_bytes BIGINT NOT NULL DEFAULT 0,
    is_compressed BOOLEAN NOT NULL DEFAULT FALSE,
    storage_tier INTEGER NOT NULL DEFAULT 0,
    tablespace_name TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(hypertable_oid, range_start)
);

-- Indexes for performance
CREATE INDEX IF NOT EXISTS idx_shards_table ON orochi.orochi_shards(table_oid);
CREATE INDEX IF NOT EXISTS idx_shards_node ON orochi.orochi_shards(node_id);
CREATE INDEX IF NOT EXISTS idx_chunks_hypertable ON orochi.orochi_chunks(hypertable_oid);
CREATE INDEX IF NOT EXISTS idx_chunks_range ON orochi.orochi_chunks(range_start, range_end);

-- ============================================================
-- Information Views
-- ============================================================

CREATE VIEW orochi.tables AS
SELECT
    table_oid,
    schema_name,
    table_name,
    CASE storage_type
        WHEN 0 THEN 'row'
        WHEN 1 THEN 'columnar'
        WHEN 2 THEN 'hybrid'
        WHEN 3 THEN 'vector'
    END as storage_type,
    CASE shard_strategy
        WHEN 0 THEN 'hash'
        WHEN 1 THEN 'range'
        WHEN 2 THEN 'list'
        WHEN 3 THEN 'composite'
        WHEN 4 THEN 'reference'
    END as shard_strategy,
    shard_count,
    distribution_column,
    is_distributed,
    is_timeseries,
    time_column
FROM orochi.orochi_tables;

CREATE VIEW orochi.shards AS
SELECT
    s.shard_id,
    t.schema_name || '.' || t.table_name as table_name,
    s.shard_index,
    s.hash_min,
    s.hash_max,
    n.hostname || ':' || n.port as node,
    s.row_count,
    pg_size_pretty(s.size_bytes) as size,
    CASE s.storage_tier
        WHEN 0 THEN 'hot'
        WHEN 1 THEN 'warm'
        WHEN 2 THEN 'cold'
        WHEN 3 THEN 'frozen'
    END as tier,
    s.last_accessed
FROM orochi.orochi_shards s
JOIN orochi.orochi_tables t ON s.table_oid = t.table_oid
LEFT JOIN orochi.orochi_nodes n ON s.node_id = n.node_id;

CREATE VIEW orochi.chunks AS
SELECT
    c.chunk_id,
    t.schema_name || '.' || t.table_name as hypertable,
    c.range_start,
    c.range_end,
    c.row_count,
    pg_size_pretty(c.size_bytes) as size,
    c.is_compressed,
    CASE c.storage_tier
        WHEN 0 THEN 'hot'
        WHEN 1 THEN 'warm'
        WHEN 2 THEN 'cold'
        WHEN 3 THEN 'frozen'
    END as tier
FROM orochi.orochi_chunks c
JOIN orochi.orochi_tables t ON c.hypertable_oid = t.table_oid;

CREATE VIEW orochi.nodes AS
SELECT
    node_id,
    hostname,
    port,
    CASE role
        WHEN 0 THEN 'coordinator'
        WHEN 1 THEN 'worker'
        WHEN 2 THEN 'replica'
    END as role,
    is_active,
    shard_count,
    pg_size_pretty(total_size) as total_size,
    round(cpu_usage::numeric, 2) as cpu_usage,
    round(memory_usage::numeric, 2) as memory_usage,
    last_heartbeat
FROM orochi.orochi_nodes;

-- ============================================================
-- Utility Functions
-- ============================================================

CREATE FUNCTION orochi_version() RETURNS text AS $$
    SELECT '1.0.0'::text;
$$ LANGUAGE sql IMMUTABLE;

CREATE FUNCTION approximate_row_count(relation regclass)
RETURNS bigint AS $$
    SELECT reltuples::bigint FROM pg_class WHERE oid = relation;
$$ LANGUAGE sql STABLE;

-- Run maintenance (compression, tiering, aggregates refresh)
CREATE FUNCTION run_maintenance()
RETURNS void AS $$
BEGIN
    PERFORM orochi._run_maintenance();
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._run_maintenance()
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_run_maintenance_sql'
    LANGUAGE C;
