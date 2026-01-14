-- Hypertable (time-series) tests
-- Test automatic chunking, time functions, and policies

CREATE EXTENSION orochi;

-- Test: Create a time-series table
CREATE TABLE metrics (
    time TIMESTAMPTZ NOT NULL,
    device_id INT NOT NULL,
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION
);

-- Test: Convert to hypertable
SELECT create_hypertable(
    'metrics',
    'time',
    chunk_time_interval => INTERVAL '1 day'
);

-- Test: Verify hypertable registration
SELECT
    table_name,
    is_timeseries,
    time_column
FROM orochi.tables
WHERE table_name = 'metrics';

-- Test: Insert time-series data
INSERT INTO metrics (time, device_id, temperature, humidity)
SELECT
    NOW() - (n || ' hours')::interval,
    (n % 3) + 1,
    20 + random() * 10,
    40 + random() * 30
FROM generate_series(0, 72) n;

-- Test: Verify chunks created
SELECT
    chunk_id,
    range_start,
    range_end,
    is_compressed
FROM orochi.chunks
WHERE hypertable = 'metrics'
ORDER BY range_start;

-- Test: Time bucket function
SELECT
    time_bucket('1 hour', time) AS hour,
    device_id,
    AVG(temperature) AS avg_temp,
    AVG(humidity) AS avg_humidity
FROM metrics
WHERE time > NOW() - INTERVAL '1 day'
GROUP BY hour, device_id
ORDER BY hour, device_id
LIMIT 10;

-- Test: Time bucket with origin
SELECT
    time_bucket('6 hours', time, '2024-01-01 00:00:00'::timestamptz) AS bucket,
    COUNT(*) as readings
FROM metrics
GROUP BY bucket
ORDER BY bucket
LIMIT 5;

-- Test: Add dimension (space partitioning)
SELECT add_dimension('metrics', 'device_id', number_partitions => 2);

-- Test: Add compression policy
SELECT add_compression_policy('metrics', compress_after => INTERVAL '2 days');

-- Test: Add retention policy
SELECT add_retention_policy('metrics', drop_after => INTERVAL '30 days');

-- Test: Query with time range (chunk elimination)
EXPLAIN (COSTS OFF)
SELECT * FROM metrics
WHERE time > NOW() - INTERVAL '12 hours';

-- Test: Aggregate query
SELECT
    device_id,
    MIN(temperature) AS min_temp,
    MAX(temperature) AS max_temp,
    AVG(temperature) AS avg_temp,
    COUNT(*) AS readings
FROM metrics
GROUP BY device_id
ORDER BY device_id;

-- Test: Multi-dimensional query
SELECT
    time_bucket('1 day', time) AS day,
    device_id,
    COUNT(*) AS readings
FROM metrics
GROUP BY day, device_id
ORDER BY day, device_id;

-- Cleanup
DROP TABLE metrics;
DROP EXTENSION orochi CASCADE;
