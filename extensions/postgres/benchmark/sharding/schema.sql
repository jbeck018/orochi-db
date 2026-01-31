-- Orochi DB - Sharding Benchmark Schema
--
-- Distributed schema definitions for benchmarking:
-- - Hash-distributed tables
-- - Range-distributed tables (time-series)
-- - Colocated table groups
--
-- Usage:
--   psql -d orochi_bench -f schema.sql
--

-- ============================================================
-- Drop existing tables
-- ============================================================

DROP TABLE IF EXISTS shard_bench_order_items CASCADE;
DROP TABLE IF EXISTS shard_bench_orders CASCADE;
DROP TABLE IF EXISTS shard_bench_customers CASCADE;
DROP TABLE IF EXISTS shard_bench_products CASCADE;
DROP TABLE IF EXISTS shard_bench_events CASCADE;
DROP TABLE IF EXISTS shard_bench_metrics CASCADE;
DROP TABLE IF EXISTS shard_bench_sessions CASCADE;

-- ============================================================
-- Hash-Distributed Tables
-- ============================================================

-- Customers table (primary entity, hash-distributed by customer_id)
CREATE TABLE shard_bench_customers (
    customer_id     BIGSERIAL PRIMARY KEY,
    name            VARCHAR(100) NOT NULL,
    email           VARCHAR(100) NOT NULL UNIQUE,
    region          VARCHAR(32) NOT NULL,
    tier            VARCHAR(20) DEFAULT 'standard',
    balance         DECIMAL(12,2) DEFAULT 0,
    metadata        JSONB DEFAULT '{}',
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Create partial index for active customers
CREATE INDEX idx_shard_customers_region ON shard_bench_customers(region);
CREATE INDEX idx_shard_customers_tier ON shard_bench_customers(tier);
CREATE INDEX idx_shard_customers_email ON shard_bench_customers(email);

COMMENT ON TABLE shard_bench_customers IS
    'Hash-distributed by customer_id. Base table for colocation group.';

-- Orders table (hash-distributed, colocated with customers)
CREATE TABLE shard_bench_orders (
    order_id        BIGSERIAL,
    customer_id     BIGINT NOT NULL,
    order_date      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    status          VARCHAR(20) NOT NULL DEFAULT 'pending',
    total_amount    DECIMAL(12,2) NOT NULL,
    currency        CHAR(3) DEFAULT 'USD',
    shipping_addr   TEXT,
    notes           TEXT,
    metadata        JSONB DEFAULT '{}',
    PRIMARY KEY (customer_id, order_id)
);

CREATE INDEX idx_shard_orders_date ON shard_bench_orders(order_date);
CREATE INDEX idx_shard_orders_status ON shard_bench_orders(status);
CREATE INDEX idx_shard_orders_amount ON shard_bench_orders(total_amount);

COMMENT ON TABLE shard_bench_orders IS
    'Hash-distributed by customer_id, colocated with customers for efficient joins.';

-- Order items (hash-distributed, colocated with orders and customers)
CREATE TABLE shard_bench_order_items (
    item_id         BIGSERIAL,
    order_id        BIGINT NOT NULL,
    customer_id     BIGINT NOT NULL,
    product_id      INTEGER NOT NULL,
    quantity        INTEGER NOT NULL DEFAULT 1,
    unit_price      DECIMAL(10,2) NOT NULL,
    discount        DECIMAL(5,2) DEFAULT 0,
    subtotal        DECIMAL(12,2) GENERATED ALWAYS AS (quantity * unit_price * (1 - discount/100)) STORED,
    PRIMARY KEY (customer_id, item_id)
);

CREATE INDEX idx_shard_items_order ON shard_bench_order_items(customer_id, order_id);
CREATE INDEX idx_shard_items_product ON shard_bench_order_items(product_id);

COMMENT ON TABLE shard_bench_order_items IS
    'Hash-distributed by customer_id, colocated with orders for three-way joins.';

-- Sessions table (hash-distributed by user_id for session management)
CREATE TABLE shard_bench_sessions (
    session_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         BIGINT NOT NULL,
    device_type     VARCHAR(50),
    ip_address      INET,
    started_at      TIMESTAMPTZ DEFAULT NOW(),
    last_active     TIMESTAMPTZ DEFAULT NOW(),
    expires_at      TIMESTAMPTZ DEFAULT NOW() + INTERVAL '24 hours',
    data            JSONB DEFAULT '{}'
);

CREATE INDEX idx_shard_sessions_user ON shard_bench_sessions(user_id);
CREATE INDEX idx_shard_sessions_expires ON shard_bench_sessions(expires_at);

COMMENT ON TABLE shard_bench_sessions IS
    'Hash-distributed by user_id for session lookups.';

-- ============================================================
-- Reference Tables (Replicated to all nodes)
-- ============================================================

-- Products table (reference table - replicated for joins)
CREATE TABLE shard_bench_products (
    product_id      SERIAL PRIMARY KEY,
    sku             VARCHAR(50) UNIQUE NOT NULL,
    name            VARCHAR(200) NOT NULL,
    category        VARCHAR(100) NOT NULL,
    subcategory     VARCHAR(100),
    brand           VARCHAR(100),
    price           DECIMAL(10,2) NOT NULL,
    cost            DECIMAL(10,2),
    weight_kg       DECIMAL(8,3),
    inventory       INTEGER NOT NULL DEFAULT 0,
    is_active       BOOLEAN DEFAULT true,
    attributes      JSONB DEFAULT '{}',
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_shard_products_category ON shard_bench_products(category);
CREATE INDEX idx_shard_products_brand ON shard_bench_products(brand);
CREATE INDEX idx_shard_products_price ON shard_bench_products(price);
CREATE INDEX idx_shard_products_attrs ON shard_bench_products USING GIN(attributes);

COMMENT ON TABLE shard_bench_products IS
    'Reference table - replicated to all shards for efficient joins.';

-- ============================================================
-- Range-Distributed Tables (Time-Series)
-- ============================================================

-- Events table (range-distributed by event_time for time-series analytics)
CREATE TABLE shard_bench_events (
    event_id        BIGSERIAL,
    event_time      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    event_type      VARCHAR(50) NOT NULL,
    user_id         BIGINT,
    session_id      UUID,
    entity_type     VARCHAR(50),
    entity_id       BIGINT,
    action          VARCHAR(100),
    properties      JSONB DEFAULT '{}',
    PRIMARY KEY (event_time, event_id)
);

CREATE INDEX idx_shard_events_type ON shard_bench_events(event_type);
CREATE INDEX idx_shard_events_user ON shard_bench_events(user_id);
CREATE INDEX idx_shard_events_entity ON shard_bench_events(entity_type, entity_id);
CREATE INDEX idx_shard_events_props ON shard_bench_events USING GIN(properties);

COMMENT ON TABLE shard_bench_events IS
    'Range-distributed by event_time. Partitioned into time-based chunks.';

-- Metrics table (range-distributed for time-series metrics)
CREATE TABLE shard_bench_metrics (
    metric_id       BIGSERIAL,
    metric_time     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    metric_name     VARCHAR(100) NOT NULL,
    dimensions      JSONB DEFAULT '{}',
    value           DOUBLE PRECISION NOT NULL,
    unit            VARCHAR(20),
    tags            TEXT[],
    PRIMARY KEY (metric_time, metric_id)
);

CREATE INDEX idx_shard_metrics_name ON shard_bench_metrics(metric_name);
CREATE INDEX idx_shard_metrics_dims ON shard_bench_metrics USING GIN(dimensions);
CREATE INDEX idx_shard_metrics_tags ON shard_bench_metrics USING GIN(tags);

COMMENT ON TABLE shard_bench_metrics IS
    'Range-distributed by metric_time. Optimized for time-series aggregations.';

-- ============================================================
-- Orochi Distribution Commands
-- ============================================================
-- Uncomment and run after creating the extension

/*
-- Create colocation group for customer-related tables
-- All tables distributed by customer_id will be colocated

-- Distribute customers (creates colocation group)
SELECT orochi_create_distributed_table(
    'shard_bench_customers'::regclass,
    'customer_id',
    shard_strategy => 0,  -- HASH
    shard_count => 8
);

-- Colocate orders with customers
SELECT orochi_create_distributed_table(
    'shard_bench_orders'::regclass,
    'customer_id',
    shard_strategy => 0,
    shard_count => 8,
    colocate_with => 'shard_bench_customers'
);

-- Colocate order_items with orders and customers
SELECT orochi_create_distributed_table(
    'shard_bench_order_items'::regclass,
    'customer_id',
    shard_strategy => 0,
    shard_count => 8,
    colocate_with => 'shard_bench_customers'
);

-- Distribute sessions by user_id (separate colocation group)
SELECT orochi_create_distributed_table(
    'shard_bench_sessions'::regclass,
    'user_id',
    shard_strategy => 0,
    shard_count => 8
);

-- Create reference table (replicated to all nodes)
SELECT orochi_create_reference_table('shard_bench_products'::regclass);

-- Range distribution for time-series tables
SELECT orochi_create_distributed_table(
    'shard_bench_events'::regclass,
    'event_time',
    shard_strategy => 1,  -- RANGE
    shard_count => 8
);

SELECT orochi_create_distributed_table(
    'shard_bench_metrics'::regclass,
    'metric_time',
    shard_strategy => 1,
    shard_count => 8
);
*/

-- ============================================================
-- Populate Reference Data
-- ============================================================

-- Products (reference data that will be replicated)
INSERT INTO shard_bench_products (sku, name, category, subcategory, brand, price, cost, weight_kg, inventory, attributes) VALUES
    ('ELEC-001', 'Laptop Pro 15', 'Electronics', 'Computers', 'TechCo', 1299.99, 950.00, 2.1, 100, '{"screen": "15.6 inch", "ram": "16GB", "storage": "512GB SSD"}'),
    ('ELEC-002', 'Laptop Air 13', 'Electronics', 'Computers', 'TechCo', 999.99, 700.00, 1.3, 150, '{"screen": "13.3 inch", "ram": "8GB", "storage": "256GB SSD"}'),
    ('ELEC-003', 'Desktop Workstation', 'Electronics', 'Computers', 'TechCo', 1899.99, 1400.00, 8.5, 50, '{"cpu": "i9", "ram": "32GB", "storage": "1TB SSD"}'),
    ('ELEC-004', 'Wireless Mouse', 'Electronics', 'Accessories', 'PeriphCo', 49.99, 25.00, 0.1, 500, '{"dpi": "3200", "buttons": 6}'),
    ('ELEC-005', 'Mechanical Keyboard', 'Electronics', 'Accessories', 'PeriphCo', 129.99, 70.00, 0.8, 200, '{"switches": "Cherry MX Blue", "backlight": true}'),
    ('ELEC-006', 'Monitor 27" 4K', 'Electronics', 'Displays', 'ViewTech', 399.99, 280.00, 5.2, 75, '{"resolution": "3840x2160", "panel": "IPS", "refresh": "60Hz"}'),
    ('ELEC-007', 'Monitor 32" Curved', 'Electronics', 'Displays', 'ViewTech', 549.99, 380.00, 7.8, 40, '{"resolution": "2560x1440", "panel": "VA", "refresh": "144Hz"}'),
    ('ELEC-008', 'USB-C Hub', 'Electronics', 'Accessories', 'ConnectPro', 79.99, 35.00, 0.15, 300, '{"ports": ["HDMI", "USB-A x3", "SD card"]}'),
    ('ELEC-009', 'Webcam HD', 'Electronics', 'Accessories', 'StreamCo', 89.99, 45.00, 0.2, 250, '{"resolution": "1080p", "fps": 30}'),
    ('ELEC-010', 'Headphones Pro', 'Electronics', 'Audio', 'AudioMax', 299.99, 180.00, 0.3, 120, '{"driver": "40mm", "anc": true, "wireless": true}'),
    ('HOME-001', 'Desk Lamp LED', 'Home', 'Lighting', 'BrightHome', 59.99, 25.00, 0.8, 150, '{"lumens": 800, "color_temp": "adjustable"}'),
    ('HOME-002', 'Office Chair Ergo', 'Home', 'Furniture', 'ComfortPlus', 299.99, 180.00, 15.0, 50, '{"adjustable": true, "lumbar": true}'),
    ('HOME-003', 'Standing Desk', 'Home', 'Furniture', 'ComfortPlus', 499.99, 300.00, 35.0, 30, '{"height_range": "70-120cm", "motorized": true}'),
    ('OFFC-001', 'Notebook Set', 'Office', 'Supplies', 'WriteCo', 14.99, 5.00, 0.5, 2000, '{"pages": 200, "ruled": true}'),
    ('OFFC-002', 'Pen Set Premium', 'Office', 'Supplies', 'WriteCo', 24.99, 8.00, 0.1, 1500, '{"count": 12, "ink": "gel"}'),
    ('OFFC-003', 'Desk Organizer', 'Office', 'Supplies', 'OrganizerPro', 34.99, 15.00, 0.6, 300, '{"compartments": 6, "material": "bamboo"}'),
    ('OFFC-004', 'Whiteboard 4x3', 'Office', 'Equipment', 'BoardMaster', 89.99, 45.00, 4.0, 80, '{"magnetic": true, "size": "4x3 ft"}'),
    ('OFFC-005', 'Paper Shredder', 'Office', 'Equipment', 'SecureCut', 149.99, 80.00, 6.0, 60, '{"sheets": 10, "cross_cut": true}'),
    ('SOFT-001', 'Antivirus Pro', 'Software', 'Security', 'SecureSoft', 49.99, 5.00, NULL, 9999, '{"devices": 3, "years": 1}'),
    ('SOFT-002', 'Office Suite', 'Software', 'Productivity', 'DocuPro', 99.99, 10.00, NULL, 9999, '{"apps": ["word", "excel", "ppt"]}')
ON CONFLICT (sku) DO NOTHING;

-- ============================================================
-- Sample Data Generation Functions
-- ============================================================

-- Function to generate sample customers
CREATE OR REPLACE FUNCTION generate_shard_bench_customers(num_customers INTEGER)
RETURNS void AS $$
BEGIN
    INSERT INTO shard_bench_customers (name, email, region, tier, balance)
    SELECT
        'Customer ' || g,
        'customer' || g || '@example.com',
        (ARRAY['us-east','us-west','eu-west','eu-east','ap-south','ap-north'])[1 + (random() * 5)::int],
        (ARRAY['standard','premium','enterprise'])[1 + (random() * 2)::int],
        (random() * 10000)::decimal(12,2)
    FROM generate_series(1, num_customers) g;
END;
$$ LANGUAGE plpgsql;

-- Function to generate sample orders
CREATE OR REPLACE FUNCTION generate_shard_bench_orders(orders_per_customer INTEGER)
RETURNS void AS $$
BEGIN
    INSERT INTO shard_bench_orders (customer_id, order_date, status, total_amount, currency)
    SELECT
        c.customer_id,
        NOW() - (random() * INTERVAL '365 days'),
        (ARRAY['pending','processing','shipped','delivered','cancelled'])[1 + (random() * 4)::int],
        (random() * 1000 + 10)::decimal(12,2),
        (ARRAY['USD','EUR','GBP'])[1 + (random() * 2)::int]
    FROM shard_bench_customers c,
         generate_series(1, orders_per_customer);
END;
$$ LANGUAGE plpgsql;

-- Function to generate sample order items
CREATE OR REPLACE FUNCTION generate_shard_bench_order_items()
RETURNS void AS $$
BEGIN
    INSERT INTO shard_bench_order_items (order_id, customer_id, product_id, quantity, unit_price, discount)
    SELECT
        o.order_id,
        o.customer_id,
        p.product_id,
        (random() * 4 + 1)::int,
        p.price,
        (random() * 20)::decimal(5,2)
    FROM shard_bench_orders o
    CROSS JOIN LATERAL (
        SELECT product_id, price
        FROM shard_bench_products
        WHERE is_active = true
        ORDER BY random()
        LIMIT (random() * 3 + 1)::int
    ) p;
END;
$$ LANGUAGE plpgsql;

-- Function to generate sample events
CREATE OR REPLACE FUNCTION generate_shard_bench_events(num_events INTEGER)
RETURNS void AS $$
DECLARE
    max_user_id BIGINT;
BEGIN
    SELECT COALESCE(MAX(customer_id), 1000) INTO max_user_id FROM shard_bench_customers;

    INSERT INTO shard_bench_events (event_time, event_type, user_id, entity_type, entity_id, action, properties)
    SELECT
        NOW() - (random() * INTERVAL '90 days'),
        (ARRAY['page_view','click','purchase','signup','logout','search','add_to_cart'])[1 + (random() * 6)::int],
        (random() * max_user_id)::bigint,
        (ARRAY['product','order','user','page'])[1 + (random() * 3)::int],
        (random() * 10000)::bigint,
        'action_' || (random() * 100)::int,
        jsonb_build_object(
            'source', (ARRAY['web','mobile','api'])[1 + (random() * 2)::int],
            'duration_ms', (random() * 5000)::int,
            'success', random() > 0.1
        )
    FROM generate_series(1, num_events);
END;
$$ LANGUAGE plpgsql;

-- Function to generate sample metrics
CREATE OR REPLACE FUNCTION generate_shard_bench_metrics(num_metrics INTEGER)
RETURNS void AS $$
BEGIN
    INSERT INTO shard_bench_metrics (metric_time, metric_name, dimensions, value, unit, tags)
    SELECT
        NOW() - (random() * INTERVAL '30 days'),
        (ARRAY['cpu_usage','memory_usage','disk_io','network_in','network_out','request_latency','error_rate'])[1 + (random() * 6)::int],
        jsonb_build_object(
            'host', 'server-' || (random() * 10 + 1)::int,
            'region', (ARRAY['us-east','us-west','eu-west'])[1 + (random() * 2)::int],
            'service', (ARRAY['api','web','worker','db'])[1 + (random() * 3)::int]
        ),
        random() * 100,
        (ARRAY['percent','bytes','ms','count'])[1 + (random() * 3)::int],
        ARRAY['production', (ARRAY['critical','warning','info'])[1 + (random() * 2)::int]]
    FROM generate_series(1, num_metrics);
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Quick setup function for benchmarking
-- ============================================================

CREATE OR REPLACE FUNCTION setup_shard_benchmark(
    num_customers INTEGER DEFAULT 10000,
    orders_per_customer INTEGER DEFAULT 10,
    num_events INTEGER DEFAULT 50000,
    num_metrics INTEGER DEFAULT 100000
)
RETURNS void AS $$
BEGIN
    RAISE NOTICE 'Generating % customers...', num_customers;
    PERFORM generate_shard_bench_customers(num_customers);

    RAISE NOTICE 'Generating % orders per customer...', orders_per_customer;
    PERFORM generate_shard_bench_orders(orders_per_customer);

    RAISE NOTICE 'Generating order items...';
    PERFORM generate_shard_bench_order_items();

    RAISE NOTICE 'Generating % events...', num_events;
    PERFORM generate_shard_bench_events(num_events);

    RAISE NOTICE 'Generating % metrics...', num_metrics;
    PERFORM generate_shard_bench_metrics(num_metrics);

    RAISE NOTICE 'Analyzing tables...';
    ANALYZE shard_bench_customers;
    ANALYZE shard_bench_orders;
    ANALYZE shard_bench_order_items;
    ANALYZE shard_bench_events;
    ANALYZE shard_bench_metrics;

    RAISE NOTICE 'Setup complete!';
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Usage Examples
-- ============================================================

-- To set up the benchmark with default settings:
--   SELECT setup_shard_benchmark();
--
-- To set up with custom scale:
--   SELECT setup_shard_benchmark(50000, 20, 200000, 500000);
--
-- To verify distribution after distributing tables:
--   SELECT * FROM orochi.orochi_shard_stats;
--
-- Example queries for benchmarking:
--
-- Local query (single shard):
--   SELECT * FROM shard_bench_orders WHERE customer_id = 1234;
--
-- Colocated join (same shard):
--   SELECT c.name, o.order_id, o.total_amount
--   FROM shard_bench_customers c
--   JOIN shard_bench_orders o ON c.customer_id = o.customer_id
--   WHERE c.customer_id = 1234;
--
-- Cross-shard aggregation:
--   SELECT region, COUNT(*), SUM(balance) FROM shard_bench_customers GROUP BY region;
--
-- Reference table join:
--   SELECT p.category, SUM(oi.subtotal)
--   FROM shard_bench_order_items oi
--   JOIN shard_bench_products p ON oi.product_id = p.product_id
--   GROUP BY p.category;
--
-- Time-series query:
--   SELECT date_trunc('hour', event_time) as hour, event_type, COUNT(*)
--   FROM shard_bench_events
--   WHERE event_time > NOW() - INTERVAL '24 hours'
--   GROUP BY hour, event_type
--   ORDER BY hour;
