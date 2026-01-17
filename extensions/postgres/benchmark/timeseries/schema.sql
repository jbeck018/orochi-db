-- Time-Series Schema for Orochi DB Benchmarks
-- Simulates IoT/sensor data and metrics collection

-- Drop existing tables
DROP TABLE IF EXISTS sensor_data CASCADE;
DROP TABLE IF EXISTS metrics CASCADE;
DROP TABLE IF EXISTS events CASCADE;
DROP TABLE IF EXISTS devices CASCADE;

-- Devices table (dimension table)
CREATE TABLE devices (
    device_id       SERIAL PRIMARY KEY,
    device_name     VARCHAR(64) NOT NULL,
    device_type     VARCHAR(32) NOT NULL,
    location        VARCHAR(128),
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- High-volume sensor data (main time-series table)
CREATE TABLE sensor_data (
    time            TIMESTAMPTZ NOT NULL,
    device_id       INTEGER NOT NULL,
    temperature     DOUBLE PRECISION,
    humidity        DOUBLE PRECISION,
    pressure        DOUBLE PRECISION,
    battery_level   DOUBLE PRECISION,
    signal_strength INTEGER
);

-- System metrics (like Prometheus metrics)
CREATE TABLE metrics (
    time            TIMESTAMPTZ NOT NULL,
    host            VARCHAR(64) NOT NULL,
    metric_name     VARCHAR(128) NOT NULL,
    metric_value    DOUBLE PRECISION NOT NULL,
    tags            JSONB
);

-- Event logs
CREATE TABLE events (
    time            TIMESTAMPTZ NOT NULL,
    device_id       INTEGER NOT NULL,
    event_type      VARCHAR(32) NOT NULL,
    severity        INTEGER NOT NULL,
    message         TEXT,
    metadata        JSONB
);

-- Create indexes for time-based queries
CREATE INDEX idx_sensor_data_time ON sensor_data(time DESC);
CREATE INDEX idx_sensor_data_device_time ON sensor_data(device_id, time DESC);
CREATE INDEX idx_metrics_time ON metrics(time DESC);
CREATE INDEX idx_metrics_host_time ON metrics(host, time DESC);
CREATE INDEX idx_events_time ON events(time DESC);

-- Convert to Orochi hypertables (time-series optimized)
-- Uncomment when using Orochi time-series features
-- SELECT orochi_create_hypertable('sensor_data', 'time', chunk_interval => INTERVAL '1 day');
-- SELECT orochi_create_hypertable('metrics', 'time', chunk_interval => INTERVAL '1 hour');
-- SELECT orochi_create_hypertable('events', 'time', chunk_interval => INTERVAL '1 day');

-- Create continuous aggregates for common rollups
-- Uncomment when using Orochi features
-- CREATE MATERIALIZED VIEW sensor_hourly AS
-- SELECT
--     time_bucket('1 hour', time) AS bucket,
--     device_id,
--     AVG(temperature) AS avg_temp,
--     MIN(temperature) AS min_temp,
--     MAX(temperature) AS max_temp,
--     AVG(humidity) AS avg_humidity,
--     COUNT(*) AS sample_count
-- FROM sensor_data
-- GROUP BY bucket, device_id
-- WITH (orochi_continuous_aggregate = true);

-- Insert sample devices
INSERT INTO devices (device_name, device_type, location) VALUES
    ('sensor-001', 'temperature', 'Building A - Floor 1'),
    ('sensor-002', 'temperature', 'Building A - Floor 2'),
    ('sensor-003', 'humidity', 'Building B - Floor 1'),
    ('sensor-004', 'pressure', 'Building B - Floor 2'),
    ('sensor-005', 'multi', 'Building C - Roof')
ON CONFLICT DO NOTHING;
