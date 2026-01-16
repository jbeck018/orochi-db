-- ddl.sql
-- Orochi DB Workflow Automation DDL definitions
-- Provides Snowflake/Databricks-style workflow DDL patterns
-- Copyright (c) 2024, Orochi DB Contributors

-- ============================================================
-- Workflow Catalog Tables
-- ============================================================

-- Main workflow registry
CREATE TABLE IF NOT EXISTS orochi.orochi_workflows (
    workflow_id BIGSERIAL PRIMARY KEY,
    workflow_name TEXT NOT NULL,
    description TEXT,
    schedule TEXT,  -- Cron expression or NULL for manual
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    auto_resume BOOLEAN NOT NULL DEFAULT FALSE,
    max_concurrent INTEGER NOT NULL DEFAULT 1,
    warehouse TEXT,
    error_integration TEXT,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=created, 1=running, 2=succeeded, 3=failed
    last_run_id BIGINT,
    total_runs BIGINT NOT NULL DEFAULT 0,
    successful_runs BIGINT NOT NULL DEFAULT 0,
    failed_runs BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_run_at TIMESTAMPTZ,
    next_run_at TIMESTAMPTZ,
    UNIQUE(workflow_name)
);

-- Workflow step definitions
CREATE TABLE IF NOT EXISTS orochi.orochi_workflow_steps (
    step_id INTEGER NOT NULL,
    workflow_id BIGINT NOT NULL REFERENCES orochi.orochi_workflows(workflow_id) ON DELETE CASCADE,
    step_name TEXT NOT NULL,
    step_type INTEGER NOT NULL,  -- 0=stage, 1=transform, 2=load, 3=merge, etc.
    config JSONB,  -- Step-specific configuration
    timeout_seconds INTEGER NOT NULL DEFAULT 3600,
    retry_count INTEGER NOT NULL DEFAULT 3,
    retry_delay_ms INTEGER NOT NULL DEFAULT 60000,
    continue_on_error BOOLEAN NOT NULL DEFAULT FALSE,
    condition TEXT,  -- Optional SQL condition
    depends_on INTEGER[],  -- Step IDs this depends on
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (workflow_id, step_id)
);

-- Workflow run history
CREATE TABLE IF NOT EXISTS orochi.orochi_workflow_runs (
    run_id BIGSERIAL PRIMARY KEY,
    workflow_id BIGINT NOT NULL REFERENCES orochi.orochi_workflows(workflow_id) ON DELETE CASCADE,
    state INTEGER NOT NULL DEFAULT 1,  -- 0=created, 1=running, 2=succeeded, 3=failed
    started_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at TIMESTAMPTZ,
    duration_ms BIGINT,
    steps_total INTEGER NOT NULL DEFAULT 0,
    steps_completed INTEGER NOT NULL DEFAULT 0,
    steps_failed INTEGER NOT NULL DEFAULT 0,
    rows_processed BIGINT NOT NULL DEFAULT 0,
    bytes_processed BIGINT NOT NULL DEFAULT 0,
    error_message TEXT,
    failed_step_id INTEGER
);

