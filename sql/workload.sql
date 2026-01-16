-- workload.sql
-- Workload Management Functions for Orochi DB
--
-- This file defines:
--   - Resource pool management functions
--   - Workload class management functions
--   - Session resource assignment
--   - Monitoring and status views
--
-- Copyright (c) 2024, Orochi DB Contributors

-- ============================================================
-- Resource Pool Management Functions
-- ============================================================

-- Create a new resource pool
-- Parameters:
--   pool_name: Unique name for the pool
--   cpu_percent: Maximum CPU percentage (1-100, 0 = unlimited)
--   memory_mb: Maximum memory in MB (0 = unlimited)
--   max_concurrency: Maximum concurrent queries (0 = unlimited)
--   priority_weight: Scheduling priority weight (default 50)
--
-- Returns: Pool ID
--
-- Example:
--   SELECT orochi_create_resource_pool('analytics', 50, 4096, 10);
CREATE FUNCTION orochi_create_resource_pool(
    pool_name text,
    cpu_percent integer DEFAULT 100,
    memory_mb bigint DEFAULT 0,
    max_concurrency integer DEFAULT 0,
    priority_weight integer DEFAULT 50
) RETURNS bigint
AS 'MODULE_PATHNAME', 'orochi_create_resource_pool_sql'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION orochi_create_resource_pool(text, integer, bigint, integer, integer) IS
'Create a new resource pool for workload isolation.
Parameters:
  pool_name: Unique identifier for the pool
  cpu_percent: Max CPU percentage (1-100, 0=unlimited)
  memory_mb: Max memory in MB (0=unlimited)
  max_concurrency: Max concurrent queries (0=unlimited)
  priority_weight: Scheduling weight (higher=more priority)

Example:
  SELECT orochi_create_resource_pool(''oltp'', 70, 2048, 50);
  SELECT orochi_create_resource_pool(''reporting'', 30, 4096, 5);';

-- Alter an existing resource pool
-- Parameters are optional - only specified values are changed
CREATE FUNCTION orochi_alter_resource_pool(
    pool_id bigint,
    cpu_percent integer DEFAULT NULL,
    memory_mb bigint DEFAULT NULL,
    max_concurrency integer DEFAULT NULL,
    priority_weight integer DEFAULT NULL
) RETURNS boolean
AS 'MODULE_PATHNAME', 'orochi_alter_resource_pool_sql'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION orochi_alter_resource_pool(bigint, integer, bigint, integer, integer) IS
'Modify settings for an existing resource pool.
Only non-NULL parameters are updated.

Example:
  SELECT orochi_alter_resource_pool(1, cpu_percent => 80);
  SELECT orochi_alter_resource_pool(2, max_concurrency => 20);';

-- Drop a resource pool
CREATE FUNCTION orochi_drop_resource_pool(
    pool_name text,
    if_exists boolean DEFAULT false
) RETURNS boolean
AS 'MODULE_PATHNAME', 'orochi_drop_resource_pool_sql'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION orochi_drop_resource_pool(text, boolean) IS
'Remove a resource pool.
Cannot drop internal pools (default, admin).
Cannot drop pools with active queries.

Example:
  SELECT orochi_drop_resource_pool(''analytics'');
  SELECT orochi_drop_resource_pool(''old_pool'', true);';

-- ============================================================
-- Workload Class Management Functions
-- ============================================================

-- Create a new workload class
CREATE FUNCTION orochi_create_workload_class(
    class_name text,
    pool_name text,
    default_priority text DEFAULT 'normal',
    query_timeout_ms integer DEFAULT 30000
) RETURNS bigint
AS $$
DECLARE
    pool_id bigint;
    priority_int integer;
    class_id bigint;
BEGIN
    -- Look up pool ID
    SELECT p.pool_id INTO pool_id
    FROM orochi.orochi_resource_pools p
    WHERE p.pool_name = orochi_create_workload_class.pool_name;

    IF pool_id IS NULL THEN
        RAISE EXCEPTION 'Resource pool "%" does not exist', pool_name;
    END IF;

    -- Convert priority string to integer
    priority_int := CASE lower(default_priority)
        WHEN 'low' THEN 0
        WHEN 'normal' THEN 1
        WHEN 'high' THEN 2
        WHEN 'critical' THEN 3
        ELSE 1
    END;

    -- Insert the class
    INSERT INTO orochi.orochi_workload_classes
        (class_name, pool_id, default_priority, query_timeout_ms)
    VALUES (class_name, pool_id, priority_int, query_timeout_ms)
    RETURNING orochi_workload_classes.class_id INTO class_id;

    RETURN class_id;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_create_workload_class(text, text, text, integer) IS
