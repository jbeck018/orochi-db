-- Vectorized Execution Benchmark Schema for Orochi DB
-- Tests SIMD operations and batch processing

-- Drop existing tables
DROP TABLE IF EXISTS vector_data CASCADE;
DROP TABLE IF EXISTS numeric_data CASCADE;

-- Table for vector operations benchmarks
CREATE TABLE vector_data (
    id          SERIAL PRIMARY KEY,
    vec_f32     REAL[] NOT NULL,          -- 32-bit float vector
    vec_f64     DOUBLE PRECISION[] NOT NULL, -- 64-bit float vector
    label       INTEGER NOT NULL
);

-- Table for numeric operation benchmarks
CREATE TABLE numeric_data (
    id          SERIAL PRIMARY KEY,
    int_col     INTEGER NOT NULL,
    bigint_col  BIGINT NOT NULL,
    float_col   REAL NOT NULL,
    double_col  DOUBLE PRECISION NOT NULL,
    decimal_col DECIMAL(15,4) NOT NULL
);

-- Create indexes
CREATE INDEX idx_vector_label ON vector_data(label);
CREATE INDEX idx_numeric_int ON numeric_data(int_col);

-- Populate numeric data for batch processing tests
INSERT INTO numeric_data (int_col, bigint_col, float_col, double_col, decimal_col)
SELECT
    (random() * 1000000)::int,
    (random() * 1000000000000)::bigint,
    random()::real * 1000,
    random() * 1000,
    (random() * 1000000)::decimal(15,4)
FROM generate_series(1, 1000000);

-- Populate vector data (128-dimensional vectors)
INSERT INTO vector_data (vec_f32, vec_f64, label)
SELECT
    (SELECT array_agg(random()::real) FROM generate_series(1, 128)),
    (SELECT array_agg(random()) FROM generate_series(1, 128)),
    (random() * 10)::int
FROM generate_series(1, 10000);

-- Analyze tables
ANALYZE vector_data;
ANALYZE numeric_data;
