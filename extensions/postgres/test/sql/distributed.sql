-- Distributed table tests
-- Test sharding, distribution, and query routing

CREATE EXTENSION orochi;

-- Test: Create a simple table
CREATE TABLE orders (
    id BIGSERIAL,
    customer_id INT NOT NULL,
    order_date TIMESTAMPTZ DEFAULT NOW(),
    total DECIMAL(10,2),
    status TEXT
);

-- Test: Distribute the table
SELECT create_distributed_table('orders', 'customer_id', shard_count => 4);

-- Test: Verify table is registered
SELECT
    table_name,
    is_distributed,
    shard_count,
    distribution_column
FROM orochi.tables
WHERE table_name = 'orders';

-- Test: Verify shards created
SELECT COUNT(*) AS shard_count FROM orochi.shards
WHERE table_oid = 'orders'::regclass::oid;

-- Test: Insert data (should route to correct shard)
INSERT INTO orders (customer_id, total, status) VALUES
    (1, 100.00, 'completed'),
    (2, 200.00, 'pending'),
    (3, 150.00, 'completed'),
    (1, 75.00, 'completed');

-- Test: Query with distribution key (single shard)
EXPLAIN (COSTS OFF) SELECT * FROM orders WHERE customer_id = 1;

-- Test: Query without distribution key (all shards)
EXPLAIN (COSTS OFF) SELECT * FROM orders WHERE status = 'completed';

-- Test: Aggregation
SELECT customer_id, COUNT(*), SUM(total)
FROM orders
GROUP BY customer_id
ORDER BY customer_id;

-- Test: Create reference table
CREATE TABLE order_status (
    code TEXT PRIMARY KEY,
    description TEXT
);

SELECT create_reference_table('order_status');

INSERT INTO order_status VALUES
    ('pending', 'Order is pending'),
    ('completed', 'Order is completed'),
    ('cancelled', 'Order was cancelled');

-- Test: Verify reference table
SELECT
    table_name,
    shard_strategy
FROM orochi.tables
WHERE table_name = 'order_status';

-- Test: Join distributed with reference (should be local)
SELECT o.id, o.total, s.description
FROM orders o
JOIN order_status s ON o.status = s.code
ORDER BY o.id;

-- Test: Co-located table
CREATE TABLE order_items (
    id BIGSERIAL,
    order_id BIGINT,
    customer_id INT NOT NULL,
    product_id INT,
    quantity INT,
    price DECIMAL(10,2)
);

SELECT create_distributed_table('order_items', 'customer_id', colocate_with => 'orders');

INSERT INTO order_items (order_id, customer_id, product_id, quantity, price) VALUES
    (1, 1, 101, 2, 50.00),
    (2, 2, 102, 1, 200.00),
    (3, 3, 103, 3, 50.00);

-- Test: Co-located join (should be local per shard)
SELECT o.id, o.total, SUM(oi.quantity * oi.price) as items_total
FROM orders o
JOIN order_items oi ON o.customer_id = oi.customer_id
GROUP BY o.id, o.total
ORDER BY o.id;

-- Cleanup
DROP TABLE order_items;
DROP TABLE orders;
DROP TABLE order_status;
DROP EXTENSION orochi CASCADE;