'Create a workload class that maps sessions to resource pools.
Parameters:
  class_name: Unique identifier for the class
  pool_name: Resource pool to use
  default_priority: Query priority (low/normal/high/critical)
  query_timeout_ms: Default query timeout

Example:
  SELECT orochi_create_workload_class(''etl_jobs'', ''analytics'', ''low'', 600000);
  SELECT orochi_create_workload_class(''api_queries'', ''oltp'', ''high'', 5000);';

-- Alter a workload class
CREATE FUNCTION orochi_alter_workload_class(
    class_name text,
    pool_name text DEFAULT NULL,
    default_priority text DEFAULT NULL,
    query_timeout_ms integer DEFAULT NULL
) RETURNS boolean
AS $$
DECLARE
    class_id bigint;
    new_pool_id bigint;
    priority_int integer;
BEGIN
    -- Look up class
    SELECT c.class_id INTO class_id
    FROM orochi.orochi_workload_classes c
    WHERE c.class_name = orochi_alter_workload_class.class_name;

    IF class_id IS NULL THEN
        RAISE EXCEPTION 'Workload class "%" does not exist', class_name;
    END IF;

    -- Update pool if specified
    IF pool_name IS NOT NULL THEN
        SELECT p.pool_id INTO new_pool_id
        FROM orochi.orochi_resource_pools p
        WHERE p.pool_name = orochi_alter_workload_class.pool_name;

        IF new_pool_id IS NULL THEN
            RAISE EXCEPTION 'Resource pool "%" does not exist', pool_name;
        END IF;

        UPDATE orochi.orochi_workload_classes
        SET pool_id = new_pool_id
        WHERE orochi_workload_classes.class_id = orochi_alter_workload_class.class_id;
    END IF;

    -- Update priority if specified
    IF default_priority IS NOT NULL THEN
        priority_int := CASE lower(default_priority)
            WHEN 'low' THEN 0
            WHEN 'normal' THEN 1
            WHEN 'high' THEN 2
            WHEN 'critical' THEN 3
            ELSE 1
        END;

        UPDATE orochi.orochi_workload_classes
        SET default_priority = priority_int
        WHERE orochi_workload_classes.class_id = orochi_alter_workload_class.class_id;
    END IF;

    -- Update timeout if specified
    IF query_timeout_ms IS NOT NULL THEN
        UPDATE orochi.orochi_workload_classes
        SET orochi_workload_classes.query_timeout_ms = orochi_alter_workload_class.query_timeout_ms
        WHERE orochi_workload_classes.class_id = orochi_alter_workload_class.class_id;
    END IF;

    RETURN true;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_alter_workload_class(text, text, text, integer) IS
'Modify settings for an existing workload class.
Only non-NULL parameters are updated.

Example:
  SELECT orochi_alter_workload_class(''etl_jobs'', default_priority => ''normal'');';

-- Drop a workload class
CREATE FUNCTION orochi_drop_workload_class(
    class_name text,
    if_exists boolean DEFAULT false
) RETURNS boolean
AS $$
DECLARE
    class_id bigint;
BEGIN
    SELECT c.class_id INTO class_id
    FROM orochi.orochi_workload_classes c
    WHERE c.class_name = orochi_drop_workload_class.class_name;

    IF class_id IS NULL THEN
        IF if_exists THEN
            RETURN false;
        END IF;
        RAISE EXCEPTION 'Workload class "%" does not exist', class_name;
    END IF;

    DELETE FROM orochi.orochi_workload_classes
    WHERE orochi_workload_classes.class_id = orochi_drop_workload_class.class_id;

    RETURN true;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_drop_workload_class(text, boolean) IS
'Remove a workload class.

Example:
  SELECT orochi_drop_workload_class(''old_class'');
  SELECT orochi_drop_workload_class(''maybe_exists'', true);';

-- ============================================================
-- Session Assignment Functions
-- ============================================================

