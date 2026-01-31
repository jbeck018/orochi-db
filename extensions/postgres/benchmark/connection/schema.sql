-- Orochi DB Connection Pooler Benchmark Schema
--
-- Minimal schema optimized for connection benchmarks.
-- Focus is on connection establishment and lightweight queries.

-- Drop existing benchmark objects
DROP TABLE IF EXISTS connection_bench_metrics CASCADE;
DROP TABLE IF EXISTS connection_bench_simple CASCADE;
DROP FUNCTION IF EXISTS bench_ping() CASCADE;
DROP FUNCTION IF EXISTS bench_current_time() CASCADE;
DROP FUNCTION IF EXISTS bench_random_int(int, int) CASCADE;

-- Simple table for lightweight queries
-- Minimal columns to reduce query overhead
CREATE TABLE IF NOT EXISTS connection_bench_simple (
    id          SERIAL PRIMARY KEY,
    value       INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Pre-populate with some rows for SELECT queries
INSERT INTO connection_bench_simple (value)
SELECT generate_series(1, 1000);

-- Metrics table for storing benchmark results
CREATE TABLE IF NOT EXISTS connection_bench_metrics (
    id              SERIAL PRIMARY KEY,
    benchmark_name  VARCHAR(128) NOT NULL,
    pooler_type     VARCHAR(32) NOT NULL,
    num_clients     INTEGER NOT NULL,
    tps             DOUBLE PRECISION,
    avg_latency_us  DOUBLE PRECISION,
    p50_latency_us  DOUBLE PRECISION,
    p95_latency_us  DOUBLE PRECISION,
    p99_latency_us  DOUBLE PRECISION,
    p999_latency_us DOUBLE PRECISION,
    total_queries   BIGINT,
    failed_queries  BIGINT,
    duration_secs   DOUBLE PRECISION,
    metadata        JSONB,
    created_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Index for querying benchmark results
CREATE INDEX IF NOT EXISTS idx_bench_metrics_pooler
ON connection_bench_metrics(pooler_type, benchmark_name, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_bench_metrics_clients
ON connection_bench_metrics(num_clients, pooler_type);

-- Simple ping function (minimal overhead)
CREATE OR REPLACE FUNCTION bench_ping()
RETURNS INTEGER AS $$
BEGIN
    RETURN 1;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- Function to return current timestamp
CREATE OR REPLACE FUNCTION bench_current_time()
RETURNS TIMESTAMP WITH TIME ZONE AS $$
BEGIN
    RETURN NOW();
END;
$$ LANGUAGE plpgsql STABLE;

-- Function to generate random integer
CREATE OR REPLACE FUNCTION bench_random_int(min_val INTEGER, max_val INTEGER)
RETURNS INTEGER AS $$
BEGIN
    RETURN floor(random() * (max_val - min_val + 1) + min_val)::INTEGER;
END;
$$ LANGUAGE plpgsql VOLATILE;

-- View for quick benchmark comparison
CREATE OR REPLACE VIEW connection_bench_comparison AS
SELECT
    benchmark_name,
    pooler_type,
    num_clients,
    ROUND(AVG(tps)::numeric, 2) AS avg_tps,
    ROUND(AVG(avg_latency_us)::numeric, 2) AS avg_latency_us,
    ROUND(AVG(p95_latency_us)::numeric, 2) AS avg_p95_us,
    ROUND(AVG(p99_latency_us)::numeric, 2) AS avg_p99_us,
    COUNT(*) AS num_runs,
    MAX(created_at) AS last_run
FROM connection_bench_metrics
GROUP BY benchmark_name, pooler_type, num_clients
ORDER BY benchmark_name, pooler_type, num_clients;

-- Function to store benchmark result
CREATE OR REPLACE FUNCTION store_bench_result(
    p_benchmark_name VARCHAR(128),
    p_pooler_type VARCHAR(32),
    p_num_clients INTEGER,
    p_tps DOUBLE PRECISION,
    p_avg_latency_us DOUBLE PRECISION,
    p_p50_latency_us DOUBLE PRECISION DEFAULT NULL,
    p_p95_latency_us DOUBLE PRECISION DEFAULT NULL,
    p_p99_latency_us DOUBLE PRECISION DEFAULT NULL,
    p_p999_latency_us DOUBLE PRECISION DEFAULT NULL,
    p_total_queries BIGINT DEFAULT 0,
    p_failed_queries BIGINT DEFAULT 0,
    p_duration_secs DOUBLE PRECISION DEFAULT 0,
    p_metadata JSONB DEFAULT NULL
) RETURNS INTEGER AS $$
DECLARE
    v_id INTEGER;
BEGIN
    INSERT INTO connection_bench_metrics (
        benchmark_name, pooler_type, num_clients, tps, avg_latency_us,
        p50_latency_us, p95_latency_us, p99_latency_us, p999_latency_us,
        total_queries, failed_queries, duration_secs, metadata
    ) VALUES (
        p_benchmark_name, p_pooler_type, p_num_clients, p_tps, p_avg_latency_us,
        p_p50_latency_us, p_p95_latency_us, p_p99_latency_us, p_p999_latency_us,
        p_total_queries, p_failed_queries, p_duration_secs, p_metadata
    ) RETURNING id INTO v_id;

    RETURN v_id;
END;
$$ LANGUAGE plpgsql;

-- Function to get pooler ranking
CREATE OR REPLACE FUNCTION get_pooler_ranking(p_num_clients INTEGER DEFAULT 100)
RETURNS TABLE (
    rank INTEGER,
    pooler_type VARCHAR(32),
    avg_tps DOUBLE PRECISION,
    avg_latency_us DOUBLE PRECISION,
    num_runs BIGINT
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        ROW_NUMBER() OVER (ORDER BY AVG(m.tps) DESC)::INTEGER AS rank,
        m.pooler_type,
        ROUND(AVG(m.tps)::numeric, 2)::DOUBLE PRECISION AS avg_tps,
        ROUND(AVG(m.avg_latency_us)::numeric, 2)::DOUBLE PRECISION AS avg_latency_us,
        COUNT(*)::BIGINT AS num_runs
    FROM connection_bench_metrics m
    WHERE m.num_clients = p_num_clients
    GROUP BY m.pooler_type
    ORDER BY avg_tps DESC;
END;
$$ LANGUAGE plpgsql;

-- Grant permissions
GRANT SELECT, INSERT ON connection_bench_simple TO PUBLIC;
GRANT SELECT, INSERT ON connection_bench_metrics TO PUBLIC;
GRANT USAGE, SELECT ON SEQUENCE connection_bench_simple_id_seq TO PUBLIC;
GRANT USAGE, SELECT ON SEQUENCE connection_bench_metrics_id_seq TO PUBLIC;
GRANT SELECT ON connection_bench_comparison TO PUBLIC;
GRANT EXECUTE ON FUNCTION bench_ping() TO PUBLIC;
GRANT EXECUTE ON FUNCTION bench_current_time() TO PUBLIC;
GRANT EXECUTE ON FUNCTION bench_random_int(INTEGER, INTEGER) TO PUBLIC;
GRANT EXECUTE ON FUNCTION store_bench_result(VARCHAR, VARCHAR, INTEGER, DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION, BIGINT, BIGINT, DOUBLE PRECISION, JSONB) TO PUBLIC;
GRANT EXECUTE ON FUNCTION get_pooler_ranking(INTEGER) TO PUBLIC;

-- Analyze tables for optimal query planning
ANALYZE connection_bench_simple;
ANALYZE connection_bench_metrics;

-- Print completion message
DO $$
BEGIN
    RAISE NOTICE 'Connection benchmark schema setup complete';
    RAISE NOTICE 'Tables: connection_bench_simple, connection_bench_metrics';
    RAISE NOTICE 'Functions: bench_ping(), bench_current_time(), bench_random_int()';
    RAISE NOTICE 'Views: connection_bench_comparison';
END $$;
