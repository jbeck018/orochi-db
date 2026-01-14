-- Orochi DB initialization script
CREATE EXTENSION IF NOT EXISTS orochi;

-- Grant usage to public schema
GRANT USAGE ON SCHEMA orochi TO PUBLIC;
GRANT SELECT ON ALL TABLES IN SCHEMA orochi TO PUBLIC;

-- Log successful initialization
DO $$ BEGIN RAISE NOTICE 'Orochi DB extension initialized successfully'; END $$;
