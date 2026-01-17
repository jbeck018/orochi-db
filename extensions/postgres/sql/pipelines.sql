/*-------------------------------------------------------------------------
 *
 * pipelines.sql
 *    Orochi DB real-time data pipeline SQL interface
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

-- ============================================================
-- Pipeline Catalog Tables
-- ============================================================

-- Main pipeline configuration table
CREATE TABLE IF NOT EXISTS orochi.orochi_pipelines (
    pipeline_id         BIGSERIAL PRIMARY KEY,
    name                VARCHAR(128) NOT NULL UNIQUE,
    source_type         INTEGER NOT NULL,  -- 0=kafka, 1=s3, 2=filesystem
    target_table_oid    OID NOT NULL,
    format              INTEGER NOT NULL DEFAULT 0,  -- 0=json, 1=csv, 2=avro
    options             JSONB NOT NULL DEFAULT '{}',
    state               INTEGER NOT NULL DEFAULT 0,  -- 0=created, 1=running, 2=paused, 3=stopped, 4=error
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    started_at          TIMESTAMPTZ,
    stopped_at          TIMESTAMPTZ,
    worker_pid          INTEGER,
    batch_size          INTEGER DEFAULT 1000,
    batch_timeout_ms    INTEGER DEFAULT 100,
    error_threshold     INTEGER DEFAULT 100,
    CONSTRAINT valid_source_type CHECK (source_type BETWEEN 0 AND 4),
    CONSTRAINT valid_format CHECK (format BETWEEN 0 AND 4),
    CONSTRAINT valid_state CHECK (state BETWEEN 0 AND 5)
);

-- Pipeline statistics table
CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_stats (
    pipeline_id         BIGINT PRIMARY KEY REFERENCES orochi.orochi_pipelines(pipeline_id) ON DELETE CASCADE,
    messages_received   BIGINT NOT NULL DEFAULT 0,
    messages_processed  BIGINT NOT NULL DEFAULT 0,
    messages_failed     BIGINT NOT NULL DEFAULT 0,
    bytes_received      BIGINT NOT NULL DEFAULT 0,
    rows_inserted       BIGINT NOT NULL DEFAULT 0,
    last_message_time   TIMESTAMPTZ,
    last_error_time     TIMESTAMPTZ,
    last_error_message  TEXT,
    avg_latency_ms      DOUBLE PRECISION DEFAULT 0,
    throughput_per_sec  DOUBLE PRECISION DEFAULT 0,
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Pipeline checkpoints for exactly-once semantics
CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_checkpoints (
    checkpoint_id       BIGSERIAL PRIMARY KEY,
    pipeline_id         BIGINT NOT NULL REFERENCES orochi.orochi_pipelines(pipeline_id) ON DELETE CASCADE,
    checkpoint_time     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    kafka_offset        BIGINT,
    kafka_partition     INTEGER,
    s3_last_key         TEXT,
    s3_last_modified    TIMESTAMPTZ,
    messages_processed  BIGINT NOT NULL DEFAULT 0,
    last_committed_xid  XID,
    is_valid            BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE INDEX IF NOT EXISTS idx_pipeline_checkpoints_pipeline
    ON orochi.orochi_pipeline_checkpoints(pipeline_id, checkpoint_time DESC);

-- S3 processed files tracking
CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_s3_files (
    pipeline_id         BIGINT NOT NULL REFERENCES orochi.orochi_pipelines(pipeline_id) ON DELETE CASCADE,
    s3_key              TEXT NOT NULL,
    etag                TEXT,
    file_size           BIGINT,
    discovered_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    processed_at        TIMESTAMPTZ,
    status              VARCHAR(20) NOT NULL DEFAULT 'pending',
    error_message       TEXT,
    PRIMARY KEY (pipeline_id, s3_key)
);

CREATE INDEX IF NOT EXISTS idx_s3_files_status
    ON orochi.orochi_pipeline_s3_files(pipeline_id, status);

-- Field transforms/mappings
CREATE TABLE IF NOT EXISTS orochi.orochi_pipeline_transforms (
    transform_id        BIGSERIAL PRIMARY KEY,
    pipeline_id         BIGINT NOT NULL REFERENCES orochi.orochi_pipelines(pipeline_id) ON DELETE CASCADE,
    source_field        TEXT NOT NULL,
    target_column       TEXT NOT NULL,
    transform_expr      TEXT,
    default_value       TEXT,
    is_required         BOOLEAN NOT NULL DEFAULT FALSE,
    ordinal             INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_pipeline_transforms
    ON orochi.orochi_pipeline_transforms(pipeline_id, ordinal);

-- ============================================================
-- Pipeline Management Functions
-- ============================================================

-- Create a new pipeline
CREATE OR REPLACE FUNCTION orochi.create_pipeline(
    name TEXT,
    source TEXT,
    target_table REGCLASS,
    format TEXT DEFAULT 'json',
    options JSONB DEFAULT '{}'
)
RETURNS BIGINT
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_create_pipeline_sql';

COMMENT ON FUNCTION orochi.create_pipeline IS
    'Create a new data ingestion pipeline';

-- Start a pipeline
CREATE OR REPLACE FUNCTION orochi.start_pipeline(pipeline_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_start_pipeline_sql';

COMMENT ON FUNCTION orochi.start_pipeline IS
    'Start a pipeline and begin data ingestion';

-- Pause a pipeline
CREATE OR REPLACE FUNCTION orochi.pause_pipeline(pipeline_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_pause_pipeline_sql';

COMMENT ON FUNCTION orochi.pause_pipeline IS
    'Pause a running pipeline';

-- Resume a paused pipeline
CREATE OR REPLACE FUNCTION orochi.resume_pipeline(pipeline_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_resume_pipeline_sql';

COMMENT ON FUNCTION orochi.resume_pipeline IS
    'Resume a paused pipeline';

-- Stop a pipeline
CREATE OR REPLACE FUNCTION orochi.stop_pipeline(pipeline_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_stop_pipeline_sql';

COMMENT ON FUNCTION orochi.stop_pipeline IS
    'Stop a pipeline permanently';

-- Drop a pipeline
CREATE OR REPLACE FUNCTION orochi.drop_pipeline(
    pipeline_id BIGINT,
    if_exists BOOLEAN DEFAULT FALSE
)
RETURNS BOOLEAN
LANGUAGE C
AS 'MODULE_PATHNAME', 'orochi_drop_pipeline_sql';

COMMENT ON FUNCTION orochi.drop_pipeline IS
    'Drop a pipeline and all its data';

-- Get pipeline status
CREATE OR REPLACE FUNCTION orochi.pipeline_status(pipeline_id BIGINT)
RETURNS TEXT
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_pipeline_status_sql';

COMMENT ON FUNCTION orochi.pipeline_status IS
    'Get the current status of a pipeline';

-- Get pipeline statistics
CREATE OR REPLACE FUNCTION orochi.pipeline_stats(pipeline_id BIGINT)
RETURNS TABLE (
    messages_received   BIGINT,
    messages_processed  BIGINT,
    messages_failed     BIGINT,
    bytes_received      BIGINT,
    rows_inserted       BIGINT,
    last_message_time   TIMESTAMPTZ,
    last_error_time     TIMESTAMPTZ,
    last_error_message  TEXT,
    avg_latency_ms      DOUBLE PRECISION,
    throughput_per_sec  DOUBLE PRECISION
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_pipeline_stats_sql';

COMMENT ON FUNCTION orochi.pipeline_stats IS
    'Get statistics for a pipeline';

-- ============================================================
-- Convenience Functions
-- ============================================================

-- Create Kafka pipeline with simplified syntax
CREATE OR REPLACE FUNCTION orochi.create_kafka_pipeline(
    name TEXT,
    target_table REGCLASS,
    brokers TEXT,
    topic TEXT,
    consumer_group TEXT DEFAULT NULL,
    format TEXT DEFAULT 'json'
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    options JSONB;
    pipeline_id BIGINT;
BEGIN
    options := jsonb_build_object(
        'bootstrap_servers', brokers,
        'topic', topic,
        'consumer_group', COALESCE(consumer_group, 'orochi_' || name),
        'enable_auto_commit', false,
        'start_offset', -1  -- Latest
    );

    pipeline_id := orochi.create_pipeline(name, 'kafka', target_table, format, options);

    RETURN pipeline_id;
END;
$$;

COMMENT ON FUNCTION orochi.create_kafka_pipeline IS
    'Create a Kafka pipeline with simplified options';

-- Create S3 pipeline with simplified syntax
CREATE OR REPLACE FUNCTION orochi.create_s3_pipeline(
    name TEXT,
    target_table REGCLASS,
    bucket TEXT,
    prefix TEXT DEFAULT '',
    region TEXT DEFAULT 'us-east-1',
    format TEXT DEFAULT 'json',
    file_pattern TEXT DEFAULT '*'
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    options JSONB;
    pipeline_id BIGINT;
BEGIN
    options := jsonb_build_object(
        'bucket', bucket,
        'prefix', prefix,
        'region', region,
        'file_pattern', file_pattern,
        'poll_interval_sec', 30,
        'delete_after_process', false
    );

    pipeline_id := orochi.create_pipeline(name, 's3', target_table, format, options);

    RETURN pipeline_id;
END;
$$;

COMMENT ON FUNCTION orochi.create_s3_pipeline IS
    'Create an S3 pipeline with simplified options';

-- ============================================================
-- Pipeline Views
-- ============================================================

-- View of all pipelines with their status
CREATE OR REPLACE VIEW orochi.pipelines AS
SELECT
    p.pipeline_id,
    p.name,
    CASE p.source_type
        WHEN 0 THEN 'kafka'
        WHEN 1 THEN 's3'
        WHEN 2 THEN 'filesystem'
        WHEN 3 THEN 'http'
        WHEN 4 THEN 'kinesis'
    END AS source,
    c.relname AS target_table,
    n.nspname AS target_schema,
    CASE p.format
        WHEN 0 THEN 'json'
        WHEN 1 THEN 'csv'
        WHEN 2 THEN 'avro'
        WHEN 3 THEN 'parquet'
        WHEN 4 THEN 'line'
    END AS format,
    CASE p.state
        WHEN 0 THEN 'created'
        WHEN 1 THEN 'running'
        WHEN 2 THEN 'paused'
        WHEN 3 THEN 'stopped'
        WHEN 4 THEN 'error'
        WHEN 5 THEN 'draining'
    END AS state,
    p.created_at,
    p.started_at,
    p.stopped_at,
    p.worker_pid,
    p.options,
    s.messages_received,
    s.messages_processed,
    s.messages_failed,
    s.rows_inserted,
    s.last_message_time,
    s.throughput_per_sec
FROM orochi.orochi_pipelines p
LEFT JOIN pg_class c ON p.target_table_oid = c.oid
LEFT JOIN pg_namespace n ON c.relnamespace = n.oid
LEFT JOIN orochi.orochi_pipeline_stats s ON p.pipeline_id = s.pipeline_id;

COMMENT ON VIEW orochi.pipelines IS
    'View of all data pipelines with their current status and statistics';

-- View of pipeline checkpoints
CREATE OR REPLACE VIEW orochi.pipeline_checkpoints AS
SELECT
    cp.checkpoint_id,
    cp.pipeline_id,
    p.name AS pipeline_name,
    cp.checkpoint_time,
    cp.kafka_offset,
    cp.kafka_partition,
    cp.s3_last_key,
    cp.s3_last_modified,
    cp.messages_processed,
    cp.is_valid
FROM orochi.orochi_pipeline_checkpoints cp
JOIN orochi.orochi_pipelines p ON cp.pipeline_id = p.pipeline_id
ORDER BY cp.checkpoint_time DESC;

COMMENT ON VIEW orochi.pipeline_checkpoints IS
    'View of pipeline checkpoints for recovery and exactly-once semantics';

-- View of S3 file processing status
CREATE OR REPLACE VIEW orochi.pipeline_s3_files AS
SELECT
    f.pipeline_id,
    p.name AS pipeline_name,
    f.s3_key,
    f.file_size,
    f.status,
    f.discovered_at,
    f.processed_at,
    f.error_message
FROM orochi.orochi_pipeline_s3_files f
JOIN orochi.orochi_pipelines p ON f.pipeline_id = p.pipeline_id
ORDER BY f.discovered_at DESC;

COMMENT ON VIEW orochi.pipeline_s3_files IS
    'View of S3 files discovered and processed by pipelines';

-- ============================================================
-- Administrative Functions
-- ============================================================

-- Reset pipeline statistics
CREATE OR REPLACE FUNCTION orochi.reset_pipeline_stats(pipeline_id BIGINT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE orochi.orochi_pipeline_stats
    SET
        messages_received = 0,
        messages_processed = 0,
        messages_failed = 0,
        bytes_received = 0,
        rows_inserted = 0,
        last_message_time = NULL,
        last_error_time = NULL,
        last_error_message = NULL,
        avg_latency_ms = 0,
        throughput_per_sec = 0,
        updated_at = NOW()
    WHERE pipeline_id = reset_pipeline_stats.pipeline_id;

    IF NOT FOUND THEN
        RAISE NOTICE 'No stats found for pipeline %', pipeline_id;
    END IF;
END;
$$;

COMMENT ON FUNCTION orochi.reset_pipeline_stats IS
    'Reset statistics for a pipeline';

-- List all running pipelines
CREATE OR REPLACE FUNCTION orochi.list_running_pipelines()
RETURNS TABLE (
    pipeline_id BIGINT,
    name TEXT,
    source TEXT,
    worker_pid INTEGER,
    started_at TIMESTAMPTZ,
    messages_processed BIGINT,
    throughput_per_sec DOUBLE PRECISION
)
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN QUERY
    SELECT
        p.pipeline_id,
        p.name::TEXT,
        CASE p.source_type
            WHEN 0 THEN 'kafka'
            WHEN 1 THEN 's3'
            WHEN 2 THEN 'filesystem'
        END::TEXT AS source,
        p.worker_pid,
        p.started_at,
        COALESCE(s.messages_processed, 0),
        COALESCE(s.throughput_per_sec, 0)
    FROM orochi.orochi_pipelines p
    LEFT JOIN orochi.orochi_pipeline_stats s ON p.pipeline_id = s.pipeline_id
    WHERE p.state = 1;  -- Running
END;
$$;

COMMENT ON FUNCTION orochi.list_running_pipelines IS
    'List all currently running pipelines';

-- Restart a failed pipeline
CREATE OR REPLACE FUNCTION orochi.restart_pipeline(pipeline_id BIGINT)
RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
DECLARE
    current_state INTEGER;
BEGIN
    SELECT state INTO current_state
    FROM orochi.orochi_pipelines
    WHERE pipeline_id = restart_pipeline.pipeline_id;

    IF current_state IS NULL THEN
        RAISE EXCEPTION 'Pipeline % does not exist', pipeline_id;
    END IF;

    -- Stop if running
    IF current_state = 1 THEN
        PERFORM orochi.stop_pipeline(pipeline_id);
    END IF;

    -- Reset error state
    UPDATE orochi.orochi_pipelines
    SET state = 0  -- Created
    WHERE pipeline_id = restart_pipeline.pipeline_id
    AND state = 4;  -- Error

    -- Start the pipeline
    RETURN orochi.start_pipeline(pipeline_id);
END;
$$;

COMMENT ON FUNCTION orochi.restart_pipeline IS
    'Restart a pipeline (useful after errors)';

-- Add field transform to pipeline
CREATE OR REPLACE FUNCTION orochi.add_pipeline_transform(
    pipeline_id BIGINT,
    source_field TEXT,
    target_column TEXT,
    transform_expr TEXT DEFAULT NULL,
    default_value TEXT DEFAULT NULL,
    is_required BOOLEAN DEFAULT FALSE
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    new_ordinal INTEGER;
    transform_id BIGINT;
BEGIN
    -- Get next ordinal
    SELECT COALESCE(MAX(ordinal), 0) + 1 INTO new_ordinal
    FROM orochi.orochi_pipeline_transforms
    WHERE pipeline_id = add_pipeline_transform.pipeline_id;

    INSERT INTO orochi.orochi_pipeline_transforms
        (pipeline_id, source_field, target_column, transform_expr, default_value, is_required, ordinal)
    VALUES
        (pipeline_id, source_field, target_column, transform_expr, default_value, is_required, new_ordinal)
    RETURNING orochi_pipeline_transforms.transform_id INTO transform_id;

    RETURN transform_id;
END;
$$;

COMMENT ON FUNCTION orochi.add_pipeline_transform IS
    'Add a field transform/mapping to a pipeline';

-- ============================================================
-- Example Usage
-- ============================================================

/*
-- Create a Kafka pipeline for IoT sensor data
SELECT orochi.create_kafka_pipeline(
    'iot_sensors',
    'public.sensor_readings',
    'kafka-broker1:9092,kafka-broker2:9092',
    'sensor-events',
    'orochi-iot-consumer',
    'json'
);

-- Create an S3 pipeline for batch data loading
SELECT orochi.create_s3_pipeline(
    'daily_logs',
    'public.application_logs',
    'my-data-bucket',
    'logs/daily/',
    'us-west-2',
    'json',
    '*.json'
);

-- Start pipelines
SELECT orochi.start_pipeline(1);
SELECT orochi.start_pipeline(2);

-- Monitor pipelines
SELECT * FROM orochi.pipelines;

-- Check statistics
SELECT * FROM orochi.pipeline_stats(1);

-- Pause pipeline for maintenance
SELECT orochi.pause_pipeline(1);

-- Resume pipeline
SELECT orochi.resume_pipeline(1);

-- Stop pipeline
SELECT orochi.stop_pipeline(1);

-- Drop pipeline
SELECT orochi.drop_pipeline(1, true);
*/
