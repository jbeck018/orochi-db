-- Distributed Benchmark Schema for Orochi DB
-- Tests cross-shard queries, local queries, and rebalancing

-- Drop existing tables
DROP TABLE IF EXISTS orders_distributed CASCADE;
DROP TABLE IF EXISTS customers_distributed CASCADE;
DROP TABLE IF EXISTS products_distributed CASCADE;
DROP TABLE IF EXISTS order_items_distributed CASCADE;

-- Customers table (distributed by customer_id)
CREATE TABLE customers_distributed (
    customer_id     SERIAL PRIMARY KEY,
    name            VARCHAR(100) NOT NULL,
    email           VARCHAR(100) NOT NULL,
    region          VARCHAR(32) NOT NULL,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Products table (reference table - replicated)
CREATE TABLE products_distributed (
    product_id      SERIAL PRIMARY KEY,
    name            VARCHAR(100) NOT NULL,
    category        VARCHAR(50) NOT NULL,
    price           DECIMAL(10,2) NOT NULL,
    inventory       INTEGER NOT NULL DEFAULT 0
);

-- Orders table (distributed by customer_id for co-location)
CREATE TABLE orders_distributed (
    order_id        SERIAL,
    customer_id     INTEGER NOT NULL,
    order_date      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    status          VARCHAR(20) NOT NULL DEFAULT 'pending',
    total_amount    DECIMAL(12,2) NOT NULL,
    PRIMARY KEY (customer_id, order_id)
);

-- Order items (distributed by customer_id for co-location)
CREATE TABLE order_items_distributed (
    item_id         SERIAL,
    order_id        INTEGER NOT NULL,
    customer_id     INTEGER NOT NULL,
    product_id      INTEGER NOT NULL,
    quantity        INTEGER NOT NULL,
    unit_price      DECIMAL(10,2) NOT NULL,
    PRIMARY KEY (customer_id, item_id)
);

-- Create indexes
CREATE INDEX idx_orders_customer ON orders_distributed(customer_id);
CREATE INDEX idx_orders_date ON orders_distributed(order_date);
CREATE INDEX idx_order_items_order ON order_items_distributed(customer_id, order_id);
CREATE INDEX idx_products_category ON products_distributed(category);

-- Convert to Orochi distributed tables
-- Uncomment when using Orochi distribution features

-- Distribute customers by customer_id
-- SELECT orochi_create_distributed_table('customers_distributed', 'customer_id', shard_count => 8);

-- Co-locate orders with customers
-- SELECT orochi_create_distributed_table('orders_distributed', 'customer_id',
--     colocate_with => 'customers_distributed');

-- Co-locate order_items with customers
-- SELECT orochi_create_distributed_table('order_items_distributed', 'customer_id',
--     colocate_with => 'customers_distributed');

-- Reference table (replicated to all nodes)
-- SELECT orochi_create_reference_table('products_distributed');

-- Populate products (reference data)
INSERT INTO products_distributed (name, category, price, inventory) VALUES
    ('Laptop Pro', 'Electronics', 1299.99, 100),
    ('Wireless Mouse', 'Electronics', 49.99, 500),
    ('USB-C Cable', 'Accessories', 19.99, 1000),
    ('Monitor 27"', 'Electronics', 399.99, 75),
    ('Keyboard', 'Electronics', 89.99, 200),
    ('Desk Lamp', 'Home', 59.99, 150),
    ('Office Chair', 'Furniture', 299.99, 50),
    ('Notebook', 'Office', 9.99, 2000),
    ('Pen Set', 'Office', 14.99, 1500),
    ('Desk Organizer', 'Office', 29.99, 300)
ON CONFLICT DO NOTHING;