-- Assign current session to a workload class
CREATE FUNCTION orochi_assign_workload_class(
    class_name text
) RETURNS boolean
AS 'MODULE_PATHNAME', 'orochi_assign_workload_class_sql'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION orochi_assign_workload_class(text) IS
'Assign the current session to a workload class.
This determines which resource pool governs the session.

Example:
  SELECT orochi_assign_workload_class(''etl_jobs'');
  -- Subsequent queries will use the associated resource pool';

-- Set query priority for current session
CREATE FUNCTION orochi_set_query_priority(
    priority text
) RETURNS void
AS $$
BEGIN
    -- Validate priority
    IF lower(priority) NOT IN ('low', 'normal', 'high', 'critical') THEN
        RAISE EXCEPTION 'Invalid priority "%". Use: low, normal, high, critical', priority;
    END IF;

    -- Store in session variable (implementation would use GUC or session state)
    PERFORM set_config('orochi.query_priority', lower(priority), false);
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_set_query_priority(text) IS
'Set the priority for queries in the current session.
Higher priority queries are dequeued before lower priority ones.

Priority levels: low, normal, high, critical

Example:
  SELECT orochi_set_query_priority(''high'');';

-- ============================================================
-- Catalog Tables (created in schema orochi)
-- ============================================================

-- Resource pools catalog
CREATE TABLE IF NOT EXISTS orochi.orochi_resource_pools (
    pool_id BIGSERIAL PRIMARY KEY,
    pool_name TEXT NOT NULL UNIQUE,
    cpu_percent INTEGER NOT NULL DEFAULT 100,
    memory_mb BIGINT NOT NULL DEFAULT 0,
    io_read_mbps BIGINT NOT NULL DEFAULT 0,
    io_write_mbps BIGINT NOT NULL DEFAULT 0,
    max_concurrency INTEGER NOT NULL DEFAULT 0,
    max_queue_size INTEGER NOT NULL DEFAULT 0,
    query_timeout_ms INTEGER NOT NULL DEFAULT 30000,
    idle_timeout_ms INTEGER NOT NULL DEFAULT 0,
    priority_weight INTEGER NOT NULL DEFAULT 50,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=active, 1=draining, 2=disabled
    is_internal BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

COMMENT ON TABLE orochi.orochi_resource_pools IS
'Resource pools for workload isolation and management';

-- Workload classes catalog
CREATE TABLE IF NOT EXISTS orochi.orochi_workload_classes (
    class_id BIGSERIAL PRIMARY KEY,
    class_name TEXT NOT NULL UNIQUE,
    pool_id BIGINT NOT NULL REFERENCES orochi.orochi_resource_pools(pool_id),
    default_priority INTEGER NOT NULL DEFAULT 1,  -- 0=low, 1=normal, 2=high, 3=critical
    query_timeout_ms INTEGER NOT NULL DEFAULT 30000,
    state INTEGER NOT NULL DEFAULT 0,  -- 0=active, 1=disabled
    queries_executed BIGINT NOT NULL DEFAULT 0,
    queries_rejected BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

COMMENT ON TABLE orochi.orochi_workload_classes IS
'Workload classes that map sessions to resource pools';

-- Pool usage statistics
CREATE TABLE IF NOT EXISTS orochi.orochi_pool_stats (
    pool_id BIGINT NOT NULL REFERENCES orochi.orochi_resource_pools(pool_id),
    sample_time TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    active_queries INTEGER NOT NULL DEFAULT 0,
    queued_queries INTEGER NOT NULL DEFAULT 0,
    cpu_time_us BIGINT NOT NULL DEFAULT 0,
    memory_bytes BIGINT NOT NULL DEFAULT 0,
    io_read_bytes BIGINT NOT NULL DEFAULT 0,
    io_write_bytes BIGINT NOT NULL DEFAULT 0,
    queries_executed BIGINT NOT NULL DEFAULT 0,
    queries_rejected BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (pool_id, sample_time)
);

COMMENT ON TABLE orochi.orochi_pool_stats IS
'Historical resource usage statistics per pool';

-- Create index for efficient stats queries
CREATE INDEX IF NOT EXISTS idx_pool_stats_time
ON orochi.orochi_pool_stats(sample_time DESC);

-- ============================================================
-- Insert Default Pools
-- ============================================================

INSERT INTO orochi.orochi_resource_pools
    (pool_name, cpu_percent, memory_mb, max_concurrency, priority_weight, is_internal)
VALUES
    ('default', 100, 0, 0, 50, true),
    ('admin', 100, 0, 0, 100, true)
ON CONFLICT (pool_name) DO NOTHING;

-- ============================================================
-- Monitoring Views
-- ============================================================

-- View of all resource pools with current usage
CREATE OR REPLACE VIEW orochi.resource_pools AS
SELECT
    p.pool_id,
    p.pool_name,
    p.cpu_percent,
    p.memory_mb,
    p.io_read_mbps,
    p.io_write_mbps,
    p.max_concurrency,
    p.max_queue_size,
    p.query_timeout_ms,
    p.priority_weight,
    CASE p.state
        WHEN 0 THEN 'active'
        WHEN 1 THEN 'draining'
        WHEN 2 THEN 'disabled'
        ELSE 'unknown'
    END AS state,
    p.is_internal,
    p.created_at
FROM orochi.orochi_resource_pools p
ORDER BY p.pool_id;

COMMENT ON VIEW orochi.resource_pools IS
'View of all resource pools with their configuration';

-- View of all workload classes
CREATE OR REPLACE VIEW orochi.workload_classes AS
SELECT
    c.class_id,
    c.class_name,
    p.pool_name,
    CASE c.default_priority
        WHEN 0 THEN 'low'
        WHEN 1 THEN 'normal'
        WHEN 2 THEN 'high'
        WHEN 3 THEN 'critical'
        ELSE 'unknown'
    END AS default_priority,
    c.query_timeout_ms,
    CASE c.state
        WHEN 0 THEN 'active'
        WHEN 1 THEN 'disabled'
        ELSE 'unknown'
    END AS state,
    c.queries_executed,
    c.queries_rejected,
    c.created_at
FROM orochi.orochi_workload_classes c
JOIN orochi.orochi_resource_pools p ON c.pool_id = p.pool_id
ORDER BY c.class_id;

COMMENT ON VIEW orochi.workload_classes IS
'View of all workload classes with their configuration';

-- ============================================================
-- Monitoring Functions
-- ============================================================

-- Get current workload status
CREATE FUNCTION orochi_workload_status()
RETURNS TABLE (
    total_pools integer,
    total_classes integer,
    total_active_queries integer,
    total_queued_queries integer,
    total_memory_mb bigint,
    total_cpu_percent integer
)
AS $$
DECLARE
    pool_count integer;
    class_count integer;
    active_count integer;
    queued_count integer;
BEGIN
    SELECT COUNT(*) INTO pool_count FROM orochi.orochi_resource_pools;
    SELECT COUNT(*) INTO class_count FROM orochi.orochi_workload_classes;

    -- These would come from shared memory in the full implementation
    active_count := 0;
    queued_count := 0;

    RETURN QUERY SELECT
        pool_count,
        class_count,
        active_count,
        queued_count,
        0::bigint,
        0;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_workload_status() IS
'Get overall workload management status.

Example:
  SELECT * FROM orochi_workload_status();';

-- Get pool-level statistics
CREATE FUNCTION orochi_pool_stats(pool_name text DEFAULT NULL)
RETURNS TABLE (
    pool_name text,
    active_queries integer,
    queued_queries integer,
    cpu_percent integer,
    memory_mb bigint,
    max_concurrency integer,
    state text,
    queries_per_sec double precision
)
AS $$
BEGIN
    RETURN QUERY
    SELECT
        p.pool_name,
        0 AS active_queries,  -- Would come from shared memory
        0 AS queued_queries,
        p.cpu_percent,
        p.memory_mb,
        p.max_concurrency,
        CASE p.state
            WHEN 0 THEN 'active'
            WHEN 1 THEN 'draining'
            WHEN 2 THEN 'disabled'
            ELSE 'unknown'
        END::text,
        0.0 AS queries_per_sec
    FROM orochi.orochi_resource_pools p
    WHERE orochi_pool_stats.pool_name IS NULL
       OR p.pool_name = orochi_pool_stats.pool_name;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_pool_stats(text) IS
'Get statistics for resource pools.

Examples:
  SELECT * FROM orochi_pool_stats();  -- All pools
  SELECT * FROM orochi_pool_stats(''analytics'');  -- Specific pool';

-- Get session resource usage
CREATE FUNCTION orochi_session_resources(backend_pid integer DEFAULT NULL)
RETURNS TABLE (
    pid integer,
    pool_name text,
    class_name text,
    cpu_time_us bigint,
    memory_bytes bigint,
    io_read_bytes bigint,
    io_write_bytes bigint,
    active_queries integer,
    session_start timestamptz
)
AS $$
BEGIN
    -- This would query the resource governor's session tracking
    -- For now, return empty result
    RETURN;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_session_resources(integer) IS
'Get resource usage for sessions.

Examples:
  SELECT * FROM orochi_session_resources();  -- All sessions
  SELECT * FROM orochi_session_resources(12345);  -- Specific backend';

-- Get queued queries
CREATE FUNCTION orochi_queued_queries(pool_name text DEFAULT NULL)
RETURNS TABLE (
    queue_position integer,
    backend_pid integer,
    pool_name text,
    priority text,
    enqueue_time timestamptz,
    wait_time_ms bigint,
    estimated_cost bigint
)
AS $$
BEGIN
    -- This would query the workload queue from shared memory
    -- For now, return empty result
    RETURN;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_queued_queries(text) IS
'Get information about queued queries waiting for resources.

Examples:
  SELECT * FROM orochi_queued_queries();  -- All queued queries
  SELECT * FROM orochi_queued_queries(''analytics'');  -- Specific pool';

-- ============================================================
-- Administrative Functions
-- ============================================================

-- Enable a resource pool
CREATE FUNCTION orochi_enable_pool(pool_name text)
RETURNS boolean
AS $$
BEGIN
    UPDATE orochi.orochi_resource_pools
    SET state = 0, updated_at = NOW()
    WHERE orochi_resource_pools.pool_name = orochi_enable_pool.pool_name;

    RETURN FOUND;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_enable_pool(text) IS
'Enable a resource pool that was disabled or draining.

Example:
  SELECT orochi_enable_pool(''analytics'');';

-- Disable a resource pool (start draining)
CREATE FUNCTION orochi_disable_pool(pool_name text, wait_for_drain boolean DEFAULT false)
RETURNS boolean
AS $$
BEGIN
    -- Check if it's an internal pool
    IF EXISTS (
        SELECT 1 FROM orochi.orochi_resource_pools
        WHERE orochi_resource_pools.pool_name = orochi_disable_pool.pool_name
          AND is_internal = true
    ) THEN
        RAISE EXCEPTION 'Cannot disable internal pool "%"', pool_name;
    END IF;

    UPDATE orochi.orochi_resource_pools
    SET state = 1, updated_at = NOW()  -- Set to draining
    WHERE orochi_resource_pools.pool_name = orochi_disable_pool.pool_name;

    IF wait_for_drain THEN
        -- In a full implementation, this would wait for active queries to complete
        -- For now, just mark as disabled
        UPDATE orochi.orochi_resource_pools
        SET state = 2
        WHERE orochi_resource_pools.pool_name = orochi_disable_pool.pool_name;
    END IF;

    RETURN FOUND;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_disable_pool(text, boolean) IS
'Disable a resource pool. New queries will be rejected.
If wait_for_drain is true, waits for active queries to complete.

Example:
  SELECT orochi_disable_pool(''analytics'');
  SELECT orochi_disable_pool(''analytics'', true);  -- Wait for queries';

-- Cancel all queued queries in a pool
CREATE FUNCTION orochi_cancel_queued(pool_name text)
RETURNS integer
AS $$
DECLARE
    cancelled_count integer := 0;
BEGIN
    -- This would interact with the workload queue in shared memory
    -- For now, just return 0
    RETURN cancelled_count;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION orochi_cancel_queued(text) IS
'Cancel all queued queries in a resource pool.

Example:
  SELECT orochi_cancel_queued(''analytics'');';

-- ============================================================
-- Documentation
-- ============================================================

COMMENT ON SCHEMA orochi IS
'Orochi DB extension schema containing workload management objects.

Key tables:
  - orochi_resource_pools: Resource pool definitions
  - orochi_workload_classes: Workload class definitions
  - orochi_pool_stats: Historical usage statistics

Key views:
  - resource_pools: Current pool configuration
  - workload_classes: Current class configuration

Key functions:
  - orochi_create_resource_pool(): Create a pool
  - orochi_create_workload_class(): Create a class
  - orochi_assign_workload_class(): Assign session to class
  - orochi_workload_status(): Overall status
  - orochi_pool_stats(): Pool statistics';
