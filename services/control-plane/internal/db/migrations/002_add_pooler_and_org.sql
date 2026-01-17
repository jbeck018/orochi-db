-- Migration: 002_add_pooler_and_org
-- Description: Add organization support and connection pooler fields for multi-tenant isolation

-- Add organization_id column for team-based cluster isolation
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS organization_id UUID;

-- Add pooler_url column for PgBouncer connection pooling
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS pooler_url TEXT;

-- Add pooler_enabled column to track if connection pooling is enabled
ALTER TABLE clusters ADD COLUMN IF NOT EXISTS pooler_enabled BOOLEAN NOT NULL DEFAULT false;

-- Create organizations table for future team management features
CREATE TABLE IF NOT EXISTS organizations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(255) NOT NULL,
    slug VARCHAR(63) NOT NULL UNIQUE,
    owner_id UUID NOT NULL REFERENCES users(id),
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

-- Organization members table for team collaboration
CREATE TABLE IF NOT EXISTS organization_members (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    organization_id UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role VARCHAR(50) NOT NULL DEFAULT 'member', -- owner, admin, member, viewer
    created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    UNIQUE(organization_id, user_id)
);

-- Add foreign key for organization_id on clusters
-- Using a partial index to allow NULL values
ALTER TABLE clusters DROP CONSTRAINT IF EXISTS clusters_organization_id_fkey;
ALTER TABLE clusters ADD CONSTRAINT clusters_organization_id_fkey
    FOREIGN KEY (organization_id) REFERENCES organizations(id) ON DELETE SET NULL;

-- Indexes for organization tables
CREATE INDEX IF NOT EXISTS idx_organizations_owner_id ON organizations(owner_id);
CREATE INDEX IF NOT EXISTS idx_organizations_slug ON organizations(slug);
CREATE INDEX IF NOT EXISTS idx_organization_members_org_id ON organization_members(organization_id);
CREATE INDEX IF NOT EXISTS idx_organization_members_user_id ON organization_members(user_id);
CREATE INDEX IF NOT EXISTS idx_clusters_organization_id ON clusters(organization_id);

-- Triggers for updated_at on new tables
DROP TRIGGER IF EXISTS update_organizations_updated_at ON organizations;
CREATE TRIGGER update_organizations_updated_at
    BEFORE UPDATE ON organizations
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS update_organization_members_updated_at ON organization_members;
CREATE TRIGGER update_organization_members_updated_at
    BEFORE UPDATE ON organization_members
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- Update the unique constraint on clusters to include organization
-- First drop the old constraint
ALTER TABLE clusters DROP CONSTRAINT IF EXISTS clusters_owner_id_name_key;

-- Add new unique constraint that considers organization
-- Clusters must be unique by (owner_id, name) when organization_id is NULL
-- or by (organization_id, name) when organization_id is NOT NULL
CREATE UNIQUE INDEX IF NOT EXISTS idx_clusters_owner_name_unique
    ON clusters (owner_id, name)
    WHERE organization_id IS NULL AND deleted_at IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_clusters_org_name_unique
    ON clusters (organization_id, name)
    WHERE organization_id IS NOT NULL AND deleted_at IS NULL;