-- Workflow step run history
CREATE TABLE IF NOT EXISTS orochi.orochi_workflow_step_runs (
    step_run_id BIGSERIAL PRIMARY KEY,
    run_id BIGINT NOT NULL REFERENCES orochi.orochi_workflow_runs(run_id) ON DELETE CASCADE,
    step_id INTEGER NOT NULL,
    state INTEGER NOT NULL DEFAULT 0,
    started_at TIMESTAMPTZ,
    finished_at TIMESTAMPTZ,
    duration_ms BIGINT,
    rows_processed BIGINT NOT NULL DEFAULT 0,
    bytes_processed BIGINT NOT NULL DEFAULT 0,
    error_message TEXT,
    attempt_number INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS idx_workflow_runs_workflow ON orochi.orochi_workflow_runs(workflow_id);
CREATE INDEX IF NOT EXISTS idx_workflow_runs_state ON orochi.orochi_workflow_runs(state);
CREATE INDEX IF NOT EXISTS idx_workflow_step_runs_run ON orochi.orochi_workflow_step_runs(run_id);

-- ============================================================
-- Stream Catalog Tables
-- ============================================================

-- Main stream registry
CREATE TABLE IF NOT EXISTS orochi.orochi_streams (
    stream_id BIGSERIAL PRIMARY KEY,
    stream_name TEXT NOT NULL,
    stream_schema TEXT NOT NULL DEFAULT 'public',
    source_table_oid OID NOT NULL,
    source_schema TEXT NOT NULL,
    source_table TEXT NOT NULL,
    stream_type INTEGER NOT NULL DEFAULT 0,  -- 0=standard, 1=append_only
    stream_mode INTEGER NOT NULL DEFAULT 0,  -- 0=default, 1=keys_only, 2=full_before
    show_initial_rows BOOLEAN NOT NULL DEFAULT FALSE,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=active, 1=stale, 2=paused, 3=disabled
    has_pending_data BOOLEAN NOT NULL DEFAULT FALSE,
    total_changes BIGINT NOT NULL DEFAULT 0,
    pending_changes BIGINT NOT NULL DEFAULT 0,
    bytes_captured BIGINT NOT NULL DEFAULT 0,
    stale_after TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_consumed_at TIMESTAMPTZ,
    UNIQUE(stream_schema, stream_name)
);

-- Stream column tracking
CREATE TABLE IF NOT EXISTS orochi.orochi_stream_columns (
    column_id BIGSERIAL PRIMARY KEY,
    stream_id BIGINT NOT NULL REFERENCES orochi.orochi_streams(stream_id) ON DELETE CASCADE,
    column_name TEXT NOT NULL,
    column_type OID NOT NULL,
    is_key BOOLEAN NOT NULL DEFAULT FALSE,
    track_changes BOOLEAN NOT NULL DEFAULT TRUE,
    UNIQUE(stream_id, column_name)
);

-- Stream offset tracking
CREATE TABLE IF NOT EXISTS orochi.orochi_stream_offsets (
    stream_id BIGINT PRIMARY KEY REFERENCES orochi.orochi_streams(stream_id) ON DELETE CASCADE,
    lsn TEXT NOT NULL,
    xid INTEGER NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL,
    sequence BIGINT NOT NULL DEFAULT 0,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_streams_source ON orochi.orochi_streams(source_table_oid);
CREATE INDEX IF NOT EXISTS idx_streams_state ON orochi.orochi_streams(state);

-- ============================================================
-- Task Catalog Tables
-- ============================================================

-- Main task registry
CREATE TABLE IF NOT EXISTS orochi.orochi_tasks (
    task_id BIGSERIAL PRIMARY KEY,
    task_name TEXT NOT NULL,
    task_schema TEXT NOT NULL DEFAULT 'public',
    description TEXT,
    sql_text TEXT NOT NULL,
    schedule_type INTEGER NOT NULL DEFAULT 0,  -- 0=cron, 1=interval, 2=trigger
    cron_expression TEXT,
    interval_value INTERVAL,
    timezone TEXT DEFAULT 'UTC',
    condition_type INTEGER NOT NULL DEFAULT 0,  -- 0=none, 1=stream, 2=expression
    condition_expression TEXT,
    condition_stream_id BIGINT,
    warehouse TEXT,
    timeout_seconds INTEGER NOT NULL DEFAULT 3600,
    allow_overlapping BOOLEAN NOT NULL DEFAULT FALSE,
    error_limit INTEGER NOT NULL DEFAULT 3,
    suspend_after_failures BOOLEAN NOT NULL DEFAULT TRUE,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=created, 1=started, 2=suspended, 3=failed
    consecutive_failures INTEGER NOT NULL DEFAULT 0,
    owner_oid OID,
    total_runs BIGINT NOT NULL DEFAULT 0,
    successful_runs BIGINT NOT NULL DEFAULT 0,
    failed_runs BIGINT NOT NULL DEFAULT 0,
    skipped_runs BIGINT NOT NULL DEFAULT 0,
    avg_duration_ms DOUBLE PRECISION,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_run_at TIMESTAMPTZ,
    next_run_at TIMESTAMPTZ,
    UNIQUE(task_schema, task_name)
);

-- Task dependencies (DAG)
CREATE TABLE IF NOT EXISTS orochi.orochi_task_dependencies (
    task_id BIGINT NOT NULL REFERENCES orochi.orochi_tasks(task_id) ON DELETE CASCADE,
    predecessor_id BIGINT REFERENCES orochi.orochi_tasks(task_id) ON DELETE CASCADE,
    predecessor_name TEXT,  -- Used when predecessor_id not resolved yet
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (task_id, COALESCE(predecessor_id, 0))
);

-- Task run history
CREATE TABLE IF NOT EXISTS orochi.orochi_task_runs (
    run_id BIGSERIAL PRIMARY KEY,
    task_id BIGINT NOT NULL REFERENCES orochi.orochi_tasks(task_id) ON DELETE CASCADE,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=scheduled, 1=running, 2=succeeded, 3=failed
    scheduled_time TIMESTAMPTZ NOT NULL,
    started_at TIMESTAMPTZ,
    finished_at TIMESTAMPTZ,
    duration_ms BIGINT,
    rows_affected BIGINT NOT NULL DEFAULT 0,
    rows_produced BIGINT NOT NULL DEFAULT 0,
    bytes_scanned BIGINT NOT NULL DEFAULT 0,
    bytes_written BIGINT NOT NULL DEFAULT 0,
    attempt_number INTEGER NOT NULL DEFAULT 1,
    error_message TEXT,
    query_id TEXT
);

CREATE INDEX IF NOT EXISTS idx_tasks_state ON orochi.orochi_tasks(state);
CREATE INDEX IF NOT EXISTS idx_tasks_next_run ON orochi.orochi_tasks(next_run_at) WHERE state = 1;
CREATE INDEX IF NOT EXISTS idx_task_runs_task ON orochi.orochi_task_runs(task_id);
CREATE INDEX IF NOT EXISTS idx_task_runs_state ON orochi.orochi_task_runs(state);
CREATE INDEX IF NOT EXISTS idx_task_deps_predecessor ON orochi.orochi_task_dependencies(predecessor_id);

-- ============================================================
-- Dynamic Table Catalog Tables
-- ============================================================

-- Main dynamic table registry
CREATE TABLE IF NOT EXISTS orochi.orochi_dynamic_tables (
    dynamic_table_id BIGSERIAL PRIMARY KEY,
    table_name TEXT NOT NULL,
    table_schema TEXT NOT NULL DEFAULT 'public',
    table_oid OID,  -- Materialization table OID
    description TEXT,
    query_text TEXT NOT NULL,
    query_view_oid OID,  -- Internal query view OID
    refresh_mode INTEGER NOT NULL DEFAULT 0,  -- 0=auto, 1=full, 2=incremental
    schedule_mode INTEGER NOT NULL DEFAULT 0,  -- 0=target_lag, 1=downstream
    target_lag INTERVAL NOT NULL,
    warehouse TEXT,
    initialize BOOLEAN NOT NULL DEFAULT TRUE,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=active, 1=suspended, 2=failed
    owner_oid OID,
    total_refreshes BIGINT NOT NULL DEFAULT 0,
    successful_refreshes BIGINT NOT NULL DEFAULT 0,
    failed_refreshes BIGINT NOT NULL DEFAULT 0,
    avg_refresh_ms DOUBLE PRECISION,
    total_rows BIGINT NOT NULL DEFAULT 0,
    total_bytes BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_refresh_at TIMESTAMPTZ,
    next_refresh_at TIMESTAMPTZ,
    UNIQUE(table_schema, table_name)
);

-- Dynamic table source dependencies
CREATE TABLE IF NOT EXISTS orochi.orochi_dynamic_sources (
    source_id BIGSERIAL PRIMARY KEY,
    dynamic_table_id BIGINT NOT NULL REFERENCES orochi.orochi_dynamic_tables(dynamic_table_id) ON DELETE CASCADE,
    source_oid OID,
    source_schema TEXT NOT NULL,
    source_name TEXT NOT NULL,
    is_dynamic_table BOOLEAN NOT NULL DEFAULT FALSE,
    source_dynamic_id BIGINT REFERENCES orochi.orochi_dynamic_tables(dynamic_table_id),
    UNIQUE(dynamic_table_id, source_schema, source_name)
);

-- Dynamic table refresh history
CREATE TABLE IF NOT EXISTS orochi.orochi_dynamic_refreshes (
    refresh_id BIGSERIAL PRIMARY KEY,
    dynamic_table_id BIGINT NOT NULL REFERENCES orochi.orochi_dynamic_tables(dynamic_table_id) ON DELETE CASCADE,
    refresh_mode INTEGER NOT NULL DEFAULT 0,
    started_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at TIMESTAMPTZ,
    duration_ms BIGINT,
    rows_inserted BIGINT NOT NULL DEFAULT 0,
    rows_updated BIGINT NOT NULL DEFAULT 0,
    rows_deleted BIGINT NOT NULL DEFAULT 0,
    bytes_produced BIGINT NOT NULL DEFAULT 0,
    source_changes BIGINT NOT NULL DEFAULT 0,
    bytes_scanned BIGINT NOT NULL DEFAULT 0,
    success BOOLEAN NOT NULL DEFAULT FALSE,
    error_message TEXT,
    lag_before INTERVAL,
    lag_after INTERVAL
);

-- Dynamic table cluster keys
CREATE TABLE IF NOT EXISTS orochi.orochi_dynamic_cluster_keys (
    dynamic_table_id BIGINT NOT NULL REFERENCES orochi.orochi_dynamic_tables(dynamic_table_id) ON DELETE CASCADE,
    key_position INTEGER NOT NULL,
    column_name TEXT NOT NULL,
    PRIMARY KEY (dynamic_table_id, key_position)
);

CREATE INDEX IF NOT EXISTS idx_dynamic_tables_state ON orochi.orochi_dynamic_tables(state);
CREATE INDEX IF NOT EXISTS idx_dynamic_tables_next_refresh ON orochi.orochi_dynamic_tables(next_refresh_at) WHERE state = 0;
CREATE INDEX IF NOT EXISTS idx_dynamic_sources_dt ON orochi.orochi_dynamic_sources(dynamic_table_id);
CREATE INDEX IF NOT EXISTS idx_dynamic_refreshes_dt ON orochi.orochi_dynamic_refreshes(dynamic_table_id);

-- ============================================================
-- Workflow DDL Functions
-- ============================================================

-- Create a workflow
CREATE FUNCTION create_workflow(
    name text,
    definition text
) RETURNS bigint AS $$
DECLARE
    v_workflow_id bigint;
BEGIN
    SELECT orochi._create_workflow(name, definition) INTO v_workflow_id;
    RAISE NOTICE 'Created workflow % with ID %', name, v_workflow_id;
    RETURN v_workflow_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_workflow(name text, definition text)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_create_workflow_sql'
    LANGUAGE C STRICT;

-- Drop a workflow
CREATE FUNCTION drop_workflow(
    workflow_id bigint,
    if_exists boolean DEFAULT FALSE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_workflow(workflow_id, if_exists);
    RAISE NOTICE 'Dropped workflow %', workflow_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_workflow(workflow_id bigint, if_exists boolean)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drop_workflow_sql'
    LANGUAGE C;

-- Run a workflow
CREATE FUNCTION run_workflow(
    workflow_id bigint
) RETURNS bigint AS $$
DECLARE
    v_run_id bigint;
BEGIN
    SELECT orochi._run_workflow(workflow_id) INTO v_run_id;
    RAISE NOTICE 'Started workflow % run %', workflow_id, v_run_id;
    RETURN v_run_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._run_workflow(workflow_id bigint)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_run_workflow_sql'
    LANGUAGE C STRICT;

-- Get workflow status
CREATE FUNCTION workflow_status(workflow_id bigint)
RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_workflow_status_sql'
    LANGUAGE C STRICT;

-- View for workflows
CREATE VIEW orochi.workflows AS
SELECT
    workflow_id,
    workflow_name,
    description,
    schedule,
    enabled,
    CASE state
        WHEN 0 THEN 'CREATED'
        WHEN 1 THEN 'RUNNING'
        WHEN 2 THEN 'SUCCEEDED'
        WHEN 3 THEN 'FAILED'
        WHEN 4 THEN 'PAUSED'
        WHEN 5 THEN 'SUSPENDED'
    END as state,
    total_runs,
    successful_runs,
    failed_runs,
    created_at,
    last_run_at,
    next_run_at
FROM orochi.orochi_workflows;

-- ============================================================
-- Stream DDL Functions
-- ============================================================

-- Create a stream on a table
CREATE FUNCTION create_stream(
    name text,
    source_table regclass,
    append_only boolean DEFAULT FALSE,
    show_initial_rows boolean DEFAULT FALSE
) RETURNS bigint AS $$
DECLARE
    v_stream_id bigint;
BEGIN
    SELECT orochi._create_stream(name, source_table) INTO v_stream_id;
    RAISE NOTICE 'Created stream % on table % with ID %', name, source_table, v_stream_id;
    RETURN v_stream_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_stream(name text, source_table oid)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_create_stream_sql'
    LANGUAGE C STRICT;

-- Drop a stream
CREATE FUNCTION drop_stream(
    stream_id bigint,
    if_exists boolean DEFAULT FALSE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_stream(stream_id, if_exists);
    RAISE NOTICE 'Dropped stream %', stream_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_stream(stream_id bigint, if_exists boolean)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drop_stream_sql'
    LANGUAGE C;

-- Check if stream has data
CREATE FUNCTION stream_has_data(stream_id bigint)
RETURNS boolean
    AS 'MODULE_PATHNAME', 'orochi_stream_has_data_sql'
    LANGUAGE C STRICT;

-- Get stream status
CREATE FUNCTION stream_status(stream_id bigint)
RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_stream_status_sql'
    LANGUAGE C STRICT;

-- View for streams
CREATE VIEW orochi.streams AS
SELECT
    s.stream_id,
    s.stream_name,
    s.stream_schema,
    s.source_schema || '.' || s.source_table as source_table,
    CASE s.stream_type
        WHEN 0 THEN 'STANDARD'
        WHEN 1 THEN 'APPEND_ONLY'
    END as stream_type,
    CASE s.state
        WHEN 0 THEN 'ACTIVE'
        WHEN 1 THEN 'STALE'
        WHEN 2 THEN 'PAUSED'
        WHEN 3 THEN 'DISABLED'
    END as state,
    s.has_pending_data,
    s.pending_changes,
    s.created_at,
    s.last_consumed_at
FROM orochi.orochi_streams s;

-- ============================================================
-- Task DDL Functions
-- ============================================================

-- Create a scheduled task
CREATE FUNCTION create_task(
    name text,
    schedule text,
    sql_statement text,
    warehouse text DEFAULT NULL
) RETURNS bigint AS $$
DECLARE
    v_task_id bigint;
BEGIN
    SELECT orochi._create_task(name, schedule, sql_statement) INTO v_task_id;
    RAISE NOTICE 'Created task % with ID %', name, v_task_id;
    RETURN v_task_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_task(name text, schedule text, sql_statement text)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_create_task_sql'
    LANGUAGE C STRICT;

-- Drop a task
CREATE FUNCTION drop_task(
    task_id bigint,
    if_exists boolean DEFAULT FALSE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_task(task_id, if_exists);
    RAISE NOTICE 'Dropped task %', task_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_task(task_id bigint, if_exists boolean)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drop_task_sql'
    LANGUAGE C;

-- Resume a task (enable scheduling)
CREATE FUNCTION resume_task(task_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_resume_task_sql'
    LANGUAGE C STRICT;

-- Suspend a task (disable scheduling)
CREATE FUNCTION suspend_task(task_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_suspend_task_sql'
    LANGUAGE C STRICT;

-- Execute a task manually
CREATE FUNCTION execute_task(task_id bigint)
RETURNS bigint AS $$
DECLARE
    v_run_id bigint;
BEGIN
    SELECT orochi._execute_task(task_id) INTO v_run_id;
    RAISE NOTICE 'Executed task % run %', task_id, v_run_id;
    RETURN v_run_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._execute_task(task_id bigint)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_execute_task_sql'
    LANGUAGE C STRICT;

-- Get task status
CREATE FUNCTION task_status(task_id bigint)
RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_task_status_sql'
    LANGUAGE C STRICT;

-- View for tasks
CREATE VIEW orochi.tasks AS
SELECT
    t.task_id,
    t.task_name,
    t.task_schema,
    CASE t.schedule_type
        WHEN 0 THEN 'CRON'
        WHEN 1 THEN 'INTERVAL'
        WHEN 2 THEN 'TRIGGER'
    END as schedule_type,
    t.cron_expression as schedule,
    CASE t.state
        WHEN 0 THEN 'CREATED'
        WHEN 1 THEN 'STARTED'
        WHEN 2 THEN 'SUSPENDED'
        WHEN 3 THEN 'FAILED'
    END as state,
    t.total_runs,
    t.successful_runs,
    t.failed_runs,
    t.avg_duration_ms,
    t.created_at,
    t.last_run_at,
    t.next_run_at
FROM orochi.orochi_tasks t;

-- View for task dependencies
CREATE VIEW orochi.task_dependencies AS
SELECT
    t.task_name,
    p.task_name as predecessor_name
FROM orochi.orochi_task_dependencies d
JOIN orochi.orochi_tasks t ON d.task_id = t.task_id
LEFT JOIN orochi.orochi_tasks p ON d.predecessor_id = p.task_id;

-- ============================================================
-- Dynamic Table DDL Functions
-- ============================================================

-- Create a dynamic table
CREATE FUNCTION create_dynamic_table(
    name text,
    query text,
    target_lag interval,
    refresh_mode text DEFAULT 'AUTO',
    initialize boolean DEFAULT TRUE
) RETURNS bigint AS $$
DECLARE
    v_dt_id bigint;
BEGIN
    SELECT orochi._create_dynamic_table(name, query, target_lag) INTO v_dt_id;
    RAISE NOTICE 'Created dynamic table % with ID %', name, v_dt_id;
    RETURN v_dt_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._create_dynamic_table(name text, query text, target_lag interval)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_create_dynamic_table_sql'
    LANGUAGE C STRICT;

-- Drop a dynamic table
CREATE FUNCTION drop_dynamic_table(
    dynamic_table_id bigint,
    if_exists boolean DEFAULT FALSE
) RETURNS void AS $$
BEGIN
    PERFORM orochi._drop_dynamic_table(dynamic_table_id, if_exists);
    RAISE NOTICE 'Dropped dynamic table %', dynamic_table_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._drop_dynamic_table(dynamic_table_id bigint, if_exists boolean)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_drop_dynamic_table_sql'
    LANGUAGE C;

-- Manually refresh a dynamic table
CREATE FUNCTION refresh_dynamic_table(dynamic_table_id bigint)
RETURNS bigint AS $$
DECLARE
    v_refresh_id bigint;
BEGIN
    SELECT orochi._refresh_dynamic_table(dynamic_table_id) INTO v_refresh_id;
    RAISE NOTICE 'Refreshed dynamic table % refresh %', dynamic_table_id, v_refresh_id;
    RETURN v_refresh_id;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION orochi._refresh_dynamic_table(dynamic_table_id bigint)
RETURNS bigint
    AS 'MODULE_PATHNAME', 'orochi_refresh_dynamic_table_sql'
    LANGUAGE C STRICT;

-- Suspend a dynamic table
CREATE FUNCTION suspend_dynamic_table(dynamic_table_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_suspend_dynamic_table_sql'
    LANGUAGE C STRICT;

-- Resume a dynamic table
CREATE FUNCTION resume_dynamic_table(dynamic_table_id bigint)
RETURNS void
    AS 'MODULE_PATHNAME', 'orochi_resume_dynamic_table_sql'
    LANGUAGE C STRICT;

-- Get dynamic table status
CREATE FUNCTION dynamic_table_status(dynamic_table_id bigint)
RETURNS text
    AS 'MODULE_PATHNAME', 'orochi_dynamic_table_status_sql'
    LANGUAGE C STRICT;

-- View for dynamic tables
CREATE VIEW orochi.dynamic_tables AS
SELECT
    dt.dynamic_table_id,
    dt.table_name,
    dt.table_schema,
    dt.target_lag,
    CASE dt.refresh_mode
        WHEN 0 THEN 'AUTO'
        WHEN 1 THEN 'FULL'
        WHEN 2 THEN 'INCREMENTAL'
    END as refresh_mode,
    CASE dt.state
        WHEN 0 THEN 'ACTIVE'
        WHEN 1 THEN 'SUSPENDED'
        WHEN 2 THEN 'FAILED'
        WHEN 3 THEN 'INITIALIZING'
        WHEN 4 THEN 'REFRESHING'
    END as state,
    dt.total_refreshes,
    dt.successful_refreshes,
    dt.failed_refreshes,
    dt.avg_refresh_ms,
    dt.total_rows,
    pg_size_pretty(dt.total_bytes) as total_size,
    dt.created_at,
    dt.last_refresh_at,
    dt.next_refresh_at
FROM orochi.orochi_dynamic_tables dt;

-- View for dynamic table sources
CREATE VIEW orochi.dynamic_table_sources AS
SELECT
    dt.table_name as dynamic_table,
    ds.source_schema || '.' || ds.source_name as source_table,
    ds.is_dynamic_table
FROM orochi.orochi_dynamic_sources ds
JOIN orochi.orochi_dynamic_tables dt ON ds.dynamic_table_id = dt.dynamic_table_id;

-- ============================================================
-- Utility Functions
-- ============================================================

-- Get task run history
CREATE FUNCTION task_run_history(
    task_id bigint,
    limit_rows integer DEFAULT 10
)
RETURNS TABLE(
    run_id bigint,
    state text,
    scheduled_time timestamptz,
    started_at timestamptz,
    duration_ms bigint,
    rows_affected bigint,
    error_message text
) AS $$
    SELECT
        run_id,
        CASE state
            WHEN 0 THEN 'SCHEDULED'
            WHEN 1 THEN 'RUNNING'
            WHEN 2 THEN 'SUCCEEDED'
            WHEN 3 THEN 'FAILED'
            WHEN 4 THEN 'SKIPPED'
            WHEN 5 THEN 'CANCELLED'
        END as state,
        scheduled_time,
        started_at,
        duration_ms,
        rows_affected,
        error_message
    FROM orochi.orochi_task_runs
    WHERE orochi.orochi_task_runs.task_id = task_run_history.task_id
    ORDER BY run_id DESC
    LIMIT limit_rows;
$$ LANGUAGE sql;

-- Get dynamic table refresh history
CREATE FUNCTION dynamic_table_refresh_history(
    dynamic_table_id bigint,
    limit_rows integer DEFAULT 10
)
RETURNS TABLE(
    refresh_id bigint,
    refresh_mode text,
    started_at timestamptz,
    duration_ms bigint,
    rows_inserted bigint,
    rows_updated bigint,
    rows_deleted bigint,
    success boolean,
    error_message text
) AS $$
    SELECT
        refresh_id,
        CASE refresh_mode
            WHEN 0 THEN 'AUTO'
            WHEN 1 THEN 'FULL'
            WHEN 2 THEN 'INCREMENTAL'
        END as refresh_mode,
        started_at,
        duration_ms,
        rows_inserted,
        rows_updated,
        rows_deleted,
        success,
        error_message
    FROM orochi.orochi_dynamic_refreshes
    WHERE orochi.orochi_dynamic_refreshes.dynamic_table_id = dynamic_table_refresh_history.dynamic_table_id
    ORDER BY refresh_id DESC
    LIMIT limit_rows;
$$ LANGUAGE sql;

-- SYSTEM$STREAM_HAS_DATA function (Snowflake compatibility)
CREATE FUNCTION system_stream_has_data(stream_name text)
RETURNS boolean AS $$
DECLARE
    v_stream_id bigint;
    v_has_data boolean;
BEGIN
    SELECT stream_id INTO v_stream_id
    FROM orochi.orochi_streams
    WHERE orochi.orochi_streams.stream_name = system_stream_has_data.stream_name;

    IF v_stream_id IS NULL THEN
        RAISE EXCEPTION 'Stream % does not exist', stream_name;
    END IF;

    SELECT stream_has_data(v_stream_id) INTO v_has_data;
    RETURN v_has_data;
END;
$$ LANGUAGE plpgsql;
