-- Columnar Storage Benchmark Schema for Orochi DB
-- Tests compression ratios and scan performance

-- Drop existing tables
DROP TABLE IF EXISTS columnar_integers CASCADE;
DROP TABLE IF EXISTS columnar_floats CASCADE;
DROP TABLE IF EXISTS columnar_timestamps CASCADE;
DROP TABLE IF EXISTS columnar_strings CASCADE;
DROP TABLE IF EXISTS columnar_mixed CASCADE;
DROP TABLE IF EXISTS row_store_baseline CASCADE;

-- Table with integer data (tests delta encoding)
CREATE TABLE columnar_integers (
    id              BIGSERIAL,
    monotonic_seq   BIGINT NOT NULL,      -- monotonically increasing (good for delta)
    random_int      INTEGER NOT NULL,      -- random integers
    small_range     SMALLINT NOT NULL,     -- small range values (good for RLE)
    boolean_col     BOOLEAN NOT NULL,      -- boolean (good for RLE)
    nullable_int    INTEGER                -- nullable column
);

-- Table with floating point data (tests Gorilla encoding)
CREATE TABLE columnar_floats (
    id              BIGSERIAL,
    timestamp_col   TIMESTAMPTZ NOT NULL,
    price           DOUBLE PRECISION NOT NULL,
    temperature     REAL NOT NULL,
    percentage      DOUBLE PRECISION NOT NULL,
    nullable_float  DOUBLE PRECISION
);

-- Table with timestamp data (tests delta encoding)
CREATE TABLE columnar_timestamps (
    id              BIGSERIAL,
    event_time      TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMP NOT NULL,
    date_only       DATE NOT NULL,
    time_only       TIME NOT NULL
);

-- Table with string data (tests dictionary encoding)
CREATE TABLE columnar_strings (
    id              BIGSERIAL,
    category        VARCHAR(32) NOT NULL,   -- low cardinality (good for dictionary)
    status          CHAR(10) NOT NULL,      -- low cardinality
    description     TEXT NOT NULL,          -- high cardinality
    uuid_col        UUID NOT NULL,          -- unique values
    nullable_str    VARCHAR(64)
);

-- Mixed data types table (realistic scenario)
CREATE TABLE columnar_mixed (
    id              BIGSERIAL PRIMARY KEY,
    event_time      TIMESTAMPTZ NOT NULL,
    device_id       INTEGER NOT NULL,
    metric_name     VARCHAR(64) NOT NULL,
    metric_value    DOUBLE PRECISION NOT NULL,
    tags            JSONB,
    raw_data        BYTEA
);

-- Row store baseline for comparison
CREATE TABLE row_store_baseline (
    id              BIGSERIAL PRIMARY KEY,
    event_time      TIMESTAMPTZ NOT NULL,
    device_id       INTEGER NOT NULL,
    metric_name     VARCHAR(64) NOT NULL,
    metric_value    DOUBLE PRECISION NOT NULL,
    tags            JSONB,
    raw_data        BYTEA
);

-- Create indexes for filter tests
CREATE INDEX idx_columnar_mixed_time ON columnar_mixed(event_time);
CREATE INDEX idx_columnar_mixed_device ON columnar_mixed(device_id);
CREATE INDEX idx_row_baseline_time ON row_store_baseline(event_time);
CREATE INDEX idx_row_baseline_device ON row_store_baseline(device_id);

-- Convert to Orochi columnar storage
-- Uncomment when using Orochi columnar features
-- SELECT orochi_convert_to_columnar('columnar_integers', compression => 'lz4');
-- SELECT orochi_convert_to_columnar('columnar_floats', compression => 'gorilla');
-- SELECT orochi_convert_to_columnar('columnar_timestamps', compression => 'delta');
-- SELECT orochi_convert_to_columnar('columnar_strings', compression => 'dictionary');
-- SELECT orochi_convert_to_columnar('columnar_mixed', compression => 'zstd');
