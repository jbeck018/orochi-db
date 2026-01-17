/*-------------------------------------------------------------------------
 *
 * cdc.sql
 *    Orochi DB Change Data Capture SQL interface
 *
 * Provides SQL functions and views for managing CDC subscriptions
 * and streaming database changes to external systems.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

-- ============================================================
-- CDC Catalog Tables
-- ============================================================

-- Main CDC subscription configuration table
CREATE TABLE IF NOT EXISTS orochi.orochi_cdc_subscriptions (
    subscription_id     BIGSERIAL PRIMARY KEY,
    name                VARCHAR(128) NOT NULL UNIQUE,
    sink_type           INTEGER NOT NULL,  -- 0=kafka, 1=webhook, 2=file
    output_format       INTEGER NOT NULL DEFAULT 0,  -- 0=json, 1=avro, 2=debezium
    options             JSONB NOT NULL DEFAULT '{}',
    state               INTEGER NOT NULL DEFAULT 0,  -- 0=created, 1=active, 2=paused, 3=error, 4=disabled
    include_schema      BOOLEAN NOT NULL DEFAULT TRUE,
    include_before      BOOLEAN NOT NULL DEFAULT TRUE,
    include_transaction BOOLEAN NOT NULL DEFAULT FALSE,
    start_lsn           PG_LSN,
    confirmed_lsn       PG_LSN,
    restart_xid         XID,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    started_at          TIMESTAMPTZ,
    stopped_at          TIMESTAMPTZ,
    worker_pid          INTEGER,
    CONSTRAINT valid_sink_type CHECK (sink_type BETWEEN 0 AND 4),
    CONSTRAINT valid_output_format CHECK (output_format BETWEEN 0 AND 3),
    CONSTRAINT valid_state CHECK (state BETWEEN 0 AND 4)
);

COMMENT ON TABLE orochi.orochi_cdc_subscriptions IS
    'CDC subscription configurations for streaming changes to external systems';

-- CDC table filters
CREATE TABLE IF NOT EXISTS orochi.orochi_cdc_table_filters (
    filter_id           BIGSERIAL PRIMARY KEY,
    subscription_id     BIGINT NOT NULL REFERENCES orochi.orochi_cdc_subscriptions(subscription_id) ON DELETE CASCADE,
    schema_pattern      TEXT NOT NULL DEFAULT '*',
    table_pattern       TEXT NOT NULL DEFAULT '*',
    is_include          BOOLEAN NOT NULL DEFAULT TRUE,
    ordinal             INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_cdc_table_filters_sub
    ON orochi.orochi_cdc_table_filters(subscription_id, ordinal);

COMMENT ON TABLE orochi.orochi_cdc_table_filters IS
    'Filter patterns to include/exclude tables from CDC subscriptions';

-- CDC statistics table
CREATE TABLE IF NOT EXISTS orochi.orochi_cdc_stats (
    subscription_id     BIGINT PRIMARY KEY REFERENCES orochi.orochi_cdc_subscriptions(subscription_id) ON DELETE CASCADE,
    events_captured     BIGINT NOT NULL DEFAULT 0,
    events_sent         BIGINT NOT NULL DEFAULT 0,
    events_failed       BIGINT NOT NULL DEFAULT 0,
    events_filtered     BIGINT NOT NULL DEFAULT 0,
    bytes_sent          BIGINT NOT NULL DEFAULT 0,
    avg_latency_ms      DOUBLE PRECISION DEFAULT 0,
    throughput_per_sec  DOUBLE PRECISION DEFAULT 0,
    last_capture_time   TIMESTAMPTZ,
    last_send_time      TIMESTAMPTZ,
    last_error_time     TIMESTAMPTZ,
    last_error_message  TEXT,
    current_lsn         PG_LSN,
    confirmed_lsn       PG_LSN,
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

COMMENT ON TABLE orochi.orochi_cdc_stats IS
    'Runtime statistics for CDC subscriptions';

-- CDC event history (optional, for debugging)
CREATE TABLE IF NOT EXISTS orochi.orochi_cdc_event_log (
    event_id            BIGSERIAL PRIMARY KEY,
    subscription_id     BIGINT NOT NULL,
    event_type          TEXT NOT NULL,
    schema_name         TEXT,
    table_name          TEXT,
    lsn                 PG_LSN,
    xid                 XID,
    event_time          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    sent_at             TIMESTAMPTZ,
    status              TEXT NOT NULL DEFAULT 'pending',
    error_message       TEXT
);

CREATE INDEX IF NOT EXISTS idx_cdc_event_log_sub
    ON orochi.orochi_cdc_event_log(subscription_id, event_time DESC);

CREATE INDEX IF NOT EXISTS idx_cdc_event_log_status
    ON orochi.orochi_cdc_event_log(subscription_id, status) WHERE status != 'sent';

COMMENT ON TABLE orochi.orochi_cdc_event_log IS
    'Optional event log for CDC debugging and replay';

-- ============================================================
-- CDC Subscription Management Functions
-- ============================================================

-- Create a new CDC subscription
CREATE OR REPLACE FUNCTION orochi.create_cdc_subscription(
    name TEXT,
    sink_type TEXT,
    format TEXT DEFAULT 'json',
    options JSONB DEFAULT '{}'
)
RETURNS BIGINT
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_create_cdc_subscription_sql';

COMMENT ON FUNCTION orochi.create_cdc_subscription IS
    'Create a new CDC subscription for streaming changes to external systems';

-- Drop a CDC subscription
CREATE OR REPLACE FUNCTION orochi.drop_cdc_subscription(
    subscription_id BIGINT,
    if_exists BOOLEAN DEFAULT FALSE
)
RETURNS BOOLEAN
LANGUAGE C
AS 'MODULE_PATHNAME', 'orochi_drop_cdc_subscription_sql';

COMMENT ON FUNCTION orochi.drop_cdc_subscription IS
    'Drop a CDC subscription and all its data';

-- Start a CDC subscription
CREATE OR REPLACE FUNCTION orochi.start_cdc_subscription(subscription_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_start_cdc_subscription_sql';

COMMENT ON FUNCTION orochi.start_cdc_subscription IS
    'Start a CDC subscription and begin streaming changes';

-- Stop a CDC subscription
CREATE OR REPLACE FUNCTION orochi.stop_cdc_subscription(subscription_id BIGINT)
RETURNS BOOLEAN
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_stop_cdc_subscription_sql';

COMMENT ON FUNCTION orochi.stop_cdc_subscription IS
    'Stop a CDC subscription';

-- List CDC subscriptions
CREATE OR REPLACE FUNCTION orochi.list_cdc_subscriptions()
RETURNS TABLE (
    subscription_id BIGINT,
    name TEXT,
    sink_type TEXT,
    state TEXT,
    events_sent BIGINT,
    created_at TIMESTAMPTZ
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_list_cdc_subscriptions_sql';

COMMENT ON FUNCTION orochi.list_cdc_subscriptions IS
    'List all CDC subscriptions with their current status';

-- Get CDC subscription statistics
CREATE OR REPLACE FUNCTION orochi.cdc_subscription_stats(subscription_id BIGINT)
RETURNS TABLE (
    events_captured BIGINT,
    events_sent BIGINT,
    events_failed BIGINT,
    events_filtered BIGINT,
    bytes_sent BIGINT,
    avg_latency_ms DOUBLE PRECISION,
    throughput_per_sec DOUBLE PRECISION,
    last_capture_time TIMESTAMPTZ,
    last_send_time TIMESTAMPTZ,
    current_lsn PG_LSN,
    confirmed_lsn PG_LSN
)
LANGUAGE C STRICT
AS 'MODULE_PATHNAME', 'orochi_cdc_subscription_stats_sql';

COMMENT ON FUNCTION orochi.cdc_subscription_stats IS
    'Get detailed statistics for a CDC subscription';

-- ============================================================
-- Convenience Functions for Creating CDC Subscriptions
-- ============================================================

-- Create Kafka CDC subscription with simplified syntax
CREATE OR REPLACE FUNCTION orochi.create_kafka_cdc_subscription(
    name TEXT,
    brokers TEXT,
    topic TEXT,
    format TEXT DEFAULT 'json'
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    options JSONB;
    subscription_id BIGINT;
BEGIN
    options := jsonb_build_object(
        'bootstrap_servers', brokers,
        'topic', topic,
        'compression_type', 'snappy',
        'acks', -1,
        'batch_size', 1000,
        'linger_ms', 5
    );

    subscription_id := orochi.create_cdc_subscription(name, 'kafka', format, options);

    RETURN subscription_id;
END;
$$;

COMMENT ON FUNCTION orochi.create_kafka_cdc_subscription IS
    'Create a Kafka CDC subscription with simplified options';

-- Create Webhook CDC subscription with simplified syntax
CREATE OR REPLACE FUNCTION orochi.create_webhook_cdc_subscription(
    name TEXT,
    url TEXT,
    format TEXT DEFAULT 'json',
    auth_type TEXT DEFAULT NULL,
    auth_credentials TEXT DEFAULT NULL
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    options JSONB;
    subscription_id BIGINT;
BEGIN
    options := jsonb_build_object(
        'url', url,
        'method', 'POST',
        'content_type', 'application/json',
        'timeout_ms', 30000,
        'retry_count', 3,
        'retry_delay_ms', 1000
    );

    IF auth_type IS NOT NULL THEN
        options := options || jsonb_build_object(
            'auth_type', auth_type,
            'auth_credentials', auth_credentials
        );
    END IF;

    subscription_id := orochi.create_cdc_subscription(name, 'webhook', format, options);

    RETURN subscription_id;
END;
$$;

COMMENT ON FUNCTION orochi.create_webhook_cdc_subscription IS
    'Create a Webhook CDC subscription with simplified options';

-- Create File CDC subscription (for debugging)
CREATE OR REPLACE FUNCTION orochi.create_file_cdc_subscription(
    name TEXT,
    file_path TEXT,
    format TEXT DEFAULT 'json',
    pretty_print BOOLEAN DEFAULT TRUE
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    options JSONB;
    subscription_id BIGINT;
BEGIN
    options := jsonb_build_object(
        'file_path', file_path,
        'max_file_size', 104857600,  -- 100MB
        'max_files', 10,
        'append_mode', true,
        'flush_immediate', true,
        'pretty_print', pretty_print
    );

    subscription_id := orochi.create_cdc_subscription(name, 'file', format, options);

    RETURN subscription_id;
END;
$$;

COMMENT ON FUNCTION orochi.create_file_cdc_subscription IS
    'Create a File CDC subscription for debugging';

-- ============================================================
-- CDC Table Filter Management
-- ============================================================

-- Add a table filter to a CDC subscription
CREATE OR REPLACE FUNCTION orochi.add_cdc_table_filter(
    subscription_id BIGINT,
    schema_pattern TEXT DEFAULT '*',
    table_pattern TEXT DEFAULT '*',
    is_include BOOLEAN DEFAULT TRUE
)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
    new_ordinal INTEGER;
    filter_id BIGINT;
BEGIN
    -- Check subscription exists
    IF NOT EXISTS (SELECT 1 FROM orochi.orochi_cdc_subscriptions WHERE subscription_id = add_cdc_table_filter.subscription_id) THEN
        RAISE EXCEPTION 'CDC subscription % does not exist', subscription_id;
    END IF;

    -- Get next ordinal
    SELECT COALESCE(MAX(ordinal), 0) + 1 INTO new_ordinal
    FROM orochi.orochi_cdc_table_filters
    WHERE subscription_id = add_cdc_table_filter.subscription_id;

    INSERT INTO orochi.orochi_cdc_table_filters
        (subscription_id, schema_pattern, table_pattern, is_include, ordinal)
    VALUES
        (subscription_id, schema_pattern, table_pattern, is_include, new_ordinal)
    RETURNING orochi_cdc_table_filters.filter_id INTO filter_id;

    RETURN filter_id;
END;
$$;

COMMENT ON FUNCTION orochi.add_cdc_table_filter IS
    'Add a table filter to include/exclude tables from a CDC subscription';

-- Remove a table filter
CREATE OR REPLACE FUNCTION orochi.remove_cdc_table_filter(filter_id BIGINT)
RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    DELETE FROM orochi.orochi_cdc_table_filters WHERE filter_id = remove_cdc_table_filter.filter_id;
    RETURN FOUND;
END;
$$;

COMMENT ON FUNCTION orochi.remove_cdc_table_filter IS
    'Remove a table filter from a CDC subscription';

-- List filters for a subscription
CREATE OR REPLACE FUNCTION orochi.list_cdc_table_filters(subscription_id BIGINT)
RETURNS TABLE (
    filter_id BIGINT,
    schema_pattern TEXT,
    table_pattern TEXT,
    is_include BOOLEAN,
    ordinal INTEGER
)
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN QUERY
    SELECT
        f.filter_id,
        f.schema_pattern,
        f.table_pattern,
        f.is_include,
        f.ordinal
    FROM orochi.orochi_cdc_table_filters f
    WHERE f.subscription_id = list_cdc_table_filters.subscription_id
    ORDER BY f.ordinal;
END;
$$;

COMMENT ON FUNCTION orochi.list_cdc_table_filters IS
    'List all table filters for a CDC subscription';

-- ============================================================
-- CDC Views
-- ============================================================

-- View of all CDC subscriptions with their status
CREATE OR REPLACE VIEW orochi.cdc_subscriptions AS
SELECT
    s.subscription_id,
    s.name,
    CASE s.sink_type
        WHEN 0 THEN 'kafka'
        WHEN 1 THEN 'webhook'
        WHEN 2 THEN 'file'
        WHEN 3 THEN 'kinesis'
        WHEN 4 THEN 'pubsub'
    END AS sink_type,
    CASE s.output_format
        WHEN 0 THEN 'json'
        WHEN 1 THEN 'avro'
        WHEN 2 THEN 'debezium'
        WHEN 3 THEN 'protobuf'
    END AS output_format,
    CASE s.state
        WHEN 0 THEN 'created'
        WHEN 1 THEN 'active'
        WHEN 2 THEN 'paused'
        WHEN 3 THEN 'error'
        WHEN 4 THEN 'disabled'
    END AS state,
    s.include_schema,
    s.include_before,
    s.include_transaction,
    s.start_lsn,
    s.confirmed_lsn,
    s.created_at,
    s.started_at,
    s.stopped_at,
    s.worker_pid,
    s.options,
    st.events_captured,
    st.events_sent,
    st.events_failed,
    st.bytes_sent,
    st.throughput_per_sec,
    st.last_send_time
FROM orochi.orochi_cdc_subscriptions s
LEFT JOIN orochi.orochi_cdc_stats st ON s.subscription_id = st.subscription_id;

COMMENT ON VIEW orochi.cdc_subscriptions IS
    'View of all CDC subscriptions with their current status and statistics';

-- View of CDC subscription details with filters
CREATE OR REPLACE VIEW orochi.cdc_subscription_details AS
SELECT
    s.subscription_id,
    s.name,
    CASE s.sink_type
        WHEN 0 THEN 'kafka'
        WHEN 1 THEN 'webhook'
        WHEN 2 THEN 'file'
    END AS sink_type,
    CASE s.state
        WHEN 0 THEN 'created'
        WHEN 1 THEN 'active'
        WHEN 2 THEN 'paused'
        WHEN 3 THEN 'error'
        WHEN 4 THEN 'disabled'
    END AS state,
    (
        SELECT COUNT(*)
        FROM orochi.orochi_cdc_table_filters f
        WHERE f.subscription_id = s.subscription_id
    ) AS filter_count,
    (
        SELECT string_agg(
            CASE WHEN f.is_include THEN '+' ELSE '-' END ||
            f.schema_pattern || '.' || f.table_pattern,
            ', ' ORDER BY f.ordinal
        )
        FROM orochi.orochi_cdc_table_filters f
        WHERE f.subscription_id = s.subscription_id
    ) AS filters,
    s.options
FROM orochi.orochi_cdc_subscriptions s;

COMMENT ON VIEW orochi.cdc_subscription_details IS
    'Detailed view of CDC subscriptions including filter patterns';

-- ============================================================
-- Administrative Functions
-- ============================================================

-- Reset CDC statistics
CREATE OR REPLACE FUNCTION orochi.reset_cdc_stats(subscription_id BIGINT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE orochi.orochi_cdc_stats
    SET
        events_captured = 0,
        events_sent = 0,
        events_failed = 0,
        events_filtered = 0,
        bytes_sent = 0,
        avg_latency_ms = 0,
        throughput_per_sec = 0,
        last_capture_time = NULL,
        last_send_time = NULL,
        last_error_time = NULL,
        last_error_message = NULL,
        updated_at = NOW()
    WHERE subscription_id = reset_cdc_stats.subscription_id;

    IF NOT FOUND THEN
        RAISE NOTICE 'No stats found for CDC subscription %', subscription_id;
    END IF;
END;
$$;

COMMENT ON FUNCTION orochi.reset_cdc_stats IS
    'Reset statistics for a CDC subscription';

-- List active CDC subscriptions
CREATE OR REPLACE FUNCTION orochi.list_active_cdc_subscriptions()
RETURNS TABLE (
    subscription_id BIGINT,
    name TEXT,
    sink_type TEXT,
    worker_pid INTEGER,
    started_at TIMESTAMPTZ,
    events_sent BIGINT,
    throughput_per_sec DOUBLE PRECISION
)
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN QUERY
    SELECT
        s.subscription_id,
        s.name::TEXT,
        CASE s.sink_type
            WHEN 0 THEN 'kafka'
            WHEN 1 THEN 'webhook'
            WHEN 2 THEN 'file'
        END::TEXT AS sink_type,
        s.worker_pid,
        s.started_at,
        COALESCE(st.events_sent, 0),
        COALESCE(st.throughput_per_sec, 0)
    FROM orochi.orochi_cdc_subscriptions s
    LEFT JOIN orochi.orochi_cdc_stats st ON s.subscription_id = st.subscription_id
    WHERE s.state = 1;  -- Active
END;
$$;

COMMENT ON FUNCTION orochi.list_active_cdc_subscriptions IS
    'List all currently active CDC subscriptions';

-- Pause a CDC subscription
CREATE OR REPLACE FUNCTION orochi.pause_cdc_subscription(subscription_id BIGINT)
RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE orochi.orochi_cdc_subscriptions
    SET state = 2  -- Paused
    WHERE subscription_id = pause_cdc_subscription.subscription_id
    AND state = 1;  -- Active

    RETURN FOUND;
END;
$$;

COMMENT ON FUNCTION orochi.pause_cdc_subscription IS
    'Pause an active CDC subscription';

-- Resume a paused CDC subscription
CREATE OR REPLACE FUNCTION orochi.resume_cdc_subscription(subscription_id BIGINT)
RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
DECLARE
    current_state INTEGER;
BEGIN
    SELECT state INTO current_state
    FROM orochi.orochi_cdc_subscriptions
    WHERE subscription_id = resume_cdc_subscription.subscription_id;

    IF current_state IS NULL THEN
        RAISE EXCEPTION 'CDC subscription % does not exist', subscription_id;
    END IF;

    IF current_state != 2 THEN
        RAISE EXCEPTION 'CDC subscription % is not paused', subscription_id;
    END IF;

    -- Start the subscription
    RETURN orochi.start_cdc_subscription(subscription_id);
END;
$$;

COMMENT ON FUNCTION orochi.resume_cdc_subscription IS
    'Resume a paused CDC subscription';

-- Restart a CDC subscription (stop and start)
CREATE OR REPLACE FUNCTION orochi.restart_cdc_subscription(subscription_id BIGINT)
RETURNS BOOLEAN
LANGUAGE plpgsql
AS $$
BEGIN
    -- Stop first
    PERFORM orochi.stop_cdc_subscription(subscription_id);

    -- Wait a moment
    PERFORM pg_sleep(0.5);

    -- Start again
    RETURN orochi.start_cdc_subscription(subscription_id);
END;
$$;

COMMENT ON FUNCTION orochi.restart_cdc_subscription IS
    'Restart a CDC subscription';

-- ============================================================
-- CDC Monitoring Functions
-- ============================================================

-- Get CDC lag information
CREATE OR REPLACE FUNCTION orochi.cdc_lag()
RETURNS TABLE (
    subscription_id BIGINT,
    name TEXT,
    state TEXT,
    current_lsn PG_LSN,
    confirmed_lsn PG_LSN,
    lag_bytes BIGINT,
    last_send_time TIMESTAMPTZ,
    lag_time INTERVAL
)
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN QUERY
    SELECT
        s.subscription_id,
        s.name::TEXT,
        CASE s.state
            WHEN 0 THEN 'created'
            WHEN 1 THEN 'active'
            WHEN 2 THEN 'paused'
            WHEN 3 THEN 'error'
            WHEN 4 THEN 'disabled'
        END::TEXT AS state,
        st.current_lsn,
        st.confirmed_lsn,
        CASE WHEN st.current_lsn IS NOT NULL AND st.confirmed_lsn IS NOT NULL
             THEN (st.current_lsn - st.confirmed_lsn)::BIGINT
             ELSE NULL
        END AS lag_bytes,
        st.last_send_time,
        CASE WHEN st.last_send_time IS NOT NULL
             THEN NOW() - st.last_send_time
             ELSE NULL
        END AS lag_time
    FROM orochi.orochi_cdc_subscriptions s
    LEFT JOIN orochi.orochi_cdc_stats st ON s.subscription_id = st.subscription_id
    WHERE s.state IN (1, 2);  -- Active or Paused
END;
$$;

COMMENT ON FUNCTION orochi.cdc_lag IS
    'Get CDC replication lag information for all active subscriptions';

-- ============================================================
-- Example Usage
-- ============================================================

/*
-- Create a Kafka CDC subscription for all tables
SELECT orochi.create_kafka_cdc_subscription(
    'all_changes_kafka',
    'kafka-broker1:9092,kafka-broker2:9092',
    'orochi-cdc-events',
    'debezium'
);

-- Add filter to include only specific schemas
SELECT orochi.add_cdc_table_filter(1, 'public', '*', true);
SELECT orochi.add_cdc_table_filter(1, 'audit', '*', true);

-- Exclude certain tables
SELECT orochi.add_cdc_table_filter(1, '*', 'temp_*', false);
SELECT orochi.add_cdc_table_filter(1, '*', '*_log', false);

-- Start the subscription
SELECT orochi.start_cdc_subscription(1);

-- Create a webhook CDC subscription
SELECT orochi.create_webhook_cdc_subscription(
    'order_changes_webhook',
    'https://api.example.com/webhooks/cdc',
    'json',
    'bearer',
    'my-api-token'
);

-- Add filter for specific tables
SELECT orochi.add_cdc_table_filter(2, 'public', 'orders', true);
SELECT orochi.add_cdc_table_filter(2, 'public', 'order_items', true);

-- Start the subscription
SELECT orochi.start_cdc_subscription(2);

-- Create a file CDC subscription for debugging
SELECT orochi.create_file_cdc_subscription(
    'debug_cdc',
    '/var/log/orochi/cdc_debug.json',
    'json',
    true
);

-- Monitor subscriptions
SELECT * FROM orochi.cdc_subscriptions;

-- Check statistics
SELECT * FROM orochi.cdc_subscription_stats(1);

-- Check lag
SELECT * FROM orochi.cdc_lag();

-- Pause subscription for maintenance
SELECT orochi.pause_cdc_subscription(1);

-- Resume subscription
SELECT orochi.resume_cdc_subscription(1);

-- Stop subscription
SELECT orochi.stop_cdc_subscription(1);

-- Drop subscription
SELECT orochi.drop_cdc_subscription(1, true);
*/
