-- Migration: Add avatar column to users table
-- This migration adds an avatar column to store user profile images as data URLs or external URLs

-- Add avatar column to users table
ALTER TABLE users ADD COLUMN IF NOT EXISTS avatar TEXT;

-- Add comment explaining the column usage
COMMENT ON COLUMN users.avatar IS 'User profile avatar URL (can be data URL or external URL)';
