-- =============================================================================
-- Orochi DB Benchmark Database Initialization
-- =============================================================================
-- This script runs automatically when the PostgreSQL container starts.
-- It sets up the benchmark database with the Orochi extension.
-- =============================================================================

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;

-- Create Orochi extension (will fail gracefully if not available)
DO $$
BEGIN
    CREATE EXTENSION IF NOT EXISTS orochi;
    RAISE NOTICE 'Orochi extension created successfully';
EXCEPTION WHEN OTHERS THEN
    RAISE WARNING 'Could not create Orochi extension: %', SQLERRM;
END;
$$;

-- Create benchmark user with appropriate permissions
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'benchmark') THEN
        CREATE ROLE benchmark WITH LOGIN PASSWORD 'benchmark' CREATEDB;
    END IF;
END;
$$;

-- Grant permissions
GRANT ALL PRIVILEGES ON DATABASE orochi_bench TO benchmark;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA public TO benchmark;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO benchmark;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO benchmark;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON SEQUENCES TO benchmark;

-- Create benchmark metadata schema
CREATE SCHEMA IF NOT EXISTS benchmark_meta;

-- Table to track benchmark runs
CREATE TABLE IF NOT EXISTS benchmark_meta.runs (
    run_id SERIAL PRIMARY KEY,
    run_timestamp TIMESTAMPTZ DEFAULT NOW(),
    suite_name VARCHAR(64) NOT NULL,
    scale_factor INTEGER,
    num_clients INTEGER,
    num_threads INTEGER,
    duration_secs INTEGER,
    status VARCHAR(32) DEFAULT 'running',
    results JSONB,
    system_info JSONB,
    completed_at TIMESTAMPTZ
);

-- Table to track individual benchmark results
CREATE TABLE IF NOT EXISTS benchmark_meta.results (
    result_id SERIAL PRIMARY KEY,
    run_id INTEGER REFERENCES benchmark_meta.runs(run_id),
    benchmark_name VARCHAR(128) NOT NULL,
    metric_name VARCHAR(64) NOT NULL,
    metric_value DOUBLE PRECISION,
    metric_unit VARCHAR(32),
    timestamp TIMESTAMPTZ DEFAULT NOW(),
    metadata JSONB
);

-- Create indexes
CREATE INDEX IF NOT EXISTS idx_runs_timestamp ON benchmark_meta.runs(run_timestamp);
CREATE INDEX IF NOT EXISTS idx_runs_suite ON benchmark_meta.runs(suite_name);
CREATE INDEX IF NOT EXISTS idx_results_run ON benchmark_meta.results(run_id);
CREATE INDEX IF NOT EXISTS idx_results_benchmark ON benchmark_meta.results(benchmark_name);

-- Function to record benchmark start
CREATE OR REPLACE FUNCTION benchmark_meta.start_run(
    p_suite_name VARCHAR,
    p_scale_factor INTEGER DEFAULT 1,
    p_num_clients INTEGER DEFAULT 1,
    p_num_threads INTEGER DEFAULT 1,
    p_duration_secs INTEGER DEFAULT 60,
    p_system_info JSONB DEFAULT '{}'
) RETURNS INTEGER AS $$
DECLARE
    v_run_id INTEGER;
BEGIN
    INSERT INTO benchmark_meta.runs (suite_name, scale_factor, num_clients, num_threads, duration_secs, system_info)
    VALUES (p_suite_name, p_scale_factor, p_num_clients, p_num_threads, p_duration_secs, p_system_info)
    RETURNING run_id INTO v_run_id;

    RETURN v_run_id;
END;
$$ LANGUAGE plpgsql;

-- Function to complete benchmark run
CREATE OR REPLACE FUNCTION benchmark_meta.complete_run(
    p_run_id INTEGER,
    p_results JSONB DEFAULT '{}'
) RETURNS VOID AS $$
BEGIN
    UPDATE benchmark_meta.runs
    SET status = 'completed',
        results = p_results,
        completed_at = NOW()
    WHERE run_id = p_run_id;
END;
$$ LANGUAGE plpgsql;

-- Function to record individual metric
CREATE OR REPLACE FUNCTION benchmark_meta.record_metric(
    p_run_id INTEGER,
    p_benchmark_name VARCHAR,
    p_metric_name VARCHAR,
    p_metric_value DOUBLE PRECISION,
    p_metric_unit VARCHAR DEFAULT NULL,
    p_metadata JSONB DEFAULT '{}'
) RETURNS VOID AS $$
BEGIN
    INSERT INTO benchmark_meta.results (run_id, benchmark_name, metric_name, metric_value, metric_unit, metadata)
    VALUES (p_run_id, p_benchmark_name, p_metric_name, p_metric_value, p_metric_unit, p_metadata);
END;
$$ LANGUAGE plpgsql;

-- View for benchmark summary
CREATE OR REPLACE VIEW benchmark_meta.run_summary AS
SELECT
    r.run_id,
    r.suite_name,
    r.run_timestamp,
    r.completed_at,
    r.status,
    r.scale_factor,
    r.num_clients,
    EXTRACT(EPOCH FROM (r.completed_at - r.run_timestamp)) AS duration_actual_secs,
    COUNT(res.result_id) AS metric_count,
    AVG(CASE WHEN res.metric_name = 'tps' THEN res.metric_value END) AS avg_tps,
    AVG(CASE WHEN res.metric_name = 'latency_avg_ms' THEN res.metric_value END) AS avg_latency_ms
FROM benchmark_meta.runs r
LEFT JOIN benchmark_meta.results res ON r.run_id = res.run_id
GROUP BY r.run_id, r.suite_name, r.run_timestamp, r.completed_at, r.status, r.scale_factor, r.num_clients;

-- Grant permissions on benchmark schema
GRANT USAGE ON SCHEMA benchmark_meta TO benchmark;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA benchmark_meta TO benchmark;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA benchmark_meta TO benchmark;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA benchmark_meta TO benchmark;

-- Log successful initialization
DO $$
BEGIN
    RAISE NOTICE 'Benchmark database initialized successfully';
END;
$$;
