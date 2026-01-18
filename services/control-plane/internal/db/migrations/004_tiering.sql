-- Migration: 004_tiering
-- Description: Add tiered storage and columnar configuration support

-- Add tiering configuration columns to clusters table
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_enabled BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_hot_duration VARCHAR(20);  -- e.g., "7d"
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_warm_duration VARCHAR(20); -- e.g., "30d"
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_cold_duration VARCHAR(20); -- e.g., "90d"
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS tiering_compression VARCHAR(10);   -- lz4, zstd

-- Add S3 configuration columns for cold/frozen tier storage
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_endpoint VARCHAR(255);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_bucket VARCHAR(255);
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS s3_region VARCHAR(100);
-- Note: S3 credentials (access_key_id, secret_access_key) are stored in Kubernetes secrets
-- and not in the database for security reasons. The provisioner service handles secret creation.

-- Add columnar storage and sharding configuration
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS enable_columnar BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS default_shard_count INTEGER NOT NULL DEFAULT 4;

-- Add indexes for tiering queries
CREATE INDEX IF NOT EXISTS idx_clusters_tiering_enabled ON clusters(tiering_enabled);

-- Add check constraints for valid configuration
ALTER TABLE clusters ADD CONSTRAINT IF NOT EXISTS check_shard_count
    CHECK (default_shard_count >= 1 AND default_shard_count <= 1024);

ALTER TABLE clusters ADD CONSTRAINT IF NOT EXISTS check_compression_type
    CHECK (tiering_compression IS NULL OR tiering_compression IN ('lz4', 'zstd'));

-- Add comments to document columns
COMMENT ON COLUMN clusters.tiering_enabled IS 'Whether tiered storage (hot/warm/cold/frozen) is enabled';
COMMENT ON COLUMN clusters.tiering_hot_duration IS 'Duration before data moves from hot to warm tier (e.g., "7d")';
COMMENT ON COLUMN clusters.tiering_warm_duration IS 'Duration before data moves from warm to cold tier (e.g., "30d")';
COMMENT ON COLUMN clusters.tiering_cold_duration IS 'Duration before data moves from cold to frozen tier (e.g., "90d")';
COMMENT ON COLUMN clusters.tiering_compression IS 'Compression algorithm for tiered data (lz4, zstd)';
COMMENT ON COLUMN clusters.s3_endpoint IS 'S3-compatible endpoint URL for cold/frozen tier storage';
COMMENT ON COLUMN clusters.s3_bucket IS 'S3 bucket name for cold/frozen tier storage';
COMMENT ON COLUMN clusters.s3_region IS 'S3 region for cold/frozen tier storage';
COMMENT ON COLUMN clusters.enable_columnar IS 'Whether columnar storage is enabled for analytics workloads';
COMMENT ON COLUMN clusters.default_shard_count IS 'Default number of shards for distributed tables (1-1024)';
