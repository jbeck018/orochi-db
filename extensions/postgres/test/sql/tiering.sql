-- Tiered storage tests
-- Test hot/warm/cold tier transitions

CREATE EXTENSION orochi;

-- Test: Create time-series table for tiering
CREATE TABLE sensor_data (
    time TIMESTAMPTZ NOT NULL,
    sensor_id INT,
    value DOUBLE PRECISION
);

SELECT create_hypertable(
    'sensor_data',
    'time',
    chunk_time_interval => INTERVAL '1 day'
);

-- Test: Insert historical data (will create multiple chunks)
INSERT INTO sensor_data (time, sensor_id, value)
SELECT
    NOW() - ((n * 12) || ' hours')::interval,
    (n % 5) + 1,
    random() * 100
FROM generate_series(0, 100) n;

-- Test: Set tiering policy
SELECT set_tiering_policy(
    'sensor_data',
    hot_after => INTERVAL '1 day',
    warm_after => INTERVAL '3 days',
    cold_after => INTERVAL '7 days'
);

-- Test: View chunk tiers
SELECT
    chunk_id,
    range_start,
    range_end,
    tier,
    is_compressed
FROM orochi.chunks
WHERE hypertable = 'sensor_data'
ORDER BY range_start;

-- Test: Query across tiers (should work transparently)
SELECT
    DATE_TRUNC('day', time) AS day,
    sensor_id,
    AVG(value) AS avg_value
FROM sensor_data
GROUP BY day, sensor_id
ORDER BY day DESC, sensor_id
LIMIT 10;

-- Test: Tier names
SELECT
    tier,
    COUNT(*) AS chunk_count
FROM orochi.chunks
WHERE hypertable = 'sensor_data'
GROUP BY tier;

-- Cleanup
DROP TABLE sensor_data;
DROP EXTENSION orochi CASCADE;
