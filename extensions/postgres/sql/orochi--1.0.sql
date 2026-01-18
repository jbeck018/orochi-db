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

    -- Validate shard count
    IF v_shard_count IS NULL OR v_shard_count <= 0 THEN
        RAISE EXCEPTION 'shard_count must be a positive integer, got %', v_shard_count;
    END IF;

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

-- Co-location groups for distributed tables
CREATE TABLE IF NOT EXISTS orochi.orochi_colocation_groups (
    group_id SERIAL PRIMARY KEY,
    shard_count INTEGER NOT NULL,
    shard_strategy INTEGER NOT NULL DEFAULT 0,
    distribution_type TEXT,
    replication_factor INTEGER NOT NULL DEFAULT 1,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Shard movement tracking
CREATE TABLE IF NOT EXISTS orochi.orochi_shard_moves (
    move_id BIGSERIAL PRIMARY KEY,
    shard_id BIGINT NOT NULL,
    source_node_id INTEGER NOT NULL,
    target_node_id INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0, -- 0=pending, 1=copying, 2=completed, 3=failed
    bytes_copied BIGINT NOT NULL DEFAULT 0,
    bytes_total BIGINT NOT NULL DEFAULT 0,
    started_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    completed_at TIMESTAMPTZ,
    error_message TEXT
);

CREATE INDEX IF NOT EXISTS idx_shard_moves_shard ON orochi.orochi_shard_moves(shard_id);
CREATE INDEX IF NOT EXISTS idx_shard_moves_status ON orochi.orochi_shard_moves(status) WHERE status < 2;

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

-- Hypertable dimensions (time and space partitioning)
CREATE TABLE IF NOT EXISTS orochi.orochi_dimensions (
    dimension_id SERIAL PRIMARY KEY,
    hypertable_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,
    column_name TEXT NOT NULL,
    dimension_type INTEGER NOT NULL DEFAULT 0,  -- 0=time, 1=space/hash
    num_partitions INTEGER NOT NULL DEFAULT 1,
    interval_length BIGINT,  -- For time dimensions, in microseconds
    aligned BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(hypertable_oid, column_name)
);

-- Hypertable policies (compression, retention, tiering)
CREATE TABLE IF NOT EXISTS orochi.orochi_policies (
    policy_id SERIAL PRIMARY KEY,
    hypertable_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,
    policy_type INTEGER NOT NULL,  -- 0=compression, 1=retention, 2=tiering
    trigger_interval INTERVAL NOT NULL,
    config JSONB,  -- Additional policy-specific configuration
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    last_run TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(hypertable_oid, policy_type)
);

-- Columnar storage stripes
CREATE TABLE IF NOT EXISTS orochi.orochi_stripes (
    stripe_id BIGSERIAL PRIMARY KEY,
    table_oid OID NOT NULL REFERENCES orochi.orochi_tables(table_oid) ON DELETE CASCADE,
    first_row_number BIGINT NOT NULL,
    row_count BIGINT NOT NULL,
    column_count INTEGER NOT NULL,
    chunk_group_count INTEGER NOT NULL DEFAULT 1,
    data_offset BIGINT,
    data_size BIGINT,
    metadata_offset BIGINT,
    metadata_size BIGINT,
    compression INTEGER NOT NULL DEFAULT 0,  -- 0=none, 1=lz4, 2=zstd, etc.
    is_flushed BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Columnar storage column chunks (for skip list/predicate pushdown)
CREATE TABLE IF NOT EXISTS orochi.orochi_column_chunks (
    chunk_id BIGSERIAL PRIMARY KEY,
    stripe_id BIGINT NOT NULL REFERENCES orochi.orochi_stripes(stripe_id) ON DELETE CASCADE,
    chunk_group_index INTEGER NOT NULL,
    column_index SMALLINT NOT NULL,
    column_type OID NOT NULL,
    value_count BIGINT NOT NULL,
    null_count BIGINT NOT NULL DEFAULT 0,
    has_nulls BOOLEAN NOT NULL DEFAULT FALSE,
    compressed_size BIGINT NOT NULL,
    decompressed_size BIGINT NOT NULL,
    compression INTEGER NOT NULL DEFAULT 0,
    -- Min/max statistics for predicate pushdown
    min_value BYTEA,
    max_value BYTEA,
    min_is_null BOOLEAN NOT NULL DEFAULT FALSE,
    max_is_null BOOLEAN NOT NULL DEFAULT FALSE,
    -- Storage location
    data_offset BIGINT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(stripe_id, chunk_group_index, column_index)
);

-- Chunk access tracking for tiered storage decisions
CREATE TABLE IF NOT EXISTS orochi.orochi_chunk_access (
    chunk_id BIGINT NOT NULL REFERENCES orochi.orochi_chunks(chunk_id) ON DELETE CASCADE,
    last_read_at TIMESTAMPTZ,
    last_write_at TIMESTAMPTZ,
    read_count BIGINT NOT NULL DEFAULT 0,
    write_count BIGINT NOT NULL DEFAULT 0,
    total_bytes_read BIGINT NOT NULL DEFAULT 0,
    total_bytes_written BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (chunk_id)
);

-- Indexes for performance
CREATE INDEX IF NOT EXISTS idx_shards_table ON orochi.orochi_shards(table_oid);
CREATE INDEX IF NOT EXISTS idx_shards_node ON orochi.orochi_shards(node_id);
CREATE INDEX IF NOT EXISTS idx_chunks_hypertable ON orochi.orochi_chunks(hypertable_oid);
CREATE INDEX IF NOT EXISTS idx_chunks_range ON orochi.orochi_chunks(range_start, range_end);
CREATE INDEX IF NOT EXISTS idx_dimensions_hypertable ON orochi.orochi_dimensions(hypertable_oid);
CREATE INDEX IF NOT EXISTS idx_policies_hypertable ON orochi.orochi_policies(hypertable_oid);
CREATE INDEX IF NOT EXISTS idx_policies_active ON orochi.orochi_policies(is_active) WHERE is_active = TRUE;
CREATE INDEX IF NOT EXISTS idx_stripes_table ON orochi.orochi_stripes(table_oid);
CREATE INDEX IF NOT EXISTS idx_stripes_flushed ON orochi.orochi_stripes(is_flushed) WHERE is_flushed = TRUE;
CREATE INDEX IF NOT EXISTS idx_column_chunks_stripe ON orochi.orochi_column_chunks(stripe_id);
CREATE INDEX IF NOT EXISTS idx_column_chunks_stats ON orochi.orochi_column_chunks(stripe_id, column_index);
CREATE INDEX IF NOT EXISTS idx_chunk_access_last_read ON orochi.orochi_chunk_access(last_read_at);
CREATE INDEX IF NOT EXISTS idx_chunk_access_last_write ON orochi.orochi_chunk_access(last_write_at);

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

-- Initialize the extension (called by provisioner during cluster bootstrap)
-- This function verifies the extension is properly loaded and returns status
CREATE FUNCTION initialize() RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_initialize_sql'
    LANGUAGE C;

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

-- ============================================================
-- Continuous Aggregate Functions
-- ============================================================

-- Create a continuous aggregate (materialized view with incremental refresh)
CREATE FUNCTION create_continuous_aggregate(
    view_name text,
    query text,
    refresh_interval interval DEFAULT INTERVAL '1 hour',
    with_data boolean DEFAULT TRUE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._create_continuous_aggregate(view_name, query, refresh_interval, with_data);
    RAISE NOTICE 'Created continuous aggregate %', view_name;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_continuous_aggregate(
    view_name text,
    query text,
    refresh_interval interval,
    with_data boolean
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_create_continuous_aggregate_sql'
    LANGUAGE C;

-- Refresh a continuous aggregate for a time range
CREATE FUNCTION refresh_continuous_aggregate(
    continuous_aggregate regclass,
    window_start timestamptz DEFAULT NULL,
    window_end timestamptz DEFAULT NULL
) RETURNS void AS $$
BEGIN
    PERFORM orochi._refresh_continuous_aggregate(continuous_aggregate, window_start, window_end);
    RAISE NOTICE 'Refreshed continuous aggregate %', continuous_aggregate;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._refresh_continuous_aggregate(
    view_oid oid,
    window_start timestamptz,
    window_end timestamptz
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_refresh_continuous_aggregate_sql'
    LANGUAGE C;

-- Drop a continuous aggregate and its materialization table
CREATE FUNCTION drop_continuous_aggregate(
    continuous_aggregate regclass
) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_continuous_aggregate(continuous_aggregate);
    RAISE NOTICE 'Dropped continuous aggregate %', continuous_aggregate;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_continuous_aggregate(view_oid oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drop_continuous_aggregate_sql'
    LANGUAGE C STRICT;

-- Add a refresh policy for automatic continuous aggregate refresh
CREATE FUNCTION add_continuous_aggregate_policy(
    continuous_aggregate regclass,
    start_offset interval,
    end_offset interval,
    schedule_interval interval DEFAULT INTERVAL '1 hour'
) RETURNS integer AS $$
DECLARE
    policy_id integer;
BEGIN
    -- Store the policy in the catalog
    INSERT INTO orochi.orochi_cagg_policies
    (view_oid, start_offset, end_offset, schedule_interval, enabled)
    VALUES (continuous_aggregate, start_offset, end_offset, schedule_interval, TRUE)
    ON CONFLICT (view_oid) DO UPDATE SET
        start_offset = EXCLUDED.start_offset,
        end_offset = EXCLUDED.end_offset,
        schedule_interval = EXCLUDED.schedule_interval,
        enabled = TRUE
    RETURNING orochi.orochi_cagg_policies.policy_id INTO policy_id;

    RAISE NOTICE 'Added continuous aggregate policy % for %', policy_id, continuous_aggregate;
    RETURN policy_id;
EXCEPTION WHEN undefined_table THEN
    -- Create the policy table if it doesn't exist
    CREATE TABLE IF NOT EXISTS orochi.orochi_cagg_policies (
        policy_id SERIAL PRIMARY KEY,
        view_oid OID UNIQUE NOT NULL,
        start_offset INTERVAL NOT NULL,
        end_offset INTERVAL NOT NULL,
        schedule_interval INTERVAL NOT NULL DEFAULT INTERVAL '1 hour',
        enabled BOOLEAN NOT NULL DEFAULT TRUE,
        last_run TIMESTAMPTZ,
        created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
    );

    INSERT INTO orochi.orochi_cagg_policies
    (view_oid, start_offset, end_offset, schedule_interval, enabled)
    VALUES (continuous_aggregate, start_offset, end_offset, schedule_interval, TRUE)
    RETURNING orochi.orochi_cagg_policies.policy_id INTO policy_id;

    RETURN policy_id;
END;
$$ LANGUAGE plpgsql;

-- Remove a continuous aggregate policy
CREATE FUNCTION remove_continuous_aggregate_policy(
    continuous_aggregate regclass
) RETURNS void AS $$
BEGIN
    DELETE FROM orochi.orochi_cagg_policies WHERE view_oid = continuous_aggregate;
    RAISE NOTICE 'Removed continuous aggregate policy from %', continuous_aggregate;
END;
$$ LANGUAGE plpgsql;

-- View for continuous aggregates information
CREATE VIEW orochi.continuous_aggregates AS
SELECT
    ca.agg_id,
    ca.view_oid,
    v.relname as view_name,
    ns.nspname as view_schema,
    t.schema_name || '.' || t.table_name as source_hypertable,
    ca.materialization_table_oid,
    ca.query_text,
    ca.refresh_interval,
    ca.last_refresh,
    ca.enabled
FROM orochi.orochi_continuous_aggregates ca
JOIN pg_class v ON ca.view_oid = v.oid
JOIN pg_namespace ns ON v.relnamespace = ns.oid
LEFT JOIN orochi.orochi_tables t ON ca.source_table_oid = t.table_oid;

-- ============================================================
-- Shard Rebalancing Functions
-- ============================================================

-- Rebalance shards for a distributed table
CREATE FUNCTION rebalance_table_shards(
    relation regclass
) RETURNS void AS $$
BEGIN
    PERFORM orochi._rebalance_table(relation);
    RAISE NOTICE 'Rebalanced shards for table %', relation;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._rebalance_table(relation oid)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_rebalance_table_sql'
    LANGUAGE C STRICT;

-- Move a specific shard to a different node
CREATE FUNCTION move_shard(
    shard_id bigint,
    target_node_id integer
) RETURNS void AS $$
BEGIN
    PERFORM orochi._move_shard(shard_id, target_node_id);
    RAISE NOTICE 'Moved shard % to node %', shard_id, target_node_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._move_shard(shard_id bigint, target_node integer)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_move_shard_sql'
    LANGUAGE C STRICT;

-- Split a shard into two
CREATE FUNCTION split_shard(
    shard_id bigint
) RETURNS void AS $$
BEGIN
    PERFORM orochi._split_shard(shard_id);
    RAISE NOTICE 'Split shard %', shard_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._split_shard(shard_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_split_shard_sql'
    LANGUAGE C STRICT;

-- Merge two adjacent shards
CREATE FUNCTION merge_shards(
    shard1_id bigint,
    shard2_id bigint
) RETURNS void AS $$
BEGIN
    PERFORM orochi._merge_shards(shard1_id, shard2_id);
    RAISE NOTICE 'Merged shards % and %', shard1_id, shard2_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._merge_shards(shard1_id bigint, shard2_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_merge_shards_sql'
    LANGUAGE C STRICT;

-- Get shard balance statistics and rebalance plan
CREATE FUNCTION get_rebalance_plan()
RETURNS text AS $$
    SELECT orochi._get_rebalance_plan();
$$ LANGUAGE sql;

CREATE FUNCTION orochi._get_rebalance_plan()
RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_get_rebalance_plan_sql'
    LANGUAGE C STRICT;

-- View showing shard distribution across nodes
CREATE VIEW orochi.shard_distribution AS
SELECT
    n.node_id,
    n.hostname,
    n.port,
    CASE n.role
        WHEN 0 THEN 'coordinator'
        WHEN 1 THEN 'worker'
        WHEN 2 THEN 'replica'
    END as role,
    n.is_active,
    COUNT(s.shard_id) as shard_count,
    COALESCE(SUM(s.size_bytes), 0) as total_size_bytes,
    COALESCE(SUM(s.row_count), 0) as total_row_count
FROM orochi.orochi_nodes n
LEFT JOIN orochi.orochi_shards s ON n.node_id = s.node_id
GROUP BY n.node_id, n.hostname, n.port, n.role, n.is_active
ORDER BY n.node_id;

-- View showing detailed shard placements
CREATE VIEW orochi.shard_placements AS
SELECT
    s.shard_id,
    t.schema_name || '.' || t.table_name as table_name,
    s.shard_index,
    s.hash_min,
    s.hash_max,
    s.node_id,
    n.hostname as node_hostname,
    s.row_count,
    s.size_bytes,
    CASE s.storage_tier
        WHEN 0 THEN 'hot'
        WHEN 1 THEN 'warm'
        WHEN 2 THEN 'cold'
        WHEN 3 THEN 'frozen'
    END as storage_tier
FROM orochi.orochi_shards s
JOIN orochi.orochi_tables t ON s.table_oid = t.table_oid
LEFT JOIN orochi.orochi_nodes n ON s.node_id = n.node_id
ORDER BY t.table_name, s.shard_index;

-- ============================================================
-- Raft Consensus Functions
-- ============================================================

-- Internal: Handle RequestVote RPC from a candidate
CREATE FUNCTION orochi.raft_request_vote(
    p_term bigint,
    p_candidate_id integer,
    p_last_log_index bigint,
    p_last_log_term bigint
) RETURNS TABLE(term bigint, vote_granted boolean)
    AS 'MODULE_PATHNAME', 'orochi_raft_request_vote_sql'
    LANGUAGE C STRICT;

-- Internal: Handle AppendEntries RPC from leader
CREATE FUNCTION orochi.raft_append_entries(
    p_term bigint,
    p_leader_id integer,
    p_prev_log_index bigint,
    p_prev_log_term bigint,
    p_entries jsonb,
    p_leader_commit bigint
) RETURNS TABLE(term bigint, success boolean, match_index bigint)
    AS 'MODULE_PATHNAME', 'orochi_raft_append_entries_sql'
    LANGUAGE C;

-- Internal: Handle InstallSnapshot RPC from leader
CREATE FUNCTION orochi.raft_install_snapshot(
    p_term bigint,
    p_leader_id integer,
    p_last_included_index bigint,
    p_last_included_term bigint,
    p_offset integer,
    p_data bytea,
    p_data_size integer,
    p_done boolean
) RETURNS TABLE(term bigint, bytes_received integer, success boolean)
    AS 'MODULE_PATHNAME', 'orochi_raft_install_snapshot_sql'
    LANGUAGE C;

-- Internal: Get read index for linearizable reads (called by followers on leader)
CREATE FUNCTION orochi.raft_leader_read_index(
    p_requester_node_id integer
) RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_raft_leader_read_index_sql'
    LANGUAGE C STRICT;

-- User-facing: Request a linearizable read index
-- This is the main entry point for applications wanting consistent reads
CREATE FUNCTION raft_read_index(
    timeout_ms integer DEFAULT 5000
) RETURNS bigint AS $$
DECLARE
    v_read_index bigint;
BEGIN
    SELECT orochi._raft_read_index(timeout_ms) INTO v_read_index;
    RETURN v_read_index;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._raft_read_index(timeout_ms integer)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_raft_read_index_sql'
    LANGUAGE C STRICT;

-- User-facing: Check if this node can serve a stale read
-- Returns true if data is fresh enough (within max_staleness_ms)
CREATE FUNCTION raft_can_read_stale(
    max_staleness_ms integer DEFAULT 1000
) RETURNS boolean AS $$
    SELECT orochi._raft_can_read_stale(max_staleness_ms);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._raft_can_read_stale(max_staleness_ms integer)
RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_raft_can_read_stale_sql'
    LANGUAGE C STRICT;

-- User-facing: Get current Raft cluster status
CREATE FUNCTION raft_status()
RETURNS TABLE(
    node_id integer,
    state text,
    term bigint,
    leader_id integer,
    commit_index bigint,
    last_applied bigint,
    is_leader boolean,
    peer_count integer
) AS $$
    SELECT * FROM orochi._raft_status();
$$ LANGUAGE sql;

CREATE FUNCTION orochi._raft_status()
RETURNS TABLE(
    node_id integer,
    state text,
    term bigint,
    leader_id integer,
    commit_index bigint,
    last_applied bigint,
    is_leader boolean,
    peer_count integer
)
    AS 'MODULE_PATHNAME', 'orochi_raft_status_sql'
    LANGUAGE C STRICT;

-- User-facing: Check if this node is the Raft leader
CREATE FUNCTION raft_is_leader()
RETURNS boolean AS $$
    SELECT orochi._raft_is_leader();
$$ LANGUAGE sql;

CREATE FUNCTION orochi._raft_is_leader()
RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_raft_is_leader_sql'
    LANGUAGE C STRICT;

-- User-facing: Get the current Raft leader's node ID
CREATE FUNCTION raft_get_leader()
RETURNS integer AS $$
    SELECT orochi._raft_get_leader();
$$ LANGUAGE sql;

CREATE FUNCTION orochi._raft_get_leader()
RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_raft_get_leader_sql'
    LANGUAGE C STRICT;

-- User-facing: Perform a follower read with bounded staleness
-- This allows reading from a replica with a freshness guarantee
CREATE FUNCTION raft_follower_read(
    max_staleness_ms integer DEFAULT 1000
) RETURNS boolean AS $$
DECLARE
    v_can_read boolean;
BEGIN
    SELECT orochi._raft_follower_read(max_staleness_ms) INTO v_can_read;
    IF NOT v_can_read THEN
        RAISE WARNING 'Follower data too stale (> % ms)', max_staleness_ms;
    END IF;
    RETURN v_can_read;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._raft_follower_read(max_staleness_ms integer)
RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_raft_follower_read_sql'
    LANGUAGE C STRICT;

-- User-facing: Trigger a snapshot creation
CREATE FUNCTION raft_create_snapshot()
RETURNS boolean AS $$
    SELECT orochi._raft_create_snapshot();
$$ LANGUAGE sql;

CREATE FUNCTION orochi._raft_create_snapshot()
RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_raft_create_snapshot_sql'
    LANGUAGE C STRICT;

-- View for Raft cluster members
CREATE VIEW orochi.raft_cluster AS
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
    last_heartbeat
FROM orochi.orochi_nodes
WHERE role IN (0, 1, 2)  -- Only show Raft-participating nodes
ORDER BY node_id;

-- ============================================================
-- JSON Indexing and Semi-Structured Data Support
-- ============================================================

-- JSON path statistics storage
CREATE TABLE IF NOT EXISTS orochi.orochi_json_path_stats (
    stat_id BIGSERIAL PRIMARY KEY,
    table_oid OID NOT NULL,
    column_attnum SMALLINT NOT NULL,
    path TEXT NOT NULL,
    value_type INTEGER NOT NULL DEFAULT 0,
    access_count BIGINT NOT NULL DEFAULT 0,
    null_count BIGINT NOT NULL DEFAULT 0,
    distinct_count BIGINT NOT NULL DEFAULT 0,
    selectivity DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    avg_value_size DOUBLE PRECISION NOT NULL DEFAULT 0,
    is_indexed BOOLEAN NOT NULL DEFAULT FALSE,
    recommended_index INTEGER NOT NULL DEFAULT 0,
    first_seen TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_accessed TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(table_oid, column_attnum, path)
);

CREATE INDEX IF NOT EXISTS idx_json_path_stats_table ON orochi.orochi_json_path_stats(table_oid, column_attnum);
CREATE INDEX IF NOT EXISTS idx_json_path_stats_access ON orochi.orochi_json_path_stats(access_count DESC);

-- JSON extracted columns metadata
CREATE TABLE IF NOT EXISTS orochi.orochi_json_columns (
    column_id SERIAL PRIMARY KEY,
    table_oid OID NOT NULL,
    source_attnum SMALLINT NOT NULL,
    path TEXT NOT NULL,
    target_type OID NOT NULL,
    storage_type INTEGER NOT NULL DEFAULT 0,
    state INTEGER NOT NULL DEFAULT 0,
    row_count BIGINT NOT NULL DEFAULT 0,
    null_count BIGINT NOT NULL DEFAULT 0,
    distinct_count BIGINT NOT NULL DEFAULT 0,
    storage_size BIGINT NOT NULL DEFAULT 0,
    compression_ratio DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_refreshed TIMESTAMPTZ,
    UNIQUE(table_oid, source_attnum, path)
);

CREATE INDEX IF NOT EXISTS idx_json_columns_table ON orochi.orochi_json_columns(table_oid, source_attnum);

-- ============================================================
-- JSON Index Functions
-- ============================================================

-- Create a JSON path index on specified paths
CREATE FUNCTION create_json_index(
    table_name regclass,
    json_column text,
    paths text[]
) RETURNS oid AS $$
DECLARE
    v_index_oid oid;
BEGIN
    SELECT orochi._create_json_index(table_name, json_column, paths) INTO v_index_oid;
    RAISE NOTICE 'Created JSON index on %.% for paths %', table_name, json_column, paths;
    RETURN v_index_oid;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_json_index(
    table_oid oid,
    json_column text,
    paths text[]
) RETURNS oid
    AS 'MODULE_PATHNAME', 'orochi_create_json_index'
    LANGUAGE C STRICT;

-- Analyze JSON paths in a table column
CREATE FUNCTION analyze_json_paths(
    table_name regclass,
    json_column text
) RETURNS TABLE(
    path text,
    value_type text,
    access_count bigint,
    distinct_count bigint,
    selectivity double precision,
    is_indexed boolean,
    recommended_index text
) AS $$
    SELECT * FROM orochi._analyze_json_paths(table_name, json_column);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._analyze_json_paths(
    table_oid oid,
    json_column text
) RETURNS TABLE(
    path text,
    value_type text,
    access_count bigint,
    distinct_count bigint,
    selectivity double precision,
    is_indexed boolean,
    recommended_index text
)
    AS 'MODULE_PATHNAME', 'orochi_analyze_json_paths'
    LANGUAGE C STRICT;

-- Get JSON index recommendations for a column
CREATE FUNCTION json_index_recommendations(
    table_name regclass,
    json_column text,
    max_recommendations integer DEFAULT 10
) RETURNS TABLE(
    path text,
    index_type text,
    priority integer,
    create_statement text,
    reason text
) AS $$
    SELECT * FROM orochi._json_index_recommendations(table_name, json_column, max_recommendations);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_index_recommendations(
    table_oid oid,
    json_column text,
    max_recommendations integer
) RETURNS TABLE(
    path text,
    index_type text,
    priority integer,
    create_statement text,
    reason text
)
    AS 'MODULE_PATHNAME', 'orochi_json_index_recommendations'
    LANGUAGE C STRICT;

-- Check if a specific JSON path is indexed
CREATE FUNCTION json_path_is_indexed(
    table_name regclass,
    json_column text,
    path text
) RETURNS boolean AS $$
    SELECT orochi._json_path_is_indexed(table_name, json_column, path);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_path_is_indexed(
    table_oid oid,
    json_column text,
    path text
) RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_json_path_is_indexed'
    LANGUAGE C STRICT;

-- Get statistics for a specific JSON path
CREATE FUNCTION json_path_stats(
    table_name regclass,
    json_column text,
    path text
) RETURNS TABLE(
    path text,
    access_count bigint,
    distinct_count bigint,
    null_count bigint,
    selectivity double precision,
    avg_value_size double precision
) AS $$
    SELECT r.path, r.access_count, r.distinct_count, r.null_count, r.selectivity, r.avg_value_size
    FROM orochi._json_path_stats(table_name, json_column, path) r;
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_path_stats(
    table_oid oid,
    json_column text,
    path text
) RETURNS record
    AS 'MODULE_PATHNAME', 'orochi_json_path_stats'
    LANGUAGE C STRICT;

-- ============================================================
-- JSON Columnar Extraction Functions
-- ============================================================

-- Extract common JSON paths to columnar storage
CREATE FUNCTION extract_json_columns(
    table_name regclass,
    json_column text,
    paths text[]
) RETURNS integer AS $$
DECLARE
    v_count integer;
BEGIN
    SELECT orochi._extract_columns(table_name, json_column, paths) INTO v_count;
    RAISE NOTICE 'Extracted % columns from %.%', v_count, table_name, json_column;
    RETURN v_count;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._extract_columns(
    table_oid oid,
    json_column text,
    paths text[]
) RETURNS integer
    AS 'MODULE_PATHNAME', 'orochi_extract_columns'
    LANGUAGE C STRICT;

-- Get information about extracted JSON columns
CREATE FUNCTION json_columnar_info(
    table_name regclass,
    json_column text
) RETURNS TABLE(
    column_id integer,
    path text,
    target_type text,
    row_count bigint,
    null_count bigint,
    storage_size bigint,
    compression_ratio double precision,
    state text
) AS $$
    SELECT * FROM orochi._json_columnar_info(table_name, json_column);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_columnar_info(
    table_oid oid,
    json_column text
) RETURNS TABLE(
    column_id integer,
    path text,
    target_type text,
    row_count bigint,
    null_count bigint,
    storage_size bigint,
    compression_ratio double precision,
    state text
)
    AS 'MODULE_PATHNAME', 'orochi_json_columnar_info'
    LANGUAGE C STRICT;

-- Refresh extracted JSON columns
CREATE FUNCTION refresh_json_columns(
    table_name regclass,
    json_column text
) RETURNS void AS $$
BEGIN
    PERFORM orochi._json_columnar_refresh(table_name, json_column);
    RAISE NOTICE 'Refreshed JSON columns for %.%', table_name, json_column;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._json_columnar_refresh(
    table_oid oid,
    json_column text
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_json_columnar_refresh'
    LANGUAGE C STRICT;

-- Get columnar storage statistics
CREATE FUNCTION json_columnar_stats(
    table_name regclass,
    json_column text
) RETURNS TABLE(
    num_columns integer,
    total_rows bigint,
    columnar_size bigint,
    original_size bigint,
    space_savings double precision,
    avg_compression double precision
) AS $$
    SELECT r.num_columns, r.total_rows, r.columnar_size, r.original_size, r.space_savings, r.avg_compression
    FROM orochi._json_columnar_stats(table_name, json_column) r;
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_columnar_stats(
    table_oid oid,
    json_column text
) RETURNS record
    AS 'MODULE_PATHNAME', 'orochi_json_columnar_stats'
    LANGUAGE C STRICT;

-- Enable hybrid row/columnar storage for JSON
CREATE FUNCTION enable_json_hybrid(
    table_name regclass,
    json_column text,
    columnar_paths text[]
) RETURNS void AS $$
BEGIN
    PERFORM orochi._json_enable_hybrid(table_name, json_column, columnar_paths);
    RAISE NOTICE 'Enabled hybrid storage for %.% with % columnar paths',
        table_name, json_column, array_length(columnar_paths, 1);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._json_enable_hybrid(
    table_oid oid,
    json_column text,
    columnar_paths text[]
) RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_json_enable_hybrid'
    LANGUAGE C STRICT;

-- ============================================================
-- JSON Query Functions
-- ============================================================

-- Execute an optimized JSON query
CREATE FUNCTION json_query(
    table_name regclass,
    json_column text,
    query_expr text
) RETURNS SETOF jsonb AS $$
    SELECT * FROM orochi._json_query(table_name, json_column, query_expr);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_query(
    table_oid oid,
    json_column text,
    query_expr text
) RETURNS SETOF jsonb
    AS 'MODULE_PATHNAME', 'orochi_json_query'
    LANGUAGE C STRICT;

-- Batch extract a path from an array of JSONB values
CREATE FUNCTION json_extract_batch(
    values jsonb[],
    path text
) RETURNS text[]
    AS 'MODULE_PATHNAME', 'orochi_json_extract_batch'
    LANGUAGE C STRICT PARALLEL SAFE;

-- Explain a JSON query plan
CREATE FUNCTION json_query_explain(
    table_name regclass,
    json_column text,
    query_expr text
) RETURNS text AS $$
    SELECT orochi._json_query_explain(table_name, json_column, query_expr);
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_query_explain(
    table_oid oid,
    json_column text,
    query_expr text
) RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_json_query_explain'
    LANGUAGE C STRICT;

-- Get JSON query statistics for a column
CREATE FUNCTION json_query_stats(
    table_name regclass,
    json_column text
) RETURNS TABLE(
    total_rows bigint,
    paths_found integer,
    indexed_paths integer,
    last_analyzed timestamptz
) AS $$
    SELECT r.total_rows, r.paths_found, r.indexed_paths, r.last_analyzed
    FROM orochi._json_query_stats(table_name, json_column) r;
$$ LANGUAGE sql;

CREATE FUNCTION orochi._json_query_stats(
    table_oid oid,
    json_column text
) RETURNS record
    AS 'MODULE_PATHNAME', 'orochi_json_query_stats'
    LANGUAGE C STRICT;

-- ============================================================
-- JSON Views
-- ============================================================

-- View for JSON path statistics
CREATE VIEW orochi.json_paths AS
SELECT
    t.schema_name || '.' || t.table_name as table_name,
    a.attname as column_name,
    s.path,
    CASE s.value_type
        WHEN 0 THEN 'null'
        WHEN 1 THEN 'string'
        WHEN 2 THEN 'number'
        WHEN 3 THEN 'boolean'
        WHEN 4 THEN 'array'
        WHEN 5 THEN 'object'
        WHEN 6 THEN 'mixed'
    END as value_type,
    s.access_count,
    s.distinct_count,
    round(s.selectivity::numeric, 4) as selectivity,
    s.is_indexed,
    CASE s.recommended_index
        WHEN 0 THEN 'gin'
        WHEN 1 THEN 'btree'
        WHEN 2 THEN 'hash'
        WHEN 3 THEN 'expression'
        WHEN 4 THEN 'partial'
    END as recommended_index,
    s.last_accessed
FROM orochi.orochi_json_path_stats s
JOIN orochi.orochi_tables t ON s.table_oid = t.table_oid
JOIN pg_attribute a ON s.table_oid = a.attrelid AND s.column_attnum = a.attnum
ORDER BY s.access_count DESC;

-- View for extracted JSON columns
CREATE VIEW orochi.json_columns AS
SELECT
    t.schema_name || '.' || t.table_name as table_name,
    a.attname as source_column,
    c.column_id,
    c.path,
    format_type(c.target_type, NULL) as target_type,
    CASE c.storage_type
        WHEN 0 THEN 'inline'
        WHEN 1 THEN 'dictionary'
        WHEN 2 THEN 'delta'
        WHEN 3 THEN 'rle'
        WHEN 4 THEN 'raw'
    END as storage_type,
    c.row_count,
    c.null_count,
    pg_size_pretty(c.storage_size) as storage_size,
    round(c.compression_ratio::numeric, 2) as compression_ratio,
    CASE c.state
        WHEN 0 THEN 'active'
        WHEN 1 THEN 'stale'
        WHEN 2 THEN 'deprecated'
    END as state,
    c.created_at,
    c.last_refreshed
FROM orochi.orochi_json_columns c
JOIN orochi.orochi_tables t ON c.table_oid = t.table_oid
JOIN pg_attribute a ON c.table_oid = a.attrelid AND c.source_attnum = a.attnum
ORDER BY c.table_oid, c.path;

-- ============================================================
-- Authentication Audit Logging Tables
-- ============================================================

/*
 * Main audit log table - partitioned by created_at (monthly)
 * This table stores all authentication-related events for compliance
 * with SOC2, HIPAA, GDPR, and other regulatory requirements.
 */
CREATE TABLE IF NOT EXISTS orochi.orochi_auth_audit_log (
    audit_id BIGSERIAL,
    tenant_id TEXT,
    user_id TEXT,
    session_id TEXT,
    event_type INTEGER NOT NULL,
    event_name TEXT NOT NULL,
    ip_address TEXT,
    user_agent TEXT,
    resource_type TEXT,
    resource_id TEXT,
    metadata JSONB,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (audit_id, created_at)
) PARTITION BY RANGE (created_at);

-- Create indexes on the audit log table for efficient querying
CREATE INDEX IF NOT EXISTS idx_auth_audit_tenant
    ON orochi.orochi_auth_audit_log (tenant_id, created_at DESC)
    WHERE tenant_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_auth_audit_user
    ON orochi.orochi_auth_audit_log (user_id, created_at DESC)
    WHERE user_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_auth_audit_session
    ON orochi.orochi_auth_audit_log (session_id, created_at DESC)
    WHERE session_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_auth_audit_event_type
    ON orochi.orochi_auth_audit_log (event_type, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_auth_audit_ip
    ON orochi.orochi_auth_audit_log (ip_address, created_at DESC)
    WHERE ip_address IS NOT NULL;

/*
 * Create initial default partition for any data that doesn't fit
 * specific month partitions. The background worker will create
 * proper monthly partitions as needed.
 */
CREATE TABLE IF NOT EXISTS orochi.orochi_auth_audit_log_default
    PARTITION OF orochi.orochi_auth_audit_log DEFAULT;

-- ============================================================
-- Authentication Audit Log Functions
-- ============================================================

/*
 * Function to manually log an audit event
 * Can be called from application code or triggers
 */
CREATE FUNCTION auth_audit_log(
    p_tenant_id TEXT DEFAULT NULL,
    p_user_id TEXT DEFAULT NULL,
    p_session_id TEXT DEFAULT NULL,
    p_event_type INTEGER DEFAULT 0,
    p_event_name TEXT DEFAULT 'unknown',
    p_ip_address TEXT DEFAULT NULL,
    p_user_agent TEXT DEFAULT NULL,
    p_resource_type TEXT DEFAULT NULL,
    p_resource_id TEXT DEFAULT NULL,
    p_metadata JSONB DEFAULT NULL
) RETURNS BIGINT AS $$
DECLARE
    v_audit_id BIGINT;
BEGIN
    INSERT INTO orochi.orochi_auth_audit_log (
        tenant_id, user_id, session_id, event_type, event_name,
        ip_address, user_agent, resource_type, resource_id, metadata
    ) VALUES (
        p_tenant_id, p_user_id, p_session_id, p_event_type, p_event_name,
        p_ip_address, p_user_agent, p_resource_type, p_resource_id, p_metadata
    ) RETURNING audit_id INTO v_audit_id;

    RETURN v_audit_id;
END;
$$ LANGUAGE plpgsql;

/*
 * Function to query audit log entries with filtering
 */
CREATE FUNCTION auth_audit_query(
    p_tenant_id TEXT DEFAULT NULL,
    p_user_id TEXT DEFAULT NULL,
    p_session_id TEXT DEFAULT NULL,
    p_event_types INTEGER[] DEFAULT NULL,
    p_start_time TIMESTAMPTZ DEFAULT NULL,
    p_end_time TIMESTAMPTZ DEFAULT NULL,
    p_ip_address TEXT DEFAULT NULL,
    p_limit INTEGER DEFAULT 1000
) RETURNS TABLE (
    audit_id BIGINT,
    tenant_id TEXT,
    user_id TEXT,
    session_id TEXT,
    event_type INTEGER,
    event_name TEXT,
    ip_address TEXT,
    user_agent TEXT,
    resource_type TEXT,
    resource_id TEXT,
    metadata JSONB,
    created_at TIMESTAMPTZ
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        a.audit_id,
        a.tenant_id,
        a.user_id,
        a.session_id,
        a.event_type,
        a.event_name,
        a.ip_address,
        a.user_agent,
        a.resource_type,
        a.resource_id,
        a.metadata,
        a.created_at
    FROM orochi.orochi_auth_audit_log a
    WHERE (p_tenant_id IS NULL OR a.tenant_id = p_tenant_id)
      AND (p_user_id IS NULL OR a.user_id = p_user_id)
      AND (p_session_id IS NULL OR a.session_id = p_session_id)
      AND (p_event_types IS NULL OR a.event_type = ANY(p_event_types))
      AND (p_start_time IS NULL OR a.created_at >= p_start_time)
      AND (p_end_time IS NULL OR a.created_at <= p_end_time)
      AND (p_ip_address IS NULL OR a.ip_address = p_ip_address)
    ORDER BY a.created_at DESC
    LIMIT p_limit;
END;
$$ LANGUAGE plpgsql STABLE;

/*
 * Function to get audit log statistics
 */
CREATE FUNCTION auth_audit_stats(
    p_tenant_id TEXT DEFAULT NULL,
    p_start_time TIMESTAMPTZ DEFAULT NOW() - INTERVAL '24 hours',
    p_end_time TIMESTAMPTZ DEFAULT NOW()
) RETURNS TABLE (
    event_name TEXT,
    event_count BIGINT,
    unique_users BIGINT,
    unique_sessions BIGINT,
    unique_ips BIGINT
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        a.event_name,
        COUNT(*) as event_count,
        COUNT(DISTINCT a.user_id) as unique_users,
        COUNT(DISTINCT a.session_id) as unique_sessions,
        COUNT(DISTINCT a.ip_address) as unique_ips
    FROM orochi.orochi_auth_audit_log a
    WHERE (p_tenant_id IS NULL OR a.tenant_id = p_tenant_id)
      AND a.created_at >= p_start_time
      AND a.created_at <= p_end_time
    GROUP BY a.event_name
    ORDER BY event_count DESC;
END;
$$ LANGUAGE plpgsql STABLE;

/*
 * Function to manually create audit partitions for a given month
 */
CREATE FUNCTION auth_audit_create_partition(
    p_year INTEGER,
    p_month INTEGER
) RETURNS TEXT AS $$
DECLARE
    v_partition_name TEXT;
    v_start_date DATE;
    v_end_date DATE;
BEGIN
    v_partition_name := format('orochi_auth_audit_log_y%04dm%02d', p_year, p_month);
    v_start_date := make_date(p_year, p_month, 1);

    -- Calculate end date (first day of next month)
    IF p_month = 12 THEN
        v_end_date := make_date(p_year + 1, 1, 1);
    ELSE
        v_end_date := make_date(p_year, p_month + 1, 1);
    END IF;

    -- Create the partition
    EXECUTE format(
        'CREATE TABLE IF NOT EXISTS orochi.%I PARTITION OF orochi.orochi_auth_audit_log '
        'FOR VALUES FROM (%L) TO (%L)',
        v_partition_name, v_start_date, v_end_date
    );

    RAISE NOTICE 'Created partition % for range % to %',
        v_partition_name, v_start_date, v_end_date;

    RETURN v_partition_name;
EXCEPTION WHEN duplicate_table THEN
    RAISE NOTICE 'Partition % already exists', v_partition_name;
    RETURN v_partition_name;
END;
$$ LANGUAGE plpgsql;

/*
 * Function to list audit log partitions
 */
CREATE FUNCTION auth_audit_list_partitions()
RETURNS TABLE (
    partition_name TEXT,
    partition_oid OID,
    range_start TEXT,
    range_end TEXT,
    row_count BIGINT,
    size_bytes BIGINT,
    size_pretty TEXT
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        c.relname::TEXT as partition_name,
        c.oid as partition_oid,
        pg_get_expr(c.relpartbound, c.oid) as range_start,
        '' as range_end,
        c.reltuples::BIGINT as row_count,
        pg_total_relation_size(c.oid) as size_bytes,
        pg_size_pretty(pg_total_relation_size(c.oid)) as size_pretty
    FROM pg_inherits i
    JOIN pg_class c ON i.inhrelid = c.oid
    JOIN pg_class p ON i.inhparent = p.oid
    JOIN pg_namespace n ON p.relnamespace = n.oid
    WHERE n.nspname = 'orochi'
      AND p.relname = 'orochi_auth_audit_log'
    ORDER BY c.relname;
END;
$$ LANGUAGE plpgsql STABLE;

/*
 * Function to archive (detach and optionally drop) old audit partitions
 */
CREATE FUNCTION auth_audit_archive_partition(
    p_partition_name TEXT,
    p_drop_after_detach BOOLEAN DEFAULT FALSE
) RETURNS BOOLEAN AS $$
BEGIN
    -- Detach the partition
    EXECUTE format(
        'ALTER TABLE orochi.orochi_auth_audit_log DETACH PARTITION orochi.%I',
        p_partition_name
    );

    RAISE NOTICE 'Detached partition %', p_partition_name;

    -- Optionally drop the detached partition
    IF p_drop_after_detach THEN
        EXECUTE format('DROP TABLE orochi.%I', p_partition_name);
        RAISE NOTICE 'Dropped partition %', p_partition_name;
    END IF;

    RETURN TRUE;
EXCEPTION WHEN OTHERS THEN
    RAISE WARNING 'Failed to archive partition %: %', p_partition_name, SQLERRM;
    RETURN FALSE;
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Authentication Audit Log Views
-- ============================================================

/*
 * View for recent authentication events (last 24 hours)
 */
CREATE VIEW orochi.auth_audit_recent AS
SELECT
    audit_id,
    tenant_id,
    user_id,
    session_id,
    event_type,
    event_name,
    ip_address,
    CASE
        WHEN length(user_agent) > 50 THEN substring(user_agent, 1, 50) || '...'
        ELSE user_agent
    END as user_agent_short,
    resource_type,
    resource_id,
    created_at
FROM orochi.orochi_auth_audit_log
WHERE created_at >= NOW() - INTERVAL '24 hours'
ORDER BY created_at DESC;

/*
 * View for failed authentication attempts (security monitoring)
 */
CREATE VIEW orochi.auth_audit_failures AS
SELECT
    audit_id,
    tenant_id,
    user_id,
    ip_address,
    event_name,
    user_agent,
    created_at
FROM orochi.orochi_auth_audit_log
WHERE event_type IN (1, 10, 18, 19)  -- LOGIN_FAILED, MFA_FAILED, PERMISSION_DENIED, RATE_LIMITED
ORDER BY created_at DESC;

/*
 * View for suspicious activity alerts
 */
CREATE VIEW orochi.auth_audit_suspicious AS
SELECT
    tenant_id,
    user_id,
    ip_address,
    COUNT(*) FILTER (WHERE event_type = 1) as failed_logins,
    COUNT(*) FILTER (WHERE event_type = 10) as failed_mfa,
    COUNT(*) FILTER (WHERE event_type = 18) as permission_denied,
    COUNT(*) FILTER (WHERE event_type = 19) as rate_limited,
    COUNT(*) as total_failures,
    MIN(created_at) as first_seen,
    MAX(created_at) as last_seen
FROM orochi.orochi_auth_audit_log
WHERE event_type IN (1, 10, 18, 19, 20)  -- All failure/suspicious events
  AND created_at >= NOW() - INTERVAL '1 hour'
GROUP BY tenant_id, user_id, ip_address
HAVING COUNT(*) >= 5  -- Threshold for suspicious activity
ORDER BY total_failures DESC;
