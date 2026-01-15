-- Columnar storage tests
-- Test columnar tables, compression, and analytical queries

CREATE EXTENSION orochi;

-- Test: Create analytical table (will use columnar storage)
CREATE TABLE sales_facts (
    sale_id BIGSERIAL,
    sale_date DATE NOT NULL,
    store_id INT NOT NULL,
    product_id INT NOT NULL,
    quantity INT NOT NULL,
    unit_price DECIMAL(10,2),
    total_amount DECIMAL(12,2)
);

-- Convert to hypertable (columnar will be applied to compressed chunks)
SELECT create_hypertable(
    'sales_facts',
    'sale_date',
    chunk_time_interval => INTERVAL '1 month'
);

-- Insert test data
INSERT INTO sales_facts (sale_date, store_id, product_id, quantity, unit_price, total_amount)
SELECT
    DATE '2023-01-01' + (n % 365),
    (n % 10) + 1,
    (n % 100) + 1,
    (n % 10) + 1,
    10.00 + (n % 90),
    (((n % 10) + 1) * (10.00 + (n % 90)))
FROM generate_series(1, 10000) n;

-- Test: Enable compression on hypertable
SELECT add_compression_policy('sales_facts', compress_after => INTERVAL '1 month');

-- Test: Analytical query (benefits from columnar)
SELECT
    store_id,
    COUNT(*) AS transaction_count,
    SUM(quantity) AS total_quantity,
    SUM(total_amount) AS total_sales,
    AVG(unit_price) AS avg_price
FROM sales_facts
WHERE sale_date >= DATE '2023-01-01' AND sale_date < DATE '2023-07-01'
GROUP BY store_id
ORDER BY total_sales DESC;

-- Test: Monthly aggregation
SELECT
    DATE_TRUNC('month', sale_date) AS month,
    SUM(total_amount) AS monthly_sales,
    COUNT(DISTINCT store_id) AS active_stores,
    COUNT(DISTINCT product_id) AS products_sold
FROM sales_facts
GROUP BY month
ORDER BY month;

-- Test: Window function (analytical)
SELECT
    store_id,
    sale_date,
    total_amount,
    SUM(total_amount) OVER (
        PARTITION BY store_id
        ORDER BY sale_date
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    ) AS running_total
FROM sales_facts
WHERE store_id = 1
ORDER BY sale_date
LIMIT 10;

-- Test: Compression status
SELECT
    chunk_id,
    is_compressed,
    tier
FROM orochi.chunks
WHERE hypertable = 'sales_facts'
ORDER BY range_start;

-- Cleanup
DROP TABLE sales_facts;
DROP EXTENSION orochi CASCADE;
