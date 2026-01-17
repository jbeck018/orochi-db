-- Basic Orochi DB tests
-- Test extension creation and basic functionality

-- Test: Extension loads successfully
CREATE EXTENSION orochi;
SELECT orochi_version();

-- Test: Schema created
SELECT schema_name FROM information_schema.schemata WHERE schema_name = 'orochi';

-- Test: Catalog tables exist
SELECT tablename FROM pg_tables
WHERE schemaname = 'orochi'
ORDER BY tablename;

-- Test: Basic views accessible
SELECT COUNT(*) >= 0 AS has_tables_view FROM orochi.tables;
SELECT COUNT(*) >= 0 AS has_shards_view FROM orochi.shards;
SELECT COUNT(*) >= 0 AS has_chunks_view FROM orochi.chunks;
SELECT COUNT(*) >= 0 AS has_nodes_view FROM orochi.nodes;

-- Test: Vector type exists
SELECT typname FROM pg_type WHERE typname = 'vector';

-- Cleanup
DROP EXTENSION orochi CASCADE;
